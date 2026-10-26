#ifndef NFC_TYPES_H
#define NFC_TYPES_H

#include <cstdint>
#include <ctime>
#include <cstring>
#include <cstdio>
#include "NFCWriteTypes.h"
#include "ConversionUtils.h"

// Standard Material Types (compatible with legacy enums)
#define OPT_MATERIAL_TYPE_PLA    0
#define OPT_MATERIAL_TYPE_PETG   1
#define OPT_MATERIAL_TYPE_ABS    2
#define OPT_MATERIAL_TYPE_ASA    3
#define OPT_MATERIAL_TYPE_TPU    4
#define OPT_MATERIAL_TYPE_PC     5
#define OPT_MATERIAL_TYPE_PA6    6
#define OPT_MATERIAL_TYPE_PVA    7
#define OPT_MATERIAL_TYPE_HIPS   8

#define OPT_OK                   0
#define OPT_ERR_NFC_READ         1
#define OPT_ERR_NFC_WRITE        2
#define OPT_ERR_INVALID_PARAM    3
#define OPT_ERR_REGION_OVERFLOW  4

typedef uint8_t opt_material_type_t;
typedef int opt_error_t;

// OpenSpool / OpenRFID Data Structure (NTAG215)
struct OpenSpoolData {
    char protocol[16];          // "openspool"
    char version[8];            // "1.0"
    char type[16];              // "PLA", "PETG", "ABS", etc.
    uint8_t material_type;      // Mapped material enum
    char color_hex[10];         // "#FFAABB"
    uint8_t color_rgba[4];      // [R, G, B, 255]
    char brand[33];             // "Prusament", "Sunlu", etc.
    int32_t spoolman_id;        // Spoolman spool ID (e.g. 14)
    int16_t min_temp;           // Min nozzle temp (e.g. 205)
    int16_t max_temp;           // Max nozzle temp (e.g. 225)
    int16_t bed_temp;           // Bed temp (e.g. 60)
    float total_weight;         // Initial spool weight in grams (e.g. 1000.0)
    float remaining_weight;     // Current remaining weight in grams (e.g. 850.0)
    float density;              // Filament density in g/cm³ (e.g. 1.24)
    float diameter;             // Filament diameter in mm (default 1.75)
};

typedef OpenSpoolData opt_tag_t;

// Current detected spool state
struct CurrentSpoolState {
    bool present;
    bool blank_tag_present;
    char spool_id[17];          // Hex string of 7-byte UID (14 chars + null)
    uint8_t uid[7];             // 7-byte UID for NTAG215
    uint8_t uid_length;
    OpenSpoolData tag_data;     // Cached OpenSpool filament data
    bool tag_data_valid;
};

// Recent spool entry for history tracking (RAM only)
struct RecentSpoolEntry {
    char spool_id[17];
    uint8_t material_type;
    uint8_t color[4];           // RGBA
    char manufacturer[33];
    int grams_remaining;
    time_t last_seen;           // Unix timestamp (seconds)
    bool valid;
    bool synced_to_spoolman;
    int32_t spoolman_id;
};

inline const char* opt_error_str(int err) {
    switch (err) {
        case OPT_OK: return "OK";
        case OPT_ERR_NFC_READ: return "NFC Read Error";
        case OPT_ERR_NFC_WRITE: return "NFC Write Error";
        case OPT_ERR_INVALID_PARAM: return "Invalid Parameter";
        case OPT_ERR_REGION_OVERFLOW: return "Region Overflow";
        default: return "Unknown Error";
    }
}

inline const char* opt_material_type_str(opt_material_type_t type) {
    return materialTypeToString((uint8_t)type);
}

inline void opt_init(OpenSpoolData* data) {
    if (!data) return;
    memset(data, 0, sizeof(OpenSpoolData));
    strncpy(data->protocol, "openspool", sizeof(data->protocol) - 1);
    strncpy(data->version, "1.0", sizeof(data->version) - 1);
    strncpy(data->type, "PLA", sizeof(data->type) - 1);
    data->material_type = OPT_MATERIAL_TYPE_PLA;
    strncpy(data->color_hex, "#FFFFFF", sizeof(data->color_hex) - 1);
    data->color_rgba[0] = 255;
    data->color_rgba[1] = 255;
    data->color_rgba[2] = 255;
    data->color_rgba[3] = 255;
    data->spoolman_id = -1;
    data->min_temp = 200;
    data->max_temp = 220;
    data->bed_temp = 60;
    data->total_weight = 1000.0f;
    data->remaining_weight = 1000.0f;
    data->density = 1.24f;
    data->diameter = 1.75f;
}

inline uint8_t opt_format_empty_tag(OpenSpoolData* data, int = 0, int = 0) {
    opt_init(data);
    return OPT_OK;
}

// Getters
inline uint8_t opt_get_material_type(const OpenSpoolData* data, uint8_t* out) {
    if (!data || !out) return 1;
    *out = data->material_type;
    return OPT_OK;
}

inline uint8_t opt_get_actual_full_weight(const OpenSpoolData* data, float* out) {
    if (!data || !out) return 1;
    *out = data->total_weight;
    return OPT_OK;
}

inline uint8_t opt_get_consumed_weight(const OpenSpoolData* data, float* out) {
    if (!data || !out) return 1;
    *out = (data->total_weight > data->remaining_weight) ? (data->total_weight - data->remaining_weight) : 0.0f;
    return OPT_OK;
}

inline uint8_t opt_get_remaining_weight(const OpenSpoolData* data, float* out) {
    if (!data || !out) return 1;
    *out = data->remaining_weight;
    return OPT_OK;
}

inline uint8_t opt_get_primary_color(const OpenSpoolData* data, uint8_t* out4) {
    if (!data || !out4) return 1;
    memcpy(out4, data->color_rgba, 4);
    return OPT_OK;
}

inline uint8_t opt_get_brand_name(const OpenSpoolData* data, char* out, size_t maxLen) {
    if (!data || !out || maxLen == 0) return 1;
    strncpy(out, data->brand, maxLen - 1);
    out[maxLen - 1] = '\0';
    return OPT_OK;
}

inline uint8_t opt_get_gp_spoolman_id(const OpenSpoolData* data, int32_t* out) {
    if (!data || !out) return 1;
    *out = data->spoolman_id;
    return (data->spoolman_id > 0) ? OPT_OK : 1;
}

inline uint8_t opt_get_density(const OpenSpoolData* data, float* out) {
    if (!data || !out) return 1;
    *out = (data->density > 0.0f) ? data->density : 1.24f;
    return OPT_OK;
}

inline uint8_t opt_get_filament_diameter(const OpenSpoolData* data, float* out) {
    if (!data || !out) return 1;
    *out = (data->diameter > 0.0f) ? data->diameter : 1.75f;
    return OPT_OK;
}

inline uint8_t opt_get_material_name(const OpenSpoolData* data, char* out, size_t maxLen) {
    if (!data || !out || maxLen == 0) return 1;
    strncpy(out, data->type, maxLen - 1);
    out[maxLen - 1] = '\0';
    return OPT_OK;
}

// Setters
inline uint8_t opt_set_material_type(OpenSpoolData* data, uint8_t mat) {
    if (!data) return 1;
    data->material_type = mat;
    const char* str = materialTypeToString(mat);
    if (str) {
        strncpy(data->type, str, sizeof(data->type) - 1);
        data->type[sizeof(data->type) - 1] = '\0';
    }
    return OPT_OK;
}

inline uint8_t opt_set_material_name(OpenSpoolData* data, const char* name) {
    if (!data || !name) return 1;
    strncpy(data->type, name, sizeof(data->type) - 1);
    data->type[sizeof(data->type) - 1] = '\0';
    data->material_type = materialTypeFromString(name);
    return OPT_OK;
}

inline uint8_t opt_set_primary_color(OpenSpoolData* data, const uint8_t* rgba) {
    if (!data || !rgba) return 1;
    memcpy(data->color_rgba, rgba, 4);
    snprintf(data->color_hex, sizeof(data->color_hex), "#%02X%02X%02X", rgba[0], rgba[1], rgba[2]);
    return OPT_OK;
}

inline uint8_t opt_set_brand_name(OpenSpoolData* data, const char* brand) {
    if (!data || !brand) return 1;
    strncpy(data->brand, brand, sizeof(data->brand) - 1);
    data->brand[sizeof(data->brand) - 1] = '\0';
    return OPT_OK;
}

inline uint8_t opt_set_actual_full_weight(OpenSpoolData* data, float weight) {
    if (!data) return 1;
    data->total_weight = weight;
    return OPT_OK;
}

inline uint8_t opt_set_consumed_weight(OpenSpoolData* data, float consumed) {
    if (!data) return 1;
    data->remaining_weight = (data->total_weight > consumed) ? (data->total_weight - consumed) : 0.0f;
    return OPT_OK;
}

inline uint8_t opt_add_consumed_weight(OpenSpoolData* data, float grams_to_remove) {
    if (!data) return 1;
    if (data->remaining_weight >= grams_to_remove) {
        data->remaining_weight -= grams_to_remove;
    } else {
        data->remaining_weight = 0.0f;
    }
    return OPT_OK;
}

inline uint8_t opt_set_density(OpenSpoolData* data, float density) {
    if (!data) return 1;
    data->density = density;
    return OPT_OK;
}

inline uint8_t opt_set_filament_diameter(OpenSpoolData* data, float diameter) {
    if (!data) return 1;
    data->diameter = diameter;
    return OPT_OK;
}

inline uint8_t opt_set_gp_spoolman_id(OpenSpoolData* data, int32_t smId) {
    if (!data) return 1;
    data->spoolman_id = smId;
    return OPT_OK;
}

#endif // NFC_TYPES_H
