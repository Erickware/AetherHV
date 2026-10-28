/* Public wire ABI: fixed-width integers, no pointers or compiler-sized fields. */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define OPH_MAGIC 0x3148504fU
#define OPH_VERSION 1U
#define OPH_DATA_SIZE 1024U
#define OPH_PAGE_SIZE 4096U
#define OPH_EXIT_BUCKETS 65U /* bucket 64 = other */
#define OPH_EVENT_CAPACITY 64U
#define OPH_EVENT_BATCH 16U
/* FILE_DEVICE_UNKNOWN, function 0x801, METHOD_BUFFERED, READ|WRITE access. */
#define IOCTL_OPH_EXCHANGE 0x0022e004U
#define VMCALL_PING 0x100U
#define VMCALL_GET_VERSION 0x101U
#define VMCALL_OPH_EXCHANGE 0x102U

enum OPH_OPERATION {
    OPH_PING = 1, OPH_GET_VERSION, OPH_GET_STATS, OPH_REGISTER,
    OPH_UNREGISTER, OPH_READ, OPH_WRITE, OPH_TRANSLATE,
    OPH_ARM_WRITE, OPH_DISARM, OPH_GET_EVENTS
};
enum OPH_FEATURE {
    OPH_FEATURE_ROOT_RPC = 1, OPH_FEATURE_LAB_MEMORY = 2,
    OPH_FEATURE_WRITE_ONESHOT = 4, OPH_FEATURE_EVENTS = 8
};
enum OPH_EVENT_KIND { OPH_EVENT_WRITE = 1, OPH_EVENT_UNEXPECTED_EPT = 2 };

#pragma pack(push, 8)
typedef struct OPH_PACKET {
    uint32_t magic, version, size, operation;
    uint64_t request_id;
    int32_t status;
    uint32_t cpu;
    uint64_t root_sequence, root_tsc;
    uint64_t args[8];
    uint8_t data[OPH_DATA_SIZE];
} OPH_PACKET;

typedef struct OPH_EVENT {
    uint64_t sequence, tsc, generation, offset, qualification;
    uint32_t cpu, kind;
} OPH_EVENT;
#pragma pack(pop)

#if defined(__cplusplus)
static_assert(sizeof(OPH_PACKET) == 1136, "wire size");
static_assert(sizeof(OPH_EVENT) == 48, "event size");
#else
_Static_assert(sizeof(OPH_PACKET) == 1136, "wire size");
_Static_assert(sizeof(OPH_EVENT) == 48, "event size");
#endif

static inline int oph_range_valid(uint64_t offset, uint64_t length) {
    return length > 0 && length <= OPH_DATA_SIZE &&
        offset < OPH_PAGE_SIZE && length <= OPH_PAGE_SIZE - offset;
}
static inline int oph_packet_valid(const OPH_PACKET *p) {
    return p && p->magic == OPH_MAGIC && p->version == OPH_VERSION &&
        p->size == sizeof(*p) && p->operation >= OPH_PING &&
        p->operation <= OPH_GET_EVENTS;
}
