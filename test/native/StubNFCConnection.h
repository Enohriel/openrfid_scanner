#ifndef STUB_NFC_CONNECTION_H
#define STUB_NFC_CONNECTION_H

#include "NFCConnectionI.h"
#include <vector>
#include <cstring>

// Test stub for NFC connection - allows simulating tag presence and tracking writes
class StubNFCConnection : public NFCConnectionI {
public:
    StubNFCConnection() {
        memset(currentUid_, 0, sizeof(currentUid_));
        memset(tagData_, 0, sizeof(tagData_));
    }

    ~StubNFCConnection() override = default;

    // NFCConnectionI interface
    bool begin() override { return true; }
    void reset() override {}
    bool hardwareReset() override { return true; }
    bool setupRF() override { return true; }

    bool detectTag(uint8_t* uid, uint8_t* uidLength) override {
        if (!tagPresent_) {
            return false;
        }
        memcpy(uid, tagUid_, tagUidLength_);
        *uidLength = tagUidLength_;
        return true;
    }

    void setCurrentUid(const uint8_t* uid, uint8_t length) override {
        memcpy(currentUid_, uid, length <= 8 ? length : 8);
    }

    bool readPage(uint8_t page, uint8_t* buffer4) override {
        if (readError_) {
            return false;
        }
        size_t offset = page * 4;
        if (offset + 4 <= sizeof(tagData_)) {
            memcpy(buffer4, tagData_ + offset, 4);
        } else {
            memset(buffer4, 0, 4);
        }
        return true;
    }

    bool writePage(uint8_t page, const uint8_t* data4) override {
        if (writeError_) {
            return false;
        }
        if (failNextWrites_ > 0) {
            failNextWrites_--;
            return false;
        }

        // Track the write
        PageWrite pw;
        pw.page = page;
        memcpy(pw.data, data4, 4);
        pageWrites_.push_back(pw);

        // Update internal tag data
        size_t offset = page * 4;
        if (offset + 4 <= sizeof(tagData_)) {
            memcpy(tagData_ + offset, data4, 4);
            if (offset + 4 > tagDataSize_) {
                tagDataSize_ = offset + 4;
            }
        }

        return true;
    }

    // Test control methods
    void setTagPresent(bool present) { tagPresent_ = present; }

    void setTagUid(const uint8_t* uid, uint8_t length) {
        memcpy(tagUid_, uid, length <= 8 ? length : 8);
        tagUidLength_ = length;
    }

    void setTagData(const uint8_t* data, size_t size) {
        size_t copySize = size < sizeof(tagData_) ? size : sizeof(tagData_);
        memcpy(tagData_, data, copySize);
        tagDataSize_ = copySize;
    }

    void setReadError(bool error) { readError_ = error; }
    void setWriteError(bool error) { writeError_ = error; }
    void setFailNextWrites(size_t count) { failNextWrites_ = count; }

    // Test inspection methods
    struct PageWrite {
        uint8_t page;
        uint8_t data[4];
    };

    const std::vector<PageWrite>& getPageWrites() const { return pageWrites_; }
    size_t getWriteCount() const { return pageWrites_.size(); }
    void clearPageWrites() { pageWrites_.clear(); }

    // Reset test state
    void resetTestState() {
        tagPresent_ = false;
        readError_ = false;
        writeError_ = false;
        failNextWrites_ = 0;
        pageWrites_.clear();
        memset(tagData_, 0, sizeof(tagData_));
        tagDataSize_ = 0;
    }

private:
    uint8_t currentUid_[8];

    // Tag simulation state
    bool tagPresent_ = false;
    uint8_t tagUid_[8] = {0};
    uint8_t tagUidLength_ = 7;
    uint8_t tagData_[540] = {0};
    size_t tagDataSize_ = 0;

    // Error simulation
    bool readError_ = false;
    bool writeError_ = false;
    size_t failNextWrites_ = 0;

    // Write tracking
    std::vector<PageWrite> pageWrites_;
};

#endif // STUB_NFC_CONNECTION_H
