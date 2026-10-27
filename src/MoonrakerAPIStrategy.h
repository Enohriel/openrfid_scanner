#ifndef MOONRAKER_API_STRATEGY_H
#define MOONRAKER_API_STRATEGY_H

#include "IPrinterLinkStrategy.h"
#include <json.hpp>

#ifndef NATIVE_TEST
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

struct MoonrakerQueryResult {
    bool valid = false;
    char rawState[24] = {0};        // "standby", "printing", "paused", "complete", "cancelled", "error"
    char mappedState[16] = {0};     // "PRINTING", "PAUSED", "FINISHED", "STOPPED", "ERROR", or ""
    char filename[160] = {0};
    float filamentUsedMm = 0.0f;
    float progressPercent = 0.0f;   // 0.0 to 100.0
    bool hasActiveJob = false;
};

class MoonrakerAPIStrategy : public IPrinterLinkStrategy {
public:
#ifndef NATIVE_TEST
    void setHttpMutex(SemaphoreHandle_t mutex) { httpMutex_ = mutex; }
#endif
    void update() override;

    bool hasActiveJob() const override { return hasJob_; }
    int getJobId() const override { return jobId_; }
    float getProgress() const override { return progress_; }
    float getTotalFilamentGrams() const override { return totalFilamentG_; }
    float getUsedFilamentGrams() const override { return usedFilamentG_; }
    const char* getJobState() const override { return jobState_; }
    bool isConnected() const override { return connected_; }
    float fetchDeferredFilament(int expectedJobId) override;

    // Pure helpers exposed for unit testing
    static bool buildMoonrakerUrl(char* out, size_t outSize, const char* base, const char* path);
    static void parseQueryPayload(io::stream& stm, MoonrakerQueryResult& out);
    static float parseMetadataFilamentWeight(io::stream& stm);
    static int parseHistoryTotalJobs(io::stream& stm);
    static float mmToGrams(float lengthMm, float diameterMm, float densityGcm3);
    static int computeJobId(const char* filename);

private:
    float fetchMetadataFilamentWeight(const char* filename);
    int fetchHistoryJobNumber();
    void resolveActiveSpoolDensityAndDiameter(float& outDensity, float& outDiameter) const;

    bool connected_ = false;
    bool hasJob_ = false;
    int jobId_ = -1;
    float progress_ = 0.0f;
    float totalFilamentG_ = 0.0f;
    float usedFilamentG_ = 0.0f;
    char jobState_[16] = {0};

    // Cached metadata and history job ID per active print
    int metadataJobId_ = -1;
    float metadataFilamentG_ = 0.0f;
    char savedFilename_[160] = {0};
    int savedFilenameJobId_ = -1;
    char cachedJobFilename_[160] = {0};
    int cachedHistoryJobId_ = -1;

#ifndef NATIVE_TEST
    SemaphoreHandle_t httpMutex_ = nullptr;
#endif
};

#endif // MOONRAKER_API_STRATEGY_H
