# P1 verify-lane findings on the integrated tree (`feat/disaggregated` @ 97b997d5, WSL `~/w7/pipe`)

All four P1 packages are merged. The pull build is clean: `symbol_report --threshold 0` reports 0 added / 0 removed / 0 resized / 0 renamed, `grep -rc pGLContext MobileGL/MG_Backend` is empty everywhere, `ctest -L unit` 1482/1482, `ctest -L integration-gpu` 878 with one known `-j` flake that reruns green, `ctest -N` names 0 removed / 26 added, `gen_pipe.py --check` and `--self-test` clean.

The push and verify builds are not clean. Three distinct problems, in the order they must be fixed.

## F1 — missing fill rows (mechanical, `MobileGL/MG_Pipe/FillPoints.def`)

The 79-case retrace under `MOBILEGL_PIPE_VERIFY=1` (`~/w7/retrace-out/p1-verify/`) aborts four cases with:

```
Fatal{UnmigratedPipeInput, "IsCapabilityEnabled@GenerateMipmap"}   x3 cases
Fatal{UnmigratedPipeInput, "IsCapabilityEnabled@DispatchCompute"}  x1 case
```

`IsCapabilityEnabled` is listed for `kDraw`, `kClear`, `kBlitOrCopy` and `kReadback` but not for `kTextureOp` (which owns `GenerateMipmap`) or `kDispatch`. Adding a row is the whole fix, but do not stop at these two: audit every class against the verb paths in `scratchpad/wf4/scout-pull-sites.md` §3b and add every row the lane can still miss, then prove it by re-running the lane and the retrace (they are the only real oracle).

One row of the same kind was already fixed at `9087f133` (`kReadback` was missing `IsTransformFeedbackActive` / `IsTransformFeedbackPaused`, which the depth/stencil read emulation reads through `ScopedEmulationDrawState`, `DirectGLES.cpp:5224`).

## F2 — the backend mutates frontend state inside a verb, so the pushed block goes stale mid-verb

Two retrace cases and eight `integration-verify` entries abort with:

```
Fatal{PipeVerifyDiffer, "GetSamplingResolutionGeneration@DrawArrays", verb=3, where=read}
Fatal{PipeVerifyDiffer, "GetSamplingResolutionGeneration@DrawElements", ..., where=read}
```

The failing entries are `DirectVulkan.Verify.UnboundImageDescriptorScenario.*` (6) and `DirectVulkan.Verify.SampledSetStalenessScenario.*` (2); the log line immediately before the divergence is `ResolveSamplerDescriptor: using fallback texture for unbound sampler ...`.

Mechanism, verified in the tree: `GetSamplingResolutionGeneration` counts frontend mutations. It is bumped from exactly two places, both frontend objects: `SamplerObject::BumpVersion()` (`MG_State/GLState/SamplerState/SamplerObject.cpp:34`) and `TextureObjectBase::BumpShapeVersion()` (`MG_State/GLState/TextureState/TextureObject.cpp:37`). During a draw, Magma's own paths (fallback-texture resolution for an unbound sampler, materialising a queued clear) write into frontend texture/sampler objects, so the counter moves between the verb's fill and the backend's read. This is the known "backend writes into frontend objects" surface (about 40 call sites; see `scout-pull-sites.md` §2).

This is a real finding, not a harness artefact, and it must not be silenced. Decide and implement one of:

- **(preferred) push on mutation.** The frontend mutator that changes a pushed value refreshes that field in the pushed block (and restamps it for the current verb) when `MOBILEGL_PIPE_PUSH` is on. This keeps the comparator's invariant ("the pushed block equals the live context at every read") literally true, and it is the shape P2's tracker needs anyway. Layering is fine: `MG_State` already includes `MG_Pipe/MGPipeValueTypes.h`.
- **(fallback) an explicit volatile-in-verb class.** The field is compared at verb entry only, the compare-at-read is skipped for it, the reason is written next to the field list, and the exact backend call sites that mutate it are listed in `docs/Disaggregated/MEASUREMENTS.md` as P2/P7 work.

Whichever you choose: enumerate every PipeInputs field a backend can invalidate through its own frontend writes (do not assume this one field is the only one; the lane found it because the fixture happened to hit it), handle them all the same way, add a unit test that reproduces the mid-verb mutation and would fail without the fix, and write the enumeration into the result file.

## F3 — unit tests that drive backend helpers with no verb

Eleven `ctest -L unit` entries fail in the push and verify builds and pass in the pull build:

```
DirectGLESSanity.BindsAMultisampleTextureDespiteTheDefaultMipmapFilter
DirectGLESSanity.BindingZeroClearsPreviousNativeTextureBinding
FramebufferTest.{DrawIntoAWidenedDrawBufferReachesTheDriverWithAlphaWritesMaskedOff,
  ClearIntoAWidenedDrawBufferKeepsAlphaWritableAndSubstitutesOne,
  ApplicationAlphaMaskOffIsStillHonouredOnANativeDrawBuffer,
  DualSourceBlendFactorsReachTheDriverWhenTheExtensionIsThere,
  DualSourceBlendIsDeclinedRatherThanThrownWhenTheExtensionIsMissing,
  DualSourceFactorsAreDeclinedEvenWithBlendingDisabled,
  DualSourceFactorsWithBlendingDisabledStillReachACapableDriver}
DirectGLESTextureSync.* (1)
TextureTest.* (1)
```

These construct a `GLContext` by hand and call a backend helper directly (for example `DirectGLES::BindCurrentTextures()` at `MG_Test/SanityTest.cpp:366`), so no GL entry point runs and no `MGP_FILL` ever fires; the first accessor read hits the poison. The tests are right and the poison is right: what is missing is that a test which drives a backend helper directly must state which verb it is standing in.

Fix in the test layer only (`MobileGL/MG_Test/**`): give those tests a small, explicit helper that fills the block for the verb class the helper under test belongs to (a scoped RAII "as if we were inside verb X" object is the natural shape), so the test declares the verb rather than the poison being weakened. Do not add a global escape hatch, do not make the poison tolerant in unit builds, and do not change any test name. The pull build must stay byte-identical.

## F4 — corrections to the integrator's own commands (not code)

- The lane-level poison control is not `-R 'Mipmap'`: the four dedicated control entries are `DirectGLES|DirectVulkan.VerifyCorrupted.PipeVerifyArmingScenario.CorruptedFieldIsReported` and `...PoisonOmitted.PoisonOmissionScenario.OmittedFieldAbortsOnThatVerb` (all four pass).
- The retrace arming line is `MGPipe verify: <case> <backend> armed, zero divergences, zero unmigrated reads` in the case log, not `MGPipe: verify armed`; 73 of 79 cases carry it, the other 6 are the F1/F2 aborts.
