#include "NFCManager.h"
#include "OpenSpoolParser.h"
#include "ConversionUtils.h"
#ifndef NATIVE_TEST
  #include "ApplicationManager.h"
  #include "HardwareNFCConnection.h"
  #include "SpoolmanManager.h"
  #include <Arduino.h>
#else
  #include "platform/NativePlatform.h"
  #include "FakeLCDManager.h"
  #include "StubApplicationManager.h"
#endif
#include <cstring>
#include <time.h>


//#define DUMP_TAGS_TO_CONSOLE

#ifdef DUMP_TAGS_TO_CONSOLE
    #include <base64.hpp>
#endif

// Static member initialization
uint32_t NFCManager::s_write_request_id_counter = 1000;

NFCManager& NFCManager::getInstance() {
    static NFCManager instance;
    return instance;
}

bool NFCManager::begin() {
    // Create hardware connection if none was injected
    if (connection_ == nullptr) {
#ifndef NATIVE_TEST
        connection_ = new HardwareNFCConnection();
        ownsConnection_ = true;
#else
        // In native tests, connection must be injected via setConnection()
        Serial.println("NFCManager: No connection injected for native test");
        return false;
#endif
    }

    if (!connection_->begin()) {
        Serial.println("NFCManager: Failed to initialize connection");
        return false;
    }

    // Create FreeRTOS primitives
    writeQueue = xQueueCreate(8, sizeof(NFCWriteRequest));
    if (writeQueue == nullptr) {
        Serial.println("NFCManager: Failed to create write queue");
        return false;
    }

    tagMutex = xSemaphoreCreateMutex();
    if (tagMutex == nullptr) {
        Serial.println("NFCManager: Failed to create tag mutex");
        return false;
    }

    completedMutex = xSemaphoreCreateMutex();
    if (completedMutex == nullptr) {
        Serial.println("NFCManager: Failed to create completed mutex");
        return false;
    }

    // Initialize state
    memset(&currentSpool, 0, sizeof(currentSpool));
    memset(lastSeenUid, 0, sizeof(lastSeenUid));
    lastSeenUidLength = 0;
    lastSeenValid = false;
    memset(completedRequests, 0, sizeof(completedRequests));
    completedRequestsIndex = 0;
    memset(recentSpools, 0, sizeof(recentSpools));
    recentSpoolsCount = 0;
    suppressReDetection_ = false;
    suppressReDetectionUid_[0] = '\0';

    Serial.println("NFCManager: Initialized successfully");
    return true;
}

bool NFCManager::getCurrentSpoolState(CurrentSpoolState& out) {
    if (tagMutex == nullptr) {
        return false;
    }
    if (xSemaphoreTake(tagMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }
    out = currentSpool;
    xSemaphoreGive(tagMutex);
    return true;
}

void NFCManager::startScanTask() {
    xTaskCreatePinnedToCore(
        scanTaskFunc,
        "NFCScanTask",
        4096,
        this,
        1,
        &scanTaskHandle,
        1  // Run on core 1
    );
    Serial.println("NFCManager: Scan task started");
}

void NFCManager::scanTaskFunc(void* param) {
    NFCManager* self = static_cast<NFCManager*>(param);
    self->scanLoop();
}

void NFCManager::scanLoop() {
    uint32_t scanCount = 0;
    uint32_t detectCount = 0;
    uint32_t failCount = 0;

    Serial.println("NFCManager: scanLoop() started, polling every 50ms");

#ifndef NATIVE_TEST
    // Register this task with the ESP32 hardware watchdog.
    // If scanLoop() hangs for >NFC_WDT_TIMEOUT_S (e.g., stuck in driver I/O),
    // the watchdog triggers a system reset automatically.
    esp_task_wdt_init(NFC_WDT_TIMEOUT_S, true);
    esp_task_wdt_add(NULL);
#endif

    // RF is already initialized and active from begin()

    // One-time startup diagnostic
#ifndef NATIVE_TEST
    static_cast<HardwareNFCConnection*>(connection_)->logDiagnostics();
#endif

    while (true) {
#ifndef NATIVE_TEST
        esp_task_wdt_reset();
#endif
        uint8_t uid[8];
        uint8_t uidLength = 0;
        scanCount++;

        // Log heartbeat every 200 scans (~10 seconds)
        //if (scanCount % 200 == 0) {
        //    Serial.printf("NFCManager: heartbeat scan=%lu detected=%lu failed=%lu\n",
        //                  scanCount, detectCount, failCount);
        //}

        // Watchdog: check if we've exceeded failure thresholds
        if (consecutiveFailures_ >= RESTART_THRESHOLD) {
            Serial.println("NFCManager: CRITICAL - too many consecutive failures, restarting ESP");
#ifndef NATIVE_TEST
            delay(100);  // let serial flush
            ESP.restart();
#endif
        }
        if (consecutiveFailures_ > 0 && (consecutiveFailures_ % RECOVERY_THRESHOLD) == 0) {
            attemptRecovery();
        }

        // In ISO14443-A, the RF field stays active; we do not reset the hardware chip every 50ms.
        vTaskDelay(pdMS_TO_TICKS(50));

        // Detect tag
        if (connection_->detectTag(uid, &uidLength)) {
            detectCount++;
            // Log UID on first detection
            if (!lastSeenValid || memcmp(uid, lastSeenUid, uidLength) != 0) {
                Serial.printf("NFCManager: Tag detected! UID=");
                for (uint8_t i = 0; i < uidLength; i++) {
                    Serial.printf("%02X", uid[i]);
                }
                Serial.println("");
            }

            // Store UID for addressed read/write commands
            connection_->setCurrentUid(uid, uidLength);

            // Check if this is the same spool we already processed
            // Also suppress re-detection if writes are in progress for this tag
            bool shouldSkipReRead = isDuplicateSpool(uid, uidLength);

            if (suppressReDetection_) {
                char uidHex[17];
                for (uint8_t i = 0; i < uidLength && i < 8; i++) {
                    sprintf(uidHex + (i * 2), "%02X", uid[i]);
                }
                uidHex[uidLength * 2] = '\0';

                if (strcmp(uidHex, suppressReDetectionUid_) == 0) {
                    shouldSkipReRead = true;  // Skip re-read for tag being written to
                }
            }

            if (!shouldSkipReRead) {
                Serial.println("NFCManager: New spool detected, reading tag...");
                // readAndParseTag manages its own mutex internally
                if (!readAndParseTag(uid, uidLength)) {
                    Serial.println("NFCManager: readAndParseTag() failed - treating as blank tag");
                    // Take mutex briefly for blank tag state update
                    if (xSemaphoreTake(tagMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                        for (uint8_t i = 0; i < uidLength && i < 8; i++) {
                            sprintf(currentSpool.spool_id + (i * 2), "%02X", uid[i]);
                        }
                        currentSpool.spool_id[uidLength * 2] = '\0';
                        memcpy(currentSpool.uid, uid, uidLength);
                        currentSpool.uid_length = uidLength;
                        currentSpool.present = true;
                        currentSpool.tag_data_valid = false;
                        currentSpool.blank_tag_present = true;

                        sendBlankTagMessage();

                        memcpy(lastSeenUid, uid, uidLength);
                        lastSeenUidLength = uidLength;
                        lastSeenValid = true;
                        xSemaphoreGive(tagMutex);
                    } else {
                        Serial.println("NFCManager: Could not acquire tagMutex");
                    }
                }
            } else {
                // Tag is a duplicate - skip re-reading
                // This log would be too verbose (every 50ms), so commenting out
                // Serial.println("NFCManager: Same tag detected, skipping read");
            }

            // Process any pending write requests while tag is present
            processWriteQueue();
        } else {
            // No tag detected - clear state
            if (xSemaphoreTake(tagMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                if (lastSeenValid) {
                    Serial.println("NFCManager: Tag removed");

                    // Send TAG_REMOVED message before clearing state
                    sendTagRemovedMessage();
                }
                currentSpool.present = false;
                currentSpool.blank_tag_present = false;
                lastSeenValid = false;

                // Clear suppression if tag removed
                suppressReDetection_ = false;
                suppressReDetectionUid_[0] = '\0';

                xSemaphoreGive(tagMutex);
            }
        }

        // Small delay to prevent tight loop
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void NFCManager::attemptRecovery() {
    Serial.println("NFCManager: WATCHDOG - attempting hardware recovery");
    if (connection_->hardwareReset()) {
        Serial.println("NFCManager: Hardware reset succeeded");
    } else {
        Serial.println("NFCManager: Hardware reset failed");
    }
}

bool NFCManager::readAndParseTag(uint8_t* uid, uint8_t uid_length) {
    OpenSpoolData localTag;
    Serial.println("NFCManager: Reading OpenSpool NDEF tag data...");

    if (!OpenSpoolParser::readFromTag(connection_, &localTag)) {
        Serial.println("NFCManager: Failed to read/parse OpenSpool tag");
        return false;
    }

    Serial.println("NFCManager: Read and parse successful!");

    // NFC I/O complete — take mutex only for the fast state copy
    if (xSemaphoreTake(tagMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        Serial.println("NFCManager: Could not acquire tagMutex for state update");
        return false;
    }

    currentSpool.tag_data = localTag;

    // Store UID as hex string (7 bytes for NTAG215)
    for (uint8_t i = 0; i < uid_length && i < 7; i++) {
        sprintf(currentSpool.spool_id + (i * 2), "%02X", uid[i]);
    }
    currentSpool.spool_id[uid_length * 2] = '\0';

    memcpy(currentSpool.uid, uid, uid_length);
    currentSpool.uid_length = uid_length;

    currentSpool.present = true;
    currentSpool.tag_data_valid = true;
    currentSpool.blank_tag_present = false;

    addToRecentSpools();

    Serial.printf("NFCManager: Parsed OpenSpool %s (Type: %s, Brand: %s, Color: %s, Rem: %.1fg, ID: %ld)\n",
                  currentSpool.spool_id, currentSpool.tag_data.type, currentSpool.tag_data.brand,
                  currentSpool.tag_data.color_hex, currentSpool.tag_data.remaining_weight,
                  (long)currentSpool.tag_data.spoolman_id);

    sendSpoolDetectedMessage();

    // Update dedup state
    memcpy(lastSeenUid, uid, uid_length);
    lastSeenUidLength = uid_length;
    lastSeenValid = true;

    xSemaphoreGive(tagMutex);
    return true;
}

bool NFCManager::formatNewSpool() {
    Serial.println("NFCManager: formatNewSpool() called for OpenSpool");

    OpenSpoolData localTag;
    memset(&localTag, 0, sizeof(localTag));
    strcpy(localTag.protocol, "openspool");
    strcpy(localTag.version, "1.0");
    strcpy(localTag.type, "PLA");
    localTag.material_type = OPT_MATERIAL_TYPE_PLA;
    strcpy(localTag.color_hex, "#FFFFFF");
    localTag.color_rgba[0] = 255;
    localTag.color_rgba[1] = 255;
    localTag.color_rgba[2] = 255;
    localTag.color_rgba[3] = 255;
    strcpy(localTag.brand, "Generic");
    localTag.total_weight = 1000.0f;
    localTag.remaining_weight = 1000.0f;
    localTag.min_temp = 205;
    localTag.max_temp = 225;
    localTag.bed_temp = 60;
    localTag.density = 1.24f;
    localTag.diameter = 1.75f;

    Serial.println("NFCManager: Writing OpenSpool NDEF to NFC tag...");
    if (!OpenSpoolParser::writeToTag(connection_, &localTag)) {
        Serial.println("NFCManager: Failed to write formatted OpenSpool tag");
        return false;
    }

    if (xSemaphoreTake(tagMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        Serial.println("NFCManager: Could not acquire tagMutex after format");
        return false;
    }
    currentSpool.tag_data = localTag;
    currentSpool.tag_data_valid = true;
    currentSpool.blank_tag_present = false;
    addToRecentSpools();

    NFCWriteRequest dummyReq;
    bool queueEmpty = (writeQueue == nullptr || xQueuePeek(writeQueue, &dummyReq, 0) != pdTRUE);
    if (queueEmpty) {
        sendSpoolDetectedMessage();
    } else {
        suppressReDetection_ = true;
        strncpy(suppressReDetectionUid_, currentSpool.spool_id, sizeof(suppressReDetectionUid_) - 1);
        suppressReDetectionUid_[sizeof(suppressReDetectionUid_) - 1] = '\0';
    }
    xSemaphoreGive(tagMutex);
    Serial.println("NFCManager: formatNewSpool() complete");
    return true;
}

void NFCManager::sendSpoolDetectedMessage(bool suppress_spoolman_sync) {
    if (!currentSpool.tag_data_valid) {
        return;
    }

    AppMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = AppMessageType::SPOOL_DETECTED;

    // Copy spool ID
    strncpy(msg.payload.spoolDetected.spool_id, currentSpool.spool_id,
            sizeof(msg.payload.spoolDetected.spool_id) - 1);
    msg.payload.spoolDetected.spool_id[sizeof(msg.payload.spoolDetected.spool_id) - 1] = '\0';

    // Get material type
    uint8_t material_type = 0;
    if (opt_get_material_type(&currentSpool.tag_data, &material_type) == OPT_OK) {
        msg.payload.spoolDetected.material_type = material_type;
    } else {
        msg.payload.spoolDetected.material_type = 0;
    }

    // Calculate remaining weight in kg
    float full_weight = 0.0f;
    float consumed = 0.0f;
    opt_get_actual_full_weight(&currentSpool.tag_data, &full_weight);
    opt_get_consumed_weight(&currentSpool.tag_data, &consumed);
    msg.payload.spoolDetected.kg_remaining = (full_weight - consumed) / 1000.0f;

    // Get primary color
    if (opt_get_primary_color(&currentSpool.tag_data, msg.payload.spoolDetected.primary_color) != OPT_OK) {
        memset(msg.payload.spoolDetected.primary_color, 0, 4);
    }

    // Get density (use 0 to signal "not available" to caller)
    if (opt_get_density(&currentSpool.tag_data, &msg.payload.spoolDetected.density) != OPT_OK) {
        msg.payload.spoolDetected.density = 0.0f;
    }

    // Get filament diameter
    if (opt_get_filament_diameter(&currentSpool.tag_data, &msg.payload.spoolDetected.diameter) != OPT_OK) {
        msg.payload.spoolDetected.diameter = 0.0f;
    }

    // Get initial (full) weight
    msg.payload.spoolDetected.initial_weight_g = full_weight;

    // Get manufacturer/brand name
    if (opt_get_brand_name(&currentSpool.tag_data, msg.payload.spoolDetected.manufacturer,
                           sizeof(msg.payload.spoolDetected.manufacturer)) != OPT_OK) {
        msg.payload.spoolDetected.manufacturer[0] = '\0';
    }

    // Get Spoolman ID from tag (if present)
    int32_t spoolman_id = -1;
    opt_get_gp_spoolman_id(&currentSpool.tag_data, &spoolman_id);
    msg.payload.spoolDetected.spoolman_id = spoolman_id;

    // Set suppress_spoolman_sync flag
    msg.payload.spoolDetected.suppress_spoolman_sync = suppress_spoolman_sync ? 1 : 0;

    // Get material name
    if (opt_get_material_name(&currentSpool.tag_data, msg.payload.spoolDetected.material_name,
                               sizeof(msg.payload.spoolDetected.material_name)) != OPT_OK) {
        // Use material type abbreviation as fallback
        const char* abbrev = opt_material_type_str((opt_material_type_t)material_type);
        if (abbrev) {
            strncpy(msg.payload.spoolDetected.material_name, abbrev,
                    sizeof(msg.payload.spoolDetected.material_name) - 1);
            msg.payload.spoolDetected.material_name[sizeof(msg.payload.spoolDetected.material_name) - 1] = '\0';
        } else {
            msg.payload.spoolDetected.material_name[0] = '\0';
        }
    }

    ApplicationManager::getInstance().sendMessage(msg);
}

void NFCManager::sendBlankTagMessage() {
    AppMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = AppMessageType::BLANK_TAG_DETECTED;
    strncpy(msg.payload.blankTag.spool_id, currentSpool.spool_id,
            sizeof(msg.payload.blankTag.spool_id) - 1);
    msg.payload.blankTag.spool_id[sizeof(msg.payload.blankTag.spool_id) - 1] = '\0';

    ApplicationManager::getInstance().sendMessage(msg);
}

void NFCManager::sendTagRemovedMessage() {
    AppMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = AppMessageType::TAG_REMOVED;
    strncpy(msg.payload.tagRemoved.spool_id, currentSpool.spool_id,
            sizeof(msg.payload.tagRemoved.spool_id) - 1);
    msg.payload.tagRemoved.spool_id[sizeof(msg.payload.tagRemoved.spool_id) - 1] = '\0';
    msg.payload.tagRemoved.last_remaining_kg = 0.0f;
    msg.payload.tagRemoved.spoolman_id = -1;

    if (currentSpool.tag_data_valid) {
        float full_weight = 0.0f, consumed = 0.0f;
        opt_get_actual_full_weight(&currentSpool.tag_data, &full_weight);
        opt_get_consumed_weight(&currentSpool.tag_data, &consumed);
        msg.payload.tagRemoved.last_remaining_kg = (full_weight - consumed) / 1000.0f;

        int32_t smId = -1;
        opt_get_gp_spoolman_id(&currentSpool.tag_data, &smId);
        msg.payload.tagRemoved.spoolman_id = smId;
    }

    ApplicationManager::getInstance().sendMessage(msg);
}

bool NFCManager::enqueueRawWrite(const NFCWriteRequest& req, const uint8_t* data, size_t dataSize) {
    if (dataSize == 0 || dataSize > RAW_WRITE_BUFFER_SIZE) {
        return false;
    }
    if (rawWritePending_) {
        Serial.println("NFCManager: Raw write already pending");
        return false;
    }
    memcpy(rawWriteBuffer_, data, dataSize);
    rawWriteBufferSize_ = dataSize;
    rawWritePending_ = true;
    return enqueueWrite(req);
}

uint32_t NFCManager::generateRequestId() {
    return ++s_write_request_id_counter;
}

bool NFCManager::writeSpoolmanDataToTag(int32_t spoolman_id, const char* expected_spool_id) {
#ifdef NATIVE_TEST
    // In native tests, SpoolmanManager is not available
    Serial.println("NFCManager: writeSpoolmanDataToTag not supported in native tests");
    return false;
#else
    // 1. Validate input
    if (spoolman_id <= 0) {
        Serial.printf("NFCManager: Invalid spoolman_id: %d\n", spoolman_id);
        return false;
    }

    // 2. Fetch spool details from Spoolman
    SpoolDetails details;
    if (!SpoolmanManager::getInstance().getSpoolDetails(spoolman_id, details)) {
        Serial.printf("NFCManager: Failed to fetch Spoolman spool %d\n", spoolman_id);
        return false;
    }

    // 3. Validate weight data
    if (details.initial_weight_g <= 0 || details.remaining_weight_g < 0) {
        Serial.println("NFCManager: Invalid weight data from Spoolman");
        return false;
    }
    if (details.remaining_weight_g > details.initial_weight_g) {
        Serial.println("NFCManager: Remaining weight exceeds initial weight");
        return false;
    }

    // 4. Build complete tag in memory (use writeScratchTag_ to avoid stack overflow)
    opt_init(&writeScratchTag_);
    opt_error_t err = opt_format_empty_tag(&writeScratchTag_, 312, 32);
    if (err != OPT_OK) {
        Serial.printf("NFCManager: Failed to format tag: %s\n", opt_error_str(err));
        return false;
    }

    // 5. Convert and populate fields
    uint8_t material_enum = materialTypeFromString(details.material_type);
    opt_set_material_type(&writeScratchTag_, material_enum);

    uint8_t rgba[4] = {255, 255, 255, 255};  // Default white
    parseHexColor(details.color_hex, rgba);  // Best effort
    opt_set_primary_color(&writeScratchTag_, rgba);

    opt_set_brand_name(&writeScratchTag_, details.manufacturer);
    opt_set_actual_full_weight(&writeScratchTag_, details.initial_weight_g);

    float consumed = details.initial_weight_g - details.remaining_weight_g;
    opt_set_consumed_weight(&writeScratchTag_, consumed);

    // Set defaults for missing fields
    float density = getDefaultDensity(material_enum);
    opt_set_density(&writeScratchTag_, density);
    opt_set_filament_diameter(&writeScratchTag_, 1.75f);

    // Write Spoolman ID to aux region
    opt_set_gp_spoolman_id(&writeScratchTag_, spoolman_id);

    // 6. Enqueue raw write
    NFCWriteRequest req;
    memset(&req, 0, sizeof(req));  // CRITICAL: zero-init (per MEMORY.md)
    req.request_id = generateRequestId();
    req.type = NFCWriteType::WRITE_RAW_TAG;
    if (expected_spool_id != nullptr) {
        strncpy(req.expected_spool_id, expected_spool_id, sizeof(req.expected_spool_id) - 1);
    }

    char jsonBuf[512];
    size_t len = OpenSpoolParser::serializeJsonPayload(&writeScratchTag_, jsonBuf, sizeof(jsonBuf));
    if (len == 0) {
        Serial.println("NFCManager: Failed to serialize Spoolman tag to JSON");
        return false;
    }

    bool queued = enqueueRawWrite(req, (const uint8_t*)jsonBuf, len);
    if (queued) {
        Serial.printf("NFCManager: Queued write for Spoolman spool %d\n", spoolman_id);
    }
    return queued;
#endif
}

bool NFCManager::enqueueWrite(const NFCWriteRequest& req) {
    if (writeQueue == nullptr) {
        return false;
    }

    // Check if already completed (duplicate)
    if (isRequestCompleted(req.request_id)) {
        return true;  // Already done, return success
    }

    bool queued = xQueueSend(writeQueue, &req, pdMS_TO_TICKS(100)) == pdTRUE;

    // If this is part of a batch (suppress_sync or FORMAT_NEW), set suppression flag
    if (queued && (req.suppress_sync || req.type == NFCWriteType::FORMAT_NEW)) {
        suppressReDetection_ = true;
        if (req.expected_spool_id[0] != '\0') {
            strncpy(suppressReDetectionUid_, req.expected_spool_id, sizeof(suppressReDetectionUid_) - 1);
            suppressReDetectionUid_[sizeof(suppressReDetectionUid_) - 1] = '\0';
        }
        // Track if this batch has suppress_sync
        if (req.suppress_sync) {
            batchHadSuppressSync_ = true;
        }
    }

    return queued;
}

void NFCManager::processWriteQueue() {
    if (writeQueue == nullptr || !currentSpool.present) {
        return;
    }

    NFCWriteRequest request;

    // Process at most one write per call — remaining writes get handled in
    // subsequent scan cycles (50ms apart), keeping tag detection responsive.
    while (xQueuePeek(writeQueue, &request, 0) == pdTRUE) {
        // Check if already completed — skip without counting as our one write
        if (isRequestCompleted(request.request_id)) {
            xQueueReceive(writeQueue, &request, 0);
            continue;
        }

        // Dequeue the request
        xQueueReceive(writeQueue, &request, 0);

        // executeWrite() manages its own mutex internally
        bool success = executeWrite(request);

        markRequestCompleted(request.request_id);

        // Snapshot spool info under mutex for the notification message
        char snapshotSpoolId[64];
        float snapshotRemainingGrams = 0;
        bool snapshotValid = false;
        if (xSemaphoreTake(tagMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            strncpy(snapshotSpoolId, currentSpool.spool_id, sizeof(snapshotSpoolId));
            snapshotSpoolId[sizeof(snapshotSpoolId) - 1] = '\0';
            if (currentSpool.tag_data_valid) {
                opt_get_remaining_weight(&currentSpool.tag_data, &snapshotRemainingGrams);
                snapshotValid = true;
            }
            xSemaphoreGive(tagMutex);
        }

        // Build and send the update message with snapshotted data
        AppMessage msg;
        msg.type = AppMessageType::SPOOL_UPDATED;
        strncpy(msg.payload.spoolUpdated.spool_id, snapshotSpoolId,
                sizeof(msg.payload.spoolUpdated.spool_id) - 1);
        msg.payload.spoolUpdated.spool_id[sizeof(msg.payload.spoolUpdated.spool_id) - 1] = '\0';
        msg.payload.spoolUpdated.update_type = static_cast<uint8_t>(request.type);
        msg.payload.spoolUpdated.success = success;
        msg.payload.spoolUpdated.suppress_sync = request.suppress_sync;
        msg.payload.spoolUpdated.kg_remaining = snapshotValid ? snapshotRemainingGrams / 1000.0f : 0;
        ApplicationManager::getInstance().sendMessage(msg);

        // One write per cycle — break to let scan loop run between writes
        break;
    }

    // Check if queue is now empty
    NFCWriteRequest peekReq;
    if (xQueuePeek(writeQueue, &peekReq, 0) != pdTRUE) {
        // Queue is empty - clear suppression flag
        if (suppressReDetection_) {
            bool hadSuppressSync = batchHadSuppressSync_;
            Serial.printf("NFCManager: Write queue empty, clearing suppression and sending SpoolDetected (suppress_spoolman_sync=%d)\n", hadSuppressSync);
            suppressReDetection_ = false;
            suppressReDetectionUid_[0] = '\0';
            batchHadSuppressSync_ = false;

            // Send SpoolDetected to notify listeners (tests, MQTT, LCD) of final state.
            // The suppress_spoolman_sync flag prevents Spoolman sync for write_spoolman_spool batches.
            sendSpoolDetectedMessage(hadSuppressSync);
        }
    }
}

bool NFCManager::writeRawTag() {
    Serial.println("NFCManager: writeRawTag() called");

    OpenSpoolData localTag;
    opt_init(&localTag);
    bool writeOk = false;

    if (rawWriteBufferSize_ > 0 && rawWriteBuffer_[0] == '{') {
        // Direct JSON payload
        if (!OpenSpoolParser::parseJsonPayload((const char*)rawWriteBuffer_, rawWriteBufferSize_, &localTag)) {
            Serial.println("NFCManager: Failed to parse raw JSON payload");
            rawWritePending_ = false;
            return false;
        }
        writeOk = OpenSpoolParser::writeToTag(connection_, &localTag);
    } else if (rawWriteBufferSize_ > 0 && rawWriteBuffer_[0] == 0xE1) {
        // Raw pages starting from CC (Page 3)
        uint8_t startPage = 3;
        size_t pages = (rawWriteBufferSize_ + 3) / 4;
        writeOk = true;
        for (size_t i = 0; i < pages && (startPage + i) <= 129; i++) {
            uint8_t pageData[4] = {0, 0, 0, 0};
            size_t offset = i * 4;
            for (size_t b = 0; b < 4 && (offset + b) < rawWriteBufferSize_; b++) {
                pageData[b] = rawWriteBuffer_[offset + b];
            }
            if (!connection_->writePage(startPage + i, pageData)) {
                writeOk = false;
                break;
            }
        }
    } else if (rawWriteBufferSize_ > 0 && rawWriteBuffer_[0] == 0x03) {
        // Raw pages starting from NDEF TLV (Page 4)
        uint8_t startPage = 4;
        size_t pages = (rawWriteBufferSize_ + 3) / 4;
        writeOk = true;
        for (size_t i = 0; i < pages && (startPage + i) <= 129; i++) {
            uint8_t pageData[4] = {0, 0, 0, 0};
            size_t offset = i * 4;
            for (size_t b = 0; b < 4 && (offset + b) < rawWriteBufferSize_; b++) {
                pageData[b] = rawWriteBuffer_[offset + b];
            }
            if (!connection_->writePage(startPage + i, pageData)) {
                writeOk = false;
                break;
            }
        }
    } else {
        Serial.println("NFCManager: Unrecognized raw write buffer format");
        rawWritePending_ = false;
        return false;
    }

    if (!writeOk) {
        Serial.println("NFCManager: Failed to write raw tag");
        rawWritePending_ = false;
        return false;
    }

    // Verify by re-reading the OpenSpool tag
    Serial.println("NFCManager: Verifying raw write...");
    const int maxRetries = 3;
    bool verified = false;
    for (int retry = 0; retry < maxRetries; retry++) {
        vTaskDelay(pdMS_TO_TICKS(50 * (retry + 1)));
        if (OpenSpoolParser::readFromTag(connection_, &localTag)) {
            verified = true;
            break;
        }
    }

    if (!verified) {
        Serial.println("NFCManager: Verification retries exhausted, trusting written data");
    }

    if (xSemaphoreTake(tagMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        Serial.println("NFCManager: Could not acquire tagMutex after raw write verify");
        rawWritePending_ = false;
        return false;
    }
    currentSpool.tag_data = localTag;
    currentSpool.tag_data_valid = true;
    currentSpool.blank_tag_present = false;
    lastSeenValid = false;  // Force re-detection on next scan
    addToRecentSpools();
    sendSpoolDetectedMessage();
    xSemaphoreGive(tagMutex);

    rawWritePending_ = false;
    Serial.println("NFCManager: writeRawTag() complete");
    return true;
}


static opt_error_t applyWriteUpdate(opt_tag_t& tag, const NFCWriteRequest& request) {
    switch (request.type) {
        case NFCWriteType::REMOVE_WEIGHT:
            return opt_add_consumed_weight(&tag, request.data.grams_to_remove);
        case NFCWriteType::CHANGE_COLOR:
            return opt_set_primary_color(&tag, request.data.new_color);
        case NFCWriteType::CHANGE_FILAMENT_TYPE:
            return opt_set_material_type(&tag, request.data.new_material_type);
        case NFCWriteType::SET_CONSUMED_WEIGHT:
            return opt_set_consumed_weight(&tag, request.data.consumed_weight);
        case NFCWriteType::SET_INITIAL_WEIGHT:
            return opt_set_actual_full_weight(&tag, request.data.consumed_weight);  // Reuse consumed_weight field for the initial weight value
        case NFCWriteType::SET_BRAND_NAME:
            return opt_set_brand_name(&tag, request.data.brand_name);
        case NFCWriteType::WRITE_SPOOLMAN_ID:
            return opt_set_gp_spoolman_id(&tag, request.data.spoolman_id);
        default:
            return OPT_ERR_INVALID_PARAM;
    }
}

bool NFCManager::executeWrite(const NFCWriteRequest& request) {
    // Handle FORMAT_NEW — formatNewSpool() manages its own mutex
    if (request.type == NFCWriteType::FORMAT_NEW) {
        // Validate under mutex
        if (xSemaphoreTake(tagMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            Serial.println("NFCManager: FORMAT_NEW - could not acquire tagMutex");
            return false;
        }
        if (request.expected_spool_id[0] != '\0' &&
            strcmp(currentSpool.spool_id, request.expected_spool_id) != 0) {
            xSemaphoreGive(tagMutex);
            Serial.println("NFCManager: FORMAT_NEW rejected - UID mismatch");
            return false;
        }
        xSemaphoreGive(tagMutex);

        // formatNewSpool() does NFC I/O without mutex, takes mutex at end for state copy
        return formatNewSpool();
    }

    // Handle WRITE_RAW_TAG — writeRawTag() manages its own mutex
    if (request.type == NFCWriteType::WRITE_RAW_TAG) {
        if (!rawWritePending_) {
            Serial.println("NFCManager: WRITE_RAW_TAG - no raw data pending");
            return false;
        }
        // Validate under mutex
        if (xSemaphoreTake(tagMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            Serial.println("NFCManager: WRITE_RAW_TAG - could not acquire tagMutex");
            rawWritePending_ = false;
            return false;
        }
        if (request.expected_spool_id[0] != '\0' &&
            strcmp(currentSpool.spool_id, request.expected_spool_id) != 0) {
            xSemaphoreGive(tagMutex);
            Serial.println("NFCManager: WRITE_RAW_TAG rejected - UID mismatch");
            rawWritePending_ = false;
            return false;
        }
        xSemaphoreGive(tagMutex);

        return writeRawTag();
    }

    // For non-FORMAT writes: take mutex to validate + modify in-memory data, then release for NFC I/O
    if (xSemaphoreTake(tagMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        Serial.println("NFCManager: executeWrite - could not acquire tagMutex");
        return false;
    }

    if (!currentSpool.tag_data_valid) {
        xSemaphoreGive(tagMutex);
        return false;
    }

    if (request.expected_spool_id[0] != '\0') {
        if (strcmp(currentSpool.spool_id, request.expected_spool_id) != 0) {
            Serial.printf("NFCManager: Write rejected: expected spool %s but found %s\n",
                request.expected_spool_id, currentSpool.spool_id);
            xSemaphoreGive(tagMutex);
            return false;
        }
    }

    // Snapshot current tag into reusable scratch storage under mutex; do not mutate shared state until write succeeds.
    writeScratchTag_ = currentSpool.tag_data;
    xSemaphoreGive(tagMutex);

    // Preflight update on a copy to detect overflow before touching shared state.
    opt_tag_t& updatedTag = writeScratchTag_;
    opt_error_t err = applyWriteUpdate(updatedTag, request);
    if (err != OPT_OK) {
        if (err == OPT_ERR_REGION_OVERFLOW) {
            Serial.println("NFCManager: Region overflow during preflight update; keeping original tag data");
        } else {
            Serial.printf("NFCManager: Failed to update in-memory tag data: %s\n", opt_error_str(err));
        }
        return false;
    }

    // NFC I/O without mutex — only scan task touches the hardware
    if (!OpenSpoolParser::writeToTag(connection_, &updatedTag)) {
        Serial.println("NFCManager: Write to tag failed");
        return false;
    }

    // Commit the successful update to shared state.
    if (xSemaphoreTake(tagMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        Serial.println("NFCManager: executeWrite - write succeeded but commit lock failed");
        return false;
    }

    currentSpool.tag_data = updatedTag;
    currentSpool.tag_data_valid = true;
    currentSpool.blank_tag_present = false;
    xSemaphoreGive(tagMutex);

    switch (request.type) {
        case NFCWriteType::REMOVE_WEIGHT:
            Serial.printf("NFCManager: Removed %.2f grams from spool\n", request.data.grams_to_remove);
            break;
        case NFCWriteType::CHANGE_COLOR:
            Serial.printf("NFCManager: Changed color to RGBA(%u,%u,%u,%u)\n",
                request.data.new_color[0], request.data.new_color[1],
                request.data.new_color[2], request.data.new_color[3]);
            break;
        case NFCWriteType::CHANGE_FILAMENT_TYPE:
            Serial.printf("NFCManager: Changed material type to %u\n", request.data.new_material_type);
            break;
        case NFCWriteType::SET_CONSUMED_WEIGHT:
            Serial.printf("NFCManager: Set consumed weight to %.2f grams\n", request.data.consumed_weight);
            break;
        case NFCWriteType::SET_INITIAL_WEIGHT:
            Serial.printf("NFCManager: Set initial weight to %.2f grams\n", request.data.consumed_weight);
            break;
        case NFCWriteType::SET_BRAND_NAME:
            Serial.printf("NFCManager: Set brand name to %s\n", request.data.brand_name);
            break;
        case NFCWriteType::WRITE_SPOOLMAN_ID:
            Serial.printf("NFCManager: Wrote spoolman ID %d to tag\n", request.data.spoolman_id);
            break;
        default:
            break;
    }

    return true;
}

void NFCManager::sendSpoolUpdatedMessage(uint32_t request_id, NFCWriteType type, bool success) {
    (void)request_id;

    AppMessage msg;
    msg.type = AppMessageType::SPOOL_UPDATED;

    strncpy(msg.payload.spoolUpdated.spool_id, currentSpool.spool_id,
            sizeof(msg.payload.spoolUpdated.spool_id) - 1);
    msg.payload.spoolUpdated.spool_id[sizeof(msg.payload.spoolUpdated.spool_id) - 1] = '\0';

    msg.payload.spoolUpdated.update_type = static_cast<uint8_t>(type);
    msg.payload.spoolUpdated.success = success;
    msg.payload.spoolUpdated.suppress_sync = 0;  // Default: allow sync

    // Get remaining weight from tag data
    float remaining_grams = 0;
    if (currentSpool.tag_data_valid) {
        opt_get_remaining_weight(&currentSpool.tag_data, &remaining_grams);
    }
    msg.payload.spoolUpdated.kg_remaining = remaining_grams / 1000.0f;

    ApplicationManager::getInstance().sendMessage(msg);
}

bool NFCManager::isRequestCompleted(uint32_t request_id) {
    if (completedMutex == nullptr) {
        return false;
    }

    if (xSemaphoreTake(completedMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }

    bool found = false;
    for (size_t i = 0; i < COMPLETED_REQUESTS_SIZE; i++) {
        if (completedRequests[i] == request_id) {
            found = true;
            break;
        }
    }

    xSemaphoreGive(completedMutex);
    return found;
}

void NFCManager::markRequestCompleted(uint32_t request_id) {
    if (completedMutex == nullptr) {
        return;
    }

    if (xSemaphoreTake(completedMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    completedRequests[completedRequestsIndex] = request_id;
    completedRequestsIndex = (completedRequestsIndex + 1) % COMPLETED_REQUESTS_SIZE;

    xSemaphoreGive(completedMutex);
}

bool NFCManager::isDuplicateSpool(const uint8_t* uid, uint8_t uid_length) {
    // Benign race: reads lastSeenValid/lastSeenUid without mutex. These are only
    // written by the scan task (same task calling this), so no torn reads. The BLE
    // thread's requestCurrentSpool() can clear lastSeenValid, but worst case we do
    // one extra NFC read — no correctness issue.
    if (!lastSeenValid) {
        return false;
    }

    if (uid_length != lastSeenUidLength) {
        return false;
    }

    return memcmp(uid, lastSeenUid, uid_length) == 0;
}

void NFCManager::requestCurrentSpool() {
    if (xSemaphoreTake(tagMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        lastSeenValid = false;
        memset(lastSeenUid, 0, sizeof(lastSeenUid));
        lastSeenUidLength = 0;
        xSemaphoreGive(tagMutex);
    }
}

void NFCManager::addToRecentSpools() {
    if (!currentSpool.tag_data_valid) {
        return;
    }

    // Check if this spool already exists in recent list
    int existingIndex = -1;
    for (size_t i = 0; i < recentSpoolsCount; i++) {
        if (strcmp(recentSpools[i].spool_id, currentSpool.spool_id) == 0) {
            existingIndex = i;
            break;
        }
    }

    // Create new entry from current spool
    RecentSpoolEntry newEntry;
    memset(&newEntry, 0, sizeof(newEntry));
    strncpy(newEntry.spool_id, currentSpool.spool_id, sizeof(newEntry.spool_id) - 1);

    opt_get_material_type(&currentSpool.tag_data, &newEntry.material_type);
    opt_get_primary_color(&currentSpool.tag_data, newEntry.color);

    if (opt_get_brand_name(&currentSpool.tag_data, newEntry.manufacturer, sizeof(newEntry.manufacturer)) != OPT_OK) {
        newEntry.manufacturer[0] = '\0';
    }

    float full_weight = 0.0f, consumed = 0.0f;
    opt_get_actual_full_weight(&currentSpool.tag_data, &full_weight);
    opt_get_consumed_weight(&currentSpool.tag_data, &consumed);
    newEntry.grams_remaining = (int)(full_weight - consumed);

    newEntry.last_seen = time(nullptr);
    newEntry.valid = true;
    newEntry.synced_to_spoolman = false;

    int32_t smId = -1;
    opt_get_gp_spoolman_id(&currentSpool.tag_data, &smId);
    newEntry.spoolman_id = smId;

    if (existingIndex >= 0) {
        // Spool exists - shift entries to remove it from current position
        for (size_t i = existingIndex; i > 0; i--) {
            recentSpools[i] = recentSpools[i - 1];
        }
    } else {
        // New spool - shift all entries down to make room
        size_t shiftCount = (recentSpoolsCount < MAX_RECENT_SPOOLS) ? recentSpoolsCount : (MAX_RECENT_SPOOLS - 1);
        for (size_t i = shiftCount; i > 0; i--) {
            recentSpools[i] = recentSpools[i - 1];
        }
        if (recentSpoolsCount < MAX_RECENT_SPOOLS) {
            recentSpoolsCount++;
        }
    }

    // Place new entry at front
    recentSpools[0] = newEntry;
}

bool NFCManager::scanOnce() {
    uint8_t uid[8];
    uint8_t uidLength = 0;
    bool result = false;

    connection_->reset();
    connection_->setupRF();

    if (connection_->detectTag(uid, &uidLength)) {
        connection_->setCurrentUid(uid, uidLength);

        // Check if this is the same spool or if re-detection is suppressed
        bool shouldSkipReRead = isDuplicateSpool(uid, uidLength);

        if (suppressReDetection_) {
            char uidHex[17];
            for (uint8_t i = 0; i < uidLength && i < 8; i++) {
                sprintf(uidHex + (i * 2), "%02X", uid[i]);
            }
            uidHex[uidLength * 2] = '\0';

            if (strcmp(uidHex, suppressReDetectionUid_) == 0) {
                shouldSkipReRead = true;
            }
        }

        if (!shouldSkipReRead) {
            // readAndParseTag manages its own mutex internally
            if (readAndParseTag(uid, uidLength)) {
                result = true;
            } else {
                // Blank tag detected — take mutex briefly for state update
                if (xSemaphoreTake(tagMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    for (uint8_t i = 0; i < uidLength && i < 8; i++) {
                        sprintf(currentSpool.spool_id + (i * 2), "%02X", uid[i]);
                    }
                    currentSpool.spool_id[uidLength * 2] = '\0';
                    memcpy(currentSpool.uid, uid, uidLength);
                    currentSpool.uid_length = uidLength;
                    currentSpool.present = true;
                    currentSpool.tag_data_valid = false;
                    currentSpool.blank_tag_present = true;
                    memcpy(lastSeenUid, uid, uidLength);
                    lastSeenUidLength = uidLength;
                    lastSeenValid = true;
                    xSemaphoreGive(tagMutex);
                }
            }
        }
        processWriteQueue();
    } else {
        if (xSemaphoreTake(tagMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (lastSeenValid) {
                sendTagRemovedMessage();
            }
            currentSpool.present = false;
            currentSpool.blank_tag_present = false;
            lastSeenValid = false;

            // Clear suppression if tag removed
            suppressReDetection_ = false;
            suppressReDetectionUid_[0] = '\0';

            xSemaphoreGive(tagMutex);
        }
    }
    return result;
}

size_t NFCManager::getRecentSpools(RecentSpoolEntry* entries, size_t maxEntries) {
    if (tagMutex == nullptr) {
        return 0;
    }

    if (xSemaphoreTake(tagMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return 0;
    }

    size_t count = 0;
    for (size_t i = 0; i < recentSpoolsCount && count < maxEntries; i++) {
        if (recentSpools[i].valid) {
            // Skip the current spool if it matches
            if (currentSpool.present && strcmp(recentSpools[i].spool_id, currentSpool.spool_id) == 0) {
                continue;
            }
            entries[count++] = recentSpools[i];
        }
    }

    xSemaphoreGive(tagMutex);
    return count;
}

void NFCManager::updateRecentSpoolSyncStatus(const char* spool_id, bool synced) {
    if (tagMutex == nullptr || spool_id == nullptr) {
        return;
    }

    if (xSemaphoreTake(tagMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    for (size_t i = 0; i < recentSpoolsCount; i++) {
        if (recentSpools[i].valid && strcmp(recentSpools[i].spool_id, spool_id) == 0) {
            recentSpools[i].synced_to_spoolman = synced;
            break;
        }
    }

    xSemaphoreGive(tagMutex);
}
