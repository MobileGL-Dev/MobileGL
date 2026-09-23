# P7 wave 2-B2：Magma blit / copy / mip 簇里包 B 没有走完的部分（分支 `p7/magma-b2`）

> 规范见 [`CONTRACT-P7.md`](../../../../MobileGL/MG_Remote/CONTRACT-P7.md) §0（规则 I / J）、
> §3.2（`multisample-blit-shape` / `-aspect` 行）、§4.2（棘轮目标与 (B′) 路线）、§5.2；
> 前一包见 [`magma-b.md`](magma-b.md)。基线 = `feat/disaggregated` 上 `~/w7/pipe` 的头
> `09ba88e7`（X1、F1、C、A、B 全部落地）。
>
> 主机口径与包 B 相同：WSL Arch + lavapipe（`/usr/share/vulkan/icd.d/lvp_icd.json`），
> `build-split` = Release / clang / ccache / `DISAGGREGATED=ON` `INPROC=ON` `PIPE_PUSH=ON`
> `BUILD_INTEGRATION_TEST=ON`。**本树的 TCP 端口是 `40917`**（`MOBILEGL_ITEST_TCP_ENDPOINT`）：
> 包 B 用 40913、pipe 用默认的 40613，兄弟树同时在跑时端口会撞，`TcpServer.Start` 整条车道红。
>
> 基线门读数（本树实测，非引自包 B）：`integration-magma-split` **88**、`-spawn` **67**、
> `-tcp` **69**、`integration-magma-full-split` **521**、`ctest -L unit` **2420**，全绿。
> 包 B 的报告写的是 84 / 63 / 65，差的四条**不是**「包 A 在它之后落地」——那句是错的，
> 审查round 已更正：包 A 在 B **之前**落地（`450d3075..2207b6d9` 在 `c4be3800` 之前）且只有 +2
> （`magma-a.md:151-153`），另外 +2 是包 C（`magma-c.md:59-61`）。本树的基线因此是
> 73 + 11（B）+ 2（A）+ 2（C）= **88 / 67 / 69**。

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

`MsResolve0.` 在三臂都注册；`MsResolve1.` **只在 split 和 spawn**（审查 round 的修正，见 §2.7）。
第二条是那条臂在此唯一可达的方式：lavapipe 两个 resolve 扩展都有，扩展臂会接下本机的每一次
resolve。`MGITEST_MAGMA_FORCE_SHADER_DEPTH_RESOLVE` 丢掉扩展臂。
**它在本机不 skip**：lavapipe 也带 `VK_EXT_shader_stencil_export`，所以 shader 臂深度和模板都走通了。
新增的 F1 用例是那条 decline。

> 集成者交代里写的「可能在 lavapipe 上 SKIP（分离的 D24/S8 不支持）」本树**没有发生**：
> 提拔的这条用例用的是**合并**的 `DEPTH24_STENCIL8`，分离 D24/S8 是同一文件里另一条用例
> （`SeparateDepthAndStencilAttachmentsAreBothReadable`）的事，没有提拔。

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

> 上面这三个 `+3` 是**片 2 落地当天**的读数，那时 `MsResolve1.` 还注册在 tcp 上。审查 round
> 把它从 tcp 摘掉了（§2.7），tcp 的最终读数在 §7。

### 2.6 欠设备核对

shader resolve 臂正是**手机**在没有 `VK_KHR_depth_stencil_resolve` 时会走的那条，而本机只能经旋钮
到达它。它的几何要在 Redmi 上看一眼。

**`MsFlip.` 在格式分叉的设备上会红**（审查 round 2 补记）：场景的多重采样源是 **renderbuffer**
`DEPTH24_STENCIL8`，目的地是 **texture** `DEPTH24_STENCIL8`。本机两者都映射到
`VK_FORMAT_D24_UNORM_S8_UINT`，所以 Y 翻转走同格式的逐行 copy-out。一台把 renderbuffer 与 texture 的
D24S8 映射到**不同** `VkFormat` 的设备（例如 texture 侧退到 `D32_SFLOAT_S8_UINT`）会在
`DeclineWireDepthStencilResolveShape` 的**跨格式 Y 镜像** decline 上拿到 `INVALID_OPERATION`，
`MsFlip.` / `MsFlip1.` 因此变红——那不是回归，是 §6 第 5 条那笔债在该设备上可见了。上 Redmi 前先看它的
两种映射是否相同。

### 2.7 审查 round：一个 server 端旋钮**没有** tcp 臂

`MGITEST_MAGMA_FORCE_*` 三个旋钮都由 **server** 读（`WireFramebuffer.inc` 的三个 `std::getenv`
跑在 apply 线程上），而 ctest 的 `ENVIRONMENT` 属性只到达 ctest 启动的那个进程，也就是 **client**。
三条臂的 server 来处不同：

| 臂 | server 从哪来 | 旋钮到得了吗 |
|---|---|---|
| inproc | 同进程的一个线程 | 到 |
| spawn | `ServerSpawn.cpp:106` 把 `::environ` 复制进子进程 | 到 |
| tcp | **车道级** `TcpServer.Start` fixture，由 `scripts/ci/tcp_server_fixture.py` 用**它自己的** `os.environ` 在任何用例之前启动一次，每个 Hello fork 一个会话子进程 | **到不了** |

所以一条 tcp 的旋钮条目跑的是**默认**臂，名字却宣称不是——对 ShaderMip 而言，这正是那条用例自己的
skip 文案说不许发生的事（「would assert the native blit twice over」）。

**实测，不是推导**：在三个 reader 里各放一个临时 fatal，五条 knob=1 条目在 split **全死**、在
spawn **全死**、在 tcp **五条全绿**；五条 knob=0 的孪生条目三臂全绿。

修法用的是 ID-P7-14 `MAGMA_INPROC_ONLY` 的先例：`scripts/ci/spawn_lane_parity.py` 新增
`MAGMA_SERVER_ENV_KNOB_NO_TCP`，并给 `compare_arms` 加一个 `no_tcp=` 参数。它和 `inproc_only=`
**不是**同一个形状，这一点是承重的：`inproc_only` 把键从**每一条**非 split 臂的比较里拿掉，而这五条
必须在 spawn 上**仍然被要求**——spawn 才是它们干活最多的地方。受影响的五条是
`.ShaderMip1.` `.ShaderMip2.` `.DepthMip.` `.DefaultBlitShape1.` `.MsResolve1.`；knob=0 的
`.DefaultBlitShape0.` / `.MsResolve0.` 留在三臂，因为零就是默认值，那两条在三臂含义相同。

其中三条（`ShaderMip1/2`、`DepthMip`、`DefaultBlitShape1`）是**包 B 既有的**同种空转，一并修。

**这是一笔债而不是一个结论**：要让 tcp 车道也能带这些条目，fixture 得接受**每用例的 server 环境**
——那是一个 supervisor 控制项，不是一个 ctest 属性。记在 §6。

### 2.8 审查 round：翻转和缩放的深/模板 resolve 不再带走会话

`ResolveWireDepthStencil` 的区域判据要求 `sx1 > sx0` 且 `sy1 > sy0`，否则
`Magma:multisample-depth-resolve-region` 带走会话。于是

```
glBlitFramebuffer(0, h, w, 0,  0, 0, w, h,  GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT)
```

从一个多重采样 framebuffer 出发时，**颜色 blit 正确完成**（颜色臂自 P5f 起就会翻转，本包又给了它
缩放），**然后死在深度上**。GL 4.6 core 18.3.1 要求的是**尺寸**相同而不是角点相同（Mesa 比的是
`abs()`），所以翻转是一次合法的 blit，欠一张镜像的图。

拆成三块：

1. **缩放** → 规则 I (a) 具名 decline。18.3.1 说尺寸不同就是 `INVALID_OPERATION`，没有「正确结果」。
2. **Y 翻转、同格式** → **退役**：copy-out 那一步本来就是「image → buffer → image」，而 buffer copy
   是按**行**寻址的，所以每行一个 `VkBufferImageCopy`、倒着走，就是一次垂直镜像。（round 1 让
   `bufferRowLength` 保持 0、行紧贴；那违反深/模板 copy 的 4 字节 `bufferOffset` 对齐，round 2 把行距
   补齐到 4 字节，见 §2.9。）
3. **X 翻转**，以及**跨格式的 Y 翻转** → 具名 decline。横向镜像要一个 region 一个 **texel**，那已经
   不是 copy 了；跨格式重编码走 `BlitDepthAcrossFormats`（与 monolith 臂共用），它也没有翻转。
   两条都进 §6。
   （round 1 的这两条 decline 与第 1 条的缩放 decline 都是在颜色 aspect **已经写完之后**才决定的——
   一次部分生效，GL 不允许。round 2 把决定提到任何 aspect 之前，见 §2.9 第 3 条。）

span 从此是**有符号**的，下游一律用 min 角（`sxMin/syMin/dxMin/dyMin`），render pass 的 renderArea、
shader 臂的 scissor、buffer copy 的 imageOffset、`BlitDepthAcrossFormats` 的四个角全部跟着改。

**场景**：`DepthStencilReadbackMatrixScenario.AFlippedMultisampleResolveMirrorsTheBandsAndAScaleDeclines`，
三臂各一条，前缀 `MsFlip.`，**不需要旋钮**（两种形状都在能力问题之前由矩形决定）。
**颜色和深度在同一次调用里**，因为那正是让这件事显形的形状；两条带子深度不等（下 0.25 / 上 0.75），
并且颜色也断言了同一个方向——一次「假翻转」（比如只是把读回顺序倒过来）会让颜色和深度不一致。

**red-once（R-16，三条，均已执行并还原，split + spawn 各一遍；审查 round 的第二个会话又亲手各跑了一遍）**：

1. 把旧判据放回去（`spanX <= 0 || spanY <= 0 || spanX != dstSpanX || spanY != dstSpanY` → Fatal）
   → 两臂都死于 `Fatal{UnmigratedVerb, "Magma:multisample-depth-resolve-region"}`；
2. 只把**缩放**那条 decline 换回旧 Fatal（翻转路径不动）→ 翻转那一半通过，两臂都在缩放那一步死于
   同一个名字——证明缩放 decline 是承重的，而不是被翻转那条红顺带盖住；
3. 把 copy-out 的 `if (!mirrorY)` 改成恒真（即拿掉逐行倒序，其余一律不动）→ 两臂都**活着**但深度
   带子上下颠倒：`flipped resolve: the destination's bottom band: 768 of 768 depth values differ
   from 0.75; first bad value 0.25`（顶带反之）。这一条才是对**新代码**的断言——它证明那张镜像的图
   是逐行倒序产生的，不是别处的副作用。

### 2.9 审查 round 2：在哪些臂上断言、行距、以及「一次调用一个答案」

**1. 用例只在 Magma wire 臂上断言。** `DepthStencilReadbackMatrixScenario` 整套被 monolith
`DirectVulkan.` 臂和每一条 DirectGLES 臂发现，于是 round 1 的新用例在它们上面全红：monolith 拒绝
翻转的深度 blit（`VulkanRenderer.cpp:9594`「depth blits with flipped rectangles are not supported
yet」，目的地保持清屏值、无错误）并**缩放**一次 GL 说是 `INVALID_OPERATION` 的多重采样深度 blit
（`:9608`）；Espryt（monolith 与 split 一样）翻转时什么也不写、也不报错，缩放时也不报错。用例先按
`Gl().BackendName() != "DirectVulkan"` 跳过（Espryt 文案，P3b/P4b），再按
`SplitLane::IsSplitLane()` 跳过（monolith 文案，P13/G1）。实测：
`DirectVulkan.DepthStencilReadbackMatrixScenario.AFlipped…` 与
`DirectGLES.{,ForcedDepthStencilEmulation.}DepthStencilReadbackMatrixScenario.AFlipped…` 均为
`Skipped`，理由即上；三条 `DirectVulkan.{Split,Spawn,Tcp}.MsFlip.` 仍然武装并通过。两笔差异进 §6
第 6、7 条。（行号是落地树上的。）
**round 3 修正**：第二道门改按**进程解析到的传输**跳过（§2.10 第 1 条），因为 `MGITEST_SPLIT_LANE`
标的是精选车道而不是传输——`DirectVulkan.{Split,Spawn,Tcp}.Full.` 与 `DirectVulkan.VerifySplit.`
故意不带它。现在**运行**这条用例的条目是：`DirectVulkan.{Split,Spawn}.{MsFlip,MsFlip1}.`、
`DirectVulkan.Tcp.MsFlip.`、`DirectVulkan.{Split,Spawn,Tcp}.Full.`（三条由 Skipped 变 Passed）、以及
落地树上的 `DirectVulkan.VerifySplit.`；仍 `Skipped` 的只有 monolith `DirectVulkan.` 与 DirectGLES 条目。

**2. copy-out 的行距补齐到 4 字节。** 深/模板目的地的 buffer→image copy 要求每个 `bufferOffset`
是 4 的倍数（`VUID-vkCmdCopyBufferToImage-dstImage-07978`，旧层叫 `pRegions-07978`）。§2.8 的逐行
镜像把第 r 行放在 `r * width * texelSize`：模板 aspect（1 字节）宽度不是 4 的倍数、或 D16（2 字节）
宽度为奇数时，第一行之后的每一行都违规——而 `BlitWireFramebuffers` 的 scissor 裁剪恰好产生任意宽度。
改为 `paddedWidth = AlignUp(width, 4 / texelSize)`，buffer 取 `paddedWidth * height * texelSize`，
两次 copy 都以 `paddedWidth` 作 `bufferRowLength`，第 r 行从 `r * paddedWidth * texelSize` 开始。
场景加一条**模板**腿：源的模板带 11 / 99，一次 29×32、从 (5, 8) 到 (20, 4) 的翻转
`GL_STENCIL_BUFFER_BIT` blit，目的地每个模板值都按镜像带或原封的 3 核对。

red-once（split + spawn，`VK_LOADER_LAYERS_ENABLE=*khronos_validation`）：lavapipe 不强制这条 VU，
未补齐的代码读回的模板字节**是对的**，所以红的是校验层——每臂 10 条
`pRegions[1].bufferOffset (29) must be a multiple 4 if using a depth/stencil format
(VK_FORMAT_D24_UNORM_S8_UINT)`；补齐之后校验层**零**错误（任何 VUID）。

**3. 一次调用一个答案。** 颜色 lambda 先跑，所以 round 1 的形状 decline 是**逐 aspect** 决定的：
一次缩放的 `COLOR|DEPTH` 多重采样 blit 先把颜色缩放写进去，**然后**在深度上记
`INVALID_OPERATION`；一次 X 镜像的则镜像了颜色、留下深度不动、再报一个 GL 本不会报的错。18.3.1
说出错的 blit 什么也不写。修法（本 round 做了，不是记债）：形状判据搬进
`DeclineWireDepthStencilResolveShape`，`BlitWireFramebuffers` 在**任何 aspect 之前**对 mask 里每个
会走多重采样 resolve 臂的深/模板 aspect 问一次，decline 则整个调用返回；`ResolveWireDepthStencil`
为其它调用者也问一次。默认 read framebuffer 留给 lambda（它从不是多重采样）。round 2 把默认 **draw**
framebuffer 也留给了 lambda，理由是「默认 draw 在多重采样 read 后面本就是 region Fatal」——那句是
错的，round 3 改正（§2.10 第 2 条）：默认 draw 与用户 framebuffer 一样进预检，以 `isWriteTarget=false`
解析，写索引不动。
**这句「整个调用什么也不写」只对形状 decline 成立**（round 3 收窄措辞）：`ResolveWireDepthStencil`
里的**能力** decline（无 `VK_KHR_depth_stencil_resolve` 也无 `VK_EXT_shader_stencil_export` 的设备上
的 stencil-only resolve）仍是逐 aspect 决定的，一次 `COLOR|STENCIL` 调用在颜色写完之后才 decline
模板；规则 I (a) 接受这一点，见 §2.10 第 3 条。
顺带：X 与跨格式 Y 两种 decline 各有自己的 `MGLOG_E_ONCE`（原来共用一个，首次 X 之后跨格式 Y 不再
留痕）；只有一侧为空的矩形（64×48 → 0×48）走缩放 decline 报 `INVALID_OPERATION`，不再静默返回。

**4. `MsFlip1.`。** 无旋钮时 lavapipe 的每次 resolve 都走扩展臂，shader 臂在翻转下的 min 角矩形
从未执行。`DirectVulkan.{Split,Spawn}.MsFlip1.` 带 `MGITEST_MAGMA_FORCE_SHADER_DEPTH_RESOLVE=1`，
只在 split + spawn（§2.7 的理由），列入 `MAGMA_SERVER_ENV_KNOB_NO_TCP`；`spawn_lane_parity.py`
现在要求每个 tail **恰好**命中一条 split 键，死 tail 或过宽 tail 都让门失败。

red-once（均已执行并还原，split + spawn，外加 tcp 的 `MsFlip.`）：

1. 关掉预检 → 五条 `MsFlip.`/`MsFlip1.` 全红：`the declined COLOR|DEPTH scale must not have
   written its colour either`（读到 {255,0,0,255}，要 {0,0,255,255}）；
2. 放回 round 1 的「一侧为空就静默返回」→ 五条全红在空目的地腿上（错误 0，要 1282）；
3. 给 shader 臂传 `sx0/sy0` 而不是 min 角 → 只有 `MsFlip1.` 两臂红（`768 of 768 depth values differ
   from 0.75`；`928 of 3072 stencil values are wrong; first at (20, 4): got 255, want 99`），
   `MsFlip.` 保持绿——证明这条孪生条目确实走到了 shader 臂的矩形；
4. 往 tail 表里加一个死的 `.MsFlip2.` → parity RC 1，`matches 0 split entrie(s)`。

两个拆开的 once 站点没有车道级 red-once：跨格式 Y decline 需要 renderbuffer 与 texture 的 D24S8
映射到不同 `VkFormat` 的设备（§2.6），lavapipe 不是。

### 2.10 审查 round 3：按传输跳过、默认 draw framebuffer 也进预检、两处措辞

**1. 跳过判据是进程解析到的传输，不是车道标记。** round 2 的第二道门按 `SplitLane::IsSplitLane()`
跳过，而 `MGITEST_SPLIT_LANE=1` 标的是**精选车道**，不是传输：`DirectVulkan.{Split,Spawn,Tcp}.Full.`
（整二进制普查）与 `DirectVulkan.VerifySplit.`（verify 构建上的 split 臂，`integration-verify-split`
作业的门）**故意不带**它却跑在 inproc / spawn / tcp 上——各自的 CMake 块写明了为什么：标记会武装
fixture 的「记录序数必须移动」断言，对只查询的用例是误报。于是这条用例在它们上面全部 `Skipped`，文案
还说「monolith 臂（MGITEST_SPLIT_LANE unset）」——而 round 2 之前它在这些条目上是跑过并通过的。
改为 `!SplitLane::IsSplitLane() && !PeekSplitRuntime().transportResolved`
（`Harness/SplitRuntimePeek.h`，即 `MG_Config::Transport != Monolith`）——这正是
`VulkanRenderer::BlitFramebuffer` 分到 `BlitWireFramebuffers` 的那个分叉；文案改说「monolith
transport」。标记单独仍然有效：一条要了 split 的精选车道照常跑这条用例，它拿没拿到传输由 fixture 的
武装断言说。

实测（`build-split`）：

- `DirectVulkan.DepthStencilReadbackMatrixScenario.AFlipped…` 仍 `Skipped`：「the monolith
  DirectVulkan transport (this process resolved no split transport, so BlitFramebuffer takes the
  monolith arm) refuses a flipped depth blit …」；
  `DirectGLES.{,ForcedDepthStencilEmulation.}DepthStencilReadbackMatrixScenario.AFlipped…` 仍
  `Skipped`（Espryt 文案）；
- `DirectVulkan.{Split,Spawn,Tcp}.Full.…AFlipped…` 三条由 `Skipped` 变 **`Passed`**；
- `DirectVulkan.{Split,Spawn}.{MsFlip,MsFlip1}.` 与 `DirectVulkan.Tcp.MsFlip.` 仍 `Passed`。

**`VerifySplit.` 在本分支上量不到，原因写明。** 那条车道由 verify-split 包注册，不在 `p7/magma-b2`
上。本树按同样的 split 选项另配了 `build-verify`（`-DMOBILEGL_PIPE_VERIFY=ON`）：monolith 的
`DirectVulkan.Verify.` 条目绿，但**每一条** split 条目——包括与本包无关的
`DepthStencilReadbackScenario.DefaultFramebufferDepthClearIsVisibleToReadPixels`——都在 EGL 启动的
pre-flight 子进程里 SIGABRT（`MOBILEGL_ITEST_REQUIRE_GPU=1` 把它变成 `ScenarioFixture.h:112` 的
Failure），环境里有没有 `MOBILEGL_PIPE_VERIFY=1` 都一样。gdb 跟进子进程：`mgl-srv-apply` 线程在
`MGPipeApplyResourceRespecify` 处 abort（`PipeWireDecoder::ApplyChecked` ← `PipeApplier::ApplyOne`
← `ServerLoop::DrainRing`）——这就是 `PLAN-PH-P34B-P7.md` 写的「verify+split 撞旧
`PipeRespecifyScope` 断言」，由 verify-split 包放宽，本分支没有它。本分支自己能证明的是同一道门在
同样的形状（无标记、传输已解析）上放行，即三条 `Full.`。
`VerifySplit.` 本身则在一棵**一次性的分离 worktree** 上量了：落地树的头 + 本 round 的提交 a
cherry-pick 上去，同样的 split 选项 + `-DMOBILEGL_PIPE_VERIFY=ON`（不碰任何既有 worktree，量完即删）：
`DirectVulkan.VerifySplit.DepthStencilReadbackMatrixScenario.AFlipped…` **`Passed`**（round 2 的门下
它是 `Skipped`），`DirectVulkan.DepthStencilReadbackMatrixScenario.AFlipped…` 仍 `Skipped`
（monolith transport 文案），`DirectGLES.VerifySplit.…AFlipped…` 仍 `Skipped`（Espryt 文案）。
落地后的正式读数仍由 integrator 的 `integration-verify-split` 门给出。

**2. 默认 draw framebuffer 也进预检。** round 2 的预检把默认 DRAW framebuffer 留给 lambda，理由是
「默认 draw 在多重采样 read 后面本就是 `ResolveWireDepthStencil` 的 region Fatal，形状无关」——
**不成立**：那条臂先调 `DeclineWireDepthStencilResolveShape`，**后**才到 `destination.isDefault` 的
Fatal。于是「MSAA FBO → 窗口，`COLOR|DEPTH`，缩放（或 X 镜像）」这一形状下：颜色 lambda 先跑
（`BlitWireColorToDefault` 把 swapchain 图像写了），深度再记 `INVALID_OPERATION` 返回——正是 §2.9
第 3 条与 §6 第 1、5 条宣称已关的部分生效，只是换了一种目的地。
修法：守卫去掉 `!draw->IsDefault`；`declines` lambda 两侧都按 `isWriteTarget=false` 解析——这里只看
`.format` / `.samples`，而 `DefaultFramebufferReadIndex` 是 `const`（`VulkanRenderer.h`），写索引
不动；`ResolveWireImage` 对非默认 framebuffer 根本不看这个参数。
场景加一条腿：窗口清成蓝色、深度 0.5，把多重采样源（下红上绿）放大铺满整个 128×96 的窗口、
`COLOR|DEPTH`，要求 `INVALID_OPERATION`，并在 (1, 1)、(64, 48)、(126, 94) 三点用 `glReadPixels`
核对颜色仍是蓝、深度仍是 0.5。harness 的 pbuffer 默认 framebuffer 只有 `glReadPixels` 一种看法，
所以两个 aspect 都由它断言。

red-once（split + spawn，已还原）：把 round 2 的 `&& !draw->IsDefault` 放回守卫 →
`DirectVulkan.{Split,Spawn}.MsFlip.` 两臂都红在新腿上：`the declined COLOR|DEPTH scale onto the
window must not have written its colour at (1, 1)`，读到 {255,0,0,255}（源的下带），(64, 48) 与
(126, 94) 读到 {0,255,0,255}，要 {0,0,255,255}；而 `INVALID_OPERATION` 的断言**照样成立**——这正是
「一半效果」的样子。修后 107 条多重采样 blit/resolve 与深/模板读回条目（三臂的
`MsFlip / MsFlip1 / MsBlit / MsResolve0 / MsResolve1`、三臂 `Full.` 的两套读回场景、monolith 的
`DirectVulkan.DepthStencilReadback*`）全绿，既有 skip 不变。

**3. 两处措辞。** `VulkanRenderer.h` 对 `DeclineWireDepthStencilResolveShape` 的说明与 §2.9 第 3 条
说的「declined call 不动任何附件」只对**形状** decline 成立：`ResolveWireDepthStencil` 里的**能力**
decline（无 `VK_KHR_depth_stencil_resolve` 也无 `VK_EXT_shader_stencil_export` 的设备上的
stencil-only resolve）仍是逐 aspect 决定的，一次 `COLOR|STENCIL` 调用在颜色写完之后才 decline
模板。规则 I (a) 接受这一点，两处措辞已收窄到形状 decline。§6 第 4 条与 §7 的 knob 条目数：落地树是
**七**条 tail（B3 合入时 `.StaleSerial.` 进了表），本分支没有 B3、它的门量到的是六条；两处都已写明。
§6 新增第 8、9、10 条（审查 round 3 点出、本 round 不改的三笔债：合法同尺寸 MSAA→默认 framebuffer
深/模板 blit 的 Fatal、scissor 裁空的静默返回、MS→MS 的逐 aspect decline）。

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
`VulkanRenderer.cpp` 记在 P5f fm（`036c3a3d`）——所以 P7 基线 `e8b2c4bd` 上只剩**两**处，就是本片
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
   带，则这条臂在真机上同样只有旋钮可达；不带，则它是真机的**默认**臂。另看 renderbuffer 与
   texture 的 D24S8 是否映射到同一个 `VkFormat`：不同则 `MsFlip.` 在跨格式 Y decline 上红（§2.6）。
3. **片 4**：`ROTATE_90/180/270` 下 monolith 臂的烘焙 blit 几何（§4.5）。
4. 包 B 遗留、本包未碰：非恒等旋转下的 `default-color-blit-shape` scratch 臂，以及
   OpenRA 三次 `pm clear` 冷跑的 1.0 验收。

---

## 6. 记录债（写给 CONTRACT-P7 §12 的几行）

1. **缩放后的第二段没有裁剪**（`WireFramebuffer.inc:932-946`，round 2 之后本树行号）。多重采样颜色 resolve 的第二段
   `vkCmdBlitImage` 把目的矩形原样交给驱动；一个越出目的图像的矩形是
   `VUID-vkCmdBlitImage-dstOffset-00248/00249`，不是一张被裁过的图。**这条与 monolith 臂共有**
   （`VulkanRenderer.cpp:10066` 的 `vkCmdBlitImage` 同样不裁；`:10060` 是它前面的
   `vkCmdResolveImage`，行号是 `pipe` 上的），所以它不是分离缺陷，也不该由本包单独改；
   两条臂一起裁是一件独立的活。
   **部分生效（审查 round 2，已关）**：同一个缩放，mask 若是 `COLOR|DEPTH`，round 1 先由这条臂把颜色
   缩放写进目的地，再由深度臂记 `INVALID_OPERATION`——出错的调用留下了一半效果，18.3.1 不允许。
   现在缩放形状在任何 aspect 之前由 `DeclineWireDepthStencilResolveShape` 决定，整个调用报错、
   什么也不写（§2.9 第 3 条，red-once 已做）。round 2 的预检漏掉了默认 draw framebuffer——同一个
   缩放落到**窗口**上时颜色照样先写进 swapchain——round 3 补上（§2.10 第 2 条，red-once 已做）。
   剩下的债只是上面的「不裁」。
2. **`MagmaWireFatal("multisample-resolve-region")`**（`WireFramebuffer.inc:878`）仍然对
   **源矩形越出读缓冲**的 blit 带走会话。GL 对这种形状的承诺是「那些像素的值未定义」，不是一个
   错误——所以正确的收口是 decline 或钳制，不是 Fatal。本包没有改它，因为它不带 `@P7`、也不是
   §3.2 点名的行；记在这里。
3. **1 → N 的等矩形 decline**（`WireFramebuffer.inc:1095-1125`）拒绝的是一个**合法**形状：
   18.3.1 说单采样源写进多重采样目的地是**样本复制**。本包 decline 它，是因为 Vulkan 没有命令能
   表达它，而 monolith 臂对同一形状的「可观测」是一次 VU 违规（`vkCmdBlitImage` 的两侧都必须是
   单采样）——即没有一个正确的对照读数可抄。要真的实现它需要一条以目的地采样数建管线的 draw，
   也就是 resolve 机器反过来再写一遍。
4. **tcp 车道带不了 server 端测试旋钮**（§2.7）。**七**条 knob 条目——`.ShaderMip1.` `.ShaderMip2.`
   `.DepthMip.` `.DefaultBlitShape1.` `.MsResolve1.` `.MsFlip1.`（round 2 加的）以及 B3 合入时进表的
   `.StaleSerial.`——因此只注册在 split + spawn，并在 `spawn_lane_parity.py` 的
   `MAGMA_SERVER_ENV_KNOB_NO_TCP` 里具名（本分支没有 B3，它的门量到的是六条；落地树七条）。要收回这条，
   `tcp_server_fixture.py` 得接受每用例的 server 环境——一个 supervisor 控制项。
5. **X 镜像与跨格式 Y 镜像的深/模板 resolve**（§2.8）decline。前者要逐 texel 的 region，后者要
   `BlitDepthAcrossFormats`（与 monolith 共用）长出翻转。两者都不是一个片的体量。这两条 decline
   拒绝的是**合法**的 blit，所以它们的 `INVALID_OPERATION` 是规则 I (a) 的具名拒绝，不是 GL 的答案。
   **部分生效（审查 round 2，已关）**：round 1 在颜色 aspect 已经镜像写完之后才 decline 深度——颜色
   镜像了、深度没动、再报一个 GL 不会报的错。现在**形状** decline 在任何 aspect 之前决定，整个调用什么也
   不写（§2.9 第 3 条；默认 draw framebuffer 那一半是 round 3 关的，§2.10 第 2 条）。两种 decline 各有
   自己的 once 站点。跨格式那条在 renderbuffer 与 texture 的 D24S8 映射到不同 `VkFormat` 的设备上会让
   `MsFlip.` 变红（§2.6）。
6. **monolith `DirectVulkan` 臂的翻转 / 缩放多重采样深度 blit**（`VulkanRenderer.cpp:9594`、`:9608`，
   落地树行号）：翻转被拒（「depth blits with flipped rectangles are not supported yet」，目的地保持
   清屏值、无错误），缩放被**执行**（GL 说是 `INVALID_OPERATION`）。这是一处 **wire 与 monolith 的
   分歧**，且 wire 臂是更正确的一方；`AFlippedMultisampleResolve…` 在 monolith 条目上以具名理由
   `Skipped`。monolith 的修正归 P13 / G1（G1 期间 pull 构建的 `.text` 不许动）。
7. **Espryt（DirectGLES，monolith 与 split 一样）的翻转 / 缩放多重采样深度 resolve**：翻转什么也不写
   且不报错，缩放不报 `INVALID_OPERATION`。同一用例在 DirectGLES 条目上以具名理由 `Skipped`。
   这是 **P3b / P4b** 的债。
8. **同尺寸、合法的 MSAA FBO → 默认 framebuffer 深/模板 blit 是会话 Fatal**（审查 round 3 记债，
   未改）。`ResolveWireDepthStencil` 的区域判据（`WireFramebuffer.inc` 的 `destination.isDefault` 子句，
   `Magma:multisample-depth-resolve-region`）对默认 draw framebuffer 一律 Fatal——而「把 MSAA 场景
   resolve 到窗口、mask 里带 `DEPTH`」是标准写法，GL 允许，18.3.1 只要求尺寸相同。round 3 之后形状
   decline 走在它前面（§2.10 第 2 条），所以现在只有**合法**的同尺寸形状还会撞到它：错误的形状 decline，
   正确的形状带走会话。收口应是把默认 framebuffer 的深/模板图像当普通目的地走同一条 resolve 臂。
9. **scissor 把目的地裁空的 blit 静默返回，即便 GL 对该形状仍会报错**（审查 round 3 记债，未改）。
   `BlitWireFramebuffers` 的 `clipAxis` 在裁空时直接 return，先于任何错误检查；而 18.3.1 的错误检查
   （尺寸不同的多重采样 blit、格式不兼容等）与 scissor 无关——一次被 scissor 裁光的非法 blit 仍欠一个
   `INVALID_OPERATION`。收口是把形状 / 格式的错误检查提到裁剪之前。
10. **MS → MS 的 blit 逐 aspect 决定**（审查 round 3 记债，未改）。`BlitWireFramebuffers` 多重采样
    目的地那一段的 decline（尺寸、采样数、`source.format != destination.format`）写在逐 aspect 的
    lambda 里，而格式相等是**每个 aspect 各自**的事实：一次 `COLOR|DEPTH` 的 MS→MS 调用可以复制颜色
    （颜色格式相同）、再 decline 深度（深/模板格式不同）——与第 1、5 条同一类部分生效。§2.9 第 3 条 /
    §2.10 第 2 条的预检只覆盖「多重采样源 → 单采样目的地」那条臂，不覆盖这段。

---

## 7. 审查 round 的门（本树一行；落地树是 integrator 的门）

round 1 的这一节把每个读数都记在「`5d896170`」上，而 `5d896170` 不是落地树的祖先（提交是被
cherry-pick 到落地树上的）。所以这里只记**本树**的读数，并点名量的是哪棵树：

- **本树**（合并前）：`p7/magma-b2@1bf48cfb`，即 round 3 的两条代码提交之顶（round 2 的三条在它
  下面）；本节所在的文档提交只动这份笔记与 `VulkanRenderer.h` 的一条注释。下表在 `1bf48cfb` +
  那条注释的树上量（即最终头的代码内容），并在最终头上重量一遍，读数相同。
- **落地树：integrator 的门。** 数字由 integrator 在合入后实测并维护，本笔记不引落地树的 SHA。落地树
  上多了本分支没有的注册（例如 `DirectGLES.{Split,Spawn,Tcp}.ForcedDs.*`、`VerifySplit.`、B3 的
  `.StaleSerial.` 条目），所以两棵树的 `integration-{split,spawn,tcp}` / `unit` / parity 读数不可互比。

| 门 | 本树 `p7/magma-b2@1bf48cfb` |
|---|---|
| `ctest -L unit` | **2423**，全绿 |
| `integration-split` | **187**，全绿 |
| `integration-spawn` | **103**，全绿 |
| `integration-tcp` | **106**，全绿 |
| `integration-magma-split` | **95**（94 + `MsFlip1.`），全绿 |
| `integration-magma-spawn` | **74**（73 + `MsFlip1.`），全绿 |
| `integration-magma-tcp` | **70**（`MsFlip1.` 不上 tcp），全绿 |
| `integration-magma-full-split` | **525**，全绿 |
| monolith `DirectVulkan.…AFlipped…` / `DirectGLES.…AFlipped…` | 均 `Skipped`，具名理由（§2.9 第 1 条、§2.10 第 1 条） |
| `DirectVulkan.{Split,Spawn,Tcp}.Full.…AFlipped…` | 三条 **`Passed`**（round 2 时 `Skipped`）；`VerifySplit.` 见 §2.10 第 1 条（一次性 worktree 上 `Passed`，正式读数归 integrator 的门） |
| `spawn_lane_parity.py build-split` | RC 0；gated 档点名 6 条 `MAGMA_SERVER_ENV_KNOB_NO_TCP` 条目（落地树 7 条，含 B3 的 `.StaleSerial.`），每个 tail 恰好命中一条 |
| `fatal_census` | 79 / 44 / 0，RC 0 |
| `link_ratchet --assert-monotone` | 173 不变，RC 0 |
| G1 pull 构建 | RC 0，`.text` `0xa52203`，`nm --defined-only` 与 `~/w7/p7-before/pull-syms.txt` 0 增 0 减 |
| `@P7` | 19 行，全是注释 |

`scripts/data/link_ratchet_baseline.txt` 的 `base commit` 头注释指 `6af422b3`——173 是在那棵树上量出来的，
不是在 `4bee1313` 上；本 round 没有动它。
