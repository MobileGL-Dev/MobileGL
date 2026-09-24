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
