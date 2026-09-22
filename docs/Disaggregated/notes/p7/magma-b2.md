# P7 wave 2-B2：Magma blit / copy / mip 簇里包 B 没有走完的部分（分支 `p7/magma-b2`）

> 规范见 [`CONTRACT-P7.md`](../../../../MobileGL/MG_Remote/CONTRACT-P7.md) §0（规则 I / J）、
> §3.2（`multisample-blit-shape` / `-aspect` 行）、§4.2（棘轮目标与 (B′) 路线）、§5.2；
> 前一包见 [`magma-b.md`](magma-b.md)。基线 = `feat/disaggregated` 上 `~/w7/pipe` 的头
> `ac8065a3`（X1、F1、C、A、B 全部落地）。
>
> 主机口径与包 B 相同：WSL Arch + lavapipe（`/usr/share/vulkan/icd.d/lvp_icd.json`），
> `build-split` = Release / clang / ccache / `DISAGGREGATED=ON` `INPROC=ON` `PIPE_PUSH=ON`
> `BUILD_INTEGRATION_TEST=ON`。**本树的 TCP 端口是 `40917`**（`MOBILEGL_ITEST_TCP_ENDPOINT`）：
> 包 B 用 40913、pipe 用默认的 40613，兄弟树同时在跑时端口会撞，`TcpServer.Start` 整条车道红。
>
> 基线门读数（本树实测，非引自包 B）：`integration-magma-split` **88**、`-spawn` **67**、
> `-tcp` **69**、`integration-magma-full-split` **521**、`ctest -L unit` **2420**，全绿。
> （包 B 的报告写的是 84 / 63 / 65；差的四条是包 A 在它之后落地的。）

## 0. 每片一条提交，每片至少一次 red-once

| 片 | 名 | 退役 / decline | 提交 |
|---|---|---|---|
| 1 | `multisample-blit-shape@P7` | 缩放/翻转的多重采样颜色 blit **退役**；多重采样目的地 **decline** | `c77c4f29` |
| 2 | `multisample-blit-aspect@P7` + 烘焙 (A) 第三对 | 无 resolve 扩展的深/模板 **退役**（烘焙 pass）；改采样数 **decline** | `aba21e26` |
| 3 | `MagmaP7AllocatorDebtScope` ×2 | **删除**（先证明它们在非 monolith 传输下从不武装） | `4bee1313` |
| 4 | (B′)：棘轮的 `p7-magma` 桶 | monolith 臂在 disaggregated 构建里改走烘焙模块 | 见 §4 |

**`grep -rn "@P7" MobileGL/ --include=*.cpp --include=*.inc --include=*.h` 在片 2 之后只剩注释**
（19 行，全部是「某某退役了」或 red-once 的散文）。树上没有任何一条拒绝再带这个标记。

---

## 1. 片 1：`multisample-blit-shape@P7`

### 1.1 一个名字下面的两半，问题不是同一个

**有臂的那一半**：多重采样颜色 blit 的目的矩形与源矩形尺寸不同。`vkCmdResolveImage` 每侧只带
**一个 extent 和一个 offset**，既表达不了缩放也表达不了翻转——而这条臂**本来就**为了翻转先
resolve 进一张单采样 scratch、再对 scratch 做 `vkCmdBlitImage`。两个 `width != |dx1-dx0|` 子句
是唯一把缩放挡在同一条路线之外的东西，而 monolith 臂走的正是这条路线
（`VulkanRenderer.cpp` 的 `regionWasTransformed` 分支）。子句去掉；scratch 仍按**源**矩形定尺寸，
所以只有第二段看得见缩放。

**顺带的一件事**：filter 是**从这次缩放起才有意义的**——1:1 时 LINEAR 与 NEAREST 读同一个 texel，
所以在此之前没有人问过这个格式能不能被过滤。对 `optimalTilingFeatures` 缺
`SAMPLED_IMAGE_FILTER_LINEAR` 的格式传 `VK_FILTER_LINEAR` 是未定义行为；GL 本来也说多重采样
resolve 忽略 filter，所以回落是 NEAREST 而不是拒绝。

**没有臂的那一半**：**多重采样目的地**。`vkCmdCopyImage` 是唯一能写多重采样图像的命令，它只带一个
extent，而 GL 4.6 core 18.3.1 说任一侧多重采样时两个矩形尺寸不同就是 `INVALID_OPERATION`。
没有「正确结果」可言，所以按规则 I (a)：`MGLOG_E_ONCE` 写出两个矩形与两个采样数、记 GL 错、
目的地原样不动、会话留下。

### 1.2 场景与车道

`F1WireScenario.MultisampleColorBlitScalesAndFlipsThroughTheResolveScratch` 与
`…MultisampleBlitOntoAMultisampleDestinationDeclinesAndKeepsTheSession`，三臂各两条，前缀 `MsBlit.`。

**split-only**，理由和包 B 的 in-place copy 一样、而且更强：monolith 臂对缩放的多重采样 blit 走
`vkCmdResolveImage` 1:1 落在目的偏移上——**缩放被丢掉**——所以没有可对照的 `DirectVulkan.` 读数，
两臂相等的断言等于断言 wire 臂也一样错。

**缩放与翻转在同一条用例里、同一个源上**，因为它们坏的方式不同：丢了缩放会把 8×4 的带子塞进 4×2 的
附件（驱动直接拒命令），丢了翻转会把对的两个颜色按错的顺序返回。第二次 blit 的期望就是第一次的
期望把两行调过来，所以谁也通不过对方的断言。

### 1.3 测出来的一件事：decline 的 GL 错**晚到**

decline 在**执行 blit 的地方**记错，也就是 server 的 apply 线程，而那个错是**跟着后一次 reply**
回到 client 队列的，不是跟着 blit 调用。第一次跑的时候它挂在二十行之下那次 1:1 resolve 上——
一次没有任何毛病的调用。用例里的 `glFinish()` 因此是承重的，不是礼貌。

### 1.4 red-once（R-16，两条，均已执行并还原）

1. 把两个尺寸相等子句放回去 → `…ScalesAndFlipsThroughTheResolveScratch` 死于
   `Fatal{UnmigratedVerb, "Magma:multisample-resolve-region"}`；
2. 把 `MagmaWireFatal("multisample-blit-shape@P7")` 放回去 → `…DeclinesAndKeepsTheSession`
   死于该名。

还原后三臂 6/6 绿。

### 1.5 门

`integration-magma-split` **90**（+2）、`-spawn` **69**（+2）、`-tcp` **71**（+2）、
`integration-magma-full-split` **523**（+2）、`ctest -L unit` **2420**，全绿；
`fatal_census` 79 abort 站点 / 44 家族词 / 0 无标记；`link_ratchet --assert-monotone` 186 不变；
G1 pull `.text` **`0xa52203`**、`nm --defined-only` 与 `~/w7/p7-before/pull-syms.txt` 逐行相同
（30570 行，0 差异）。

---

## 2. 片 2：`multisample-blit-aspect@P7` 与第三对烘焙模块

### 2.1 `vkCmdResolveImage` 只做颜色

Vulkan 里不用 shader 把深度或模板 aspect resolve 下来的**唯一**办法，是带 depth/stencil resolve
附件的 render pass，它要 `VK_KHR_create_renderpass2` 和 `VK_KHR_depth_stencil_resolve`。两个都没有
的设备，在应用第一次
`glBlitFramebuffer(GL_DEPTH_BUFFER_BIT)`（**这就是 GL 里每一次多重采样深度 resolve**）时会死于
`Magma:multisample-depth-resolve-capability`。§3.2 的 aspect 行点了两条臂的名，这一片补上第二条。

`WireMultisampleResolve.inc` + 三个烘焙模块（一个顶点阶段，每个 aspect 一个片元阶段），取
**样本零**——和扩展臂要的 `VK_RESOLVE_MODE_SAMPLE_ZERO_BIT` 同一个样本，所以两条臂在同一台设备上
互换不会改变像素，车道也才断言得动。GL 4.6 core 18.3.1 对模板本来就要求**选一个**样本，对深度要求
结果落在该像素的 min/max 之间——样本零两边都满足，而平均**不满足**后者的精神（四个深度的均值是一个
哪里都不存在的表面）。

**这条 pass 什么都不带。** scratch 与源同尺寸、同坐标系（两张都是原生图像，彼此不翻转），所以片元
阶段直接用 `gl_FragCoord` 取址，矩形交给 scissor。没有 push constant，没有 varying，没有前端对象。

**模板那一半要扩展才存在**：片元着色器没有 `VK_EXT_shader_stencil_export` 根本写不了模板 aspect。
没有它就 decline 模板（§3.2 原话），而且**模块本身不创建**——把带不支持 capability 的 SPIR-V 交给
`vkCreateShaderModule` 是未定义行为，不是一个能接住的错误。

**两个能力判据不是同一个判据**，差别是结构性的：subpass resolve 的是**格式**拥有的每一个 aspect，
所以每个都要有 mode；而上面这条 pass 只写这次 blit 要的那一个 aspect，scratch 其余部分留成未定义
——安全，因为随后的 copy 读的正是那一个 aspect。

### 2.2 没有臂的那一半

采样数不同、且**目的地**是多重采样的那一侧：一个样本铺成 N 个，或 N 个降成 M 个。
`vkCmdResolveImage` 只写单采样目的地，`vkCmdBlitImage` 两侧都拒绝多重采样，`vkCmdCopyImage` 要求
两个数相等。decline。

### 2.3 场景与车道

**用例是提拔来的，不是新写的。** `DepthStencilReadbackMatrixScenario.AResolvedMultisampleDepth…`
本来就拥有这个可观测（清一张 4× `DEPTH24_STENCIL8`、用 `glBlitFramebuffer` resolve 进单采样、两个
aspect 都读回、并检查只做深度的 resolve 没动目的地的模板、反之亦然），而且**本来就在三条 Magma 臂
的信息档（`…Full.`）里跑**。缺的只是一条 gating 条目。

每臂两条（`MsResolve0.` / `MsResolve1.`），第二条是它那条臂在此唯一可达的方式：lavapipe 两个 resolve
扩展都有，扩展臂会接下本机的每一次 resolve。`MGITEST_MAGMA_FORCE_SHADER_DEPTH_RESOLVE` 丢掉扩展臂。
**它在本机不 skip**：lavapipe 也带 `VK_EXT_shader_stencil_export`，所以 shader 臂深度和模板都走通了。
新增的 F1 用例是那条 decline。

> 集成者交代里写的「可能在 lavapipe 上 SKIP（分离的 D24/S8 不支持）」本树**没有发生**：
> 提拔的这条用例用的是**合并**的 `DEPTH24_STENCIL8`，分离 D24/S8 是同一文件里另一条用例
> （`SeparateDepthAndStencilAttachmentsAreBothReadable`）的事，没有提拔。三臂 6/6 绿。

`MOBILEGL_BAKED_INTERNAL_SHADERS` 的表从 8 行涨到 **11 行**。

### 2.4 red-once（R-16，三条，均已执行并还原）

1. **shader 臂**：把 `MagmaWireFatal("multisample-depth-resolve-capability")` 放回非扩展臂 →
   `DirectVulkan.Split.MsResolve1.…` 死于该名，而 `MsResolve0`（扩展臂）**仍然绿**——这同时是旋钮
   确实改道的证明；
2. **aspect decline**：把 `MagmaWireFatal("multisample-blit-aspect@P7")` 放回去 →
   `DirectVulkan.Split.MsBlit.…FromASingleSampleSourceDeclines…` 死于该名；
3. **新鲜度门**：把 `WireMultisampleStencilResolve.frag` 的取样索引从 0 改成 1（**不需要重建**——
   门在运行时读源）→ `WireMultisampleStencilResolveFragment` 在**第 158 个字**变红，并指名符号、
   头文件与重新生成的命令。

还原后三臂 15/15 绿、新鲜度门 11/11 绿。

### 2.5 门

`integration-magma-split` **93**（+3）、`-spawn` **72**（+3）、`-tcp` **74**（+3）、
`integration-magma-full-split` **524**（+1）、`ctest -L unit` **2423**（+3），全绿；
`bake_internal_shaders.py --check` 五个头全新鲜；`fatal_census` 79 / 44 / 0；
`link_ratchet --assert-monotone` 186 不变；G1 `.text` `0xa52203`、符号表逐行相同。

### 2.6 欠设备核对

shader resolve 臂正是**手机**在没有 `VK_KHR_depth_stencil_resolve` 时会走的那条，而本机只能经旋钮
到达它。它的几何要在 Redmi 上看一眼。

---

## 3. 片 3：两处 `MagmaP7AllocatorDebtScope`

### 3.1 债是真的，但它早就没有对象了

隐藏的 blit 程序、它的两个采样器和隐藏的深度 mip 程序都是**前端对象**。在活动传输下，server 后端在
apply 线程上创建它们、也在 apply 线程上销毁它们，而它们的析构函数会跑 client 死亡助手和那个
lifetime-id 探针——所以 P5c 给这两处 teardown 一个具名豁免，P5e 裁定 12 把它按债重命名。

**退役它的不是一个新豁免，而是对象不存在。** P5f fv 早已给 `InitializeBlitResources` 和
`InitializeDepthMipmapResources` 加了非 monolith 传输的 early return，所以在 monolith 臂之外
`m_blitResources` 从不持有任何东西，`= {}` 什么也不销毁。这两个 scope 自那天起就是罩在一条跑不到的
路径上的标记。

### 3.2 证明，不是论证

两处 teardown 里放一个临时探针：在非 monolith 传输下任一资源非空就按名结束会话。武装着跑：
`integration-magma-split` 93、`-spawn` 72、`-tcp` 74、`integration-magma-full-split` 524、
`-full-spawn` 524、`integration-split` 187、`integration-magma-buffers` 21，**全绿，它一次都没响**。

**负对照才让这成为测量**：把探针的传输判据去掉，**每一条 monolith `DirectVulkan.` 条目都在 teardown
时死了**（8/8 abort，而且用例本身先 PASS 了），split 条目照样绿。

`integration-magma-full-tcp` 在探针下的那一条红是 CONTRACT-P7 §2.5 记过名的 StreamLink × Magma
曝光 texel（`IterationRPProgram203Scenario`），不是探针的死亡——它 Failed，不是 aborted。

### 3.3 对着 P5e 裁定 12 的「四笔债」重数

裁定 12 点了**四**处 Magma 站点（`VulkanRenderer.cpp` ×3、`ResourceTracker.h` ×1）。其中两处在 P7
开始前就没了——`git log -S` 把 `ResourceTracker.h` 那处记在 P5f fv（`4f3d2d6b`）、第三处
`VulkanRenderer.cpp` 记在 P5f fm（`036c3a3d`）——所以 P7 基线 `78b7d6be` 上只剩**两**处，就是本片
删掉的这两处。Magma 的 apply 线程分配器债现在是**零**处。

### 3.4 类型留下，并且写明了为什么

`MG_Test/Wire/RemoteClientTest` 的 `RemoteGuards.BarrieredLegacyScopesCannotExemptAllocator` 与
`.BarrieredFrontendRegistryMembersRefuseBothLegacyScopes` 拿它当**负对照**：在 apply 线程上构造它，
然后断言分配器**照样**拒绝。那是 P5f (fr) 立下的性质，删掉类就删掉了唯一钉住它的用例。
后端已无任何使用点。

---

## 4. 片 4：(B′)，棘轮的 `p7-magma` 桶

### 4.1 做了什么

在 **disaggregated 构建**里，monolith 臂也改走烘焙模块：

- `InitializeBlitResources` / `InitializeDepthMipmapResources` → `return true`（无事可做）；
- `TryBlitToDefaultFramebufferWithShader` → 把两个 `BlitImageBinding` 转成 `WireImage` 后调
  `BlitWireColorToDefault`（`WireColorBlit.inc`）；
- `GenerateDepthMipmapWithShader` → 逐级调 `GenerateWireDepthMipLevel`（`WireDepthMipmap.inc`），
  收尾一次 `TransitionWireImage` 把图像交回调用方承诺的 `finalLayout`；
- `GetOrCreateBlitPipeline`、三段 GLSL 文本、八个 `kHidden*` id、`BlitResources` /
  `DepthMipmapResources` 的前端成员：`#if !MOBILEGL_BUILD_DISAGGREGATED`。

**为什么是 `#if` 而不是运行时分支**，这正是 §4.2 的全部内容：P5f fv 的 early return 让符号**仍然被
引用**，棘轮不管那条分支跑不跑。把构造编译掉才让它们掉下来。而**成员也得走**，不只是构造：
`SharedPtr` 成员的析构本身就是一次对 `~ProgramObject` 的引用。

`#else` 分支逐语句原样，G1 钉住的就是它。

### 4.2 桶表：前 / 后

| bucket | before | after | a6 | 含义 |
|---|---:|---:|---:|---|
| `p6-core` | 11 | 11 | 6 | 没有后端对象引用：传输/applier 自己的账 |
| **`p7-magma`** | **101** | **88** | 101 | 只被 DirectVulkan 引用：P7 的账 |
| `p3b-p4b-espryt` | 13 | 18 | 14 | 只被 DirectGLES 引用 |
| `both-backends` | 61 | 56 | 60 | 两个后端都引用 |
| **合计** | **186** | **173** | 184 | |

`p3b-p4b-espryt` 的 +5 不是变坏：那 5 个符号从 `both-backends` **移**了过去，因为 DirectVulkan 不再
引用它们，只剩 DirectGLES。掉的是 13 个，合计 186 → 173。

**掉下来的 13 个**（`--assert-monotone` 逐条列名，随本片 `--write-baseline` 重基线，7 条
`# P13` 注解原样带过）：

- `ProgramObject::{AllocateLifetimeId, AttachShader(...), Link(bool), ~ProgramObject}`（4）
- `SamplerObject::{SetLodRange, SetWrapR, SetWrapS, SetWrapT}`（4）
- `ShaderObject::{Compile, DropCompileNode, JoinPendingCompile, ReleaseCompileNode, SetShaderSource}`（5）

即 §4.2 点名的那三个前端类的全部构造面。

### 4.3 剩下的 88 个：逐个引用对象点名，以及为什么它们不是 (B′) 的

§4.2 写的「降到 0」**按实测不成立**，而且差距不在 (B′) 做没做完，在于桶里绝大多数符号**根本不是
隐藏程序引来的**。逐引用对象：

| 引用对象 | 数量 | 是什么，为什么还在 |
|---|---:|---|
| `Renderer/UniformManager.cpp.o` | 62 | monolith 描述符绑定器对**应用自己的** GL 纹理对象的遍历：`TextureObjectBase` 35、`TextureObjectWithOneMipmap` 20（含其 vtable / typeinfo）、四个 `TextureObject{1D,2D,2DCube,3D}` 各 1、`MGPipeNoteAggregate` 1。split 下 `SetupDraw` 分支到 `SetupWireDraw`，这条路一行都不跑——但那是**运行时**分支（ID-P7-5 明确把它留在优化阶段），所以符号仍被引用。清它要对 `UniformManager` 的 monolith 臂做和 (B′) 同形的 `#if`，是 wave 3 的活，不是本片的 |
| `Renderer/VkSamplerManager.cpp.o` | 9 | `SamplerObject::Get{BorderColor,BorderColorI,BorderColorUI,CompareMode,MinLod,SamplerCompareFunc,WrapR,WrapS,WrapT}`——同上，monolith 采样器缓存读应用的 `SamplerObject` |
| `UniformManager.cpp.o` + `VkSamplerManager.cpp.o` | 5 | `SamplerObject::Get{MagFilter,MaxAnisotropy,MaxLod,MinFilter,MipmapMode}`，同一条路的两个读者 |
| `Renderer/VulkanRenderer.cpp.o` | 3 | `GLImpl::CopyTextureImageToClientOrPBO_State`（monolith 回读打包）、`PipeInputs::HasOpenTransformFeedbackSpan`、`FramebufferAttachmentObject::IsValid` |
| `Renderer/VkTextureManager.cpp.o` | 3 | `PipeInputs::GetTextureObject`、`MGPipeTextureLegacyArmScope::{ctor,dtor}`——legacy 臂的具名 scope，P3b/P4b 的 |
| `BackendObject_DirectVulkan.cpp.o` | 1 | `PipeInputs::InvalidateCompileEnv` |
| `UniformManager.cpp.o` + `VulkanRenderer.cpp.o` | 1 | `BufferObject::EnsureGpuResidentStorage` |
| `VkClearManager` + `VkRenderPassManager` + `VulkanRenderer` | 1 | `FramebufferAttachmentObject::GetSize` |
| `VkRenderPassManager` + `VulkanRenderer` | 1 | `FramebufferAttachmentObject::IsComplete` |
| `Renderer/VkRenderPassManager.cpp.o` | 1 | `RenderbufferObject::IsAllocated` |
| `DirectVulkan.cpp.o` + `VulkanRenderer.cpp.o` | 1 | `VertexArrayObject::GetIndexBufferBindingSlot` |

**没有一个标 `# P13`**，而这是有意的：§4.2 的 `# P13` 是给「不动 pull `.text` 就消不掉」的符号准备
的，上面没有一个证明了这一点——`UniformManager` / `VkSamplerManager` 的 monolith 臂是**pull 构建
自己的代码**，对它们做 `#if MOBILEGL_BUILD_DISAGGREGATED` 与本片对 `VulkanRenderer.cpp` 做的是同一件
事，pull 构建逐字节不动。它们是 **wave 3 的工作量**，不是 P13 的边界问题。把它们提前标成 P13 会把一
件没人试过的事记成一件试过做不到的事。

### 4.4 门

`integration-gpu` **1705**（monolith `DirectVulkan.` 条目都在里面）、`integration-magma-split`
**93**、`-spawn` **72**、`-tcp` **74**、`integration-magma-full-split` **524**、
`ctest -L unit` **2423**，全绿。

`link_ratchet --assert-monotone` 对**重基线后**的文件：173 不变，RC 0。

**retrace（本片的关键读数，因为 monolith 臂在 disaggregated 构建里换了实现）**：

| case | ssim | mismatchPixels |
|---|---:|---:|
| `OpenRA.DirectVulkan`（monolith） | **1.000000** | 0 |
| `OpenRA.DirectVulkan.SPLIT` | **1.000000** | 0 |

整条 DirectVulkan 矩阵：monolith 39/40 绿、SPLIT 38/39 绿，两边红的是**同一条**
`minecraft-1.21.4-fabric-iris-iterationrp-in-world`（ssim 0.027915）。**已核实是既有的**：在包 B 的
树（`~/w7/p7-magma-b/build-retrace`，本分支的父提交）上同一条用例同样红。它要 CONTRACT-P7 §2.4 说的
三个 iterationRP 旋钮，而那三个是 CI 作业导出的、不是 ctest 条目自带的。

G1 pull `.text` **`0xa52203`**、`nm --defined-only` 与 `~/w7/p7-before/pull-syms.txt` 逐行相同
（30570 行，0 差异）——本片改的每一行要么在 `#if MOBILEGL_BUILD_DISAGGREGATED` 里，要么在
`#if !MOBILEGL_BUILD_DISAGGREGATED` 里而那一侧逐语句未动。

### 4.5 欠设备核对

monolith 臂在 **Android 的 disaggregated 包**里现在跑的是烘焙 blit。旋转（`ROTATE_90/180/270`）的
几何只有真机能看：`BlitWireColorImage` 的 `quarterTurn` 归一化与它替换掉的
`TryBlitToDefaultFramebufferWithShader` 的 `dstNormWidth/dstNormHeight` 是同一个算式，但本机每一个
表面都是 `IDENTITY`，这条等式没有被执行过。

---

## 5. 要手机的清单（本包全部的设备债）

1. **片 1**：旋转表面上的缩放多重采样 blit（本机只有 identity）。
2. **片 2**：没有 `VK_KHR_depth_stencil_resolve` 的设备上的 shader 深/模板 resolve——本机只能经
   `MGITEST_MAGMA_FORCE_SHADER_DEPTH_RESOLVE` 到达。Adreno 830 是否带这两个扩展本身也要看一眼：
   带，则这条臂在真机上同样只有旋钮可达；不带，则它是真机的**默认**臂。
3. **片 4**：`ROTATE_90/180/270` 下 monolith 臂的烘焙 blit 几何（§4.5）。
4. 包 B 遗留、本包未碰：非恒等旋转下的 `default-color-blit-shape` scratch 臂，以及
   OpenRA 三次 `pm clear` 冷跑的 1.0 验收。
