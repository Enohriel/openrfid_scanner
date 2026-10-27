#include "MoonrakerAPIStrategy.h"
#include "ConversionUtils.h"
#include "NFCTypes.h"

#ifndef NATIVE_TEST
#include "ConfigurationManager.h"
#include "NFCManager.h"
#include "ApplicationManager.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#endif

#include <cstring>
#include <cstdio>
#include <cmath>
#include <cctype>

using namespace io;
using namespace json;

static constexpr size_t URL_BUFFER_SIZE = 256;

bool MoonrakerAPIStrategy::buildMoonrakerUrl(char* out, size_t outSize, const char* base, const char* path) {
    if (out == nullptr || outSize == 0 || base == nullptr || base[0] == '\0' || path == nullptr) {
        return false;
    }

    // Strip trailing slashes from base
    size_t baseLen = strlen(base);
    while (baseLen > 0 && base[baseLen - 1] == '/') {
        baseLen--;
    }
    if (baseLen == 0) {
        return false;
    }

    char cleanBase[160];
    if (baseLen >= sizeof(cleanBase)) {
        return false;
    }
    memcpy(cleanBase, base, baseLen);
    cleanBase[baseLen] = '\0';

    // Check if scheme is present; if not, prepend http://
    char withScheme[176];
    const char* schemeSep = strstr(cleanBase, "://");
    if (schemeSep == nullptr) {
        snprintf(withScheme, sizeof(withScheme), "http://%s", cleanBase);
    } else {
        strncpy(withScheme, cleanBase, sizeof(withScheme) - 1);
        withScheme[sizeof(withScheme) - 1] = '\0';
    }

    // Check if host part already specifies a port
    const char* hostStart = strstr(withScheme, "://");
    hostStart = (hostStart != nullptr) ? (hostStart + 3) : withScheme;
    const char* slashInHost = strchr(hostStart, '/');
    const char* colonInHost = strchr(hostStart, ':');
    bool hasPort = (colonInHost != nullptr && (slashInHost == nullptr || colonInHost < slashInHost));

    const char* relPath = (*path == '/') ? (path + 1) : path;

    int written = 0;
    if (hasPort) {
        written = snprintf(out, outSize, "%s/%s", withScheme, relPath);
    } else if (slashInHost != nullptr) {
        size_t hostLen = static_cast<size_t>(slashInHost - withScheme);
        written = snprintf(out, outSize, "%.*s:7125%s/%s",
                           static_cast<int>(hostLen), withScheme, slashInHost, relPath);
    } else {
        written = snprintf(out, outSize, "%s:7125/%s", withScheme, relPath);
    }

    return written > 0 && static_cast<size_t>(written) < outSize;
}

float MoonrakerAPIStrategy::mmToGrams(float lengthMm, float diameterMm, float densityGcm3) {
    if (lengthMm <= 0.0f) return 0.0f;
    float d = (diameterMm > 0.0f) ? diameterMm : 1.75f;
    float rho = (densityGcm3 > 0.0f) ? densityGcm3 : 1.24f;
    float radiusMm = d * 0.5f;
    // Volume in mm^3 = pi * r^2 * lengthMm
    // 1 cm^3 = 1000 mm^3 -> grams = (Volume_mm3 / 1000) * densityGcm3
    constexpr float PI_VAL = 3.14159265f;
    float volumeCm3 = (PI_VAL * radiusMm * radiusMm * lengthMm) / 1000.0f;
    return volumeCm3 * rho;
}

int MoonrakerAPIStrategy::computeJobId(const char* filename) {
    if (filename == nullptr || filename[0] == '\0') {
        return -1;
    }
    // FNV-1a 32-bit hash folded to positive signed int (> 0)
    uint32_t hash = 2166136261u;
    for (const char* p = filename; *p != '\0'; ++p) {
        hash ^= static_cast<uint8_t>(*p);
        hash *= 16777619u;
    }
    int id = static_cast<int>(hash & 0x7FFFFFFFu);
    return (id > 0) ? id : 1;
}

static bool readJsonString(json_reader& reader, char* out, size_t outSize) {
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
        if (part == nullptr) return;
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

void MoonrakerAPIStrategy::parseQueryPayload(stream& stm, MoonrakerQueryResult& out) {
    out = MoonrakerQueryResult{};

    json_reader reader(stm);
    int printStatsDepth = -1;
    int virtualSdcardDepth = -1;
    bool seenStatus = false;

    while (reader.read()) {
        json_node_type node = reader.node_type();
        if (node == json_node_type::field) {
            char field[40];
            const char* rawField = reader.value();
            if (rawField == nullptr) continue;
            strncpy(field, rawField, sizeof(field) - 1);
            field[sizeof(field) - 1] = '\0';

            if (strcmp(field, "status") == 0) {
                seenStatus = true;
                continue;
            }

            if (printStatsDepth < 0 && virtualSdcardDepth < 0) {
                if (strcmp(field, "print_stats") == 0) {
                    if (reader.read() && reader.node_type() == json_node_type::object) {
                        printStatsDepth = static_cast<int>(reader.depth());
                        out.valid = true;
                    }
                    continue;
                }
                if (strcmp(field, "virtual_sdcard") == 0) {
                    if (reader.read() && reader.node_type() == json_node_type::object) {
                        virtualSdcardDepth = static_cast<int>(reader.depth());
                        out.valid = true;
                    }
                    continue;
                }
            }

            if (printStatsDepth >= 0 && static_cast<int>(reader.depth()) == printStatsDepth) {
                if (strcmp(field, "state") == 0) {
                    if (reader.read()) {
                        readJsonString(reader, out.rawState, sizeof(out.rawState));
                    }
                    continue;
                }
                if (strcmp(field, "filename") == 0) {
                    if (reader.read()) {
                        readJsonString(reader, out.filename, sizeof(out.filename));
                    }
                    continue;
                }
                if (strcmp(field, "filament_used") == 0) {
                    if (reader.read() &&
                        reader.node_type() == json_node_type::value &&
                        (reader.value_type() == json_value_type::real ||
                         reader.value_type() == json_value_type::integer)) {
                        out.filamentUsedMm = static_cast<float>(reader.value_real());
                    }
                    continue;
                }
            }

            if (virtualSdcardDepth >= 0 && static_cast<int>(reader.depth()) == virtualSdcardDepth) {
                if (strcmp(field, "progress") == 0) {
                    if (reader.read() &&
                        reader.node_type() == json_node_type::value &&
                        (reader.value_type() == json_value_type::real ||
                         reader.value_type() == json_value_type::integer)) {
                        float rawProg = static_cast<float>(reader.value_real());
                        out.progressPercent = rawProg * 100.0f;
                        if (out.progressPercent < 0.0f) out.progressPercent = 0.0f;
                        if (out.progressPercent > 100.0f) out.progressPercent = 100.0f;
                    }
                    continue;
                }
            }
        } else if (node == json_node_type::end_object) {
            int depth = static_cast<int>(reader.depth());
            if (printStatsDepth >= 0 && depth <= printStatsDepth) {
                printStatsDepth = -1;
            }
            if (virtualSdcardDepth >= 0 && depth <= virtualSdcardDepth) {
                virtualSdcardDepth = -1;
            }
        }
    }

    if (seenStatus) {
        out.valid = true;
    }

    // Map Moonraker print_stats.state to PrinterManager expected states
    if (strcmp(out.rawState, "printing") == 0) {
        strncpy(out.mappedState, "PRINTING", sizeof(out.mappedState) - 1);
        out.hasActiveJob = (out.filename[0] != '\0');
    } else if (strcmp(out.rawState, "paused") == 0) {
        strncpy(out.mappedState, "PAUSED", sizeof(out.mappedState) - 1);
        out.hasActiveJob = (out.filename[0] != '\0');
    } else if (strcmp(out.rawState, "complete") == 0) {
        strncpy(out.mappedState, "FINISHED", sizeof(out.mappedState) - 1);
        out.progressPercent = 100.0f;
        out.hasActiveJob = (out.filename[0] != '\0');
    } else if (strcmp(out.rawState, "cancelled") == 0) {
        strncpy(out.mappedState, "STOPPED", sizeof(out.mappedState) - 1);
        out.hasActiveJob = (out.filename[0] != '\0');
    } else if (strcmp(out.rawState, "error") == 0) {
        strncpy(out.mappedState, "ERROR", sizeof(out.mappedState) - 1);
        out.hasActiveJob = (out.filename[0] != '\0');
    } else {
        // "standby" or empty
        out.mappedState[0] = '\0';
        out.hasActiveJob = false;
    }
}

float MoonrakerAPIStrategy::parseMetadataFilamentWeight(stream& stm) {
    json_reader reader(stm);
    float weightG = 0.0f;
    float lengthMm = 0.0f;

    while (reader.read()) {
        if (reader.node_type() != json_node_type::field) {
            continue;
        }
        char field[40];
        const char* rawField = reader.value();
        if (rawField == nullptr) continue;
        strncpy(field, rawField, sizeof(field) - 1);
        field[sizeof(field) - 1] = '\0';

        if (strcmp(field, "filament_weight_total") == 0) {
            if (reader.read() &&
                reader.node_type() == json_node_type::value &&
                (reader.value_type() == json_value_type::real ||
                 reader.value_type() == json_value_type::integer)) {
                weightG = static_cast<float>(reader.value_real());
            }
        } else if (strcmp(field, "filament_total") == 0) {
            if (reader.read() &&
                reader.node_type() == json_node_type::value &&
                (reader.value_type() == json_value_type::real ||
                 reader.value_type() == json_value_type::integer)) {
                lengthMm = static_cast<float>(reader.value_real());
            }
        }
    }

    if (weightG > 0.0f) {
        return weightG;
    }
    if (lengthMm > 0.0f) {
        return mmToGrams(lengthMm, 1.75f, 1.24f);
    }
    return 0.0f;
}

int MoonrakerAPIStrategy::parseHistoryTotalJobs(stream& stm) {
    json_reader reader(stm);
    while (reader.read()) {
        if (reader.node_type() != json_node_type::field) {
            continue;
        }
        const char* rawField = reader.value();
        if (rawField == nullptr) continue;
        if (strcmp(rawField, "total_jobs") == 0) {
            if (reader.read() &&
                reader.node_type() == json_node_type::value &&
                (reader.value_type() == json_value_type::integer ||
                 reader.value_type() == json_value_type::real)) {
                return static_cast<int>(reader.value_int());
            }
        }
    }
    return -1;
}

#ifndef NATIVE_TEST

static void urlEncodeFilename(const char* src, char* dst, size_t dstSize) {
    if (src == nullptr || dst == nullptr || dstSize == 0) return;
    static const char hex[] = "0123456789ABCDEF";
    size_t pos = 0;
    for (const char* p = src; *p != '\0' && pos + 1 < dstSize; ++p) {
        unsigned char c = static_cast<unsigned char>(*p);
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
            dst[pos++] = static_cast<char>(c);
        } else {
            if (pos + 3 >= dstSize) break;
            dst[pos++] = '%';
            dst[pos++] = hex[(c >> 4) & 0x0F];
            dst[pos++] = hex[c & 0x0F];
        }
    }
    dst[pos] = '\0';
}

void MoonrakerAPIStrategy::resolveActiveSpoolDensityAndDiameter(float& outDensity, float& outDiameter) const {
    outDensity = 1.24f;
    outDiameter = 1.75f;

    CurrentSpoolState state;
    if (NFCManager::getInstance().getCurrentSpoolState(state) && state.tag_data_valid) {
        float tagDensity = 0.0f;
        if (opt_get_density(&state.tag_data, &tagDensity) == OPT_OK && tagDensity > 0.0f) {
            outDensity = tagDensity;
        } else {
            uint8_t matType = OPT_MATERIAL_TYPE_PLA;
            opt_get_material_type(&state.tag_data, &matType);
            outDensity = getDefaultDensity(matType);
        }
        float tagDiam = 0.0f;
        if (opt_get_filament_diameter(&state.tag_data, &tagDiam) == OPT_OK && tagDiam > 0.0f) {
            outDiameter = tagDiam;
        }
        return;
    }

    // Fallback to recent spools if the spool was scanned and then placed on the printer holder
    RecentSpoolEntry recent[NFCManager::MAX_RECENT_SPOOLS];
    size_t count = NFCManager::getInstance().getRecentSpools(recent, NFCManager::MAX_RECENT_SPOOLS);
    if (count > 0) {
        outDensity = getDefaultDensity(recent[0].material_type);
    }
}

int MoonrakerAPIStrategy::fetchHistoryJobNumber() {
    auto& config = ConfigurationManager::getInstance();
    char url[URL_BUFFER_SIZE];
    if (!buildMoonrakerUrl(url, sizeof(url), config.getPrusaLinkURL(), "/server/history/totals")) {
        return -1;
    }

    WiFiClient client;
    HTTPClient http;
    http.useHTTP10(true);
    http.setReuse(false);
    http.setTimeout(3000);
    http.begin(client, url);
    if (strlen(config.getPrusaLinkAPIKey()) > 0) {
        http.addHeader("X-Api-Key", config.getPrusaLinkAPIKey());
    }

    int code = http.GET();
    if (code != 200) {
        http.end();
        return -1;
    }

    String resp = http.getString();
    http.end();

    const_buffer_stream stm(reinterpret_cast<const uint8_t*>(resp.c_str()), resp.length());
    int totalJobs = parseHistoryTotalJobs(stm);
    if (totalJobs > 0) {
        Serial.printf("MoonrakerAPIStrategy: Resolved Moonraker total_jobs=%d\n", totalJobs);
    }
    return totalJobs;
}

float MoonrakerAPIStrategy::fetchMetadataFilamentWeight(const char* filename) {
    if (filename == nullptr || filename[0] == '\0') return 0.0f;
    auto& config = ConfigurationManager::getInstance();

    char encodedFile[192];
    urlEncodeFilename(filename, encodedFile, sizeof(encodedFile));

    char path[224];
    snprintf(path, sizeof(path), "/server/files/metadata?filename=%s", encodedFile);

    char url[URL_BUFFER_SIZE];
    if (!buildMoonrakerUrl(url, sizeof(url), config.getPrusaLinkURL(), path)) {
        return 0.0f;
    }

    WiFiClient client;
    HTTPClient http;
    http.useHTTP10(true);
    http.setReuse(false);
    http.setTimeout(5000);
    http.begin(client, url);
    if (strlen(config.getPrusaLinkAPIKey()) > 0) {
        http.addHeader("X-Api-Key", config.getPrusaLinkAPIKey());
    }

    int code = http.GET();
    if (code != 200) {
        Serial.printf("MoonrakerAPIStrategy: metadata GET failed (%d) for %s\n", code, filename);
        http.end();
        return 0.0f;
    }

    String resp = http.getString();
    http.end();

    const_buffer_stream stm(reinterpret_cast<const uint8_t*>(resp.c_str()), resp.length());
    float weightG = parseMetadataFilamentWeight(stm);
    if (weightG > 0.0f) {
        Serial.printf("MoonrakerAPIStrategy: Parsed metadata filament_weight_total=%.2fg for %s\n",
                      weightG, filename);
    }
    return weightG;
}

void MoonrakerAPIStrategy::update() {
    auto& config = ConfigurationManager::getInstance();

    connected_ = false;
    hasJob_ = false;
    jobId_ = -1;
    progress_ = 0.0f;
    totalFilamentG_ = 0.0f;
    usedFilamentG_ = 0.0f;
    jobState_[0] = '\0';

    if (strlen(config.getPrusaLinkURL()) == 0) {
        return;
    }

    bool mutexHeld = false;
    if (httpMutex_ != nullptr) {
        if (xSemaphoreTake(httpMutex_, pdMS_TO_TICKS(10000)) != pdTRUE) {
            Serial.println("MoonrakerAPIStrategy: Could not acquire HTTP mutex");
            return;
        }
        mutexHeld = true;
    }

    do {
        char queryUrl[URL_BUFFER_SIZE];
        if (!buildMoonrakerUrl(queryUrl, sizeof(queryUrl), config.getPrusaLinkURL(),
                               "/printer/objects/query?print_stats&virtual_sdcard")) {
            Serial.println("MoonrakerAPIStrategy: query URL too long");
            break;
        }

        WiFiClient client;
        HTTPClient http;
        http.useHTTP10(true);
        http.setReuse(false);
        http.setTimeout(5000);
        http.begin(client, queryUrl);
        if (strlen(config.getPrusaLinkAPIKey()) > 0) {
            http.addHeader("X-Api-Key", config.getPrusaLinkAPIKey());
        }

        int statusCode = http.GET();
        if (statusCode != 200) {
            Serial.printf("MoonrakerAPIStrategy: Query request failed: %d\n", statusCode);
            http.end();
            break;
        }

        String response = http.getString();
        http.end();

        const_buffer_stream stm(reinterpret_cast<const uint8_t*>(response.c_str()), response.length());
        MoonrakerQueryResult query;
        parseQueryPayload(stm, query);

        if (!query.valid) {
            Serial.println("MoonrakerAPIStrategy: Invalid query response");
            break;
        }

        connected_ = true;
        if (!query.hasActiveJob) {
            cachedJobFilename_[0] = '\0';
            cachedHistoryJobId_ = -1;
            break;
        }

        hasJob_ = true;

        // Resolve human-readable job number from Moonraker history totals (e.g. 644), fallback to hash
        if (strcmp(query.filename, cachedJobFilename_) != 0 || cachedHistoryJobId_ <= 0) {
            strncpy(cachedJobFilename_, query.filename, sizeof(cachedJobFilename_) - 1);
            cachedJobFilename_[sizeof(cachedJobFilename_) - 1] = '\0';
            int histId = fetchHistoryJobNumber();
            cachedHistoryJobId_ = (histId > 0) ? histId : computeJobId(query.filename);
        }
        jobId_ = cachedHistoryJobId_;

        progress_ = query.progressPercent;
        strncpy(jobState_, query.mappedState, sizeof(jobState_) - 1);
        jobState_[sizeof(jobState_) - 1] = '\0';

        // Reset cache when print finishes/stops so a reprint of the same file gets the new job number
        if (strcmp(jobState_, "FINISHED") == 0 || strcmp(jobState_, "STOPPED") == 0 || strcmp(jobState_, "ERROR") == 0) {
            cachedJobFilename_[0] = '\0';
        }

        strncpy(savedFilename_, query.filename, sizeof(savedFilename_) - 1);
        savedFilename_[sizeof(savedFilename_) - 1] = '\0';
        savedFilenameJobId_ = jobId_;

        // Priority 1: Convert actual extruded filament (mm) to grams
        if (query.filamentUsedMm > 0.0f) {
            float density = 1.24f;
            float diameter = 1.75f;
            resolveActiveSpoolDensityAndDiameter(density, diameter);
            usedFilamentG_ = mmToGrams(query.filamentUsedMm, diameter, density);
        }

        // Priority 2: Fetch file metadata once per job for total estimated filament
        if (jobId_ == metadataJobId_ && metadataFilamentG_ > 0.0f) {
            totalFilamentG_ = metadataFilamentG_;
        } else if (metadataJobId_ != jobId_) {
            metadataJobId_ = jobId_;
            metadataFilamentG_ = fetchMetadataFilamentWeight(query.filename);
            totalFilamentG_ = metadataFilamentG_;
        }

        // If metadata didn't have weight, expose usedFilamentG_ as total so PrinterManager logs > 0g
        if (totalFilamentG_ <= 0.0f && usedFilamentG_ > 0.0f) {
            totalFilamentG_ = usedFilamentG_;
        }
    } while (false);

    if (mutexHeld) {
        xSemaphoreGive(httpMutex_);
    }
}

float MoonrakerAPIStrategy::fetchDeferredFilament(int expectedJobId) {
    if (expectedJobId <= 0 || expectedJobId != savedFilenameJobId_ || savedFilename_[0] == '\0') {
        return 0.0f;
    }

    bool mutexHeld = false;
    if (httpMutex_ != nullptr) {
        if (xSemaphoreTake(httpMutex_, pdMS_TO_TICKS(10000)) != pdTRUE) {
            return 0.0f;
        }
        mutexHeld = true;
    }

    float weightG = fetchMetadataFilamentWeight(savedFilename_);

    if (mutexHeld) {
        xSemaphoreGive(httpMutex_);
    }
    return weightG;
}

#else

void MoonrakerAPIStrategy::update() {}
float MoonrakerAPIStrategy::fetchDeferredFilament(int) { return 0.0f; }

#endif
