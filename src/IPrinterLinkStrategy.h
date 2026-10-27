#ifndef I_PRINTER_LINK_STRATEGY_H
#define I_PRINTER_LINK_STRATEGY_H

#ifdef NATIVE_TEST
#include "platform/NativePlatform.h"
#else
#include <Arduino.h>
#endif

class IPrinterLinkStrategy {
public:
    virtual ~IPrinterLinkStrategy() = default;

    // Called each poll cycle - strategy updates its internal state
    virtual void update() = 0;

    // Job status queries
    virtual bool hasActiveJob() const = 0;
    virtual int getJobId() const = 0;
    virtual float getProgress() const = 0;
    virtual float getTotalFilamentGrams() const = 0;

    // Actual filament used in grams if tracked directly by the printer (e.g. Klipper filament_used mm).
    // Returns <= 0.0f if the printer only provides total job filament estimate.
    virtual float getUsedFilamentGrams() const { return 0.0f; }

    // Job state: "PRINTING", "PAUSED", "FINISHED", "STOPPED", "ERROR", or ""
    virtual const char* getJobState() const = 0;

    // Connection status
    virtual bool isConnected() const = 0;

    // Deferred filament fetch for a specific job.
    // Implementations must return data only for the requested job ID.
    virtual float fetchDeferredFilament(int expectedJobId) {
        (void)expectedJobId;
        return 0.0f;
    }
};

#endif // I_PRINTER_LINK_STRATEGY_H
