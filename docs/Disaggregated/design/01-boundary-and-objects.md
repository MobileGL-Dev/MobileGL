# 边界与对象模型（原 ARCHITECTURE §1–§2）

> 设计细节。设计要点与全部章节的索引见 [`../ARCHITECTURE.md`](../ARCHITECTURE.md)。章节编号沿用原 `ARCHITECTURE.md`，代码注释里的 `ARCHITECTURE.md §N` 按编号在这里找到。

## 1. 边界

### 1.1 一句话

`MG_Backend` 本来就是一台贴着目标 API 的状态机（Espryt = DirectGLES，Magma = DirectVulkan），缺的是一份"我被告知了什么"的显式声明。MGPipe 就是这份声明：前端在每条 verb 之前把变化**推**过去，后端不再拉 `MG_State::pGLContext`。接口从两个后端自己维护的关键结构反推（`SetupDrawSnapshot`、`DrawTextureSyncKeys`、`ResolvedVertexBindings`、`g_syncedRenderStateParameters`、`BufferBackendOps` …）；gallium 是词汇的目的地，不是推导前提。

### 1.2 两张函数指针表

`MGPipeScreen`（share-group 作用域：caps、resource、persistent map、fence）与 `MGPipeContext`（其余全部），由 `PipeCalls.def` 生成。

- 函数指针 struct 而非虚基类：null 项已表示"未实现，前端回退"，`MG_Test` 整表替换即可 mock。
- 两张表从第一天分开，事后拆会给记录重新编号；v1 只有一个 screen、一个 context、一条 flow。
- EGL 生命周期与 caps 面留在 `pActiveBackendObject` 的虚函数上（罕见路径，走控制面）。

### 1.3 三种形态，一份后端

| 形态 | 表里装的是什么 | 用途 |
|---|---|---|
| `monolith`（默认） | 后端自己的函数，回调直调 `MG_State` | 出货 |
| `inproc` | 发射器 → 同进程第二个线程上的 applier | CI 主验收；也就是 monolith 的**渲染线程** |
| `spawn` | 发射器 → 数据面 → 另一个进程的 applier → 同一批后端函数 | 两进程形态；控制面可以是 `fork` / `unix:` / `tcp://`（§11.9），server 可在另一台机器上 |

唯一 hook 点是 `MG_Backend::Init()` 里一个 `#if MOBILEGL_BUILD_DISAGGREGATED` 分支：`MG_Config::Transport != Monolith` 时装 `MG_Remote::BackendObject_Remote`。`MG_Impl` 的边界调用点零 `#ifdef`。

## 2. 对象模型

### 2.1 句柄 = `{slot, gen}`（`MG_Pipe/MGPipeHandles.h`）

- 8 字节 POD；**client 铸造，server 永不返回句柄** → 整份目录零创建 round trip（对 gallium 的偏离 D1）。
- slot 按 kind 稠密分配（free list + 高水位），server 对象表是数组；`gen` 只在 slot 复用时 ++。
- kind：`Buffer, Texture, Renderbuffer, Framebuffer, Xfb, RenderStateCso, VertexElementsCso, SamplerCso, SamplerViewCso, ShaderCso, Fence, Query, Context`。保留：`{0,0}` = null；`Framebuffer {0,1}` = 默认帧缓冲；`ShaderCso` 高段给 program pipeline 合成体。
- GL name 只作诊断（`GlNameForDiag`），永不做身份、memo 键或 content hash。

### 2.2 两种世代，严格分开

| | 拥有者 | 回答 | 过线 |
|---|---|---|---|
| `MGPipeHandle::Gen` | client | "还是同一个 GL 对象吗？" | 是 |
| `MGGen`（后端纪元，如 `g_bufferMutationEpoch`） | server | "我自己是否重铸了驱动对象？" | **永不**（只以纹理拉取请求的形式出现，§8.4） |

client 的回绕 `Uint16` 版本计数器永远不是新鲜度的唯一证明：过线时要么加宽，要么与 `{slot, gen}` 同行。

### 2.3 CSO 与可变对象

- CSO（render state、vertex elements、sampler、sampler view、shader）在 **client 侧内容寻址**（Mesa `cso_cache` 先例），LRU 淘汰时发 `delete_*`；render-state 容量 64（P13 重调）、sampler state 256。
- Buffer / Texture / Renderbuffer 走 create / respecify / subdata / destroy；Framebuffer / Xfb 是 per-context 身份 + `set_*` payload。
- 两条偏离：**D-G1** Espryt 的 vertex-elements 按 VAO 身份寻址（twin 持驱动 VAO 名，不能共享）；**D-F2** Espryt 的 sampler view 按纹理身份寻址，sampler state 内容寻址且带引用计数（被 `BuiltinSampler` 指着的项不许被 LRU 挤掉）。

#### 2.3.1 sampler state 缓存：引用计数与值生命周期

sampler state 在 client 侧按值寻址（`MGPipeSamplerCsoCache`，容量 256）：哈希、探测、再 memcmp 确认；只凭 64 位哈希相等会把两种状态并进一个 CSO，造成没有门看得见的错误过滤。
- 活句柄躲开 LRU 靠引用计数而不是容量：已发布的 `MGPTextureParams` 在纹理参数不动时一直指着它的 `BuiltinSampler`，applier 不解析这个句柄，而淘汰不算参数变化，既不会重发也没人拒绝。所以每次 `Acquire` 取一个引用、`Release` 还一个；LRU 只考虑引用为 0 的项；全被钉住时超容量铸造并计入 `OverCapacityMints`（多一个槽费内存，句柄被抽走费正确性）。
- 持有引用的是纹理的内建 sampler（纹理死亡或参数变化重取时释放）和 `bind_sampler_states` 当前绑定的每个 CSO；凡在返回后还会指名该句柄的调用方都必须持有。容量大于 `kMGPipeMaxTextureUnits` 只是尺寸界：装不下一整轮发射的缓存会每轮都超容量铸造。
- 槽不带 lifetime id：CSO 属于值而不属于前端对象（别的 `SamplerObject` 可能持有同值），`~SamplerObject` 不释放任何槽，唯一死亡路径是 client 侧的 LRU 淘汰。因此来自 `~SamplerObject` 的 `NotifyStateObjectDestroyed(SamplerCso, lifetimeId)` 背后永远没有句柄；后端不得按 `SamplerObject` 的 lifetime id 键控 sampler twin，twin 的生命只是 `create_sampler_state` 到 LRU 发出的 `delete_sampler_state`。
