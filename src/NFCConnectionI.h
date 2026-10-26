#ifndef NFC_CONNECTION_I_H
#define NFC_CONNECTION_I_H

#include <cstdint>
#include <cstddef>

// Interface for NFC hardware abstraction
// Supports ISO 14443-A tags (NTAG215)
class NFCConnectionI {
public:
    virtual ~NFCConnectionI() = default;

    // Hardware initialization
    virtual bool begin() = 0;
    virtual void reset() = 0;
    virtual bool setupRF() = 0;

    // Full hardware reset + RF re-init for recovery
    virtual bool hardwareReset() = 0;

    // Tag detection - returns true if tag found, populates uid/uidLength (7 bytes for NTAG215)
    virtual bool detectTag(uint8_t* uid, uint8_t* uidLength) = 0;

    // Set current UID for addressed commands
    virtual void setCurrentUid(const uint8_t* uid, uint8_t length) = 0;

    // NTAG page read/write (4 bytes per page)
    virtual bool readPage(uint8_t page, uint8_t* buffer4) = 0;
    virtual bool writePage(uint8_t page, const uint8_t* data4) = 0;
};

#endif // NFC_CONNECTION_I_H
