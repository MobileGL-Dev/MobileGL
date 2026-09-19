# P5 scout — backend installation, `BackendObject_Remote`, and the thread/EGL question

Tree: `/home/swung/w7/pipe` @ `a29807cc` (P4a landed). READ-ONLY survey; nothing built, nothing run.
All line numbers are from that commit. Where I am guessing I say "guess".

---

## 0. Headline for the brief writer

Three things, before anything else:

1. **The two function-pointer tables are dead code today.** `gMGPipeScreen` / `gMGPipeContext`
   (`MobileGL/MG_Pipe/MGPipe.h:137-138`) are defined, zero-initialised, and **never written by
   anyone**; the generated thunks `MGP_*` (`MG_Pipe/generated/PipeThunks.inc:20-302`) that read
   them are **never called by anyone**. The live P2..P4a boundary is a set of *direct function
   calls* from `MG_Impl/Pipe/*` into `MGPipeApply*` (`MG_Pipe/PipeApply.cpp`). So "install
   `BackendObject_Remote` in `Init.cpp`" **by itself redirects nothing**: P5 must also decide how
   the ~50 `MGPipeApply*` call sites get routed through a table. See §2.
2. **The GL context is current on the app thread today, and moving it to `mgl-srv-apply` is not
   a no-op**: `IsBackendContextCurrentOnThisThread()` (`DirectGLES.cpp:12002`) gates **16 call
   sites** in `DirectGLES.cpp` and, via `CanTouchGLNow()` (`Managers.cpp:928`), **19 more** in
   `Managers.cpp`. Every one of them silently DEGRADES (returns null / "already signaled" /
   defers the upload) when called from a non-owner thread. Under inproc those calls arrive on
   the *app* thread unless P5 moves them across the wire first. See §3 — this is the risk.
3. **`MOBILEGL_TRANSPORT` does not exist in any form.** No `MG_Config::Transport`, no enum, no
   parse. `grep -rn "MOBILEGL_TRANSPORT\|TransportKind\|Transport" MobileGL/Config.h
   MobileGL/ConfigLoader.cpp` → **zero hits**. Three of the P5 exit-gate artifacts
   (`PersistentCoherentMapScenario`, a `Triangle` scenario, `DirectGLES.Split.*` lanes) also do
   not exist yet. See §4, §6.

---

## 1. The installation path today

### 1.1 Startup chain

`MobileGL::Initialize()` — `MobileGL/Init.cpp:101-138`:

```
Init.cpp:107  MG_Util::Debug::InitFile()
Init.cpp:109  MG_ConfigLoader::Init()          <- MOBILEGL_TRANSPORT would be parsed here
Init.cpp:114  MG_Util::PipeStats::Init()
Init.cpp:115  MG_State::Init()
Init.cpp:117  MG_Backend::Init()               <- THE HOOK POINT
Init.cpp:119  MG_Impl::Init()
Init.cpp:121  glslang::InitializeProcess()
```

`Initialize()` is reached lazily from the first EGL entry point:
`MG_Impl/EGLImpl/EGLImpl.cpp:35-38` (`GetStateEnsureInitialized()` → `MobileGL::EnsureInitialized()`).

### 1.2 `MG_Backend::Init()` — the exact hook

`MobileGL/MG_Backend/Init.cpp`:

```
48  void Init() {
49      MGLOG_D("Initializing MobileGL Backend...");
50
51      switch (MG_Config::ActiveBackendType) {           <- INSERT THE #if HERE
52      case BackendType::DirectGLES:
53          pActiveBackendObject = MakeUnique<DirectGLES::BackendObject_DirectGLES>();
54          break;
55      case BackendType::DirectVulkan:
56          pActiveBackendObject = MakeUnique<DirectVulkan::BackendObject_DirectVulkan>();
57          break;
58      case BackendType::Unknown:
59      default:
61          pActiveBackendObject = nullptr;
62      }
63
64      Bool result = InitSpecificBackendLibs();
...
70  }
```

**The single hook P5 is supposed to add goes at `MobileGL/MG_Backend/Init.cpp:50`** (immediately
before the `switch` at :51), matching `docs/Disaggregated/ARCHITECTURE.md:29`
("唯一 hook 点是 `MG_Backend::Init()`…`MG_Config::Transport != Monolith` 时装
`MG_Remote::BackendObject_Remote`，否则走今天的 `switch`"). ARCHITECTURE says P0's `Init.cpp`
does not contain it — confirmed, it does not.

Note the second half of the same file, which P5 also has to satisfy:

```
38  Bool InitSpecificBackendLibs() {
43      pActiveBackendObject->Initialize();
44      gBackendFunctionsTable = pActiveBackendObject->GetBackendFunctions();
45      return true;
46  }
```

`gBackendFunctionsTable` is populated *from the backend object* — so a `BackendObject_Remote`
whose `GetBackendFunctions()` returns an all-null table would turn 91 `MG_Impl/GLImpl` call sites
into null-pointer calls. (Count: `grep -rn gBackendFunctionsTable MobileGL/` → 91 in
`MG_Impl/GLImpl`, 2 in `MG_Impl/Pipe`, plus tests.) This is a real P5 item that ROADMAP's P5 row
does not name.

### 1.3 The globals

```
MobileGL/GlobalObjects.cpp:23   UniquePtr<BackendObject>& pActiveBackendObject = *new UniquePtr<BackendObject>();
MobileGL/GlobalObjects.cpp:24   GlobalBackendFunctionsTable gBackendFunctionsTable;
MobileGL/MG_Backend/BackendObjects.h:16-17   the two extern declarations
MobileGL/Config.h:21            extern BackendType ActiveBackendType;   (a `Transport` sibling would go here)
```

Frontend readers of `pActiveBackendObject` outside `MG_Backend/` (the surface a role-shim must
cover) are few: `MG_Impl/EGLImpl/EGLImpl.cpp` (:40-46, :150, :263, :275, :284, :312, :326, :346,
:448), `MG_State/GLState/Core.cpp:34`, `MG_IntegrationTest/Harness/BackendCapsPeek.cpp:17,29`.
Everything else is tests.

### 1.4 The two tables — declared, generated, never installed

```
MG_Pipe/generated/PipeTables.inc:17   struct MGPipeScreen  { 11 fn ptrs }
MG_Pipe/generated/PipeTables.inc:33   struct MGPipeContext { 60 fn ptrs }
MG_Pipe/generated/PipeTables.inc:94-96  kMGPipeScreenCallCount=11, kMGPipeContextCallCount=60, total 71
MG_Pipe/MGPipe.h:137-138              inline MGPipeScreen gMGPipeScreen{}; inline MGPipeContext gMGPipeContext{};
MG_Pipe/generated/PipeThunks.inc:20+  inline MGP_GetCaps(...) { gMGPipeScreen.GetCaps(...); } ... (71 thunks)
```

Generator: `scripts/gen_pipe.py` from `MG_Pipe/PipeCalls.def`. Consumer of the *catalogue*:
`MG_Test/Pipe/PipeCatalogueTest.cpp`.

**Verified absence** (WSL grep over the whole tree): the only occurrences of `gMGPipeScreen` /
`gMGPipeContext` are the two definitions in `MGPipe.h` and the 71 reads inside `PipeThunks.inc`.
No assignment, no `InstallPipe`-style function, no `= {…}` aggregate anywhere. And
`grep -rn "MGP_[A-Z][A-Za-z]*(" MobileGL/ | grep -v generated/` returns **nothing** — the thunks
have no callers. (The only "install" named in the tree is
`MGPipeInstallClientResourceCallbacks()`, `MG_Impl/Pipe/ResourceTracker.h:606`, called from
`MG_Impl/Pipe/PipeFill.cpp:628` — that is the *callback* direction, §2.3, not these tables.)

---

## 2. What the boundary actually is today, and what `BackendObject_Remote` must replace

### 2.1 The live path (P2..P4a)

```
app GL call
  -> MG_Impl/GLImpl/*  (validate)
  -> MG_Impl/Pipe/{PipeFill.cpp, Tracker.h, CsoCache.h, TextureEmit.h, SamplerEmit.h,
                    FramebufferEmit.h, ProgramEmit.h, VertexInputEmit.h, ResourceTracker.h}
  -> MGPipeApply*(...)                       DIRECT CALL, MG_Pipe/PipeApply.{h,cpp}
       - writes MG_Pipe::gPipeInputs  (defined MG_Backend/MGPipe/PipeInputs.h:706)
       - dispatches object/resource work through MGPipeResourceOps (PipeApply.cpp:402, g_resourceOps)
  -> backend reads gPipeInputs at draw time (MGB_CTX, ARCHITECTURE.md:344)
```

Representative `MGPipeApply*` call sites (all direct, not through a table):
`MG_Impl/Pipe/PipeFill.cpp:645, 678, 691, 705, 727, 747, 761, 784, 799, 839, 1403, 1424, 1465,
1502, 1523, 2013, 2026, 2115, 2184`; `MG_Impl/Pipe/CsoCache.h:157, 178`;
`MG_Impl/Pipe/FramebufferEmit.h:459`; `MG_Impl/Pipe/VertexInputEmit.h:161, 181, 245, 270, 399`.
Declarations: `MG_Pipe/PipeApply.h:740-1052` (≈45 entries).

`MG_Pipe/PipeApply.h:15-17` states the intent explicitly: *"the in-process applier: the SERVER
half of the calls P2 emits. Under split this file is `MG_Remote/Server/PipeApplier`"*.

**So the two readings P5 must choose between, and I cannot settle from the tree alone:**

- **(A) Retrofit the tables.** Turn each `MGPipeApply*` into `MGP_*` (the generated thunk), and
  have monolith `Init` install a table of `&MGPipeApply*` while remote `Init` installs a table of
  emitters. Cost: touching every call site listed above; benefit: exactly the shape
  ARCHITECTURE §1.2 describes, and `MG_Test` can keep mocking by table substitution.
- **(B) Branch inside the applier.** Leave the call sites alone and put the transport fork inside
  `PipeApply.cpp` / a thin `PipeEmitter`. Cheaper, but then `gMGPipeScreen`/`gMGPipeContext`
  remain dead and the "one hook in `Init.cpp`" claim (`ARCHITECTURE.md:29`) becomes false.

*Evidence that would settle it:* whether any P5 gate asserts on the tables (nothing in the tree
does today), and whether the "B 门符号" purity gate
(`ARCHITECTURE.md:516`, `nm --undefined-only libMobileGLServer.so`) can pass under (B) — under (B)
the server links `PipeApply.cpp`, which includes `MG_Impl/Pipe` headers only in the client
direction, so I *think* it still can (**guess**).

### 2.2 `MGPipeResourceOps` — the precedent for how a backend registers a table

`MG_Pipe/PipeApply.h:85-102`:
```
 85  struct MGPipeResourceOps { Create, Respecify, SubData, SubDataResident, FlushRange,
                                Readback, Destroy, MapPersistent, UnmapPersistent };  (9 members)
101  void MGPipeSetResourceOps(const MGPipeResourceOps* ops);
102  const MGPipeResourceOps* MGPipeGetResourceOps();
```
Storage `MG_Pipe/PipeApply.cpp:402` (`g_resourceOps`), setters at `:1094-1095`.
DirectGLES's table: `MG_Backend/DirectGLES/Managers.cpp:2298-2308` (`g_glesResourceOps`),
registered/unregistered around `RegisterBufferBackendOps()` (`DirectGLES.cpp:11933`,
`Managers.cpp:2584`). **DirectVulkan registers none** — that is the "consumer gate" that makes
the four P4a families emit nothing on Magma (`ARCHITECTURE.md:213-…`, and
`MG_IntegrationTest/Scenarios/ObjectSubsystemControlScenario.cpp:119, 784, 857`).
This is the pattern P5 should copy for the server's op tables; it is also the thing that will
make a DirectVulkan split lane vacuous.

### 2.3 `BackendObject` — the interface `BackendObject_Remote` must implement

`MobileGL/MG_Backend/BackendObject.h:567-644`. Lifetime: owned by
`UniquePtr<BackendObject>& pActiveBackendObject`; constructed in `MG_Backend/Init.cpp:53/56`,
destroyed at `MobileGL/Init.cpp:68` (`pActiveBackendObject.reset()`).

**8 pure virtuals — every one must be answered by the remote object:**

| line | member | what a remote must do |
|---|---|---|
| 572 | `void Initialize() = 0` | bring up transport + handshake (no GL) |
| 573 | `Bool InitCapabilities() = 0` | **this is the `MGPCaps` snapshot round trip** |
| 574 | `Bool InitWindowSurface() = 0` | ship the native token; `ARCHITECTURE.md:559-563` |
| 590 | `const RendererInfo& GetRendererInfo() const = 0` | from the caps snapshot; must return a **reference**, so the remote has to own a `RendererInfo` |
| 591 | `String GetBackendAPIVersionString() const = 0` | from snapshot |
| 592 | `const GlobalBackendFunctionsTable& GetBackendFunctions() const = 0` | see §1.2 — 91 `MG_Impl/GLImpl` sites read the table this returns |
| 593 | `const DynamicBackendParameters& GetDynamicParameters() const = 0` | from snapshot; struct is `BackendObject.h:297-…`, ~70 scalars incl. `MaxComputeWorkGroupCount[3]` |
| 595 | `BackendType GetBackendType() const = 0` | must answer the **server's** backend, not "Remote" — many frontend branches switch on it (**guess**: returning a new enumerator would break them) |

**9 EGL-lifecycle virtuals with base implementations** (`BackendObject.cpp:168-494`), the
"EGL 生命周期 8 项" of `ARCHITECTURE.md:19` (I count nine, not eight — `ResizeEGLWindowSurface`
is the likely uncounted one):

`:576 InitializeEGLDisplay`, `:577 CreateEGLWindowSurface`, `:578 ResizeEGLWindowSurface`,
`:579 CreateEGLPbufferSurface`, `:580 MakeEGLCurrent`, `:581 SwapEGLBuffers`,
`:584 SetEGLSwapInterval`, `:585 ReleaseEGLSurface`, `:586 ReleaseEGLResources`.
Plus 2 protected: `:624 InitPbufferSurface`, `:625 OnEGLSurfaceReleased`.

Also inherited, and **remote-hostile**, is the base class's own state
(`BackendObject.h:627-638`): `m_eglStateMutex` (recursive), `m_formatCapabilities`
(`FormatCapabilityCache` — a full per-target × per-format matrix, populated by GL probes in
`BackendObject_DirectGLES.cpp:855-866`), `m_eglCurrentThreads`, `m_eglSurfaces`.
`GetFormatCapabilities()` is **non-virtual** (`BackendObject.h:589`, impl
`BackendObject.cpp:478`), so a remote object must *fill* `m_formatCapabilities` from the caps
snapshot rather than override the accessor. Reader:
`MG_Backend/DirectGLES/DirectGLES.cpp:12446` (`ClampSamplesToBackendSupport`) and
`MG_IntegrationTest/Harness/BackendCapsPeek.cpp:29`.

Two base behaviours P5 must not lose:
- `BackendObject::MakeEGLCurrent` calls `InitCapabilities()` on first make-current per surface
  (`BackendObject.cpp:341-347`) — i.e. **the caps snapshot's natural timing is already there**.
- `ActivateEGLSurface` clears `m_eglCurrentThreads` and resets
  `m_backendCapabilitiesInitialized` (`BackendObject.cpp:301-302`).

---

## 3. THE THREAD QUESTION

### 3.1 Which thread calls the backend in monolith

**The app's calling thread, always. There is no render thread.**
Every `MG_Impl/GLImpl` entry point runs `gBackendFunctionsTable.GL.*` inline on the thread that
made the GL call (91 sites). `grep -rn "std::thread\b" MobileGL/` outside tests finds **no
thread creation at all** except `MG_Util/Async/ShaderCompilePool.cpp` — and the compile pool is
explicitly forbidden from touching the backend
(`MG_State/GLState/ProgramState/ShaderCompileTask.cpp:239`,
`ProgramLinkTask.cpp:564`, `ProgramSpirvTask.cpp:94`: *"no GL/EGL call, no
`pActiveBackendObject` read"*). So today: **one thread, the app's, does frontend + backend + the
driver call.**

### 3.2 Which thread owns the EGL/GL context

`DirectGLES` keeps **one process-global ES context**, and its owner is a single global slot:

```
DirectGLES.cpp:11856-11864  comment: "The single backend ES context migrates between app threads
                            (FCL/pojav-style LWJGL hands the EGL context from JVM thread to JVM
                            thread). Ownership must live in ONE global slot..."
DirectGLES.cpp:11865        std::atomic<std::thread::id> g_backendContextOwnerThread{};
DirectGLES.cpp:11919-11955  Bool MakeCurrent()   — the real eglMakeCurrent at :11925
DirectGLES.cpp:11931        g_backendContextOwnerThread.store(std::this_thread::get_id(), release)
DirectGLES.cpp:11958-11975  Bool ReleaseCurrent() — eglMakeCurrent(NO_SURFACE…) at :11964,
                            owner cleared at :11961 / :11973
DirectGLES.cpp:12002-12025  Bool IsBackendContextCurrentOnThisThread()
DirectGLES.cpp:11980-11996  the EGL verification stamp (g_eglVerifiedFrameSerial /
                            g_eglVerifiedContextGeneration) — re-verifies against
                            eglGetCurrentContext() once per (frame, context generation).
                            Comment: glvnd's eglGetCurrentContext measured at 16% of the render
                            thread, hence the stamp.
```

**Is it ever current on the app thread today? Yes — it is only ever current on an app thread.**
The chain runs entirely on the caller:

```
app eglMakeCurrent
 -> MG_Impl/EGLImpl/EGLImpl.cpp:240  MakeCurrent(...)   [takes EGLOperationMutex, :241]
 -> EGLImpl.cpp:284                  backendObject->MakeEGLCurrent(dpy, draw, read, ctx)
 -> MG_Backend/DirectGLES/BackendObject_DirectGLES.cpp:935  MakeEGLCurrent
 -> BackendObject_DirectGLES.cpp:963  DirectGLES::MakeCurrent()
 -> DirectGLES.cpp:11925              g_EGLFuncs.eglMakeCurrent(...)     <- ON THE APP THREAD
 -> DirectGLES.cpp:11931              owner := this thread
 -> DirectGLES.cpp:11933-11953        BufferImpl::RegisterBufferBackendOps() + SIX cache
                                      invalidations (broadcast memo, indexed buffer bindings,
                                      pixel buffer bindings, framebuffer bindings + handle arm
                                      memos, pack state, synced render state) + ApplyRequestedSwapInterval
```

Line 11933-11953 is what `ARCHITECTURE.md:535` calls "`MakeCurrent` 的缓存失效风暴" — under
split it runs **once at startup on `mgl-srv-apply`** instead of on every app-thread migration.

`BackendObject` (the base) separately keeps a *multi-thread* map
`UnorderedMap<std::thread::id, EGLCurrentState> m_eglCurrentThreads` (`BackendObject.h:637`,
written `BackendObject.cpp:350-355`) — so the frontend's EGL bookkeeping already models "many
threads, one context", while DirectGLES's driver-side ownership is a single slot. These two are
consistent today only because the same thread does both.

### 3.3 What breaks if two threads touch it

Nothing crashes; **everything silently degrades**, which is worse. A non-owner thread makes
`IsBackendContextCurrentOnThisThread()` return false at `DirectGLES.cpp:12006`, and then:

`DirectGLES.cpp` — **16 guarded sites**:

| line | function | degraded behaviour |
|---|---|---|
| 12034 | `FenceSync` | returns `nullptr` → frontend substitutes an always-signaled sync |
| 12058 | `ClientWaitSync` | returns `GL_ALREADY_SIGNALED` |
| 12069 | `WaitSync` (server-side wait) | no-op |
| 12081 | `DeleteSync` | leaks the native `GLsync` (wrapper only freed) |
| 12093 | `GetSyncStatus` | reports signaled |
| 12197 | `BeginTimerQuery`-ish | returns null handle |
| 12212 | end-query | skipped |
| 12221 | `QueryCounterTimestamp` | null handle |
| 12242 | `BeginCoreQuery` | null handle |
| 12257 | end core query | skipped |
| 12299 | `glGetQueryObjectuiv` read | falls back |
| 12326 | query availability | falls back |
| 12373 | query delete | wrapper only |
| 12385 | `GetGpuTimestampNs` | returns 0 |
| 12399 | `WaitForFrameSerialCompleted` | returns false |
| 12428 | `Present()`'s frame fence | **no fence is created for the frame** → the buffer pool's recycle watermark never advances |

`Managers.cpp` via `CanTouchGLNow()` (`:928-930`) — **19 sites**: `1494` (persistent-map
acquire), `1589`, `1610`, `1632`, `1681`, `1747`, `1793`, `1919`, `1939`, `1986`, `2038`, `2087`,
`2170` (the second persistent-map tier), `2602` (`ProcessDeferredBufferReleases`), `3293`,
`3428`. These are the buffer paths: a false answer means the write is **deferred into
`pendingRanges` / `pendingResidentWrites`** and applied later — correct only if a later call on
the owner thread drains them.

Concretely, the single most dangerous one for the P5 exit gate:
`Managers.cpp:1494` and `:2170` are the `glBufferStorageEXT` + `glMapBufferRange` persistent-map
acquisitions, and `Ops_H_MapPersistentTracked` (`Managers.cpp:2306`) is the op behind
`MGPipeApplyMapPersistent` (`PipeApply.cpp:1918-1919`, called from `PipeFill.cpp:784`). If the
app thread calls it while the apply thread owns the context, it **declines** and the
`PersistentCoherentMapScenario` gate is unreachable. `ARCHITECTURE.md:502` already lists
`MapPersistent` (T1, once per storage definition) as one of the unavoidable blocking round trips
— i.e. the design says it must cross the wire, not be called locally.

### 3.4 What ARCHITECTURE.md says

- `ARCHITECTURE.md:535` (§14 Present、线程与帧节奏): *client: **v1 不加线程**, encoding happens on
  the GL thread and writes the ring directly (the frontend is already a per-context single-thread
  contract); foreign-thread sync/query reads are answered lock-free from `RingControl`; the few
  that must emit take `ctrlMutex` and go over CTRL as an `AuxRequest` (the SPSC ring allows no
  second producer); `ShaderCompilePool` stays as-is on the client.*
  *server: `mgl-srv-io` (asio, framing, `SCM_RIGHTS`, doorbell, CTRL RPC), `mgl-srv-apply`
  (**终身持有原生 context** — `g_backendContextOwnerThread` is written **once**, the MakeCurrent
  invalidation storm becomes a one-off at startup, the per-frame EGL re-verification is
  permanently true, **off-thread degradation disappears**), optional `mgl-srv-dec`.*
- `ARCHITECTURE.md:536`: bind `mgl-srv-apply` to a big core via `MOBILEGL_IPC_SERVER_AFFINITY`
  (default auto), reusing `ShaderCompilePool`'s big-core probe.
- `ARCHITECTURE.md:26` (§1.3): inproc *is* the monolith's render thread — "把 `PrepareForDraw`
  与驱动调用搬离 GL 线程，是本项目手上最大的单一 CPU 杠杆".
- `ARCHITECTURE.md:581` (§16): `MOBILEGL_BUILD_DISAGGREGATED_INPROC` **does not exist yet**; it
  implies `MOBILEGL_BUILD_DISAGGREGATED` and adds a **role-isolation shim**, because in inproc
  *both roles live in one process*. MGPipe cuts the process globals needing a role split from
  four (`pGLContext`, `gBackendFunctionsTable`, `pActiveBackendObject`,
  `pDefaultFramebufferInfo`) to **two** (the pipe tables and `pActiveBackendObject`).

**The unresolved inproc problem this creates, and P5 owns it:** `pActiveBackendObject` is ONE
global (`GlobalObjects.cpp:23`) and `MG_Backend::Init()` writes it once. In inproc the *client*
role needs `BackendObject_Remote` there and the *server* role needs the real
`BackendObject_DirectGLES` there, in the same address space. The Init.cpp hook as written in
`ARCHITECTURE.md:29` produces only one of the two. Two readings:
- **(i)** the server role never reads `pActiveBackendObject` at all, so the server side can hold
  its `BackendObject_DirectGLES` in a private `UniquePtr` owned by `ServerLoop`. Against this:
  `DirectGLES.cpp:12446` (`ClampSamplesToBackendSupport`) reads `pActiveBackendObject` from
  *inside the backend*, on the server side of the seam.
- **(ii)** the role shim makes `pActiveBackendObject` role-scoped (a thread-keyed accessor).
  `ARCHITECTURE.md:581` says both shims are "不在 GL 热路径的每次访问上", which reads like (ii).
*Evidence that would settle it:* an inventory of `pActiveBackendObject` reads reachable from
backend code (I found exactly one: `DirectGLES.cpp:12446`). If it is only that one, (i) wins
cheaply by passing the format cache down instead.

### 3.5 What has to change for the app thread to give up the context

Today the app thread acquires the context at `BackendObject_DirectGLES.cpp:963`
(`DirectGLES::MakeCurrent()`). Under split, that call must not happen on the app thread at all:
`BackendObject_Remote::MakeEGLCurrent` should emit a make-current record and let the *apply
thread* run `DirectGLES::MakeCurrent()` once, then never release. The frontend already has the
serialisation point: `EGLOperationMutex` (`EGLImpl.cpp:48-51`), held by `MakeCurrent` (:241),
`SwapBuffers` (:163), `DestroySurface` (:305). `ARCHITECTURE.md:213` says exactly this —
`eglMakeCurrent` is a flow-ownership transfer emitted under the existing `EGLOperationMutex`,
and `ReleaseThread` / `SwapInterval` should be made to take that lock too (they do **not**
today: `EGLImpl.cpp:341 ReleaseThread` and `:435 SwapInterval` take no lock).

Three call sites that today reach the driver on the app thread and must be re-pointed:
- `EGLImpl.cpp:284` → `MakeEGLCurrent` → `DirectGLES::MakeCurrent()`
- `EGLImpl.cpp:173` → `SwapEGLBuffers` → `BackendObject.cpp:396` `backendFunctions.Present()`
  (note `BackendObject::SwapEGLBuffers` first checks `m_eglCurrentThreads` for **the calling
  thread**, `BackendObject.cpp:375-384` — a remote object must override or relax this)
- `EGLImpl.cpp:448` → `SetEGLSwapInterval` → `BackendObject.cpp:400-405`

---

## 4. `MOBILEGL_TRANSPORT` parsing: **none exists**

- `MobileGL/Config.h`: no `Transport` member. The MGPipe block is `Config.h:319-398`
  (`PipePush` at `:349`, `PipeVerify` at `:355`, `PipeStats`, `PipeLegacyMemos`,
  `PipeTexelRetainMb`, `PipeIndexMirrorMb`, `PipeStatsPeriod`, `PipeStatsFile`).
- `MobileGL/ConfigLoader.cpp`: the MGPipe parse block is `:248-282`. No transport line.
  Helpers available to copy: `QueryEnvVariable`, `QueryEnvFlag`, `QueryEnvUint32`,
  `QueryEnvUint64` (`:171` documents the strict integer parse), `QueryEnvQuirkOverride`.
  `ConfigLoader.cpp:34` — every `MOBILEGL_`/`LIBGL_` prefixed variable is accepted by
  `InitializeAcceptedEnvVariables` by construction, so a new name needs no allow-list entry.
- The enum-style precedent to copy is `InitBackendType()` at `ConfigLoader.cpp:284-300`, which
  writes the **global** `MG_Config::ActiveBackendType` (declared `Config.h:21`,
  defined `GlobalObjects.cpp:13`) rather than a `Features` member. `ARCHITECTURE.md:576` requires
  `MG_Config::Transport` to be a `constexpr Monolith` when `MOBILEGL_BUILD_DISAGGREGATED=OFF`, so
  it cannot be a plain `Features` field — it needs the `#if` shape.
- Spec of the value: `ARCHITECTURE.md:583` —
  `MOBILEGL_TRANSPORT = monolith | inproc | spawn | unix:<path> | pipe:<name>`.
- Related unset knobs (all P5+, none parsed today): `ARCHITECTURE.md:613`
  (`MOBILEGL_IPC_SERVER_PATH`, `_RING_MB`(8), `_STAGE_MB`(32), `_PRESENT_CREDIT`(1),
  `_SPIN_US`(50), `_POLL_ESCALATE`(64), `_PERSISTENT_BLOCK_KB`(64), `_ADOPT_TIER`,
  `_SHADOW_SHM`, `_INLINE_PAYLOADS`, `_SERVER_AFFINITY`, `_STRICT_ERRORS`, `_AUDIT`, `_TRACE`,
  `_ATTACH`, `_RESPAWN`, `_IDLE_EXIT_S`(30)).

### Build wiring that does exist

- `CMakeLists.txt:23` `option(MOBILEGL_BUILD_DISAGGREGATED ... OFF)`.
- `CMakeLists.txt:453-466` forces it OFF (normal variable, not cache) when
  `3rdparty/flatbuffers/include` is missing.
- `CMakeLists.txt:503-520` appends only the 8 `MG_Remote/Transport/*.cpp` files — **no
  `Client/` or `Server/` entries yet**.
- `CMakeLists.txt:572-574` `-DMOBILEGL_BUILD_DISAGGREGATED=1`.
- `CMakeLists.txt:605-609` adds the flatbuffers include dir.
- `CMakeLists.txt:484-499` is the `MOBILEGL_PIPE_PUSH` source list (`PipeApply.cpp`,
  `PipeFill.cpp`, `SlotAllocator.cpp`, `MGPipeRenderStateSpans.cpp`, `PipeInputs.cpp`,
  `ProgramArtifactsCodec.cpp`) — the P5 client/server files will need the same treatment.
- `MOBILEGL_BUILD_DISAGGREGATED_INPROC` does not appear anywhere in the tree
  (only `ARCHITECTURE.md:581, 595`).
- `MG_Remote/` contains `Protocol/` and `Transport/` only. **No `Client/`, no `Server/`.**

---

## 5. Teardown — what a two-role build has to sequence

### 5.1 The three nested teardown paths today

**(a) `MobileGL::DestroyImpl` — `MobileGL/Init.cpp:38-98`, in order:**
```
:50  MG_Util::PipeStats::Shutdown()                 final stats line + JSON dump
:56  ShaderCompilePool::Get().StopAndDrain()        "the one cancellation path that waits"
:62  MG_Impl::GLImpl::DestroyAllSyncObjects()       drains while gBackendFunctionsTable is still live
:67  MG_Impl::GLImpl::DestroyAllQueryObjects()      ditto
:68  MG_Backend::pActiveBackendObject.reset()       -> ~BackendObject_DirectGLES -> DestroyEGLContext()
:69  MG_State::pGLContext.reset()
:70  MG_State::pEGLContext.reset()
:71  pProxyTextureManager.reset()
:72  pDefaultFramebufferInfo.reset()
:78  glslang::FinalizeProcess()                     MUST be after :69 (comment :73-77)
:82  ShaderCompiler::ResetPrewarmLatch()
:87-90  translation-cache stats + clear
:91  MG_Backend::gBackendFunctionsTable = {}
```

**(b) `MG_Impl::EGLImpl::Terminate` — `EGLImpl.cpp:319-338`:**
`state->TerminateDisplay(dpy)` → `backendObject->ReleaseEGLResources()` (:326) → and *only if*
`!HasAnyInitializedDisplay() && !HasAnyCurrentContext()` → `MobileGL::Destroy()` (:333).
So (a) is normally reached **through** EGL, not at process exit.

**(c) `DirectGLES::DestroyEGLContext` — `DirectGLES.cpp:12472-12510`:**
```
:12473-12477  BufferImpl / XfbImpl / MultiDrawImpl / OnRestartSubstitutionContextDestroyed /
              ScratchFBOImpl :: OnBackendContextDestroyed()
:12478        ReleasePackedWordScratchTexture()
:12479-12484  invalidate framebuffer / VAO / pack-state / broadcast-memo caches
:12486        ++g_backendContextGeneration
:12489        g_backendContextOwnerThread.store({})
:12491        ++g_syncContextGeneration        (all outstanding fences become "signaled")
:12494-12496  abandon the 4-slot frame-fence ring, floor the completed watermark
:12498-12509  eglMakeCurrent(NO_SURFACE) ; eglDestroyContext ; eglDestroySurface ; eglTerminate
```
I checked the three `OnBackendContextDestroyed` bodies — `Managers.cpp:2583-2596`,
`Managers.cpp:9849-9855`, `DirectGLES.cpp:1426-1433` — and **none of them issues a GL call**;
they only drop shadows, bump generations and unregister the ops table
(`Managers.cpp:2584 UnregisterBufferBackendOps()`). Good news: the destroy path itself is
thread-agnostic. (`Managers.cpp:2602`'s `CanTouchGLNow()` belongs to
`ProcessDeferredBufferReleases`, a *different* function starting at `:2598` — do not confuse
them; I nearly did.)

### 5.2 ARCHITECTURE's required split order, and where it disagrees with today

`ARCHITECTURE.md:537`:
> 拆机顺序：publish + server 排空并 ack → 停 apply 线程 → 关 transport → client 排空 compile
> pool（先于 `glslang::FinalizeProcess()` 与 `pGLContext` 析构）→ `MobileGL::Destroy()` →
> 释放 sync/query handle。

**This is not today's order.** Today `DestroyAllSyncObjects` / `DestroyAllQueryObjects`
(`Init.cpp:62, 67`) run **before** `pActiveBackendObject.reset()` (`:68`), and the comments at
`:58-66` say that is deliberate — the registries are drained "while the backend function table
can still release the backend handles". ARCHITECTURE puts sync/query release **after**
`MobileGL::Destroy()`. Two readings:
- **(i)** ARCHITECTURE means "release the *client-side* handle objects last, after the server is
  gone", because in split a sync handle is minted client-side (`ARCHITECTURE.md:500`, "client 铸造
  handle") and needs no backend call — so the ordering constraint that motivates today's `:62/:67`
  simply evaporates.
- **(ii)** ARCHITECTURE is loose here and P5 should keep today's order.
*Evidence that would settle it:* whether `BackendObject_Remote`'s `GetBackendFunctions()` table
has real `DeleteSync`/`DeleteQuery` entries that must run before the transport closes. If it
does, (ii); if syncs are client-minted, (i). I could not settle this from the tree because
neither the remote table nor the client fence store exists yet.

### 5.3 Additional teardown hazards P5 must handle

1. **`EGLImpl.cpp:326` `ReleaseEGLResources()` is called from the app thread**, and for
   DirectGLES it runs `DestroyEGLContext()` (`BackendObject_DirectGLES.cpp:981-985`), which
   calls `eglDestroyContext`/`eglTerminate`. In split those must execute on `mgl-srv-apply` (the
   context owner), so `BackendObject_Remote::ReleaseEGLResources` has to be a **blocking**
   request, not fire-and-forget — otherwise `MobileGL::Destroy()` at `EGLImpl.cpp:333` proceeds
   while the server still holds the context.
2. **`OnEGLSurfaceReleased` → `DestroyEGLContext`** (`BackendObject_DirectGLES.cpp:986-989`),
   reached from `BackendObject::ReleaseEGLSurface` (`BackendObject.cpp:446-463`) and from
   `DestroyPendingEGLSurfaceIfUnused` (`:419-431`). A surface destroyed while current is
   *deferred* (`DestroyPending = true`, `:454`) until the last thread releases it. Two roles make
   "the last thread" ambiguous.
3. **`~BackendObject_DirectGLES()` calls `DestroyEGLContext()`** (`BackendObject_DirectGLES.cpp:833-835`),
   i.e. `pActiveBackendObject.reset()` at `Init.cpp:68` can destroy the native context **on the
   app thread**. Same problem as (1).
4. **`Present()` is the only frame-boundary drain** (`DirectGLES.cpp:12424-12470`: fence poll,
   `UboRingOnPresent` / `UnpackRingOnPresent` / `UploadRingOnPresent` / `TrimBufferPool`,
   `PipeStats::OnPresent()` at `:12467`). `ARCHITECTURE.md:531` insists `present` ↔
   `eglSwapBuffers` stays strictly 1:1 for exactly this reason. A shutdown that drops in-flight
   presents starves the retire path.
5. **`PipeStats::Shutdown()` at `Init.cpp:50` runs in the client role**, but the
   `persistent-map-push` counter the P5 gate demands is written where the push happens.
   `MG_Util/Metrics/PipeStats.h:46-122` lists `persistent-map-push` as declared-but-unwired
   (`ARCHITECTURE.md:617`: "P0 未接线"). Whichever role owns the counter must flush before its
   half exits; with two roles there are two `PipeStats` instances in inproc unless the shim
   scopes it (**guess**: it does not need scoping, since the counter is client-side).

---

## 6. Gate artifacts that do not exist yet (P5 must create them)

- `PersistentCoherentMapScenario` — named in `ARCHITECTURE.md:500` and in the ROADMAP P5 exit
  gate; **no such file** under `MobileGL/MG_IntegrationTest/Scenarios/` (86 scenarios listed;
  `ClearThenReadPixelsScenario.cpp` exists, a `Triangle*Scenario.cpp` does **not**).
- `DirectGLES.Split.` test prefix — the itest CMake has prefixes
  `DirectGLES.` (`MG_IntegrationTest/CMakeLists.txt:739`),
  `DirectGLES.ForcedDepthStencilEmulation.` (:786), `DirectGLES.UnlocatedIoBlocks.` (:817),
  `DirectGLES.MallocPerturb.` (:839), `DirectGLES.AsyncOn./AsyncOff.` (:868, :895),
  `DirectGLES.OptimisticShaderStatus.` (:921), `DirectGLES.NoViewportArrayEmulation.` (:945),
  `DirectGLES.WidenedPacked16.` (:983), `DirectGLES.PointSizeDemotion.` (:1000),
  `DirectGLES.HandleRecycle.Handles./Legacy.` (:1157, :1175). **No `Split.`**, and
  no `DirectGLES.Pipe.` prefix either (ARCHITECTURE.md:518 names it as the target shape).
  The env-joining helper is `mgl_itest_join_environment` (`MG_IntegrationTest/CMakeLists.txt:321`);
  every list must append `${MGL_ITEST_COMMON_ENV}` (:1036, and the trap is restated at
  `ARCHITECTURE.md:584`).
- trace-replay `SPLIT` suffix + `-DTRACE_TRANSPORT=`: `add_trace_replay_test` is
  `tools/trace_replay/CMakeLists.txt:311-383` (the `run_trace_case.cmake` invocation is at
  `:372`); `add_trace_replay_test_for_backends` at `:384-387`. Neither knows about `SPLIT` or
  `TRACE_TRANSPORT` today.
- Wire-layer tests that DO exist and are the model for new ones:
  `MobileGL/MG_Test/Wire/{RingTest,FramingTest,FdPassingTest,InProcessTransportTest,ProtocolSmokeTest}.cpp`,
  registered only under the option (`MobileGL/MG_Test/CMakeLists.txt:93-95`).
- `InProcessTransport` today is a **mutex + two condvars** message queue
  (`MG_Remote/Transport/InProcessTransport.cpp:41-51`: `std::mutex mutex`,
  `condition_variable cv` for messages and `fdCv` for fd offers). It is the control plane only —
  it does **not** create a thread and it does **not** run the G3 codec. P5's "InProcessTransport
  runs the SAME G3 codec as spawn" means driving `PipeWire.inc`
  (`MG_Pipe/generated/PipeWire.inc`, 939 lines) through it, plus a `SEG_CMD` ring
  (`MG_Remote/Transport/Ring.{h,cpp}`) and a `Doorbell`. Note the header's own comment
  (`InProcessTransport.h:13`): *"side is the monolith's own render thread, which is the single
  largest CPU…"*.

---

## 7. Quick reference — the file:line list P5 will keep reopening

| what | where |
|---|---|
| **the hook** | `MobileGL/MG_Backend/Init.cpp:50` (before the `switch` at `:51`) |
| backend table populated from the object | `MobileGL/MG_Backend/Init.cpp:43-44` |
| the two globals | `MobileGL/GlobalObjects.cpp:23-24`; decls `MG_Backend/BackendObjects.h:16-17` |
| `BackendObject` interface | `MobileGL/MG_Backend/BackendObject.h:567-644` (8 pure, 9 EGL, 2 protected) |
| `GlobalBackendFunctionsTable` | `MobileGL/MG_Backend/BackendObject.h:293-299` |
| `DynamicBackendParameters` | `MobileGL/MG_Backend/BackendObject.h:297-…` |
| base EGL impl | `MobileGL/MG_Backend/BackendObject.cpp:168-494` (MakeEGLCurrent `:306`, caps-on-first-current `:341`) |
| DirectGLES EGL overrides | `MG_Backend/DirectGLES/BackendObject_DirectGLES.cpp:875-989` |
| **context owner slot** | `MG_Backend/DirectGLES/DirectGLES.cpp:11865` |
| driver make/release current | `DirectGLES.cpp:11919` / `:11958` (real `eglMakeCurrent` `:11925` / `:11964`) |
| **owner predicate** | `DirectGLES.cpp:12002-12025` (+ stamp `:11980-11996`) |
| buffer-path owner gate | `MG_Backend/DirectGLES/Managers.cpp:928-930` (19 users) |
| native context destroy | `DirectGLES.cpp:12472-12510` |
| frontend EGL entry points | `MG_Impl/EGLImpl/EGLImpl.cpp` — MakeCurrent `:240`, SwapBuffers `:162`, Terminate `:319`, ReleaseThread `:341`, SwapInterval `:435`, DestroySurface `:304`, mutex `:48` |
| library teardown | `MobileGL/Init.cpp:38-98` |
| the two pipe tables | `MG_Pipe/generated/PipeTables.inc:17` / `:33`; instances `MG_Pipe/MGPipe.h:137-138` |
| the thunks nobody calls | `MG_Pipe/generated/PipeThunks.inc:20-302` |
| the applier actually in use | `MG_Pipe/PipeApply.h:740-1052`, `MG_Pipe/PipeApply.cpp` |
| backend op-table precedent | `MG_Pipe/PipeApply.h:85-102`; `PipeApply.cpp:402, 1094-1095`; `Managers.cpp:2298-2308` |
| server-side state block | `MG_Backend/MGPipe/PipeInputs.h:706` (`gPipeInputs`) |
| config | `MobileGL/Config.h:319-398`; `MobileGL/ConfigLoader.cpp:248-282`; enum precedent `:284-300` |
| CMake gating | `CMakeLists.txt:23, 453-466, 484-499, 503-520, 572-574, 605-609` |
| design of record | `docs/Disaggregated/ARCHITECTURE.md:29, 213, 500-502, 516-518, 531-537, 576-584, 595, 613` |
| ROADMAP P5 row | `docs/Disaggregated/ROADMAP.md:21` |
