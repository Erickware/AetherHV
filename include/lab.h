#pragma once
#include "ophion_protocol.h"

typedef struct _LAB_CPU {
    OPH_PACKET mailbox;
    volatile LONG pending;
    UINT64 sequence, exits[OPH_EXIT_BUCKETS], handler_ticks, max_handler_ticks;
    OPH_EVENT events[OPH_EVENT_CAPACITY];
    UINT64 event_sequence;
    PVMM_EPT_DYNAMIC_SPLIT split;
    UINT64 split_pa;
    PEPT_PML2_ENTRY parent;
    PEPT_PML1_ENTRY leaf;
    UINT64 original_parent, original_leaf, page_pa, generation;
    BOOLEAN installed, armed, owns_split;
} LAB_CPU;

BOOLEAN lab_cpu_init(VIRTUAL_MACHINE_STATE *vcpu);
VOID lab_cpu_destroy(VIRTUAL_MACHINE_STATE *vcpu);
VOID lab_root_dispatch(VIRTUAL_MACHINE_STATE *vcpu);
BOOLEAN lab_ept_violation(VIRTUAL_MACHINE_STATE *vcpu, UINT64 gpa);
VOID lab_record_exit(VIRTUAL_MACHINE_STATE *vcpu);
VOID lab_record_duration(VIRTUAL_MACHINE_STATE *vcpu, UINT64 start);
NTSTATUS lab_exchange(OPH_PACKET *packet);
VOID lab_initialize(VOID);
NTSTATUS lab_open(PFILE_OBJECT file);
VOID lab_close(PFILE_OBJECT file);
NTSTATUS lab_ioctl(PFILE_OBJECT file, OPH_PACKET *packet);
VOID *hostcr3_map_lab(UINT32 cpu, UINT64 page_pa);
VOID hostcr3_unmap_lab(UINT32 cpu);
BOOLEAN hostcr3_lab_init(VOID);
BOOLEAN broadcast_prepare_all(VOID);
