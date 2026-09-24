# 前端：state tracker、纹理上传、着色器（原 ARCHITECTURE §5–§7）

> 设计细节。设计要点与全部章节的索引见 [`../ARCHITECTURE.md`](../ARCHITECTURE.md)。章节编号沿用原 `ARCHITECTURE.md`，代码注释里的 `ARCHITECTURE.md §N` 按编号在这里找到。

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
