# P3a specification, extracted from the docs (scout, read-only)

Sources actually opened: `docs/Disaggregated/{README.md, ARCHITECTURE.md, ROADMAP.md, MEASUREMENTS.md}` in
`C:/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher/MobileGL-disagg`, plus
`~/w7/notes/p2/INTEGRATOR-DECISIONS.md` and `~/w7/notes/p2/BRIEF-P2.md` (sections A, D4, D12, D13, D14, D17, D18, D19, D20, D.5, E).
Where a payload or a hook is quoted I also opened the tree file: `MobileGL/MG_Pipe/PipeCalls.def`,
`MobileGL/MG_Pipe/MGPipeTypes.h`, `MobileGL/MG_State/GLState/BufferState/BufferObject.h`,
`MobileGL/MG_Backend/DirectGLES/{Managers.h,Managers.cpp,DirectGLES.cpp}`,
`MobileGL/MG_Backend/DirectVulkan/Renderer/VkBufferManager.cpp`, `tools/trace_replay/trace_cases.json`.
Line numbers are from those files as they stand on `feat/disaggregated` today.

---

## 1. The P3a row of the phase table

`docs/Disaggregated/ROADMAP.md:19`, verbatim (all five cells):

> | **P3a** handle wave 1（Espryt）：buffer、VAO | 18–23 | 7 个 `BufferBackendOps` → `resource_*`、`buffer_subdata_resident`（可 null）、`resource_flush_range`、`resource_readback`、`map_persistent`（不碰实现）；pool 与延迟释放原样搬；vertex elements 三件（两个视图都带）；`set_vertex_buffers`（`baseInstance` 显式字段）；`set_index_buffer`；Adreno SIGSEGV workaround 保留 | 全套门；buffer/VAO 族场景（`LargeArenaAdoption`、`StorageBufferRegrow` 发布 `map-persistent-roundtrips`、`VertexAttribBinding`、`MultiDraw`、`PrimitiveRestart`…）；Create/rd12/26.3/sodium trace；MC 26.3 在 Adreno 上 p99 不变。**再基线检查点 1：超过 27 天必须重定基线** | P2 |

Cell by cell: title = handle wave 1, **Espryt only**, subject buffer + VAO (Magma's buffer path is P7; Magma's VAO memo re-key already landed in P2 as subsystem 4, `ROADMAP.md:18`, `BRIEF-P2.md:303`); days 18–23; cumulative low end P2 43 → **P3a 61** (`ROADMAP.md:32`); dependency = P2, and P4a depends on P3a (`ROADMAP.md:20` last cell).

### 1.1 Deliverables, item by item

**D-a — the seven `BufferBackendOps` become `resource_*` calls.**
`struct BufferBackendOps` is `MobileGL/MG_State/GLState/BufferState/BufferObject.h:76-122`. The seven pointers, in declaration order, with the contract each doc comment states (these contracts are what the pipe call must preserve):

| hook | line | contract, as written |
|---|---|---|
| `Respecify(BufferObject&)` | `:80` | storage (re)definition = `glBufferData`/`glBufferStorage`; "The orphaning point - the backend decides (busy-tracking) whether to swap storage or write in place. Shadow already holds the new contents." |
| `SubData(BufferObject&, SizeT offset, SizeT size)` | `:82` | "Contents update of [offset, offset + size) from the shadow." |
| `ResidentSubData(BufferObject&, SizeT offset, DataPtr data)` | `:95` | update of an **adopted (GPU-resident)** store; `data` holds the app's bytes and is "valid for the duration of the call only (a write map's staging store is freed the moment the unmap that lands it returns)"; the frontend has **not** touched the resident mapping; an in-place host write would tear frames still reading the old bytes (Minecraft patches live chunk sections this way); the backend instead lands the bytes on the GPU timeline "after in-flight readers, before the next consumer"; the frontend marks the buffer gpu-write-pending so reads reconcile through `ReadbackFromGpu`; "Backends without this op keep the legacy ordered in-place host write." |
| `FlushMappedRange(BufferObject&, Range1D, Flags<BufferMappingAccessBit> appAccess)` | `:99` | write-map flush (`glUnmapBuffer`/`glFlushMappedBufferRange`); "Carries the app's real mapping flags so the backend can honour INVALIDATE_* / UNSYNCHRONIZED semantics per call instead of merging them." |
| `OnDestroy(SharedPtr<BackendBufferResource>&&)` | `:103` | "Final release of the backend resource (called from ~BufferObject). The backend defers actual destruction until the GPU is done with it." |
| `AcquirePersistentMap(BufferObject&) -> void*` | `:114` | zero-copy persistent map; for a coherent (non-`FLUSH_EXPLICIT`) persistent write map the backend may return a host-visible, coherent, persistently mapped pointer into its own GPU storage for the whole buffer `[0, size)`, created with every buffer usage and seeded from the shadow; "From that point the GPU buffer is the single source of truth ... and NO further backend transfer ops are dispatched for this buffer"; returns `nullptr` when it cannot back the map; **must be idempotent**. |
| `ReadbackFromGpu(BufferObject&)` | `:121` | pulls the backend's current contents for the whole buffer into the shadow through `WritebackFromBackend`; "Only ever called for a buffer the GPU may have written behind the frontend's back - a shader storage or atomic counter binding of a draw or dispatch"; backends that cannot read back leave it null and the shadow keeps its pre-dispatch bytes. |

Registration: Espryt's table `g_glesBufferBackendOps` sets **all seven** (`MG_Backend/DirectGLES/Managers.cpp:1482-1490`), installed by `RegisterBufferBackendOps()` (`:1505-1507`, called from `BackendObject_DirectGLES.cpp:849` and `DirectGLES.cpp:10402`), removed by `UnregisterBufferBackendOps()` (`:1512-1515`, and `:1528` notes it "also bumps the buffer-mutation epoch"). The dispatcher is `SetBufferBackendOps`/`GetBufferBackendOps` (`BufferObject.h:124-127`: "Registered by the active backend at init, cleared at shutdown. Null table (unit tests, benchmarks) => shadow-only state tracking.").

The call-catalogue targets (`MobileGL/MG_Pipe/PipeCalls.def`, screen block and `kCtxObject` block):

```
X(ResourceCreate,          MGPResourceDesc,  kScreen,    kNone)
X(ResourceRespecify,       MGPResourceDesc,  kScreen,    kNone)
X(ResourceDestroy,         MGPHandleOnly,    kScreen,    kNone)
X(MapPersistent,           MGPHandleOnly,    kScreen,    kReplySlot|kOptional)
X(UnmapPersistent,         MGPHandleOnly,    kScreen,    kOptional)
X(ResourceSubData,         MGPSubData,       kCtxObject, kHasBlob|kVarTail)
X(BufferSubDataResident,   MGPSubData,       kCtxObject, kHasBlob|kOptional)
X(ResourceSubDataComplete, MGPSubDataComplete, kCtxObject, kNone)
X(ResourceFlushRange,      MGPFlushRange,    kCtxObject, kNone)
X(ResourceReadback,        MGPReadback,      kCtxObject, kReplySlot)
X(ResourceCopyRegion,      MGPCopyRegion,    kCtxObject, kNone)
```

Mapping (the one-to-one the phase asks for): `Respecify` → `ResourceRespecify`; `SubData` → `ResourceSubData`; `ResidentSubData` → `BufferSubDataResident`; `FlushMappedRange` → `ResourceFlushRange`; `OnDestroy` → `ResourceDestroy`; `AcquirePersistentMap`/unmap → `MapPersistent`/`UnmapPersistent`; `ReadbackFromGpu` → `ResourceReadback`. Plus `ResourceCreate`, which has no hook today: `ARCHITECTURE.md:204` — "`resource_create` 在前端对象构造时发，存储由 `resource_respecify` 惰性定义；`resource_destroy` 在析构时发."

**D-b — `buffer_subdata_resident` (nullable).** `kOptional` means "后端表里可为 null" and `ARCHITECTURE.md:102` names the exception explicitly: "`kOptional`(O，后端表里可为 null：Magma 故意不注册 `BufferSubDataResident` 与 `SetSwapInterval`)". Verified in the tree: `g_vulkanBufferBackendOps` (`MG_Backend/DirectVulkan/Renderer/VkBufferManager.cpp:105-112`) sets `.Respecify`, `.SubData`, `.FlushMappedRange`, `.OnDestroy`, `.AcquirePersistentMap`, `.ReadbackFromGpu` and **omits `.ResidentSubData`**. The standing rule against "fixing" it in flight is `ROADMAP.md:87` (open question 10): "**`ResidentSubData` 的不对称怎么收口。** null 项保住今天的行为；给 Magma 补真实现是行为变更，独立 `dev` PR." Espryt's arm is `Ops_ResidentSubData` (`Managers.cpp:1304`) wrapped as `Ops_ResidentSubDataTracked` (`:1457-1458`).

**D-c — `resource_flush_range`.** `MGPFlushRange` (`MG_Pipe/MGPipeTypes.h:629-636`), preceded by the comment "Carries the application's REAL access flags, not a normalized subset" (`:628`):

```cpp
struct MGPFlushRange {
    MGPipeHandle Res;
    Uint64 Offset, Size;
    Uint32 AccessFlags; // Flags<BufferMappingAccessBit>
    Uint32 Pad0;
};
MGP_ASSERT_POD(MGPFlushRange, 32);
```

**D-d — `resource_readback`.** `MGPReadback` (`MGPipeTypes.h:637-641`):

```cpp
struct MGPReadback { MGPipeHandle Res; Uint64 Offset, Size; };
MGP_ASSERT_POD(MGPReadback, 24);
```

`kReplySlot` = "答进 `MGPReplySlot`，永不阻塞" (`ARCHITECTURE.md:102`); `MGPReplySlot{Uint64 Id;}` / 8 B (`MGPipeTypes.h:78-83`), documented as "Where an asynchronous answer lands. Every server query in this catalogue is async-with-handle; none of them blocks".

**D-e — `map_persistent`, implementation untouched.** This is D-B4, `ARCHITECTURE.md:466`: "`AcquirePersistentMap` 是永久的地址空间捐赠（返回 host-visible coherent 指针，成为该 buffer 的唯一真相源；≥16 MiB 可变 store 由 `TryAdoptLargeStorage` 自动走到，实测 MC 26.3 p99 163→21 ms、40→115 fps、省 ~400 MB）。**整个 monolith 改造期一动不动**（D-B4），只有 IPC 那一步会打破它。" P3a therefore routes the call through the pipe and changes nothing inside; the tiering work (T0/T1/T2/T3), `SEG_ADOPT`, and `MOBILEGL_IPC_ADOPT_TIER` are all P11 (`ARCHITECTURE.md:468-486`, `ROADMAP.md:28`).

**D-f — pools and deferred release move as-is.** `ARCHITECTURE.md:315` (§9.1, "原样不动的东西") lists for Espryt: "三条 persistent-mapped ring 与 `PersistentRing` 算法、**buffer pool**、7 条 fallback-repack 路径、`m_backendColorSlots` 置换表、三个 scratch FBO 及驱动侧影子、`PackState`、全部驱动绑定影子、**Adreno 禁用属性 SIGSEGV workaround**、Mali XFB 捕获丢失 workaround、`ScopedDefaultUnpackState`、SPIRV-Cross 会话与 post-emission ESSL 重写、驱动 POST 自检族、restart 重写与 multi-draw 五档." Pool maintenance entry points: `TrimBufferPool()` / `ClearBufferPool()` (`MG_Backend/DirectGLES/Managers.h:730-734` — "TrimBufferPool evicts over-budget entries (called once per frame from Present); ClearBufferPool drops all pooled ids"). The frame-boundary constraint that protects them: `ARCHITECTURE.md:515` — "**`present` 与 `eglSwapBuffers` 严格 1:1**：两个后端的帧边界排空（… Espryt 三个 ring 与 `TrimBufferPool` 的 retire）只在 `Present` 内发生，批量会饿死它们."

**D-g — vertex elements, three calls, both views.** `X(CreateVertexElements, MGPVertexElements, kCtxCso, kHasBlob)`, `X(BindVertexElements, MGPHandleOnly, kCtxCso, kNone)`, `X(DeleteVertexElements, MGPHandleOnly, kCtxCso, kNone)`. Payload (`MGPipeTypes.h:253-259`):

```cpp
struct MGPVertexElements {
    MGPipeHandle Cso;
    Uint32 AttributeCount;
    Uint32 BindingPointCount;
    MGPBlobRef Blob; // VertexAttribute[] followed by VertexBufferBindingPoint[]
};
MGP_ASSERT_POD(MGPVertexElements, 40);
```

"两个视图都带" is `ARCHITECTURE.md:130` verbatim: "blob 同时带解析后的 `VertexAttribute[]` **和** `VertexBufferBindingPoint[]`，缺一不可（pointer 调用的 stride 0 = element size，binding 模型的 stride 0 = 每顶点读同一 element）；`IsLong` 与 `Type == Float64` 分开携带；仅供查询的 `LegacyStride/LegacyPointer` 留在 client." The CSO's server-side counterpart is `VertexInputStateFactory::m_cache` (`ARCHITECTURE.md:55`); CSO content addressing lives on the client with a per-kind `ska::flat_hash_map<xxHash, MGPipeHandle>`, capacity **vertex-elements 1024**, LRU eviction emits `delete_*` (`ARCHITECTURE.md:63`).

**D-h — `set_vertex_buffers` with an explicit `baseInstance`.** `X(SetVertexBuffers, MGPVertexBuffers, kCtxState, kVarTail)`. Landed payload (`MGPipeTypes.h:356-372`):

```cpp
struct MGPVertexBuffer { MGPipeHandle Res; Uint64 Offset; Uint32 Stride; Uint32 Divisor; Uint32 BindingIndex; Uint32 Pad0; };
MGP_ASSERT_POD(MGPVertexBuffer, 32);
// Var-tail header: MGPVertexBuffer[Count] follows.
struct MGPVertexBuffers { Uint32 Start, Count; Uint64 ContentHash; };
MGP_ASSERT_POD(MGPVertexBuffers, 16);
```

**There is no `BaseInstance` field yet** — `grep -rn "BaseInstance" MobileGL/MG_Pipe/` hits only draw-entry names in `FillPoints.def:86,88,90` and their generated mirrors. So this deliverable is a payload change (a size change, hence a new `MGP_ASSERT_POD` number and a G3 wire `static_assert` update). Why it is required: Espryt emulates `baseInstance` by baking a byte shift into the instanced arrays' offsets, driven by an **ambient process global** — `g_pendingFetchBaseInstance` (`MG_Backend/DirectGLES/Managers.cpp:2358-2366`, `SetPendingFetchBaseInstance`/`GetPendingFetchBaseInstance`), set by `VertexArrayImpl::ScopedFetchBaseInstance` around three draw entry points (`DirectGLES.cpp:5135, 5172, 5224`, value from `EmulatedFetchBaseInstance`, `:5128`), consumed in the VAO sync (`Managers.cpp:2463-2464` `const Uint32 fetchBaseInstance = g_pendingFetchBaseInstance; const Bool baseInstanceDirty = m_syncedFetchBaseInstance != fetchBaseInstance;`, then `:2524`, `:2589` `attrib.Offset + BaseInstanceByteShift(attrib, fetchBaseInstance)`, `:2646`, and `:2758-2801` inside `SyncFloat64AttributeAsFloat32`). The state comment names the problem exactly (`Managers.h:942-946`): "Byte shift currently baked into the instanced arrays' offsets by the baseInstance emulation (see SetPendingFetchBaseInstance). It is **draw state, not VAO state**, so it is deliberately NOT covered by the config version: the frontend never bumps for it. Kept here because it describes what was last EMITTED, which is what the next sync has to correct." An ambient global cannot cross a pushed boundary; `MGPDrawInfo` already carries `StartInstance` (`MGPipeTypes.h:719`), so the explicit field is the disambiguation between "the GL draw's baseInstance" and "the vertex-fetch shift the VAO was emitted with".

**D-i — `set_index_buffer`.** `X(SetIndexBuffer, MGPIndexBuffer, kCtxState, kNone)`; `MGPipeTypes.h:373-380`:

```cpp
// An independent call, NOT a subset of the VAO configuration version (D5).
struct MGPIndexBuffer { MGPipeHandle Res; Uint64 Offset; Uint32 IndexSize; Uint32 Pad0; };
MGP_ASSERT_POD(MGPIndexBuffer, 24);
```

Same rule in prose, `ARCHITECTURE.md:101`: "`SetIndexBuffer` 独立于 VAO 配置版本（D5）：索引 slot 重绑不移动 VAO config version." The tracker's shutter for it is dirty bit 10 `NEW_INDEX_BUFFER`, "index slot version + bound object `{slot, gen}`" (`BRIEF-P2.md:133`ff table; `ARCHITECTURE.md:164`).

**D-j — the Adreno SIGSEGV workaround is kept.** `ARCHITECTURE.md:315` keeps it in the do-not-touch list. The code: `VertexArrayImpl::SyncFloat64AttributeAsFloat32(Uint attribIndex, const VertexAttribute&, Uint32 fetchBaseInstance)` (`MG_Backend/DirectGLES/Managers.h:886-896`), whose comment is the workaround's statement: "Narrows one enabled GL_DOUBLE array into a tightly packed float32 stream held in this VAO's own scratch buffer and declares the attribute against it. ES has no 64-bit vertex format ... Returns false when the stream cannot be built, in which case the caller must DISABLE the array - leaving a 64-bit array enabled with no pointer is what the Adreno driver turns into a SIGSEGV at the next draw." Two more SIGSEGV notes in the same path: `Managers.cpp:2520` ("and take the process with it (SIGSEGV in libGLESv2_adreno,") and `:2572` ("and kills the process rather than reporting an error (SIGSEGV in"). The memo that backs it is `ConvertedFloat64Stream` (`Managers.h:900-910`), keyed on `sourceLifetimeId`/`sourceChangeSerial`/`sourceOffset` — a `lifetimeId`-keyed memo, i.e. exactly the Track H re-key shape (`ARCHITECTURE.md:363`: "`ConvertedVertexStreamKey` 的 `sourcePin`" is one of the 11 memos the handle scheme deletes outright). The wire enabler is that `MGPVertexElements` carries `IsLong` and `Type == Float64` **separately** (`ARCHITECTURE.md:130`), and fp64 narrowing stays a server-side emulation (`ARCHITECTURE.md:236`: "fp64 顶点窄化 | server | 原始字节；`IsLong` 与 `Type` 分开过线"), advertised by `kCapFloat64VertexAttrib` (`ARCHITECTURE.md:106`).

### 1.2 The acceptance gate, decoded

The gate cell reads, verbatim: "全套门；buffer/VAO 族场景（`LargeArenaAdoption`、`StorageBufferRegrow` 发布 `map-persistent-roundtrips`、`VertexAttribBinding`、`MultiDraw`、`PrimitiveRestart`…）；Create/rd12/26.3/sodium trace；MC 26.3 在 Adreno 上 p99 不变。**再基线检查点 1：超过 27 天必须重定基线**".

**(a) 全套门 = the five-part validation gate**, `ARCHITECTURE.md:497-507` (§13.2), which replaces the byte-identity gate:

1. **Three interface-purity gates** (non-verify builds only): **A — include graph**: compiling `MG_Backend` in the disaggregated configuration with `MG_State/GLState` removed from the include search path (`nm --undefined-only` is blind to "included but not called", and `RenderState.h → FramebufferObject.h → TextureObject.h` is exactly that coupling); depends on P0.5. **B — symbols**: `nm --undefined-only libMobileGLServer.so | grep -E 'MG_State::GLState::|glslang'` is empty. **C — undeclared**: `grep -c 'pGLContext' MG_Backend/` == 0. Plus a debug assertion "每个后端 memo 键都是 `{slot, gen}`，永不是裸前端指针", backed by `HandleRecycleScenario` (which "重键前必须在至少一个后端上是红的").
2. **Semantic shadow comparison `MOBILEGL_PIPE_VERIFY=1`** — the decisive one; field-wise, per draw, prints the first diverging field and the draw index; catches fields the tracker forgot and **dirty bits that fire too rarely** (the dangerous direction); third CI mode; 40 traces + all integration tests; 5–10x slower; never shipped; field-wise not `memcmp` (padding false-DIFFERs). "**保留模式**" for consume-and-clear groups. "**活过 P13**".
3. **Behavioural A/B**: 40 traces under `{monolith-pull, monolith-push, split}` at SSIM ≥ 0.99; `ctest -L integration-gpu` name-for-name identical between `DirectGLES.` and `DirectGLES.Pipe.`/`DirectGLES.Split.` (same for DirectVulkan); unit tests green; CTS per-backend conformance within 0.5 pp (rows = GL version/extension, columns = status counts, rate = Pass/(Pass+Fail), NS not in the denominator). The name-for-name functional baseline is "P1 出口的重构后 monolith"; `81b17c0b` is only the performance anchor.
4. **Monolith performance non-regression**: two devices, reboot-clean, same thermal window, paired A/B, `tools/bench.sh` + trace replay `--benchmark` per-frame JSON; **metric is per-thread CPU time**, p50 and p99; **absolute threshold** — tracker ns/draw published with a ceiling; Blaze3D blend-toggle microbenchmark listed separately; CSO-content-addressing-off negative control. (See §5 for the 2026-09-08 amendment: for P3a this part is recorded, not gated.)
5. **Coverage + poison + handle discipline**: G6 regenerates with 0 UNMAPPED; `gen_pipe_dirty_surface.py` regenerates with 0 unmapped mutators; per-verb generation poison; G7 setter-consistency test; `ResidualValueBlock`'s `offsetof` assertions.

Two byte-level equalities survive (`ARCHITECTURE.md:508`): with `MOBILEGL_BUILD_DISAGGREGATED=OFF`, `nm --defined-only libMobileGL.so | grep MG_Remote` is empty and the link line gains no library; `nm -D libMobileGL.so | grep mobilegl_server_main` hits in RelWithDebInfo. Symbol and `.text` drift are published per phase as informational.

**(b) The buffer/VAO scenario family, by name.** All five named scenarios already exist as files in the tree (so P3a extends/arms them rather than inventing them): `MobileGL/MG_IntegrationTest/Scenarios/LargeArenaAdoptionScenario.cpp`, `StorageBufferRegrowScenario.cpp`, `VertexAttribBindingScenario.cpp`, `MultiDrawScenario.cpp`, `PrimitiveRestartScenario.cpp`. Neighbours in the same directory that the P2 brief already binds to buffer/VAO behaviour: `CrossFrameBufferScenario.cpp`, `ResidentIndexScenario.cpp`, `HandleRecycleScenario.cpp` (P2's D18 negative control). The roadmap's "…" is open-ended; `BRIEF-P2.md:620` lists the existing must-not-break set for the Track H slice as `SanityTest.cpp:2575-2596, :2650-2665, :2757-2790, :3020-3062` plus `Scenarios/{SampledSetStalenessScenario, CrossFrameBufferScenario, VertexArrayEnableDisableScenario, VertexAttribBindingScenario, MultiDrawScenario, ResidentIndexScenario}.cpp`.

`StorageBufferRegrow` must **publish `map-persistent-roundtrips`**. Same counter is demanded at P5 (`ROADMAP.md:21`) and P11 (`ROADMAP.md:28`); its meaning is set by `ARCHITECTURE.md:474` (T1 costs "**每次存储定义一次 round trip**（不是每 store 一次），`StorageBufferRegrowScenario` 发布 `map-persistent-roundtrips`"). In monolith P3a the number is expected to be zero/absent — the point is that the counter exists and is wired before P5 needs it.

`LargeArenaAdoption` is the >=16 MiB adoption scenario; its P11 form is "`LargeArenaAdoptionScenario` 在所选档下绿" (`ROADMAP.md:28`).

**(c) Traces: Create / rd12 / 26.3 / sodium.** Exact case names in `tools/trace_replay/trace_cases.json` (40 cases total):
- `minecraft-1.21.1-neoforge-create-indirect-in-world` — carries `"coherent_as_flush": true` (json `:270,275`)
- `minecraft-1.21.1-neoforge-create-instancing-in-world` — carries `"coherent_as_flush": true` (json `:278,283`)
- `minecraft-1.21.4-rd12-odinlite-in-world`
- `improved-transparency-minecraft-26.3`
- `minecraft-1.21.4-fabric-sodium-in-world`

Two caveats that bind P3a directly:
- **`create-indirect` fails on BOTH devices today and it is a pre-existing `dev` bug**: `MEASUREMENTS.md:96` — "Adreno 830：Espryt ~4.5 分钟后黑帧，Magma 纹理上传提交时 `VK_ERROR_DEVICE_LOST`；Mali：SSIM 0.85 / 0.45 … Adreno 830 上用 `dev@81b17c0b` 基线 APK 复现，**是基线就有的问题，不是本分支造成**；它是 P3a/P8 验收清单里的用例，需先在 `dev` 修." Repeated as open question 17, `ROADMAP.md:90`. So P3a's gate is blocked on a `dev`-side fix that is not P3a's own work.
- `MOBILEGL_COHERENT_AS_FLUSH` "在拆分模式下照常生效：两个带 `coherent_as_flush: true` 的 Create fixture 在 split 与 monolith 下走同一条 buffer 路径，逐名对比才有意义" (`ARCHITECTURE.md:485`).

**(d) "MC 26.3 在 Adreno 上 p99 不变"** — the one hard device number in the P3a gate. Its baseline is the adoption result recorded in `MEASUREMENTS.md:87`: "**persistent map 采纳的既有基线**（`dev`，MC 26.3，Adreno）：>=16 MiB 可变 store 定义时采纳为 coherent persistent map 后 p99 163→21 ms、稳态 40→115 fps、省 ~400 MB。P11 的回归上限对着它." The corpus name is `improved-transparency-minecraft-26.3`, whose P0 boundary-counter baseline is in `MEASUREMENTS.md:60-61` (1200-frame window, 1320 draws/frame; Espryt acc/draw **8.44**, buf 333 K B/f, gates ers 156925/2791, etl 148606/11110, eub 148246/11470; Magma acc/draw **6.53**, buf 173 K B/f, `stage-ubo-named` **331 K B/f**, gates mfp 21360/137036, mpm 134421/2615, mdt 156611/1785).

**(e) Re-baseline checkpoint 1**, verbatim from the gate cell: "**再基线检查点 1：超过 27 天必须重定基线**", and again in the checkpoint table, `ROADMAP.md:63-64`: "| 触发 | 动作 | / | P3a > 27 天 | "窄句柄化"的前提错了，P4a 开始前重定基线 |". `ROADMAP.md:67`: "任一触发，先跑 `inproc` 的证伪数字再决定是否继续." (P4a's own is "> 39 天", `:65`; P7's is the midpoint <40% rule, `:66`, with the note that "P3a 的检查点发现不了 Magma 特有的超期".)

---

## 2. Every other mention of P3a in the docs

1. `ROADMAP.md:9` — the two lanes: "**monolith 跑道** P0 → P0.5 → P1 → P2 → **P3a** → P4a → P3b/P4b → P7 → P8 → P13，每段可独立交付、可随时中止且 monolith 严格好于起点；**IPC 跑道** P5 → P6 → P9 → P10 → P11 → P12."
2. `ROADMAP.md:32` — day arithmetic: "P2 43 → **P3a 61** → P4a 87 …".
3. `ROADMAP.md:34` — CTS turnaround: "`gl44to46` 约 56,271 例。逐阶段只跑该阶段可能影响的具名块（P4a `packed_pixels`、P3b/P4b `texture_*`/`shader_image_*`、P9 `transform_feedback*`）；**完整 caselist 只在五个架构边界（P0.5、P3a、P4a、P3b/P4b、P13）与每次合并 `dev` 之前跑**，放 CI 不放关键路径。若周转仍主导排期，加宽估时而不是削弱门." → **P3a is one of the five boundaries that owes a full 56,271-case caselist run on both devices**, off the critical path.
4. `ROADMAP.md:63-64` — re-baseline checkpoint 1 (see 1.2(e)).
5. `ROADMAP.md:66` — "P3a 的检查点发现不了 Magma 特有的超期" (why P7 has its own).
6. `ROADMAP.md:90` — open question 17: the `create-indirect` fixture failure "是 P3a/P8 验收清单里的用例，需要先在 `dev` 上修".
7. `ARCHITECTURE.md:293` — ack policy: "目录里目前没有条目携带 `kNeedsAck`（`ResourceRespecify` 是 `kNone`），**标记随 P3a 的 buffer 路径落地**." Context (`:291-294`): texture allocation OOM is already deferred to sync time in monolith so that class does not ack; "**唯一允许同步 ack 的入口是 `glBufferStorage`（真同步分配）**"; `glRenderbufferStorage*` does not ack (41 trace fixtures, 0 OOM-probe idioms, 9 calls across 5 fixtures, none followed by `glGetError` within 3 calls; the corpus's real success check is `glCheckFramebufferStatus`, answered locally by the client); everything else arrives late through the ordered `OnGlError`. → **P3a is the phase that first sets `kNeedsAck`, on the `glBufferStorage` path only.**
8. `ARCHITECTURE.md:367` — "对策：**编译期** `MOBILEGL_PIPE_LEGACY_MEMOS`（默认 ON）**在 P3a/P4a 期间**保留 registry / `TwinLookupMemo` 实现活在同一个 `PipeInputs` 接口之下，随 pull 路径在 P13 退役（各阶段 +1 天维护）." Reason (`:365-366`): after phase C the `MOBILEGL_PIPE_PUSH` bitmap is no longer a valid A/B, because with a bit cleared `SnapshotFromGLContext()` still synthesises handles and the backend still runs the re-keyed memo code — a re-key bug is in both arms.
9. `MEASUREMENTS.md:96` — `create-indirect` is a P3a/P8 acceptance case, pre-existing on `dev` (quoted above).
10. `BRIEF-P2.md:156` and `:956` — **P2 emits only for dirty bits 0–4; bits 5–17 are computed, counted and asserted but "their emission is P3a/P3b/P4a/P4b"**, and "the per-bit fire rates go into `MEASUREMENTS.md` **so P3a starts from data**". The bits P3a owns out of that table (`BRIEF-P2.md` D4): **5 `NEW_VERTEX_ELEMENTS`** (shutter `VertexArrayObject::GetConfigVersion()`), **9 `NEW_VERTEX_BUFFERS`** (object class, shutter `VertexArrayState::m_anyVaoAttributeGeneration`, then a 32-attribute prefix walk), **10 `NEW_INDEX_BUFFER`** (index slot version + bound object `{slot, gen}`), and the buffer-binding trio **15–17 `NEW_CONST_BUFFERS`/`NEW_SHADER_BUFFERS`/`NEW_SO_TARGETS`** (object class, shutter `BufferState::m_anyBufferChangeGeneration`, `GetTouchedBindPointCount()` prefix).
11. `BRIEF-P2.md:293` — the set-hash suppressor: P2 wires exactly one of the seven `kVarTail` slots (`SetVertexAttribDefaults`) and unit-tests the class on all seven; "The other six are P3b/P4b (`ROADMAP.md:23`)" — note the tension with P3a landing `SetVertexBuffers`: the **call** is P3a's, its **suppressor slot** is scheduled P3b/P4b, and a hash of 0 is reserved for "never emitted" so the first emission always goes out.
12. `BRIEF-P2.md:969` — "Widening the scan root [of `gen_pipe_dirty_surface.py`, today `MG_Impl/GLImpl` only, which leaves the four `MGP_NOTE_MUTATION` sites in `TextureState.h` outside it] is **recorded as a P3a item**." Its two sibling blind spots, also written into `DirtySurface.def`'s header comment: a mutator inside a lambda is attributed to the enclosing function; a mutation published through a helper reads as deferred.
13. `BRIEF-P2.md:928` (D.4.6) — the Track H unit-cost census P3a inherits: `ARCHITECTURE.md:363` counts 11 direct deletions, 2 moved to client debounce, 7 re-keys, 1 unchanged; "P2 pays into that census: **11 of the 11 direct deletions and 2 of the 7 re-keys**. The remaining slices are P3a/P3b/P4a/P4b/P7 and their unit cost is extrapolated from these two." Criterion (`ROADMAP.md:56`): "Track H 单位成本不超出估计的 50%".
14. `BRIEF-P2.md:822` — a P2 decision that lands on P3b, adjacent to P3a's buffer work: `integration`'s second, `MOBILEGL_ESPRYT_DISABLE_INVALIDATE_FLUSH=1` filtered pass (186 entries) is **not** run under the comparator in P2 ("the 186 entries are a buffer/readback filter that P2 changes nothing in; revisit at P3b").

---

## 3. What ARCHITECTURE.md says about buffers and VAOs at the boundary

### 3.1 Where a buffer op is pushed — the one exception to push-at-validate

`ARCHITECTURE.md:149-155` (§5.1) is the rule and its exception:

- Push happens at the **validate moment before a verb**, never in the GL setter: "Blaze3D 每个 batch 用 `glEnable/glDisable(GL_BLEND)` 包住（Espryt 代码自己标它为最热路径），per-setter 推送会把每次冗余开关变成一次接口调用加一次 server 侧 CSO 查表，严格慢于今天。正确形态是 gallium `st_validate_state`."
- Eight validate entry points, generated from `PipeCalls.def`'s `kCtxVerb`/`kCtxObject` rows: `ValidateForDraw` (20 draw entry points), `ValidateForDispatch`, `ValidateForClear`, `ValidateForBlitOrCopy`, `ValidateForTextureOp` (GenerateMipmap / CopyTex* / BindImageTexture), `ValidateForReadback`, `ValidateForXfbSpan`, `ValidateForQuery`. Eight, not four, because of the ~70 table entries `MG_Impl` uses only ~22 are draw/dispatch. (P2's D1 raised this to nine — `BRIEF-P2.md` D.5 records the doc edit "`ARCHITECTURE.md:153` — 八个 validate 入口 → 九个".)
- **The exception, verbatim (`ARCHITECTURE.md:155`)**: "**只有今天就在 GL 调用时刻分发的资源 op 在 GL 调用时刻推送**——即 `BufferBackendOps` 的七个 hook。纹理 subdata 不在此列（§6）." So the seven buffer hooks are the *only* calls P3a pushes at GL-call time; everything else P3a touches (vertex elements, vertex buffers, index buffer) is pushed at validate.
- Contrast with textures (`ARCHITECTURE.md:247-249`): `glTexSubImage*` never calls the backend table at all; the client accumulates in its own `MipmapStorage` rect model and emits **one** `ResourceSubData` at the next validate/flush point, carrying union box **and** region list so the server picks the shape.

### 3.2 Ordering invariant for the calls P3a emits

`ARCHITECTURE.md:194` (§5.4, D-B3), verbatim: "**一条 verb 的全部 `set_*`/`bind_*` 必须在该 verb 之前完成；server 在 verb 处、从它此刻持有的全部已推送状态惰性特化 shader 与 pipeline。除"资源 create 先于对它的 bind"外，`set_*` 之间没有顺序要求。**" The recommended emission order is code organisation, not contract, and its vertex segment is "…render state/dynamic → **vertex elements/buffers/index/attrib defaults** → patch/XFB → verb".

Also §5.4 last line (`ARCHITECTURE.md:200`): "索引绑定范围在 validate 时刻实时解析（`glBindBufferBase` 之后再 `glBufferData` 是普通应用代码）" — i.e. `MGPBufferRange`s are resolved live, not cached from bind time. `MGPResourceDesc`'s `BufferForTexBuffer/BufOffset/BufSize` are likewise "实时解析（`kMGPipeWholeBuffer = ~0`）" (`ARCHITECTURE.md:127`).

### 3.3 The `resource_*` payloads, exactly as they stand

`MGPResourceDesc` (`MG_Pipe/MGPipeTypes.h:150-176`, `MGP_ASSERT_POD(MGPResourceDesc, 88)`, `kMGPipeWholeBuffer = ~0ull` at `:177`) — one discriminated create/respecify shape for buffer / every texture target / renderbuffer:

```cpp
struct MGPResourceDesc {
    MGPipeHandle Resource;
    Uint8  Target;        // Buffer | Tex1D..TexCubeArray | Tex2DMS.. | Renderbuffer | TexBuffer
    Uint8  StorageKind;   // == TextureStorageType (Mipmap | Buffer)
    // VERTEX|INDEX|CONSTANT|SHADER_BUFFER|INDIRECT|SAMPLER|SHADER_IMAGE|RENDER_TARGET|
    // DEPTH_STENCIL|STREAM_OUTPUT|ATOMIC|ELEMENT_ARRAY. The ELEMENT_ARRAY bit is the
    // D-B7 switch: with kCapNeedsHostIndexBytes set the server mirrors this resource.
    Uint16 BindMask;
    Uint32 InternalFormat;      // already resolved to an uncompressed fallback by the client
    Uint32 Width, Height, Depth;
    Uint16 ArrayLayers, Levels, Samples;
    Uint8  FixedSampleLocations, Immutable;
    Uint32 Usage;               // BufferUsage
    Uint32 StorageFlags;        // glBufferStorage flags
    Uint8  HasDefinedContent;   // false after a NULL-data respecify
    Uint8  ImageBindableHint;   // client-side everImageBound; pre-emptive allocation
    Uint16 Pad0;
    Uint32 GlNameForDiag;       // diagnostics only; never an identity, memo key or hash input
    Uint32 Pad1;
    MGPipeHandle ViewOf;             // storage owner for a texture view
    MGPipeHandle BufferForTexBuffer; // texture-buffer backing store
    Uint64 BufOffset, BufSize;       // kWholeBuffer == ~0, resolved live
};
```

Prose gloss, `ARCHITECTURE.md:127`: "`BindMask` 的 `ELEMENT_ARRAY` 位是索引镜像的开关；`ImageBindableHint` 预防性分配 image-bindable 存储；`ViewOf` 是纹理视图的存储属主（server 侧 keep-alive）；`BufferForTexBuffer/BufOffset/BufSize` 实时解析（`kMGPipeWholeBuffer = ~0`）。Renderbuffer 保持独立类（自己的 format-capability target、`ComponentSizes`、twin）。" So **P3a's buffer create/respecify is the call that first sets the `ELEMENT_ARRAY` bit that P8's index host mirror keys on** (§4.2 below). The buffer half also carries `Usage` and `StorageFlags` — the `glBufferStorage` flags that decide whether a store is immutable/persistent/coherent.

`MGPSubData` (`MGPipeTypes.h:587-607`, `MGP_ASSERT_POD(MGPSubData, 72)`), with the buffer-half convention spelled out in its own comment block (`:570-586`):

```cpp
struct MGPSubData {
    MGPipeHandle Res;
    Uint16 Target, Level;
    Uint8  SourceIsVerbatimLevelShadow; // replaces the backend's `uploadData == mipData` compare
    Uint8  Pad0[3];
    MGPBox UnionBox;
    Uint32 RegionCount;   // MGPSubRegion[] in the variable tail
    Uint32 Pad1;
    MGPBlobRef Blob;
};
```

The buffer half, verbatim from the header comment (`:574-585`): "THE BUFFER HALF. With `Target == Buffer` there is no level and no box, so the destination byte range rides in the box's first coordinate and first extent: `UnionBox.X` is the byte offset, `UnionBox.W` the byte size, `Y = Z = 0`, `H = D = 1`, `Level = 0`, `RegionCount = 0`, and `Blob` holds exactly `Size` source bytes. That caps ONE record at a 2^31-1 offset and a 2^32-1 size; a range beyond either is split by the emitter - the same rule, and at `SEG_STAGE`'s 32 MiB the far tighter one, that the ring's half-capacity bound already imposes on it. `MGPipeSetSubDataBufferRange` / `MGPipeSubDataBufferOffset` / `Size` below are the only spelling of this convention; nothing else reads the box for a buffer." The three helpers are `MGPipeSetSubDataBufferRange(MGPSubData&, Uint64 offset, Uint64 size) -> Bool` (`:609-618`, returns false and leaves the record untouched when the range does not fit — the emitter must split), `MGPipeSubDataBufferOffset` (`:619-623`, reads `UnionBox.X` as unsigned so a corrupt negative lands above the encodable bound and the applier's bounds gate refuses it) and `MGPipeSubDataBufferSize` (`:624`). The same rule in prose at `ARCHITECTURE.md:122`.

Supporting primitives: `MGPBlobRef{Uint64 Offset, Size; Uint32 Seg, Pad0;}` / 24 B (`:55-61`) — monolith: `Seg == kMGHostSpanSegNone` and `Offset` is an address in the caller's staging arena; split: `Seg` names a transport segment. `MGPRange{Uint64 Offset, Size;}` / 16 B (`:63-68`). `MGPBox{Int32 X,Y,Z; Uint32 W,H,D;}` / 24 B (`:70-76`). `MGPSubRegion{Int32 X,Y,Z; Uint32 W,H,D; Uint64 SrcOffset; Uint32 SrcRowStride, SrcSliceStride;}` / 40 B (`:566-579`) — "The source strides are CARRIED, not inferred from a pointer comparison" (texture path; `MGPSubRegion`'s stride rework is scheduled P3b/P4b, `ROADMAP.md:23`). `MGPHandleOnly{MGPipeHandle Handle; Uint32 Kind; Uint32 Pad0;}` / 16 B (`:93-99`) — "The payload of every call that carries nothing but an object identity", i.e. `ResourceDestroy`, `MapPersistent`, `UnmapPersistent`, `BindVertexElements`, `DeleteVertexElements`. `MGPSubDataComplete{MGPipeHandle Res; Uint16 Target, FirstLevel, LevelCount, Pad0; Uint64 PullSerial;}` / 24 B (`:621-627`) — texture-pull terminator, not a buffer call, but it shares the `kCtxObject` group.

`MGPipeHandle` itself is the 8-byte `{slot, gen}` POD in `MG_Pipe/MGPipeHandles.h`; `kMGPipeFirstAllocatableSlot = 1` (`:81`), `Gen` bumps only on slot reuse (`:44-48`), `kMGPipeDefaultFramebuffer` is `{0,1}` of kind Framebuffer (`:71`), and the top 1/16 of the `ShaderCso` slot space is reserved for program-pipeline composites (`:88-90`) — all cited from `ARCHITECTURE.md:35-40` and `BRIEF-P2.md:307ff`.

Payload record conventions that P3a's new/changed payloads must obey (`ARCHITECTURE.md:116-122`): every payload is a flat POD with explicit padding and a `static_assert` on trivial copyability **and exact size**, and **never contains a pointer**; var-tail records are fixed prefix + self-describing tail; `kHasBlob` records additionally validate that the `BlobRef` lands inside its declared segment; runtime violations are `Fatal{ProtocolCorruption}` (a `static_assert` cannot police a region the peer writes concurrently); the wire record header is `MGPWireRecHeader{Op:u16, Flags:u16, Size:u32}` (8 B, `Size` includes the header and is a multiple of 8); **the chunking upper bound is half the ring capacity** (`RingProducer::MaxRecordBytes()`), and payloads above it — explicitly "大 `ResourceSubData`" — are split by the emitter, while the ring outright refuses anything larger (nullptr + `MGLOG_E`).

### 3.4 Persistent maps (D-B4) and the ≥16 MiB adoption

- Untouched through the monolith work: `ARCHITECTURE.md:466` (quoted in §1.1 D-e). The tiers, the POST probe, `SEG_ADOPT`, `MOBILEGL_IPC_ADOPT_TIER` and the client-side push triplet are P5/P11 (`ARCHITECTURE.md:468-486`). P3a inherits none of it beyond routing `MapPersistent`/`UnmapPersistent` and publishing `map-persistent-roundtrips`.
- Espryt's three persistent-mapped rings and the `PersistentRing` algorithm are in the do-not-touch list (`ARCHITECTURE.md:315`); the transport reuses that algorithm verbatim for its own allocation/backpressure (`ARCHITECTURE.md:445`).
- The 20 `SyncPersistentMappedRange` + 6 `SyncGpuWrites` call sites have a **per-site** attribution table, not a blanket rule (`ARCHITECTURE.md:239-243`): client vertex-array range computation → nothing; max-index scan with an EBO source → `SyncPersistentMappedRange()` **and** `SyncGpuWrites()`; max-index scan from a client pointer → nothing; `*IndirectCount` count resolution → **only** `SyncPersistentMappedRange()`, because monolith does only that and adding the other would cost Create/Flywheel a publish-and-wait per batch; server-side restart rewrite / multi-draw flattening → the server reads the mirror. Those are P8's to move; P1 already produced the per-site ownership table (`ROADMAP.md:17`, `MEASUREMENTS.md:110`: 20 / 6, "与计划一致").
- Open question 15 (`ROADMAP.md:88`) is a hands-off note that touches P3a's neighbourhood: "**monolith 的 `*IndirectCount` 不调 `SyncGpuWrites()` 是不是潜在缺口**（compute 写的 indirect buffer）。独立 `dev` 问题，**拆分不得借机顺手修**."

### 3.5 The index host mirror (server side, P8)

`ARCHITECTURE.md:383-389` (§10.3, `MG_Remote/Server/IndexHostMirror`, P8):

- Coverage: resources with `BindMask & ELEMENT_ARRAY`, and only when `kCapNeedsHostIndexBytes` (split, and the server needs index bytes for restart rewriting / multi-draw flattening).
- "由 server 本来就要收的 `ResourceCreate/Respecify/SubData` 流增量维护：**零额外线上流量、零 round trip**." — i.e. the mirror is built entirely out of the three calls **P3a lands**. GPU writers' effect on the mirror is decided server-locally from the `OnGpuWritten` narrowed set.
- Budget `MOBILEGL_PIPE_INDEX_MIRROR_MB` (default 64), publishes `index-mirror-bytes` per frame; over budget, that buffer degrades to per-draw shipping through `MGHostSpan` (`Seg` → `SEG_STAGE`), counted as `index-bytes-shipped`.
- Why it must exist: `kMaxRestartRewriteBytes` = 64 MiB is twice the default `SEG_STAGE`, `kMaxFlattenedIndices` = 1<<24 is the same order; "它是本设计里唯一的"数据副本"".
- `MGHostSpan` (32 B, `MG_Pipe/MGPipeHostSpan.h`) is "整份接口里**唯一形状随传输而变**的东西"; `Seg == kMGHostSpanSegFromServerIndexMirror` (`MGPipeHostSpan.h:26`) means "字节已在你那边的索引镜像里"; it appears only in variable tails (`DrawVbo`'s user indices, `SetShaderBuffers`' named-UBO bytes) and never inline in a fixed payload, "VBO 路径（MC/Sodium 的全部 draw）不为它付字节" (`ARCHITECTURE.md:120`).
- Open question 16 (`ROADMAP.md:89`): the mirror's real memory footprint is unmeasured — element-array buffer totals across the MC/Sodium/Iris corpus are unknown; if materially over 64 MiB, the degraded path's frequency and cost must be measured.

### 3.6 The reverse channel a buffer needs

`ARCHITECTURE.md:267-284` (§8.1, `MobileGL/MG_Pipe/MGPipeCallbacks.h:27-51`, ten named callbacks + one forward terminator, replacing 95 call sites / 17 methods that poke frontend objects today). The two that a buffer needs:

- **`OnGpuWritten(res, ranges[])`** — replaces 6 `MarkGpuWritten` sites; "client 在每个 draw/dispatch 发射点**保守自建** pending 集，这是**收窄**通道" (i.e. the callback only ever narrows what the client already assumed). P9 narrows it further (`ROADMAP.md:26`: "`OnGpuWritten` 收窄").
- **`OnBufferWriteback(res, offset, bytes)`** — replaces PBO readback and XFB capture; "**按操作级批处理**（今天两处逐行循环绝不能变成每扫描线一次 IPC）；必须与 epoch bump 有序".

Ordering is a correctness requirement, `ARCHITECTURE.md:288` (§8.2), verbatim: "每一次 `WritebackFromBackend` 后面都紧跟 `BumpBufferMutationEpoch()`，否则 server 的 draw-clean memo 会在 epoch 背后变陈旧——split 里这变成反向通道上的排序规则：写回的 epoch bump 必须在任何后续读该 handle 的命令之前被 server 应用。**反向通道需要与正向通道相同的有序保证。**" (The epoch itself: `CurrentBufferMutationEpoch()`/`BumpBufferMutationEpoch()`, `MG_Backend/DirectGLES/Managers.cpp:1492-1499`; it is an `MGGen`, server-owned, and by §2.2 it **never crosses the line**.)

Other reverse-channel rows that bear on P3a's calls: `OnGlError(code)` must be ordered with the command stream (`glGetError` is always answered locally); `OnLog(level,text)` is lossy ≤WARN and lossless ≥ERROR with a rate limiter (`ARCHITECTURE.md:295`). The disposition of the other 95 writeback sites (`ARCHITECTURE.md:284`) removes exactly the things Track H replaces: "`SetBackendResource` 删除（server 拥有资源表）；`SetBackendStateMemo`（前端 VAO 里存后端堆裸指针）**直接删除**；`SetBackendHashMemo/AuxMemo` → server 侧 per-slot 字段."

### 3.7 The Track H handle scheme for buffers and VAOs

Handles (`ARCHITECTURE.md:35-40`, §2.1): 8-byte POD passed by value in a register pair; **client mints, the server never returns a handle** → zero creation round trips (deviation D1 from gallium). Slots are dense and **allocated per kind** (free list + high-water mark) so the server's object table is an array, not a hash map — "与 `IndexGenerator` 无关——后者的 LIFO 名字复用正是句柄要关掉的问题". `gen` increments **only on slot reuse, never on respecify**; a debug allocator asserts on wrap (2^32 reuses of one slot ≈ 50 days at 1000 fps per-frame reuse). Kinds include `Buffer` and `VertexElementsCso`. GL names appear only as `GlNameForDiag` and are never an identity, never a memo key, never part of a content hash; `GetLifetimeId()` stays on the client as the tracker's own identity and the client keeps a `lifetimeId → slot` map.

Two generations, strictly separated (`ARCHITECTURE.md:44-49`, §2.2): `MGPipeHandle::Gen` is client-owned, answers "is this still the same GL object?", **crosses**; `MGGen` (`g_bufferMutationEpoch`, `m_textureImageEpoch`, `m_cacheStructureEpoch`, 12 backend epochs in all) is server-owned, answers "did I re-mint my driver object?", and **never crosses** — the only server→client appearance is a texture pull request (§8.4). Norm: "任何 MGPipe 调用不得要求 client 提供或知晓 `MGGen`；反过来，client 的回绕 `Uint16` 版本计数器永远不是新鲜度的唯一证明——过线时要么加宽、要么与 `{slot, gen}` 同行."

Lifetime hooks (`ARCHITECTURE.md:204`, §5.6): `resource_create` at frontend-object construction, storage lazily defined by `resource_respecify`, `resource_destroy` at destruction. Three ordering constraints are expressed **by payload**, not by protocol: a view is destroyed before its storage owner (`ViewOf` + server keep-alive); an FBO attachment pins its texture (the surface handle implies keep-alive); a **buffer texture pins its buffer** (`BufferForTexBuffer`, range resolved live).

P2's D13 already built the machinery P3a extends (`BRIEF-P2.md:307-336`): `MobileGL/MG_Impl/Pipe/SlotAllocator.{h,cpp}` (per-kind free list + high-water, `lifetimeId → slot` map per kind, debug wrap assert) and `MobileGL/MG_Backend/DirectGLES/SlotTables.h` (per-kind dense twin arrays). Crucially for P3a, D13 records that **buffers are the one kind that already has a death signal**: "Today nothing tells the backend an object died except buffers (`BufferBackendOps::OnDestroy`, `BufferObject.h:103`, fired from `BufferObject.cpp:39-41`); `Managers.h:306-314` quantifies the resulting dead gigabytes. P2 adds the death notification for the six kinds by emitting `delete_*` from the frontend's `Mark*ForDeletion` path (`Core.cpp:116-140, 167-169, 279-307, 346-352, 1188-1190, 1213-1225, 1252-1254`) when the object's last `SharedPtr` drops — **not** when the name is marked, because a still-bound object keeps living (`TextureState.cpp:118-155`). The hook follows `BufferBackendOps`' shape." (`BRIEF-P2.md:335`.) So P3a inherits: the six registries are already slot tables, the VAO's backend raw pointers are already gone, and buffer destroy already has the canonical shape.

The 21-memo census P3a pays into (`ARCHITECTURE.md:363`, §9.5): the unifying fact is "每个进入 memo 键的版本计数器要么是回绕 `Uint16`，要么根本不会被它害怕的那个 mutation bump；身份比较是堵回绕洞的补丁." `{slot, gen}` + explicit destroy delete 11 memos outright (six registries' co-located `weak_ptr` + GC, `TwinLookupMemo` x3 + `OwnerEquals`, `UnitSamplerLookupMemo`'s `WeakPtr` test, `SetBackendStateMemo`, `VkTextureManager::TextureIdentity` liveness probe, **`ConvertedVertexStreamKey`'s `sourcePin`**), 2 are deleted server-side with the debounce moved to the client, 7 are re-keyed (including "`VertexInputStateFactory::ComputeHash` 里的 lifetimeId → `gen` **混进** server 侧每个 content hash"), and 1 (D18's container discipline) is untouched.

Note for P3a's VAO work specifically: Espryt's `ConvertedFloat64Stream` memo (`MG_Backend/DirectGLES/Managers.h:900-910`) is keyed on `sourceLifetimeId` + `sourceChangeSerial` + `sourceOffset`; `m_syncedAttributeVersions` and `m_hasSyncedConfigVersion`/`m_syncedConfigVersion` (`Managers.h:938-942`) are the VAO twin's shutters; `BufferImpl::g_bufferBackendIdGeneration` (`Managers.h:800`) is a **driver-object epoch, not identity, and survives untouched** (`BRIEF-P2.md:333`) — the same list marks `g_attachmentBackendIdGeneration` (`DirectGLES.cpp:1891`) and `InProcessTeardown()` the same way.

### 3.8 The purity gates

Stated at `ARCHITECTURE.md:499` (part 1 of the five-part gate) and again as the build-layout goal at `:552-566`:

- **A — include graph**: build `MG_Backend` in the disaggregated configuration with `MG_State/GLState` removed from the include search path. `nm --undefined-only` is blind to "included but never called", and `RenderState.h → FramebufferObject.h → TextureObject.h` is exactly that coupling. Depends on P0.5. Today gate A asserts on `MGPipeValueTypes.h`, not `MGPipeTypes.h`, because `DynamicBackendParameters` stayed in `BackendObject.h` (`ROADMAP.md:15`, `ARCHITECTURE.md:262`).
- **B — symbols**: `nm --undefined-only libMobileGLServer.so | grep -E 'MG_State::GLState::|glslang'` empty.
- **C — undeclared**: `grep -c 'pGLContext' MG_Backend/` == 0. P1 already reached 0 (`MEASUREMENTS.md:135`: "`MG_Backend` 里的 `pGLContext` | 0（唯一保留处是开关头文件的 pull 分支）"); the purity grep is on `pGLContext`, **not** `pGLContext->` (`ARCHITECTURE.md:350`).
- Plus the debug assertion that every backend memo key is `{slot, gen}` and never a raw frontend pointer, supported by `HandleRecycleScenario` (red before the re-key, on at least one backend).
- P13, not P3a, is when the three purity gates turn green on non-verify builds (`ARCHITECTURE.md:369`, `ROADMAP.md:30`); `MOBILEGL_PIPE_VERIFY` and the `SnapshotFromGLContext()` + `MG_State` include it needs are deliberately kept (D-B5, verify builds never ship).
- Related standing rule the P3a diff must not violate (`BRIEF-P2.md:334`): "Purity gate C: `grep -rc pGLContext MobileGL/MG_Backend` stays empty."

---

## 4. What P4a and P8 expect P3a to leave in place

### 4.1 P4a (`ROADMAP.md:20`)

Deliverables cell, verbatim: "`set_framebuffer_state`（解析后的 `ReadSurface`、内联格式、`ContentHash`、`{0,1}`）；sampler CSO（含 `borderColorForm`）；sampler view + `set_texture_params`；`set_sampler_views`/`bind_sampler_states`/`set_shader_images`；shader CSO（SPIR-V + 归档）；`set_draw/dispatch_program`；`set_global_constants`；`CompositeResolver`；**纹理/renderbuffer 的 `resource_*`**。emulation 在 split 下显式 Fatal 直到 P8". Gate cell: "全套门；framebuffer/纹理/program 族场景；**新增"只作 attachment / image 单元 / CopyImage 端点的纹理其 `glTexParameter` 生效"场景（落地前必须红）**；两台设备 `KHR-GL46.direct_state_access.framebuffers*` 与整个 `packed_pixels` 块（~3300 例，句柄复用压力测试）。**再基线检查点 1b：超过 39 天**". Dependency cell: "P3a".

Therefore P4a expects P3a to leave behind, ready to reuse without redesign:

1. **The `resource_*` family itself, generalised.** P4a's textures and renderbuffers ride the *same* `ResourceCreate`/`ResourceRespecify`/`ResourceDestroy`/`ResourceSubData` calls and the *same* `MGPResourceDesc` (one discriminated shape for "buffer / 全部纹理 target / renderbuffer", `ARCHITECTURE.md:127`). P3a must not shape those calls or the descriptor around buffers only — the texture fields (`Levels`, `Samples`, `ArrayLayers`, `ImageBindableHint`, `ViewOf`, `HasDefinedContent`, `FixedSampleLocations`) are already in the 88-byte struct and their meaning must survive.
2. **The `{slot, gen}` scheme and the slot allocator scaled to a second wave.** P4a is explicitly the "句柄复用压力测试" (the whole `packed_pixels` block, ~3300 cases), so P3a's allocator, its wrap assert, its `lifetimeId → slot` maps and its explicit-destroy path have to hold under heavy recycling.
3. **`MOBILEGL_PIPE_LEGACY_MEMOS` alive as a compile-time arm** — `ARCHITECTURE.md:367` says it is retained "在 P3a/P4a 期间" and retires only with the pull path at P13 (+1 day of maintenance per phase). P3a must add its buffer/VAO legacy arm in that shape, and P4a will add more under the same switch.
4. **The `kNeedsAck` mechanism, first used on `glBufferStorage`** (`ARCHITECTURE.md:293`), because P4a's texture/renderbuffer allocations explicitly do **not** ack and rely on that asymmetry being expressible.
5. **A working `HandleRecycleScenario` + the ABA control arm** (P2's D18, `BRIEF-P2.md:423-437`): the three always-on ctest arms (`.Handles` green, `.Legacy` green, `.AbaControl` green-by-asserting-the-corruption via `MOBILEGL_PIPE_HANDLE_ABA_CONTROL` / `Features.PipeHandleAbaControl`). P3a's VAO/buffer re-keys extend that scenario's coverage; P4a re-uses it for textures and framebuffers (the P2 scenario already repeats its shape "for a texture (against `g_backendTextureObjects`) and a framebuffer").
6. **The full `gl44to46` caselist run at the P3a boundary** (`ROADMAP.md:34`), which is P4a's conformance starting point (P4a itself only owes the named `packed_pixels` block plus `direct_state_access.framebuffers*`).
7. **Per-dirty-bit fire rates in `MEASUREMENTS.md`** — P2 produced them "so P3a starts from data" (`BRIEF-P2.md:956`); P3a owes the same for the bits it turns from counted into emitted, so P4a's object-class bits (11–14) start from data too.

### 4.2 P8 (`ROADMAP.md:25`)

Deliverables: "`MG_Impl/Pipe/HostResolve.cpp`（client 数组范围、最大索引扫描、`*IndirectCount` 解析，各带逐站点 reconcile）；`MGHostSpan` split 填法；**`Server/IndexHostMirror`**；CopyImage 镜像搬到 client；`draw_vbo` 收编 multi-draw 族（分档仍在 server）；viewport-array 回放验证；`generate_mipmap` 计划 + CPU 回退纹素；G3 分块路径；无 present fence tick + 无 present split 用例；`kCapDriverOrderedXfbCapture`". Gate: name-for-name `'^DirectGLES\.Split\.'` vs `'^DirectGLES\.'` (same for DirectVulkan); 40 traces split, both backends, SSIM ≥ 0.99 **including the two `coherent_as_flush` Create fixtures**; `ClientArrayAfterComputeWriteScenario` green (removing the wait must show missing geometry); `roundtrips-per-frame` reads zero on `create-indirect`; `index-mirror-bytes`/`index-bytes-shipped` published per case. Day 145 = full-feature split.

P8 expects P3a to leave:

1. **`BindMask & ELEMENT_ARRAY` correctly set on every index buffer**, on `ResourceCreate` **and** `ResourceRespecify`, because that bit is the mirror's coverage switch (`MGPipeTypes.h:153-156`, `ARCHITECTURE.md:385`). Getting it wrong silently disables restart rewriting / multi-draw flattening in split.
2. **A `ResourceCreate/Respecify/SubData` stream that is complete and incremental enough to rebuild buffer contents from nothing** — the mirror is maintained "由 server 本来就要收的 … 流增量维护：零额外线上流量、零 round trip" (`ARCHITECTURE.md:385`). Any buffer mutation path P3a leaves outside those three calls becomes a silent mirror hole.
3. **The `MGPSubData` buffer-half convention and its emitter-side splitting** (`MGPipeTypes.h:574-618`), because P8 adds the G3 chunked path on top of it and because the mirror consumes exactly those records.
4. **`OnGpuWritten` as a narrowing channel with a conservative client-side pending set** (`ARCHITECTURE.md:271`), because the mirror decides GPU-writer visibility server-locally from that narrowed set (`ARCHITECTURE.md:386`).
5. **`map_persistent` untouched**, plus the `hasLiveHostWrites` question answerable per resource — P5's client-side persistent-map push adds "一个 `hasLiveHostWrites` 位" to the `ResourceRespecify/SubData` payload with "零新增记录种类" (`ARCHITECTURE.md:479`). P3a's payload shape must leave room for that rather than forcing a new record kind.
6. **`MGPDrawInfo`'s index fields intact** — `IndexResource`, `IndexSize`, `RestartIndex`, `MinIndex/MaxIndex` under `kDrawHasIndexRange`, `kDrawIndicesAreClient`, the `MGHostSpan` in the variable tail only under `kDrawHasUserIndices` (`MGPipeTypes.h:696-729`) — because P8's `draw_vbo` multi-draw absorption and host-resolve work all key on them, and because "the client resolves the COUNT itself, so the server never reads an indirect command block" (`MGPDrawIndirect` comment, `:737-745`).
7. **The `create-indirect` fixture usable as an acceptance case**, i.e. the `dev`-side failure fixed first (`MEASUREMENTS.md:96`, `ROADMAP.md:90`); P8 asserts `roundtrips-per-frame == 0` on precisely that fixture.
8. **Emulation ownership unchanged**: restart rewriting and multi-draw tiering stay server-side with zero wire traffic; the client supplies index bytes only when caps ask (`ARCHITECTURE.md:229-237`, and the one-line rule at `:112`: "**multi-draw 分档与 restart 重写永远由 server 拥有；client 在 caps 说需要时提供索引字节**"). P3a must not move either into the client as a convenience.

---

## 5. Commit / CI discipline, and the 2026-09-08 rule

### 5.1 Per-commit discipline, verbatim

`ROADMAP.md:7` (the "通用纪律（每个 commit）" section), verbatim: "默认 ALL target 必须完整构建；禁止提交热路径插桩（CI grep 门）；**每个门必须能因它存在的理由变红**；Windows 机器不是正确性门；设备对比走 reboot-clean + 同热窗口配对 A/B，CPU 定频按项目协议；每阶段出口跑一次五部分门；每阶段性能判据是**逐线程 CPU 时间**."

Unpacked into obligations P3a inherits:
- the default ALL target builds;
- no hot-path instrumentation is committed — enforced by a CI grep gate; the P2 restatement is "**The tracker gets no timer.** The absolute-ns number comes from `DriverBench`'s `ns_per_op` … which times whole frames from outside the library" (`BRIEF-P2.md:410-412`), and every counting site keeps the `if (PipeStats::Enabled())` shape so an off build pays one predicted branch (`BRIEF-P2.md:421`, `PipeStats.h:29`);
- **every gate must be able to go red for the reason it exists** — this is why each phase ships negative controls (P2's `g7_negative_control.sh`, the `AbaControl` arm, the deliberately corrupted verify field, the pulled fill stamp);
- Windows is not a correctness gate;
- device comparisons are reboot-clean, same thermal window, paired A/B, CPU pinned per the project protocol;
- the five-part gate runs once at each phase exit;
- the per-phase performance criterion is per-thread CPU time.

### 5.2 CI lanes and generated-file discipline

- `pipe-gates` (`.github/workflows/test.yml:809`, P0 landed, `ARCHITECTURE.md:568`): regenerate with `gen_pipe.py` and `git diff --exit-code`; the stdio-instrumentation grep gate over `MG_Backend`/`MG_State`; `gen_pipe_dirty_surface.py --summary` (informational at P0, a gate from P1/P2 — P2's D.5 corrects the doc to say P2); `check_doc_citations.py` (warning level, `--strict` once the docs are final). Separate job `flatc-check`. Follow-ups listed: `include-graph-check` (P0.5) and `monolith-symbol-report`.
- Generated files are committed and CI diffs them (`BRIEF-P2.md:461`, `test.yml:1492-1495`). Codegen never enters the default build graph (`ARCHITECTURE.md:424`).
- **`PipeCalls.def` numbering never churns**: the wire opcode is the 1-based position in the file, new calls may only be **appended**, retired calls keep their slot, and `MGP_CALL_LIST_DOCUMENTED_COUNT = 71` is pinned by `MG_Test/Pipe/PipeCatalogueTest.cpp` (`PipeCalls.def:18-24, 69`; `ARCHITECTURE.md:75`; `BRIEF-P2.md:460`). Every call P3a emits already has an opcode, so P3a should not need to edit the catalogue — but **any payload whose size changes (e.g. `MGPVertexBuffers` gaining `baseInstance`) changes `MGP_ASSERT_POD` and the G3 wire `static_assert`, which is a deliberate, reviewed break, not a silent one.**
- Commit messages (`BRIEF-P2.md:459`): single-line `[Type] (Scope): description`. **Never** add `Co-Authored-By` or any other attribution line.
- Logging (`BRIEF-P2.md:462`): `MGLOG_D` for anything non-critical, `MGLOG_I` only where already justified; no stdio anywhere under `MG_Backend`/`MG_State` (the `pipe-gates` grep, `test.yml:1515-1522`).
- Namespacing/layering (`BRIEF-P2.md:456-458`): no new namespaces, everything is `MobileGL::MG_Pipe`; run `python3 scripts/check_include_closure.py` after adding any file to the shared `MG_Pipe` directory; if the closure gate objects, the file moves to `MG_Impl/Pipe/` (the client side is unrestricted).
- Docs at phase exit (`BRIEF-P2.md:930-946`, D.5): `scripts/check_doc_citations.py docs/Disaggregated/*.md` must stay clean — every `file:line` a doc edit adds has to be true. The phase updates `README.md:3` (status + hash), its own `ROADMAP.md` row (tick + the real numbers), any open questions it answers, the `ARCHITECTURE.md` lines its work makes stale, and adds a numbered `MEASUREMENTS.md` section with the phase's tables. Then `[Merge]` to `dev` per the milestone flow, and the full `gl44to46` caselist is scheduled on both devices **off the critical path**.
- Local-verify convention that P1/P2 both used and P3a should expect to reuse (`BRIEF-P2.md:965`): the exit evidence is the local WSL run (`~/w7/pipe`), not CI; CI green is required before the `dev` merge, not before the phase verdict.

### 5.3 Durable locations and integrator decisions carried forward from P2

`INTEGRATOR-DECISIONS.md` (`~/w7/notes/p2/INTEGRATOR-DECISIONS.md`, dated 2026-09-07), five entries; the ones with life beyond P2:

- **ID-4, command errata** (apply when running the gates): ctest name extraction must be `grep -E "^[[:space:]]*Test[[:space:]]+#[0-9]+:" | sed -E "s/^ *Test +#[0-9]+: //" | LC_ALL=C sort` on both sides — "the brief's `^\s+Test #` drops every id under 1000"; `retrace_gate.py` has **no** `--ssim` / `--backend` flags, the SSIM threshold is the per-case one in `trace_cases.json`; the verify arming line is `MGPipe: verify armed - 63 fields, 69 verbs, fatal=1` and retrace case logs carry `MGPipe verify: <case> <backend> armed, zero divergences, zero unmigrated reads`.
- **ID-5, durable locations** ("the Windows scratchpad was wiped once already"): brief at `~/w7/notes/<phase>/BRIEF-*.md`; scouts at `~/w7/notes/<phase>/scout-*.md`; results and reviews at `~/w7/notes/<phase>/<phase>-results/`; helper scripts at `~/w7/notes/tools/` (today: `p2_ab.sh`, `wsl_integrate_p2.sh`, `wsl_p2_bench.sh`, `wsl_p2_gate.sh`, `wsl_p2_tree.sh`). Write through a WSL script or the `//wsl.localhost/Arch/home/swung/w7/...` UNC path, not only to the scratchpad. Delete your own intermediate logs when a round ends; keep only the logs your result file cites.
- **ID-1 / ID-2 / ID-3** are P2-specific (the ownership grant for the six destructor notifications, the integration order `contract -> spans -> espryt -> tracker -> magma -> gates`, and the G1 admitted resize set) but they set the precedent P3a's integrator should copy: an ownership deviation is a **recorded waiver**, integration order is chosen so the package carrying shared-file edits lands first and the others rebase, and any symbol resize outside a named, admitted set must be attributed per symbol.

For reference, P2's decoded gate set (`BRIEF-P2.md:33-58`) is the template a P3a brief should mirror: **G1** pull-build symbol identity via `scripts/symbol_report.py --threshold 0` (0 added / removed / renamed, resizes only from a named admitted set); **G2** `ctest -L integration-gpu` name-for-name identical between pull and push builds, green in both, on both backends; **G3** the 40-trace corpus under push at SSIM ≥ 0.99 (79 desktop cases = 40 x 2 minus `iterationrp x DirectGLES`) via `~/w7/retrace_gate.py`; **G4** the verify build with no `Fatal{PipeVerifyDiffer`, `Fatal{UnmigratedPipeInput`, `Fatal{PipeResidualDiverged` and every case armed; **G5** a named untouched region proven by `sha256sum` of an `awk`-extracted namespace across two refs; **G6/G7** a consistency test plus its scripted negative control (patch, build, expect non-zero ctest rc, revert); **G8** the always-on three-arm recycle scenario; **G9** `gen_pipe_dirty_surface.py --check` and `--self-test`; **G10** a `static_assert` ratchet plus a device counter reading non-zero; **G11** the two-device paired A/B; **G12** the microbenchmark plus a ctest that proves the negative-control switch actually changes behaviour; **G13** purity (no `pGLContext` under `MG_Backend/`, include closure, no stdio, generators regenerate clean); **G14** no test name ever removed, full CI matrix green.

### 5.4 The user's 2026-09-08 rule: speed over perf

**As given to me by the orchestrator** (it is not written in the docs I opened — `docs/Disaggregated/*` and the P2 notes contain no such clause, so this must be transcribed into the P3a brief and into `MEASUREMENTS.md`, and `ROADMAP.md:19`'s gate cell read in its light):

> Speed over perf: **performance is recorded, not gated**. The pull baseline is in `MEASUREMENTS.md`. **Correctness gates stay.**

Concretely, for P3a:

- Part 4 of the five-part gate (`ARCHITECTURE.md:504`: two devices, reboot-clean, same thermal window, paired A/B, per-thread CPU p50/p99, the tracker's absolute ns/draw ceiling, the blend-toggle microbenchmark, the CSO-content-addressing negative control) becomes a **measure-and-publish** obligation rather than a pass/fail gate. The P2-equivalent statement (G11, `BRIEF-P2.md`) "per-thread CPU p50 and p99 deltas are **not negative** on either device" does **not** block P3a.
- Likewise "MC 26.3 在 Adreno 上 p99 不变" (`ROADMAP.md:19`) is recorded against the baseline, not enforced as a stop condition.
- Everything in parts 1, 2, 3 and 5 of the gate stays a hard gate: purity A/B/C, `MOBILEGL_PIPE_VERIFY=1` zero divergence, name-for-name integration equality across `{pull, push}` on both backends, 40-trace SSIM ≥ 0.99, CTS within 0.5 pp, coverage/poison/handle discipline, and every negative control able to go red.
- The **pull baseline to record against** is already in `MEASUREMENTS.md`: §3 the two-device / two-backend / four-trace boundary-counter table (`MEASUREMENTS.md:53-62`) — steady-state dynamic accessor cost is **6.5–9.3 per draw** (`:66`), which is what push must beat and from which the tracker's ns ceiling is derived; the six memo gates' hit/miss pairs; `stage-*` byte classes including the `stage-ubo-named` 331 KB/frame asymmetry (`:68`) and the union-box vs region-list 635 K vs 40 K, 16x (`:70`). §4 desktop points (`:74-88`), including the payload `static_assert` sizes and the **persistent-map adoption baseline p99 163→21 ms / 40→115 fps / ~400 MB** (`:87`) and the Mali +6 ms/frame upload-job cliff (`:88`). §8 (`:129-142`) is P1's acceptance table and the shape a P3a table should copy.
- The reporting caveat that must travel with any `acc/draw` number (`BRIEF-P2.md:417-419`, `D17`): `CallClass::AccessorCalls` is a set of **static tallies at ~10 hot entry points** (`PipeStats.cpp:76-88`), so a tracker that merely *relocates* reads scores a lower `acc/draw` without removing work. Lead with the **gate hit/miss pairs** and the **CPU-time series**, and quote `acc/draw` only alongside a re-audit of the tally constants (`DirectGLES.cpp:1421,1524,2043,2053,2977`; `VulkanRenderer.cpp:5009,5016,5274,5927,5934,6416,6449,6462`). Write the caveat **above** the numbers, not below.
- Counters available without new instrumentation (`ARCHITECTURE.md:599-601`, `MG_Util/Metrics/PipeStats.h:46-122`): byte classes `stage-buffer`, `stage-texture`, `stage-ubo-global`, `stage-ubo-named`, `stage-vertex-client`, `stage-index-client`, `stage-indirect-cmd`, `persistent-map-push`, `residual-value-block`; call classes `draws`, `accessor-calls`, `texture-upload-emissions/box/rect/jobs` (P2 added `RenderStateCsoMints`/`RenderStateCsoBinds`, short names `csom`/`csob`, `BRIEF-P2.md:414`); six memo gates each with hit/miss; a 24-bucket per-draw payload histogram (`RecordDrawPayloadBytes`, `PipeStats.h:129,154`) that the tracker became the first emitter of in P2. Summary line every `MOBILEGL_PIPE_STATS_PERIOD` frames (default 120); the list of paths that are **not** wired is at `PipeStats.cpp:16-100` and "那份清单是契约".

---

## 6. Traps and standing facts an implementer will otherwise re-discover

1. **The trace harness never reaches `MobileGL::DestroyImpl`**, so `MOBILEGL_PIPE_STATS_FILE`'s JSON dump is never written on device — only the periodic summary lines in `mobilegl.log`; a trace shorter than one period produces nothing (`MEASUREMENTS.md:92`). Set `MOBILEGL_PIPE_STATS_PERIOD` small enough.
2. **Two devices must run serially from one tree**: `run_android_retrace_local.py` shares one `.trace-work/android-retrace-result` root per tree and `rmtree`s it on every invocation (`MEASUREMENTS.md:93`).
3. **MSYS path conversion** mangles `/data/...` inside `--env` values; run with `MSYS_NO_PATHCONV=1` (`MEASUREMENTS.md:94`).
4. `coherent_as_flush` is plumbed independently of `--env`: `--ez coherent_as_flush true` → `trace_replay_core.cpp`'s `setenv` (`MEASUREMENTS.md:95`).
5. Repro command for the counter baseline (`MEASUREMENTS.md:72-78`): `ANDROID_SERIAL=<serial> MSYS_NO_PATHCONV=1 python3 tools/trace_replay/run_android_retrace_local.py --case <case> --backend DirectGLES --env MOBILEGL_PIPE_STATS=1 --env MOBILEGL_PIPE_STATS_PERIOD=120`, then `grep 'MGPipe stats:'` in the result directory's `mobilegl.log` and take the last complete window.
6. **Devices**: `35d0befa` = Xiaomi 24129PN74C, Adreno 830, Android 16; `3B159D009VZ00000` = Oppo PLG110, Mali, Android 16 (ColorOS). Device-lock protocol as usual (`MEASUREMENTS.md:3`). ColorOS install trap: a first `adb install` of an unknown package stalls on `com.oplus.appdetail InstallGuideActivity` until "继续安装" is tapped (`MEASUREMENTS.md:30`).
7. **Test-wiring traps** (`ARCHITECTURE.md:566`): ctest `ENVIRONMENT` **replaces** rather than appends, `;` must be escaped, and the property overrides job env — use `mgl_itest_join_environment(... ${MGL_ITEST_COMMON_ENV})`; `add_trace_replay_test` needs a `SPLIT` suffix (otherwise it collides with the same case+backend) plus `-DTRACE_TRANSPORT=`.
8. **A test that calls a backend helper directly aborts on the per-verb poison** unless it declares its verb; `MG_Test/ScopedPipeVerb.h` exists for exactly that and eleven tests already use it (`ef6227e1`, `BRIEF-P2.md:971`). The poison is never weakened and no test name changes.
9. **P1 left 8 over-approximated fill rows** — statically reachable but never exercised by the harness, and an over-approximation permanently disables that (class, field) poison; "**P2 收紧填充表时先复查这 8 行**" (`MEASUREMENTS.md:119`). If P2 did not close them, P3a inherits the item.
10. **push-on-mutation** is the shape P1 chose for backend-writes-frontend inside a verb (`MEASUREMENTS.md:121-128`): `MGP_NOTE_MUTATION(Field)` (`MG_Pipe/PipeMutation.h`) refreshes one field in the pushed block when a frontend counter moves, keeping "the pushed block equals the live context at every read" literally true. The three fields and their hooks: `GetSamplingResolutionGeneration` / `TextureState::BumpSamplingResolutionGeneration()`; `GetTextureBindGeneration` / `BumpTextureBindGeneration()` and `NoteUnitTouched()`'s `bindingChanged` branch; `GetMaxTouchedTextureUnit` / `NoteUnitTouched()`'s high-water branch. Note the explicit statement that **`BufferObject`/`ProgramObject`/`VertexArrayObject` backend writes move no pushed field — "它们是句柄类读点"** (`MEASUREMENTS.md:128`), which is precisely why P3a's kinds are the handle wave.
11. Espryt's `SyncTextureObjectToBackend`'s by-value copy and second `Find` (`DirectGLES.cpp:1310-1345`) were deleted in P2's D13 "in the same change, not left as harmless"; `g_fbSlotCache`/`GetFramebufferBindingSlotFast` (`DirectGLES.cpp:134-157`, five callers at `:1613, 1907, 2745, 2860, 2935`) was replaced by a plain `MGB_CTX->GetFramebufferBindingSlot(target)` read, which **closed a P1 poison bypass** — the fast getter handed out a raw pointer forever, so the per-verb poison stamp and the verify read hook were skipped at those five sites (`BRIEF-P2.md:318`). P3a should assume no equivalent fast-path getter survives in the buffer/VAO paths.
12. Espryt-side things P3a must keep armed while moving the registries/pools (P2's D13 must-not-break list, `BRIEF-P2.md:325-334`): `EnsureProcessTeardownSentinel()` arming (`Managers.h:59-60`, `Managers.cpp:161-170`) moves to the slot table's first insertion — a registry destructor hook is wrong and `Managers.h:52-57` says why; the twin destructors' `g_backendContextGeneration` compare (`Managers.h:64-70`, pinned by `SanityTest.cpp:2650-2665, 2765-2773, 2786-2790`) so a twin outliving its ES context does not `glDelete*` a recycled name; the whole-registry save/reset/restore fixture `ScopedDirectGLESTextureBindings` (`SanityTest.cpp:145-197`); the one direct-iteration site `ScopedDetachedTextureFramebufferAttachments` (`DirectGLES.cpp:6413-6425`) needs a per-kind "which slots are live" bit.
13. `MOBILEGL_PIPE_*` runtime switches that exist today (`ARCHITECTURE.md:586-596`, `Config.h:319-358`, `ConfigLoader.cpp:245-256`): `MOBILEGL_PIPE_PUSH` (0, a subsystem bitmap including one bit that disables CSO content addressing), `MOBILEGL_PIPE_VERIFY` (0), `MOBILEGL_PIPE_STATS` (0), `MOBILEGL_PIPE_LEGACY_MEMOS` (ON, tri-state, only an explicit falsy turns it off), `MOBILEGL_PIPE_TEXEL_RETAIN_MB` (0), `MOBILEGL_PIPE_INDEX_MIRROR_MB` (64), `MOBILEGL_PIPE_STATS_PERIOD` (120), `MOBILEGL_PIPE_STATS_FILE` (empty). Existing negative-control switches are all retained: `MOBILEGL_ESPRYT_DISABLE_{UBO,UNPACK,UPLOAD}_RING`, `_INVALIDATE_FLUSH`, `MOBILEGL_DISABLE_LARGE_BUFFER_ADOPTION`, `MOBILEGL_COHERENT_AS_FLUSH`.
14. The two lanes are independent and P3a is on the monolith lane; the monolith argument is per-thread CPU numbers, **not** deleted lines — "monolith 净代码量是**增加**的（约 +6,650 手写 + 4,000 生成，对 ~372 行真删除）" (`ARCHITECTURE.md:381`). The monolith-side wins P3a's kind of work buys, listed at `:381`: an entire class of address-reuse ABA becomes inexpressible; the FBO→program ordering hazard disappears; the `SwapchainObject` layering inversion disappears; two latent bugs already fixed; one glslang compile leaves the startup path; `inproc` becomes the render thread; the `MG_Test` mock backend becomes the MGPipe recorder.
