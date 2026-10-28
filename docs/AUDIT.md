# Baseline audit and compatibility evidence

Audited baseline: 7b501992bd71af7f5330ec697a1cbe27921157f9.
The initial source has 12 C files, 7 MASM files, 5 headers; 5,884 physical lines.
Core execution path: DriverEntry -> VMX init/EPT allocation -> DPC -> VMXON ->
VMCS -> VMLAUNCH; exits enter assembly, C dispatch, then VMRESUME. Baseline
VMCALL services are TEST and VMXOFF; baseline IOCTL only returns CPU count.

VMCS requests TSC offsetting, MSR/I/O bitmaps, secondary controls, EPT, VPID,
RDTSCP, INVPCID, XSAVES and user-wait/pause when hardware permits. NMI exiting
and virtual NMIs are requested; external-interrupt exiting is not explicitly
requested, despite the README table. Controls are adjusted by capability MSRs,
so the effective controls must be captured on each tested processor. Exception
bitmap defaults to zero. CPUID/VMCALL and architectural exits are handled;
CR3/DR/HLT/INVLPG/RDTSC can also be forced by capability constraints. I/O bitmaps
are zero. MSR bitmap intercepts TSC reads and FEATURE_CONTROL/VMX capability
MSRs. Host uses private CR3/IDT/GDT; guest preserves Windows mappings.

## Findings addressed in this change

| Finding | Change | Verification required beyond source review |
|---|---|---|
| Partial per-CPU launch failure freed live resources | Roll back launched CPUs before freeing; termination skips unlaunched CPUs | Forced launch failure on one CPU |
| VMCS setup result ignored; VMX failure left control registers changed | Check result, central VMXOFF failure path, restore prelaunch CR0/CR4 | VMXON/VMCLEAR/VMPTRLD/VMLAUNCH fault injection |
| GDT allocated after host snapshot | Per-CPU preparation before snapshot/VMXON | Mapping and unload comparison |
| NMI RDMSR clobbered RDX | Save/restore RDX, bound index | Debugger-assisted NMI register sentinel |
| IDTR restoration used one global capture | Per-CPU original IDTR and limit | Per-CPU load/unload state |
| C VMRESUME wrapper could alter restored GPRs | VMRESUME directly in assembly; non-returning failure path | Inspect generated binary and stress |
| MTRR overlap/large-page boundary errors | Pure tested overlap resolver, actual initialization splits | Physical MTRR map audit on target |
| Split allocations not tracked | List ownership and shutdown release | Pool tracking across repeated cycles |
| Host page clone silently reused original tables on failure | Fail instead of sharing fallback; cap allocation before allocating | Allocation failure injection |
| EPROCESS +0x28 dependency | Attach to system process and read CR3 during initialization | WDK build and target validation |
| Processor group count/index mismatch | Reject multi-group topology; group-aware affinity and count | Multi-group rejection test |
| AVX-512 XCR0 dependency check accepted SSE-only | Require both SSE and AVX bits | Target XSETBV exception tests |
| Device accepted broad access | Secure ACL, exclusive owner/file binding, versioned bounded ABI | Standard-user/duplicate-handle tests |

## Residual architectural limits

Private CR3 is a snapshot of kernel mappings, not an independent memory ownership
boundary. Leaf physical pages remain shared. Snapshot construction has no general
Windows page-table freeze API, and later changes to kernel mappings are not
automatically reflected. New lab mappings avoid dynamic kernel VA dependencies
through static temporary slots, but the broader host snapshot design remains a
hardware-validation risk. The host clone has a 4,096-page allocation ceiling and
fails initialization on exhaustion. Identity EPT covers only the first 512 GiB.

The baseline is not a complete architectural emulator. Unhandled exit defaults,
triple-fault behavior, debug-register corner cases, event collisions and full
power-management restoration need further review/testing. Host #PF/#GP/#DF
handlers halt, so a root exception is fatal. Existing CPUID/feature presentation
behavior is inherited and is not evidence of compatibility with any security
product. No effort in this change is directed at hiding from anti-cheat or
integrity mechanisms. The lab does not add arbitrary read/write, split physical
views, DLL injection, module hiding, or security-setting modification.

## Reference provenance

The requested community repository is contextual material, not a vendor
specification or proof of a detector's behavior. Its timing document explicitly
describes a 2024-2025 landscape; a label such as "Confirmed" lacks a reproducible
test for this build. Current EAC/BE compatibility remains unverified.

Requested sources:
- https://github.com/RejiDev/game-hacking-guidelines/blob/master/anti-cheat/eac.md
- https://github.com/RejiDev/game-hacking-guidelines/blob/master/anti-cheat/detection-timing.md
- https://github.com/RejiDev/game-hacking-guidelines/blob/master/techniques/hypervisors.md
- https://github.com/RejiDev/game-hacking-guidelines/blob/master/techniques/memory-access.md
- https://github.com/RejiDev/game-hacking-guidelines/blob/master/techniques/kernel-drivers.md
- https://github.com/RejiDev/game-hacking-guidelines/blob/master/techniques/driver-safety.md

All six requested reference files and https://zer0condition.github.io/Ophion/
were retrieved on 2026-10-27 after initial cache/access failures. The published
Ophion page repeats the README timing-default and compatibility claims; it is
not independent runtime evidence. Retrieved-byte SHA-256 identifiers are recorded
in VALIDATION.md because these branch/website URLs are mutable.

The community material is unsuitable as an implementation specification. Its
memory example omits a declared byte-count variable, uses KernelMode when
handling addresses, and does not establish buffer ownership. Its driver-safety
SEH advice cannot recover faults under Ophion's private root IDT. Claims about
universal detection, exact enforcement delays and kernel visibility lack a
versioned, reproducible experiment. No such claim is used as an acceptance test.

Authoritative technical contracts used:
- Intel SDM (VMX, EPT, MTRR, instruction semantics): https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html
- MTRR overlap rules: https://cdrdv2-public.intel.com/874249/253668-090-sdm-vol-3a.pdf
- MDL locking: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-mmprobeandlockpages
- Safe physical copy: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddk/nf-ntddk-mmcopymemory
- Secured device: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdmsec/nf-wdmsec-wdmlibiocreatedevicesecure
- Processor count: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-kequeryactiveprocessorcountex

Compatibility must record CPU model/microcode, Windows build, BIOS settings,
Secure Boot, VBS/HVCI state, signing policy and other installed hypervisors.
An existing hypervisor causes this implementation to refuse initialization;
security products are neither disabled nor modified. Signed-driver admission is
separate from correct execution and from vendor approval/compatibility.
