#include "ophion_client.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <vector>

static void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
static void expect_rejected(ophion::client& c, OPH_PACKET p, DWORD input, DWORD output) {
    DWORD bytes = 0;
    BOOL ok = DeviceIoControl(c.native_handle(), IOCTL_OPH_EXCHANGE,
        &p, input, &p, output, &bytes, nullptr);
    require(!ok, "Malformed request was accepted");
}
struct affinity_guard {
    DWORD_PTR old;
    explicit affinity_guard(unsigned cpu): old(SetThreadAffinityMask(GetCurrentThread(), 1ULL << cpu)) {
        if (!old) throw std::system_error(GetLastError(), std::system_category(), "Affinity");
    }
    ~affinity_guard() { SetThreadAffinityMask(GetCurrentThread(), old); }
};
int main() {
    try {
        ophion::client c;
        auto version = c.call(OPH_GET_VERSION);
        unsigned cpus = static_cast<unsigned>(version.args[2]);
        require(cpus > 0 && cpus <= 64, "Invalid CPU count");
        std::cout << "Protocol " << version.args[0] << ", CPUs " << cpus
                  << ", timing compensation " << version.args[3] << '\n';
        OPH_PACKET malformed{};
        malformed.magic = OPH_MAGIC; malformed.version = OPH_VERSION; malformed.size = sizeof(malformed);
        malformed.operation = OPH_PING;
        expect_rejected(c, malformed, sizeof(malformed)-1, sizeof(malformed));
        expect_rejected(c, malformed, sizeof(malformed), sizeof(malformed)-1);
        malformed.version++;
        expect_rejected(c, malformed, sizeof(malformed), sizeof(malformed));
        malformed.version = OPH_VERSION; malformed.operation = 0xffffffff;
        expect_rejected(c, malformed, sizeof(malformed), sizeof(malformed));
        malformed.operation = OPH_PING; malformed.cpu = cpus;
        expect_rejected(c, malformed, sizeof(malformed), sizeof(malformed));
        malformed.cpu = 0; malformed.operation = OPH_READ; malformed.args[2] = 8;
        expect_rejected(c, malformed, sizeof(malformed), sizeof(malformed)); // no registration
        ophion::buffer b(c);
        malformed.operation = OPH_REGISTER;
        malformed.args[0] = reinterpret_cast<uint64_t>(b.data()); malformed.args[1] = OPH_PAGE_SIZE;
        expect_rejected(c, malformed, sizeof(malformed), sizeof(malformed)); // second registration
        malformed.operation = OPH_READ; malformed.args[0] = b.token() + 1;
        malformed.args[1] = 0; malformed.args[2] = 8;
        expect_rejected(c, malformed, sizeof(malformed), sizeof(malformed)); // stale generation
        malformed.args[0] = b.token(); malformed.args[2] = 0;
        expect_rejected(c, malformed, sizeof(malformed), sizeof(malformed));
        malformed.args[2] = OPH_DATA_SIZE + 1;
        expect_rejected(c, malformed, sizeof(malformed), sizeof(malformed));
        const uint64_t marker = 0x1122334455667788ULL;
        b.write(16, &marker, sizeof(marker));
        auto read = b.read(16, sizeof(marker));
        require(std::memcmp(read.data, &marker, sizeof(marker)) == 0, "Root memory round trip");
        require(std::memcmp(const_cast<const uint8_t *>(b.data())+16, &marker, sizeof(marker)) == 0,
            "Root write not visible in application");
        auto translation = c.call(OPH_TRANSLATE, 0, b.token(), 16);
        require(translation.args[2] == translation.args[4], "GPA/HPA identity mismatch");
        for (auto offset : {4096ULL, ~0ULL}) {
            malformed = {}; malformed.magic = OPH_MAGIC; malformed.version = OPH_VERSION;
            malformed.size = sizeof(malformed); malformed.operation = OPH_READ;
            malformed.args[0] = b.token(); malformed.args[1] = offset; malformed.args[2] = 8;
            expect_rejected(c, malformed, sizeof(malformed), sizeof(malformed));
        }
        std::vector<uint64_t> cursors(cpus);
        for (unsigned cpu=0; cpu<cpus; ++cpu) {
            auto e = c.call(OPH_GET_EVENTS, cpu);
            while (e.args[1]) e = c.call(OPH_GET_EVENTS, cpu, e.args[0]);
            cursors[cpu] = e.args[0];
        }
        b.arm();
        for (unsigned cpu=0; cpu<cpus; ++cpu) {
            affinity_guard pin(cpu);
            b.data()[128+cpu] = static_cast<uint8_t>(cpu+1);
            b.data()[128+cpu] = static_cast<uint8_t>(cpu+2); // one-shot: no second event
        }
        for (unsigned cpu=0; cpu<cpus; ++cpu) {
            auto reply = c.call(OPH_GET_EVENTS, cpu, cursors[cpu]);
            require(reply.args[1] == 1 && reply.args[2] == 0, "Expected one EPT event per CPU");
            OPH_EVENT e{}; std::memcpy(&e, reply.data, sizeof(e));
            require(e.kind == OPH_EVENT_WRITE && e.cpu == cpu && e.generation == b.token() &&
                (e.qualification & 2) && e.offset == 128+cpu, "EPT event contents");
            require(b.data()[128+cpu] == cpu+2, "Guest write was not retried");
        }
        b.disarm();
        for (unsigned cpu=0; cpu<cpus; ++cpu) {
            auto stats = c.call(OPH_GET_STATS, cpu);
            uint64_t exits[OPH_EXIT_BUCKETS]{}; std::memcpy(exits, stats.data, sizeof(exits));
            require(exits[18] > 0 && exits[48] > 0, "VMCALL/EPT counters missing");
        }
        std::vector<double> latency;
        for (unsigned i=0; i<300; ++i) {
            auto start = std::chrono::steady_clock::now(); c.call(OPH_PING);
            latency.push_back(std::chrono::duration<double,std::micro>(
                std::chrono::steady_clock::now()-start).count());
        }
        std::sort(latency.begin(), latency.end());
        std::cout << "PING round trip us: median=" << latency[150] << " p95=" << latency[285]
                  << " p99=" << latency[297] << "\nPASS: root nonce/sequence, memory, translation, EPT, ABI rejection\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
