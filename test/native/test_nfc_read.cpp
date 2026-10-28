// Tests for NFCManager read behavior
// Build with: cd test/native && make test_nfc_read

#include "platform/NativePlatform.h"
#include "StubNFCConnection.h"
#include "FakeLCDManager.h"
#include "StubApplicationManager.h"
#include "NFCTypes.h"
#include "OpenSpoolParser.h"

#include <cmath>
#include <cstdio>
#include <cstring>

// Test assertion helpers (minimal, local to this test)
#define TEST_ASSERT(cond) \
    do { \
        if (!(cond)) { \
            printf("FAIL: %s at %s:%d\n", #cond, __FILE__, __LINE__); \
            return 1; \
        } \
    } while(0)

#define TEST_ASSERT_EQ(a, b) \
    do { \
        if ((a) != (b)) { \
            printf("FAIL: %s != %s at %s:%d\n", #a, #b, __FILE__, __LINE__); \
            return 1; \
        } \
    } while(0)

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
    } while(0)

// Include real NFCManager implementation (unity build)
#include "NFCManager.cpp"

static StubNFCConnection* g_stubNfc = nullptr;
static LCDManager* g_lcd = nullptr;

static bool nearlyEqual(float a, float b, float epsilon = 0.0001f) {
    return std::fabs(a - b) <= epsilon;
}

void setup_nfc_test() {
    g_stubNfc = new StubNFCConnection();
    g_lcd = new LCDManager();

    auto& nfcMgr = NFCManager::getInstance();
    nfcMgr.setConnection(g_stubNfc);
    nfcMgr.begin();

    // Reset application manager message tracking
    ApplicationManager::getInstance().reset();
}

void teardown_nfc_test() {
    if (g_stubNfc) {
        g_stubNfc->resetTestState();
        delete g_stubNfc;
        g_stubNfc = nullptr;
    }
    if (g_lcd) {
        delete g_lcd;
        g_lcd = nullptr;
    }
}

// Test: Blank spool should NOT be automatically formatted
int test_blank_spool_not_formatted() {
    setup_nfc_test();

    // Setup: Tag present with blank data (all zeros - will fail NDEF parse)
    uint8_t testUid[7] = {0x04, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    g_stubNfc->setTagPresent(true);
    g_stubNfc->setTagUid(testUid, 7);

    uint8_t blankData[256] = {0};  // Blank tag - NDEF parse will fail
    g_stubNfc->setTagData(blankData, sizeof(blankData));

    // Act: Trigger scan
    NFCManager::getInstance().scanOnce();

    // Assert: NO writes should occur (blank spools should not be formatted)
    size_t writeCount = g_stubNfc->getPageWrites().size();
    printf("  [DEBUG] Write count: %zu (expected 0)\n", writeCount);

    TEST_ASSERT_EQ(writeCount, (size_t)0);

    teardown_nfc_test();
    return 0;
}

// Test: Read error should show LCD error, not attempt formatting
int test_read_error_not_formatted() {
    setup_nfc_test();

    // Setup: Tag present but reads fail
    uint8_t testUid[7] = {0x04, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    g_stubNfc->setTagPresent(true);
    g_stubNfc->setTagUid(testUid, 7);
    g_stubNfc->setReadError(true);  // Simulate read failure

    // Act: Trigger scan
    NFCManager::getInstance().scanOnce();

    // Assert: NO writes should occur
    size_t writeCount = g_stubNfc->getPageWrites().size();
    printf("  [DEBUG] Write count: %zu (expected 0)\n", writeCount);

    TEST_ASSERT_EQ(writeCount, (size_t)0);

    teardown_nfc_test();
    return 0;
}

// Test: Blank tag removal should still send TAG_REMOVED to ApplicationManager
int test_blank_tag_removal_sends_tag_removed_message() {
    setup_nfc_test();

    uint8_t testUid[7] = {0x04, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
    uint8_t blankData[256] = {0};

    g_stubNfc->setTagPresent(true);
    g_stubNfc->setTagUid(testUid, 7);
    g_stubNfc->setTagData(blankData, sizeof(blankData));
    NFCManager::getInstance().scanOnce();

    g_stubNfc->setTagPresent(false);
    NFCManager::getInstance().scanOnce();

    const auto& sent = ApplicationManager::getInstance().getSentMessages();
    bool sawTagRemoved = false;
    for (const auto& msg : sent) {
        if (msg.type == AppMessageType::TAG_REMOVED) {
            sawTagRemoved = true;
            TEST_ASSERT_EQ(strcmp(msg.payload.tagRemoved.spool_id, "04102030405060"), 0);
            TEST_ASSERT_EQ(msg.payload.tagRemoved.spoolman_id, -1);
            break;
        }
    }

    TEST_ASSERT(sawTagRemoved);

    teardown_nfc_test();
    return 0;
}

// Test: Real OpenSpool tag parses as valid spool
int test_real_spool_bin_parses_as_valid_spool() {
    setup_nfc_test();

    OpenSpoolData tag;
    opt_init(&tag);
    opt_set_material_type(&tag, OPT_MATERIAL_TYPE_PETG);
    strncpy(tag.type, "PETG Jet Black", sizeof(tag.type) - 1);
    const uint8_t petgColor[4] = {0x24, 0x29, 0x2A, 0xFF};
    opt_set_primary_color(&tag, petgColor);
    opt_set_brand_name(&tag, "Prusament");
    opt_set_actual_full_weight(&tag, 1050.0f);
    tag.remaining_weight = 1050.0f;
    tag.spoolman_id = 14;

    uint8_t uid[7] = {0x04, 0x5A, 0xEC, 0x18, 0x09, 0x01, 0x04};
    g_stubNfc->setTagPresent(true);
    g_stubNfc->setTagUid(uid, 7);

    // Write OpenSpool tag into stub connection
    bool wrote = OpenSpoolParser::writeToTag(g_stubNfc, &tag);
    TEST_ASSERT(wrote);
    g_stubNfc->clearPageWrites(); // Clear writes so we verify scanOnce doesn't write

    bool parsed = NFCManager::getInstance().scanOnce();
    TEST_ASSERT(parsed);
    TEST_ASSERT_EQ(g_stubNfc->getWriteCount(), (size_t)0);

    const auto& sent = ApplicationManager::getInstance().getSentMessages();
    bool sawSpoolDetected = false;
    bool sawBlankTag = false;
    for (const auto& msg : sent) {
        if (msg.type == AppMessageType::BLANK_TAG_DETECTED) {
            sawBlankTag = true;
        }
        if (msg.type == AppMessageType::SPOOL_DETECTED) {
            sawSpoolDetected = true;
            TEST_ASSERT_EQ(strcmp(msg.payload.spoolDetected.spool_id, "045AEC18090104"), 0);
            TEST_ASSERT_EQ(msg.payload.spoolDetected.material_type, OPT_MATERIAL_TYPE_PETG);
            TEST_ASSERT(nearlyEqual(msg.payload.spoolDetected.kg_remaining, 1.05f));
            TEST_ASSERT_EQ(msg.payload.spoolDetected.primary_color[0], (uint8_t)0x24);
            TEST_ASSERT_EQ(msg.payload.spoolDetected.primary_color[1], (uint8_t)0x29);
            TEST_ASSERT_EQ(msg.payload.spoolDetected.primary_color[2], (uint8_t)0x2A);
            TEST_ASSERT_EQ(strcmp(msg.payload.spoolDetected.material_name, "PETG Jet Black"), 0);
            TEST_ASSERT_EQ(strcmp(msg.payload.spoolDetected.manufacturer, "Prusament"), 0);
            TEST_ASSERT_EQ(msg.payload.spoolDetected.spoolman_id, 14);
            break;
        }
    }

    TEST_ASSERT(sawSpoolDetected);
    TEST_ASSERT(!sawBlankTag);

    teardown_nfc_test();
    return 0;
}

// Test: Anycubic ACE binary tag parses as valid spool and not blank tag
int test_anycubic_ace_tag_parses_as_valid_spool() {
    setup_nfc_test();

    uint8_t anycubicUid[7] = {0x53, 0x2A, 0x4A, 0xAC, 0x95, 0x00, 0x01};
    g_stubNfc->setTagPresent(true);
    g_stubNfc->setTagUid(anycubicUid, 7);

    uint8_t tagMemory[144] = {0};
    // Page 4: Magic 7B 00 65 00
    tagMemory[4 * 4 + 0] = 0x7B;
    tagMemory[4 * 4 + 1] = 0x00;
    tagMemory[4 * 4 + 2] = 0x65;
    tagMemory[4 * 4 + 3] = 0x00;

    // Pages 5..7: SKU "AHPEBK-102"
    memcpy(&tagMemory[5 * 4], "AHPEBK-102", 10);

    // Page 10: Brand "AC" (Anycubic)
    memcpy(&tagMemory[10 * 4], "AC", 2);

    // Page 15: Material "PETG"
    memcpy(&tagMemory[15 * 4], "PETG", 4);

    // Page 20: Color ABGR (Alpha=0xFF, B=0, G=0, R=0 -> Black)
    tagMemory[20 * 4 + 0] = 0xFF;

    // Page 24: Extruder temp min=230 (0x00E6), max=250 (0x00FA)
    tagMemory[24 * 4 + 0] = 0xE6;
    tagMemory[24 * 4 + 1] = 0x00;
    tagMemory[24 * 4 + 2] = 0xFA;
    tagMemory[24 * 4 + 3] = 0x00;

    // Page 29: Bed temp min=70 (0x0046)
    tagMemory[29 * 4 + 0] = 0x46;
    tagMemory[29 * 4 + 1] = 0x00;

    // Page 30: Diameter 175 (0x00AF)
    tagMemory[30 * 4 + 0] = 0xAF;
    tagMemory[30 * 4 + 1] = 0x00;

    // Page 31: Weight 1000g (0x03E8)
    tagMemory[31 * 4 + 0] = 0xE8;
    tagMemory[31 * 4 + 1] = 0x03;

    g_stubNfc->setTagData(tagMemory, sizeof(tagMemory));

    bool parsed = NFCManager::getInstance().scanOnce();
    TEST_ASSERT(parsed);

    const auto& sent = ApplicationManager::getInstance().getSentMessages();
    bool sawSpoolDetected = false;
    bool sawBlankTag = false;
    for (const auto& msg : sent) {
        if (msg.type == AppMessageType::BLANK_TAG_DETECTED) {
            sawBlankTag = true;
        }
        if (msg.type == AppMessageType::SPOOL_DETECTED) {
            sawSpoolDetected = true;
            TEST_ASSERT_EQ(strcmp(msg.payload.spoolDetected.spool_id, "532A4AAC950001"), 0);
            TEST_ASSERT_EQ(msg.payload.spoolDetected.material_type, OPT_MATERIAL_TYPE_PETG);
            TEST_ASSERT(nearlyEqual(msg.payload.spoolDetected.kg_remaining, 1.0f));
            TEST_ASSERT_EQ(strcmp(msg.payload.spoolDetected.manufacturer, "Anycubic"), 0);
            break;
        }
    }

    TEST_ASSERT(sawSpoolDetected);
    TEST_ASSERT(!sawBlankTag);

    teardown_nfc_test();
    return 0;
}

int main() {
    int passed = 0, failed = 0, total = 0;

    printf("\n=== NFC Read Behavior Tests ===\n\n");

    RUN_TEST(test_blank_spool_not_formatted);
    RUN_TEST(test_read_error_not_formatted);
    RUN_TEST(test_blank_tag_removal_sends_tag_removed_message);
    RUN_TEST(test_real_spool_bin_parses_as_valid_spool);
    RUN_TEST(test_anycubic_ace_tag_parses_as_valid_spool);

    printf("\n=== Results: %d/%d passed ===\n", passed, total);

    return failed > 0 ? 1 : 0;
}
