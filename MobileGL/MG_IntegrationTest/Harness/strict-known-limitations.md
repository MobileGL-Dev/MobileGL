# Strict lane coverage limitations

A skipped assertion is not a passing assertion. The strict result checker matches
both the exact entry and its documented reason; unknown skips still fail CI.
Configuration-specific controls are selected separately and must pass without skips.

## Unresolved behavior

- `ClipDistanceScenario.ADisabledClipDistanceRemovesNothing` and
  `TheEnablesAreIndependentPerDistance`: Magma's NATIVE clip distances (lavapipe,
  Adreno) still ignore the GL_CLIP_DISTANCEi enables - Vulkan has no such state.
  Every emulating backend has them (EmulateClipDistancePass); Espryt on llvmpipe
  now emulates, because its native probe catches llvmpipe clipping by disabled
  distances, so the GLES entries pass wherever the emulation is armed.
- `PartialUploadOverGpuContentScenario.OneDimensionalArraySubImageKeepsTheGpuClearedLayer`:
  the Vulkan path cannot attach the 1D array layer. GLES passes, which does not
  establish Vulkan coverage.
- `SsboDeclarationFormScenario.PackedBlockWithAnUnsizedArrayBeforeAnotherMember`
  and `TwoUnsizedArraysInOneBlock`: the tests unconditionally skip before compiling
  or drawing because the layout is known to overlap. No executing assertion exists
  in either backend; resolving the intended shader/layout semantics is separate work.

The checker reports these as **known limitations (unresolved)**. Removing a skip
must not require adding a new exception: an executing, passing case is accepted.

## Driver capability gaps

The five GLES IterationRP subgroup probes need compute basic/arithmetic subgroups.
The tested llvmpipe GLES driver lacks these. The two FirstReduction probes also
require subgroup width 16..256; local lavapipe exposes 8, so those two have no local
execution coverage. Program203 and ScratchFix have passing Vulkan entries, which
is not proof of the GLES path. The checker reports **capability gaps (not covered)**.

## Optional measurement

`FencePollScenario.PacedFrameFenceWaitsBench` is not part of generic split correctness
registrations. It remains available in ambient/full-suite entries for an explicit
`MGITEST_FENCE_PACE_BENCH=1` measurement. Ordinary fence polling assertions remain gated.
