#include "PrusaLinkAPIStrategy.h"
#include "BgcodeParser.h"

#ifndef NATIVE_TEST
#include "ConfigurationManager.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include "esp_heap_caps.h"
#include "lwip/tcpip.h"
#include "lwip/priv/tcp_priv.h"
#else
#include "platform/NativePlatform.h"
#endif

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cctype>

static constexpr size_t URL_BUFFER_SIZE = 192;

//#define PRUSALINK_HEAP_TRACE 1
#ifndef PRUSALINK_HEAP_TRACE
#define PRUSALINK_HEAP_TRACE 0
#endif

//#define PRUSALINK_API_TRACE 1
#ifndef PRUSALINK_API_TRACE
#define PRUSALINK_API_TRACE 0
#endif

using namespace io;
using namespace json;

bool PrusaLinkAPIStrategy::isBgcodePath(const char* downloadRef) {
    if (downloadRef == nullptr || downloadRef[0] == '\0') {
        return false;
    }
    // Match .bgcode, .BGCODE, and FAT32 8.3 short filenames .bgc / .BGC
    for (const char* p = downloadRef; *p != '\0'; ++p) {
        if (p[0] == '.' &&
            tolower(static_cast<unsigned char>(p[1])) == 'b' &&
            tolower(static_cast<unsigned char>(p[2])) == 'g' &&
            tolower(static_cast<unsigned char>(p[3])) == 'c') {
            return true;
        }
    }
    return false;
}

#if PRUSALINK_API_TRACE
static const char* nodeTypeName(json_node_type node) {
    switch (node) {
        case json_node_type::field: return "field";
        case json_node_type::object: return "object";
        case json_node_type::end_object: return "end_object";
        case json_node_type::array: return "array";
        case json_node_type::end_array: return "end_array";
        case json_node_type::value: return "value";
        case json_node_type::value_part: return "value_part";
        case json_node_type::end_value_part: return "end_value_part";
        default: return "unknown";
    }
}

static const char* valueTypeName(json_value_type type) {
    switch (type) {
        case json_value_type::none: return "none";
        case json_value_type::null: return "null";
        case json_value_type::integer: return "integer";
        case json_value_type::real: return "real";
        case json_value_type::boolean: return "boolean";
        default: return "unknown";
    }
}

static void logReaderNode(const char* tag, json_reader& reader) {
    const char* text = reader.value();
    if (text == nullptr) {
        text = "";
    }
    Serial.printf("PrusaLinkAPIStrategy[%s]: depth=%u node=%s value_type=%s value='%s'\n",
                  tag,
                  reader.depth(),
                  nodeTypeName(reader.node_type()),
                  valueTypeName(reader.value_type()),
                  text);
}
#endif

static bool readStringValue(json_reader& reader, char* out, size_t outSize) {
    if (out == nullptr || outSize == 0) {
        return false;
    }
    out[0] = '\0';
    json_node_type node = reader.node_type();
    if (node != json_node_type::value &&
        node != json_node_type::value_part &&
        node != json_node_type::end_value_part) {
        return false;
    }

    size_t written = 0;
    auto appendValue = [&]() {
        const char* part = reader.value();
        if (part == nullptr) {
            return;
        }
        while (*part != '\0' && written + 1 < outSize) {
            out[written++] = *part++;
        }
        out[written] = '\0';
    };

    appendValue();
    if (node == json_node_type::value_part) {
        while (reader.read()) {
            json_node_type next = reader.node_type();
            if (next != json_node_type::value_part &&
                next != json_node_type::end_value_part) {
                return written > 0;
            }
            appendValue();
            if (next == json_node_type::end_value_part) {
                break;
            }
        }
    }
    return written > 0;
}

void PrusaLinkAPIStrategy::parseStatusPayload(stream& stm, bool& outHasJob, int& outJobId, float& outProgress,
                                              char* outPrinterState, size_t outPrinterStateSize) {
    outHasJob = false;
    outJobId = -1;
    outProgress = 0.0f;
    if (outPrinterState != nullptr && outPrinterStateSize > 0) {
        outPrinterState[0] = '\0';
    }

    json_reader reader(stm);
    bool inJobObject = false;
    unsigned jobDepth = 0;
    bool inPrinterObject = false;
    unsigned printerDepth = 0;

    while (reader.read()) {
        json_node_type node = reader.node_type();
#if PRUSALINK_API_TRACE
        logReaderNode("status", reader);
#endif
        if (node == json_node_type::field) {
            char field[32];
            const char* rawField = reader.value();
            if (rawField == nullptr) continue;
            strncpy(field, rawField, sizeof(field) - 1);
            field[sizeof(field) - 1] = '\0';

            if (!inJobObject && !inPrinterObject && strcmp(field, "job") == 0) {
                if (reader.read() && reader.node_type() == json_node_type::object) {
                    inJobObject = true;
                    jobDepth = reader.depth();
                }
                continue;
            }
            if (!inJobObject && !inPrinterObject && strcmp(field, "printer") == 0) {
                if (reader.read() && reader.node_type() == json_node_type::object) {
                    inPrinterObject = true;
                    printerDepth = reader.depth();
                }
                continue;
            }
            if (inJobObject && reader.depth() == jobDepth && strcmp(field, "id") == 0) {
                if (reader.read() &&
                    reader.node_type() == json_node_type::value &&
                    (reader.value_type() == json_value_type::integer ||
                     reader.value_type() == json_value_type::real)) {
                    outJobId = static_cast<int>(reader.value_int());
                    outHasJob = true;
                }
                continue;
            }
            if (inJobObject && reader.depth() == jobDepth && strcmp(field, "progress") == 0) {
                if (reader.read() &&
                    reader.node_type() == json_node_type::value &&
                    (reader.value_type() == json_value_type::real ||
                     reader.value_type() == json_value_type::integer)) {
                    outProgress = static_cast<float>(reader.value_real());
                }
                continue;
            }
            if (inPrinterObject && reader.depth() == printerDepth && strcmp(field, "state") == 0) {
                if (outPrinterState != nullptr && outPrinterStateSize > 0 && reader.read()) {
                    readStringValue(reader, outPrinterState, outPrinterStateSize);
                }
                continue;
            }
        } else if (node == json_node_type::end_object) {
            if (inJobObject && reader.depth() < jobDepth) {
                inJobObject = false;
            }
            if (inPrinterObject && reader.depth() < printerDepth) {
                inPrinterObject = false;
            }
        }
    }
}

void PrusaLinkAPIStrategy::parseJobPayload(stream& stm, char* outState, size_t outStateSize, float& outFilamentG,
                                           char* outDownloadRef, size_t outDownloadRefSize, size_t& outFileSize,
                                           float* outProgress) {
    if (outState != nullptr && outStateSize > 0) {
        outState[0] = '\0';
    }
    if (outDownloadRef != nullptr && outDownloadRefSize > 0) {
        outDownloadRef[0] = '\0';
    }
    outFilamentG = 0.0f;
    outFileSize = 0;
    if (outProgress != nullptr) {
        *outProgress = 0.0f;
    }

    json_reader reader(stm);
    int fileDepth = -1;
    int metaDepth = -1;
    int refsDepth = -1;

    while (reader.read()) {
        json_node_type node = reader.node_type();
#if PRUSALINK_API_TRACE
        logReaderNode("job", reader);
#endif
        if (node == json_node_type::field) {
            char field[48];
            const char* rawField = reader.value();
            if (rawField == nullptr) continue;
            strncpy(field, rawField, sizeof(field) - 1);
            field[sizeof(field) - 1] = '\0';

            const bool atTopLevel = (fileDepth < 0 && metaDepth < 0 && refsDepth < 0);
            if (atTopLevel && strcmp(field, "state") == 0) {
                if (outState != nullptr && outStateSize > 0 && reader.read()) {
                    readStringValue(reader, outState, outStateSize);
                }
                continue;
            }

            if (atTopLevel && strcmp(field, "progress") == 0) {
                if (outProgress != nullptr && reader.read() &&
                    reader.node_type() == json_node_type::value &&
                    (reader.value_type() == json_value_type::real ||
                     reader.value_type() == json_value_type::integer)) {
                    *outProgress = static_cast<float>(reader.value_real());
                }
                continue;
            }

            if (atTopLevel && strcmp(field, "file") == 0) {
                if (reader.read() && reader.node_type() == json_node_type::object) {
                    fileDepth = static_cast<int>(reader.depth());
                }
                continue;
            }

            if (fileDepth >= 0 && static_cast<int>(reader.depth()) == fileDepth && strcmp(field, "meta") == 0) {
                if (reader.read() && reader.node_type() == json_node_type::object) {
                    metaDepth = static_cast<int>(reader.depth());
                }
                continue;
            }

            if (fileDepth >= 0 && static_cast<int>(reader.depth()) == fileDepth && strcmp(field, "refs") == 0) {
                if (reader.read() && reader.node_type() == json_node_type::object) {
                    refsDepth = static_cast<int>(reader.depth());
                }
                continue;
            }

            if (fileDepth >= 0 && static_cast<int>(reader.depth()) == fileDepth && strcmp(field, "size") == 0) {
                if (reader.read() &&
                    reader.node_type() == json_node_type::value &&
                    (reader.value_type() == json_value_type::integer ||
                     reader.value_type() == json_value_type::real)) {
                    outFileSize = static_cast<size_t>(reader.value_int());
                }
                continue;
            }

            if (metaDepth >= 0 && static_cast<int>(reader.depth()) == metaDepth &&
                (strcmp(field, "filament used [g]") == 0 || strcmp(field, "total filament used [g]") == 0)) {
                if (reader.read() &&
                    reader.node_type() == json_node_type::value &&
                    (reader.value_type() == json_value_type::real ||
                     reader.value_type() == json_value_type::integer)) {
                    float val = static_cast<float>(reader.value_real());
                    if (val > 0.0f) {
                        outFilamentG = val;
                    }
                }
                continue;
            }

            if (refsDepth >= 0 && static_cast<int>(reader.depth()) == refsDepth && strcmp(field, "download") == 0) {
                if (outDownloadRef != nullptr && outDownloadRefSize > 0 && reader.read()) {
                    readStringValue(reader, outDownloadRef, outDownloadRefSize);
                }
                continue;
            }
        } else if (node == json_node_type::end_object) {
            // Note: htcw_json decrements reader.depth() BEFORE returning end_object
            const int depth = static_cast<int>(reader.depth());
            if (refsDepth >= 0 && depth < refsDepth) {
                refsDepth = -1;
            }
            if (metaDepth >= 0 && depth < metaDepth) {
                metaDepth = -1;
            }
            if (fileDepth >= 0 && depth < fileDepth) {
                fileDepth = -1;
            }
        }
    }
}

#ifndef NATIVE_TEST

static bool buildUrl(char* out, size_t outSize, const char* base, const char* path) {
    if (out == nullptr || outSize == 0 || base == nullptr || path == nullptr) {
        return false;
    }
    size_t baseLen = strlen(base);
    while (baseLen > 0 && base[baseLen - 1] == '/') {
        baseLen--;
    }
    const char* p = path;
    if (*p == '/') p++;
    int written = snprintf(out, outSize, "%.*s/%s", static_cast<int>(baseLen), base, p);
    return written > 0 && static_cast<size_t>(written) < outSize;
}

static bool buildJobUrl(char* out, size_t outSize, const char* base, int jobId) {
    char jobPath[32];
    snprintf(jobPath, sizeof(jobPath), "/api/v1/job/%d", jobId);
    return buildUrl(out, outSize, base, jobPath);
}

static float parseFilamentFromRawJobJson(const String& response) {
    static const char* const jsonKeys[] = {
        "\"filament used [g]\"",
        "\"total filament used [g]\"",
        "\"filament_used_g\"",
        "\"filament_used\"",
        "\"filament_weight\""
    };

    for (const char* key : jsonKeys) {
        int keyPos = response.indexOf(key);
        if (keyPos < 0) continue;

        const char* raw = response.c_str();
        const char* p = raw + keyPos + strlen(key);

        while (*p != '\0' && *p != ':') {
            p++;
        }
        if (*p != ':') continue;
        p++;

        while (*p != '\0' && isspace(static_cast<unsigned char>(*p))) {
            p++;
        }

        char* end = nullptr;
        float val = strtof(p, &end);
        if (end != p && val > 0.0f) {
            return val;
        }
    }
    return 0.0f;
}

static size_t parseFileSizeFromRawJobJson(const String& response) {
    int pos = response.indexOf("\"size\":");
    if (pos < 0) pos = response.indexOf("\"size\" :");
    if (pos < 0) return 0;
    const char* p = response.c_str() + pos + 6;
    while (*p != '\0' && (*p == ' ' || *p == ':')) p++;
    return static_cast<size_t>(atol(p));
}

static void logHeapSnapshot(const char* stage) {
#if PRUSALINK_HEAP_TRACE
    Serial.printf("PrusaLinkAPIStrategy: Heap %s free=%u largest=%u\n",
             stage,
             static_cast<unsigned>(ESP.getFreeHeap()),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
#else
    (void)stage;
#endif
}

void PrusaLinkAPIStrategy::update() {
    auto& config = ConfigurationManager::getInstance();

    // Reset state
    connected = false;
    hasJob = false;
    jobId = -1;
    progress = 0.0f;
    totalFilamentG = 0.0f;
    jobState[0] = '\0';

    // Acquire HTTP mutex if available
    bool mutexHeld = false;
    if (httpMutex_ != nullptr) {
        if (xSemaphoreTake(httpMutex_, pdMS_TO_TICKS(10000)) != pdTRUE) {
            Serial.println("PrusaLinkAPIStrategy: Could not acquire HTTP mutex");
            return;
        }
        mutexHeld = true;
    }

    do {
        HTTPClient http;
        http.useHTTP10(true);
        http.setReuse(false);

        // Get quick status
        char statusUrl[URL_BUFFER_SIZE];
        if (!buildUrl(statusUrl, sizeof(statusUrl), config.getPrusaLinkURL(), "/api/v1/status")) {
            Serial.println("PrusaLinkAPIStrategy: status URL too long");
            break;
        }
        http.begin(statusUrl);
        http.addHeader("X-Api-Key", config.getPrusaLinkAPIKey());

        logHeapSnapshot("before_status_get");
        int statusCode = http.GET();
        if (statusCode != 200) {
            Serial.printf("PrusaLinkAPIStrategy: Status request failed: %d\n", statusCode);
            logHeapSnapshot("status_get_failed");
            http.end();
            break;
        }
        logHeapSnapshot("after_status_get");

        connected = true;
        logHeapSnapshot("before_status_deserialize");
        arduino_stream statusStream(&http.getStream());
        char printerState[sizeof(jobState)] = {0};
        parseStatusPayload(statusStream, hasJob, jobId, progress, printerState, sizeof(printerState));
        logHeapSnapshot("after_status_deserialize");
        http.end();

        // Preserve printerState in jobState so PrinterManager can inspect it even if job just disappeared
        if (printerState[0] != '\0') {
            strncpy(jobState, printerState, sizeof(jobState) - 1);
            jobState[sizeof(jobState) - 1] = '\0';
        }

        if (!hasJob) {
            break;
        }

        // Get detailed job info
        char jobUrl[URL_BUFFER_SIZE];
        if (!buildUrl(jobUrl, sizeof(jobUrl), config.getPrusaLinkURL(), "/api/v1/job")) {
            Serial.println("PrusaLinkAPIStrategy: job URL too long");
            break;
        }
        http.begin(jobUrl);
        http.addHeader("X-Api-Key", config.getPrusaLinkAPIKey());

        logHeapSnapshot("before_job_get");
        int jobStatusCode = http.GET();
        if (jobStatusCode != 200) {
            Serial.printf("PrusaLinkAPIStrategy: Job request failed: %d\n", jobStatusCode);
            logHeapSnapshot("job_get_failed");
            http.end();
            break;
        }
        logHeapSnapshot("after_job_get");

        String response = http.getString();
        Serial.printf("PrusaLinkAPIStrategy: Job API response: %s\n", response.c_str());

        size_t respLen = response.length();
        uint8_t* respBuf = static_cast<uint8_t*>(malloc(respLen));
        if (respBuf == nullptr) {
            Serial.println("PrusaLinkAPIStrategy: Failed to allocate buffer for job response");
            http.end();
            break;
        }
        memcpy(respBuf, response.c_str(), respLen);
        buffer_stream bufStream(respBuf, respLen);

        char parsedState[sizeof(jobState)] = {0};
        char parsedDownloadRef[sizeof(savedDownloadRef)] = {0};
        float parsedFilamentG = 0.0f;
        size_t parsedFileSize = 0;
        float parsedJobProgress = 0.0f;
        parseJobPayload(bufStream, parsedState, sizeof(parsedState), parsedFilamentG,
                        parsedDownloadRef, sizeof(parsedDownloadRef), parsedFileSize, &parsedJobProgress);

        free(respBuf);
        logHeapSnapshot("after_job_deserialize");
        http.end();

        if (progress <= 0.0f && parsedJobProgress > 0.0f) {
            progress = parsedJobProgress;
        }

        if (parsedFileSize == 0) {
            parsedFileSize = parseFileSizeFromRawJobJson(response);
        }
        savedFileSize = parsedFileSize;

        if (parsedFilamentG <= 0.0f) {
            float rawParsed = parseFilamentFromRawJobJson(response);
            if (rawParsed > 0.0f) {
                parsedFilamentG = rawParsed;
                Serial.printf("PrusaLinkAPIStrategy: Parsed filament from raw job JSON fallback: %.2fg\n", parsedFilamentG);
            }
        }

        if (parsedState[0] != '\0') {
            strncpy(jobState, parsedState, sizeof(jobState) - 1);
            jobState[sizeof(jobState) - 1] = '\0';
        }
        totalFilamentG = parsedFilamentG;

        if (parsedDownloadRef[0] != '\0') {
            strncpy(savedDownloadRef, parsedDownloadRef, sizeof(savedDownloadRef) - 1);
            savedDownloadRef[sizeof(savedDownloadRef) - 1] = '\0';
            savedDownloadRefJobId = jobId;
        }

        // Use cached bgcode/gcode data if available, or fetch
        if (totalFilamentG <= 0.0f && savedDownloadRef[0] != '\0') {
            if (jobId == bgcodeFilamentJobId && bgcodeFilamentG > 0.0f) {
                totalFilamentG = bgcodeFilamentG;
            } else if (bgcodeFilamentJobId != jobId) {
                // 1. Try PrusaLink Files API first (official endpoint with parsed metadata)
                float fromFiles = fetchFilamentFromFilesApi(savedDownloadRef);
                if (fromFiles > 0.0f) {
                    totalFilamentG = fromFiles;
                    bgcodeFilamentJobId = jobId;
                    bgcodeFilamentG = fromFiles;
                } else {
                    // Release HTTP mutex before file streaming so SpoolmanManager can run immediately if a tag is scanned
                    if (mutexHeld && httpMutex_ != nullptr) {
                        xSemaphoreGive(httpMutex_);
                        mutexHeld = false;
                    }
                    // 2. Fall back to reading file directly
                    float fromHeader = fetchFilamentFromBgcode(savedDownloadRef, savedFileSize);
                    bgcodeFilamentJobId = jobId;
                    bgcodeFilamentG = fromHeader;
                    if (fromHeader > 0.0f) {
                        totalFilamentG = fromHeader;
                    }
                }
            }
        }
    } while (false);

    if (mutexHeld) {
        xSemaphoreGive(httpMutex_);
    }
}

float PrusaLinkAPIStrategy::fetchFilamentFromFilesApi(const char* downloadRef) {
    if (downloadRef == nullptr || downloadRef[0] == '\0') return 0.0f;
    auto& config = ConfigurationManager::getInstance();

    char filesPath[192];
    if (downloadRef[0] == '/') {
        snprintf(filesPath, sizeof(filesPath), "/api/v1/files%s", downloadRef);
    } else {
        snprintf(filesPath, sizeof(filesPath), "/api/v1/files/%s", downloadRef);
    }

    char filesUrl[URL_BUFFER_SIZE];
    if (!buildUrl(filesUrl, sizeof(filesUrl), config.getPrusaLinkURL(), filesPath)) {
        return 0.0f;
    }

    HTTPClient http;
    http.useHTTP10(true);
    http.setReuse(false);
    http.begin(filesUrl);
    http.addHeader("X-Api-Key", config.getPrusaLinkAPIKey());

    int code = http.GET();
    Serial.printf("PrusaLinkAPIStrategy: GET %s -> code %d\n", filesUrl, code);
    if (code == 200) {
        String resp = http.getString();
        http.end();
        Serial.printf("PrusaLinkAPIStrategy: Files API response: %s\n", resp.c_str());
        float filamentG = parseFilamentFromRawJobJson(resp);
        if (filamentG > 0.0f) {
            Serial.printf("PrusaLinkAPIStrategy: Parsed filament from Files API: %.2fg\n", filamentG);
            return filamentG;
        }
    } else {
        http.end();
    }
    return 0.0f;
}

float PrusaLinkAPIStrategy::fetchFilamentFromBgcode(const char* downloadRef, size_t fileSize) {
    auto& config = ConfigurationManager::getInstance();
    if (downloadRef == nullptr || downloadRef[0] == '\0') return 0.0f;

    // Parse host/port from PrusaLink URL to avoid HTTPClient URL-encoding issues
    const char* baseUrl = config.getPrusaLinkURL();
    const char* scheme = strstr(baseUrl, "://");
    if (scheme == nullptr) return 0.0f;
    const char* hostPort = scheme + 3;
    char host[96] = {0};
    int port = 80;
    const char* colon = strchr(hostPort, ':');
    if (colon != nullptr) {
        size_t hostLen = static_cast<size_t>(colon - hostPort);
        if (hostLen >= sizeof(host)) hostLen = sizeof(host) - 1;
        memcpy(host, hostPort, hostLen);
        host[hostLen] = '\0';
        port = atoi(colon + 1);
    } else {
        const char* slash = strchr(hostPort, '/');
        size_t hostLen = (slash != nullptr) ? static_cast<size_t>(slash - hostPort) : strlen(hostPort);
        if (hostLen >= sizeof(host)) hostLen = sizeof(host) - 1;
        memcpy(host, hostPort, hostLen);
        host[hostLen] = '\0';
    }
    char* slash = strchr(host, '/');
    if (slash != nullptr) *slash = '\0';

    const bool isBgcode = isBgcodePath(downloadRef);
    static constexpr size_t GCODE_TAIL_WINDOW = 65536; // 64KB covers OrcaSlicer/PrusaSlicer CONFIG_BLOCK + filament summary

    char rangeHeader[48] = {0};
    bool requestedTailRange = false;
    if (isBgcode) {
        snprintf(rangeHeader, sizeof(rangeHeader), "bytes=0-%zu", BGCODE_BUF_SIZE - 1);
    } else if (fileSize > GCODE_TAIL_WINDOW) {
        snprintf(rangeHeader, sizeof(rangeHeader), "bytes=%zu-%zu", fileSize - GCODE_TAIL_WINDOW, fileSize - 1);
        requestedTailRange = true;
    } else if (fileSize > 0) {
        snprintf(rangeHeader, sizeof(rangeHeader), "bytes=0-%zu", fileSize - 1);
    } else {
        snprintf(rangeHeader, sizeof(rangeHeader), "bytes=0-%zu", GCODE_TAIL_WINDOW - 1);
    }

    const int maxAttempts = 3;
    const int baseDelayMs = 500;
    const size_t BUF_SIZE = BGCODE_BUF_SIZE;
    uint8_t* buf = bgcodeBuf_;
    float result = 0.0f;

    auto flushTcpDelayedAck = []() {
        tcpip_try_callback([](void*) { tcp_fasttmr(); }, nullptr);
    };

    auto fetchRangeAndParse = [&](const char* currentRange, int& outHttpCode) -> float {
        outHttpCode = -1;
        WiFiClient client;
        HTTPClient http;
        http.setReuse(false);
        http.begin(client, host, port, downloadRef);
        http.addHeader("X-Api-Key", config.getPrusaLinkAPIKey());
        if (currentRange != nullptr && currentRange[0] != '\0') {
            http.addHeader("Range", currentRange);
        }

        logHeapSnapshot("before_bgcode_get");
        int code = http.GET();
        outHttpCode = code;
        if (code != 200 && code != 206) {
            http.end();
            client.stop();
            return 0.0f;
        }

        int contentLength = http.getSize();
        Serial.printf("PrusaLinkAPIStrategy: file fetch HTTP %d (Range: %s), size=%d\n",
                      code, currentRange ? currentRange : "none", contentLength);
        flushTcpDelayedAck();

        // Peek first 4 bytes to distinguish binary bgcode vs text G-code
        uint8_t magic[4] = {0};
        size_t magicRead = 0;
        unsigned long magicTimeout = millis() + 4000;
        while (magicRead < 4 && millis() < magicTimeout) {
            if (client.available()) {
                magic[magicRead++] = static_cast<uint8_t>(client.read());
                flushTcpDelayedAck();
            } else if (!client.connected()) {
                break;
            } else {
                flushTcpDelayedAck();
                delay(2);
            }
        }

        float parsedResult = 0.0f;
        if (magicRead == 4 && memcmp(magic, "GCDE", 4) == 0) {
            // Binary bgcode: read up to BUF_SIZE bytes into buf
            memcpy(buf, magic, 4);
            size_t bytesRead = 4;
            unsigned long timeout = millis() + 5000;
            while (bytesRead < BUF_SIZE && millis() < timeout) {
                if (client.available()) {
                    int toRead = client.available();
                    if (toRead > static_cast<int>(BUF_SIZE - bytesRead)) {
                        toRead = static_cast<int>(BUF_SIZE - bytesRead);
                    }
                    int n = client.readBytes(buf + bytesRead, toRead);
                    if (n > 0) {
                        bytesRead += static_cast<size_t>(n);
                        flushTcpDelayedAck();
                    }
                } else if (!client.connected()) {
                    break;
                } else {
                    flushTcpDelayedAck();
                    delay(2);
                }
            }
            client.stop();
            http.end();

            Serial.printf("PrusaLinkAPIStrategy: binary bgcode read %zu bytes\n", bytesRead);
            parsedResult = parseBgcodeFilament(buf, bytesRead);
            if (parsedResult > 0.0f) {
                Serial.printf("PrusaLinkAPIStrategy: Parsed filament from bgcode binary: %.2fg\n", parsedResult);
            }
        } else {
            // Text G-code: stream in bulk 8KB chunks into bgcodeBuf_ and parse comments
            Serial.printf("PrusaLinkAPIStrategy: Streaming text G-code in chunks (magic=0x%02X%02X%02X%02X)...\n",
                          magic[0], magic[1], magic[2], magic[3]);

            GcodeStreamState st;
            feedGcodeStreamChunk(st, magic, magicRead);

            unsigned long streamStartMs = millis();
            unsigned long streamDeadline = streamStartMs + 300000;
            unsigned long lastDataMs = streamStartMs;
            size_t totalBytesStreamed = magicRead;
            size_t lastYieldBytes = 0;

            while (!st.done && millis() < streamDeadline && (millis() - lastDataMs < 5000)) {
                if (contentLength > 0 && totalBytesStreamed >= static_cast<size_t>(contentLength)) {
                    break;
                }
                int avail = client.available();
                if (avail > 0) {
                    size_t toRead = static_cast<size_t>(avail);
                    if (toRead > BUF_SIZE) toRead = BUF_SIZE;
                    int n = client.read(buf, toRead);
                    if (n > 0) {
                        flushTcpDelayedAck();
                        lastDataMs = millis();
                        totalBytesStreamed += static_cast<size_t>(n);
                        feedGcodeStreamChunk(st, buf, static_cast<size_t>(n));
                        if (totalBytesStreamed - lastYieldBytes >= 32768) {
                            lastYieldBytes = totalBytesStreamed;
                            vTaskDelay(1);
                        }
                    }
                } else if (!client.connected()) {
                    break;
                } else {
                    flushTcpDelayedAck();
                    vTaskDelay(1);
                }
            }

            client.stop();
            http.end();

            parsedResult = finalizeGcodeStream(st);
            Serial.printf("PrusaLinkAPIStrategy: Text G-code stream finished in %lums (%d lines, %zu bytes, result=%.2fg)\n",
                          millis() - streamStartMs, st.linesRead, totalBytesStreamed, parsedResult);
        }
        return parsedResult;
    };

    for (int attempt = 1; attempt <= maxAttempts; attempt++) {
        if (attempt > 1) {
            int delayMs = baseDelayMs * (attempt - 1);
            Serial.printf("PrusaLinkAPIStrategy: file fetch attempt %d/%d after %dms delay\n", attempt, maxAttempts, delayMs);
            vTaskDelay(pdMS_TO_TICKS(delayMs));
        }

        int code = -1;
        result = fetchRangeAndParse(rangeHeader, code);
        if (result > 0.0f) {
            break;
        }

        // If we requested the tail 64KB of a .gcode file (HTTP 206) and didn't find filament in the footer,
        // also check the first 16KB header (for slicers that only write filament usage in the header).
        if (requestedTailRange && code == 206 && result <= 0.0f) {
            int headerCode = -1;
            result = fetchRangeAndParse("bytes=0-16383", headerCode);
            if (result > 0.0f) {
                break;
            }
        }

        if (code == 200) {
            // Full file was already streamed from byte 0; no need to retry
            break;
        }
    }

    return result;
}

float PrusaLinkAPIStrategy::fetchDeferredFilament(int expectedJobId) {
    if (expectedJobId < 0) return 0.0f;
    if (savedDownloadRefJobId != expectedJobId) {
        Serial.printf("PrusaLinkAPIStrategy: Deferred filament skipped, expected job %d but saved ref is job %d\n",
            expectedJobId, savedDownloadRefJobId);
        return 0.0f;
    }

    if (bgcodeFilamentJobId == expectedJobId && bgcodeFilamentG > 0.0f)
        return bgcodeFilamentG;

    bool mutexHeld = false;
    if (httpMutex_ != nullptr) {
        if (xSemaphoreTake(httpMutex_, pdMS_TO_TICKS(10000)) != pdTRUE) return 0.0f;
        mutexHeld = true;
    }

    auto& config = ConfigurationManager::getInstance();
    float result = 0.0f;

    // 1. Try job API first — may have metadata now that print is done
    HTTPClient http;
    http.useHTTP10(true);
    http.setReuse(false);
    char jobUrl[URL_BUFFER_SIZE];
    if (!buildJobUrl(jobUrl, sizeof(jobUrl), config.getPrusaLinkURL(), expectedJobId)) {
        if (mutexHeld) xSemaphoreGive(httpMutex_);
        Serial.println("PrusaLinkAPIStrategy: deferred job URL too long");
        return 0.0f;
    }
    const int maxAttempts = 3;
    int code = -1;
    for (int attempt = 1; attempt <= maxAttempts; attempt++) {
        if (attempt > 1) {
            int delayMs = (attempt - 1) * 1000;
            Serial.printf("PrusaLinkAPIStrategy: Deferred job API retry %d/%d after %dms delay\n", attempt, maxAttempts, delayMs);
            delay(delayMs);
        }
        http.begin(jobUrl);
        http.addHeader("X-Api-Key", config.getPrusaLinkAPIKey());
        logHeapSnapshot("before_deferred_job_get");
        code = http.GET();
        if (code == 200) {
            logHeapSnapshot("before_deferred_job_deserialize");

            String deferredResponse = http.getString();
            size_t deferredLen = deferredResponse.length();
            uint8_t* deferredBuf = static_cast<uint8_t*>(malloc(deferredLen));
            if (deferredBuf != nullptr) {
                memcpy(deferredBuf, deferredResponse.c_str(), deferredLen);
                buffer_stream bufStream(deferredBuf, deferredLen);

                char stateBuf[2] = {0};
                char downloadRefBuf[2] = {0};
                float parsedFilament = 0.0f;
                size_t dummySize = 0;
                parseJobPayload(bufStream, stateBuf, sizeof(stateBuf), parsedFilament,
                                downloadRefBuf, sizeof(downloadRefBuf), dummySize);

                free(deferredBuf);

                if (parsedFilament <= 0.0f) {
                    float rawParsed = parseFilamentFromRawJobJson(deferredResponse);
                    if (rawParsed > 0.0f) {
                        parsedFilament = rawParsed;
                        Serial.printf("PrusaLinkAPIStrategy: Parsed deferred filament from raw JSON fallback: %.2fg\n",
                            parsedFilament);
                    }
                }

                if (parsedFilament > 0.0f) {
                    result = parsedFilament;
                    Serial.printf("PrusaLinkAPIStrategy: Got deferred filament from job API: %.2fg\n", result);
                } else {
                    Serial.printf("PrusaLinkAPIStrategy: Deferred job API attempt %d/%d had no filament metadata yet\n",
                        attempt, maxAttempts);
                }
            } else {
                Serial.println("PrusaLinkAPIStrategy: Failed to allocate buffer for deferred response");
            }

            logHeapSnapshot("after_deferred_job_deserialize");
            http.end();
            if (result > 0.0f) {
                break;
            }
            continue;
        } else {
            Serial.printf("PrusaLinkAPIStrategy: Deferred job API attempt %d/%d failed: %d\n", attempt, maxAttempts, code);
            logHeapSnapshot("deferred_job_get_failed");
            http.end();
            if (code == 404 || code == 405 || code == 204) {
                Serial.printf("PrusaLinkAPIStrategy: HTTP %d - skipping further job API retries\n", code);
                break;
            }
        }
    }

    // 2. Try Files API next (official endpoint with parsed metadata)
    if (result <= 0.0f && savedDownloadRef[0] != '\0') {
        result = fetchFilamentFromFilesApi(savedDownloadRef);
        if (result > 0.0f) {
            Serial.printf("PrusaLinkAPIStrategy: Got deferred filament from Files API: %.2fg\n", result);
        }
    }

    // 3. Fall back to bgcode/gcode file parsing
    if (result <= 0.0f && savedDownloadRef[0] != '\0') {
        if (mutexHeld && httpMutex_ != nullptr) {
            xSemaphoreGive(httpMutex_);
            mutexHeld = false;
        }
        result = fetchFilamentFromBgcode(savedDownloadRef, savedFileSize);
    }

    bgcodeFilamentJobId = expectedJobId;
    bgcodeFilamentG = result;

    if (mutexHeld) xSemaphoreGive(httpMutex_);
    return bgcodeFilamentG;
}

void PrusaLinkAPIStrategy::diagGet(const char* path) {
    if (path == nullptr || path[0] == '\0') return;
    bool mutexHeld = false;
    if (httpMutex_ != nullptr && xSemaphoreTake(httpMutex_, pdMS_TO_TICKS(10000)) == pdTRUE) {
        mutexHeld = true;
    }
    auto& config = ConfigurationManager::getInstance();
    char url[URL_BUFFER_SIZE];
    if (buildUrl(url, sizeof(url), config.getPrusaLinkURL(), path)) {
        HTTPClient http;
        http.useHTTP10(true);
        http.setReuse(false);
        http.begin(url);
        http.addHeader("X-Api-Key", config.getPrusaLinkAPIKey());
        int code = http.GET();
        Serial.printf("DIAG GET %s -> HTTP %d\n", url, code);
        if (code > 0) {
            WiFiClient& stream = http.getStream();
            size_t total = 0;
            unsigned long deadline = millis() + 8000;
            while ((stream.available() || stream.connected()) && millis() < deadline && total < 16384) {
                int avail = stream.available();
                if (avail > 0) {
                    int toRead = avail > 256 ? 256 : avail;
                    uint8_t tmp[257];
                    int n = stream.read(tmp, toRead);
                    if (n > 0) {
                        tmp[n] = '\0';
                        Serial.print(reinterpret_cast<const char*>(tmp));
                        total += static_cast<size_t>(n);
                    }
                } else {
                    delay(5);
                }
            }
            Serial.printf("\nDIAG GET finished (%zu bytes)\n", total);
        }
        http.end();
    }
    if (mutexHeld) xSemaphoreGive(httpMutex_);
}

void PrusaLinkAPIStrategy::diagTestFile(const char* downloadRef, size_t fileSize) {
    if (downloadRef == nullptr || downloadRef[0] == '\0') return;
    bool mutexHeld = false;
    if (httpMutex_ != nullptr && xSemaphoreTake(httpMutex_, pdMS_TO_TICKS(10000)) == pdTRUE) {
        mutexHeld = true;
    }
    Serial.printf("DIAG TESTFILE ref='%s' size=%zu isBgcode=%d\n",
                  downloadRef, fileSize, isBgcodePath(downloadRef) ? 1 : 0);
    float f1 = fetchFilamentFromFilesApi(downloadRef);
    Serial.printf("DIAG TESTFILE fetchFilamentFromFilesApi -> %.2fg\n", f1);
    if (mutexHeld && httpMutex_ != nullptr) {
        xSemaphoreGive(httpMutex_);
        mutexHeld = false;
    }
    float f2 = fetchFilamentFromBgcode(downloadRef, fileSize);
    Serial.printf("DIAG TESTFILE fetchFilamentFromBgcode -> %.2fg\n", f2);
    if (mutexHeld) xSemaphoreGive(httpMutex_);
}

void PrusaLinkAPIStrategy::diagnose() {
    auto& config = ConfigurationManager::getInstance();
    Serial.printf("=== PRUSALINK DIAG ===\nURL: %s | PrinterType: %u | KeyLen: %u\n",
                  config.getPrusaLinkURL(),
                  static_cast<unsigned>(config.getPrinterType()),
                  static_cast<unsigned>(strlen(config.getPrusaLinkAPIKey())));
    diagGet("/api/v1/status");
    diagGet("/api/v1/job");
    Serial.println("=== END PRUSALINK DIAG ===");
}

#endif // NATIVE_TEST
