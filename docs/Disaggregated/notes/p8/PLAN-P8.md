# P8 计划（重定界，2026-09-29）

> 依据：只读核查（树外 `~/w7/notes/p8/AUDIT-P8.md`，审计树 `p11/main`）。用户 09-29 批准重定界并开工。原范围划掉保留在 [`README.md`](README.md)。裁定见 [`INTEGRATOR-DECISIONS-P8.md`](INTEGRATOR-DECISIONS-P8.md)。

## 结论

- 原方向"把仿真搬到 client"对剩下的工作不成立：三个真实缺口都在 server 侧（Espryt 生成 mip、Espryt 暂存影子与 GPU 写、Magma wire indirect）。
- split 可达的 `UnmigratedEmulation` 只剩 Espryt 生成 mip 的两个名：`generate-mipmap-cpu-filter`（`DirectGLES.cpp:12907-12912`，RGB16F / RGB32F）与 `generate-mipmap-storage`（`DirectGLES.cpp:11631-11653`，存储检查比 GL 要求严）。
- `MultiDraw*+CLIENT_INDICES` / `+INSTANCED` 四个 server 闩（`PipeApplier.cpp:972-978`）我们的 client 已发不出，只剩对外来 peer 的协议守卫。
- 396 个只在 monolith 登记的用例，CI 的 A/B 步骤已用 inproc 全量重跑（`test.yml` Transport A/B），缺的是按名登记、spawn / tcp 两臂与武装断言。

## 已关闭（无需工作）

| 原项 | 由谁完成 | 证据 |
|---|---|---|
| `HostResolve.cpp` 最大索引扫描、`*IndirectCount` | `beba0256` 自有 buffer、`62bfe461` ClientFetchPlan；server 读自己的影子 | `ClientFetchPlan.h:203-240`、`DirectGLES.cpp:9533-9571`、`DirectVulkan.cpp:381-385` |
| `Server/IndexHostMirror`、`index-mirror-bytes` | 不建：server 每个 buffer 已有 R-11 暂存影子，restart 重写从它读 | `DirectGLES.cpp:9198-9252` |
| multi-draw client indices | client 拼成一个自有 EBO | `EmitTables.cpp:688-709`、`OwnedDrawInputs.h:51` |
| `texture-remint-pull` | P9 W2 | [`../p9/W2-REMINT.md`](../p9/W2-REMINT.md) |
| `kCapDriverOrderedXfbCapture` | 被 `kCapBackendOwnsXfbCapture`（P5b t2）取代 | `MGPipeTypes.h:124-135` |
| viewport-array 回放（开放问题 9） | server 一次 draw 内按状态分组回放 | `DirectGLES.cpp:8421-8465` |
| 门 `ClientArrayAfterComputeWriteScenario` | 改名 `ClientVertexArrayScenario.GpuWritten*`，已登记 split 各臂 | `ClientVertexArrayScenario.cpp:271` |
| 门 trace split SSIM ≥ 0.99 | P7 最低 0.9935 | [`../p7/iris-census-1135c664.md`](../p7/iris-census-1135c664.md)；出口再跑一次 |

## 包（第一波并行：P8-0、A、B、C、D、E、F；G 等 P8-0）

| 包 | 内容 | 门 | red-once |
|---|---|---|---|
| P8-0 普查 | 设备（红米，两后端）与主机的 trace 语料：monolith 下临时插桩计 5 个 `UnmigratedEmulation` 点、mip 窗口谓词、GPU 写过的 EBO / 参数被 CPU 读、CopyImage 到非可渲染格式的命中数；spawn 下 grep `Fatal{` / `Refuse{` / `MGWIRE-DECLINES`，`MOBILEGL_PIPE_STATS=1` 取逐 op 等待与最大暂存 blob。插桩不提交 | 普查报告：每名命中数与定性 | — |
| B | Espryt 生成 mip：B1 存储检查改读计划窗口（`VerbMipBaseLevel` / `LevelCount`、MaxLevel、immutable）；B2 RGB16F / RGB32F 在 server 完成（原生可渲染时 `glGenerateMipmap`，否则 server 从自己的层做 CPU 滤波），删 `cpu-filter` 闩，修 `PipeCatalogueTest` 名单 | 新 F1 用例：R11F_G11F_B10F / 深度 × {MAX_LEVEL 偏小、部分 TexStorage}；RGB16F 仅采样与渲染目标 × 生成 mip；两后端 × split / spawn / tcp | B1 在当前头上 split 预期 `Fatal{UnmigratedEmulation,"generate-mipmap-storage"}`，不红就关 B1；B2 去掉新臂即 Fatal |
| C | Espryt server 暂存影子与 GPU 写对账：被 draw / dispatch 写过的 buffer 标脏，CPU 读者（restart 重写、IndirectCount、原生 indirect 的 baseVertex、MultiDraw 重定基）先做 server 本地回读 | compute 写 EBO + restart 索引；compute 写参数 + `MultiDrawElementsIndirectCount` | 去掉对账即像素错；不红就关 C |
| D | Magma wire 臂原生 `vkCmdDraw*Indirect[Count]`（`DirectVulkan.cpp:375-431`），CPU 展开只留在 monolith 也展开的场合 | create-indirect × Magma × split 每帧 `WaitForWireBufferHostAccess` 次数与毫秒 → 0，create-indirect / instancing SSIM 不变 | 强制 CPU 路径即等待 > 0 |
| E | CopyImage 在 server store 上处理（复制 store 字节，或把目标层标 GPU 脏），不搬到 client；删 `DirectGLES.cpp:13118-13129` 过时注释与死闩 | copy 到 RGB16 / RGBA16_SNORM 后 `glGetTexImage`，强制影子路径开关 | 去掉即陈旧字节 |
| A | 覆盖对齐：(a) 类用例登记到 split / spawn / tcp（tcp fixture 带 ENV）；撤 `ClientSideIndices` 排除；豁免表（机制 / 单后端 / 无记录 / 进程内 peek）；脚本强制归一化后 split ⊆ monolith ∪ 豁免 | 新脚本 rc 0；各 split 车道全绿 | 删一条登记即按名红 |
| F | 重分类：`PipeApplier.cpp:972-978` 改 `ProtocolCorruption`；`WireDraw.inc:297` 去 `@P8`；Magma mip 着色器形状在 split 由 Fatal 改具名拒绝（与 monolith 一致）；`kCapNeedsHost*` / `kCapViewportArray` 标保留位 | `fatal_census` / `PeerLatchTest` 绿 | PeerLatch 仍按名闩住 |
| G | 大 blob（program archive、`draw_vbo` range 尾）：P8-0 测得最大值大于 arena / 4 才做分片 | 测量报告 | — |

## 门（改写，ID-P8-2）

- **覆盖**：归一化前缀后 `DirectGLES.Split.*` ⊆ `DirectGLES.*` ∪ 豁免表（原因类必填），脚本强制；取代"逐名相同"（机制类、单后端、进程内 peek 用例天生只属 monolith，按字面不可达）。
- **create-indirect 往返**：原 trace 只有 1 帧，门是空的；改为多帧设备 trace 上加载帧非 `ResourceCreate` 等待 = 0。
- **trace split 双后端 SSIM ≥ 0.99**（含两个 `coherent_as_flush` fixture），出口再跑。
- **P13 前置**：C、D 先于 P13 落地，否则删掉 monolith 臂后继承 split 臂的缺陷。
- **G1**：pull 构建不变；split 半边在 `MOBILEGL_BUILD_DISAGGREGATED` 下。monolith 半边若 red-once 证实有缺陷，在 dev 上单独修再合并（ID-P8-3）。

## 第二波（2026-09-30，ID-P8-13）

| 包 | 树 / 分支 | 内容 |
|---|---|---|
| SE（split，Espryt + 文档） | `~/w7/p8-se` / `p8/se` | 默认帧缓冲深度在 Espryt spawn / tcp 读 0（A 的 pending-fix 豁免）；B2 的 CPU mip 回退看 E 的 driver-written 标；D 的 case 3 在 Espryt split 臂放开（C 已落地）；`PipeCatalogueTest` 名单去死名 `copy-image-shadow-mirror` 并加"名单 = 调用点"的门；B / C / D / E 包说明的行号漂移 |
| SV（split，Magma） | `~/w7/p8-sv` / `p8/sv` | 3D 纹理原生 mip blit 把 depth 当 `layerCount`（VUID-vkCmdBlitImage-srcImage-00240）；深度模板 mip 拒绝补 `WireDeclineTally` 行；`GenerateWireDepthMipLevel` 拒 1D；wire 臂 `DispatchComputeIndirect` 原生化 |
| MD（dev，设备） | `~/w7/dev-p8-md` / `devfix/p8-md` | create-* 在 Adreno 830 上的三条：Espryt create-instancing monolith SSIM 0.870；Espryt 每次 indirect draw 整 buffer 同步回读（设备上挂死 > 10 分钟）；Magma 计算写与 indirect 读之间无依赖（可能就是 ID-P7-4）。先在 dev 构建上复现，复现才在 dev 上修；只在 feat 构建上出现的交回集成者 |

### monolith 缺陷（ID-P8-3）的取舍

12 条（13 条报告，C、D 重复一条）都在 monolith 专属臂里；P13 把 monolith 换到第一波已修好的记录臂上，它们随之消失。按 P8-0 的真实内容命中取舍：

| 缺陷 | 来源 | 真实内容命中 | 做法 |
|---|---|---|---|
| Espryt create-instancing monolith 画面错（0.870） | S | Create（Adreno） | **MD 修（dev）** |
| Espryt indirect draw 整 buffer 同步回读、设备挂死 | S | Create（Adreno） | **MD 修（dev）** |
| Magma 计算写 → indirect 读无依赖 | D | Create（推断，ID-P7-4） | **MD 修（dev）** |
| Espryt `*IndirectCount` / 原生 indirect `gl_BaseVertex` / fp64 收窄读未同步影子 | C | 0 | 留给 P13 |
| Magma `glMultiDrawArraysIndirectCount` CPU 臂读未同步影子 | C、D | 0 | 留给 P13 |
| Magma 录制中途 `SyncGpuWrites` → SIGSEGV | C | 0 | 留给 P13 |
| Espryt RGB16F / RGB32F CPU 滤波读陈旧影子；R11F / 深度链无视 BASE / MAX | B | 0 | 留给 P13 |
| Magma 无 BLIT 颜色格式缺着色器 mip 臂 | B | 0 | 留给 P13 |
| Espryt 拷贝后 `glGetTexImage` 读拷贝前影子 | E | 0 | 留给 P13 |

## 收官（2026-09-30，ID-P8-19）

两波全部合入；第二波另加 MF（feat 的 push 阶梯镜像 dev 修复）与出口设备检查 X。结果见 [`README.md`](README.md)。
