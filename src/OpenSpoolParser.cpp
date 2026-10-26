#include "OpenSpoolParser.h"
#include "ConversionUtils.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>

#ifndef NATIVE_TEST
#include <Arduino.h>
#endif

// Helper to find a JSON key value in a simple string buffer without heavy dependencies
static const char* findJsonKey(const char* json, const char* key, size_t jsonLen) {
    if (!json || !key || jsonLen == 0) return nullptr;

    char searchPattern[48];
    snprintf(searchPattern, sizeof(searchPattern), "\"%s\"", key);
    size_t patternLen = strlen(searchPattern);

    for (size_t i = 0; i + patternLen < jsonLen; i++) {
        if (strncmp(json + i, searchPattern, patternLen) == 0) {
            const char* p = json + i + patternLen;
            while (*p == ' ' || *p == ':' || *p == '\t') {
                p++;
            }
            return p;
        }
    }
    return nullptr;
}

static bool extractStringValue(const char* valStart, char* out, size_t maxLen) {
    if (!valStart || !out || maxLen == 0) return false;
    while (*valStart == ' ' || *valStart == '\"') valStart++;
    size_t idx = 0;
    while (*valStart && *valStart != '\"' && *valStart != ',' && *valStart != '}' && idx + 1 < maxLen) {
        out[idx++] = *valStart++;
    }
    out[idx] = '\0';
    return idx > 0;
}

static double extractNumberValue(const char* valStart) {
    if (!valStart) return 0.0;
    while (*valStart == ' ' || *valStart == '\"' || *valStart == ':') valStart++;
    char* endPtr = nullptr;
    return strtod(valStart, &endPtr);
}

bool OpenSpoolParser::parseJsonPayload(const char* jsonStr, size_t len, OpenSpoolData* outData) {
    if (!jsonStr || len == 0 || !outData) return false;

    // Find opening '{' and closing '}'
    const char* start = (const char*)memchr(jsonStr, '{', len);
    if (!start) return false;

    size_t remaining = len - (start - jsonStr);
    const char* end = nullptr;
    int braceCount = 0;
    for (size_t i = 0; i < remaining; i++) {
        if (start[i] == '{') braceCount++;
        else if (start[i] == '}') {
            braceCount--;
            if (braceCount == 0) {
                end = start + i;
                break;
            }
        }
    }
    if (!end) return false;
    size_t jsonLen = (end - start) + 1;

    memset(outData, 0, sizeof(OpenSpoolData));
    outData->diameter = 1.75f;
    outData->density = 1.24f;

    // Protocol & Version
    const char* pProto = findJsonKey(start, "protocol", jsonLen);
    if (pProto) extractStringValue(pProto, outData->protocol, sizeof(outData->protocol));
    else strcpy(outData->protocol, "openspool");

    const char* pVer = findJsonKey(start, "version", jsonLen);
    if (pVer) extractStringValue(pVer, outData->version, sizeof(outData->version));
    else strcpy(outData->version, "1.0");

    // Type / Material
    const char* pType = findJsonKey(start, "type", jsonLen);
    if (!pType) pType = findJsonKey(start, "material", jsonLen);
    if (pType) {
        extractStringValue(pType, outData->type, sizeof(outData->type));
        outData->material_type = materialTypeFromString(outData->type);
        outData->density = getDefaultDensity(outData->material_type);
    } else {
        strcpy(outData->type, "PLA");
        outData->material_type = OPT_MATERIAL_TYPE_PLA;
        outData->density = 1.24f;
    }

    // Color Hex
    const char* pColor = findJsonKey(start, "color_hex", jsonLen);
    if (!pColor) pColor = findJsonKey(start, "color", jsonLen);
    if (pColor) {
        char tempColor[16];
        if (extractStringValue(pColor, tempColor, sizeof(tempColor))) {
            if (tempColor[0] != '#') {
                snprintf(outData->color_hex, sizeof(outData->color_hex), "#%s", tempColor);
            } else {
                strncpy(outData->color_hex, tempColor, sizeof(outData->color_hex) - 1);
            }
            parseHexColor(outData->color_hex, outData->color_rgba);
        }
    } else {
        strcpy(outData->color_hex, "#FFFFFF");
        outData->color_rgba[0] = 255;
        outData->color_rgba[1] = 255;
        outData->color_rgba[2] = 255;
        outData->color_rgba[3] = 255;
    }

    // Brand / Manufacturer
    const char* pBrand = findJsonKey(start, "brand", jsonLen);
    if (!pBrand) pBrand = findJsonKey(start, "manufacturer", jsonLen);
    if (pBrand) {
        extractStringValue(pBrand, outData->brand, sizeof(outData->brand));
    } else {
        strcpy(outData->brand, "Generic");
    }

    // Spoolman ID
    const char* pSpoolId = findJsonKey(start, "spool_id", jsonLen);
    if (!pSpoolId) pSpoolId = findJsonKey(start, "spoolman_id", jsonLen);
    if (pSpoolId) {
        outData->spoolman_id = (int32_t)extractNumberValue(pSpoolId);
    }

    // Temps
    const char* pMinTemp = findJsonKey(start, "min_temp", jsonLen);
    if (pMinTemp) outData->min_temp = (int16_t)extractNumberValue(pMinTemp);
    const char* pMaxTemp = findJsonKey(start, "max_temp", jsonLen);
    if (pMaxTemp) outData->max_temp = (int16_t)extractNumberValue(pMaxTemp);
    const char* pBedTemp = findJsonKey(start, "bed_temp", jsonLen);
    if (pBedTemp) outData->bed_temp = (int16_t)extractNumberValue(pBedTemp);

    // Weights
    const char* pTotWeight = findJsonKey(start, "total_weight", jsonLen);
    if (pTotWeight) outData->total_weight = (float)extractNumberValue(pTotWeight);
    else outData->total_weight = 1000.0f;

    const char* pRemWeight = findJsonKey(start, "remaining_weight", jsonLen);
    if (pRemWeight) outData->remaining_weight = (float)extractNumberValue(pRemWeight);
    else outData->remaining_weight = outData->total_weight;

    return true;
}

size_t OpenSpoolParser::serializeJsonPayload(const OpenSpoolData* inData, char* outBuffer, size_t maxLen) {
    if (!inData || !outBuffer || maxLen == 0) return 0;

    int written = snprintf(outBuffer, maxLen,
        "{\"protocol\":\"%s\",\"version\":\"%s\",\"type\":\"%s\",\"color_hex\":\"%s\","
        "\"brand\":\"%s\",\"spool_id\":%ld,\"min_temp\":%d,\"max_temp\":%d,"
        "\"total_weight\":%.1f,\"remaining_weight\":%.1f}",
        inData->protocol[0] ? inData->protocol : "openspool",
        inData->version[0] ? inData->version : "1.0",
        inData->type[0] ? inData->type : "PLA",
        inData->color_hex[0] ? inData->color_hex : "#FFFFFF",
        inData->brand[0] ? inData->brand : "Generic",
        (long)inData->spoolman_id,
        (int)inData->min_temp,
        (int)inData->max_temp,
        inData->total_weight > 0.0f ? inData->total_weight : 1000.0f,
        inData->remaining_weight >= 0.0f ? inData->remaining_weight : 1000.0f);

    return (written > 0 && (size_t)written < maxLen) ? (size_t)written : 0;
}

bool OpenSpoolParser::readFromTag(NFCConnectionI* conn, OpenSpoolData* outData) {
    if (!conn || !outData) return false;

    // Buffer for NTAG215 user memory: pages 4 to 129 (126 pages * 4 = 504 bytes)
    uint8_t userMemory[504];
    memset(userMemory, 0, sizeof(userMemory));

    // Read pages 4 to 80 first (300+ bytes is more than enough for OpenSpool JSON)
    for (uint8_t page = 4; page < 80; page++) {
        if (!conn->readPage(page, &userMemory[(page - 4) * 4])) {
            // If reading failed early
            if (page < 10) return false;
            break;
        }
    }

    return parseJsonPayload((const char*)userMemory, sizeof(userMemory), outData);
}

bool OpenSpoolParser::writeToTag(NFCConnectionI* conn, const OpenSpoolData* inData) {
    if (!conn || !inData) return false;

    char jsonBuf[384];
    size_t jsonLen = serializeJsonPayload(inData, jsonBuf, sizeof(jsonBuf));
    if (jsonLen == 0) return false;

    // Ensure Capability Container (CC) is written at page 3 (NTAG215 standard CC)
    uint8_t ccPage[4] = {0xE1, 0x10, 0x6D, 0x00};
    conn->writePage(3, ccPage);

    // Construct NDEF MIME message:
    // [0x03] [TLV_LEN] [RECORD_HEADER] [TYPE_LEN] [PAYLOAD_LEN] ["application/json"] [jsonBuf...] [0xFE]
    static const char mimeType[] = "application/json";
    size_t mimeLen = strlen(mimeType);

    uint8_t ndefPayload[504];
    memset(ndefPayload, 0, sizeof(ndefPayload));

    bool shortRecord = (jsonLen <= 255);
    size_t recordLen = (shortRecord ? 3 : 6) + mimeLen + jsonLen;
    if (recordLen > 480) return false;

    size_t offset = 0;
    ndefPayload[offset++] = 0x03; // NDEF TLV
    if (recordLen < 255) {
        ndefPayload[offset++] = (uint8_t)recordLen;
    } else {
        ndefPayload[offset++] = 0xFF;
        ndefPayload[offset++] = (uint8_t)(recordLen >> 8);
        ndefPayload[offset++] = (uint8_t)(recordLen & 0xFF);
    }

    if (shortRecord) {
        ndefPayload[offset++] = 0xD2; // MB=1, ME=1, SR=1, TNF=0x02 (MIME)
        ndefPayload[offset++] = (uint8_t)mimeLen;
        ndefPayload[offset++] = (uint8_t)jsonLen;
    } else {
        ndefPayload[offset++] = 0xC2; // MB=1, ME=1, SR=0, TNF=0x02 (MIME)
        ndefPayload[offset++] = (uint8_t)mimeLen;
        ndefPayload[offset++] = 0x00;
        ndefPayload[offset++] = 0x00;
        ndefPayload[offset++] = (uint8_t)(jsonLen >> 8);
        ndefPayload[offset++] = (uint8_t)(jsonLen & 0xFF);
    }
    memcpy(&ndefPayload[offset], mimeType, mimeLen);
    offset += mimeLen;
    memcpy(&ndefPayload[offset], jsonBuf, jsonLen);
    offset += jsonLen;
    ndefPayload[offset++] = 0xFE; // Terminator TLV

    // Write to NTAG215 user pages (Page 4 onwards) in 4-byte chunks
    size_t totalPagesToWrite = (offset + 3) / 4;
    for (size_t p = 0; p < totalPagesToWrite; p++) {
        uint8_t pageAddr = 4 + p;
        if (!conn->writePage(pageAddr, &ndefPayload[p * 4])) {
            return false;
        }
    }

    return true;
}
