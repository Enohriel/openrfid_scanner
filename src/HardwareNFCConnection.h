#ifndef HARDWARE_NFC_CONNECTION_H
#define HARDWARE_NFC_CONNECTION_H

#include "NFCConnectionI.h"
#include <PN5180.h>
#include <PN5180ISO14443.h>

// Production NFC connection using PN5180 hardware for ISO 14443-A (NTAG215)
class HardwareNFCConnection : public NFCConnectionI {
public:
    HardwareNFCConnection();
    ~HardwareNFCConnection() override;

    bool begin() override;
    void reset() override;
    bool hardwareReset() override;
    bool setupRF() override;
    bool detectTag(uint8_t* uid, uint8_t* uidLength) override;
    void setCurrentUid(const uint8_t* uid, uint8_t length) override;

    bool readPage(uint8_t page, uint8_t* buffer4) override;
    bool writePage(uint8_t page, const uint8_t* data4) override;
    bool readBlock(uint8_t startPage, uint8_t* buffer16) override;

    // Diagnostics: log RF_STATUS, IRQ_STATUS, SYSTEM_STATUS registers
    void logDiagnostics();

private:
    PN5180ISO14443* nfc_ = nullptr;
    uint8_t currentUid_[7];

    // PN5180 SPI pins (right side of ESP32)
    static constexpr int PN5180_RST   = 13;  // Hardware reset (active low)
    static constexpr int PN5180_NSS   = 14;  // SPI chip select (active low)
    static constexpr int PN5180_MOSI  = 27;  // SPI master-out slave-in
    static constexpr int PN5180_MISO  = 26;  // SPI master-in slave-out
    static constexpr int PN5180_SCK   = 25;  // SPI clock
    static constexpr int PN5180_BUSY  = 33;  // Busy signal (input)

    static constexpr int PN5180_GPIO  = 32;  // General purpose I/O
    static constexpr int PN5180_IRQ   = 35;  // Interrupt request
    static constexpr int PN5180_AUX   = 34;  // Auxiliary monitoring
};

#endif // HARDWARE_NFC_CONNECTION_H
