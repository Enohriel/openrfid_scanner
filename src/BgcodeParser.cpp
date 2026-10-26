#include "BgcodeParser.h"
#include <cstring>
#include <cstdlib>
#include <cctype>

#ifndef NATIVE_TEST
  #include <Arduino.h>
#else
  #include "platform/NativePlatform.h"
#endif

// Search a metadata block's key=value pairs for "filament used [g]=<value>"
static float searchMetadataForFilament(const uint8_t* data, size_t len) {
    const char* meta = (const char*)data;
    const char* end = meta + len;
    const char* needle = "filament used [g]=";
    const size_t needleLen = 18;

    const char* pos = meta;
    while (pos <= end - needleLen) {
        if (memcmp(pos, needle, needleLen) == 0) {
            const char* valStart = pos + needleLen;
            char valBuf[32];
            size_t i = 0;
            while (valStart + i < end && valStart[i] != '\n' && valStart[i] != '\0' && i < sizeof(valBuf) - 1) {
                valBuf[i] = valStart[i];
                i++;
            }
            valBuf[i] = '\0';
            return atof(valBuf);
        }
        while (pos < end && *pos != '\n') pos++;
        if (pos < end) pos++;
    }
    return 0.0f;
}

static const char* findCaseInsensitive(const char* haystack, size_t haystackLen, const char* needle) {
    if (haystack == nullptr || needle == nullptr) return nullptr;
    size_t needleLen = strlen(needle);
    if (needleLen > haystackLen) return nullptr;

    const char* end = haystack + haystackLen - needleLen;
    for (const char* p = haystack; p <= end; p++) {
        size_t i = 0;
        for (; i < needleLen; i++) {
            char c1 = tolower(static_cast<unsigned char>(p[i]));
            char c2 = tolower(static_cast<unsigned char>(needle[i]));
            if (c1 != c2) break;
        }
        if (i == needleLen) {
            return p;
        }
    }
    return nullptr;
}

static float parseFilamentFromMatch(const char* match, size_t matchKeyLen, const char* end) {
    const char* p = match + matchKeyLen;
    // Skip spaces / tabs
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    // Skip ':' or '='
    if (p < end && (*p == '=' || *p == ':')) {
        p++;
    }
    // Skip spaces / tabs
    while (p < end && (*p == ' ' || *p == '\t')) p++;

    float total = 0.0f;
    while (p < end && *p != '\n' && *p != '\r') {
        char* next = nullptr;
        float val = strtof(p, &next);
        if (next == p || next == nullptr) {
            break;
        }
        if (val > 0.0f) {
            total += val;
        }
        p = next;
        // Skip trailing units like 'g' or 'G' or whitespace
        while (p < end && (*p == ' ' || *p == '\t' || *p == 'g' || *p == 'G')) p++;
        if (p < end && *p == ',') {
            p++; // skip comma
            while (p < end && (*p == ' ' || *p == '\t')) p++;
        } else {
            break;
        }
    }
    return total;
}

static float parseTextGcodeFilament(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0) return 0.0f;
    const char* text = reinterpret_cast<const char*>(data);
    const char* end = text + len;

    // Ordered by specificity (prefer total over individual)
    static const char* const keys[] = {
        "total filament used [g]",
        "total filament weight [g]",
        "filament used [g]",
        "filament weight [g]",
        "total_filament_used_g",
        "filament_used_g",
        "total filament used",
        "filament weight",
        "total_filament_used",
        "filament_used"
    };

    for (const char* key : keys) {
        size_t keyLen = strlen(key);
        const char* match = findCaseInsensitive(text, len, key);
        if (match != nullptr) {
            float val = parseFilamentFromMatch(match, keyLen, end);
            if (val > 0.0f) {
                return val;
            }
        }
    }

    // Fallback: volume [cm3] -> weight (approx 1.24 g/cm3)
    static const char* const volKeys[] = {
        "total filament used [cm3]",
        "filament used [cm3]"
    };
    for (const char* key : volKeys) {
        size_t keyLen = strlen(key);
        const char* match = findCaseInsensitive(text, len, key);
        if (match != nullptr) {
            float val = parseFilamentFromMatch(match, keyLen, end);
            if (val > 0.0f) {
                return val * 1.24f;
            }
        }
    }

    // Fallback: length [mm] -> weight (for 1.75mm, ~0.00298 g/mm)
    static const char* const lenKeys[] = {
        "total filament used [mm]",
        "filament used [mm]"
    };
    for (const char* key : lenKeys) {
        size_t keyLen = strlen(key);
        const char* match = findCaseInsensitive(text, len, key);
        if (match != nullptr) {
            float val = parseFilamentFromMatch(match, keyLen, end);
            if (val > 0.0f) {
                return val * 0.00298f;
            }
        }
    }
    return 0.0f;
}

float parseBgcodeFilament(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0) {
        return 0.0f;
    }

    // 1. Binary bgcode: magic("GCDE", 4)
    if (len >= 4 && memcmp(data, "GCDE", 4) == 0) {
        if (len < 22) {
            return 0.0f;
        }

        uint16_t checksumType;
        memcpy(&checksumType, data + 8, sizeof(uint16_t));
        size_t checksumSize = (checksumType == 1) ? 4 : 0;

        size_t offset = 10; // first block header

        while (offset + 12 <= len) {
            uint16_t blockType, compression;
            uint32_t uncompressedSize, compressedSize;

            memcpy(&blockType, data + offset, sizeof(uint16_t));
            memcpy(&compression, data + offset + 2, sizeof(uint16_t));
            memcpy(&uncompressedSize, data + offset + 4, sizeof(uint32_t));
            memcpy(&compressedSize, data + offset + 8, sizeof(uint32_t));

            // GCode blocks (type 1) have an extra raw_data_size uint32
            size_t headerSize = (blockType == 1) ? 16 : 12;
            size_t dataStart = offset + headerSize;
            size_t dataEnd = dataStart + compressedSize;

            // Search uncompressed metadata blocks (types 0,2,3,4) for filament key
            if (blockType != 1 && blockType != 5 && dataStart < len) {
                if (compression != 0) {
#ifndef NATIVE_TEST
                    Serial.printf("BgcodeParser: Skipping compressed metadata block type %u (compression=%u, size=%u)\n",
                             blockType, compression, uncompressedSize);
#endif
                } else {
                    size_t searchLen = (dataEnd <= len) ? compressedSize : (len - dataStart);
                    float result = searchMetadataForFilament(data + dataStart, searchLen);
                    if (result > 0.0f) {
                        return result;
                    }
                }
            }

            // If block data extends beyond our buffer, stop scanning
            if (dataEnd > len) {
                break;
            }

            offset = dataEnd + checksumSize;
        }

        return 0.0f;
    }

    // 2. ASCII text G-code header (OrcaSlicer, PrusaSlicer, Bambu Studio, Cura, etc.)
    return parseTextGcodeFilament(data, len);
}
