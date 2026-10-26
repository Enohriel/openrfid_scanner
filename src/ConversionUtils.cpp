#include "ConversionUtils.h"
#include "NFCTypes.h"
#include <cstring>
#include <cstdio>

#include <cctype>

static bool containsIgnoreCase(const char* haystack, const char* needle) {
    if (!haystack || !needle) return false;
    size_t nlen = strlen(needle);
    if (nlen == 0) return true;
    for (size_t i = 0; haystack[i] != '\0'; i++) {
        size_t j = 0;
        while (needle[j] != '\0' && haystack[i + j] != '\0' &&
               tolower((unsigned char)haystack[i + j]) == tolower((unsigned char)needle[j])) {
            j++;
        }
        if (j == nlen) return true;
    }
    return false;
}

uint8_t materialTypeFromString(const char* type) {
    if (!type) return OPT_MATERIAL_TYPE_PLA;
    if (containsIgnoreCase(type, "PETG")) return OPT_MATERIAL_TYPE_PETG;
    if (containsIgnoreCase(type, "PLA")) return OPT_MATERIAL_TYPE_PLA;
    if (containsIgnoreCase(type, "ABS")) return OPT_MATERIAL_TYPE_ABS;
    if (containsIgnoreCase(type, "ASA")) return OPT_MATERIAL_TYPE_ASA;
    if (containsIgnoreCase(type, "TPU")) return OPT_MATERIAL_TYPE_TPU;
    if (containsIgnoreCase(type, "PC")) return OPT_MATERIAL_TYPE_PC;
    if (containsIgnoreCase(type, "Nylon") || containsIgnoreCase(type, "PA")) return OPT_MATERIAL_TYPE_PA6;
    if (containsIgnoreCase(type, "PVA")) return OPT_MATERIAL_TYPE_PVA;
    if (containsIgnoreCase(type, "HIPS")) return OPT_MATERIAL_TYPE_HIPS;
    return OPT_MATERIAL_TYPE_PLA; // default
}

const char* materialTypeToString(uint8_t type) {
    switch (type) {
        case OPT_MATERIAL_TYPE_PLA: return "PLA";
        case OPT_MATERIAL_TYPE_PETG: return "PETG";
        case OPT_MATERIAL_TYPE_ABS: return "ABS";
        case OPT_MATERIAL_TYPE_ASA: return "ASA";
        case OPT_MATERIAL_TYPE_TPU: return "TPU";
        case OPT_MATERIAL_TYPE_PC: return "PC";
        case OPT_MATERIAL_TYPE_PA6: return "Nylon";
        case OPT_MATERIAL_TYPE_PVA: return "PVA";
        case OPT_MATERIAL_TYPE_HIPS: return "HIPS";
        default: return "PLA";
    }
}

bool parseHexColor(const char* hex, uint8_t* rgba) {
    if (hex[0] != '#' || strlen(hex) != 7) return false;
    unsigned int r, g, b;
    if (sscanf(hex + 1, "%02x%02x%02x", &r, &g, &b) != 3) return false;
    rgba[0] = r;
    rgba[1] = g;
    rgba[2] = b;
    rgba[3] = 255;
    return true;
}

float getDefaultDensity(uint8_t material_type) {
    switch (material_type) {
        case OPT_MATERIAL_TYPE_PLA:
            return 1.24f;
        case OPT_MATERIAL_TYPE_PETG:
            return 1.27f;
        case OPT_MATERIAL_TYPE_ABS:
            return 1.04f;
        case OPT_MATERIAL_TYPE_ASA:
            return 1.05f;
        case OPT_MATERIAL_TYPE_TPU:
            return 1.21f;
        case OPT_MATERIAL_TYPE_PC:
            return 1.20f;
        case OPT_MATERIAL_TYPE_PA6:  // Nylon
            return 1.14f;
        case OPT_MATERIAL_TYPE_PVA:
            return 1.23f;
        case OPT_MATERIAL_TYPE_HIPS:
            return 1.04f;
        default:
            return 1.20f;  // Generic default
    }
}
