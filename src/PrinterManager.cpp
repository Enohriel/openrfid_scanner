#include "PrinterManager.h"
#include "ConfigurationManager.h"
#include "ApplicationManager.h"
#include <Arduino.h>
#include <cstring>

PrinterManager& PrinterManager::getInstance() {
    static PrinterManager instance;
    return instance;
}

void PrinterManager::begin() {
    state = PrinterState::IDLE;
    currentJobId = -1;
    currentJobTotalFilamentG = 0.0f;
    currentJobUsedFilamentG = 0.0f;
    lastProgressPercent = 0.0f;
    missingJobPollCount = 0;
    Serial.println("PrinterManager: Initialized");
}

void PrinterManager::startPollingTask() {
    if (pollingTaskHandle != nullptr) {
        Serial.println("PrinterManager: Polling task already running");
        return;
    }

    xTaskCreate(
        pollingTaskFunc,
        "PrinterPoll",
        POLLING_TASK_STACK_SIZE,
        this,
        POLLING_TASK_PRIORITY,
        &pollingTaskHandle
    );

    Serial.println("PrinterManager: Polling task started");
}

void PrinterManager::pollingTaskFunc(void* param) {
    PrinterManager* self = static_cast<PrinterManager*>(param);
    auto& config = ConfigurationManager::getInstance();

    while (true) {
        self->poll();
        vTaskDelay(pdMS_TO_TICKS(config.getPollIntervalMs()));
    }
}

void PrinterManager::setStrategy(IPrinterLinkStrategy* strat) {
    strategy = strat;
}

bool PrinterManager::isConnected() const {
    return strategy != nullptr && strategy->isConnected();
}

void PrinterManager::poll() {
    if (!strategy) {
        return;
    }

    strategy->update();

    // Check connection status
    if (!strategy->isConnected()) {
        if (state == PrinterState::TRACKING) {
            missingJobPollCount++;
            if (missingJobPollCount >= JOB_MISSING_GRACE_POLLS) {
                Serial.printf("PrinterManager: Job %d missing after disconnect (%u polls)\n",
                    currentJobId, missingJobPollCount);
                handleJobDisappeared();
            } else {
                Serial.printf("PrinterManager: Connection lost while tracking job %d (grace %u/%u)\n",
                    currentJobId, missingJobPollCount, JOB_MISSING_GRACE_POLLS);
            }
        }
        return;
    }

    // Check if there's a job
    if (!strategy->hasActiveJob()) {
        if (state == PrinterState::TRACKING) {
            const char* st = strategy->getJobState();
            if (st != nullptr && strcmp(st, "FINISHED") == 0) {
                resolveAndSendJobEnd(currentJobId, 100.0f, true, "finished");
                return;
            }
            if (st != nullptr &&
                (strcmp(st, "STOPPED") == 0 || strcmp(st, "CANCELED") == 0 || strcmp(st, "ERROR") == 0)) {
                resolveAndSendJobEnd(currentJobId, lastProgressPercent, true, "stopped_or_error");
                return;
            }

            missingJobPollCount++;
            if (missingJobPollCount >= JOB_MISSING_GRACE_POLLS) {
                Serial.printf("PrinterManager: Job %d missing from API (%u polls)\n",
                    currentJobId, missingJobPollCount);
                handleJobDisappeared();
            } else {
                Serial.printf("PrinterManager: Job %d temporarily missing (grace %u/%u)\n",
                    currentJobId, missingJobPollCount, JOB_MISSING_GRACE_POLLS);
            }
        }
        return;
    }

    missingJobPollCount = 0;

    // Get job info from strategy
    int jobId = strategy->getJobId();
    float progress = strategy->getProgress();
    float totalFilamentG = strategy->getTotalFilamentGrams();
    float usedFilamentG = strategy->getUsedFilamentGrams();
    const char* jobState = strategy->getJobState();

    // State machine logic
    if (state == PrinterState::IDLE) {
        if (strcmp(jobState, "PRINTING") == 0 || strcmp(jobState, "PAUSED") == 0) {
            handleJobDetected(jobId, totalFilamentG, progress);
            if (usedFilamentG > 0.0f) {
                currentJobUsedFilamentG = usedFilamentG;
            }
        }
    } else if (state == PrinterState::TRACKING) {
        // Check if job ID changed (new job while we were tracking)
        if (jobId != currentJobId) {
            // Old job was replaced - treat as canceled
            resolveAndSendJobEnd(currentJobId, lastProgressPercent, false, "job_replaced");
            // Start tracking new job
            handleJobDetected(jobId, totalFilamentG, progress);
            if (usedFilamentG > 0.0f) {
                currentJobUsedFilamentG = usedFilamentG;
            }
            return;
        }

        // Update progress for potential cancel estimation
        lastProgressPercent = progress;

        // Update stored filament if API still reports it (may become 0 once finished)
        if (totalFilamentG > 0) {
            currentJobTotalFilamentG = totalFilamentG;
        }
        if (usedFilamentG > 0.0f) {
            currentJobUsedFilamentG = usedFilamentG;
        }

        if (strcmp(jobState, "FINISHED") == 0) {
            resolveAndSendJobEnd(jobId, 100.0f, true, "finished");
        } else if (strcmp(jobState, "STOPPED") == 0 || strcmp(jobState, "ERROR") == 0 || strcmp(jobState, "CANCELED") == 0) {
            resolveAndSendJobEnd(jobId, progress, true, "stopped_or_error");
        }
        // PRINTING or PAUSED - continue tracking
    }
}

void PrinterManager::handleJobDetected(int jobId, float totalFilamentG, float initialProgress) {
    state = PrinterState::TRACKING;
    currentJobId = jobId;
    currentJobTotalFilamentG = totalFilamentG;
    currentJobUsedFilamentG = 0.0f;
    lastProgressPercent = (initialProgress > 0.0f) ? initialProgress : 0.0f;
    missingJobPollCount = 0;

    AppMessage msg;
    msg.type = AppMessageType::PRINT_STARTED;
    msg.payload.printStarted.job_id = jobId;
    ApplicationManager::getInstance().sendMessage(msg);

    if (totalFilamentG <= 0) {
        Serial.printf("PrinterManager: WARNING - No filament data from API for job %d\n", jobId);
    }

    Serial.printf("PrinterManager: Now tracking job %d (total filament: %.2fg, progress: %.1f%%)\n",
        jobId, totalFilamentG, lastProgressPercent);
}

void PrinterManager::resolveAndSendJobEnd(int jobId, float progressPercent, bool allowDeferredFilament, const char* reason) {
    bool canceled = (progressPercent < 100.0f);

    Serial.printf("PrinterManager: Resolving job %d reason=%s progress=%.1f%% total=%.2fg used=%.2fg allow_deferred=%s\n",
        jobId, reason ? reason : "unknown", progressPercent, currentJobTotalFilamentG, currentJobUsedFilamentG,
        allowDeferredFilament ? "true" : "false");

    // Try deferred filament only when explicitly allowed and for this exact job
    if (currentJobUsedFilamentG <= 0.0f && currentJobTotalFilamentG <= 0.0f && strategy) {
        if (allowDeferredFilament) {
            float deferred = strategy->fetchDeferredFilament(jobId);
            if (deferred > 0.0f) {
                currentJobTotalFilamentG = deferred;
                Serial.printf("PrinterManager: Got deferred filament for job %d: %.2fg\n", jobId, deferred);
            } else {
                Serial.printf("PrinterManager: No deferred filament available for job %d\n", jobId);
            }
        } else {
            Serial.printf("PrinterManager: Skipping deferred filament lookup for job %d (reason=%s)\n",
                jobId, reason ? reason : "unknown");
        }
    }

    float filamentUsed = (currentJobUsedFilamentG > 0.0f)
        ? currentJobUsedFilamentG
        : ((progressPercent / 100.0f) * currentJobTotalFilamentG);

    AppMessage msg;
    msg.type = AppMessageType::PRINT_ENDED;
    msg.payload.printEnded.job_id = jobId;
    msg.payload.printEnded.filament_used_grams = filamentUsed;
    msg.payload.printEnded.canceled = canceled;
    ApplicationManager::getInstance().sendMessage(msg);

    Serial.printf("PrinterManager: Job %d %s at %.1f%% (filament: %.2fg)\n",
        jobId, canceled ? "canceled" : "finished", progressPercent, filamentUsed);

    state = PrinterState::IDLE;
    currentJobId = -1;
    currentJobTotalFilamentG = 0.0f;
    currentJobUsedFilamentG = 0.0f;
    lastProgressPercent = 0.0f;
    missingJobPollCount = 0;
}

void PrinterManager::handleJobDisappeared() {
    const char* lastState = strategy ? strategy->getJobState() : "";
    bool printerFinished = (lastState != nullptr && strcmp(lastState, "FINISHED") == 0);
    bool printerCanceled = (lastState != nullptr &&
        (strcmp(lastState, "STOPPED") == 0 || strcmp(lastState, "CANCELED") == 0 || strcmp(lastState, "ERROR") == 0));

    float progress = lastProgressPercent;
    if (printerFinished || lastProgressPercent >= 95.0f || (!printerCanceled && lastProgressPercent <= 0.0f)) {
        progress = 100.0f;
    }

    Serial.printf("PrinterManager: Job %d disappeared at %.1f%% (printerState='%s') - treating as %s\n",
        currentJobId, lastProgressPercent, lastState ? lastState : "",
        progress >= 100.0f ? "finished" : "canceled");

    resolveAndSendJobEnd(currentJobId, progress, true, "job_disappeared");
}
