#ifndef PRUSA_LINK_API_STRATEGY_H
#define PRUSA_LINK_API_STRATEGY_H

#include "IPrinterLinkStrategy.h"
#include <cstddef>
#include <cstdint>
#include <json.hpp>

#ifndef NATIVE_TEST
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

class PrusaLinkAPIStrategy : public IPrinterLinkStrategy {
public:
#ifndef NATIVE_TEST
    void setHttpMutex(SemaphoreHandle_t mutex) { httpMutex_ = mutex; }
    void diagnose();
    void diagGet(const char* path);
    void diagTestFile(const char* downloadRef, size_t fileSize);
#endif
    void update() override;

    bool hasActiveJob() const override { return hasJob; }
    int getJobId() const override { return jobId; }
    float getProgress() const override { return progress; }
    float getTotalFilamentGrams() const override { return totalFilamentG; }
    const char* getJobState() const override { return jobState; }
    bool isConnected() const override { return connected; }
    float fetchDeferredFilament(int expectedJobId) override;

    // Pure helpers exposed for unit testing
    static bool isBgcodePath(const char* downloadRef);
    static void parseStatusPayload(io::stream& stm, bool& outHasJob, int& outJobId, float& outProgress,
                                   char* outPrinterState = nullptr, size_t outPrinterStateSize = 0);
    static void parseJobPayload(io::stream& stm, char* outState, size_t outStateSize, float& outFilamentG,
                                char* outDownloadRef, size_t outDownloadRefSize, size_t& outFileSize,
                                float* outProgress = nullptr);

private:
    static constexpr size_t BGCODE_BUF_SIZE = 8192;
    float fetchFilamentFromFilesApi(const char* downloadRef);
    float fetchFilamentFromBgcode(const char* downloadRef, size_t fileSize = 0);

    bool connected = false;
    bool hasJob = false;
    int jobId = -1;
    float progress = 0.0f;
    float totalFilamentG = 0.0f;
    char jobState[16] = {0};

    // Cache bgcode filament fetch (one attempt per job)
    int bgcodeFilamentJobId = -1;
    float bgcodeFilamentG = 0.0f;

    // Saved download ref for deferred bgcode fetch after print completes
    char savedDownloadRef[160] = {0};
    int savedDownloadRefJobId = -1;
    size_t savedFileSize = 0;
    uint8_t bgcodeBuf_[BGCODE_BUF_SIZE] = {0};

#ifndef NATIVE_TEST
    SemaphoreHandle_t httpMutex_ = nullptr;
#endif
};

#endif // PRUSA_LINK_API_STRATEGY_H
