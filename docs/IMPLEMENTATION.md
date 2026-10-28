# Ophion laboratory instrumentation, protocol v1

Baseline: `7b501992bd71af7f5330ec697a1cbe27921157f9` (2026-03-20).
This document describes source changes, not a claim of hardware validation.
See `VALIDATION.md` for executed checks and outstanding hardware acceptance.

## Supported scope

The driver exposes one exclusive, administrator/SYSTEM-only device. A session is
bound to both its file object and opening process. Requests from a duplicated
handle in another process are rejected. The x64 client supplies one committed,
page-aligned, writable 4 KiB page. Registration probes that user page in the
owner's process, locks it with an MDL, and assigns a monotonically increasing
generation. No API accepts a PID, CR3, physical address, arbitrary kernel address,
code payload, executable callback, or remote process handle.

Version 1 intentionally supports one registered page and one processor group
(up to 64 logical processors). It rejects existing hypervisors, LA57, unsupported
EPT/INVVPID capabilities and mismatched TSC_AUX processor numbering at startup.
It does not configure Secure Boot, HVCI, VBS, driver signing, or other protections.
CPU hotplug and suspend/resume are not supported. Use a dedicated lab boot; do
not infer power-transition support from a successful load/unload test.

## Request path and root evidence

`C++ client -> DeviceIoControl -> secured buffered IOCTL -> pinned CPU mailbox ->
VMCALL -> VM-exit dispatcher -> root service -> mailbox -> user response`.

`include/ophion_protocol.h` is shared by C and C++. Pack alignment is 8, the packet
is 1,136 bytes, the data offset is 112, and an event is 48 bytes. All wire fields
are fixed width. Both IOCTL buffer lengths must exactly match the v1 packet size.
Bad version, command, size, CPU, generation, alignment and range are rejected.
The IOCTL requires read AND write handle access. The legacy unrestricted status
IOCTL is retired; use GET_VERSION or GET_STATS. Unsupported major functions and
device subpaths fail explicitly. All dispatch work requires PASSIVE_LEVEL.

Each CPU owns a nonpaged mailbox allocated before host CR3 construction. The
driver serializes requests with a KMUTEX, pins the thread to the selected CPU,
raises IRQL to DISPATCH_LEVEL, publishes the mailbox and invokes VMCALL. Root
consumes a pending flag and validates the message again. It never dereferences
a caller-supplied pointer. The PING and GET_VERSION VMCALL identifiers are
separate; all services share the same versioned packet validation.

Root increments a CPU-local sequence, records raw TSC and echoes request_id.
PING additionally transforms request_id using the documented fixed nonce mask.
The client checks packet size/version, CPU, request correlation, root sequence
progression and the PING nonce. These establish a functional root round trip in
this implementation; they are not cryptographic attestation against a hostile
kernel. The actual VMCALL/EPT exit counters provide independent path evidence in
the hardware test. No success response is synthesized solely in the IOCTL path.

## Protocol operation arguments

Unused fields must be initialized to zero by clients. `cpu` selects the queried
CPU; ARM/DISARM additionally visit every active CPU before returning success.

| Operation | Input args | Output |
|---|---|---|
| PING | none | args[0] = request_id XOR nonce mask |
| GET_VERSION | none | version, features, CPU count, timing flag, page size |
| GET_STATS | none | raw handler ticks, max ticks, event sequence, armed state; data = 65 counters |
| REGISTER | [0] owner user VA, [1] 4096 | generation, page GPA, length |
| UNREGISTER | [0] generation | acknowledged removal, then MDL release |
| READ / WRITE | [0] generation, [1] offset, [2] length | up to 1024 bytes in data |
| TRANSLATE | [0] generation, [1] offset | [2] GPA, [3] guest page size, [4] HPA, [5] EPT RWX, [6] EPT leaf size |
| ARM_WRITE / DISARM | [0] generation | all-CPU completion |
| GET_EVENTS | [0] last consumed sequence | cursor, count, overwritten count; data = up to 16 events |

NTSTATUS is returned through normal IRP completion and recorded in validated
packets. On failed DeviceIoControl, clients use GetLastError; they must not assume
Windows copied a failed response packet. Exact NTSTATUS can be observed in a
kernel debugger. Counter bucket 64 aggregates exit reasons >=64. Handler tick
statistics exclude assembly entry/restore and the current unfinished exit.

## Memory translation and temporary mapping

Guest virtual -> guest paging -> GPA -> EPT -> HPA is distinct from host virtual
-> private host CR3 -> HPA. Identity EPT means GPA=HPA here, not VA=PA.
`ophion_paging.h` implements a checked four-level walker with 4 KiB, 2 MiB and
1 GiB terminal leaves, canonical-address and large-page reserved-bit checks,
plus accumulated user/write/execute permissions. The driver supplies a safe
MmCopyMemory reader at PASSIVE_LEVEL. TRANSLATE can walk only the registered VA
plus a page-bounded offset and verifies the resulting PFN against the locked MDL.
If the process remapped its original VA, translation fails rather than following
the replacement mapping. Registration remains bound to the original pinned PFN.

Root accesses that pinned PFN through a private supervisor, NX temporary slot.
Three page-aligned static paging arrays are connected at private PML4 slot 255;
they are never installed in Windows/guest CR3. Each CPU has its own 4 KiB slot.
Root sets only its own PTE, executes local INVLPG, performs a bounded volatile
byte copy, clears the PTE, and executes INVLPG again. Only WB RAM below the EPT
coverage limit can register. There are no allocations, waits, physical-reader
APIs, or user pointer dereferences in this root path.

READ/WRITE are not atomic transactions against simultaneous application access.
The laboratory application must quiesce writers when validating a byte snapshot.
Root copies bypass guest EPT and therefore do not generate guest EPT write events.

## EPT observation, synchronization and cleanup

Initialization now splits MTRR boundary-crossing 2 MiB leaves, tracks allocations
and frees them on failure/unload. MTRR end addresses are inclusive; overlap
resolution follows UC precedence and WT+WB=WT. Unsupported overlap combinations
conservatively select UC. Invalid zero masks and range-array overflow fail setup.

For runtime observation each CPU has a preallocated split table. Registration
prepares it without changing active translations. ARM_WRITE, running on each
CPU in root, installs that CPU's table when needed, clears W on exactly the
registered leaf and performs single-context INVEPT. Existing 4 KiB leaves are
reused. R and X are never swapped between different physical views.

On the first matching write violation per CPU, root records sequence/TSC/CPU/
generation/page offset/qualification, restores original leaf permissions,
invalidates EPT translations and retries the instruction without advancing RIP.
The watch is now consumed on that CPU. A second write generates no event until
rearmed. The test performs two writes on every CPU and requires exactly one
event per CPU. EPT is physical-page based: another alias of that PFN on the same
CPU can consume the watch; this is not instruction- or process-filtered tracing.

ARM/DISARM completion is a rendezvous implemented by synchronous affinity-bound
calls. Changes are acknowledged on every CPU, but are not globally simultaneous.
The application must quiesce page accesses during transitions. On partial ARM
failure the driver disarms every CPU. DISARM restores the original leaf and,
where owned, the original large-page parent, then INVEPTs before acknowledging.
Unregistration/CLEANUP unlocks the MDL only after every CPU acknowledges removal.
Tables remain allocated until all processors have left VMX during driver unload.

An unexpected EPT fault is logged without disclosing unrelated physical
addresses, then follows the existing guest #GP path. It is not silently granted
access or retried forever. INVEPT failure after a validated capability check is
fatal: root stops that CPU rather than resuming with stale mappings. A cleanup
rendezvous failure is a bugcheck rather than freeing a still-referenced PFN.
These paths require crash/debugger testing; they are not recovery guarantees.

## MTF and timing

MTF requests a VM exit after guest instruction execution, allowing a monitor to
restore permissions after a temporary access grant. A robust continuous monitor
must coordinate MTF with faults, interrupts, repeated/string instructions and
cross-CPU state. Each observed operation can add an EPT exit, an MTF exit and
translation invalidations. Cache/TLB pressure and scheduling noise affect cost.
Version 1 uses explicitly one-shot observations and does NOT enable MTF; it
does not pretend to provide a continuous instruction trace. INVEPT invalidates
EPT-derived translations; INVLPG on a host temporary VA is a different operation.
INVVPID manages guest linear translations and is not a substitute for INVEPT.

`STEALTH_COMPENSATE_TIMING` remains **0**. Git history shows this value already in
the initial `eeee734d89628777649f279a60ca56dc86214e7c` commit (2026-03-14). There
is no commit message establishing why the author disabled it. README said 1;
this is documentation drift, not evidence of a validated alternative default.

Enabling it would compile the CPUID-triggered next-RDTSC/RDTSCP trap, the
calibration and forced-exit checks. Calibration currently lives inside CPUID
cache initialization; the feature switches are not independent. The calibration
uses a minimum sample on the initializing CPU and is shared across cores.
Changing returned timestamps is distinct from measuring actual work and would
complicate diagnostic interpretation. This implementation records raw host TSC
and user round-trip QPC/steady-clock latency without enabling compensation.
Raw TSC ticks must not be labeled wall-clock nanoseconds or CPU core cycles.

## Desktop application and build

The reusable RAII client is in `client/`. `ophion_lab_test` performs the hardware
acceptance checks. `ophion_console` is a standalone normal Win32 window using
Dear ImGui and DX11, with keyboard navigation, explicit connection/error state,
CPU selection, lab-page controls, timing plot and an event table. Requests run
on one asynchronous worker so the render loop is not blocked by DeviceIoControl.
It neither injects into applications nor hooks a graphics pipeline.

Client/tests (Visual Studio 2022 x64 or another supported Windows C++ toolchain):

```powershell
cmake -S . -B build/client -A x64
cmake --build build/client --config Release
ctest --test-dir build/client -C Release --output-on-failure
```

Dear ImGui is pinned to commit `dbb5eeaadffb6a3ba6a60de1290312e5802dba5a`
(v1.91.8). Configure `-DOPHION_BUILD_GUI=OFF` for client/CLI/core tests only.
Offline builds can use `-DFETCHCONTENT_SOURCE_DIR_IMGUI=<local-imgui-directory>`.
Third-party source stays under build/_deps; its upstream license accompanies it.

Driver: Visual Studio 2022, MSVC/MASM and WDK/SDK 10.0.26100.0 are required.
`MSBuild Ophion.sln /p:Configuration=Release /p:Platform=x64 /p:SignMode=Off`
builds the unsigned laboratory artifact. MASM imports and Wdmsec linkage are
explicit; the original machine-specific certificate was removed. Signing is an
external lab/deployment prerequisite, not a protection-disabling step in this
project. No build/test script installs or starts the driver or changes boot flags.
