# P4a package F — `gates`. Rework result, v3 (verification round, integrates LAST)

Branch `refs/heads/p4a/gates`, head **`4678519f`**, **fifteen** commits on
`refs/heads/feat/disaggregated` = **`41b79050`** (contract c0..c0f, wire, clientsp v3, clientfb v2,
esprytobj v2, **package D's verification round and package E's**). Eight commits are v2's, rebased;
**seven are new in v3**. Nothing pushed. Working tree clean.

**The base moved twice under this round** and every number below is from the third and final
rebase. F started on `29d51ab9`, moved to `29bd4d2c` (D's verification round), and finished on
`41b79050` (E's). All three rebases were clean; §2.

---

## 0. The headline

**Everything is green.** On `4678519f`:

| | |
|---|---|
| `ctest -L integration-gpu` push, default `0x1fff` | **1079/1079** |
| the same at `0x1ff` / at `0` / pull build | **1079/1079** / **1079/1079** / **1079/1079** |
| DirectVulkan lane / DirectGLES lane | **540/540** / **539/539** |
| `ctest -L integration-verify` | **884/884**, **zero Fatal** |
| `ctest -L unit` × 3 builds | **1767 / 1767 / 1767** |
| G1 symbols | **0 added / 0 removed / 0 resized / 0 renamed** |
| G2 pull vs push names | **2846 == 2846, diff empty** |
| G14 vs `37da3c3a` | **0 removed / +258** |
| G5 `p4a_untouched_regions.sh` two-ref / `--self-test` | **rc 0, 17 rows** / **rc 0, 8 negative controls** |
| G7 `p4a_descriptor_negative_control.sh build-push` | **rc 0**, both controls tripped and named |

Two things in that table are new this phase and worth reading twice:

* **`HandleRecycle.Handles` is 36/36.** ID-38/ID-39's six armless `.Handles` aborts are closed, and
  the itest CMake's `0x1fff` phase pin — F's — is what closed them.
* **The G7 descriptor negative control reaches its real path for the first time** and exits 0
  naming `Layered` and `borderColorForm`. `gates-v2.md` §7's first row is satisfied.

**Four defects F's gates found on the two intermediate bases, all now fixed** (§4.5). Three were
fixed by the integrator's own D/E verification commits, arriving independently and with the same
diagnosis; one was F's own case and F fixed it. They are recorded because F's lanes are what will
keep them fixed, and because a reader of the intermediate gate runs would otherwise not understand
why the numbers moved. Nothing is owed.

---

## 1. Commits

| # | hash | what |
|---|---|---|
| 1..8 | `246434bb` … `9a0a7f76` | v2's eight, rebased (§2) |
| 9 | `e62abff6` | **F-v2-m3**: `MagmaPipeAbaControlCoversKind` deleted |
| 10 | `b938c83b` | **F-v2-m4**: `REPAIR_RC` is read, in two places |
| 11 | `f9c0e7ec` | **F-v2-m1**: the refusal matcher reads the sentence's DIRECTION; **F-v2-m2**'s wording |
| 12 | `972dd811` | **G9's white-box half** (ID-19/ID-31): `Harness/PipeApplyPeek.{h,cpp}` + the four cases |
| 13 | `7107d0f4` | c0f's belt pinned in a lane, on both backends (ID-40, "if cheap") |
| 14 | `4614abb9` | **wire MINOR-1** (ID-27, integrator grant): the cube-face erase-key case |
| 15 | `4678519f` | the SamplerCso leak case corrected against C's content-addressed cache (§3.4) |

No `Co-Authored-By`, no other attribution, fifteen single-line `[Type] (Scope): description`
messages (grepped for `co-authored|claude|assistant|generated`: 0 hits).

**Files v3 touched**: `MG_Backend/DirectVulkan/Renderer/MagmaPipeArms.h` (granted; v2 already owned
it), `MG_IntegrationTest/CMakeLists.txt`, `MG_IntegrationTest/Harness/PipeApplyPeek.{h,cpp}` (new),
`MG_IntegrationTest/Scenarios/{ObjectSubsystemControlScenario,
TextureParamsWithoutASamplerViewScenario, HandleRecycleScenario}.cpp`,
`MG_Test/Pipe/TextureEmitTest.cpp` (ID-27's grant — one case added, nothing else in the file
changed), `scripts/p4a_descriptor_negative_control.sh`.

**`.github/workflows/test.yml` was not touched by v3 at all.** The TEMPORARY trigger at `:9` and
`BASELINE: "37da3c3a"` at `:1599` are v2's, unmodified.

---

## 2. The three rebases

| from | onto | result |
|---|---|---|
| `ecf9cb45` (v2's head) | **`29d51ab9`** (c0..c0f) | `REBASE_RC=0`, **8/8**, no conflict → `8d40c55a` |
| `796e6bfa` | **`29bd4d2c`** (+ D's verification round, six commits) | `REBASE_RC=0`, **15/15**, no conflict → `1c58d48a` |
| `1c58d48a` | **`41b79050`** (+ E's verification round, ten commits) | `REBASE_RC=0`, **15/15**, no conflict → `4678519f` |

Nothing new against the re-review's §4 prediction. The file sets stay disjoint: pipe moved
`MG_Pipe/**`, `MG_Impl/Pipe/**`, `MG_Backend/DirectGLES/**` and `MG_Test/**`; F owns
`MG_IntegrationTest/**`, `MagmaPipeArms.h`, `scripts/p4a_*.sh` and `test.yml`. The only overlap is
`MG_Test/Pipe/TextureEmitTest.cpp`, which F touches for the first time this round under ID-27's
grant and after every rebase.

Three things the re-review said to check on the rebased tree:

* **`PeekPipeCompositeSlot*` still resolves** — `SlotAllocator.h`'s `CompositeHighWater()` /
  `CompositeLiveCount()` and `kMGPipeShaderCsoCompositeSlotBase` are unchanged; `PipeSlotPeek.cpp`
  compiles and its cases run.
* **The itest CMake pins are still F's** — every `MOBILEGL_PIPE_PUSH=` there is `0x1fff` except the
  documented off / refusal / `0` lanes and the two new consumer lanes (also `0x1fff`).
* **The G7 script's targets survive** — `MGPSurface::Layered` and
  `SamplerParameters::borderColorForm` both exist, and the script now reaches its real path (§5).

**F-v2-m2's correction, for the integrator.** `gates-v2.md` §4 and its §7 row told you to expect
the `RefusedTexture` lane RED until package D's rework. **That window does not exist**: esprytobj
v2 carries D-K2's fourth row, the arm arms, and its refusal assertion passes — the matcher finds
Espryt's exact sentence, quoted in §4.1. Both places in `gates-v2.md` now carry a `[CORRECTED …]`
marker pointing here.

---

## 3. What was added, and how each piece was proven

### 3.1 G9's white-box half (ID-19 / ID-31) — and the artefact ROADMAP.md:20 asks for

**`MG_IntegrationTest/Harness/PipeApplyPeek.{h,cpp}`** (new, 110 + 188 lines), a separate
translation unit for `PipeSlotPeek`'s stated reason and more so: it includes Espryt's own
`Managers.h`, which may not meet a scenario's GL headers. Four entry points, each returning `false`
and touching nothing where it cannot look (a pull build has no applier; Android resolves no
internal symbol; a backend that is not Espryt has no twin):

* `PeekPipeTextureParamsRecord(glName, out)` — scans `MGPipeApplier().TextureResources` for a live
  record whose `Desc.GlNameForDiag` is that GL name (a **diagnostic** field used as a diagnostic
  search key, never as an identity) and returns `{Slot, Gen, ParamsSerial, Swizzle[4] as GL enums,
  DepthStencilMode as a GL enum}`. Purely server-side: it consults neither the client emitter nor
  the twin registry, so it arms on **D's applier state** and not on the markers B and C set — which
  ID-31 is explicit about, and which is how F-M5's shape is avoided.
* `PeekEsprytAppliedTextureParams(glName, glTarget, out)` — reads the value back **from the driver**
  through the twin's own ES name (`g_backendTextureObjects.Find(...)->GetBackendTextureId()`, then
  `g_GLESFuncs.glGetTexParameteriv`). The binding on the **already-active** unit is saved and
  restored byte for byte and no unit is switched, so Espryt's `g_boundTexturesCache` still
  describes reality afterwards. Binding through the twin's own `Bind()` would have updated that
  shadow and changed what the scenario measures next; this does not.
* `PeekEsprytHasSamplerViewForTexture(glName, out)` —
  `SamplerViewImpl::FindSamplerViewForHandle(HandleOfSamplerViewForTexture(object))`, i.e. Espryt's
  own table asked the way Espryt asks it. It deliberately does **not** read the record's `ViewCso`,
  which is the client's statement about the same fact and would make one side of the seam vouch
  for the other.
* `PeekPipeApplierRefusedNoConsumer(out)` — c0f's counter, for §3.2.

**`TextureParamsWithoutASamplerViewScenario.cpp`**: each of the four cases now calls
`TakeTheWhiteBoxReadingBeforeAnySample(...)` at the point where its texture is reachable only its
own way and **before** the observing sample, asserting all three parts ID-31 names: (a) a record at
a non-zero `ParamsSerial` carrying the field the case moved; (b) the driver's value equals it
**now**; (c) **no sampler view exists for that texture yet**.

**A reading that cannot be taken is DECLINED BY NAME and the case continues.** This is a deliberate
departure from the `GTEST_SKIP` the review sketched, and the file header carries the argument:
these four cases are dual-purpose — they are also the end-to-end regression net around D10, and
that net is the *only* thing measuring D10 on exactly the arms where the peek cannot look (the pull
build; the `0x1ff` and `0` lanes; Magma). Skipping the case there would delete the one verdict
those lanes carry in order to report the absence of a second one. Every decline is printed and
`RecordProperty`'d, so a lane that silently stopped taking the reading is visible in the log rather
than in a count.

The header also carries **esprytobj re-review N-9**, as asked: D's unit probe drives
`SyncTextureParamsToBackend` directly and so sees only a deferral *inside* that function; these
readings run through the real per-frame paths and also see one introduced *above* it —
`SyncNeccessaryTextures`, the attachment walk, E's per-unit walk. Neither half covers both alone.

**Proof 1 — the artefact, and it was a measurement rather than a mutation.** On the base before E
landed, `AReadAttachmentOnlyTexturesDepthStencilModeReachesTheDriver` was red on **exactly one
assertion, the white-box one**, while its public-GL assertion was **green in the same run**:

```
[ TextureParamsWithoutASamplerView ] white-box: texture 1 -> applier handle {15, 1}
  paramsSerial 5, Espryt ES name 1, sampler view: none, GL_DEPTH_STENCIL_TEXTURE_MODE applied
  before any sample
...:437: Failure   applied.DepthStencilMode which is 6402 (GL_DEPTH_COMPONENT)
                   expected 6401 (GL_STENCIL_INDEX)
```

The applier's record carried `GL_STENCIL_INDEX`, Espryt held no sampler view for the texture, and
the driver was still at the GL default — and the sample at the end of the case repairs it, which is
precisely why the end-to-end half passed. **That pair is R1's gap, measured, and it is the
"落地前必须红" artefact ROADMAP.md:20 asks for**: no public-GL case on a monolith tree can produce
it, and this is the first time it has been produced.

**Proof 2 — it closed with package E, predicted then observed.** Before E landed, a throwaway
worktree = F's head + `git merge refs/heads/p4a/esprytdraw` gave **4/4 green** (measured twice, on
`29d51ab9` and on `29bd4d2c`). E's `SyncReadFramebufferTextureAttachments` is the read-side sync
list that closes D-E3's gap. E is now integrated at `41b79050` and the case is green on the real
tree, with no change to F.

**Proof 3 — mutation**, applied in place, restored, never committed, and re-taken on the final base:
gating Espryt's `SyncTextureParamsToBackend` on `FindSamplerViewForHandle(...) != nullptr` — the
sampler-view coupling ARCHITECTURE.md:100 (D10) exists to remove — makes **all four cases red**,
with **10** `ESPRYT HAS NOT APPLIED …` white-box assertion failures **and 4** of the cases'
public-GL messages. Restored, rebuilt, re-run: **6/6 green**, `git status --porcelain` empty.

### 3.2 c0f's belt, pinned in a lane (ID-40)

New case `ObjectSubsystemControlScenario.TheAppliersNoConsumerBeltNeverFiresBehindTheClientsGate`
and two new lanes at the **phase default** `0x1fff`:

* **`DirectVulkan.ObjectSubsystemControl.NoConsumer.`** — the backend *without* a consumer, and the
  first DirectVulkan entry in this family. A non-zero `RefusedNoConsumer` delta there means the
  client's gate leaked and only the belt stopped a dirty flag being cleared: ID-39's bug caught one
  layer later, and invisible in these pixels *because* the belt does its job (the 66 red cases were
  in another suite entirely).
* **`DirectGLES.ObjectSubsystemControl.Consumer.`** — the control, so that a green cannot also mean
  "nothing is ever emitted anywhere", which is what a gate that had become accidentally
  always-false would look like.

Read as a **delta** across the workload: the counter is process-global and `MGPipeApplierReset`
zeroes it, so a value that went *down* is read as "the applier was reset, everything since is
`after`". Both lanes pass, delta **0**, and both draw the same green every other lane draws. The
CMake comment says why the block's "DirectGLES only" rule does not apply to these two.

### 3.3 wire MINOR-1 — the cube-face erase key (ID-27, integrator grant)

`MG_Test/Pipe/TextureEmitTest.cpp`: **one case added, nothing else in the file changed**.
`TextureEmit.ARespecifyOfOneCubeFaceKeepsTheOtherFacesUploadOfTheSameLevel` puts two cube faces of
**the same level** into `PendingUploads` — the ordinary shape of building a cube map, six
`glTexImage2D` calls at level 0 — and then respecifies one of them with a moved `InternalFormat`
(a real storage redefinition, not ID-18 M4's metadata update, which drops nothing and would make
the case vacuous the other way).

**The SECOND-added face is the one redefined, and that choice is the whole mutation sensitivity.**
The erase walks the vector in order and `break`s at the first match, so a level-only key still
removes exactly one entry — and if the redefined face were first it would remove the *right* one by
insertion-order accident. Measured: the first draft redefined `+X` and was **green** under the
mutation. It now redefines `-X` and asserts the survivor's identity, not only the count.

**Proof**, in place and restored, re-taken on the final base: deleting the
`it->UploadTarget != level->UploadTarget ||` half of the key at `MG_Pipe/PipeApply.cpp:1690` makes
**the new case red and `ARespecifyOfOneLevelKeepsThePendingUploadsOfTheOthers` stay green** — which
is exactly the review's point, that the existing case cannot see this half. Restored, rebuilt,
green, `git status` clean.

### 3.4 The SamplerCso leak case was measuring C's cache, and is fixed

`DestroyedSamplersReturnTheirSamplerCsoSlots` went red the moment package C's emitter was in the
base — *"48 SamplerCso objects were created, drawn with and destroyed and 14 slots never came
back"*, live 7 → 21, high water 9 → 23. It is **F's own case and F's own defect**, not C's:
`SamplerCso` is minted by a **content-addressed cache** (D-F1, ID-17's reference-count ruling), so
destroying the frontend sampler releases its reference and leaves the entry in the cache,
unreferenced, until LRU eviction at capacity 256. The case's churn cycles the LOD bias
`round % 16` — **16 distinct contents** — and 14 is those minus the two the warm-up had already
interned. A flat "every slot comes back" assertion there measures the cache's capacity policy and
calls it P3a's C-1 leak.

The fix (`4678519f`) is not a weaker assertion but a **warmer cache**:
`AssertChurnReturnsEverySlot` gains a `warmUpRounds` parameter (default 2, unchanged for the other
six kinds) and the sampler case passes `16 + 2`, so the baseline is taken with every distinct
content already interned. The three assertions then say something *stronger* than they can for a
per-object kind — **a warm content-addressed cache must not grow at all under churn** — and an
unbounded leak, which is what C-1 is about, still moves every one of them.

---

## 4. The four minors, closed — and the four defects the gates caught in passing

### 4.1 F-v2-m1 — the refusal matcher now reads the direction

`ObjectSubsystemControlScenario.cpp:196-266`: `LineNamesTheBit` and its symmetric conjunction are
gone, replaced by `EarliestSpellingOffset` plus an **ordering** test in `FindTheRefusalLine`. A line
matches only when, in this order:

`<set-bit spelling>` … `" is set but "` … `<needed-bit spelling>` … `" is clear"`

and it also carries the helper's own decision clause verbatim —
`"REFUSING the dependent bit and running the legacy arm"` — instead of the old loose `REFUS`/`refus`
substring. The **earliest** offset is taken per bit on purpose: Espryt's bit-10-requires-bit-11
sentence says *"only bit 11 mints sampler CSOs"* in its reason, and a later occurrence must not be
able to satisfy an ordering the first does not.

Both lanes find their own sentence and print it. The 0x5ff one, read out of the lane's log:

```
[Linux MobileGLIntegra/ERROR]: MGPipe: kMGPipeSubsystemTextureResources (bit 10) is set but
kMGPipeSubsystemSamplers (bit 11) is clear; MGPTextureParams::BuiltinSampler is a SamplerCso
handle, only bit 11 mints sampler CSOs, and the applier's verdict for a null one is
Fatal{ProtocolCorruption} - REFUSING the dependent bit and running the legacy arm. Set both
bits, or clear both
```

Swapping a caller's two arguments now breaks the chain, which is what the two cases claimed to be
doing and could not.

### 4.2 F-v2-m2 — the 0x5ff arm's wording, and the sentence it asserts

The scenario's comment block (`:681-694`) now says the arm **arms and passes** at integration and
that a red on the refusal assertion is a real finding about D's resolver; the call site (`:742-752`)
quotes Espryt's sentence verbatim, read out of `Managers.cpp`'s
`ResolveTextureResourceSubsystemArm`. `gates-v2.md` §4 and its §7 row carry `[CORRECTED …]` markers.
The lane is **green** on the final base, on both of its assertions.

### 4.3 F-v2-m3 — the wrapper is gone

`MagmaPipeArms.h`: `MagmaPipeAbaControlCoversKind` deleted, with the reason in its place. The knob
has exactly two consumers (`VulkanRenderer.cpp:3653`, `VertexInputStateFactory.cpp:67`) and each
holds one kind — `VertexElementsCso` — by construction, so the conjunction was not a variable at
either site and no build evaluated it; it is not `constexpr` (it reads `MG_Config::Features`) so no
assert could reach it either. The two pieces stand alone and each is pinned by something that runs:
the `constexpr` per-kind table with its three `static_assert`s, and the knob its consumers read.
`grep -r MagmaPipeAbaControlCoversKind MobileGL/`: 0 hits. G1 is unchanged.

### 4.4 F-v2-m4 — `REPAIR_RC` is read

`scripts/p4a_descriptor_negative_control.sh`: the flag is documented as sticky (`:184-188`), read
after the control loop (`:499-504`, forcing `WORST=2`), and read by the **EXIT trap** (`on_exit`,
`:242-250`), which `exit 2`s over whatever status an early exit had chosen. That last one is the
path the review asked for: a repair that fails while the script is on its way out through an early
`exit` can no longer leave a corrupted build directory behind a 0 or a 1. `bash -n` clean.

### 4.5 Four defects the gates caught, and where each was fixed

All four were red on one of the two intermediate bases and are green on `41b79050`. Three were
fixed by the integrator's D/E verification commits, independently and with the same diagnosis.

| what F measured, and where | fixed by |
|---|---|
| **At `MOBILEGL_PIPE_PUSH=0x5ff` the client emitted the texture family while Espryt refused bit 10 and ran its legacy arm.** The lane's own log carried `emit[fbe=0 sve=0 sse=0 sie=0 ctu=5]` on the very run that carried the refusal line; the applier accepted, the client cleared the level's dirty flags on that acceptance, and the legacy upload path found nothing to upload — the sampled texture was **black** (11625/11625 pixels). ID-39's failure mode with the consumer signal replaced by the dependency. The mirror at 0x9ff was benign (`sve=2 sse=2`, nothing carrying texels, pixels green). Measured on `29d51ab9` | **`29bd4d2c`** — "gate the four P4a families on D-K2's dependency bits as well", in `PipeFill.cpp`: the site and shape F's diagnosis named. `DirectGLES.ObjectSubsystemControl.RefusedTexture.` is now the pin, and its `emit[…]` bracket is the only place this shape is visible |
| **G2 was broken by one name.** `TextureEmit.WithNoBackendConsumerTheFamilyGateIsFalseAndNothingReachesTheApplier` (c0f's own unit case) sat inside `TextureEmitTest.cpp`'s push-only half and was never added to `MGL_TEXTURE_EMIT_CLIENT_TEST_LIST`, so pull named 2842 and push 2843. Measured on `29d51ab9` | **`245daca0`** + **`ee31944b`** — the pull-build skip twin. G2 is now **2846 == 2846, zero diff** |
| **The built-in-sampler seam was a four-case seam, not ID-39's two.** F's `HandleRecycle.Handles.…ASamplerAtARecycledAddressDoesNotInheritItsPredecessorsParameters` was red with `expected=FRESH observed=NEITHER`, first offender **blue** — the texture's own colour, i.e. a plain sampler object's `GL_CLAMP_TO_BORDER` + float `GL_TEXTURE_BORDER_COLOR` never reached the driver at all, in the **warm-up** frames, before any recycling. Not confined to the integer-sampler path ID-39 named. Measured on `29d51ab9`, and still red with E's branch merged at that point | **`1e8cdc59`** (D) and **`41b79050`** (E) — "the twin resolved a content-addressed record by an identity handle". All four sampler-shaped cases are green now |
| **D-E3's read-attachment gap, measured white-box** — §3.1. Red on `29d51ab9` and on `29bd4d2c`, green on both merged-with-E worktrees, green on `41b79050` | **`41b79050`**'s branch — E's `SyncReadFramebufferTextureAttachments` |

---

## 5. Section 6, re-taken on `4678519f`

`GLIBC_TUNABLES=glibc.malloc.tcache_count=0` exported for every build and every run;
`CCACHE_BASEDIR=/home/swung/w7`, `-j 8`, Release/clang/Ninja, llvmpipe + lavapipe.

| gate | result |
|---|---|
| three builds | `build-linux` / `build-push` / `build-verify` **rc 0** |
| **G1** `symbol_report.py --before ~/w7/p4a-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | **0 added / 0 removed / 0 resized / 0 renamed**. `.text` 10806323 → 10806323 (+0). 27811 → 27811 defined symbols |
| `ctest -L unit` × 3 | `build-linux` **1767**, `build-push` **1767**, `build-verify` **1767** — 100%, 0 failed in each |
| **G2** pull vs push names | **2846 == 2846, diff empty** |
| **G14** vs `~/w7/p4a-before-ctest-names.txt` (37da3c3a, `LC_ALL=C`) | **0 removed**, **+258 added**. F v3's own share is **+3**: the two consumer lanes and the cube-face unit case |
| **G5** `p4a_untouched_regions.sh 37da3c3a HEAD` | **rc 0**, **17 rows** |
| **G5** `--self-test` | **rc 0**, 3 positive + **8** negative controls, all tripped and all named |
| `p3a_untouched_regions.sh 37da3c3a HEAD` / `--self-test` | **rc 0** / **rc 0** |
| `check_include_closure.py --mode text --self-test --require-all --expect-probes 4` | **rc 0**; `--expect-probes 5` **rc 1** |
| **G7** `scripts/g7_negative_control.sh build-push` (P2) | **rc 0** |
| **G7** `scripts/p3a_vertex_input_negative_control.sh build-push` (P3a) | **rc 0** |
| **G7** `scripts/p4a_descriptor_negative_control.sh build-push` (P4a) | **rc 0 — `Layered` tripped, `borderColorForm` tripped**, "both controls tripped and named their field; exit 0" |
| `scripts/p4a_descriptor_negative_control.sh build-linux` | **rc 2**, refused as a non-push build directory (F-m10) |
| **G7 SIGINT**, delivered to the process group mid-rebuild from a new session with the child's disposition reset to `SIG_DFL` | the script **died by SIGINT** (`returncode -2`, i.e. 130), the repair ran and printed *"restored …; rebuilding build-push from it"*, and **`git status --porcelain` was empty**. No summary is printed on that path, which is correct: exit 2 *with* the interruption wording is the signalled-**child** path, not this one |
| `ctest --test-dir build-push -L integration-gpu -j 8 --no-tests=error` (default `0x1fff`) | **1079/1079** |
| the same at `MOBILEGL_PIPE_PUSH=0x1ff` / at `0` | **1079/1079** / **1079/1079** |
| `ctest --test-dir build-linux -L integration-gpu` (PULL) | **1079/1079** |
| **DirectVulkan lane** `-R '^DirectVulkan'` | **540/540** (c0f holds; it was 475/475 before F's lanes added names) |
| DirectGLES lane `-R '^DirectGLES'` | **539/539** |
| `ctest --test-dir build-verify -L integration-verify -j 4` | **884/884**, **zero `Fatal` lines** |
| `ctest -R HandleRecycle` push / verify / pull | **144/144** / **180/180** / **144/144** |
| `HandleRecycle.Handles` push | **36/36** — ID-38/ID-39's six armless `.Handles` aborts are closed by the itest CMake's `0x1fff` phase pin |
| `ctest -R ObjectSubsystemControl` push | **14/14** (both refusal lanes, both consumer lanes) |
| `ctest -R 'TextureParamsWithoutASamplerView\|TextureUploadShape'` push | **11/11** |
| the four families, pull / verify | **169/169** / **223/223** |
| `ctest -N` totals | push **2846**, verify **3730** |
| working tree / attribution / message shape | clean; no `Co-Authored-By`; fifteen single-line messages |

**Recorded, not gated.** `TextureUploadShape` on DirectGLES/llvmpipe now reads
`server[emit=6 box=6 rect=0 jobs=6] client[ctu=6]`. The client half was `ctu=0` at v2 (no emitter
on that base) and now agrees with the server's count — which is exactly what F-m7's build probe was
added to make readable, and it is the first tree on which the two numbers can be compared at all.

Re-confirmed and worth restating: **a ctest `ENVIRONMENT` property overrides the job env**, so the
`HandleRecycle.Handles`, refusal and consumer lanes keep their own `MOBILEGL_PIPE_PUSH` whatever
outer arm the gate runs. That is why the `0x1ff` and `0` arms above do not move those lanes, and it
is deliberate.

---

## 6. What the integrator must re-run on the final tree

`gates-v2.md` §7 stands as amended there. On top of it:

| what | why |
|---|---|
| `ctest -L integration-gpu` on both builds and at the three masks | **1079/1079 everywhere** is the answer today. Anything less is a regression, not a known red: F leaves none |
| `diff <(ctest --test-dir build-linux -N) <(ctest --test-dir build-push -N)` | G2 must stay empty. Extract names with `sed -n 's/^ *Test *#[0-9]*: //p'` and `LC_ALL=C sort`; `awk '{print $3}'` truncates the five parameterised `Shapes/DemoteFloat64EsslTest…` names and manufactures a spurious 5 removed / 5 added |
| `bash scripts/p4a_descriptor_negative_control.sh build-push` | **rc 0 naming `Layered` and `borderColorForm`** is now the answer, not rc 2. rc 2 with the contract-tree wording means an emit header went back to a stub; rc 2 with the *interruption* wording means the run was killed and reported nothing about the controls |
| `bash scripts/p4a_untouched_regions.sh 37da3c3a HEAD` **and** `--self-test`, at **every** integration step | 17 rows / 8 negative controls. The cheapest check that a merge did not quietly touch Espryt's protected regions |
| the `0x9ff` and `0x5ff` lanes **with the lane logs kept** | The matcher pins the DIRECTION and the decision clause, so the two lanes accept disjoint sentences. Read the printed `refusal line:` **and** the `emit[…]` bracket in the same log — the bracket is what caught the client/server disagreement in §4.5, and it is the only place that shape is visible |
| `ctest -R 'ObjectSubsystemControl.(Consumer\|NoConsumer)'` | Both must pass with `refused_no_consumer_delta` **0**. A non-zero on the NoConsumer lane is the gate/belt disagreement c0f closed, reappearing |
| `ctest -R HandleRecycle` on all three builds, reading the printed `[ HandleRecycle ] … live A -> B (peak P), high water H1 -> H2` lines | The SamplerCso case now takes its baseline with the content cache warm (§3.4). A `live` that grows there again **is** a real leak, because every content is already interned |
| `ctest -R 'TextureParamsWithoutASamplerView'` **and read the case's stdout** | Each case prints one `white-box:` line naming the applier handle, the params serial, Espryt's ES name and "sampler view: none". A `white-box reading DECLINED` line on the push build's DirectGLES lanes means the peek stopped being able to look, which a green count will not tell you |
| `ctest --test-dir build-verify -R 'EvictedPipelineComposites'`, reading the band line | Unchanged from `gates-v2.md` §7: a high water still equal to the floor (983040) means C's composite resolver is not minting through `AllocateComposite` and the case is skipping, not passing |
| grep the configure output for `MOBILEGL_PIPE_HANDLE_ABA_CONTROL steers <backend>` | Unchanged. F-M5's probe is directory-wide |
| the CI `pipe-gates` include-closure step | `--expect-probes 4` is pinned and must change in the same commit as `check_include_closure.py`'s `PROBES` list. Do **not** "fix" `clang++-20` to bare `clang++` on the strength of a local failure (ID-9 D16 / ID-11) |
| a SIGINT test of the G7 script, if re-verified | Launch it with SIGINT deliverable **to the process group**, from a new session, with the child's disposition reset to `SIG_DFL`. A `&` from a non-interactive shell makes the trap unreachable and the test measures nothing |
| **G1, the three `-L unit` runs, the pull/push name diff and the G14 delta, re-taken** | They move with whatever lands after this head |

**Not re-run here, and still the integrator's (ID-3):** D.3/D.4's retrace sweeps, the device A/B,
DriverBench, the APKs and the docs pass.

---

## 7. Method note

Every mutation in this report was applied to the working tree, measured, restored, rebuilt and
re-run green, with `git status --porcelain` empty afterwards — the same discipline the two shipped
negative-control scripts use, and the check is stated in each subsection. Nothing was committed.
Both mutation proofs were re-taken after the final rebase, on the head this report names. The
E-merge measurements used throwaway `git worktree`s whose submodules were symlinked at the gates
tree's checkouts; they were removed and pruned, as have been every log and helper this round wrote
under `~/w7`.
