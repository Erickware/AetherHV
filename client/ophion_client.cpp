#include "ophion_client.hpp"
#include <cstring>

namespace ophion {
client::client() {
    handle_ = CreateFileW(L"\\\\.\\Ophion", GENERIC_READ | GENERIC_WRITE, 0,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE)
        throw std::system_error(GetLastError(), std::system_category(), "Open Ophion");
}
client::~client() { if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }

OPH_PACKET client::exchange(OPH_PACKET request) {
    std::lock_guard<std::mutex> lock(mutex_);
    OPH_PACKET response{};
    DWORD bytes = 0;
    request.magic = OPH_MAGIC; request.version = OPH_VERSION;
    request.size = sizeof(request); request.request_id = ++request_;
    if (!DeviceIoControl(handle_, IOCTL_OPH_EXCHANGE, &request, sizeof(request),
        &response, sizeof(response), &bytes, nullptr))
        throw std::system_error(GetLastError(), std::system_category(), "Ophion request");
    if (bytes != sizeof(response) || !oph_packet_valid(&response) || response.status < 0 ||
        response.operation != request.operation || response.cpu != request.cpu ||
        response.request_id != request.request_id || !response.root_tsc ||
        response.cpu >= last_root_.size() || response.root_sequence <= last_root_[response.cpu])
        throw std::runtime_error("Invalid or stale VMX-root response");
    last_root_[response.cpu] = response.root_sequence;
    if (request.operation == OPH_PING &&
        response.args[0] != (request.request_id ^ 0x4f5048494f4e5254ULL))
        throw std::runtime_error("Root nonce mismatch");
    return response;
}
OPH_PACKET client::call(uint32_t op, uint32_t cpu, uint64_t a0, uint64_t a1, uint64_t a2) {
    OPH_PACKET p{};
    p.operation = op; p.cpu = cpu;
    p.args[0] = a0; p.args[1] = a1; p.args[2] = a2;
    return exchange(p);
}
buffer::buffer(client& connection): client_(connection) {
    memory_ = VirtualAlloc(nullptr, OPH_PAGE_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!memory_) throw std::system_error(GetLastError(), std::system_category(), "VirtualAlloc");
    try {
        token_ = client_.call(OPH_REGISTER, 0,
            reinterpret_cast<uint64_t>(memory_), OPH_PAGE_SIZE).args[0];
    } catch (...) { VirtualFree(memory_, 0, MEM_RELEASE); memory_ = nullptr; throw; }
}
buffer::~buffer() {
    if (token_) {
        try { client_.call(OPH_UNREGISTER, 0, token_); }
        catch (...) {
            /* Keep VA committed on failure. Driver holds the MDL until handle
               cleanup. Process teardown will reclaim VA after this client. */
            return;
        }
    }
    if (memory_) VirtualFree(memory_, 0, MEM_RELEASE);
}
OPH_PACKET buffer::read(uint64_t offset, uint64_t length, uint32_t cpu) {
    if (!oph_range_valid(offset, length)) throw std::out_of_range("Buffer read");
    return client_.call(OPH_READ, cpu, token_, offset, length);
}
void buffer::write(uint64_t offset, const void *data, uint64_t length, uint32_t cpu) {
    if (!data || !oph_range_valid(offset, length)) throw std::out_of_range("Buffer write");
    OPH_PACKET p{};
    p.operation = OPH_WRITE; p.cpu = cpu;
    p.args[0] = token_; p.args[1] = offset; p.args[2] = length;
    std::memcpy(p.data, data, static_cast<size_t>(length));
    client_.exchange(p);
}
}
