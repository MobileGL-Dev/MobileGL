# contract-v3 — c0c, the four seam encodings (P4a package A, ID-12)

**Branch** `refs/heads/p4a/contract` · **commit `17db7598`** · parent `2cb44039` (= `feat/disaggregated`)
· base `37da3c3a` · one commit, three files.

```
[Fix] (Pipe): state the four seam encodings the packages were each inventing - the sub-data
target packing, the depth-stencil aspect numbers, the surface kind constants and the texture
target the surface record grew where its padding was

 MobileGL/MG_Pipe/MGPipeTypes.h              | 118 +++++++++++++++++++++-
 MobileGL/MG_Pipe/PipeFields.def             |   6 +-
 MobileGL/MG_Test/Pipe/PipeCatalogueTest.cpp | 148 ++++++++++++++++++++++++++++
 3 files changed, 268 insertions(+), 4 deletions(-)
```

Nothing else moved. `MobileGL/MG_Pipe/generated` is untouched (`git diff --exit-code` rc 0):
the shadow comparator expands `MGP_FIELDS_MGPSurface` from `PipeFields.def` at compile time,
so the new row needs no regeneration — `gen_pipe.py` was still re-run and reports
`generated files are up to date`.

---

## 1. The exact declarations, for B/C/D/E to quote

All four blocks are in `MobileGL/MG_Pipe/MGPipeTypes.h`, namespace `MobileGL::MG_Pipe` — the
same namespace the in-flight packages' emit headers use, so a package that keeps its own copy
gets a redefinition error rather than a silent second authority.

### 1.1 `MGPSubData::Target` — the packing (ID-12 DV-3)

Placed immediately after `MGP_ASSERT_POD(MGPSubData, 72);`. The struct's comment block above it
now states the encoding and why the halves are that way round.

```cpp
constexpr inline Uint16 MGPipePackSubDataTarget(Uint32 resourceTarget, Uint32 uploadTarget) {
    return static_cast<Uint16>((resourceTarget & 0xFFu) | ((uploadTarget & 0xFFu) << 8));
}
// Comparable against static_cast<Uint8>(MGPipeResourceTarget) / kMGPipeResourceTargetBuffer.
constexpr inline Uint8 MGPipeSubDataResourceTargetOf(Uint16 packed) {
    return static_cast<Uint8>(packed & 0xFFu);
}
// Comparable against static_cast<Uint8>(MobileGL::TextureUploadTarget).
constexpr inline Uint8 MGPipeSubDataUploadTargetOf(Uint16 packed) {
    return static_cast<Uint8>((packed >> 8) & 0xFFu);
}
static_assert(MGPipePackSubDataTarget(kMGPipeResourceTargetBuffer, 0u) ==
                  kMGPipeResourceTargetBuffer,
              "a buffer sub-data record's Target must stay exactly "
              "kMGPipeResourceTargetBuffer: the applier's SubDataNamesABuffer tests the "
              "whole field == 0");
```

* low byte = `MGPipeResourceTarget`, high byte = `MobileGL::TextureUploadTarget`.
* **`Uint32` arguments, not the two enum types.** `MGPipeResourceTarget` is minted in this
  header but the upload half is MG_State's, and no reader of the packed field ever needs that
  type — the applier and both backends read the halves back as bytes through the two
  accessors. It also lets `kMGPipeResourceTargetBuffer` be passed as it stands. **This is the
  one signature difference from clientfb's copy**, whose second parameter was
  `MobileGL::TextureUploadTarget`. A scoped enum does not convert implicitly, so B's call
  sites need `static_cast<Uint32>(...)` on the upload argument once its own copy is deleted —
  which the compiler points at, one site at a time (see §4).
* The comment block above the struct records **why**: the applier's `SubDataNamesABuffer` tests
  the *whole* field `== 0` and `TextureUploadTarget::Texture1D` is 0, so a texture record
  carrying the bare upload enumerator would be indistinguishable from a buffer record exactly
  when its owner is a 1D texture, and that texture's upload would be dispatched into the buffer
  path. With the resource target in the low byte a buffer record's `Target` stays exactly
  `kMGPipeResourceTargetBuffer` (P3a's records unchanged — their upload byte is zero too) and a
  texture record can never be zero, because no texture's `MGPipeResourceTarget` is.
* `MGPipeBuildSubDataRecord` (`MG_Impl/Pipe/ResourceTracker.h:212`,
  `out.Target = kMGPipeResourceTargetBuffer`) is correct as it stands and was not touched.

### 1.2 `MGPTextureParams::DepthStencilMode` (ID-12 DV-2)

Placed immediately after `MGP_ASSERT_POD(MGPTextureParams, 40);`.

```cpp
inline constexpr Uint8 kMGPipeDepthStencilModeDepth = 0;   // GL_DEPTH_COMPONENT
inline constexpr Uint8 kMGPipeDepthStencilModeStencil = 1; // GL_STENCIL_INDEX
```

The comment states that 0 is depth because `GL_DEPTH_COMPONENT` is the GL initial value of
`GL_DEPTH_STENCIL_TEXTURE_MODE` and a texture that never asks for the stencil aspect never
emits the call — a zeroed record must decode to what an untouched texture already has — and
that numbering by the enum's low byte (0x02 / 0x01) would have left zero meaning nothing. The
`GLenum -> byte` helper stays B's.

### 1.3 `MGPSurface::Kind` (ID-12 DV-4)

Placed immediately before `struct MGPSurface`.

```cpp
inline constexpr Uint8 kMGPipeSurfaceKindNone = static_cast<Uint8>(MGPipeKind::None);
inline constexpr Uint8 kMGPipeSurfaceKindTexture = static_cast<Uint8>(MGPipeKind::Texture);
inline constexpr Uint8 kMGPipeSurfaceKindRenderbuffer =
    static_cast<Uint8>(MGPipeKind::Renderbuffer);
static_assert(kMGPipeSurfaceKindNone == 0,
              "a zero-initialised MGPSurface must already be the empty attachment point");
```

Byte-identical in value to clientfb's copy (same reuse of `MGPipeKind`, same static_assert).

### 1.4 `MGPSurface::Pad0` -> `Uint16 TextureTarget` (ID-12 DV-5)

```cpp
inline constexpr Uint16 kMGPipeSurfaceNoTextureTarget = 0xFFFF;
static_assert(kMGPipeSurfaceNoTextureTarget ==
                  static_cast<Uint16>(MobileGL::TextureTarget::Unknown),
              "kMGPipeSurfaceNoTextureTarget is TextureTarget::Unknown widened to the "
              "field, and MG_State moved Unknown off -1");

struct MGPSurface {
    MGPipeHandle Res;
    Uint32 InternalFormat;
    Uint8 Kind; // kMGPipeSurfaceKind{None,Texture,Renderbuffer}, above
    Uint8 Layered;
    Uint16 Level;
    Uint32 Layer;
    Uint16 UploadTarget; // static_cast<Uint16>(MobileGL::TextureUploadTarget)
    Uint16 TextureTarget;   // was Pad0
};
MGP_ASSERT_POD(MGPSurface, 24);   // UNCHANGED
```

`TextureTarget` is `static_cast<Uint16>(MobileGL::TextureTarget)`, is
`kMGPipeSurfaceNoTextureTarget` on every non-texture point, and is **consulted only when
`Kind == kMGPipeSurfaceKindTexture`**. Its comment carries the reason it exists (the four
cross-object masks reduce to `(format, TEXTURE TARGET)` — `ShouldUseCaveatTextureFormat` and
`BackendTextureFormatAddsAlpha` — and no `TextureUploadTarget -> TextureTarget` inverse exists
in the tree, so `UploadTarget` cannot answer them) and the one sharp edge: a zero-initialised
record carries 0, which is `TextureTarget::Texture1D` and **not** the sentinel — harmless
because such a record is `Kind == None`, which is why gating on `Kind` is the reader's
contract.

`PipeFields.def`:

```
// P4a, ID-12 / esprytobj DV-5: Pad0 became Uint16 TextureTarget. Same trip wire as
// MGPFramebufferState's Target below - PADDING_MEMBER_RE only excludes a member still NAMED
// Pad<n>, so the rename without this row is a pipe-gates failure, and the row without the
// rename is one too. A meaning-carrying byte cannot enter this record silently.
#define MGP_FIELDS_MGPSurface(F) \
    F(Res) F(InternalFormat) F(Kind) F(Layered) F(Level) F(Layer) F(UploadTarget) F(TextureTarget)
```

**Existing readers/writers of `MGPSurface::Pad0` on this branch: none.** `grep` over
`MG_Pipe`, `MG_Impl/Pipe`, `MG_Backend` and `MG_Test` finds `MGPSurface` only in
`PipeMutation.h` (a comment), `MGPipe.h` (a comment), `MGPipeCallbacks.h` (`MGPSurfaceInfo`, a
different struct), `generated/PipeVerify.inc` (the macro expansion) and
`FramebufferEmitTest.cpp` (a comment). c0/c0b's own code never named the padding member, so
nothing needed changing and everything still compiles — verified by the three full builds
below. The packages' own files are not on this branch.

---

## 2. The unit cases (A-owned)

Both added to `MobileGL/MG_Test/Pipe/PipeCatalogueTest.cpp`, the contract-owned suite c0/c0b
already extend (c0b added `TheCompositeShaderBandIsCountedApartFromTheOrdinarySpace` there).
No CMake change: `PipeCatalogueTest` is registered by `MG_Test/Pipe/CMakeLists.txt` and
discovered with `LABELS unit`. Both run — not skipped — on all three lanes.

* **`PipeCatalogue.SubDataTargetPacksAResourceTargetAndAnUploadTarget`**
  static_asserts that both halves fit their byte; round-trips **every** `MGPipeResourceTarget`
  (0..`Count`-1, i.e. all 13) against five upload targets — `0`, `Texture2D`,
  `CubeMapPositiveX`, `CubeMapNegativeZ` and `TextureUploadTargetCount - 1` — through
  `MGPipeSubDataResourceTargetOf` / `…UploadTargetOf`, naming the offending pair on failure;
  pins the buffer invariant at compile time *and* at runtime, including a zero-initialised
  `MGPSubData` whose `Target` compares equal to `kMGPipeResourceTargetBuffer` whole-field;
  pins the collision it prevents (`TextureUploadTarget::Texture1D == 0`, and no non-zero
  resource target packs to the buffer value even with upload byte 0); and shows two cube faces
  sharing one resource target but differing in the high byte.
* **`PipeCatalogue.SurfaceNamesItsKindItsTextureTargetAndItsDepthStencilAspect`**
  the three surface-kind constants against `MGPipeKind` and `None == 0`; `sizeof(MGPSurface)
  == 24` with `offsetof(UploadTarget) == 20`, `offsetof(TextureTarget) == 22`; the sentinel
  against `TextureTarget::Unknown` and against every real target; **a zeroed `MGPSurface` has
  `TextureTarget == 0`, documented in the case body as `TextureTarget::Texture1D` and not the
  sentinel, with the reader's gate on `Kind` spelled out**; a renderbuffer point carrying the
  sentinel; the `PipeFields.def` row proved live by making `MGPipeVerify(MGPSurface, …)` name
  `"TextureTarget"`; and the two depth-stencil numbers, including
  `MGPTextureParams{}.DepthStencilMode == kMGPipeDepthStencilModeDepth`.

---

## 3. Gates — every number

Run in `~/w7/p4a-contract`, `CCACHE_BASEDIR=/home/swung/w7`, `-j 8`,
`GLIBC_TUNABLES=glibc.malloc.tcache_count=0` exported for every ctest lane. All three build
dirs (`build-linux` pull, `build-push`, `build-verify`) already existed and were rebuilt
incrementally; 0 compiler errors, 0 new warnings attributable to this commit.

| gate | result |
|---|---|
| **(a) G1** `symbol_report.py --before ~/w7/p4a-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0 --fail-on-symbol-set-change --fail-on-added-bytes 0` | **rc 0 — 0 added / 0 removed / 0 resized / 0 renamed.** `.text` 10806323 -> 10806323 (+0, +0.000%); `.data` 76840 -> 76840; `.bss` 1296872 -> 1296872; `.rodata` 1536538 -> 1536538; total 17226471 -> 17226471; file 19114360 -> 19114360 bytes. 27811 -> 27811 defined symbols, 27072 -> 27072 normalised names, **27072 unchanged (name, size and mangling all identical)**. The admitted set stays EMPTY. |
| **(b) unit, pull** `ctest --test-dir build-linux -L unit` | **rc 0, 100% passed, 0 failed out of 1638** |
| **(b) unit, push** `ctest --test-dir build-push -L unit` | **rc 0, 100% passed, 0 failed out of 1638** |
| unit, verify `ctest --test-dir build-verify -L unit` | **rc 0, 100% passed, 0 failed out of 1638** (extra lane, not required) |
| the two new cases, each lane | **2/2 passed** in build-linux, build-push and build-verify (neither is skipped in a pull build) |
| ctest names pull == push | `diff` empty, **1638 == 1638** |
| names vs `~/w7/p4a-before-ctest-names.txt` (whole suite, `LC_ALL=C`) | **0 removed**, 16 added, 2588 -> **2604**. The 16 = c0's 13 + c0b's 1 + **c0c's 2** (`PipeCatalogue.SubDataTargetPacksAResourceTargetAndAnUploadTarget`, `PipeCatalogue.SurfaceNamesItsKindItsTextureTargetAndItsDepthStencilAspect`). |
| **(c) G5** `p3a_untouched_regions.sh 37da3c3a HEAD` | **rc 0**, the eleven pool / deferred-release / ring / flush-drain functions byte-identical |
| **(d)** `check_include_closure.py --mode both --compiler clang++ --self-test --require-all --expect-probes 4` | **rc 0 — 4 probes, 0 skipped, 0 problems, 6 negative controls tripped.** value-header 66 (text) / 654 (clang) headers, artifacts-header 65 / 653, mutation-header 84 / 1, wire-header 2 / 42; 0 forbidden everywhere; all four self-contained under `-fsyntax-only` (`/usr/sbin/clang++`) |
| **(e)** `gen_pipe_dirty_surface.py --check` | **rc 0** — 79 mutators all mapped, no stale rows, 0 COARSE, **2 UNDECIDED** (the same `BindVertexArray <- NEW_VERTEX_ELEMENTS` and `UseProgram <- NEW_SHADER` c0 left listed), 26 prose answers. `--self-test` rc 0, 27 negative controls all tripped. This commit does not touch the dirty surface; the row set is unchanged from c0b. |
| `gen_pipe.py --check` | **rc 0**, generated files up to date; 71 calls, 72 verify payloads, 63 PipeInputs fields, 69 verbs, 477 inventory rows, 0 UNMAPPED |
| `gen_pipe.py --self-test` | **rc 0**, 7 negative controls tripped |
| `git diff --exit-code -- MobileGL/MG_Pipe/generated` | **rc 0** |

### 3.1 The `PipeFields.def` trip wire, proved both ways

Because this commit is the second one in P4a to turn a `Pad<n>` into a meaning-carrying member,
both directions of `gen_pipe.py`'s guard were driven on a scratch edit and reverted (backed up
by file copy, **not** `git checkout` — a `git checkout` here reverts the whole uncommitted
change, which is how the first attempt at this control lost the edit and had to be re-applied):

* row removed, member kept -> **rc 1**,
  `MGPSurface: member(s) with no F(...) in PipeFields.def: TextureTarget`
* member renamed back to `Pad0`, row kept -> **rc 1**,
  `MGPSurface: F(...) name(s) that are not members: TextureTarget`

Tree restored and re-verified clean before the commit; the builds and the whole gate table
above were then re-run end to end against the restored tree, so every number in §3 is from the
exact bytes that were committed.

---

## 4. Rework list, per in-flight package

Every package's rework/verification round rebases onto `refs/heads/feat/disaggregated` once c0c
is integrated (ID-11). What each has to do:

### B — clientfb — **delete three helpers and five constants, fill one field**

`MobileGL/MG_Impl/Pipe/TextureEmit.h` and `FramebufferEmit.h` are in the **same namespace**
(`MobileGL::MG_Pipe`) as `MGPipeTypes.h`, so after the rebase the copies are redefinitions and
the build fails loudly. Delete, do not keep and do not rename:

* `TextureEmit.h` ~lines 148-185: `MGPipePackSubDataTarget`, `MGPipeSubDataResourceTargetOf`,
  `MGPipeSubDataUploadTargetOf`, their `static_assert`, and
  `kMGPipeDepthStencilModeDepth` / `kMGPipeDepthStencilModeStencil`.
  **Keep** `MGPipeDepthStencilModeByte(GLenum)` — the GLenum decision is B's; it now reads the
  contract's two constants. **Keep** `MGPipeTextureStorageKindForTarget`, untouched by c0c.
  One signature note: the contract's `MGPipePackSubDataTarget` takes `Uint32 uploadTarget`, so
  a call passing a `MobileGL::TextureUploadTarget` still compiles (implicit conversion is not
  available for a scoped enum — **add `static_cast<Uint32>(...)` at each call site**, which is
  the documented spelling).
* `FramebufferEmit.h` ~lines 70-80: `kMGPipeSurfaceKindNone`, `kMGPipeSurfaceKindTexture`,
  `kMGPipeSurfaceKindRenderbuffer` and their `static_assert`.
  **Keep** `MGPipeResolveAttachmentUploadTarget` — the resolution is B's.
* `MGPipeBuildSurface` gains the new field, one statement per field as its own comment
  requires: texture arm `surface.TextureTarget = static_cast<Uint16>(texture->GetTarget())`
  (whatever B's accessor for the attachment's `MobileGL::TextureTarget` is), renderbuffer arm
  and the `IsEmpty()` early return `surface.TextureTarget = kMGPipeSurfaceNoTextureTarget`.
  Note the empty arm returns a value-initialised `MGPSurface`, which carries **0, not the
  sentinel** — set it explicitly, or state in the comment that `Kind == None` makes it
  unreadable. B's own G7 scripted control (drop one member from the conversion and expect the
  suite to go red naming it) should now cover `TextureTarget` too.
* B keeps emitting `MGPTextureParams::DepthStencilMode` through its helper; nothing else moves.

### C — clientsp — **nothing**, unless it reads `MGPSurface`

C owns Sampler/Image/Program emit. It does not build `MGPSurface` and does not emit
`resource_subdata`. If any C file kept a private copy of the depth-stencil constants (it should
not — that field is B's `MGPTextureParams`), delete it. Otherwise: no rework.

### D — esprytobj — **decode the high byte, move the four masks onto `TextureTarget`**

* `FindPipeTextureUpload(record, uploadTarget, level)` (`Managers.cpp` ~3745) and the consume
  site `ConsumePipeTextureUpload` (~3757) match `pending.UploadTarget` against the value the
  applier stored from `MGPSubData::Target`. That field is now **packed**: the applier's
  `PendingUpload::UploadTarget` must be filled with `MGPipeSubDataUploadTargetOf(record.Target)`
  (wire's side) and/or D must compare against the decoded byte rather than the whole field.
  Whichever side does the decode, the comparison must be
  `pending.UploadTarget == MGPipeSubDataUploadTargetOf(...)` — a bare
  `static_cast<Uint16>(TextureUploadTarget)` match against the raw field is now wrong for every
  texture whose resource target is non-zero, i.e. all of them.
* The long comment at `Managers.cpp` ~6033 ("THE UPLOAD-TARGET ENCODING is
  `static_cast<Uint16>(TextureUploadTarget)`, which is what package B must emit into
  `MGPSubData::Target`'s cube-face half; it is recorded as a deviation because the contract left
  it unstated") is now stale — the contract states it, and it states the **packed** form. Rewrite
  it to name `MGPipeSubDataUploadTargetOf` and drop the deviation note.
* DV-5's four cross-object masks (`IsSnormFallbackAttachment`, `IsUnormFallbackAttachment`,
  `IsAlphaWidenedColorAttachment` and their sibling) can stop reading the frontend attachment
  objects and read `MGPSurface::InternalFormat` + `MGPSurface::TextureTarget` instead, gated on
  `Kind == kMGPipeSurfaceKindTexture`. `ShouldUseCaveatTextureFormat` /
  `BackendTextureFormatAddsAlpha` stay byte-identical (D-N) — what changes is where their
  `target` argument comes from, and it now comes from the record rather than from a guessed
  inverse. Keep the G1 macro discipline (DV-9): these are value sites, not new lambdas.
* Where D compares a resource target out of `MGPSubData::Target` (the buffer-vs-texture branch),
  either keep testing the whole field `== kMGPipeResourceTargetBuffer` — still correct — or use
  `MGPipeSubDataResourceTargetOf`. Do not open-code `& 0xFF`.

### E — esprytdraw — **`SyncAttachmentObject` reads `TextureTarget`**

DV-7's blocker is gone. `SyncAttachmentObject` (`DirectGLES.cpp`) can resolve the texture and
renderbuffer twins from `MGPSurface::Res` now that the record carries the target the resolution
needs; gate on `Kind`, and treat `kMGPipeSurfaceNoTextureTarget` as "not a texture" rather than
as a target value. DV-1/DV-7/DV-8's other halves are unchanged C.7 boundary notes.

### wire — **nothing**

`SubDataNamesABuffer` matches `MGPSubData::Target` verbatim against
`kMGPipeResourceTargetBuffer` and the packing is designed to keep that exactly right (§1.1's
`static_assert`). The only thing wire *may* want, and it is D's call above, is to store
`MGPipeSubDataUploadTargetOf(record.Target)` into `PendingUpload::UploadTarget` so D's two match
sites compare a byte against a byte. W1's defaulted `const MGPSubRegion*` tail is untouched.

### F — gates — nothing here; `p4a_untouched_regions.sh` is still F's to add (esprytobj DV-12).

---

## 5. Deviations from ID-12

**None on substance.** Two notes:

1. ID-12 asks for "an A-owned unit test file under `MobileGL/MG_Test/Pipe/`". No new file was
   created: `PipeCatalogueTest.cpp` is the contract-owned suite c0b already extended, it is
   where D-E1's size pin and D-A3's target-table case live, and adding a file would have meant
   editing `MG_Test/Pipe/CMakeLists.txt`, which c0 deliberately wrote so no later package has to
   come back to it. Two cases were appended instead.
2. `MobileGL/MG_Pipe/generated` needed no regeneration (§0). `gen_pipe.py` was run in both
   `--check` and write modes and reports the files up to date, and `git diff --exit-code` over
   the directory is rc 0.
