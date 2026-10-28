#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <array>
#include "ophion_protocol.h"

namespace ophion {
class client {
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::mutex mutex_;
    uint64_t request_ = 0;
    std::array<uint64_t, 64> last_root_{};
public:
    client();
    ~client();
    client(const client&) = delete;
    client& operator=(const client&) = delete;
    OPH_PACKET exchange(OPH_PACKET request);
    OPH_PACKET call(uint32_t operation, uint32_t cpu = 0,
        uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0);
    HANDLE native_handle() const { return handle_; }
};

/* Owns committed, page-aligned memory and its registration. The client must
   outlive this object. CloseHandle also synchronously disarms/unpins on errors. */
class buffer {
    client& client_;
    void *memory_ = nullptr;
    uint64_t token_ = 0;
public:
    explicit buffer(client& connection);
    ~buffer();
    buffer(const buffer&) = delete;
    buffer& operator=(const buffer&) = delete;
    uint64_t token() const { return token_; }
    volatile uint8_t *data() const { return static_cast<volatile uint8_t *>(memory_); }
    OPH_PACKET read(uint64_t offset, uint64_t length, uint32_t cpu = 0);
    void write(uint64_t offset, const void *data, uint64_t length, uint32_t cpu = 0);
    void arm() { client_.call(OPH_ARM_WRITE, 0, token_); }
    void disarm() { client_.call(OPH_DISARM, 0, token_); }
};
}
