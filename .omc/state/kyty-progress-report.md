
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