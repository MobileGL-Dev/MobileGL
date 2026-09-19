# P3a — closing the exit-order class: no MobileGL destructor may run from `__run_exit_handlers`

Investigator: exit-order round, 2026-09-08. Private worktree `~/w7/p3a-exit`, branch **`p3a/exit`** from
`d54ec57a` (the fix ID-18 landed). Two commits, not pushed. `~/w7/pipe` untouched.

## VERDICT

`d54ec57a` closed **two of the three** things the class needs and left the third wide open.

* Its three never-destroyed singletons (`MGPipeSlots`, `MGPipeResourceTrackerInstance`, `g_applier`) are
  correct and stay.
* It did **not** cover `MGPipeVertexInputEmitterInstance()` — the singleton the C-1 rework put *directly* on
  `~VertexArrayObject`'s own death path one commit earlier. Every teardown of a bound-and-deleted vertex
  array reads and then **writes** its `Vector<Latch>` after that vector has been freed. Under ASan this
  fires on **6/6** representative cases across both backends; natively it is what the `tcache_count=0` knob
  turns into 10 SegFaults in the verify lane and 12 in the push lane at `d54ec57a`.
* And it left the **class** open, because it fixed destinations rather than the source. The destination set
  is not closable from the pipe: `~BufferObject` reaches, through `MGPipeApplyResourceDestroy` →
  `g_resourceOps->Destroy` → `Ops_H_Destroy`, Espryt's `g_backendBufferResources`, `g_deferredBufferReleases`,
  the buffer pool and `g_GLESFuncs` — namespace-scope globals in `Managers.cpp` that the pipe does not own
  and that G5 protects functions inside.

**The fix is therefore at the source, and it is the repository's own already-written rule.** `Init.cpp:160-172`
and `GlobalObjects.cpp:17-23` state it: *"the global singletons use leak-at-exit storage … so a process that
exits without eglTerminate simply leaks them to the OS instead of running backend destructors during static
teardown"*. `pGLContext`, `pEGLContext`, `pActiveBackendObject`, `pVulkanRenderer`, `pProxyTextureManager`,
`pDefaultFramebufferInfo` and the shader-compile pool all obey it. **The four `PipeInputs` blocks did not**,
and a `PipeInputs` is the one static in the tree that holds `SharedPtr`s to *frontend* GL objects. They obey
it now.

With the fix, on the knob that made the class visible: **integration-verify 844/844**, **integration-gpu
966/966**, unit 1622 × 3 on all three builds, G1 0/0/0/0, G5 rc 0, G2 0, G14 0 removed / +69.

---

## 1. Path inventory

### 1a. Chain starters — every static that can run a frontend destructor at `exit()`

A chain starts wherever a static-storage object holds the **last** `SharedPtr` to a frontend GL object. The
only frontend destructors that reach the pipe are `~BufferObject` (`BufferObject.cpp:60`) and
`~VertexArrayObject` (`VertexArrayObject.cpp:60`); a VAO owns its buffers through
`VertexAttribute::Buffer` / `VertexBufferBindingPoint::Buffer` (`MGPipeValueTypes.h:516`, `:533`), so one VAO
release is also a buffer release. The other five death-notice raisers (`ProgramObject`, `SamplerObject`,
`FramebufferObject`, `TextureObject`, `RenderbufferObject`) reach only
`NotifyStateObjectDestroyed` → the backend.

| # | static | what it holds | destroyed at exit before? | after? |
|---|---|---|---|---|
| 1 | `MG_Pipe::gPipeInputs` (`MG_Backend/MGPipe/PipeInputs.h:691`) | `SharedPtr<VertexArrayObject> m_boundVertexArray`, three `SharedPtr<ProgramObject>` | **YES — this is the one the ASan trace names** | no (leak-at-exit) |
| 2 | `g_snapshot` (`MG_Impl/Pipe/PipeFill.cpp:366`) | a whole `PipeInputs`, filled from the live context (verify builds) | **YES** | no |
| 3 | `g_readScratch` (`PipeFill.cpp:367`) | same | **YES** | no |
| 4 | `probe` (`PipeFill.cpp:1166`, function-local in `ApplierDerivesRenderStateFields`) | a `PipeInputs`; today render state only | **YES** | no |
| 5 | `MG_State::pGLContext` — owns *every* GL object of a live context | `UniquePtr<GLContext>` | no: `Core.cpp:1534` is already `*new` (leak-at-exit, `b8a8a660`) | no |
| 6 | `MG_Backend::pActiveBackendObject`, `pVulkanRenderer`, `pEGLContext`, `pProxyTextureManager`, `pDefaultFramebufferInfo` | backend/EGL/impl singletons | no: all already leak-at-exit | no |
| 7 | DirectGLES twin tables (`g_backendTextureObjects`, `g_backendBufferResources`, `g_backendProgramObjects`, `TwinRegistry`) | `Entry::stateRef` is a **`WeakPtr`** (`SlotTables.h:67-70`, `DirectGLES.cpp:113`) | destroyed, but **non-owning**: cannot start a chain | unchanged |
| 8 | `g_liveSyncObjects` / `g_liveQueryObjects` (`GL_Sync.cpp:30`, `GL_Query.cpp:52`) | raw `SyncObject*` / `QueryObject*` | destroyed, **non-owning** | unchanged |
| 9 | `nullBufferObject`, `nullProgramObject`, `nullTextureObject`, `nullSamplerObject`, `nullFramebufferObject`, `nullRenderbufferObject`, `nullShaderObject` (the `*State.cpp` files) | `SharedPtr<...> = nullptr`, never assigned | destroyed, **always empty** | unchanged |
| 10 | `MG_Test/Pipe/PipeInputsTest.cpp:362` `static PipeInputs snapshot{}` | a unit-test-local `PipeInputs` | destroyed | still destroyed — but now harmless, see §3 |

The itest harness starts no chain and never tears anything down: `HeadlessGL::Get()` is a Meyers singleton
(`HeadlessGL.cpp:545`) **with no destructor**, so `HeadlessGL::ShutDown()` — the only thing that would call
`eglTerminate` → `MobileGL::Destroy()` → `pGLContext.reset()` — is never reached at exit, and the pre-flight
child leaves through `_exit` on purpose (`HeadlessGL.cpp:365`). So at exit the context and everything in it
simply leak; the *only* frontend objects whose last reference is elsewhere are the ones rows 1-4 hold, i.e.
objects the application deleted (or rebound away from) while the pipe block still named them.

### 1b. Destinations — every destroyed object those chains reach

Starting from row 1-4 and walking `~VertexArrayObject` / `~BufferObject`:

| destination | reached from | state at `d54ec57a` |
|---|---|---|
| `MGPipeSlots()` (`SlotAllocator.cpp:170`) | both emitters | never destroyed ✔ (ID-18) |
| `MGPipeResourceTrackerInstance()` (`ResourceTracker.h:554`) | `MGPipeEmitResourceDestroyAndFree` | never destroyed ✔ (ID-18) |
| `g_applier` (`PipeApply.cpp:384`) | `MGPipeApplyResourceDestroy`, `MGPipeApplyDeleteVertexElements` | never destroyed ✔ (ID-18) |
| **`MGPipeVertexInputEmitterInstance()` (`VertexInputEmit.h:434`)** | **`MGPipeEmitVertexElementsDestroyAndFree` (`PipeFill.cpp:818`, `:823`) — C-1's own path** | **DESTROYED — the open hole** |
| Espryt `g_backendBufferResources` (`Managers.cpp:2316`), `g_deferredBufferReleases` (`:700`), the buffer pool, `g_GLESFuncs` | `MGPipeApplyResourceDestroy` → `g_resourceOps->Destroy` → `Ops_H_Destroy` (`Managers.cpp:2268`) → `Ops_OnDestroy` | **DESTROYED, and not fixable from the pipe** — `Ops_H_Destroy` carries no `InProcessTeardown()` guard |
| Espryt twin tables via `NotifyStateObjectDestroyed` | `MGPipeEmitVertexElementsDestroyAndFree` (`PipeFill.cpp:841`) | already guarded: `OnFrontendStateObjectDestroyed` returns on `InProcessTeardown()` (`Managers.cpp:200`) |
| `MGPipeTrackerInstance`, `MGPipeSetHashSuppressorInstance`, `MGPipeCsoCacheInstance` | not on today's death paths | DESTROYED (latent) |

The fourth row is the defect this round found. `MGPipeEmitVertexElementsDestroyAndFree` asks
`emitter.RecordIsPublished(handle)` — a read of `m_latch.size()` and `m_latch[slot]` — and, when the record
is live, `emitter.NoteRecordDestroyed(handle)`, which **writes** `m_latch[slot] = Latch{}`. On a destroyed
emitter that is a read of a freed `Vector` buffer followed by a write into it: exactly the shape ID-18
described for the slot allocator, one level further along.

The fifth row is why the class cannot be closed by enumerating destinations. It is closed here because
after the fix **no such chain runs at all**.

---

## 2. The fix

Branch `p3a/exit`, two commits on `d54ec57a`, six files, +56/-12.

**`6515c8e6` — the source half (this is what kills the class).** Every static `PipeInputs` becomes
leak-at-exit storage, the same `*new` idiom `GlobalObjects.cpp` documents:

```cpp
// MG_Backend/MGPipe/PipeInputs.h
-   inline PipeInputs gPipeInputs{};
+   inline PipeInputs& gPipeInputs = *new PipeInputs();

// MG_Impl/Pipe/PipeFill.cpp
-   PipeInputs g_snapshot{};    PipeInputs g_readScratch{};
+   PipeInputs& g_snapshot = *new PipeInputs();   PipeInputs& g_readScratch = *new PipeInputs();
-   static PipeInputs probe;
+   static PipeInputs& probe = *new PipeInputs();
```

References, not pointers, so none of the ~50 use sites (including `MGB_CTX`) changes. Each carries a comment
naming the hazard and pointing at `Init.cpp`'s statement of the rule.

**`fde5fda3` — the destination half (defence in depth).** The four MGPipe process singletons `d54ec57a` did
not reach become never-destroyed, in `MGPipeSlots()`' exact shape:
`MGPipeVertexInputEmitterInstance` (the live defect), `MGPipeTrackerInstance`,
`MGPipeSetHashSuppressorInstance`, `MGPipeCsoCacheInstance`. None of the four types declares a destructor, so
nothing is lost; each costs one allocation for the life of the process.

### The completeness argument

1. **A chain can only start at a static that owns the last reference to a frontend GL object.** §1a is that
   list, derived by sweeping the tree for static-storage objects whose type can hold a frontend `SharedPtr`.
   Rows 5-6 were already leak-at-exit before this round; rows 7-8 hold weak/raw references and own nothing;
   row 9 is always empty. Rows 1-4 were the only owners, and all four are now never destroyed.
2. **Therefore no `~VertexArrayObject` / `~BufferObject` — and no `NotifyStateObjectDestroyed` raiser —
   runs from `__run_exit_handlers` at all.** That is stronger than making the destinations survive: it also
   covers the destinations the pipe cannot fix (Espryt's twin tables, the deferred-release queue, the buffer
   pool, and the driver itself, which may already be unloaded), and it is the same guarantee `Init.cpp`
   already claims for the backend.
3. **A holder outside this list is still safe on the client side**, because of half two: every MGPipe
   process singleton now outlives `exit()`, so a destructor reached from an application's own static, a test
   fixture's static (row 10 is a real instance of this), or a future holder finds live client state rather
   than freed storage. What such a holder could still reach is the *backend* half, which is the backend's
   own `InProcessTeardown()` business — armed at the first twin creation (`SlotTables.h:229`, `:299`), i.e.
   before any pre-`main` global's destructor, and honoured by the death-notice consumer.
4. **The ordering is not relied on anywhere.** "Never destroyed" is order-independent by construction, which
   is the reason a latch was rejected: a latch armed by `std::atexit` is only correct if its registration is
   later than the construction of *every* singleton it protects, and those are constructed lazily at
   different moments (`MGPipeSlots` at the first `glGenBuffers`, the emitter at the first validated draw).
   No single `call_once` registration can guarantee that.
5. **Empirically:** the whole verify lane under ASan and under the layout knob is clean (§4), where before
   the fix ASan fired on 6/6 representatives and the knob produced 22 crashes across the two lanes.

### G1 (the pull build must not move)

Every file touched is compiled only into push builds: `MG_Backend/MGPipe/PipeInputs.h` is included solely
through `PipeInputsSwitch.h`'s `#if MOBILEGL_PIPE_PUSH` arm, and the four `MG_Impl/Pipe/*.h` plus
`PipeFill.cpp` are wholly inside `#if MOBILEGL_PIPE_PUSH`. The pull link is therefore untouched by
construction — `cmake --build build-linux` after the two commits reported **`ninja: no work to do`** — and
`symbol_report.py --threshold 0` is 0 added / 0 removed / 0 resized / 0 renamed with `.text` identical to
the byte.

---

## 3. Backtraces

ASan build of the **pristine** `d54ec57a` (`build-asan`, RelWithDebInfo + `-fsanitize=address`,
`ASAN_OPTIONS=detect_leaks=0`). Trimmed; file:line verbatim.

```
==2117599==ERROR: AddressSanitizer: heap-use-after-free on address 0x7135e0697270
READ of size 1 at 0x7135e0697270 thread T0
  #0 MGPipeVertexInputEmitter::RecordIsPublished(MGPipeHandle) const   MG_Impl/Pipe/VertexInputEmit.h:312:26
  #1 MGPipeEmitVertexElementsDestroyAndFree(unsigned long)             MG_Impl/Pipe/PipeFill.cpp:818:40
  #2 MG_State::GLState::VertexArrayObject::~VertexArrayObject()        MG_State/.../VertexArrayObject.cpp:60:9
  #3..#5 _Sp_counted_base::_M_release / ~__shared_ptr<VertexArrayObject>
  #6 MG_Pipe::PipeInputs::~PipeInputs()                                MG_Backend/MGPipe/PipeInputs.h:158:12
  #7/#8 __run_exit_handlers / exit                                     (libc)
  #9/#10 __libc_start_main / _start

0x7135e0697270 is located 32 bytes inside of 40-byte region [0x7135e0697250,0x7135e0697278)
freed by thread T0 here:
  #0 operator delete(void*, unsigned long)
  #1/#2 __run_exit_handlers / exit                                     (libc)
       (the Meyers static MGPipeVertexInputEmitter's ~Vector<Latch>)

previously allocated by thread T0 here:
  #0 operator new(unsigned long)
  #1..#6 std::vector<MGPipeVertexInputEmitter::Latch>::resize
  #7 MGPipeVertexInputEmitter::EmitVertexElements(GLContext&)          MG_Impl/Pipe/VertexInputEmit.h:173:49
  #8 (anonymous namespace)::EmitVertexElements(GLContext&)             MG_Impl/Pipe/PipeFill.cpp:1477:55
  #9 MGPipeValidateForVerb(MGPipeVerb)                                 MG_Impl/Pipe/PipeFill.cpp:1597:29
  #10 MG_Impl::GLImpl::Clear_Backend / Clear                           MG_Impl/GLImpl/Drawing/GL_Drawing.cpp:531 / :1219
  #11.. the scenario body

SUMMARY: AddressSanitizer: heap-use-after-free MG_Impl/Pipe/VertexInputEmit.h:312:26 in
         MobileGL::MG_Pipe::MGPipeVertexInputEmitter::RecordIsPublished(MGPipeHandle) const
```

Every run printed `[  PASSED  ] 1 test.` first, exactly as CI does. The **same** report, byte for byte in its
first six frames, came out of all six representative runs:

| backend | `DrawParametersScenario.MultiDrawElementsIndirect…` | `VertexArrayEnableDisableScenario.EitherHalf…` | `ProgramPipelineScenario.UniformsGoToTheActive…` |
|---|---|---|---|
| DirectVulkan | UAF `VertexInputEmit.h:312` | UAF `VertexInputEmit.h:312` | UAF `VertexInputEmit.h:312` |
| DirectGLES | UAF `VertexInputEmit.h:312` | UAF `VertexInputEmit.h:312` | UAF `VertexInputEmit.h:312` |

The allocator-level UAF ID-18 documented (`SlotAllocator.cpp:100`) no longer appears: `d54ec57a` closed it,
and the emitter is the frame that now gets there first.

**gdb note for whoever repeats this.** A native backtrace of the SegFault could not be obtained and should
not be attempted: the crash is decided by heap layout, and gdb's own environment block shifts it — the exact
ctest command and `ENVIRONMENT` of a failing entry, replayed under `gdb -batch -ex run` *or* replayed bare
from a script, both **pass** (`rc=0`, verified on
`DirectVulkan.Verify.DrawParametersScenario.MultiDrawElementsIndirectCarriesEveryCommandsParameters`). ASan
is layout-independent and is the only tool that names this defect; use it.

---

## 4. Verification transcript

All on `~/w7/p3a-exit` at `fde5fda3`, `GLIBC_TUNABLES=glibc.malloc.tcache_count=0` where stated.

### Before (pristine `d54ec57a`, knob on) — the population the fix has to move

```
build-verify  ctest -L integration-verify -j 8   ->  99% passed, 10 failed out of 844
build-push    ctest -L integration-gpu     -j 8   ->  99% passed, 12 failed out of 966
```

verify lane (all crash **after** `[  PASSED  ]`):

```
DirectGLES.Verify.ProgramPipelineScenario.UniformsGoToTheActiveShaderProgram            (Subprocess aborted)
DirectGLES.Verify.PointSizeDemotionScenario.AGeometryOnlyChainCarriesTheVertexValue     (Subprocess aborted)
DirectGLES.Verify.VertexArrayEnableDisableScenario.EitherHalfOfTheAttributesInTurn      (SEGFAULT)
DirectGLES.Verify.VertexArrayEnableDisableScenario.TheEvenHalfAlone                     (SEGFAULT)
DirectVulkan.Verify.DrawParametersScenario.MultiDrawElementsIndirectCarriesEveryCommandsParameters (SEGFAULT)
DirectVulkan.Verify.PrimitiveRestartScenario.AnAllOnesVertexIndexSurvivesTheSubstitution          (SEGFAULT)
DirectVulkan.Verify.Glsl420DeclarationScenario.AnExplicitDefaultColorIndexStillDraws              (SEGFAULT)
DirectVulkan.Verify.XfbCaptureBufferReuseScenario.EverySpanIntoOneImmutableStorageBuffer          (SEGFAULT)
DirectVulkan.Verify.TessellationXfbCaptureScenario.TheEvaluationStageSeesTheUserPerVertexBlockOfItsPatch (SEGFAULT)
DirectVulkan.Verify.LayeredAttachmentShapeScenario.LayeredOneDArrayClearMaterialisedBySamplingReachesEveryLayer (SEGFAULT)
```

push lane:

```
DirectGLES.XfbAfterClipDistanceScenario.SkipComponentsCaptureAlone                       (SEGFAULT)
DirectGLES.ViewportArrayScenario.AnIndexedScissorEnableClipsOnlyThatIndex                (SEGFAULT)
DirectGLES.ProgramPipelineScenario.UniformsGoToTheActiveShaderProgram                    (Subprocess aborted)
DirectVulkan.PrimitiveRestartScenario.AnArbitraryRestartIndexDrawsInsteadOfKillingTheProcess (Subprocess aborted)
DirectVulkan.PrimitiveRestartScenario.ChangingTheRestartIndexBetweenDrawsIsHonoured      (Subprocess aborted)
DirectVulkan.DoublePrecisionScenario.AnEnabledLongArrayDoesNotBreakADrawThatIgnoresIt    (SEGFAULT)
DirectVulkan.TessellationDrawModeScenario.TessellationProgramRejectsNonPatchModes        (SEGFAULT)
DirectVulkan.VertexAttribBindingScenario.BindingDivisorAppliesToEveryAttributeOnThePoint (Subprocess aborted)
DirectVulkan.PointSizeDemotion.PointSizeDemotionScenario.AGeometryOnlyChainCarriesTheVertexValue (SEGFAULT)
DirectVulkan.CsoContentAddressing.On.CsoContentAddressingScenario.TheBlendToggleMintsBoundedly… (SEGFAULT)
DirectGLES.MapPersistentRoundtrips.LargeArenaAdoptionScenario.AnAdoptionCostsExactlyOneMapPersistentRoundtrip (SEGFAULT)
DirectGLES.ResourceSubsystemOn.LargeArenaAdoptionScenario.SubDataAfterAnInFlightDrawReachesTheNextDraw (SEGFAULT)
```

This is **not** the same 21 names the brief listed from the earlier `LastTestsFailed.log`, and that is the
point: the selection is pure allocator layout, the defect is in every one of them, and only the fatality
moves. The two `VerifyCorrupted.PipeVerifyArmingScenario` controls were `Skipped` in this run, not failing.

### After (`fde5fda3`, same knob, same commands)

```
build-verify  GLIBC_TUNABLES=glibc.malloc.tcache_count=0 ctest -L integration-verify -j 8
              -> 100% tests passed, 0 tests failed out of 844      rc=0
build-push    GLIBC_TUNABLES=glibc.malloc.tcache_count=0 ctest -L integration-gpu     -j 8
              -> 100% tests passed, 0 tests failed out of 966      rc=0
```

### The rest of the gate

| gate | result |
|---|---|
| G1 `symbol_report.py --before ~/w7/p3a-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | **0 added, 0 removed, 0 resized, 0 renamed**; 27811 → 27811 defined symbols; `.text` 10806323 → 10806323 (+0.000%). `build-linux` itself reported `ninja: no work to do` |
| G5 `scripts/p3a_untouched_regions.sh 3e298c9a HEAD` | **rc 0** — `[p3a-untouched] byte-identical between 3e298c9a and HEAD` |
| G2 pull-vs-push ctest names | **0 differing lines** |
| G14 vs `~/w7/p3a-before-ctest-names.txt` | **0 removed**, +69 (unchanged from the P3a landing) |
| unit × 3, `build-linux` | 100%, 0 failed of **1622**, three times |
| unit × 3, `build-push` | 100%, 0 failed of **1622**, three times |
| unit × 3, `build-verify` | 100%, 0 failed of **1622**, three times |
| ASan, 10 representative runs (5 scenarios × 2 backends), fix in | **10/10 rc 0, zero ASan reports** (incl. `CrossFrameBufferScenario.VertexCopyBufferSubData` — the two original CI cases — and `HandleRecycleScenario.DestroyedVertexArraysReturnTheirVertexElementsSlots`) |
| ASan, whole `integration-verify` lane, fix in | 838/844; **zero teardown reports** — the six are one pre-existing test-side bug, see below |

`ASAN_OPTIONS=detect_leaks=0 ctest -L integration-verify -j 8` on `build-asan`: `99% tests passed, 6 tests
failed out of 844`. Every AddressSanitizer report in the whole lane — `grep -o "ERROR: AddressSanitizer:
[a-z-]*" | sort | uniq -c` over `Testing/Temporary/LastTest.log` — is **`6 stack-buffer-overflow`** and
nothing else: **no `heap-use-after-free`, and no report from an exit handler anywhere in 844 processes.**

The six are `IntegerBorderColorScenario.{ASignedIntegerBorderIsClampedToTheFormatsRepresentableRange,
ANegativeBorderOnAnUnsignedFormatClampsToTheFormatsMaximum, AnOversizedUnsignedBorderIsClampedToTheFormatsMaximum}`
× both backends, and they are **not this round's and not P3a's**: the scenario passes a 4-byte
`std::int8_t texels[4]` to a `glTexSubImage2D(…, 2, 2, GL_RED_INTEGER, GL_BYTE, …)` whose default
`GL_UNPACK_ALIGNMENT` of 4 pads row 0 to four bytes, so a correct unpack needs **six** source bytes and
`PixelStoreProcessor::ProcessTexturePixelsDataUnpack` reads two past the array
(`IntegerBorderColorScenario.cpp:206`, from `:336-338`). The file is unchanged since `01d20e5c`
(2026-08-27) and neither of this round's commits touches it. It fires in the test body, never at teardown.
Recorded as follow-up 6.

---

## 5. Hashes

```
base                    d54ec57a   (ID-18's fix; feat/disaggregated head is c20e2f2b + d54ec57a)
p3a/exit  6515c8e6ae…   [Fix] (MG_Backend, MG_Impl): give the MGPipe input blocks leak-at-exit storage …
p3a/exit  fde5fda3b5…   [Fix] (MG_Impl): never destroy the four remaining MGPipe singletons …
worktree                /home/swung/w7/p3a-exit   (build-linux, build-push, build-verify, build-asan)
```

Files: `MobileGL/MG_Backend/MGPipe/PipeInputs.h`, `MobileGL/MG_Impl/Pipe/{PipeFill.cpp, VertexInputEmit.h,
Tracker.h, SetHashSuppressor.h, CsoCache.h}`.

---

## 6. Follow-ups for the integrator

1. **Arm the knob in CI.** ID-18's follow-up 3 is still the cheapest guard there is: one arm of the
   integration lane with `GLIBC_TUNABLES=glibc.malloc.tcache_count=0`. It costs nothing and turns this whole
   class from "CI-only, two cases, once" into a hard deterministic red. Without it the next holder of a
   frontend `SharedPtr` at namespace scope reopens the class silently.
2. **`Ops_H_Destroy` has no `InProcessTeardown()` guard** (`Managers.cpp:2268`), where its five siblings and
   `OnFrontendStateObjectDestroyed` do. It is unreachable at exit *after this fix*, and only because of it.
   A one-line guard there would make Espryt's half independently safe, and belongs to the backend owner.
3. **`VerifyState::~VerifyState` logs from a static destructor** (`PipeFill.cpp:379-386`, `MGLOG_E` when a
   divergence survived `MOBILEGL_PIPE_VERIFY_FATAL=0`). It runs at exit into `MG_Util::Debug`, whose file
   stream is not leak-at-exit. Only on an already-failing verify run, so not touched here; recorded because
   it is the same class one file over.
4. **The leak is real and intended.** `gPipeInputs` never releases its `SharedPtr`s, so a VAO the
   application deleted while it was bound, its buffers and (on Espryt) their twins live to process exit —
   ID-18's follow-up 2, now deliberate rather than accidental. Releasing them at context teardown
   (`MGPipeApplierReleaseObjectRecords`' moment) is the right long-term shape and is a `MG_Backend/MGPipe`
   edit, i.e. contract-frozen territory; it stays a follow-up row.
5. **Nothing is pushed.** `p3a/exit` sits on `d54ec57a` and rebases onto `feat/disaggregated` cleanly (the
   six hunks touch nothing else in flight).
6. **A real, pre-existing, test-side stack overflow in `IntegerBorderColorScenario`** (`01d20e5c`, 2026-08-27):
   four bytes of source for a 2×2 `GL_RED_INTEGER`/`GL_BYTE` upload that `GL_UNPACK_ALIGNMENT=4` makes six
   bytes long. Invisible in every non-sanitizer build (it reads two bytes of adjacent stack and the pixels
   happen to come out right), and the only thing standing between the whole verify lane and a clean ASan
   run. Fix is one line in the scenario (`std::int8_t texels[6]`, or `glPixelStorei(GL_UNPACK_ALIGNMENT, 1)`
   before the upload); it belongs to the scenario's owner, not to this round.
