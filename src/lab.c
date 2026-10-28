/* One exclusive, process-bound laboratory session. All pageable/Windows work
   stays in non-root. Root only touches preallocated state and a pinned PFN. */
#include "hv.h"
#include "ophion_paging.h"

static struct {
    KMUTEX mutex;
    PFILE_OBJECT file;
    PEPROCESS owner;
    PMDL mdl;
    UINT64 user_va, page_pa, generation;
    volatile LONG registered;
} g_lab;

static VOID root_copy(volatile UCHAR *dst, const volatile UCHAR *src, SIZE_T n)
{
    while (n--) *dst++ = *src++;
}

static VOID root_flush(VIRTUAL_MACHINE_STATE *v)
{
    INVEPT_DESCRIPTOR d = {0};
    d.EptPointer = v->ept_pointer;
    if (asm_invept(InveptSingleContext, &d)) {
        /* Never resume with stale translations or free a still-referenced page. */
        _disable();
        for (;;) __halt();
    }
}

BOOLEAN lab_cpu_init(VIRTUAL_MACHINE_STATE *v)
{
    v->lab = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(LAB_CPU), HV_POOL_TAG);
    if (!v->lab) return FALSE;
    v->lab->split = ExAllocatePool2(POOL_FLAG_NON_PAGED,
        sizeof(VMM_EPT_DYNAMIC_SPLIT), HV_POOL_TAG);
    if (!v->lab->split) return FALSE;
    v->lab->split_pa = va_to_pa(v->lab->split);
    return TRUE;
}

VOID lab_cpu_destroy(VIRTUAL_MACHINE_STATE *v)
{
    if (!v->lab) return;
    if (v->lab->split) ExFreePoolWithTag(v->lab->split, HV_POOL_TAG);
    ExFreePoolWithTag(v->lab, HV_POOL_TAG);
    v->lab = NULL;
}

VOID lab_record_exit(VIRTUAL_MACHINE_STATE *v)
{
    ++v->lab->exits[v->exit_reason < 64 ? v->exit_reason : 64];
}

VOID lab_record_duration(VIRTUAL_MACHINE_STATE *v, UINT64 start)
{
    UINT64 delta = __rdtsc() - start;
    v->lab->handler_ticks += delta;
    if (delta > v->lab->max_handler_ticks) v->lab->max_handler_ticks = delta;
}

static VOID record_event(VIRTUAL_MACHINE_STATE *v, UINT64 gpa, UINT32 kind)
{
    LAB_CPU *l = v->lab;
    UINT64 seq = ++l->event_sequence;
    OPH_EVENT *e = &l->events[(seq - 1) % OPH_EVENT_CAPACITY];
    e->sequence = seq;
    e->tsc = __rdtsc();
    e->generation = l->generation;
    /* Unexpected addresses are deliberately not exposed. */
    e->offset = kind == OPH_EVENT_WRITE ? gpa & 0xfff : 0;
    e->qualification = v->exit_qual;
    e->cpu = v->core_id;
    e->kind = kind;
}

BOOLEAN lab_ept_violation(VIRTUAL_MACHINE_STATE *v, UINT64 gpa)
{
    LAB_CPU *l = v->lab;
    if (l->installed && l->armed && (gpa & ~0xfffULL) == l->page_pa &&
        (v->exit_qual & 2)) {
        record_event(v, gpa, OPH_EVENT_WRITE);
        l->leaf->AsUInt = l->original_leaf;
        l->armed = FALSE;
        root_flush(v);
        v->advance_rip = FALSE; /* Retry the faulting write, once, with original permissions. */
        return TRUE;
    }
    record_event(v, gpa, OPH_EVENT_UNEXPECTED_EPT);
    return FALSE;
}

static VOID root_disarm(VIRTUAL_MACHINE_STATE *v)
{
    LAB_CPU *l = v->lab;
    if (!l->installed) return;
    l->leaf->AsUInt = l->original_leaf;
    if (l->owns_split) l->parent->AsUInt = l->original_parent;
    root_flush(v);
    l->installed = l->armed = FALSE;
}

VOID lab_root_dispatch(VIRTUAL_MACHINE_STATE *v)
{
    LAB_CPU *l = v->lab;
    OPH_PACKET *p = &l->mailbox;
    if (_InterlockedExchange(&l->pending, 0) != 1) {
        v->regs->rax = (UINT64)STATUS_INVALID_DEVICE_STATE;
        return;
    }
    p->status = STATUS_SUCCESS;
    p->cpu = v->core_id;
    p->root_sequence = ++l->sequence;
    p->root_tsc = __rdtsc();
    if (!oph_packet_valid(p)) {
        p->status = STATUS_INVALID_PARAMETER;
        goto done;
    }
    if (p->operation >= OPH_REGISTER && p->operation <= OPH_DISARM &&
        (!g_lab.registered || p->args[0] != g_lab.generation)) {
        p->status = STATUS_INVALID_HANDLE;
        goto done;
    }
    switch (p->operation) {
    case OPH_PING:
        p->args[0] = p->request_id ^ 0x4f5048494f4e5254ULL;
        break;
    case OPH_GET_VERSION:
        p->args[0] = OPH_VERSION;
        p->args[1] = OPH_FEATURE_ROOT_RPC | OPH_FEATURE_LAB_MEMORY |
            OPH_FEATURE_WRITE_ONESHOT | OPH_FEATURE_EVENTS;
        p->args[2] = g_cpu_count;
        p->args[3] = STEALTH_COMPENSATE_TIMING;
        p->args[4] = PAGE_SIZE;
        break;
    case OPH_GET_STATS:
        root_copy(p->data, (const UCHAR *)l->exits, sizeof(l->exits));
        p->args[0] = l->handler_ticks;
        p->args[1] = l->max_handler_ticks;
        p->args[2] = l->event_sequence;
        p->args[3] = l->armed;
        break;
    case OPH_REGISTER:
    case OPH_UNREGISTER:
        p->args[0] = g_lab.generation;
        p->args[1] = g_lab.page_pa;
        p->args[2] = PAGE_SIZE;
        break;
    case OPH_READ:
    case OPH_WRITE: {
        volatile UCHAR *window;
        if (!oph_range_valid(p->args[1], p->args[2])) {
            p->status = STATUS_INVALID_BUFFER_SIZE;
            break;
        }
        window = hostcr3_map_lab(v->core_id, g_lab.page_pa);
        if (!window) { p->status = STATUS_INVALID_ADDRESS; break; }
        if (p->operation == OPH_READ)
            root_copy(p->data, window + p->args[1], (SIZE_T)p->args[2]);
        else
            root_copy(window + p->args[1], p->data, (SIZE_T)p->args[2]);
        hostcr3_unmap_lab(v->core_id);
        break;
    }
    case OPH_TRANSLATE: {
        UINT64 leaf;
        if (p->args[1] >= PAGE_SIZE) { p->status = STATUS_INVALID_ADDRESS; break; }
        leaf = l->installed ? l->leaf->AsUInt : l->original_leaf;
        /* args[2:4] contain the non-root, owner-only page-walk result. */
        p->args[4] = g_lab.page_pa + p->args[1]; /* HPA (identity EPT) */
        p->args[5] = leaf & 7;
        p->args[6] = l->installed || !l->owns_split ? PAGE_SIZE : SIZE_2_MB;
        break;
    }
    case OPH_ARM_WRITE:
        if (!l->installed && l->owns_split)
            l->parent->AsUInt = l->split_pa | 7;
        l->leaf->AsUInt = l->original_leaf & ~2ULL;
        l->generation = g_lab.generation;
        l->installed = l->armed = TRUE;
        root_flush(v);
        break;
    case OPH_DISARM:
        root_disarm(v);
        break;
    case OPH_GET_EVENTS: {
        UINT64 cursor = p->args[0], first, count = 0, lost = 0;
        if (cursor > l->event_sequence) { p->status = STATUS_INVALID_PARAMETER; break; }
        first = l->event_sequence >= OPH_EVENT_CAPACITY ?
            l->event_sequence - OPH_EVENT_CAPACITY + 1 : 1;
        if (cursor < first - 1) { lost = first - 1 - cursor; cursor = first - 1; }
        while (cursor < l->event_sequence && count < OPH_EVENT_BATCH) {
            ++cursor;
            root_copy(p->data + count * sizeof(OPH_EVENT),
                (const UCHAR *)&l->events[(cursor - 1) % OPH_EVENT_CAPACITY], sizeof(OPH_EVENT));
            ++count;
        }
        p->args[0] = cursor; p->args[1] = count; p->args[2] = lost;
        break;
    }
    default: p->status = STATUS_INVALID_DEVICE_REQUEST; break;
    }
done:
    v->regs->rax = (UINT64)(LONG64)p->status;
}

/* Caller holds the session mutex at PASSIVE_LEVEL. Affinity plus IRQL prevents
   migration or reentry while the CPU-local mailbox is owned. */
NTSTATUS lab_exchange(OPH_PACKET *p)
{
    GROUP_AFFINITY affinity = {0}, previous;
    KIRQL old_irql;
    NTSTATUS status;
    LAB_CPU *l;
    UINT64 service;
    if (p->cpu >= g_cpu_count || KeQueryActiveGroupCount() != 1 ||
        KeQueryActiveProcessorCountEx(0) != g_cpu_count) return STATUS_NOT_SUPPORTED;
    affinity.Group = 0;
    affinity.Mask = (KAFFINITY)1 << p->cpu;
    KeSetSystemGroupAffinityThread(&affinity, &previous);
    KeRaiseIrql(DISPATCH_LEVEL, &old_irql);
    l = g_vcpu[p->cpu].lab;
    RtlCopyMemory(&l->mailbox, p, sizeof(*p));
    l->mailbox.root_sequence = 0;
    l->mailbox.root_tsc = 0;
    _InterlockedExchange(&l->pending, 1);
    service = p->operation == OPH_PING ? VMCALL_PING :
        p->operation == OPH_GET_VERSION ? VMCALL_GET_VERSION : VMCALL_OPH_EXCHANGE;
    status = asm_vmx_vmcall(service, OPH_VERSION, 0, 0);
    RtlCopyMemory(p, &l->mailbox, sizeof(*p));
    _InterlockedExchange(&l->pending, 0);
    KeLowerIrql(old_irql);
    KeRevertToUserGroupAffinityThread(&previous);
    if (NT_SUCCESS(status) && !p->root_sequence) return STATUS_DEVICE_PROTOCOL_ERROR;
    return status;
}

static NTSTATUS all_cpus(OPH_PACKET *p)
{
    OPH_PACKET request = *p, reply;
    NTSTATUS status = STATUS_SUCCESS;
    for (UINT32 i = 0; i < g_cpu_count; ++i) {
        reply = request; reply.cpu = i;
        status = lab_exchange(&reply);
        if (!NT_SUCCESS(status)) break;
        if (i == request.cpu) *p = reply;
    }
    return status;
}

static NTSTATUS unregister_page(OPH_PACKET *p)
{
    OPH_PACKET stop = *p;
    NTSTATUS status;
    stop.operation = OPH_DISARM;
    stop.args[0] = g_lab.generation;
    status = all_cpus(&stop);
    if (!NT_SUCCESS(status)) return status; /* Keep MDL pinned until every CPU acknowledges. */
    p->operation = OPH_UNREGISTER;
    p->args[0] = g_lab.generation;
    status = lab_exchange(p);
    if (!NT_SUCCESS(status)) return status;
    _InterlockedExchange(&g_lab.registered, 0);
    MmUnlockPages(g_lab.mdl);
    IoFreeMdl(g_lab.mdl);
    g_lab.mdl = NULL;
    g_lab.user_va = g_lab.page_pa = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS register_page(OPH_PACKET *p)
{
    UINT64 va = p->args[0];
    PMDL mdl;
    NTSTATUS status = STATUS_SUCCESS;
    if (g_lab.registered) return STATUS_DEVICE_BUSY;
    if (p->args[1] != PAGE_SIZE || !va || (va & (PAGE_SIZE - 1)) ||
        va > (UINT64)MmHighestUserAddress - (PAGE_SIZE - 1)) return STATUS_INVALID_PARAMETER;
    mdl = IoAllocateMdl((PVOID)va, PAGE_SIZE, FALSE, FALSE, NULL);
    if (!mdl) return STATUS_INSUFFICIENT_RESOURCES;
    __try { MmProbeAndLockPages(mdl, UserMode, IoModifyAccess); }
    __except(EXCEPTION_EXECUTE_HANDLER) { status = GetExceptionCode(); }
    if (!NT_SUCCESS(status)) { IoFreeMdl(mdl); return status; }
    g_lab.page_pa = (UINT64)MmGetMdlPfnArray(mdl)[0] << PAGE_SHIFT;
    if (g_lab.page_pa >= (1ULL << 39) ||
        ept_get_memory_type(g_lab.page_pa >> PAGE_SHIFT, FALSE) != MEMORY_TYPE_WRITE_BACK) {
        MmUnlockPages(mdl); IoFreeMdl(mdl); return STATUS_NOT_SUPPORTED;
    }
    for (UINT32 i = 0; i < g_cpu_count; ++i) {
        LAB_CPU *l = g_vcpu[i].lab;
        l->parent = ept_get_pml2(g_vcpu[i].ept_page_table, g_lab.page_pa);
        l->original_parent = l->parent->AsUInt;
        l->owns_split = l->parent->LargePage ? TRUE : FALSE;
        if (l->owns_split) {
            UINT64 base = g_lab.page_pa & ~(SIZE_2_MB - 1ULL);
            for (UINT32 j = 0; j < 512; ++j) {
                l->split->PML1[j].AsUInt = (base + (UINT64)j * PAGE_SIZE) |
                    (l->original_parent & 0x7f); /* preserve R/W/X, cache type, ignore-PAT */
            }
            l->leaf = &l->split->PML1[ADDRMASK_EPT_PML1_INDEX(g_lab.page_pa)];
        } else l->leaf = ept_get_pml1(g_vcpu[i].ept_page_table, g_lab.page_pa);
        if (!l->leaf) { status = STATUS_INVALID_ADDRESS; break; }
        l->original_leaf = l->leaf->AsUInt;
        l->page_pa = g_lab.page_pa;
        l->installed = l->armed = FALSE;
    }
    if (!NT_SUCCESS(status)) { MmUnlockPages(mdl); IoFreeMdl(mdl); return status; }
    g_lab.mdl = mdl; g_lab.user_va = va;
    ++g_lab.generation;
    if (!g_lab.generation) ++g_lab.generation;
    _InterlockedExchange(&g_lab.registered, 1);
    p->args[0] = g_lab.generation;
    status = lab_exchange(p);
    if (!NT_SUCCESS(status)) {
        _InterlockedExchange(&g_lab.registered, 0);
        MmUnlockPages(mdl); IoFreeMdl(mdl); g_lab.mdl = NULL;
    }
    return status;
}

static int read_pte(void *context, uint64_t pa, uint64_t *entry)
{
    MM_COPY_ADDRESS address;
    SIZE_T bytes = 0;
    UNREFERENCED_PARAMETER(context);
    address.PhysicalAddress.QuadPart = pa;
    return NT_SUCCESS(MmCopyMemory(entry, address, sizeof(*entry),
        MM_COPY_MEMORY_PHYSICAL, &bytes)) && bytes == sizeof(*entry);
}

VOID lab_initialize(VOID) { KeInitializeMutex(&g_lab.mutex, 0); }

NTSTATUS lab_open(PFILE_OBJECT file)
{
    NTSTATUS status = STATUS_SUCCESS;
    KeWaitForSingleObject(&g_lab.mutex, Executive, KernelMode, FALSE, NULL);
    if (g_lab.file) status = STATUS_SHARING_VIOLATION;
    else {
        g_lab.file = file; g_lab.owner = PsGetCurrentProcess();
        ObReferenceObject(g_lab.owner);
    }
    KeReleaseMutex(&g_lab.mutex, FALSE);
    return status;
}

VOID lab_close(PFILE_OBJECT file)
{
    KeWaitForSingleObject(&g_lab.mutex, Executive, KernelMode, FALSE, NULL);
    if (g_lab.file == file) {
        if (g_lab.registered) {
            OPH_PACKET p = {0};
            p.magic = OPH_MAGIC; p.version = OPH_VERSION; p.size = sizeof(p);
            if (!NT_SUCCESS(unregister_page(&p))) {
                /* Continuing cleanup would release a PFN still referenced by EPT. */
                KeBugCheckEx(0x20001, 0x4f5048, 1, 0, 0);
            }
        }
        ObDereferenceObject(g_lab.owner);
        g_lab.owner = NULL; g_lab.file = NULL;
    }
    KeReleaseMutex(&g_lab.mutex, FALSE);
}

NTSTATUS lab_ioctl(PFILE_OBJECT file, OPH_PACKET *p)
{
    NTSTATUS status;
    KeWaitForSingleObject(&g_lab.mutex, Executive, KernelMode, FALSE, NULL);
    if (file != g_lab.file || PsGetCurrentProcess() != g_lab.owner) {
        status = STATUS_ACCESS_DENIED; goto out;
    }
    if (!oph_packet_valid(p) || p->cpu >= g_cpu_count) {
        status = STATUS_INVALID_PARAMETER; goto out;
    }
    if (p->operation == OPH_REGISTER) { status = register_page(p); goto out; }
    if (p->operation >= OPH_UNREGISTER && p->operation <= OPH_DISARM &&
        (!g_lab.registered || p->args[0] != g_lab.generation)) {
        status = STATUS_INVALID_HANDLE; goto out;
    }
    if (p->operation == OPH_UNREGISTER) { status = unregister_page(p); goto out; }
    if (p->operation == OPH_TRANSLATE) {
        OPH_TRANSLATION result;
        int cpuid[4];
        if (p->args[1] >= PAGE_SIZE) { status = STATUS_INVALID_ADDRESS; goto out; }
        __cpuid(cpuid, (int)0x80000008);
        if (!oph_walk4(__readcr3(), g_lab.user_va + p->args[1], cpuid[0] & 255,
            read_pte, NULL, &result) || (result.physical & ~0xfffULL) != g_lab.page_pa) {
            status = STATUS_INVALID_ADDRESS; goto out;
        }
        p->args[2] = result.physical; p->args[3] = result.page_size;
    }
    if (p->operation == OPH_ARM_WRITE || p->operation == OPH_DISARM) {
        status = all_cpus(p);
        if (!NT_SUCCESS(status) && p->operation == OPH_ARM_WRITE) {
            OPH_PACKET stop = *p; stop.operation = OPH_DISARM;
            if (!NT_SUCCESS(all_cpus(&stop)))
                KeBugCheckEx(0x20001, 0x4f5048, 2, 0, 0);
        }
    } else status = lab_exchange(p);
out:
    p->status = status;
    KeReleaseMutex(&g_lab.mutex, FALSE);
    return status;
}
