# P4a package B — `clientfb`, the framebuffer and texture/renderbuffer client. Result, v1

Branch `refs/heads/p4a/clientfb`, three commits on top of the tag `p4a/contract` (`08192d72`).
Working tree clean. **Not pushed.** No other worktree touched.

| step | hash | message |
|---|---|---|
| `b1` | `51d1760f` | `[Feat] (Pipe, State): mint a {slot, gen} handle for every texture and renderbuffer and publish its create, respecify, parameters and accumulated sub-data as pipe calls` |
| `b2` | `a78eaf93` | `[Feat] (Pipe): push the bound framebuffers with a resolved read surface, inline attachment formats and a content hash that covers the draw-buffer array` |
| `b3` | `f7de87b5` | `[Feat, Test] (Pipe): wire the framebuffer subsystem bit and pin the resolved read surface, the sticky bind mask, the level-shadow strides and the union-box/region-list invariant` |

`b3`'s message is **not** C.1's wording, because C.1's wording would have been false: only ONE of the
two wired-subsystem constants is flipped. §5 (D4) is the whole reason and it is the single most
important thing in this document for the integrator.

13 files, +2708 / −56 against the tag.

---

## 1. Verdict on the hard rules

| rule | result |
|---|---|
| **G1** `symbol_report --before ~/w7/p4a-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0 --fail-on-symbol-set-change --fail-on-added-bytes 0` | **0 added / 0 removed / 0 renamed / 0 resized**, `.text` 10806323 → 10806323 (+0, +0.000%), 27811 → 27811 defined symbols. Run after every commit. P4a's admitted-resize set stays EMPTY. |
| `gen_pipe.py --check` / `--self-test` | rc 0 / rc 0 |
| `gen_pipe_dirty_surface.py --check` / `--self-test` | rc 0 / rc 0 (the two contract-marked UNDECIDED rows unchanged) |
| `check_include_closure.py --mode both --compiler clang++ --self-test --require-all` | rc 0, 4 probes, 0 skipped, 0 problems, 6 controls tripped |
| `git diff --exit-code -- MobileGL/MG_Pipe/generated` | rc 0 |
| `p3a_untouched_regions.sh 37da3c3a HEAD` | rc 0, the eleven byte-identical |
| three builds (pull / push / verify) | all three, rc 0 |
| `ctest -L unit` ×3 | **1665 / 1665 in each**, 0 failed |
| ctest names pull == push | `diff` empty |
| no name removed vs `~/w7/p4a-before-ctest-names.txt` | 0 removed, **43 added** (§7) |
| `ctest -L integration-gpu -j 8`, default mask | 966 / 966 |
| same, `MOBILEGL_PIPE_PUSH=0` | 966 / 966 |
| same, `MOBILEGL_PIPE_PUSH=0x1ff` (P4a off, G12's T2 arm) | 966 / 966 |
| `ctest -L integration-verify -j 4` under `GLIBC_TUNABLES=glibc.malloc.tcache_count=0` | 844 / 844, **0 `Fatal{` lines** |
| `MOBILEGL_PIPE_VERIFY=1 retrace_gate.py … -j 4` | **79 / 79**; `grep -l 'Fatal{'` empty; `grep -L 'MGPipe verify:'` empty |
| only B's files (+ granted sites) | yes, §6 |
| no push, no attribution lines | yes |

---

## 2. Per-file summary

### `MG_Impl/Pipe/TextureEmit.h` (+1024, B owns it)
The whole texture and renderbuffer client. Pure builders first, so every case is one `EXPECT` per
field and G7's scripted control can name the field it dropped:

- `MGPipeTextureStorageKindForTarget` — `StorageKind` derived from the TARGET rather than from
  `GetStorageType()`, because `resource_create` is emitted from `TextureObjectBase`'s constructor
  where the derived object does not exist and that accessor is **pure virtual**. Calling it there
  is undefined behaviour, not a wrong answer; the target mapping is exact.
- `MGPipePackSubDataTarget` / `…ResourceTargetOf` / `…UploadTargetOf` — the low byte is
  `MGPipeResourceTarget` (so a buffer record still compares `== kMGPipeResourceTargetBuffer`
  whole, unchanged from P3a) and the high byte is `TextureUploadTarget`, which is the only place a
  cube face can ride. §5 (D2) records it as a deviation.
- `MGPipeTextureExtentOf` — the extent trio plus the layer count, which the frontend answers in
  three different axes by target (1D array in `y`, other layered targets in `z`, cube = six blobs).
- `MGPipeBuildTextureResourceDesc`, `MGPipeBuildRenderbufferResourceDesc`,
  `MGPipeBuildTextureParams`, `MGPipeLevelPitchOf`, `MGPipeBoxOfDirtyRegion`,
  `MGPipeBuildSubRegion`.
- `MGPipeTextureEmitter` — the handles and their inverse, the sticky `BindMask` with the
  `ImageBindableHint` transition arming `ForceResync` exactly once, the publication latch, the
  descriptor **memcmp dedupe**, the drain list, and `DrainTextureSubData` with the
  empty-list early-out.
- Six MG_State entry points and the two step-1 death emitters (§5, D3).

### `MG_Impl/Pipe/FramebufferEmit.h` (+425, B owns it)
`MGPipeBuildSurface` (one statement per field), `MGPipeResolveAttachmentUploadTarget`,
`MGPipeDrawBufferIndex`, `MGPipeFramebufferStateContentHash` (field-wise into a zero-initialised
staging copy, `ContentHash` left 0), and `MGPipeFramebufferEmitter::EmitFramebufferState` — the
per-target discrimination, the `Target = Both` collapse, the suppressor slot fed the combined hash
with per-target latches deciding which record goes out, `Complete` from `CheckCompleteness()`, and
the client half of D-C3's over-wide-attachment refusal.

### `MG_State/GLState/TextureState/TextureObject.{h,cpp}` (+34 / +142)
Three non-virtual, push-only protected helpers on `TextureObjectBase`
(`PipePublishDescriptor`, `PipePublishParams`, `PipeNoteLevelDirty`) declared in the header and
defined in the `.cpp`, which is the ONE MG_State translation unit that sees the client emitter
(§5, D1). `~TextureObjectBase` becomes the three-step death path. Hooks: the constructor (mint +
create), `SetInternalFormat`, the three border-colour setters, both swizzle setters,
`SetBaseLevel`, `SetMaxLevel`, `SetImmutableLevels`, `SetSamples`, `SetFixedSampleLocations`,
`SetDepthStencilTextureMode` (header, inline), and — **after** the storage mutation, never before
`BumpShapeVersion` — `TextureObjectWithOneMipmap::{AllocateStorage, TruncateMipmapLevels,
MarkStorageDirty, MarkStorageDirtyRegion}`.

### `TextureObject2DCube.cpp` (+17), `TextureObjectView.cpp` (+12), `TextureObjectBuffer.h` (+9)
The cube's four storage overrides call the inherited helpers; the view's constructor publishes
`ViewOf` explicitly (because `SetImmutableLevels` early-returns on a degenerate view); the buffer
texture's `SetBufferRange` publishes the `BufferForTexBuffer` / `BufOffset` / `BufSize` trio at the
first statement at which it is complete.

### `MG_State/GLState/RenderbufferState/RenderbufferObject.{h,cpp}` (+7 / +61)
One push-only private `PipePublishDescriptor`, the constructor's mint + create, the three storage
setters (D-D2's hole closed by EMISSION, with no new member and no widened shutter), and the
three-step death path.

### `MG_State/GLState/FramebufferState/FramebufferObject.cpp` (+25)
`~FramebufferObject` now calls `MGPipeEmitFramebufferDestroyAndFree` — steps 2 and 3 only, because
D-I2 gives this kind a handle and no wire lifetime.

### `MG_Test/Pipe/{TextureEmitTest, FramebufferEmitTest}.cpp` (+484 / +367)
14 + 8 emitter-side cases, appended before `main()` and disjoint from the wire package's
applier-side cases. All 24 registered names exist in a pull build as visible skips.

### `MG_Test/State/ObjectLifetimeIdTest.cpp` (+69, granted)
`ProbeLifetimeIdAcrossAddressReuse` extended to `TextureObject2D`, `FramebufferObject`,
`SamplerObject` and `ProgramObject` (8 new cases, all green). The texture case doubles as a
64-round mint/free churn check under push.

---

## 3. `EmittedCallSuppliesTheWholeField`, per field

`PipeFill.cpp` is the contract package's for the whole phase and **was not touched**; the six P4a
rows already sit in its **false** arm. This package's job was to confirm each answer against what
it actually emits, and it concurs with all six. Only two of them are this package's families:

| field | emitted by | supplies the whole field? | why |
|---|---|---|---|
| `GetFramebufferBindingSlot` | `SetFramebufferState` (this package) | **NO** | The field's storage is `BindingSlot<FramebufferObject>` — a frontend heap reference — and the call carries an eight-byte `{slot, gen}` `Fbo` plus a fully resolved descriptor. The applier has no way to produce the pointer and P4a deliberately does not give it one (a payload never contains a pointer). Skipping the pull would leave the mirror null on every draw of every push build. What retires it is the phase where the backend stops reading a frontend `FramebufferObject`, not a better applier. This is P3a's `GetBoundVertexArray` precedent, one for one. |
| *(none)* | the texture-resource family | **n/a** | `kMGPipeSubsystemTextureResources` names NO emitted field and cannot: `resource_create`/`_respecify`/`_subdata`/`_destroy` and `set_texture_params` are dispatched at the GL call that causes them rather than filled into a `PipeInputs` field. `PipeFill.cpp` asserts this (`NoDirtyBitOwnsTheTextureResourceSubsystem`), and no `Coverage.def` row exists or may exist. |

The other four (`GetImageTextureBinding`, `GetTextureUnitObject`, `GetProgramForDraw`,
`GetProgramForDispatch`, plus `GetMaxTouchedTextureUnit`) belong to package C and are untouched.

**Consequence, and it is the one that matters for the verify lane: this package retires no pull at
all.** Every field it could have affected keeps coming through the residual fill, which is why the
79-case verify retrace reports zero divergences rather than "no divergence we looked for".

---

## 4. The wired-bit commit

`b3` (`f7de87b5`) sets `kMGPipeWiredFramebufferSubsystem = kMGPipeSubsystemFramebuffer` in
`MG_Impl/Pipe/FramebufferEmit.h`. `PipeFill.cpp`'s OR and its four `static_assert`s were not
touched; the assert that the constant is either 0 or its own bit still holds.

`kMGPipeWiredTextureSubsystem` **stays 0**. §5 (D4) is the full argument and the one-line edit the
integrator makes after the rebase.

---

## 5. Deviations

**D1 — MG_State reaches the client emitter through TWO translation units, not zero.**
P3a's buffer family declares its emission points in `MG_Pipe/PipeMutation.h` and defines them in
`MG_Impl/Pipe/PipeFill.cpp`, so `BufferObject.cpp` sees a declaration and never the client's
tracker. P4a cannot copy that: **both of those files belong to the contract package for the whole
phase** (C.7 gives them to A, and the "no file touched twice" rule is what discharges the
`bb2a236d` merge trap structurally), and neither carries a texture emission declaration. The
next-best arrangement keeps the same property one level in:

* every texture emission point is a **protected, non-virtual, push-only member of
  `TextureObjectBase`** declared in `TextureState/TextureObject.h` and defined in
  `TextureState/TextureObject.cpp` — the one MG_State TU that includes
  `MG_Impl/Pipe/TextureEmit.h`. The cube's, the view's and the buffer texture's TUs call the
  inherited helper and still see only a declaration;
* the renderbuffer half has no base class to hang helpers on and exactly one `.cpp`, so
  `RenderbufferState/RenderbufferObject.cpp` is the second and last such TU.

So the client's tracker reaches exactly **two** MG_State translation units instead of zero.
`check_include_closure.py`'s mutation-header probe is unaffected (it asserts
`MG_Pipe/PipeMutation.h`'s own closure, which is unchanged) and is green. **The integrator can fold
the six free functions into `PipeMutation.h` and `PipeFill.cpp` in one mechanical commit once the
phase's file ownership relaxes; nothing else has to move.**

**D2 — `MGPSubData::Target` is PACKED for a texture: low byte the resource target, high byte the
upload target.**
D-D3's table asks `Target` to carry "the owner's upload target (`MGPipeResourceTarget` from D-A3,
**plus** the cube-face upload target in `MGPSurface`-style terms)". The payload has one `Uint16`
for two facts the server needs and no other field can hold either: which kind of storage the
destination is (the applier branches on it — a buffer target dispatches into `MGPipeResourceOps`,
every other target stores and returns) and which cube face the level belongs to, which is not
derivable from the resource target. The buffer half is preserved exactly: a buffer record's upload
byte is 0, so `Target == kMGPipeResourceTargetBuffer` still holds for the whole field and `w1`'s
branch is right whether it tests the whole `Uint16` or only the low byte. A `static_assert` pins
that. **If A would rather own the encoding, moving these three `constexpr` helpers to
`MGPipeTypes.h` beside `MGPSubData` is a copy-paste.**

**D3 — step 1 of the death order is emitted from the destructor, one statement before the
contract's helper.**
`MG_Impl/Pipe/PipeFill.cpp`'s `MGPipeEmitTextureDestroyAndFree` and
`…RenderbufferDestroyAndFree` hard-code `published = false` at the tag, with the note "the texture
emitter publishes nothing yet". That file is A's for the whole phase, so this package supplies the
answer from the object's destructor instead:

```
~TextureObjectBase / ~RenderbufferObject
  -> MGPipeEmit<Kind>ResourceDestroy(lifetimeId)   // 1. the wire delete, published-gated
  -> MGPipeEmit<Kind>DestroyAndFree(lifetimeId)    // 2. the notice, 3. the slot
```

which is **exactly** `PipeMutation.h`'s fixed three-step order and not a variation on it. Both new
functions are published-gated rather than slot-gated, for the reason `PipeFill.cpp` states in full.
The integrator can fold them into the helpers' bodies when D1 is folded.

**D4 — `kMGPipeWiredTextureSubsystem` is still 0, and that is a BLOCKED flip rather than an
unfinished one. THIS IS THE ONE THING TO READ.**
Unlike the other three P4a families, the texture family gets no fresh apply entry points: the
catalogue is closed and a texture rides P3a's OWN `resource_create` / `resource_respecify` /
`resource_subdata` / `resource_destroy` rows. On a base without `w1` those four have P3a's
**buffer** bodies, and two of their properties make a texture record actively harmful rather than
merely ignored:

* `MGPipeApplierState::Resources` is ONE vector indexed by SLOT (D-B2 makes it three, one per
  resource kind). Buffer, Texture and Renderbuffer slot spaces are **independent**, so a texture
  create at slot 12 overwrites the buffer record at slot 12 and the next write to that buffer is
  refused against the texture's extent — a dropped content write with no diagnostic beyond the
  refusal counter;
* `SubDataBoxFault` validates every record as the buffer half of `MGPSubData`, so a texture
  sub-data record is `Fatal{ProtocolCorruption}` on `record.Level != 0` alone.

This was **found, not argued**: the first verify-build unit run with the bit flipped went red with
`Fatal{ProtocolCorruption} resource_subdata {slot=12, gen=0, glName=8}: the buffer half carries a
mip level`, and it took `TextureTest.GetTexImageReadsALevelWhoseLowerLevelsWereNeverDefined` —
a pre-existing case, nothing to do with P4a — down with it. That is the applier's trip wire doing
exactly its job, and it is evidence that D.1's stated order (`wire` **before** `clientfb`) is not
merely tidy.

The package is therefore complete and gated but not switched on:

* `kMGPipeWiredTextureSubsystem = 0`, so `kMGPipeWiredSubsystems` does not carry the bit, the
  validate point never calls `DrainTextureSubData`, the drain list never grows, and nothing this
  file builds reaches an applier that cannot hold it;
* `MGPipeTextureRecordsReachTheApplier()` is that constant asked as a `constexpr` predicate and
  guards the **four** `MGPipeApplyResource*` calls with `if constexpr`. `set_texture_params` is
  deliberately NOT behind it — `MGPipeApplySetTextureParams` is one of P4a's own fifteen entry
  points and is a stub at the tag, so sending it keeps that seam exercised;
* `MGPipeTextureEmitter::ArmForTest(Bool)` (beside `ResetForTest()`) is initialised from the
  constant and is what the two suites flip, so the whole conversion — descriptors, params, the
  sticky mask, the drain list, the sub-data record and its strides — is gated by tests that assert
  on the **emitted records** rather than on any applier state. The flip cannot land untested.

**The integrator's edit after the rebase onto `p4a/wire` is one line**: change that `0` to
`kMGPipeSubsystemTextureResources`. §8 lists what to re-run.

**D5 — the drain list's membership flag lives on the CLIENT, not on `MipmapStorage`.**
D-D4 asks for "a `Bool m_onDrainList` beside the level's existing `m_isDirty`, inside
`#if MOBILEGL_PIPE_PUSH`". It is a per-slot `Vector<Uint32>` of packed `(uploadTarget, level)` keys
in the emitter instead, for D-D5's own inversion: the emission cursor is client state and
`MG_State` gains no member at all this way (D-D2 makes the same argument for the renderbuffer).
The cost is a linear scan of ≤ 96 shorts on the `glTexSubImage` path, which has just memcpy'd
texels. `TextureEmit.ABailedLevelStaysDirtyAndStaysOnTheDrainList` pins the behaviour either way.

**D6 — the descriptor is deduped on its own bytes rather than on a version.**
`glTexStorage2D` allocates level by level and every parameter setter that touches a descriptor
field publishes; an 88-byte `memcmp` against the last emitted descriptor is cheaper than the
emission it avoids and is what keeps `glRenderbufferStorage`'s three-setter sequence **one**
record rather than three (`TextureEmit.ARenderbufferRespecifyPublishesItsExtentWithoutAVersionCounter`
pins that). It is the "version-first skip before anything expensive" shape, with the descriptor
standing in for a version the frontend does not have.

**D7 — `MGPSurface::Kind` reuses `MGPipeKind` rather than minting a three-value enum.**
`MGPipeKind` already spells `Texture` and `Renderbuffer`, its `None` is 0, and a zero-initialised
`MGPSurface` is therefore already the empty attachment point the contract's own sentence requires.
A `static_assert` pins `None == 0`. If A wants a dedicated enumeration beside `MGPSurface` it is a
rename, not a re-encoding.

**D8 — `MGPFramebufferState::DrawBuffers[]` indexes THIS RECORD'S `Color[]`, and the four
default-framebuffer tokens narrow to 0.**
`None` → −1 and `Color0..Color31` → 0..31 are the field's documented convention read literally. A
default framebuffer keeps its one colour surface under `BackLeft`, this record carries it in
`Color[0]`, and `IsDefault` is what tells the server which framebuffer it is looking at. The
distinction the narrowing loses is FRONT vs BACK and LEFT vs RIGHT, which MobileGL's frontend never
gives a default framebuffer — `FramebufferObject`'s constructor seeds `BackLeft` and nothing writes
another. A phase that needs stereo has to widen the field, not re-encode this one.

**D9 — `MGPTextureParams::DepthStencilMode` is numbered, not a GLenum.**
The frontend keeps `GL_DEPTH_COMPONENT` / `GL_STENCIL_INDEX` (0x1902 / 0x1901) and the payload byte
cannot hold one. 0 is DEPTH_COMPONENT, which is also GL's initial value and therefore what a
zero-initialised record already says.

**D10 — `MGPTextureParams::BuiltinSampler` is IDENTITY-addressed on this branch.**
It is `MGPipeSlots().Acquire(MGPipeKind::SamplerCso, texture->GetSamplerObject()->GetLifetimeId())`
— the same key `~SamplerObject`'s death helper already resolves through, so the two agree by
construction. Minting a handle is client state; the CALL that fills the record
(`create_sampler_state`) is package C's. **The cross-package seam to check at integration** is
whether C's content-addressed cache (capacity 256) keys its slots on the `SamplerObject`'s lifetime
id. If it does, nothing changes. If it mints on a synthetic id, this line has to become a call into
C's cache, and `TextureEmit.TwoTexturesWithIdenticalSamplingShareOneBuiltinCso` — which SKIPs today
with that reason printed — is its gate.

**D11 — one SamplerCso slot per texture is not released on this branch, by construction.**
`~SamplerObject` still raises the bare `NotifyStateObjectDestroyed` at the tag; D-I1's table gives
package C the edit that turns it into `MGPipeEmitSamplerCsoDestroyAndFree`. Until `clientsp` lands,
every texture that ever published its params leaves one `SamplerCso` slot behind. It is an ordered
dependency, not a leak in this package: `clientfb` → `clientsp` is D.1's order, and
`TextureEmit.ADestroyedTextureReleasesItsResourceViewAndBuiltinSamplerSlots` says so in the case
body rather than asserting something it cannot yet prove.

---

## 6. Granted sites

Exactly one grant was taken.

| file:line | what |
|---|---|
| `MobileGL/MG_Test/State/ObjectLifetimeIdTest.cpp:34-40` | the five new includes |
| `MobileGL/MG_Test/State/ObjectLifetimeIdTest.cpp:161-227` | eight cases extending `ProbeLifetimeIdAcrossAddressReuse` / `ExpectDistinctIdsWhileBothAlive` to `TextureObject2D`, `FramebufferObject`, `SamplerObject` and `ProgramObject` |

**Grants NOT taken, and why:**

* `MG_Impl/GLImpl/Texture/GL_Texture.cpp` — **not needed, including the one anticipated exception.**
  The generated-mip storage grow at `:528-539` calls `AllocateStorage` + `MarkStorageDirty(false)` +
  `TruncateMipmapLevels` + `BumpContentVersion()`, and the first and third of those are hooked on
  the OBJECT, so the respecify goes out from there. The same is true of both `glTexBuffer` entry
  points: they end in `SetBufferRange` + `SetInternalFormat`, both of which publish. No storage path
  was found that bypasses `TextureObjectBase` / `TextureObjectMipmap`.
* `MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.cpp` — not needed;
  `AllocateRenderbufferStorage_State` goes through `RenderbufferObject`'s own three setters.
* `MG_Test/Framebuffer/FramebufferTest.cpp`, `MG_Test/Texture/{TextureTest, TextureViewTest}.cpp` —
  no payload edit broke a case (all three suites are green in all three builds), so nothing was
  appended and ID-8's collision warning does not apply.
* `MG_Test/Buffer/BufferTest.cpp` — package D's read-and-confirm, not B's.

---

## 7. What was added, and what was not touched

43 ctest names added, 0 removed. 35 are the contract commit's; the 8 this package adds are the
`ObjectLifetimeIdTest` extension. The 22 emitter-side cases in `TextureEmit` / `FramebufferEmit`
land inside names the contract commit already registered.

Untouched, confirmed: everything under `MG_Backend/`, `MG_IntegrationTest/`, `.github/`,
`MG_State/GLState/{SamplerState, ProgramState}/**`, `MG_State/GLState/Core.{h,cpp}`, every
`MG_Pipe/` file, every `scripts/` file, every `generated/*.inc`, and
`MG_Impl/Pipe/{PipeFill.*, Tracker.h, SetHashSuppressor.h, SlotAllocator.*, CsoCache.h,
ResourceTracker.h, VertexInputEmit.h, SamplerEmit.h, ImageEmit.h, ProgramEmit.h}`.

---

## 8. Verification transcript

```
$ git log --format='%h %s' 08192d72..HEAD
f7de87b5 [Feat, Test] (Pipe): wire the framebuffer subsystem bit and pin the resolved read surface, …
a78eaf93 [Feat] (Pipe): push the bound framebuffers with a resolved read surface, …
51d1760f [Feat] (Pipe, State): mint a {slot, gen} handle for every texture and renderbuffer …

$ python3 scripts/symbol_report.py --before ~/w7/p4a-before-libMobileGL.so \
      --after build-linux/libMobileGL.so --threshold 0 --fail-on-symbol-set-change --fail-on-added-bytes 0
symbol-report: .text 10806323 -> 10806323 (+0, +0.000%)
symbol-report: 27811 -> 27811 defined symbols: 0 added, 0 removed, 0 resized, 0 renamed   rc=0

$ python3 scripts/gen_pipe.py --check                        rc=0
$ python3 scripts/gen_pipe.py --self-test                    rc=0
$ python3 scripts/gen_pipe_dirty_surface.py --check          rc=0
$ python3 scripts/gen_pipe_dirty_surface.py --self-test      rc=0
$ python3 scripts/check_include_closure.py --mode both --compiler clang++ --self-test --require-all
include-closure: 4 probes, 0 skipped, 0 problem(s), 6 negative-control trip(s)             rc=0
$ git diff --exit-code -- MobileGL/MG_Pipe/generated         rc=0
$ bash scripts/p3a_untouched_regions.sh 37da3c3a HEAD        rc=0  (the eleven byte-identical)

$ cmake --build build-linux  -j 12   rc=0
$ cmake --build build-push   -j 12   rc=0
$ cmake --build build-verify -j 12   rc=0
$ ctest --test-dir build-linux  -L unit -j 12   100% 1665/1665
$ ctest --test-dir build-push   -L unit -j 12   100% 1665/1665
$ ctest --test-dir build-verify -L unit -j 12   100% 1665/1665
$ ctest --test-dir build-push -R 'FramebufferEmit\.|TextureEmit\.'   100% 24/24 (1 SKIP, D10)

$ diff <(build-linux names) <(build-push names)                       (empty)
$ comm -23 ~/w7/p4a-before-ctest-names.txt <(build-linux names)        0 removed
$ comm -13 ~/w7/p4a-before-ctest-names.txt <(build-linux names)       43 added

$ ctest --test-dir build-push -L integration-gpu --no-tests=error -j 8
100% tests passed, 0 tests failed out of 966
$ MOBILEGL_PIPE_PUSH=0     ctest --test-dir build-push -L integration-gpu -j 8   966/966
$ MOBILEGL_PIPE_PUSH=0x1ff ctest --test-dir build-push -L integration-gpu -j 8   966/966
$ GLIBC_TUNABLES=glibc.malloc.tcache_count=0 \
      ctest --test-dir build-verify -L integration-verify --no-tests=error -j 4
100% tests passed, 0 tests failed out of 844        Fatal{ lines: 0

$ MOBILEGL_PIPE_VERIFY=1 python3 ~/w7/retrace_gate.py --tree ~/w7/p4a-clientfb \
      --lib ~/w7/p4a-clientfb/build-verify/libMobileGL.so --out ~/w7/retrace-out/p4a-clientfb -j 4
passed 79 / 79; failed: []                                                                 rc=0
$ grep -l 'Fatal{'         ~/w7/retrace-out/p4a-clientfb/*.log     (empty)
$ grep -L 'MGPipe verify:' ~/w7/retrace-out/p4a-clientfb/*.log     (empty)
$ rm -rf ~/w7/retrace-out/p4a-clientfb
```

**A trap worth recording for the integrator, because it cost a whole retrace round.** The first
retrace reported `passed 2 / 79` with `ssim=None` on every case — the exact false red C.1 warns
about. `tools/trace_replay/fixtures` in this worktree had been pulled back to 131/133-byte **LFS
pointers**, with an mtime matching the "reset to the tag" the six sibling trees were given at 12:24;
`git update-index --assume-unchanged` was still set, so `git status` was clean and nothing looked
wrong. Restoring them from `~/w7/pipe/tools/trace_replay/fixtures` (803 MB) and re-running gave
79/79. **Every sibling tree reset at the same time is very likely in the same state.**

Two other findings, both gates working:

1. the `Fatal{ProtocolCorruption}` of §5 (D4), which is why the texture bit is not flipped;
2. `MG_Config::Features.PipePush` defaults to **0** and only `ConfigLoader` sets the phase mask, so
   a unit case that does not arm it is green for the wrong reason. Both suites arm it explicitly
   and restore it, and the arming is called out in the scope's comment.

---

## 9. What must be re-run after the rebase onto `p4a/wire`

The integrator's edit is **one line** — `kMGPipeWiredTextureSubsystem = 0` →
`= kMGPipeSubsystemTextureResources` in `MG_Impl/Pipe/TextureEmit.h` — and it must be made in the
same commit as the rebase, because `w1` is what makes it safe. Then, in this order:

1. **`ctest -L unit` in build-verify**, first and on its own. It is the cheapest place the two
   defects of §5 (D4) surface: `TextureEmit`'s four drain cases and
   `TextureTest.GetTexImageReadsALevelWhoseLowerLevelsWereNeverDefined` were the exact five that
   went red before, and they are the named expected-failing set to re-state unchanged.
2. **`ctest -L integration-gpu` on build-push at the default mask AND at `0x1ff`.** The default arm
   is the first place a texture record meets a real applier record table; the `0x1ff` arm proves
   the A/B still isolates it.
3. **`ctest -L integration-verify` under `GLIBC_TUNABLES=glibc.malloc.tcache_count=0`**, for the
   two new death paths (`~TextureObjectBase`, `~RenderbufferObject`) reaching a live applier.
4. **The 79-case verify retrace**, which is the only thing that exercises the drain list on real
   texture traffic (`rei`, `xaero-*`, `journeymap`, `modernui` are the atlas fixtures D-D5 names).
   **Check the fixtures are not LFS pointers first.**
5. **G1**, because the flip changes a `constexpr` in a push-only header and must still move nothing
   in the pull build.

Also re-check at integration, in this order of risk:

* **§5 (D10)** — whether package C's sampler CSO cache keys its slots on the `SamplerObject`'s
  lifetime id. If not, `MGPipeTextureEmitter::EmitTextureParams`'s one `Acquire` becomes a call
  into C's cache and `TwoTexturesWithIdenticalSamplingShareOneBuiltinCso` stops skipping.
* **§5 (D11)** — that `clientsp` really does replace `~SamplerObject`'s bare notice with the death
  helper, or every texture keeps leaking one `SamplerCso` slot into G8b.
* **§5 (D2)** — that `w1`'s `Desc.Target` branch reads `MGPSubData::Target` in a way the packing
  satisfies (it does if it tests either the whole `Uint16` or the low byte against
  `kMGPipeResourceTargetBuffer`).
* **§5 (D1/D3)** — whether to fold the six MG_State entry points and the two step-1 death emitters
  back into `PipeMutation.h` / `PipeFill.cpp` now that A's ownership window has closed.
