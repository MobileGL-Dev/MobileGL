# P2 package C — `p2/espryt` (Espryt 0b, the first Track H slice) — result v4 (rework round 4)

Tree `~/w7/p2-espryt`, branch `p2/espryt`, base tag `p2/contract` (`9c6a8a25`), HEAD **`a859ff71`**.
Build directories: `build-linux` (pull, the G1 build), `build-push`, `build-verify`
(`-DMOBILEGL_PIPE_VERIFY=ON -DMOBILEGL_ITEST_REQUIRE_GPU=ON`) and `build-nolegacy`
(`-DMOBILEGL_PIPE_PUSH=ON -DMOBILEGL_PIPE_LEGACY_MEMOS=OFF`). `build-nolegacy/` is an untracked
build directory, not content, and is not committed. `git status --porcelain` at the end of the
round: only `?? build-nolegacy/`, exactly as at the start.

This round answers the three majors of `espryt-review-v3.md` and five of its minors.

---

## 1. Commits

Four new on top of the nine from rounds 1–3.

| sha | subject |
|---|---|
| `dc9b7237` | `[Feat] (Espryt): give the backend a dense {slot, gen} twin table beside the address-keyed registry` |
| `fbdaeef3` | `[Refactor] (Espryt): key every backend twin on {slot, gen} instead of the frontend object heap address` |
| `1e0f4d6d` | `[Test] (Espryt): pin the twin table identity contract …` |
| `d714600a` | `[Fix] (Espryt): do not return a reference through a null slot pointer …` |
| `d89fb684` | `[Fix] (Espryt): sweep the twin table on object churn again, refuse to run an arm the operator disabled …` |
| `5f245ac7` | `[Test] (Espryt): pin the churn-driven sweep cadence, the null tolerance …` |
| `3e59a856` | `[Test] (Espryt): run the sanity binary on the twin arm it was compiled for, and pin that it does` |
| `103cafed` | `[Fix] (Espryt): tell the backend when a frontend object dies instead of discovering it in a garbage sweep` |
| `994730a7` | `[Fix] (Espryt): pick the unit-bindings debounce by the runtime arm …` |
| **`838aba10`** | **`[Fix] (Espryt): stop the armless knob pair inside the test that needs an arm, not inside the EGL bring-up a forked pre-flight swallows`** — MAJOR 1 |
| **`e04c5c3e`** | **`[Fix] (Espryt, State): let the last four object classes announce their own death and delete the twin table's garbage collector`** — MAJOR 3 |
| **`fbc2fb65`** | **`[Test] (Espryt): drive the acquire / look up / delete / re-acquire walk through the real registry for all six re-keyed kinds`** — MAJOR 2 |
| **`a859ff71`** | **`[Fix] (Espryt): close four review minors on the twin table …`** — minors 1, 2, 4, 5, 14 |

`git diff --stat p2/contract..HEAD`:

```
 MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp                        377 +++++-
 MobileGL/MG_Backend/DirectGLES/Managers.cpp                          135 ++++-
 MobileGL/MG_Backend/DirectGLES/Managers.h                            169 ++++-
 MobileGL/MG_Backend/DirectGLES/SlotTables.h                          360 +++++   (new)
 MobileGL/MG_State/GLState/FramebufferState/FramebufferObject.{h,cpp}  21 +       (new this round)
 MobileGL/MG_State/GLState/ProgramState/ProgramObject.cpp              16 +-
 MobileGL/MG_State/GLState/RenderbufferState/RenderbufferObject.{h,cpp} 21 +
 MobileGL/MG_State/GLState/SamplerState/SamplerObject.{h,cpp}          21 +       (new this round)
 MobileGL/MG_State/GLState/StateObjectDeathNotice.h                    62 +       (new file)
 MobileGL/MG_State/GLState/TextureState/TextureObject.{h,cpp}          23 +       (new this round)
 MobileGL/MG_State/GLState/VertexArrayState/VertexArrayObject.{h,cpp}  22 +       (new this round)
 MobileGL/MG_Test/SanityTest.cpp                                      670 +++++++
 17 files changed, 1854 insertions(+), 43 deletions(-)
```

`git diff --summary p2/contract..HEAD` prints only the two `create mode 100644` lines for the two
new files — **no file-mode change**.

---

## 2. The three majors

### MAJOR 1 — a knob pair greened the whole DirectGLES lane by skipping it (FIXED, `838aba10`)

**The finding, restated.** `d89fb684` raised `Fatal{PipeLegacyMemosDisabled}` from
`ResolveEsprytSlotTablesArm()`, and `InitDisplayAndContext()` forced that resolution — i.e. the
stop lived inside EGL bring-up. The integration harness pre-flights EGL bring-up in a **forked
child** (`MG_IntegrationTest/Harness/HeadlessGL.cpp:405-415`) and converts a child that dies on a
signal into "no usable GPU/display/ICD", which `ScenarioFixture.h:83` turns into `GTEST_SKIP` for
every scenario. So `MOBILEGL_PIPE_PUSH=0 MOBILEGL_PIPE_LEGACY_MEMOS=0 ctest -L integration-gpu`
reported *100 % tests passed* having run nothing — on the exact pair of env vars the D14/D18 A/B is
driven with. `ROADMAP.md:7` forbids that.

**The fix — split diagnosis from stop.** The arm decision becomes a pure function of the two knobs:

```cpp
enum class EsprytSlotArmVerdict { Handles, Legacy, NoArm };
EsprytSlotArmVerdict ClassifyEsprytSlotArm(Bool subsystemBitSet, Bool legacyMemosEnabled);
EsprytSlotArmVerdict CurrentEsprytSlotArmVerdict();
void DiagnoseEsprytSlotArm();          // names both knobs at ERROR, RETURNS
Bool ResolveEsprytSlotTablesArm();     // names both knobs at FATAL, STOPS
```

`InitDisplayAndContext()` now calls **`DiagnoseEsprytSlotArm()`** — the operator is told, by name,
in the log, before the first draw, and bring-up survives, so the pre-flight child exits 0 and the
harness comes up. The stop stays in `ResolveEsprytSlotTablesArm()`, which the inline latch
`EsprytSlotTablesEnabled()` reaches at the **first twin lookup** — inside a scenario body, where a
crash is a test failure. A process that never looks a twin up never needs an arm and is no longer
stopped by one it would not have used; that is the only behaviour this moves.

**Proof — the same command, now RED** (`~/w7/p2-espryt`, `MOBILEGL_BACKEND_TYPE=DirectGLES`,
`__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json`):

```
$ MOBILEGL_PIPE_PUSH=0 MOBILEGL_PIPE_LEGACY_MEMOS=0 \
    ctest --test-dir build-push -L integration-gpu -R '^DirectGLES\.CrossFrameBufferScenario' --no-tests=error
0% tests passed, 13 tests failed out of 13
	1513 - DirectGLES.CrossFrameBufferScenario.VertexBufferSubData (Subprocess aborted) integration-gpu
	… all 13, (Subprocess aborted)
```

(v3 printed `100% tests passed, 0 tests failed out of 13`, all `(Skipped)`.)

The harness now *comes up* under that pair — the pre-flight child survives and the parent prints
`renderer: Espryt (MobileGL Core) (llvmpipe …)` — and the abort happens inside the scenario body.
With a log path given, the message names both knobs:

```
$ MOBILEGL_PIPE_PUSH=0 MOBILEGL_PIPE_LEGACY_MEMOS=0 MOBILEGL_LOG_FILE_PATH=$HOME/w7/armless.log \
    ./build-push/…/MobileGLIntegrationTest --gtest_filter=CrossFrameBufferScenario.VertexBufferSubData
binary exit=134
[…/ERROR]: MGPipe: PipeLegacyMemosDisabled - MOBILEGL_PIPE_PUSH leaves kMGPipeSubsystemEsprytSlots
  (bit 5) clear and MOBILEGL_PIPE_LEGACY_MEMOS=0 makes the legacy twin registry unreachable, so this
  context has no twin table arm at all; the first twin lookup will stop the process
[…/FATAL]: MGPipe: Fatal{PipeLegacyMemosDisabled, "MOBILEGL_PIPE_PUSH leaves
  kMGPipeSubsystemEsprytSlots (bit 5) clear and MOBILEGL_PIPE_LEGACY_MEMOS=0 makes the legacy twin
  registry unreachable, so there is no twin table arm to run"}
```

Control, same binary, one env var removed: `[ PASSED ] 1 test.`

**And an always-on ctest entry**, `DirectGLESSlotTable.AnArmlessKnobCombinationStopsInsteadOfSkippingTheLane`,
which is what stops this regressing. It asserts (a) the pure classifier over all four knob
combinations, (b) that `DiagnoseEsprytSlotArm()` **returns** — if it ever stops again, that line
takes the case down — and (c) that `ResolveEsprytSlotTablesArm()` dies of `SIGABRT` **and** that
the log line it wrote names `PipeLegacyMemosDisabled`, `MOBILEGL_PIPE_PUSH`,
`MOBILEGL_PIPE_LEGACY_MEMOS=0` and `kMGPipeSubsystemEsprytSlots`. It pins the message and not only
the signal, because an operator handed a bare "Subprocess aborted" has been told nothing. It skips
visibly in the pull build and in `build-nolegacy` (no legacy arm ⇒ no armless combination), so G2
name parity holds. Negative control NC5 below.

**Note on the message channel** (a limit, stated rather than hidden): `MOBILEGL_LOG_ENABLE_CONSOLE`
is `0` (`Defines.h:71`) and `MOBILEGL_LOG_FILE_PATH` is empty off Android, so on desktop the two
lines above reach a file only when the operator sets `MOBILEGL_LOG_FILE_PATH`. Without it the ctest
output is `(Subprocess aborted)` with no text. The library cannot fix that from `MG_Backend/` —
`pipe-gates`' stdio grep (`test.yml:1515-1522`) bans every stdio spelling there, and MGLOG is the
only sanctioned channel. Two things would close it and both belong to other owners: package E
adding `MOBILEGL_LOG_FILE_PATH` to `MGL_ITEST_GLES_ENVIRONMENT` (it already does exactly that for
four other lanes, `MG_IntegrationTest/CMakeLists.txt:363-391`), or the harness pre-flight
distinguishing "the library refused this configuration" from "no GPU".

### MAJOR 2 — five of six re-keyed kinds had no case that could go red (FIXED, `fbc2fb65`)

**The finding, restated and confirmed.** The D13 "must not break" pins make **zero** slot
acquisitions: the scratch-FBO scrub and the three context-generation guards build their twins with
`MakeShared<…BackendTextureObject>()` directly or drive `ScratchFBOImpl` under
`ScopedStateGuardMocks`, so they never reach `StateBackendObjectRegistry` and pass identically on
both arms. The four acquisitions in the whole binary were all kind `Texture`. The eleven
`DirectGLESSlotTable` cases drive `BackendSlotTable` directly on the throwaway kinds `Query` /
`Fence`, so they would have passed had the six registries never been re-keyed.

**The fix.** `DirectGLESSlotTable.EverySwitchedOverKindResolvesItsTwinThroughTheHandleArm` runs the
whole walk — acquire, look up by object, look up by handle, delete, re-acquire — through the **real
registry global the shipping paths call**, for each of the six kinds:

| kind | registry driven | frontend object built |
|---|---|---|
| `Texture` | `TextureImpl::g_backendTextureObjects` | `TextureObject2D` |
| `Framebuffer` | `FramebufferImpl::g_backendFramebufferObjects` | `FramebufferObject` |
| `Renderbuffer` | `RenderbufferImpl::g_backendRenderbufferObjects` | `RenderbufferObject` |
| `SamplerCso` | `SamplerImpl::g_backendSamplerObjects` | `SamplerObject` |
| `ShaderCso` | `PrgramImpl::g_backendProgramObjects` | `ProgramObject` |
| `VertexElementsCso` | `VertexArrayImpl::g_backendVertexArrayObjects` | `VertexArrayObject` |

Per kind it asserts: `GetOrCreate` mints a **non-null** `{slot, gen}` (the legacy arm answers
`kMGPipeNullHandle`, so a kind still on it fails **by name**); `Find(object)` and
`FindByHandle(handle)` return the *same* twin-storage address `GetOrCreate` handed back; the
object's own destructor notice frees the slot with **no** `CollectGarbage*` call anywhere; the
successor lands on the freed slot with a **moved `Gen`** while the predecessor's handle resolves to
nothing. No backend twin is constructed — each of the six twin classes mints a driver id in its
constructor and none of that is what was re-keyed.

**Breakpoint evidence** (`gdb -batch`, `ignore N 100000000`, `info breakpoints`):

| binary / filter | `MGPipeSlotAllocator::Acquire` | `ResolveEsprytSlotTablesArm` |
|---|---|---|
| `build-push`, `--gtest_filter=…EverySwitchedOverKindResolvesItsTwinThroughTheHandleArm` | **12** | 1 |
| `build-push`, `--gtest_filter=-DirectGLESSlotTable.*` (the other 82 cases) | 4 | 1 |

12 = six kinds × the two objects each. The 4 for the rest of the binary is the v3 figure, unchanged
— i.e. this one case triples the binary's coverage of the re-keyed path and is the only case that
reaches five of the six kinds. It skips visibly on the legacy arm (`MOBILEGL_PIPE_PUSH=0`) and in
the pull build. Negative control NC6 below takes one kind (`Framebuffer`) back off the handle arm
and this is the only case that fails.

### MAJOR 3 — `e2` for two kinds of six, and the GC still the primary death signal (FIXED, `e04c5c3e`)

**The firing side, all six.** The four remaining frontend destructor sites now raise the notice, on
`RenderbufferObject`'s pattern (out of line, declared only under `#if MOBILEGL_PIPE_PUSH`, so the
pull build keeps its implicit destructor):

| kind | site |
|---|---|
| `ShaderCso` | `MG_State/GLState/ProgramState/ProgramObject.cpp` — `~ProgramObject` (round 3) |
| `Renderbuffer` | `…/RenderbufferState/RenderbufferObject.cpp` — `~RenderbufferObject` (round 3) |
| **`Texture`** | `…/TextureState/TextureObject.cpp` — **`~TextureObjectBase`**, the one base every concrete texture (2D, 3D, cube, buffer, view) derives from, so it announces once per object |
| **`Framebuffer`** | `…/FramebufferState/FramebufferObject.cpp` — `~FramebufferObject` |
| **`SamplerCso`** | `…/SamplerState/SamplerObject.cpp` — `~SamplerObject` |
| **`VertexElementsCso`** | `…/VertexArrayState/VertexArrayObject.cpp` — `~VertexArrayObject` |

**The garbage collector is gone on the handle arm.** `BackendSlotTable` loses **both** drivers:
`kGCInterval`, `kCreationGCInterval`, `m_gcTick`, `m_creationTick`, the creation tick inside
`GetOrCreate`, and the two test accessors that read the cadence. `CollectGarbageIfNeeded()` is an
empty function. `CollectGarbageNow()` survives as an **explicit** collection — someone asked, so it
runs; nothing calls it on a tick.

**The seven `CollectGarbageIfNeeded` call sites, one by one.** All seven still stand, all seven now
drive the *legacy* registry only. On the handle arm each is `if (EsprytSlotTablesEnabled()) return;`
— one perfectly-predicted branch — and `StateBackendObjectRegistry::CollectGarbageIfNeeded`'s legacy
body is now wrapped in `#if MOBILEGL_PIPE_LEGACY_MEMOS`, so a build without the legacy arm
(`build-nolegacy`) carries **no collector at all**:

| site (HEAD) | enclosing function | registry | fate |
|---|---|---|---|
| `DirectGLES.cpp:1275` | `ResolveVaoTwin` | `g_backendVertexArrayObjects` | legacy-arm driver; no-op on the handle arm; compiled out with `MOBILEGL_PIPE_LEGACY_MEMOS=OFF` |
| `:1715` | `SyncNeccessaryTextures` | `g_backendTextureObjects` | as above |
| `:2080` | `SyncCurrentFBO` | `g_backendFramebufferObjects` | as above |
| `:2081` | `SyncCurrentFBO` | `TextureImpl::g_backendTextureObjects` | as above |
| `:2082` | `SyncCurrentFBO` | `RenderbufferImpl::g_backendRenderbufferObjects` | as above |
| `:2910` | `SyncCurrentProgram` | `g_backendProgramObjects` | as above |
| `:2911` | `SyncCurrentProgram` | `SamplerImpl::g_backendSamplerObjects` | as above |

**The one sweep that remains, and why.** `ReclaimDeadSlots()` is kept as the body of the explicit
`CollectGarbageNow()`, and the `std::weak_ptr` per entry is kept, for exactly two jobs:

1. `ForEachLive()` hands its callee a **strong** reference to the frontend object, which the one
   direct-iteration site (`ScopedDetachedTextureFramebufferAttachments`, `DirectGLES.cpp:6711`)
   needs. That is a P3+ shape problem (a server cannot hold a client object at all), already
   recorded as debt 4 below — not a garbage collector.
2. A notice that could not be delivered: a destructor that runs after `exit()` has begun, where
   `InProcessTeardown()` drops the notice because a twin destructor must not call the driver. At
   that point the process is handing every GPU object back anyway.

Neither is periodic, so neither is a "GC" in `ROADMAP.md:18`'s sense. **What would remove even
these**: the `ForEachLive` caller re-expressed as a per-slot query (P3), and the teardown drop
replaced by an ordered shutdown that frees twins before `atexit` (P3+). Both are named in
`SlotTables.h`'s header.

**Ordering versus §E.** §E requires `e2` before `e3`. As landed, `e3` is commit 2 of 13 and `e2`
completes at commit 11 — the ordering rule is still violated *in the branch's history*, and that
cannot be undone without rewriting nine commits that three review rounds have already read. What §E
actually protects against — "`e3` merged with a slot table that has no garbage collector and no
explicit destroy" — is now false in the tree the integrator merges: at HEAD all six kinds announce,
the consumer is registered, and the sweep is retired. **The integrator should record this as an
ordering deviation on a squash-merge, not as a missing mechanism.**

**Tests.** `AProgramAndARenderbufferAnnounceTheirOwnDeath` becomes
`EveryReKeyedObjectClassAnnouncesItsOwnDeath` and drives all six classes, asserting by *membership*
rather than count or order — every `TextureObjectBase` owns a private `SamplerObject`
(`TextureObject.cpp:55`), so tearing a texture down legitimately raises a `SamplerCso` notice too.
`ObjectChurnAloneDrivesTheSweep` becomes
`AnnouncedDeathKeepsObjectChurnFromAccumulatingWithoutASweep`: 256 churned objects, one live twin at
a time, slot count back where it started, and **no** `CollectGarbage*` call anywhere in the case.
Negative control NC7 below.

---

## 3. Verification — every command and what it printed

All from `~/w7/p2-espryt` at `a859ff71`, `CCACHE_BASEDIR=/home/swung/w7`, with
`MOBILEGL_BACKEND_TYPE=DirectGLES` and
`__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json` for the GPU lanes.

### 3.1 Build and static gates

| gate | command | result |
|---|---|---|
| build | `cmake --build {build-linux,build-push,build-verify,build-nolegacy} -j 12` | all `rc=0` |
| **G1** | `symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | `27799 -> 27799 defined symbols: 0 added, 0 removed, 4 resized, 0 renamed`. The four: `RenderState::RenderState()` +148, `SetCapability` +77, `IsCapabilityEnabled` +29, `_GLOBAL__sub_I_DirectGLES.cpp` −9 — **all the contract's** |
| **G1 attribution** | same, `--before ~/w7/p2-contract/build-linux/libMobileGL.so` | `0 added, 0 removed, 0 resized, 0 renamed`, `.text +0` — package C's pull-build delta is still exactly zero, including the four new `MG_State` destructors |
| **G5** | `git show {48268068, p2/contract, HEAD}:…/DirectGLES.cpp \| awk '/namespace RenderStateImpl {/,/} \/\/ namespace RenderStateImpl/' \| sha256sum` | all three `d8fd1c48…220efe27`, equal to `~/w7/p2-before-syncrenderstate.sha` |
| **G13** | `grep -rc 'pGLContext' MobileGL/MG_Backend \| grep -v ':0$'` | empty (`rc=1`) |
| **G13** | `python3 scripts/check_include_closure.py` | `4 probes, 0 skipped, 0 problem(s)`, `rc=0` |
| **G13** | `gen_pipe.py --check` / `--self-test` | `rc=0` / `rc=0` (`7 negative-control trips, positive control OK`) |
| **G13 stdio** | the `pipe-gates` grep verbatim over `MG_Backend` + `MG_State` | **0 hits** (the nine "puts"-in-a-comment hits of v3 are gone: the alternation `(^\|[^[:alnum:]_>.:])puts[[:space:]]*\(` needs a `(`) |
| **G2** | `ctest -N \| grep -E '^[[:space:]]*Test[[:space:]]+#[0-9]+:' \| sed -E 's/^ *Test +#[0-9]+: //' \| LC_ALL=C sort` × 4, then `diff` | `build-linux` **2380**, `build-push` **2380**, `build-nolegacy` **2380** — both diffs IDENTICAL. `build-verify` 3198 (its verify-only lanes, by design) |
| **G14** | `LC_ALL=C comm -23 <sorted baseline> <pull names>` | **0** names removed. Added: the contract's 4 placeholders + this package's **13** `DirectGLESSlotTable.*` |

### 3.2 Tests

| lane | command | result |
|---|---|---|
| unit × 4 | `ctest --test-dir <d> -L unit --no-tests=error -j 6` | `100% tests passed, 0 failed out of 1502` on **build-linux, build-push, build-verify, build-nolegacy** |
| slot cases × 4 | `ctest --test-dir <d> -R DirectGLESSlotTable --no-tests=error` | `100% tests passed, 0 failed out of 13` in **all four** dirs (all 13 present and skipping in the pull build) |
| sanity, handle arm | `./build-push/…/SanityTest` | `[ PASSED ] 95 tests` |
| sanity, legacy arm | `MOBILEGL_PIPE_PUSH=0 ./build-push/…/SanityTest` | `[ PASSED ] 92`, 3 skipped (the three that are about the handle arm) |
| sanity, order independence | `--gtest_shuffle --gtest_random_seed={1..5}` | `95/95` × 5 |
| integration, handle arm | `ctest --test-dir build-push -L integration-gpu -j 4 -R DirectGLES` | **100 %, 446/446** |
| integration, legacy arm | same with `MOBILEGL_PIPE_PUSH=0` | **100 %, 446/446** |
| integration, no legacy compiled | `ctest --test-dir build-nolegacy …` | **100 %, 446/446** |
| integration, verify build | `ctest --test-dir build-verify -L integration-gpu -j 4 -R DirectGLES` | **100 %, 855/855** |
| **G4 (a)** | `ctest --test-dir build-verify -L integration-verify --no-tests=error -j 4` | **100 %, 818/818**; `grep -c 'Fatal{'` over all **10** lane logs = **0 each** |
| **G3 (DirectGLES half)** | `retrace_gate.py --tree ~/w7/p2-espryt --lib $PWD/build-push/libMobileGL.so --out ~/w7/retrace-out/v4-espryt -j 4 --only 'DirectGLES$'` | `rc=0`, **`passed 39 / 39; failed: []`** |
| **G4 (b), the retrace half — new this round** | `MOBILEGL_PIPE_VERIFY=1 retrace_gate.py … --lib $PWD/build-verify/libMobileGL.so --out ~/w7/retrace-out/v4-espryt-verify -j 4 --only 'DirectGLES$'` | `rc=0`, **`passed 39 / 39`**; of the 39 `*/DirectGLES/output/mobilegl.log`: **0** contain `Fatal{`, **0** contain `PipeVerifyDiffer\|UnmigratedPipeInput\|PipeResidualDiverged`, and **0 are unarmed** (every one carries `MGPipe: verify armed - 63 fields, 69 verbs, fatal=1`) |
| **MAJOR 1 red proof** | `MOBILEGL_PIPE_PUSH=0 MOBILEGL_PIPE_LEGACY_MEMOS=0 ctest --test-dir build-push -L integration-gpu -R '^DirectGLES\.CrossFrameBufferScenario'` | **`0% tests passed, 13 tests failed out of 13`** (v3: 100 % passed, 13 skipped) |

### 3.3 The gdb probe

See MAJOR 2's table: 12 `Acquire` hits for the new per-kind case, 4 for the whole rest of the
binary, 1 `ResolveEsprytSlotTablesArm` in each run.

### 3.4 Negative controls — each new gate goes red for the reason it exists

Each perturbation was applied alone, `SanityTest` rebuilt, the slot suite run, then the file
restored from a `/tmp` copy (**no `git stash`**, see §5.4 of v3 — it destroys the copied fixtures).
`git status --porcelain` afterwards: only `?? build-nolegacy/`.

| # | perturbation | result |
|---|---|---|
| NC5 | `ClassifyEsprytSlotArm` never returns `NoArm` | **only** `AnArmlessKnobCombinationStopsInsteadOfSkippingTheLane` fails (at the classifier assertion and again at the death assertion); 12 pass |
| NC6 | `StateBackendObjectRegistry::GetOrCreate` takes the slot-table arm for every kind **except `Framebuffer`** | **only** `EverySwitchedOverKindResolvesItsTwinThroughTheHandleArm` fails, at the `Framebuffer` leg; 12 pass |
| NC7 | `~VertexArrayObject` stops raising the notice | `EveryReKeyedObjectClassAnnouncesItsOwnDeath` fails naming `VertexElementsCso`, **and** the per-kind walk fails at the `VertexElementsCso` leg (its slot is never returned) — correctly, since with the sweep retired nothing else would free it; 11 pass |
| restore | files back, rebuild | `13/13 PASSED`, `git status` clean |

(NC1–NC4 from v3 — the arm environment, `DestroyByLifetimeId`, `~RenderbufferObject`, the ops
registration — were not re-run; none of the code they perturb changed this round except by
extension, and NC5–NC7 cover the same three seams at their new scope.)

### 3.5 Gates this package cannot reach from its own tree

Unchanged from v3 §3.6, re-probed at HEAD: **G6/G7** (package A owns `MGPipeRenderStateSpans.*`; E
owns `g7_negative_control.sh` — the only `RenderStateSpans` ctest entry is the contract's
placeholder); **G8** `HandleRecycleScenario` (E owns `MG_IntegrationTest/**`; no such ctest name in
any of the four dirs); **G9** (`gen_pipe_dirty_surface.py --check` → `unrecognized arguments`);
**G10** (`MGL_RESIDUAL_BLOCK_SIZE` is already 8 at `MGPipeTypes.h:546`, but the ratchet and the
device `resid=` window are A's and E's); **G11** (two devices + E's `frameCpuTimesMs[]`); **G12**
(no `CsoContentAddressing` ctest name exists); **G14's CI-matrix half** (`gh workflow run` is the
integrator's). So `1502×4 + 446×3 + 855 + 818 + 39 + 39` is **not** a P2 acceptance pass.

**DriverBench was not re-measured this round.** v3 §3.5's reading stands and is a bound, not a
result (the delta changed sign between two runs an hour apart; within-arm spread 13–19 % of the
median; host load average 32–42 throughout this round too). Round 4's only per-draw change is a
*removal* — the creation tick and its branch leave `BackendSlotTable::GetOrCreate`, and
`CollectGarbageIfNeeded` becomes empty — so nothing was added that a desktop A/B could resolve.
D.4.2/D.4.3 on the two devices remain owed.

---

## 4. Deviations from the brief

### 4.1 The four remaining frontend destructor files were edited (granted for this round)

C.5 gives `TextureState/*`, `FramebufferState/*`, `SamplerState/*` and
`VertexArrayState/VertexArrayObject.cpp` to package **B** and `VertexArrayObject.h` to package
**D**. The rework instruction for this round explicitly grants those four destructor sites to
package C and states that no other package will touch them in this round. Each edit is a
declaration plus a one-statement out-of-line destructor under `#if MOBILEGL_PIPE_PUSH`; nothing
else in those files is touched. **The integrator must confirm the grant** before merging, and
should land B and D after C so their rebases see it.

### 4.2 D14's "startup Fatal" is now a first-use Fatal

D14 says an armless knob pair is "a startup `Fatal{PipeLegacyMemosDisabled}`". It is now raised at
the first twin lookup instead, with a non-fatal, knob-naming diagnosis at startup. Reason: a stop
at startup is inside EGL bring-up, which the harness pre-flights in a forked child and reports as a
missing GPU — MAJOR 1. Nothing that needs an arm can proceed without hitting the stop; only a
process that never twins anything is now allowed to continue, and such a process would not have
used either arm.

### 4.3 `ROADMAP.md:18`'s "delete the GC" is delivered on the handle arm only

The legacy arm keeps its own collector, because the legacy arm keys on the frontend heap address
and cannot consume a death notice at all — that asymmetry *is* the A/B `MOBILEGL_PIPE_LEGACY_MEMOS`
exists for (`ARCHITECTURE.md:365-369`). A build with `MOBILEGL_PIPE_LEGACY_MEMOS=OFF` has no
collector anywhere.

### 4.4 §E's `e2`-before-`e3` merge rule is satisfied in content, not in commit order

See MAJOR 3. Integrator decision on a squash-merge; the mechanism is complete at HEAD.

### 4.5 Carried unchanged from v3

§4.3 (the `TwinLookupMemo`s are replaced by a table-local one-entry memo, not a bare array index —
the memo's comment is now honest about the two callers it does *not* help, review minor 1);
§4.4 (`SyncTextureObjectToBackend`'s by-value copy and second `Find` survive as a keep-alive across
the nested `glTextureView` sync); §4.5 (`UnitBindingsSnapshot` holds lifetime ids on the handle arm
and P1's `WeakPtr`s on the legacy arm, selected at runtime); §4.6 (the
`pDefaultFramebufferInfo->defaultFBO` compares are not retired — nobody owns minting
`kMGPipeDefaultFramebuffer` yet); §4.7 (some deletions are arm-split rather than outright, to keep
pull-build codegen); §4.9 (the null assert was removed from `BackendSlotTable::GetOrCreate` only;
`StateBackendObjectRegistry::GetOrCreate` still asserts).

### 4.6 Test names changed by this package, on this branch

`AProgramAndARenderbufferAnnounceTheirOwnDeath` → `EveryReKeyedObjectClassAnnouncesItsOwnDeath`;
`ObjectChurnAloneDrivesTheSweep` → `AnnouncedDeathKeepsObjectChurnFromAccumulatingWithoutASweep`
(plus v2's `…TwoTablesOfTheSameKindAgreeOnOneObjectsHandle` → `…ShareOneSlotAndKeepTheirOwnTwin`).
G14 is untouched — none of the old names is in `~/w7/p2-before-ctest-names.txt` — but the integrator
should know the names moved.

---

## 5. Where the tree contradicts the brief

### 5.1 `retrace_gate.py` has no `--backend` or `--ssim` (v3 §4.8, re-confirmed)

`--help`: `--tree --lib --out -j --only`. C.2's, C.3's and section A's G3/G4 blocks spell flags that
do not exist; `--only 'DirectGLES$'` is the working form and the SSIM threshold comes from the
reference `build-retrace` `CTestTestfile.cmake`.

### 5.2 **NEW** — G4's arming grep names a string that does not exist

Section A's G4 says `grep -L 'MGPipe verify:' …/mobilegl.log` must be empty. That substring appears
**nowhere in the tree** (`grep -rn 'MGPipe verify' MobileGL/ scripts/` → no hits), so the check is
vacuously satisfiable and would never have caught an unarmed case. The line the verify build
actually writes is `MGPipe: verify armed - 63 fields, 69 verbs, fatal=1`. The working form is
`grep -L 'MGPipe: verify armed' …`, and §3.2 uses it (0 of 39 unarmed). The integrator should fix
section A's G4 row.

### 5.3 **NEW** — the retrace output layout is not `<out>/*/mobilegl.log`

Section A's G4 globs `~/w7/retrace-out/p2-verify/*/mobilegl.log`. The real path is
`<out>/<case>/<backend>/output/mobilegl.log` (39 of them for the DirectGLES half); the brief's glob
matches nothing, which again makes both `grep -l` / `grep -L` checks vacuous. Working form:
`find <out> -path '*/output/mobilegl.log'`.

### 5.4 Carried from v3

§5.1 the contract resizes a fourth pull-build symbol (`_GLOBAL__sub_I_DirectGLES.cpp`, −9), so G1's
admitted set in D15 point 4 and C.0 must be widened by it; §5.3 section A's `grep -E '^\s+Test #'`
silently drops four-digit test ids — use the `sed`/`grep -E '^[[:space:]]*Test[[:space:]]+#[0-9]+:'`
form with `LC_ALL=C sort` on both sides; §5.4 `git stash -u` in a P2 worktree destroys the copied
trace fixtures; §5.5 `p2/contract` is an ambiguous refname in this worktree (resolves to the tag);
§5.6 a whole-tree `-DMOBILEGL_BUILD_BENCHMARK=ON` build fails inside fetched google-benchmark under
this Arch clang (`'__COUNTER__' is a C2y extension`), unrelated to P2.

---

## 6. Unfinished, and debts carried

1. **The death-notice ops table is a single global pointer.**
   `MG_State/GLState/StateObjectDeathNotice.h` holds one `const StateObjectDeathOps*`, and
   `Managers.cpp` installs Espryt's. If a second backend (package D's Magma subsystem 4 keys
   `VaoDrawMemo` out of the same per-kind allocator) ever installs its own, it **displaces**
   Espryt's and Espryt's twins stop being freed — and the sweep that used to cover that is now
   retired. No package installs a second consumer in P2, so this is not a P2 defect; it is a P3
   precondition, and the fix is a small fixed-size list of consumers rather than one pointer.
   **Flagged for the integrator as the top cross-package item of this round.**
2. **Slots are still not returned when a *table* is destroyed or reset**, and the real hazard is
   slot **reuse between reset and restore**: `ScopedDirectGLESTextureBindings` does
   `saved = table; table = {}; …; table = saved;`, so if the copy-assign released the old contents'
   slots, an intervening `Acquire` could hand slot 3 back at a higher `Gen` and the restore would
   reinstate an entry claiming the old one. The fix is a refcounted slot-ownership token shared by
   table copies, and it wants an owner who holds both holders. Cost today: a **test-only** slot and
   `ByLifetimeId` leak on table reset; real tables are process-lifetime globals.
3. **Two holders of one kind is a cross-package hazard.** `ScopedDirectGLESTextureBindings` already
   makes a second live table of kind `Texture`; package D's subsystem 4 re-keys `VaoDrawMemo` out of
   the same per-kind allocator. Whichever holder frees first leaves the other naming a stale handle
   — **safe** (`Free` is generation-guarded, `FindByHandle` compares `Gen`), but the object's next
   resolution re-`Acquire`s onto a new slot and orphans the first table's entry.
   `DestroyByLifetimeId` frees only a slot **this** table holds, so it cannot make it worse; the
   hazard is written into `SanityTest.cpp`'s two-holder case.
4. **The backend still mints client handles.** `SlotTables.h` calls `MG_Pipe::MGPipeSlots()` from
   under `MG_Backend/` off a frontend `SharedPtr`'s `GetLifetimeId()`, against
   `MGPipeHandles.h:13-16`, and `ForEachLive` hands a frontend `SharedPtr` back to backend code. A
   P3+ DEBT block at the top of `SlotTables.h` names it; `check_include_closure.py` does not probe
   `MG_Backend` headers, so nothing catches a second instance. **This belongs in `MEASUREMENTS.md`
   as an explicit P3 entry, not only as a header comment** (review minor 6).
5. **The one-entry resolution memo is the wrong shape for two of its callers.** `BindCurrentFBO`
   resolves both targets in a frame and `ResolveUnitSamplerBackend` asks per texture unit, so both
   thrash it and pay an `UnorderedMap<Uint64,Uint32>` probe P1 did not (P1 had a per-unit memo and a
   direct-mapped 6-slot array). The comment now says so; the fix is a per-unit / per-target memo and
   G11 is the gate that would price it (device-only, owed).
6. **Device measurement.** v3 §3.5 bounds this slice on a contended llvmpipe host and no more.
   D.4.2's paired two-device A/B and D.4.3's T1/T2 are owed, with the `0x7f` vs `0x5f` arm A/B.
7. **Gates G6–G12** are not exercisable from this tree (§3.5).
8. **No `PipeStats` counter for this subsystem** (package A owns `PipeStats.{h,cpp}`), so the
   integrator's A/B still has no counter saying "N twins resolved through the slot table this
   frame" — only the gdb counts.
9. **Review minors left open, deliberately**: minor 3 (cadence divergence between the arms) is moot
   — the handle arm no longer has a cadence; minor 8 (`EnsureProcessTeardownSentinel()` did not move
   onto the slot table's first insertion, it is still in `StateBackendObjectRegistry::GetOrCreate`
   ahead of the arm dispatch) is an undeclared-until-now deviation from D13, harmless and arguably
   better, and moving it would make a `BackendSlotTable` used outside the registry arm it — which is
   what the eleven unit cases do; minor 9 (`EsprytSlotArmEnvironment` duplicates ConfigLoader's
   default rather than calling it) cannot be closed without running `MG_ConfigLoader::Init` in
   `SanityTest`, which would pull the whole config path into a unit binary; minors 11–13 are
   integrator record-keeping and are recorded in §4 and §5 above.
