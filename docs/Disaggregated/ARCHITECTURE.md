# MGPipe 设计与架构

> 只写**已决定**的设计，每条附一句理由。章节编号稳定（代码注释按 `ARCHITECTURE.md §N` 引用它们）。阶段号与状态见 [`ROADMAP.md`](ROADMAP.md)，实测见 [`MEASUREMENTS.md`](MEASUREMENTS.md)；各阶段的落地细节、偏差与当时的论证在 `notes/<阶段>/README.md`（§17 是索引）。wire 契约原文在 `MobileGL/MG_Remote/CONTRACT-*.md`，与本文冲突时以更新的契约为准。

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

## 3. 调用目录

### 3.1 单一真相源

`MobileGL/MG_Pipe/PipeCalls.def`：一行一个 `X(Name, Payload, Class, Flags, WaitClass)`。**线上 opcode 就是行序**，只能追加、退役保留槽位；行数由 `MGP_CALL_LIST_DOCUMENTED_COUNT`（今天 81）与 `PipeCatalogueTest` 钉住。生成器产物提交进树，CI `pipe-gates` 重生成并 `git diff --exit-code`：

| | 产物 | 内容 |
|---|---|---|
| G1 | `PipeTables.inc` | 两张函数指针表 |
| G2 | `PipeThunks.inc` | monolith 直调 thunk |
| G3 | `PipeWire.inc` | wire 记录、尺寸 `static_assert`、applier 边界检查 |
| G4 | `PipeVerify.inc` | `MOBILEGL_PIPE_VERIFY` 逐字段比对器（`PipeFields.def`） |
| G5 | `PipeFilled.inc` | `PipeInputs` 字段 id 与逐 verb 世代 poison |
| G6 | `PipeCoverage.inc` | 后端读点 → 调用映射（`Coverage.def`），0 UNMAPPED 是门 |
| G7 | `PipeSpanTable.inc` | render-state pipeline 子集成员表 |
| G8 | `PipeFieldOwnership.inc` | 每个 `PipeInputs` 字段恰属 `RECORD-SUPPLIED / APPLIER-DERIVED / BARRIER-PULLED / FATAL` 之一（P5f 起 0 BARRIER-PULLED） |

另有 `FillPoints.def`（填充点）与 `DirtySurface.def`（§5.2）。G1 codegen 规则：capability gate 替换指针表达式时保留**表达式形状**，否则 pull `.text` 漂移。

### 3.2 分组与 flag

- Class：`kScreen`（caps、resource、persistent map、fence、`ApplierReset`）、`kCtxQuery`、`kCtxCso`（create/bind/delete × 五种 CSO）、`kCtxState`（`Set*` 状态、`SetContextValues`、`SetProgramBindings`、迁移期的 `SetResidualValueState`）、`kCtxObject`（subdata、readback、mip、`ObjectDeath` …）、`kCtxVerb`（blit、clear、`DrawVbo`、dispatch、XFB、`Present` …）。
- Flags：`kNeedsAck`、`kHasBlob`、`kVarTail`、`kHostSpan`、`kReplySlot`、`kOptional`。第五列 `WaitClass`（P5e）决定 run-ahead 下 client 等不等（§11.6）。
- 20 个 draw 入口塌成 `DrawVbo` 一条；`Clear` 一条判别式；sampler 集合**没有 stage 维度**（192 单元合并空间）；`SetTextureParams` 按资源寻址、与 sampler view 分开（D10）；`SetIndexBuffer` 独立于 VAO 配置（D5）。
- 显式不移植：`GetIntegeri_v` / `GetInteger64i_v` / `GetProgramiv`、`set_pixel_unpack_state`（前端已解析）、压缩格式概念、`pipe_transfer`。

### 3.3 能力位（`MGPCapBit`）

`CallMask` 取代"槽位是否为 null"这一隐式能力探测；`MGPCaps` = `DynamicBackendParameters` + `CallMask` + 格式能力表与 renderer 字符串两个 blob，握手后快照、`MakeCurrent` / `InitCapabilities` 后按 generation 重发布。位包括 viewport array、fp64 顶点、`kCapResidentSubData`、XFB / query 家族、`kCapNeedsHostIndexBytes`、`kCapNeedsHostUboBytes`、`kCapBackendOwnsXfbCapture`、`kCapRunAheadApply`（`MGPipeRunAheadCapBitsFor`：Espryt 自 P5e、Magma 自 P5f 之后的 Magma run-ahead 起发布，后者契约 `MG_Remote/CONTRACT-MAGMA-RUNAHEAD.md`）。**不存在"归属开关"（D-B7）**：multi-draw 分档与 restart 重写永远由 server 拥有，client 只在 caps 要求时提供索引字节。

## 4. 记录与 payload 约定（`MG_Pipe/MGPipeTypes.h`）

- 每个 payload 是平坦 POD、显式 padding、`static_assert` 精确尺寸；**永不含指针**。
- `MGPBlobRef{Offset, Size, Seg}` 指向 blob 区；`MGHostSpan` 是唯一随传输而变的形状（monolith 下是指针，split 下字节在 `SEG_STAGE`），只进变长尾。
- wire 记录头 `MGPWireRecHeader{Op, Flags, Size}`（8 B）；**没有逐记录序号**，seq 就是记录序数。
- 单条记录上界 = ring 一半，**记录本身永不分块**（R-10）；**内容侧分块**：会超出 `SEG_STAGE` 的 blob 按 `MGPipeStageChunkBytes()`（默认 segment/4 = 8 MiB）切成多条记录（buffer 范围走查、纹理整宽 slab，server 拼回整级）。

### 4.1 关键 payload

| payload | 要点 |
|---|---|
| `MGPResourceDesc` | buffer / 纹理 / renderbuffer 一个判别式 create/respecify 形状；`ImageBindableHint`、`ViewOf`（视图的存储属主）、`HasDefinedContent` |
| `MGPRenderStateDesc` / `MGPBindRenderState` / `MGPDynamicState` | §5.3 |
| `MGPVertexElements` | blob 同时带解析后的属性与绑定点视图（记录自洽） |
| `MGPSamplerView` / `MGPTextureParams` | view 只带视图限制；纹理参数挂纹理对象，`BuiltinSampler` 句柄不许为空（D-E1） |
| `MGPProgramDesc` | 逐 stage SPIR-V + 反射归档（§7） |
| `MGPFramebufferState` | 8 color + depth + stencil + client 解析后的 read surface；`ContentHash` 同时是 render-pass memo 键与发射抑制器；`Target` 含 `Named = 3`，applier 按 framebuffer 句柄存 |
| `MGPSubData` / `MGPSubRegion` | §6 |
| `MGPDrawInfo` / `MGPDrawRange` | `DrawVbo`；multi-draw 范围与用户索引在变长尾，indirect 的 `DrawCount` 由 client 解析 |

每条 `kVarTail` 的 `set_*` 带 `ContentHash`，hash 未变就不发（§5.4）。

## 5. 前端 state tracker（`MG_Impl/Pipe/Tracker`）

### 5.1 推送发生在 verb 之前的 validate 时刻，不在 GL setter 里

per-setter 推送会把 Blaze3D 每个 batch 的冗余 `glEnable/glDisable` 变成接口调用，严格慢于今天；正确形态是 gallium `st_validate_state`。落地为唯一的 `MGPipeValidateForVerb(MGPipeVerb)`（九个 validate 类，`FillPoints.def`），恰在每次经表调用之前、每个提前返回之后。只有今天就在 GL 调用时刻分发的资源 op（`BufferBackendOps` 的七个 hook）在调用时刻推送。

### 5.2 dirty 位：值类零新增记账，对象类 5 个聚合世代

- 值类组（render state、pixel pack、patch、attrib defaults、shader bindings …）直接复用既有版本计数器作快门。
- 对象类组（vertex buffers、framebuffer、sampler views / samplers / images、const / shader buffers、SO targets）靠 5 个**聚合世代**把快门降成一次 `Uint64` 比较——没有聚合就回答不了"有没有哪张已绑定纹理动了"。
- 完整性由 `scripts/gen_pipe_dirty_surface.py` 保证（每个 mutator → 必须 bump 的世代，`--check` + `--self-test` 进 CI）。**快门看不见自己的主体**是踩过三次的坑：新增字段必须带"记录字段 → setter → 快门"一行，优先混入已有世代（新计数器会撑大 pull 对象，G1 不允许）。

### 5.3 渲染状态：整块 blob 过线，身份只取 pipeline 子集（D-B1）

```
create_render_state(cso, pipelineSubsetChunks)   // 只带 pipeline 子集
bind_render_state(cso, version, pipelineVersion) // 稳态 12 B
set_dynamic_state(dynamicChunks, version)        // 只带变化的动态 chunk
```

- `RenderStateParameters`（1168 B）整块过线：Espryt 做三段 memcmp，字段顺序承重；拆成多个 CSO 要手工维护 ~150 字段划分且无绊线。
- 身份只取 pipeline 子集，否则 `glViewport` / `glScissor` 每次都铸新 CSO、冲掉 server 的 pipeline memo。划分只写在一处：`MGPipeRenderStateSpans` 的 chunk 表（7 pipeline chunk 396 B + 8 dynamic chunk 772 B，`static_assert` 完整性），G7 断言"子集 hash 变 ⟺ `m_pipelineStateVersion` 变"。
- Espryt 的 `SyncRenderState` 一行不动；Magma 的 pipeline memo 键是 `cso.slot`。

### 5.4 验证不变式、合并与抑制器

规范（D-B3）：**一条 verb 的全部 `set_*` / `bind_*` 在该 verb 之前完成；server 在 verb 处从已推送状态惰性特化 shader 与 pipeline。**除"资源 create 先于对它的 bind"外，`set_*` 之间无顺序要求。合并：整块优于逐字段；高水位直接是 `count` 实参；只发 program 解析过的集合；**集合 hash 抑制器**在 client 侧去抖（原先在后端的 ~175 行）。

### 5.5 sampler view 在 client 侧解析

shader 看见哪个纹理取决于 sampler uniform 类型、mipmap 完备性与未定义默认纹理；解析留在 client（带 memo），gallium "每槽一个 view" 就是解析后的形态。两处后端特定后处理留在 server：Espryt 的 raw-depth-fetch sampler 替换、Magma 的 feedback-loop 检测。

### 5.6 生命周期、共享组、composite program

- `resource_create` 在前端对象构造时发，存储由 `resource_respecify` 惰性定义，`resource_destroy` 在析构时发；死亡转发给每个 emitter（只释放 slot 曾导致 UAF）。view 先于存储属主销毁、FBO attachment 钉住纹理、buffer texture 钉住 buffer。
- program pipeline 合成体（`CompositeResolver`）完全在前端合成、推一个句柄；释放恰好一次（resolver 换掉或 `~ProgramObject`，D-H7）；记忆按 `(ContextId, 管线 GL 名)` 键控。

### 5.7 emulation 的归属

规则：**驱动表达不了的变换在 tracker 里 lowering，硬件 / 驱动强加的变换在 driver 里 lowering。**

| 归 client（读前端字节的纯 CPU 变换） | 归 server |
|---|---|
| client 顶点 / 索引数组（字节过线，永不是指针）、最大索引扫描、`*IndirectCount` 计数解析、CopyImage shadow 镜像、XFB CPU 图元计数、压缩纹理 / unpack 规整 | primitive-restart 重写、multi-draw 分档与展平（从索引宿主镜像读，§10.3）、viewport-array 回放、fp64 顶点窄化、image-bindable 存储加宽、生成 mipmap（client 分配 level 存储） |

**陈旧索引纪律是逐站点表**：client 侧扫描前的 reconcile 必须逐字复现 monolith 的集合（最大索引扫描 = `SyncPersistentMappedRange()` + `SyncGpuWrites()`；`*IndirectCount` 只前者）。

## 6. 纹理 subdata 与 dirty 归属

- 纹理上传由 Espryt 在 sync 时刻按**累积**区域做（Mali 按作业数计价，~100 个 rect 对一个 union box 实测 +6 ms/帧），逐 `glTexSubImage` 发记录会复现这个悬崖。
- 因此 client 在自己的 rect 模型里累积，在下一个 validate / flush 点发**一条** `ResourceSubData`，同时携带 union box 与 region 列表，**由 server 选上传形状**（决策留在付 GPU 代价的一侧）。步长显式携带，不再靠指针比较。
- **dirty 归属反转**：client 维护发射游标、发射后清自己的标志；清 dirty 要等 applier 的 acceptance（被拒的 level 留在脏表里）；server 自己的重新变脏是 server 的事。
- `TextureUploadShapeScenario` 把上传形状录成金标（P3b/P4b D2 起是门；Mali 帧时增量无设备，按 Adreno 记录）。

## 7. Shader state = SPIR-V + 反射归档

- `CreateShaderState` 过线的是逐 stage SPIR-V + 反射归档，**不是源码**：glslang 全在 client、SPIRV-Cross 全在 server，文件级切割；server 侧无编译池。归档序列化器 `ProgramArtifactsCodec`，monolith 只在 verify 构建里调它（D-H3）。
- server 在 verb 时刻从已推送状态**惰性特化**（D-B2：draw FBO clamp mask、fragColor 广播数、storage-block 绑定签名、image 格式、patch 参数等）——正是两个后端原来的做法。
- link / compile 失败不需要同步返回：`GL_LINK_STATUS` 由 client 从 `ProgramObject` 回答，后端失败以 `OnLog` ≥ERROR 无损呈现。
- 内部 shader（颜色 blit、深度 mip、multisample resolve）烘焙成签进树的 SPIR-V（`Wire*Spirv.h`），新鲜度门 `MOBILEGL_BAKED_INTERNAL_SHADERS`（`MG_Test/SelfTest/BakedInternalShadersTest.cpp`）重跑树内 glslang 逐字节比对（P7）。

## 8. 反向通道

### 8.1 `MGPipeCallbacks`（`MG_Pipe/MGPipeCallbacks.h`）

九个具名回调 + 一个正向终止符（`ResourceSubDataComplete`），取代后端直接 poke 前端对象的 95 个调用点（具名化是有意偏离 D8）。monolith 下直调，split 下是 `SEG_EVENT` 上的记录。

| 回调 | 作用 |
|---|---|
| `OnGlError` | 延迟的 GL 错误；**必须对命令流有序** |
| `OnGpuWritten` | GPU 写过的 buffer 范围（收窄 client 保守自建的 pending 集） |
| `OnBufferWriteback` | PBO 回读、XFB 捕获结果；必须与 epoch bump 有序 |
| `OnTextureWriteback` | CPU 回退生成 mip 的纹素 |
| `OnTexturePullRequest` | §8.4 |
| `OnMipLevelsGenerated` | 只带形状 |
| `OnSurfaceChanged` | 默认帧缓冲的格式与尺寸（server 拥有显示时 client 靠它得知窗口尺寸） |
| `OnCapsInvalidated` | 取代 `InvalidateCompileEnv` |
| `OnLog` | ≤WARN 有损，≥ERROR 无损 + 速率限制 |

后端凭空造的前端对象（Magma 占位纹理、swapchain 默认 FB 占位）改为 server 原生；pull 构建里的 `m_backend` 类成员真删会动 `sizeof`（G1），随 P13 退役（D-K）。

### 8.2 有序性是正确性要求

每次 writeback 之后的 epoch bump 必须在任何后续读该 handle 的命令之前被 server 应用。**反向通道需要与正向通道相同的有序保证。**

### 8.3 错误、ack 与日志

- **唯一允许同步 ack 的入口是 `glBufferStorage`**：`ResourceRespecify` 的 `kNeedsAck` 由逐记录谓词收窄到"不可变 **buffer**"（D-A2），纹理 OOM 在 monolith 里本来就推迟到 sync 时刻。respecify 带逐 level 作用域与"只换元数据"的形式，不误丢已接受的待上传。
- 其余错误一律晚到，走有序的 `OnGlError`。

### 8.4 唯一的新停顿类：server 发起的纹理重铸拉取（D-B6）

server 不保留纹素；image-bindable 重铸、整格式再生、view 源重铸会要求重发 level。缓解：`ImageBindableHint` 预防主因；拉取异步（阻塞 apply 线程，不阻塞应用线程）；保留 LRU 默认关（`MOBILEGL_PIPE_TEXEL_RETAIN_MB=0`）；显式终止符可带零个 region。真实语料上发生率可忽略（780 个统计窗口 2 次）；split 下这条路径今天仍是具名 `Fatal{UnmigratedEmulation, "texture-remint-pull"}`，归 P9。

### 8.5 XFB scatter 留在 server

补丁循环需要"捕获前的字节"，而拆分后权威影子归 server（`StagedShadow`），所以 scatter 在 server 原地跑完；回程只有一条 `OnBufferWriteback`，载补好的整段，按 `SEG_EVENT` 容量的四分之一切片（`MGPipeBufferWritebackSliceBytes`），最后一片落地即蕴含之前每一片。孤儿目标（`HasDefinedContent == 0`）零填充后照常散射。`OnXfbScatterReady` 已删除（回调 10 → 9）。落地论证见 [`notes/p34b/README.md`](notes/p34b/README.md)（espryt D1）。

## 9. 后端状态机改造

### 9.1 原样不动的东西

Espryt 的 persistent ring、buffer pool、fallback-repack、scratch FBO、驱动绑定影子、Adreno / Mali workaround、SPIRV-Cross 会话、restart 重写与 multi-draw 分档；Magma 的 `VulkanRenderer` memo、`PipelineFactory`、`ProgramFactory`、`UniformManager`、五个 `Vk*Manager`、`SwapchainObject`、D18 的节点式容器纪律。"输入变了、算法不许动"的那一类由 **G5 字节一致门**把关（`scripts/p3a_untouched_regions.sh` 十一函数、`scripts/p4a_untouched_regions.sh` 17 区），re-pin 必须两份同改。

### 9.2 strangler 脚手架：`PipeInputs` + 逐 verb 填充 + poison 世代（P1）

```cpp
// MG_Backend/MGPipe/PipeInputs.h —— 按 memo 键组织，访问器类型与后端原来读到的完全一致
#if MOBILEGL_PIPE_PUSH
#  define MGB_CTX (&::MobileGL::MG_Pipe::gPipeInputs)
#else
#  define MGB_CTX (::MG_State::pGLContext)
#endif
```

三步：A 别名（P1，机械替换 + 逐 verb 类填充点，`nm` 不变）→ B 推送（P2，tracker 填 `gPipeInputs`，verify 构建逐 draw 比对快照版）→ C 句柄化（P3a–P4a、P7，`SharedPtr<前端对象>` → 句柄 + 描述符，写回变回调）。poison 是**逐 verb 世代**：读一个当前 verb 未填的字段是 `Fatal{UnmigratedPipeInput, "<Getter>@<Verb>"}`。

### 9.3 Track V / Track H

Track V（值类型：render state、pixel store、capability 位、标量）是机械迁移；Track H（167 个 `SharedPtr<MG_State…>` 读点）是真活。74% 的读点是翻译输入，所以"bump 一个版本让 server 自己拉"行不通，值本身必须过去。

### 9.4 残余值块

迁移期临时调用 `SetResidualValueState`（`ResidualValueBlock`）：尺寸只降不升（1248 → 8，只剩 `CapabilityBits`），P13 变成 `static_assert(sizeof == 0)`；布局逐成员 `offsetof` 断言；stats 单独计字节。

### 9.5 身份 memo 的重键

`{slot, gen}` + 显式 destroy 让 21 条身份 memo 中 **11 条直接删除**、2 条的去抖搬到 client（§5.4）、7 条重键成更便宜的比较、1 条（D18）不动。统一事实：进 memo 键的版本计数器要么会回绕，要么根本不被它害怕的 mutation bump，身份比较是堵洞的补丁。

### 9.6 A/B 与口径收窄

`MOBILEGL_PIPE_PUSH` 子系统位图在阶段 B 是真 A/B；阶段 C 之后位清零时后端仍跑重键后的代码，所以**编译期** `MOBILEGL_PIPE_LEGACY_MEMOS`（默认 ON）保留 pre-handle 臂，随 pull 路径在 P13 退役。位依赖两侧都拒（客户端族门里**根本不发射**，否则会"客户端已清 dirty、服务端却走旧臂"而丢上传）；没有消费者的后端一条不发。退役的 twin 成员仍在 pull 构建里编译——真删会动 G1。

## 10. server 侧

### 10.1 对象表与 applier

`MG_Remote/Server/PipeApplier`：解码 → 更新对象表与 `PipeInputs` → 调后端。server 不持有 buffer 的完整副本（只有 staged shadow）、不持有前端对象图；任何传输下都不得有 `SharedPtr` 或裸前端指针跨过 applier 边界（P5c 角色守卫、P5e 规则 F）。`InProcessTransport` 与 spawn 走完全相同的编解码路径。

### 10.2 monolith 侧的净收益

即使 IPC 永不上线：复用地址 ABA 一整类不可表达；FBO → program 排序 hazard 消失；`SwapchainObject` 写 `MG_Impl` 的分层倒置消失；`inproc` = 渲染线程。monolith 净代码量是增加的，论据只看逐线程 CPU。

### 10.3 索引宿主镜像（`Server/IndexHostMirror`，P8，待重裁）

设计：`kCapNeedsHostIndexBytes` 下，server 从本来就要收的 `ResourceCreate/Respecify/SubData` 流增量维护 element-array buffer 的宿主镜像，零额外流量；预算 `MOBILEGL_PIPE_INDEX_MIRROR_MB`（默认 64），超预算退化为逐 draw 经 `MGHostSpan` 传送。**P6 的 a6 审计实测宿主索引 span 无 producer、client 索引已是 owned buffer**，所以 P8 要按真实消费者重新裁定，不照抄本节（[`notes/p8/README.md`](notes/p8/README.md)）。

## 11. 传输与数据面（`MobileGL/MG_Remote/`）

### 11.1 段

| 段 | 拥有者 | 默认 | 内容 |
|---|---|---|---|
| `SEG_CMD` | client（server 只读） | 8 MiB | `RingControl` 页 + POD 记录 + 小内联负载 |
| `SEG_STAGE` | client | 32 MiB | bulk 字节：buffer / 纹理 subdata、UBO、client 数组、persistent-map 脏块 |
| `SEG_REPLY` | server | 16 MiB，8 × 2 MiB slot | readback 像素、acceptance 答案（ID-47） |
| `SEG_EVENT` | server | 256 KiB SPSC ring | 回调事件 |
| `SEG_SHADOW` / `SEG_ADOPT` | client / server | — | 零拷贝 shadow（Phase 2）/ ≥16 MiB 采纳（P11）；stream 数据面上具名拒绝 |

段在同机上由 `ShmSegment` 创建（Android `ASharedMemory`、Linux `memfd`、Windows `CreateFileMappingW`），描述符经 `SCM_RIGHTS` 传递；stream 数据面上两端各持一份同尺寸的私有段（§11.9）。

### 11.2 `RingControl`（`Ring.h`）

一页 4 KiB，每个争用组一条 cache line，按写者分组（P6 lk）：producer 线 `cmdHead` / `submittedSeq`；consumer 线两个 tail；四个 consumer 写的水位收成 `Progress`（`appliedSeq` / `retiredSeq` / `completedFrameSerial` / `presentAckSerial`）；最后是 epoch、generation、park 标志与 `eventRingFull` / `eventDropped`。游标是单调字节计数、2 的幂掩码、永不重置；pad 记录处理 wrap；不可能的头 → `Fatal{ProtocolCorruption}`。

### 11.3 双向 doorbell（`Doorbell.h`）

两个方向都是"先自旋、再置 parked、再阻塞"，发布方仅当对端 parked 时敲；丢失唤醒由两个 `seq_cst` fence 关闭。`CondVarDoorbell`（同进程，带 `Kill()`）与 `SocketDoorbell`（跨进程，对端关闭即死亡检测）。stream 数据面上门铃是本地条件变量，由读线程敲。

### 11.4 控制面（`protocol.fbs`、`Framing.h`、`ITransport.h`）

- 一份 FlatBuffers schema：热路径是 `struct` 直接进 ring（与 POD 逐条 `static_assert` 对齐）；罕见 / 变长 / 需演进的走控制连接的 `CtrlMsg`（`Hello`、`Welcome`、`Refuse`、`CapsSnapshot`、`SurfaceOp/SurfaceReply`、`SessionFault`、`LogLine` …），union tag 只追加，控制修订号钉在 `sha256(protocol.fbs)` 上。
- 生成物提交进树，CI `flatc-check` 重生成并 diff；codegen 不进默认构建图。
- 封帧 `[u32 'MGLF'][u32 len][payload]`，64 MiB 上限，读时校验，坏帧立即闩住失败。

### 11.5 WAR 危害、拷贝账与背压

Phase 1（今天）：GL 调用时刻把字节拷进 stage slot，slot 到 apply 越过它为止不可变，危害按构造消除，代价一次 memcpy；Phase 2（`SEG_SHADOW` 零拷贝）未做。server 没有第二份 `BufferObject`，不存在中间拷贝。分配失败升级：扩容 → 对最老未退休批次有界等待 → 硬 drain + generation bump（之后 tracker 全部重推）。

### 11.6 publish、序号与 credit

- 每条记录 release-store `cmdHead`，仅当 consumer parked 时敲门铃；显式门铃点：`present`、`kNeedsAck`、`eglMakeCurrent`、`glFlush`、stage 余量不足、轮询类入口。
- seq = 记录序数；字节 credit 与 present credit 两个独立窗口。
- **等待规则由 `WaitClass` 列决定（P5e）**：`kWaitReply` / `kWaitPresent` / `kWaitApplied` / `kWaitNone`；run-ahead 武装时 `kWaitNone` 发布即返回。barriered 谓词（`MGPipeBarriered`）client 与 server 用同一个函数、同一份数据算。
- 强制等待点：`MapBuffer(READ)` / `GetBufferSubData` 源走 `SyncGpuWrites`；`glFinish` = 等 `appliedSeq` 追上再排空反向通道；每个 `Server*` EGL forwarder 之前等一次。`glGetError` 最多晚一个 present credit。

### 11.7 事件回传与溢出

client 在 `glGetError`、query / sync 查询、`eglSwapBuffers`、buffer 回读与每轮等待循环中排空 `SEG_EVENT`。日志有损；语义事件无损：环满时 server 置 `eventRingFull`、在记录边界停 apply、敲 client，client 排空后清标志并敲回（两半都在，才不死锁）。run-ahead 下 producer 阻塞等排空，整笔预算 `MOBILEGL_IPC_EVENT_WAIT_MS`；client 不排空 / 已走 / 会话停止 → `ReverseChannelForfeit{…}` 弃投并闩住（Ph PH-6），而不是 `Fatal` 打掉 server。不发布 `kCapRunAheadApply` 的 server（lockstep 臂）保留 P5c 的 Fatal。

### 11.8 fence 与无 present 负载（P10）

fence 完成度必须来自真的逐 fence 退休，不是 present 水位（DirectGLES 的 `g_completedFrameSerial` 今天只在 `Present` 前进）；无 present 循环需要 server 插非 present fence tick。Magma 那半已随 run-ahead 落地。

### 11.9 传输栈的两根轴（P6.5）

终局（2026-09-22）：client 与 server 可在不同机器、不同 OS / 架构上经 TCP 连接，同机 `spawn` 保留。传输栈拆成**两根独立可选、握手协商、可混搭**的轴，上层只见两个接口：

| 轴 | 接口 | 实现 | 选项（`MOBILEGL_IPC_CONTROL` / `MOBILEGL_IPC_DATA`） |
|---|---|---|---|
| 控制面 | `ITransport`（只承载控制帧，不承载描述符） | `SocketTransport` | `fork`（继承 fd）、`unix:<path>`、`tcp://host:port` |
| 数据面 | `ILink`（记录预留 / 提交、水位、reply、event、span 解析） | `ShmLink`（共享段 + `RingControl` + 门铃）、`StreamLink`（分块封帧，水位与 reply 变消息，发送窗口） | `shm`、`stream`、`auto` |

- 配对在会话建立时选定一次；`LinkTerms`（数据面、窗口尺寸、`maxReplyBytes` …）由 **server 陈述**，client 请求可被钳制。
- 共享段交付与控制面解耦：`ShmLink` 自带 AF_UNIX aux 汇合名，在 `Welcome` 里公布，所以同机"TCP 控制 + 共享段数据"是合法配对。`auto` 的判据是**段实际交付成功**，回落到 `stream` 必须具名进日志。
- **上层零链路种类分支**：`ShmLink` / `StreamLink` / `RingControl` 等只出现在 `Transport/` 与其测试（纯度 grep 门）；角色谓词回答"谁在哪个进程"，不回答"用什么链路"。
- 两端是不同二进制：`wireFingerprint`（`PipeFields.def` 派生的逐成员布局摘要 + 目录摘要 + 字节序 / 指针宽度）永远比较；`buildFingerprint` 只在 `Dial == Fork`（或 `MOBILEGL_IPC_REQUIRE_SAME_BUILD=1`）下比较；不一致是 `Refuse`，不是 abort。
- TCP 上每条 `kWaitReply` 就是一个 RTT、`SEG_STAGE` 字节对的是 30–100 MB/s 的链路：这两列从"记录项"变成可行性数（开放问题 19、20）。

第一波（全 TCP 控制 + stream 数据）已落地；同机混搭（nd `auto` / sd）是第二波。细节 [`notes/p65/README.md`](notes/p65/README.md)，契约 `MobileGL/MG_Remote/CONTRACT-P65.md`。

## 12. persistent map 与 ≥16 MiB 采纳

`AcquirePersistentMap` 是永久的地址空间捐赠（≥16 MiB 可变 store 自动采纳，MC 26.3 p99 163 → 21 ms），整个 monolith 改造期不动（D-B4）。拆分下三档由 POST 探针选择（spike B 实测）：

| 档 | 形态 | 结论 |
|---|---|---|
| T0 | client 分配 `AHardwareBuffer` BLOB，server 导入（Vulkan / GLES） | 唯一在两台设备、两个后端上都完整读写的档（P11 主攻） |
| T1 | server 导出 opaque fd | 仅 Adreno 的 Vulkan 路径 |
| T2 | 拒绝，client 侧推送 | 永久正确回退；**今天 split 用的档**（`MOBILEGL_IPC_ADOPT_TIER=2`），stream 数据面上强制 |

T2 下 client 侧推送三件套：不做 map/unmap 命令对（payload 带 `hasLiveHostWrites`）；在每个 validate 点按块（`MOBILEGL_IPC_PERSISTENT_BLOCK_KB`，默认 64）推送可达的已映射 buffer，脏页追踪 + 哈希抑制只发变化的块（P5d）；门 `PersistentCoherentMapScenario`。`mpr` 数的是每一次 `MapPersistent` 发射（铸成或拒绝都算），所以在 monolith 下也可断言。

## 13. 回读、roundtrip 清单与验证

### 13.1 稳态零 roundtrip 与不可避免的阻塞点

- 零 roundtrip：全部 draw / clear / blit / copy / dispatch / XFB / bind / CSO / `set_*` / 上传 / `present`；全部 caps 站点；`glGetError` / `glFinish` / `glFlush`；fence 与 query 的创建和非阻塞轮询；`eglSwapBuffers`；restart / multi-draw。
- 不可避免（罕见）：握手；surface 生命周期；`glReadPixels` 到客户内存；GPU-write pending 的 buffer 首次 CPU 读；带超时的 sync / query 结果；`glBufferStorage` 的 ack；纹理拉取；ring / stage 耗尽与 present credit。
- 跨机上每条 `kWaitReply`（`ResourceCreate`、`SetTextureParams`、纹理 `ResourceSubData`）都是一个 RTT：逐帧次数是 P6.5 必测数，减少它是 P9 / P10 的活。

### 13.2 五部分验证门（取代 monolith 的字节一致门）

1. **接口纯度**：A 门 include 图（`scripts/check_include_closure.py`）、B 门 server 镜像不引用 `MG_State::GLState` 与 glslang（P6 的 a6 实测 server 仍链 `MG_Impl`，由 `scripts/link_ratchet.py` 的下行棘轮追踪，P13 转绿）、C 门 `MG_Backend` 里零 `pGLContext`；外加"每个后端 memo 键都是 `{slot, gen}`"（`HandleRecycleScenario`）。
2. **语义影子比对** `MOBILEGL_PIPE_VERIFY=1`：tracker 再用 `SnapshotFromGLContext()` 填一份 `PipeInputs`，G4 逐字段每 draw 比对，打印第一个分歧；verify 构建永不出货，活过 P13。
3. **行为 A/B**：trace 语料在 monolith-pull / monolith-push / split 下 SSIM ≥ 0.99；`DirectGLES.` 与 `DirectGLES.Split.` 等名集合逐名相同（G2）、测试名只增不删（G14）；CTS 逐后端 0.5 pp 内；上传形状金标。
4. **性能**：Redmi `2f7cbe2e` reboot-clean、同热窗口配对 A/B，指标是**逐线程 CPU 时间**；对着 pull 臂**只记录、不设门**（用户 2026-09-08）。
5. **覆盖 + poison + 句柄纪律**：G6 0 UNMAPPED、dirty-surface 0 未映射、G8 归属完备、逐 verb poison、G7、残余块棘轮。

两条幸存的字节级等式：`MOBILEGL_BUILD_DISAGGREGATED=OFF` 时库里零 `MG_Remote` 符号；**G1** pull 构建的符号集与 `.text` 对基线恒等（每阶段 0 增 / 0 删 / 0 resize / 0 重命名）。**每个门必须能因它存在的理由变红**（R-16）：带阴性对照并真跑过一次红；公共 GL 看不见的改白盒断言；拒绝普查逐用例读私有日志（`ctest -V` 是假零）。

### 13.3 长期语义门：MGPipe recorder（P13）

`MG_Test` 的 mock 后端变成 MGPipe recorder，在一组 fixture 上录每 draw 的已推送状态做金标；它只覆盖推送内容，不覆盖后端对它的解释（开放问题 12）。

## 14. Present、线程与帧节奏

- `eglSwapBuffers` → `present{frameSerial}` → publish + 敲门铃 → 返回，除非超出 credit；**`present` 与 `eglSwapBuffers` 严格 1:1**（后端的帧边界排空只在 `Present` 内发生）。
- `MOBILEGL_IPC_PRESENT_CREDIT` 默认 1（range 1..8）：credit 是 run-ahead 唯一的稳态背压，`FrameSerial` 由 client 铸造、1 起算，server 在 `Present()` 返回后归还 credit；等待发生在**编码之前**。credit > 1 从未量过（P10）。
- 预算（~850 draw/帧）：`SEG_CMD` ~70 KB/帧，`SEG_STAGE` ~1 MB/帧；跨机时这 ~1 MB/帧对着的是 Wi-Fi。
- 线程：client v1 不加线程，编码在 GL 线程直接写 ring；server 有 io 线程与**终身持有原生 context** 的 `mgl-srv-apply`。全库无有效亲和性控制（Redmi 内核忽略 app 线程的 `sched_setaffinity`）。
- 拆机顺序：publish + server 排空 → 停 apply 线程 → 关传输 → client 排空编译池 → `MobileGL::Destroy()`（落地形状见 §17）。

## 15. 进程、EGL 与平台（P6 / P6.5 / P12）

### 15.1 启动与握手

- 端点：`fork`（`socketpair` + `fork`/`execve`，fd 3 = socket；server 由 `MOBILEGL_IPC_SERVER_PATH` 或 `dladdr` 同目录的 `libMobileGLServer.so` 定位）、`unix:<path>`、`tcp://host:port`。设备侧 supervisor `mobilegl_server_main <endpoint> --serve`：listen → 认证 → 每会话 fork 一个子进程（或进程内线程，P12），同时只服务一个会话，第二个连接 `Refuse{Busy}`。
- server 进程里 `Transport` 读作 `Spawn`，**不强制 monolith**（两百多处 `Transport != Monolith` 判断靠它选 server 臂；`CONTRACT-P6.md` §3.1 修订了原"子进程强制 monolith"的规则）。防无界 fork 链靠不继承的 `Dial == No`（`MOBILEGL_IPC_DIAL=no`），spawn 时构造的 envp 剔除 `MOBILEGL_TRANSPORT` 与 `MOBILEGL_IPC_*` 作第二道独立保险；server 主程序自设 `MOBILEGL_IPC_ROLE=server`。
- `Hello`（abi、后端类型、指纹、配置、令牌、`LinkTerms` 请求）→ `Welcome`（server 陈述的尺寸、段 / aux 名、`dataNonce`、真实 pid）或 `Refuse{code, detail, 对端值}`；握手路径零 abort。非 loopback 监听必须有令牌（≥16 字节、常量时间比较、数据面按 `dataNonce` 绑定到已认证的控制连接、fork 前认证；Ph，OQ-21 裁定不做 TLS）。
- TCP keepalive / 心跳失败**由传输层报告为挂断**，进 device-lost 闩；"闩绝不取自 apply 超时"不因此松动。冷启动期 server 发 `SurfaceProgress`，client 预算这段沉默（控制修订 2）。

### 15.2 Android

- 交付链（spike A 已证）：APK 唯一可 exec 的位置是 `lib/<abi>/`，server 以 `add_executable` + `lib*.so` 命名构建；从 `untrusted_app` 进程 `fork`+`execve` 同域、零 avc denial；`posix_spawn` 在 minSdk 26 不可用。一份共享库两个角色（stub `dlopen` + `dlsym("mobilegl_server_main")`）。
- 离屏 server：前台 `MobileGLServerService`（`:mglsrv`），从 `nativeLibraryDir` exec，fork-per-session，EGL 是 pbuffer / surfaceless。
- **上屏 server（P12 子集）**：窗口归 **server 自己**（client 的窗口不跨进程）。`MobileGLDisplayActivity`（`:mglwin`）持全屏 SurfaceView，在自己的进程里用一个线程跑 TCP server（会话串行、会话间重置闩、后端按进程钉住）；`ServerDisplay` 以 acquire / release / 几何三个钩子持有窗口，apply 线程以**租约**使用它。client 设 `MOBILEGL_IPC_SURFACE=server` 后**无头**：`eglCreateWindowSurface(NULL)` 发一帧 `WindowKind::ServerOwned`（控制修订 3），不发 `SetWindowHandle`，尺寸由 reply 带回，resize 变成对 server 窗口的几何请求。一台设备同一时刻一个 server；一个会话一种表面模式（`SurfaceModeMismatch`）；无显示的 server 对 ServerOwned 具名拒绝（`NoServerDisplay`）。
- 失窗：`surfaceDestroyed` 有界（3 s）阻塞，直到 apply 线程（批内每条记录前也检查）放掉后端表面并以 `Fatal{ServerWindowLost}` 闩住会话；server 继续监听。进程内会话结束时手动做进程退出本来白给的清理（Espryt twin 与单元影子、swap interval、Magma renderer）。
- `HeadlessGL` 的 fork 预检会产生孤儿 server：server 的 EOF 检测即时且无条件退出。

### 15.3 Linux / Windows / 崩溃

- Linux / WSL / CI 永不开窗（`EGL_PLATFORM=surfaceless`）；Wayland 不支持。Windows / 桌面 client 在 TCP 终局下是真实形态（Windows 是第二批，开放问题 22）；macOS 不拆分；Windows 机器不是正确性门。
- server 死：client 读到挂断 → **device-lost 闩**（GL 调用 no-op、`eglSwapBuffers` 返回 `EGL_CONTEXT_LOST`、reset status `GL_UNKNOWN_CONTEXT_RESET`）；`Session::Fail` 漏斗向对端发 `SessionFault` 帧命名家族（`FatalFamilies.def`）。**不重启**：`MOBILEGL_IPC_RESPAWN` 今天是具名拒绝。client 死：server 读到 EOF 立即销毁 context 并退出（supervisor 继续服务下一连接）。

## 16. 构建布局

```
MobileGL/MG_Pipe/            永远进构建：*.def 目录与表、MGPipe*.h payload / 句柄 / 回调、PipeApply、PipeRoute、generated/*.inc
MobileGL/MG_Impl/Pipe/       Tracker、PipeFill、SlotAllocator、CsoCache、CompositeResolver、ResourceTracker、SetHashSuppressor、*Emit.h
MobileGL/MG_Backend/MGPipe/  PipeInputs.{h,cpp}
MobileGL/MG_Remote/          仅 MOBILEGL_BUILD_DISAGGREGATED：CONTRACT-*.md、CapsCodec、FatalFamilies.def / FatalFunnel、Handshake.h
  Protocol/   protocol.fbs、generated/、SurfaceOpCodec、mg_protocol_base.h
  Transport/  ITransport、SocketTransport、InProcessTransport、ILink、ShmLink、StreamLink、Ring、SessionRings、ReplySlot、EventRing、
              Doorbell、ShmSegment(+Posix/Win32)、FdPassing、Framing、AuthToken、ControlInbox、LinkMetrics、WireLog
  Wire/       PipeWireCodec（记录编解码、WireVerbSink）
  Client/     BackendObject_Remote、ClientSession、EmitTables、WireTables、CapsMirror、PersistentMapTracker、GpuWritePending
  Server/     PipeApplier、ServerLoop、ServerSession、ServerMain / ServerEntry / ServerSpawn、PreAuthGate、StagedShadow、StagedTextureStore、
              SurfaceControlFrame、ServerDisplay、InProcessServer、DisplayServerJni
```

- `MOBILEGL_BUILD_DISAGGREGATED`（默认 OFF）追加 `MG_Remote/**`；OFF 时 `MG_Config::Transport` 是 `constexpr Monolith`。`MOBILEGL_BUILD_DISAGGREGATED_INPROC` 隐含前者并加角色隔离：两个角色靠 apply 线程与控制邮箱分开，而不是给 1494 个 `pGLContext->` 读点加 TLS。
- `MOBILEGL_TRANSPORT = monolith | inproc | spawn` 是**拓扑**（G1 相关）；控制面与数据面由 §11.9 的两个变量选。**split build 不设 `MOBILEGL_TRANSPORT` 时是 monolith 对照臂**。
- 四个构建 flavour：pull（默认）、push、verify、split；Android 三份 APK flavour（pull / push / split）。
- 测试接线：ctest `ENVIRONMENT` 用 `mgl_itest_join_environment(...)` 构造；每条 split / spawn / tcp 条目带独立日志路径，日志按角色分文件（`<base>.client.log` / `<base>.server.log`）；spawn、tcp 车道与 split 名集合一致（`scripts/ci/spawn_lane_parity.py`），每条目记 arm 证明。
- CI（`.github/workflows/test.yml`、`apk.yml`）：`pipe-gates`（G1–G8、生成器 self-test、符号报告、dirty-surface、字段归属、G5、文档引用 lint）、`flatc-check`、include 闭包、各 flavour 的 build / integration / retrace 车道、APK + AVD。两份 workflow 里的 `feat/disaggregated` 触发器是临时的，合入 `dev` 前移除。

## 17. 阶段落地形状（索引）

各阶段落地时的形状、偏差与论证原文在 notes 里；下表只列仍然生效的规则。

| 原节 | 主题 | 仍生效的规则 | 全文 |
|---|---|---|---|
| 17.1 | verb barrier 与诚实的同地址空间传输（P5） | 同地址空间不得成为旁路（R-2）：encoder 把 `MGHostSpan::Ptr` 恒写 `nullptr`；reply 以记录序号为 slot id（R-3）；等待看门狗 120 s 只判死锁，不是性能门 | [`notes/p5/README.md`](notes/p5/README.md) |
| 17.2 | 后端槽三类、caps、tight readback（P5） | A 本地答 / B 发射 / C 具名拒绝，永不回落 monolith（R-4）；能力只读 `CallMask`（R-8）；`ReadPixels` 线上恒 tight，client 按自己的 pack state 散射 | 同上 |
| 17.3 | server 角色、shadow 与退出（P5） | apply 线程终身持有 native context，EGL 罕见操作经控制邮箱进 apply 线程；staged shadow 只交 server-owned 拷贝；有界退出顺序 | 同上 |
| 17.4 | lane 隔离（P5） | 每条 split 条目私有日志；E1 / E3 控制把"选中条目被跳过"判为失败 | 同上 |
| 17.5 | class-C verb 迁移（P5b） | 迁移顺序由动态普查决定；一次迁移 = 一个发射器 + 一个 sink 体；记录逐字带 GL 调用参数，未测量的形式按名拒绝（规则 D） | [`notes/p5b/README.md`](notes/p5b/README.md) |
| 17.6 | 共享地址空间访问归零（P5c） | server 端纹理 staged shadow；`SEG_EVENT` 是唯一反向通道；sink / twin 按记录句柄解析；`applier_reset` / `object_death` 控制记录；角色守卫 `Fatal{RoleViolation}` | [`notes/p5c/README.md`](notes/p5c/README.md) |
| 17.7 | run-ahead（P5e） | 规则 F：unbarriered 记录的 apply 不读任何 client 内存（违反即具名 Fatal，与 strict 开关无关）；`gPipeInputs` 是 server 角色内存；`RunAheadArmed()` 在首次 caps 采纳时锁存，只能关不能开；`RUN_AHEAD=0` 是 A/B 对照，`VERB_BARRIER=0` 会连带关掉 run-ahead | [`notes/p5e/README.md`](notes/p5e/README.md) |

## 附 A：开关

CMake：`MOBILEGL_BUILD_DISAGGREGATED`（OFF）、`MOBILEGL_BUILD_DISAGGREGATED_INPROC`（OFF，隐含前者）、`MOBILEGL_PIPE_PUSH`（OFF；push / verify / split flavour 打开）、`MOBILEGL_PIPE_VERIFY`（OFF；隐含 push，永不出货）、`MOBILEGL_PIPE_LEGACY_MEMOS`（ON）、`MOBILEGL_BUILD_SERVER_SPIKE`（仅 Android spike）、`MOBILEGL_FLATC_EXECUTABLE`（只服务 `flatc-check`）。

运行时，MGPipe（`MobileGL/Config.h`、`ConfigLoader.cpp`）：

| 变量 | 默认 | 说明 |
|---|---|---|
| `MOBILEGL_PIPE_PUSH` | pull `0`；push `0x1fff` | 子系统位图（`MG_Pipe/MGPipe.h`，位永不复用；依赖两侧都拒）；位 63 是"关 CSO 内容寻址"的行为对照 |
| `MOBILEGL_PIPE_VERIFY` / `_VERIFY_FATAL` / `_VERIFY_CORRUPT` / `_POISON_OMIT` | 0 / 1 / 空 / 空 | 影子比对与两个阴性对照（verify 构建才有） |
| `MOBILEGL_PIPE_HANDLE_ABA_CONTROL` | 0 | 故意打掉句柄身份的阴性对照 |
| `MOBILEGL_PIPE_STATS` / `_STATS_PERIOD` / `_STATS_FILE` | 0 / 120 / 空 | 边界计数器（附 B）；dump 按角色写 `<base>.client.json` / `<base>.server.json` |
| `MOBILEGL_PIPE_LEGACY_MEMOS` / `_TEXEL_RETAIN_MB` / `_INDEX_MIRROR_MB` | ON / 0 / 64 | pre-handle 臂、纹理拉取保留、索引镜像预算（P8） |

运行时，传输与 IPC：

| 变量 | 默认 | 说明 |
|---|---|---|
| `MOBILEGL_TRANSPORT` | `monolith` | 拓扑：`monolith` / `inproc` / `spawn` |
| `MOBILEGL_IPC_CONTROL` / `MOBILEGL_IPC_DATA` | `fork` / `auto` | 控制面端点与数据面（§11.9）；`MOBILEGL_IPC_SERVER_PATH` 定位 fork 出的 server |
| `MOBILEGL_IPC_TOKEN` / `MOBILEGL_IPC_REQUIRE_SAME_BUILD` | 空 / 0 | 令牌（≥16 字节；无令牌只允许 loopback）；要求两端同一构建（车道纪律） |
| `MOBILEGL_IPC_PREAUTH_*` / `MOBILEGL_IPC_AUTH_BACKOFF_*` | — | server 预认证的上限与失败退避（Ph） |
| `MOBILEGL_IPC_CONTROL_TIMEOUT_MS` / `MOBILEGL_IPC_COLD_START_MS` | 5000 / 20000 | 控制回复的稳态上限与冷启动预算 |
| `MOBILEGL_IPC_LOG_FORWARD` | 远端时开 | server 日志经控制连接前送，client 写进自己的 `<base>.server.log` |
| `MOBILEGL_IPC_SURFACE` | `offscreen` | `server` = 使用 server 自有窗口，client 无头（P12） |
| `MOBILEGL_IPC_RING_MB` / `MOBILEGL_IPC_STAGE_MB` | 8 / 32 | `SEG_CMD` / `SEG_STAGE`；stage 的 1/4 是内容分块预算 |
| `MOBILEGL_IPC_RUN_AHEAD` | 1 | P5e run-ahead；只是合取式的一半（server 须发布 `kCapRunAheadApply`）；`0` 是 A/B 对照 |
| `MOBILEGL_IPC_VERB_BARRIER` | 1 | `0` 只作阴性对照，且同时关掉 run-ahead |
| `MOBILEGL_IPC_PRESENT_CREDIT` | 1 | 1..8，§14 |
| `MOBILEGL_IPC_BATCH_WAITS` | 1 | lockstep 下值类记录发布即返回；verify 强制 0 |
| `MOBILEGL_IPC_EVENT_WAIT_MS` | 2000 | 反向事件遇满环时 server 等 client 排空的整笔预算（§11.7） |
| `MOBILEGL_IPC_WIRE_DEFERRED_MB` | 64 | Magma wire 臂延迟回收的水位线；`0` 是 M2 阴性对照 |
| `MOBILEGL_IPC_PERSISTENT_BLOCK_KB` / `_PERSISTENT_HASH_SUPPRESS` | 64 / 1 | persistent-map 推送粒度 / 只推变化的块；`0` 是对照 |
| `MOBILEGL_IPC_ADOPT_TIER` | 2 | `auto/0/1/2`；T0 / T1 今天是 Fatal-at-use |
| `MOBILEGL_IPC_STRICT_ERRORS` / `MOBILEGL_IPC_AUDIT` / `MOBILEGL_IPC_ROLE_SPLIT_STATE` | 0 / 0 / 0 | 残余输入读升级为 Fatal / 退休 staging 填 `0xDD` / 双块演练（P5f） |
| `MOBILEGL_IPC_SPIN_US` / `MOBILEGL_IPC_SERVER_AFFINITY` | 50 / `auto` | park 前自旋预算 / apply 线程亲和（Redmi 内核忽略） |
| `MOBILEGL_IPC_RESPAWN` | — | 具名拒绝：没有阶段实现 server 重启后的全量重推 |

显式不设立：`MOBILEGL_IPC_PROGRAM`（没有 relink 档）、`MOBILEGL_IPC_VALIDATE_SERVER`。计划中未接线：`MOBILEGL_IPC_POLL_ESCALATE`（P10）、`MOBILEGL_IPC_IDLE_EXIT_S`（未解析）。

## 附 B：边界计数器（`MobileGL/MG_Util/Metrics/PipeStats.h`）

关闭时每站点一次全局 load + 一条永不命中的分支。字节类（stage buffer / texture / UBO / client 数组、`pmap`、`resid`、`csob-blob`、`seg` = 写进 `SEG_STAGE` 的字节）、调用类（`draws`、`accessor-calls`、纹理上传 emit / box / rect / jobs、`csom` / `csob`、`mpr`、`trp`、`rsp`、`wrec`、`maxrec` / `ringwraps` / `ringpads` / `ringwaits`、门铃 `srv` / `srvpark` / `cli` / `clipark`）与六个 memo 门的 hit/miss。每 `MOBILEGL_PIPE_STATS_PERIOD` 帧一条 `MGPipe stats:` 汇总行，按角色写日志。读法陷阱：`accessor-calls` 是静态下界，判性能只看 CPU 时间；spawn 下 client 不推进帧窗口（`frames=0`），窗口化字段在 spawn client 上无效，run total 仍有效（[`notes/p6/README.md`](notes/p6/README.md) §12.1）。站点清单（哪些路径**没有**接线）写在 `PipeStats.cpp` 头部，那份清单是契约。
