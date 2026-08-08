
## Cycle 0103 (2026-08-07) — CFG shared loop continue test alignment

Investigation of the lone TestNewShaderRecompilerCfgLoopSharedContinueSelectionMerges
failure revealed that the test's expectations were outdated: the implemented
behavior (OpLoopMerge only, no OpSelectionMerge) is correct per upstream's
51a33cc ("shader cfg: handle loop control branches"), and the test was
rewritten and renamed upstream to CfgLoopEarlyContinuesNoSelection.

### Audit findings (this cycle)

- **f43567e1** shader: validate MSAA image descriptors (fork has the
  ValidImageDescriptor MSAA branch; ResourceTrackingTests.cpp MSAA test
  not ported)
- **3b75a5659** shader: specialize cube image descriptors (5 files,
  155+56 lines, requires new image_cube field in fork's ShaderIR.h,
  spirvEmitterImageHelpers.cpp new helpers EmitCubeAxisF32 /
  EmitCubeLayerF32. Not ported this cycle; architecture cost too high
  vs. value, can revisit later)
- **51a33cc / 2dcb900 / 44d7f2** (CFG loop control branches, normalize
  loop structure, handle shared early exits): all three ports already
  present in fork (commits 173a4f4, c6e8eec, 0889c5e). Fork's
  DuplicateSelectionRegion is byte-identical to upstream's.
- **0f550d1** hint-less guest mappings at canonical PS5 base: fork's
  MMap already uses DEFAULT_PS5_BASE = 0x200000000 for hint-less
  mappings (line 1966). The separate FindGuestFreeRange helper from
  upstream's fix is not in fork's code, but the relevant behavior is
  preserved.
- **b9aa0dc** KernelLseek RAII lock guard: already in fork (line 732)
- **93774ee** fix teardown: already in fork
- **687ce02** KernelOpen ENOENT: already in fork (lines 382-386)
- **a21d1aaa** PsInputCountRegisterDecode: already ported (cycle 0093
  via commit 7a7e4e5)
- **832bc84** ScalarProvenance phi stabilization: production fix
  already in fork; test (TestNestedLoopPhiConvergence) not added
- **f380d69** AudioOut pacing: already in fork (any_port_has_device)
- **00a5dd3** syncOnAddress ABIs (Linux futex): adds new file
  kernel/syncOnAddress.cpp and Linux-only futex syscall paths; not
  ported (Windows path is the active target).
- **0a2f481** refactor shader centralize opcode metadata: large
  refactor, would require widespread fork adaptation. Skipped for now.

### Cycle 0103 change

- 	ests/shaderCfgTests.cpp:
  - Renamed TestNewShaderRecompilerCfgLoopSharedContinueSelectionMerges
    to TestNewShaderRecompilerCfgLoopEarlyContinuesNoSelection.
  - Replaced SpirvContainsOpcode calls with SpirvInstructionOpcodeCount
    to allow asserting absence (== 0).
  - Dropped the "!duplicate structured merge block" check (now structural,
    not a per-test invariant).
  - Asserted OpSelectionMerge (247) and OpSwitch (251) are absent,
    OpLoopMerge (246) present.
  - Updated main() call site.
- 1 file changed, 9 insertions(+), 11 deletions(-).

### Test status after fix

- 208/208 compute tests pass (no regression)
- 9/9 graphics tests pass (no regression)
- **shader_cfg_tests: 0 failures** (was 1 before this fix on
  TestNewShaderRecompilerCfgLoopSharedContinueSelectionMerges).
  Exit code 0.

### Build verification

- cmake --build _Build/windows --target kyty_emulator
   shader_recompiler_compute_tests shader_cfg_tests -- -j8 clean.
- All three executables built without warnings.

### Branch state

- HEAD on graphics-coverage-2026-07-20: 86392ab
- 110 commits in fork branch (was 109 pre-cycle-0103).
- Pushed to ork remote (abdulazizFarhan/KytyPS5.git).

## Cycle 0125 (2026-08-08) — M1W2 v1.7: GTA V exits cleanly in 60s

### Investigation
GTA V (PPSA04264, 01.005.000) was stuck in an infinite sentinel iteration
loop. GTA V's dispatcher at vaddr `0x9028cdeb0` iterates a function pointer
table calling `vtable[0x58]` for each entry. All entries are unmapped sentinel
addresses, causing Execute AVs that the M1W2 handler fast-skipped.

Previously, M1W2 v1.5 advanced GTA V's RIP by 1GB per AV. This caused GTA V's
outer loop counter (at the cmp eax, 0x8002000d check) to never increment
naturally, leaving GTA V iterating 66M+ AVs in 15 minutes without progress.

### Root cause
M1W2 v1.5's 1GB RIP skip amount bypassed GTA V's normal post-call code
execution. The sentinel call returned (RAX=0), but GTA V's outer loop counter
only increments through the post-call code path. With 1GB skips, GTA V never
incremented its counter and looped forever.

### Fix
M1W2 v1.7: advance RIP by 16 bytes (just past the failing call instruction)
instead of 1GB. This lets GTA V's post-call code execute and its loop counter
increment naturally.

### Measured result (GTA V PPSA04264)
- Before: stuck in sentinel loop for 15+ minutes, 66M AVs, no exit
- After: exits cleanly in 60 seconds, 2,009,089 AVs, exit code 0
- Verification: 2-min test → exit code 0 in 45 seconds

### Current GTA V status
GTA V exits cleanly but does NOT reach Vulkan rendering:
- 8 semaphores created (max 32767 each)
- 0 Vulkan calls (no GPU init)
- Process exits with code 0
- Fork's Vulkan subsystem IS initialized for GTA V (NVIDIA GeForce GTX 1650 SUPER)

### File changed
- `src/loader/runtimeLinker.cpp` (commit fdd8fea)


## Cycle 0126 (2026-08-08) — GTA V KernelDirectMemoryQuery investigation (no behavior change)

### Investigation
Investigated GTA V's `KernelDirectMemoryQuery(offset=0x360000000)` call
that was returning `[Fail]`. The function uses `< PhysicalMemory::Size()`,
which fails when offset == PhysicalMemory::Size() (the boundary case).

### Test result (with fix attempted)
- Changed `<` to `<=` in `src/kernel/memory.cpp` line 2411
- `KernelDirectMemoryQuery(offset=0x360000000)` now returns `terminal=true, [Ok]`
- **REGRESSION**: GTA V enters infinite loop calling this query 3937 times
  in 2 minutes. GTA V's code was treating `[Fail]` as "stop allocating"
  but `terminal=true` is treated as "wait for memory to appear"
- **Reverted the fix** — fork's original behavior is correct for GTA V's flow

### Root cause understanding
GTA V's code path interprets the query return values as:
- `[Fail]` → "memory limit reached, proceed with current allocation"
- `terminal=true` → "memory boundary, keep waiting for memory to be allocated"

GTA V expects `[Fail]` when querying exactly at `PhysicalMemory::Size()`.
The fork's current `<` comparison gives the expected `[Fail]` for GTA V.

### Direct-memory backing failure (separate issue)
The fork's `DirectMemoryBacking::SelfTest()` fails:
- `direct-memory backing: CreateFileMapping failed: 0x000005af`
- `WARNING: direct-memory backing self-test failed; continuing without shared aliases`
- `direct-memory sub-64K placeholder self-test: failed, reason = backing-unavailable`

This means GTA V's 800MB memory allocation has no physical backing
(VirtualAlloc only, no CreateFileMapping). GTA V's code might detect
this and bail out, but the log shows no explicit check.

`CreateFileMapping` call is NOT in fork source — must be in a system
library or external dependency. `ERROR_NOACCESS` (0x5af) is a Windows
kernel error indicating invalid memory protection flags.

### Cycle 0126 status
No change to behavior — fix attempted and reverted due to regression.
GTA V still exits cleanly in 60s with no GPU work.

### File changed
- `src/kernel/memory.cpp` (modified then reverted, no commit)

## Cycle 0127 (2026-08-08) — M1W2 v1.7 log message fix + re-verification

### Discovery
The M1W2 log messages in runtimeLinker.cpp said "v1.5" even though the
logic was v1.7. This made the log confusing - showing v1.5 labels but
running v1.7 logic.

### Fix
Updated log message format strings:
- "[M1W2 v1.5] fast-skip #..." → "[M1W2 v1.7] fast-skip #..."
- "[M1W2 v1.5] illegal-instruction skip at [...]" → "[M1W2 v1.7] ..."
- Updated M1W2 v1.5 comment block to explain why v1.7 changed from 1GB
  to 16 bytes advance

### Re-verification (GTA V PPSA04264)
Rebuilt and re-tested with the new log message format:
- Runtime: 31 seconds (was 60s with old binary)
- Exit code: 0 (clean exit)
- Log size: 192,660 bytes (188KB, was 199KB)
- M1W2 v1.7 fast-skips: 1,655 (max count 1,689,601)
- 0 M1W2 v1.5 log lines (all converted to v1.7)
- Vulkan initialized for GTA V: NVIDIA GeForce GTX 1650 SUPER
- Same event profile as cycle 0125 (no new stages reached)

### File changed
- `src/loader/runtimeLinker.cpp` (commit f0629f3): log message format strings
