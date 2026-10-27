#include "BgcodeParser.h"
#include <cstring>
#include <cstdlib>
#include <cctype>

#ifndef NATIVE_TEST
  #include <Arduino.h>
#else
  #include "platform/NativePlatform.h"
#endif

static const char* findCaseInsensitive(const char* haystack, size_t haystackLen, const char* needle) {
    if (haystack == nullptr || needle == nullptr) return nullptr;
    size_t needleLen = strlen(needle);
    if (needleLen == 0 || needleLen > haystackLen) return nullptr;

    const char* end = haystack + haystackLen - needleLen;
    for (const char* p = haystack; p <= end; p++) {
        size_t i = 0;
        for (; i < needleLen; i++) {
            char c1 = static_cast<char>(tolower(static_cast<unsigned char>(p[i])));
            char c2 = static_cast<char>(tolower(static_cast<unsigned char>(needle[i])));
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
    // Require ':' or '=' delimiter so we don't match partial words like "total filament used for wipe tower"
    if (p >= end || (*p != '=' && *p != ':')) {
        return 0.0f;
    }
    p++;
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
            p++; // skip comma for multi-extruder lists
            while (p < end && (*p == ' ' || *p == '\t')) p++;
        } else {
            break;
        }
    }
    return total;
}

// Search a metadata block's key=value pairs for "total filament used [g]=" or "filament used [g]="
static float searchMetadataForFilament(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0) return 0.0f;
    const char* meta = reinterpret_cast<const char*>(data);
    const char* end = meta + len;

    static const char* const needles[] = {
        "total filament used [g]",
        "filament used [g]"
    };

    for (const char* needle : needles) {
        size_t needleLen = strlen(needle);
        if (len < needleLen) continue;
        const char* pos = meta;
        while (pos <= end - needleLen) {
            const char* match = findCaseInsensitive(pos, static_cast<size_t>(end - pos), needle);
            if (match == nullptr) break;
            // Ensure match is at start of buffer or preceded by newline
            if (match == meta || *(match - 1) == '\n' || *(match - 1) == '\r') {
                float val = parseFilamentFromMatch(match, needleLen, end);
                if (val > 0.0f) {
                    return val;
                }
            }
            pos = match + needleLen;
        }
    }
    return 0.0f;
}

float parseGcodeCommentLine(const char* line, size_t len, int& outPriority) {
    outPriority = 0;
    if (line == nullptr || len == 0) return 0.0f;
    const char* end = line + len;

    // Priority 3: Explicit total filament weight in grams
    static const char* const totalGramKeys[] = {
        "total filament used [g]",
        "total filament weight [g]",
        "total_filament_used_g"
    };
    for (const char* key : totalGramKeys) {
        size_t keyLen = strlen(key);
        const char* match = findCaseInsensitive(line, len, key);
        if (match != nullptr) {
            float val = parseFilamentFromMatch(match, keyLen, end);
            if (val > 0.0f) {
                outPriority = 3;
                return val;
            }
        }
    }

    // Priority 2: Explicit filament weight in grams
    static const char* const gramKeys[] = {
        "filament used [g]",
        "filament weight [g]",
        "filament_used_g",
        "total filament used",
        "filament weight"
    };
    for (const char* key : gramKeys) {
        size_t keyLen = strlen(key);
        const char* match = findCaseInsensitive(line, len, key);
        if (match != nullptr) {
            float val = parseFilamentFromMatch(match, keyLen, end);
            if (val > 0.0f) {
                outPriority = 2;
                return val;
            }
        }
    }

    // Priority 1: Fallback from volume [cm3] or length [mm]
    static const char* const volKeys[] = {
        "total filament used [cm3]",
        "filament used [cm3]"
    };
    for (const char* key : volKeys) {
        size_t keyLen = strlen(key);
        const char* match = findCaseInsensitive(line, len, key);
        if (match != nullptr) {
            float val = parseFilamentFromMatch(match, keyLen, end);
            if (val > 0.0f) {
                outPriority = 1;
                return val * 1.24f;
            }
        }
    }

    static const char* const lenKeys[] = {
        "total filament used [mm]",
        "filament used [mm]"
    };
    for (const char* key : lenKeys) {
        size_t keyLen = strlen(key);
        const char* match = findCaseInsensitive(line, len, key);
        if (match != nullptr) {
            float val = parseFilamentFromMatch(match, keyLen, end);
            if (val > 0.0f) {
                outPriority = 1;
                return val * 0.00298f;
            }
        }
    }

    return 0.0f;
}

bool isGcodeConfigBlockStart(const char* line) {
    if (line == nullptr) return false;
    return (strstr(line, "CONFIG_BLOCK_START") != nullptr ||
            strstr(line, "prusaslicer_config = begin") != nullptr ||
            strstr(line, "bambuslicer_config = begin") != nullptr);
}

static void processCompletedGcodeLine(GcodeStreamState& st) {
    st.lineBuf[st.lineLen] = '\0';
    st.linesRead++;

    if (strstr(st.lineBuf, "thumbnail begin") != nullptr ||
        strstr(st.lineBuf, "THUMBNAIL_BLOCK_START") != nullptr) {
        st.inThumbnail = true;
        return;
    }
    if (strstr(st.lineBuf, "thumbnail end") != nullptr ||
        strstr(st.lineBuf, "THUMBNAIL_BLOCK_END") != nullptr) {
        st.inThumbnail = false;
        return;
    }

    if (st.inThumbnail) {
        return;
    }

    const char* p = st.lineBuf;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != ';') {
        return;
    }

    if (isGcodeConfigBlockStart(p)) {
        st.done = true;
        return;
    }

    int prio = 0;
    float val = parseGcodeCommentLine(p, strlen(p), prio);
    if (val > 0.0f && prio >= st.bestPriority) {
        st.bestFilamentG = val;
        st.bestPriority = prio;
        if (prio == 3) {
            st.done = true;
        }
    }
}

float feedGcodeStreamChunk(GcodeStreamState& st, const uint8_t* chunk, size_t chunkLen) {
    if (st.done || chunk == nullptr || chunkLen == 0) {
        return st.bestFilamentG;
    }

    for (size_t i = 0; i < chunkLen && !st.done; i++) {
        char c = static_cast<char>(chunk[i]);
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            processCompletedGcodeLine(st);
            st.lineLen = 0;
            st.lineBuf[0] = '\0';
        } else if (st.lineLen < sizeof(st.lineBuf) - 1) {
            st.lineBuf[st.lineLen++] = c;
        }
    }

    return st.bestFilamentG;
}

float finalizeGcodeStream(GcodeStreamState& st) {
    if (!st.done && st.lineLen > 0) {
        processCompletedGcodeLine(st);
        st.lineLen = 0;
        st.lineBuf[0] = '\0';
    }
    st.done = true;
    return st.bestFilamentG;
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

        while (offset + 10 <= len) {
            uint16_t blockType = 0, compression = 0;
            uint32_t uncompressedSize = 0, compressedSize = 0;

            memcpy(&blockType, data + offset, sizeof(uint16_t));
            memcpy(&compression, data + offset + 2, sizeof(uint16_t));
            memcpy(&uncompressedSize, data + offset + 4, sizeof(uint32_t));

            size_t headerSize = 0;
            if (compression == 0) {
                compressedSize = uncompressedSize;
                // Uncompressed libbgcode header: 8B base + 6B (thumbnail) or 2B (encoding)
                headerSize = (blockType == 5) ? 14 : 10;
            } else {
                if (offset + 12 > len) break;
                memcpy(&compressedSize, data + offset + 8, sizeof(uint32_t));
                headerSize = (blockType == 5) ? 18 : 14;
            }

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

            if (dataEnd > len) {
                break;
            }

            offset = dataEnd + checksumSize;
        }

        // Fallback: direct scan across the binary header buffer for uncompressed ASCII metadata
        return searchMetadataForFilament(data + 10, len - 10);
    }

    // 2. ASCII text G-code (OrcaSlicer, PrusaSlicer, Bambu Studio, Cura, etc.)
    GcodeStreamState st;
    feedGcodeStreamChunk(st, data, len);
    return finalizeGcodeStream(st);
}
