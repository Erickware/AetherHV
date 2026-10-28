# Validation record

Recorded 2026-10-28. Baseline commit:
`7b501992bd71af7f5330ec697a1cbe27921157f9`.
Results apply to the uncommitted working-tree implementation, not an upstream
release. The full hardware acceptance criteria have **not** been met.

## Executed checks

| Check | Observed result |
|---|---|
| Portable C tests, Zig 0.14.1, C17, `-Wall -Wextra -Werror` | PASS: 4,290 ABI, bounds, MTRR and paging checks |
| C++ client and laboratory CLI, C++17, `-Wall -Wextra -Werror` | Compile/link succeeded, exit 0 |
| Standalone ImGui/DX11 console, pinned ImGui v1.91.8 | Compile/link succeeded, exit 0 |
| CLI without installed Ophion device | `FAIL: Open Ophion: No such file or directory`, exit 1; no false PASS |
| Console launch | Window titled `Ophion | Laboratory Diagnostics` was enumerated |
| Console visual/interaction checks | Incomplete: user stopped Computer Use with Escape before screenshot inspection |
| Git whitespace check | `git diff --check` returned 0 at the reviewed checkpoint |
| Driver WDK/MSVC/MASM build | Not run: tools absent from PATH and requested WDK header installation not found |
| Driver load, root RPC, memory and EPT acceptance | Not run: no driver installed/loaded in this session |
| Hosted CI | Workflow added; no remote run claimed |

Local tools were installed only under ignored `build/tools`. The portable test
binary was compiled directly with `zig cc`; clients with `zig c++`. CMake and
Visual Studio project paths are provided but were not exercised here. For the
MinGW import libraries, the console uses `d3dcompiler_47`; MSVC uses
`d3dcompiler`. Zig emitted an unused `-mwindows` compilation warning during the
GUI build; linking and window creation succeeded. The compiler's C++ acceptance
does not validate WDK types, MASM syntax or VMX architectural correctness.

The first validation attempt was interrupted by automatic approval review's
usage limit. It was a review-service failure, not an unsafe-action finding.
Compilation was resumed after the user's continuation request.

## Hardware acceptance still required

Use a separately provisioned lab target and record CPU model/microcode, Windows
build, firmware version, topology, security state, driver binary SHA-256, PDB and
effective VMCS controls. Retain all existing security settings. Signed-driver
admission and VMX availability are prerequisites, not proven properties of this
change. Existing Hyper-V/VBS is rejected rather than reconfigured.

1. Build Debug and Release with WDK/SDK 10.0.26100.0 and MSVC/MASM. Inspect
   compiler diagnostics and VM-entry assembly. Complete static driver analysis.
2. With the lab driver admitted by the target's existing signing policy, run
   `ophion_lab_test`. It checks root nonce/sequence, per-CPU VMCALL counters,
   owner-buffer write/read/translation, exactly one first-write EPT event per CPU
   despite two writes, and malformed size/version/opcode/CPU/generation/ranges.
   It reports median/p95/p99 for 300 PING round trips. No timing values have
   been measured on hardware in this session.
3. Verify device open is denied to a standard user; duplicate a valid handle
   into another process and verify requests fail. Validate inaccessible,
   misaligned, read-only and decommitted registration addresses. These ownership
   and ACL cases require additional execution beyond the current CLI.
4. Exercise explicit unregister, abrupt client termination, repeated sessions,
   repeated driver load/unload, concurrent callers and event-ring overflow.
   Check MDL/pool lifetime and restoration of leaf and parent EPT entries.
5. Force initialization allocation failures and one-CPU launch failure. Verify
   every launched CPU leaves VMX before any table or stack is freed. Validate
   per-CPU IDTR/GDTR/TR, CR0/CR4, debug state and SIMD state across transitions.
6. Verify host temporary PTEs are cleared after every root read/write. Validate
   MTRR boundary splits against the target's actual physical map and check that
   each processor acknowledges INVEPT before the registered PFN is unlocked.
7. Test sustained activity and NMI delivery with debugger/crash collection.
   CPU hotplug and suspend/resume remain unsupported; this revision is not a
   production-ready power-management implementation. Root exception/INVEPT
   failure paths halt, and failed cleanup rendezvous deliberately bugchecks.

Passing the portable suite cannot establish any of these hardware results.
No EAC/BE/other security-product compatibility or nondetection claim is made.

## Retrieved reference snapshots

Retrieved on 2026-10-27. Hashes identify actual downloaded bytes, not a guessed
upstream commit. Files are local ignored research inputs in `build/references`.
The six Markdown files came from the requested repository's `master` branch.

| File | SHA-256 |
|---|---|
| Ophion published documentation HTML (`0`) | `032E76C24F0F00C42A1B571C72043856B255177391A75E985E6407EDD9B1974C` |
| anti-cheat/eac.md (`1`) | `5D22BABFC2C9CCB9FC3BA9AE3923FA194C8269C0640B26DAA71C5E188B1AF645` |
| anti-cheat/detection-timing.md (`2`) | `C1B523ED6CC0F6C2E3288B9796F812C6F51F7EE0D98BB13C027AF7354BE08640` |
| techniques/hypervisors.md (`3`) | `E971EE41BD2A25360DFA9808A2C4FEF5F205F7284D091A0566FAA1C51C3C2856` |
| techniques/memory-access.md (`4`) | `6FE2EB0334BA2EFA9504D7F7D68704B355FC3B4547A0D22B65319BA487C25285` |
| techniques/kernel-drivers.md (`5`) | `2A117C6C136706E709B12CD4CE1227D6690C97CE00858F0350FFCE8604636C90` |
| techniques/driver-safety.md (`6`) | `2BD9444CE8446656986FE9EF4CA27B91360C4E16FD77D687238D290629460820` |
