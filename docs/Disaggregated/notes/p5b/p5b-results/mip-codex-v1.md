# P5b GLES mip storage follow-up — Codex v1

Date: 2026-09-16. Branch `p5b/blit-codex`; parent `31b26a440de4d52c1afb746ed934247da8b8b145`.
Commit: `4d1219cdadd1c1ecb3ebbd25ff44bc702ce42ac0`.

## Result

`minecraft-1.21.4-fabric-iris-bsl-in-world.DirectGLES` crosses the previous
`Fatal{UnmigratedEmulation, "generate-mipmap-storage"}` and renders in inproc:
SSIM **0.997496**, threshold 0.99, runner exit 0. Actual selection **1 / 79**.
The private `output/mobilegl.log` contains **zero Fatal records**.

## Implementation and authority

CONTRACT-P5B §2 f1 expressly permits deriving storage growth from the applier descriptor.
The frontend `EnsureGeneratedMipmapStorageAllocated` already allocates and publishes the
complete chain before the GenerateMipmap verb. In a disaggregated build under a real transport,
the GLES helper now reads the live applier resource descriptor and verifies `Levels` covers the
chain derived from `Width/Height/Depth/Target`. Array layers are excluded from shrinking axes.
The backend then proceeds to GPU generation without allocating or dirtying client level shadows.
Missing/insufficient descriptors retain the named storage refusal.

Identity lookup reuses the existing backend registry `HandleOf`: it reads the barrier-held
object lifetime id and `FindByLifetimeId`, with a registry memo update on hits. It neither mints
a slot nor mutates the client allocator. **This identity lookup remains P5b barrier debt**; this
change does not claim process separation. No new client mip-level, texel-data, or MappedData read
is introduced. The RGB16F/RGB32F CPU fallback refusal remains intact, pending writeback work.
The new code is behind `MOBILEGL_BUILD_DISAGGREGATED`, with monolith falling through unchanged.

## Directed validation

- Split ALL incremental build completed, max `-j8`; `p5b-mip-codex-build.log`.
- Three selected DirectGLES split mip tests passed (0 skipped): existing RGBA8 control, plus
  new mutable base-only R11F_G11F_B10F and DEPTH_COMPONENT32F controls reading generated mip 2.
  Both new tests assert wire emission, framebuffer completeness, GL_NO_ERROR and actual pixels.
  `p5b-mip-codex-tests.log`; all three private logs contain zero Fatal records.
- Dynamic trace library: `/home/swung/w7/p5b-blit-codex/build-split/libMobileGL.so`.
  SHA-256 `e932dd21542f29a0cf1a4337a1204b68c8c2e25034bfa71aa7d858dfa9d10ad5`.
- Trace scripts and hydrated fixtures came from `/home/swung/w7/p5b-integrate-codex`; only the
  explicit output directory was written. This was a library-under-test run, not evidence that
  the integration tree had already built this change.
- Output `/home/swung/w7/retrace-out/p5b-mip-codex/summary.json`;
  runner log `/home/swung/w7/p5b-mip-codex-trace.log`;
  private log `/home/swung/w7/retrace-out/p5b-mip-codex/minecraft-1.21.4-fabric-iris-bsl-in-world/DirectGLES/output/mobilegl.log`.
- Default SEG_STAGE **32 MiB**. No stage-size override here; parent handles the separate
  improved-transparency 128 MiB blob with its 256 MiB-stage run.
- `git diff --check` passed. No full gate, whole census, device run or adversarial review repeated.

## Remaining

Parent integrates this commit and verifies combined Android/device behavior. Vulkan results
from the named-blit package remain separate prior evidence; they were not rerun for this
GLES-only backend fix. RGB three-channel CPU mip fallback is still named debt.
