#include "platform/NativePlatform.h"
#include "../../src/MoonrakerAPIStrategy.h"

#include <cstdio>
#include <cstring>
#include <cmath>

#define TEST_ASSERT(cond) \
    do { \
        if (!(cond)) { \
            printf("FAIL: %s at %s:%d\n", #cond, __FILE__, __LINE__); \
            return 1; \
        } \
    } while (0)

#define RUN_TEST(test_func) \
    do { \
        printf("Running %s... ", #test_func); \
        int result = test_func(); \
        if (result == 0) { \
            printf("PASS\n"); \
            passed++; \
        } else { \
            failed++; \
        } \
        total++; \
    } while (0)

using namespace io;

int test_url_normalization() {
    char url[256];

    // No port -> default 7125 added
    TEST_ASSERT(MoonrakerAPIStrategy::buildMoonrakerUrl(url, sizeof(url), "http://192.168.1.50", "/server/info"));
    TEST_ASSERT(strcmp(url, "http://192.168.1.50:7125/server/info") == 0);

    // Trailing slash stripped and default 7125 added
    TEST_ASSERT(MoonrakerAPIStrategy::buildMoonrakerUrl(url, sizeof(url), "http://192.168.1.50/", "/printer/objects/query?print_stats"));
    TEST_ASSERT(strcmp(url, "http://192.168.1.50:7125/printer/objects/query?print_stats") == 0);

    // Explicit port preserved (e.g. Mainsail 4409 or Moonraker 7125)
    TEST_ASSERT(MoonrakerAPIStrategy::buildMoonrakerUrl(url, sizeof(url), "http://192.168.1.50:4409/", "/server/info"));
    TEST_ASSERT(strcmp(url, "http://192.168.1.50:4409/server/info") == 0);

    // Bare IP without scheme -> http:// and :7125 added
    TEST_ASSERT(MoonrakerAPIStrategy::buildMoonrakerUrl(url, sizeof(url), "192.168.1.50", "server/spoolman/spool_id"));
    TEST_ASSERT(strcmp(url, "http://192.168.1.50:7125/server/spoolman/spool_id") == 0);

    return 0;
}

int test_parse_printing_query() {
    const char* payload = R"({
        "result": {
            "eventtime": 12345.67,
            "status": {
                "print_stats": {
                    "filename": "CE3V3KE_Benchy.gcode",
                    "total_duration": 1250.5,
                    "print_duration": 1100.2,
                    "filament_used": 1000.0,
                    "state": "printing",
                    "message": ""
                },
                "virtual_sdcard": {
                    "file_path": "/usr/data/printer_data/gcodes/CE3V3KE_Benchy.gcode",
                    "progress": 0.425,
                    "is_active": true,
                    "file_position": 54321
                }
            }
        }
    })";

    const_buffer_stream stm(reinterpret_cast<const uint8_t*>(payload), strlen(payload));
    MoonrakerQueryResult res;
    MoonrakerAPIStrategy::parseQueryPayload(stm, res);

    TEST_ASSERT(res.valid);
    TEST_ASSERT(res.hasActiveJob);
    TEST_ASSERT(strcmp(res.mappedState, "PRINTING") == 0);
    TEST_ASSERT(strcmp(res.filename, "CE3V3KE_Benchy.gcode") == 0);
    TEST_ASSERT(fabsf(res.filamentUsedMm - 1000.0f) < 0.01f);
    TEST_ASSERT(fabsf(res.progressPercent - 42.5f) < 0.05f);

    // 1000 mm (1m) of 1.75mm PLA (1.24 g/cm3) -> ~2.9825g
    float grams = MoonrakerAPIStrategy::mmToGrams(res.filamentUsedMm, 1.75f, 1.24f);
    TEST_ASSERT(grams > 2.97f && grams < 3.00f);

    int jobId = MoonrakerAPIStrategy::computeJobId(res.filename);
    TEST_ASSERT(jobId > 0);
    TEST_ASSERT(jobId != MoonrakerAPIStrategy::computeJobId("Other_File.gcode"));

    return 0;
}

int test_parse_complete_and_standby() {
    const char* completePayload = R"({
        "result": {
            "status": {
                "print_stats": {
                    "filename": "cube.gcode",
                    "filament_used": 3500.0,
                    "state": "complete"
                },
                "virtual_sdcard": {
                    "progress": 1.0
                }
            }
        }
    })";

    {
        const_buffer_stream stm(reinterpret_cast<const uint8_t*>(completePayload), strlen(completePayload));
        MoonrakerQueryResult res;
        MoonrakerAPIStrategy::parseQueryPayload(stm, res);
        TEST_ASSERT(res.valid);
        TEST_ASSERT(res.hasActiveJob);
        TEST_ASSERT(strcmp(res.mappedState, "FINISHED") == 0);
        TEST_ASSERT(fabsf(res.progressPercent - 100.0f) < 0.01f);
        TEST_ASSERT(fabsf(res.filamentUsedMm - 3500.0f) < 0.01f);
    }

    const char* standbyPayload = R"({
        "result": {
            "status": {
                "print_stats": {
                    "filename": "",
                    "filament_used": 0.0,
                    "state": "standby"
                },
                "virtual_sdcard": {
                    "progress": 0.0
                }
            }
        }
    })";

    {
        const_buffer_stream stm(reinterpret_cast<const uint8_t*>(standbyPayload), strlen(standbyPayload));
        MoonrakerQueryResult res;
        MoonrakerAPIStrategy::parseQueryPayload(stm, res);
        TEST_ASSERT(res.valid);
        TEST_ASSERT(!res.hasActiveJob);
        TEST_ASSERT(res.mappedState[0] == '\0');
    }

    return 0;
}

int test_parse_metadata() {
    const char* metaPayload = R"({
        "result": {
            "filename": "CE3V3KE_Benchy.gcode",
            "size": 1452300,
            "slicer": "OrcaSlicer",
            "filament_total": 4120.5,
            "filament_weight_total": 12.45
        }
    })";

    const_buffer_stream stm(reinterpret_cast<const uint8_t*>(metaPayload), strlen(metaPayload));
    float weight = MoonrakerAPIStrategy::parseMetadataFilamentWeight(stm);
    TEST_ASSERT(fabsf(weight - 12.45f) < 0.01f);

    return 0;
}

int main() {
    int passed = 0, failed = 0, total = 0;

    printf("\n=== Moonraker Strategy Tests ===\n\n");
    RUN_TEST(test_url_normalization);
    RUN_TEST(test_parse_printing_query);
    RUN_TEST(test_parse_complete_and_standby);
    RUN_TEST(test_parse_metadata);

    printf("\n=== Results: %d/%d passed ===\n", passed, total);
    return failed > 0 ? 1 : 0;
}

