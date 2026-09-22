# P7 wave 2-B：Magma blit / copy / mip 簇（分支 `p7/magma-b`）

> 计划见 [`PLAN-PH-P34B-P7.md`](PLAN-PH-P34B-P7.md) §1.3 与 §3 wave 2-B；规范见
> [`CONTRACT-P7.md`](../../../../MobileGL/MG_Remote/CONTRACT-P7.md) §0（规则 I / J）、§3.2（退役 vs
> decline 表）、§5.1 / §5.2。基线 = `feat/disaggregated` 上 `~/w7/pipe` 的头 `8652bac0`
> （wave 0 已落地：Magma 两进程三臂、`MGPipeSessionFailHook` → `Session::Fail` 漏斗、
> `scripts/link_ratchet.py`）。
>
> 主机口径：WSL Arch + lavapipe（`/usr/share/vulkan/icd.d/lvp_icd.json`，系统唯一 ICD），
> `build-split` = Release / clang / ccache / `DISAGGREGATED=ON` `INPROC=ON` `PIPE_PUSH=ON`
> `BUILD_INTEGRATION_TEST=ON`，ICD 与 `~/w7/pipe/build-split` 逐项相同。
>
> 基线门读数（本树实测）：`integration-magma-split` **73**（71 PASS + 2 SKIP）、`-spawn` **52**、
> `-tcp` **54**，三臂 0 红。

## 0. 每片一条提交，每片一次 red-once

| 片 | 名 | 退役 / decline | 提交 |
|---|---|---|---|
| 1 | `mipmap-shader-format-or-shape` | 诊断 + 颜色 shader mip 回落（**退役**一半：可渲染格式的不可附着图像） | 见 §1 |
| 2 | `depth-stencil-mipmap@P7` + 烘焙 (A)(D) | DEPTH-only **退役**（烘焙深度 mip 程序）；D\|S **decline** | 见 §2 |
| 3 | `copy-image-in-place@P7` | **退役**（单次 GENERAL 转换）；同级同层矩形重叠 **decline** | 见 §3 |
| 4 | `default-color-blit-shape@P7` | **退役**（恒等回落 + 旋转 scratch）；四旋转之外 **decline** | 见 §4 |
| 5 | 出口门 3：OpenRA 回读竞态 | **修复**（回读自带写可见性 barrier + 等齐所有在飞提交） | 见 §5 |

---

## 1. 片 1：`mipmap-shader-format-or-shape` 的诊断与颜色回落

### 1.1 设备证据与它为什么主机看不见

`minecraft-1.21.4-fabric-iris-iterationt-in-world` 与 `-iterationt-nodsa-in-world` 在 Redmi
（`2f7cbe2e`，Adreno 830v2，DirectVulkan inproc，`--use-pbuffer`）上确定性地让 server apply 线程死于

```
MGPipe: Fatal{UnmigratedVerb, "Magma:mipmap-shader-format-or-shape"}
```

run-ahead 与 lockstep 都复现；monolith 臂通过；WSL/lavapipe 全绿。

那一行就是全部证据，而 `WireFramebuffer.inc` 里这个名字下挂着**五个互斥的析取项**（aspect 非颜色、
1D/1D_ARRAY/3D 视图、图像缺 `COLOR_ATTACHMENT` usage、层范围越界、格式不是采样浮点）。设备能告诉我们的
唯一一件事——**是哪一个**——恰恰是它没告诉我们的。

主机看不见它，原因是结构性的：lavapipe 对本集成二进制能创建的**每一个**颜色格式都报
`BLIT_SRC|BLIT_DST`，于是 `GenerateWireMipmap` 的 `nativeBlit` 恒真，**两条 shader 臂在门机器上从未
执行过一次**。旁边那条看起来覆盖了 shader 臂的 `F1WireScenario.GenerateMipmapPackedFloatPixels` 也没有：
`R11F_G11F_B10F` 在这里同样原生 blit。一条没有任何主机车道能到达的拒绝，就是一条会发版的拒绝。

### 1.2 做了什么

**(a) 诊断。** 名字仍是第一个词、不加前缀（census、retrace 拒绝普查与所有 log grep 都按首词键控），
其后附上驱动自己的答案：格式枚举、（视图重解释时的）存储格式、extent、levels、aspect、viewType、
usage 位、层范围，以及 `optimalTilingFeatures` 里**缺**的特征位名。实测输出形如

```
Magma:mipmap-shader-format-or-shape format=37 extent=8x8x1 levels=4 aspect=0x1 view=1 \
    usage=0x17 layers=0+1/1 missing=DEPTH_STENCIL_ATTACHMENT
```

下一次设备窗口不必再推导，读一行即可指名。

**(b) 颜色回落（退役的那一半）。** 原来的 shader 臂**直接渲染进下一级**，这要求图像创建时带
`COLOR_ATTACHMENT` usage；而设备拒绝把某格式作为颜色附件时，`VkTextureManager` 就不会给它这个位——
于是每一个「缺 BLIT 且只采样」的纹理都落进拒绝而不是回落。现在区分两种形状：

- **格式可渲染、图像可附着** → 原臂不变（直接渲染进目标级）；
- **格式可渲染、图像不可附着** → 以目标级尺寸创建自有 scratch 颜色附件，shader 渲染进 scratch，再
  `vkCmdCopyImage` 原样搬进该级。纯 texel 搬运，无格式转换，texel 仍来自 GPU 内容而非 staged 影子；
  scratch 按 multisample resolve 臂同样的形状随提交退休（共享 scratch 在此处改尺寸会毁掉更早排队的一次
  pass 仍在读的图像）。

仍然拒绝且现在**指名**的残余：非颜色 aspect（深度归片 2）、1D/1D_ARRAY/3D、层范围越界、非采样浮点格式、
以及格式本身就不能作颜色附件（此时没有任何 shader 臂可走）。

**(c) 主机可达性（测试旋钮）。** `MGITEST_MAGMA_FORCE_SHADER_MIPMAP`，只读一次，只在
`#if MOBILEGL_BUILD_DISAGGREGATED` 内编译，不进 `ConfigLoader` 的 accepted-env 表：

- `1` = 丢掉原生 blit（Adreno 对缺 `BLIT_DST` 格式走的那条臂）；
- `2` = 再把目标当作不可附着（Adreno 对只采样纹理走的那条臂，也就是本片之前的拒绝本身）。

### 1.3 场景与车道

`F1WireScenario.GenerateMipmapWithoutNativeBlitPixels`：8×8 RGBA8、上下两半不同色（一色铺满时，丢失
GL 行序的实现会平均成同一个数，单色断言会把它判对）、`glGenerateMipmap` 后读 level 1 的四行，断言下两行
是下半色、上两行是上半色（边界从不落在目标 texel 内部，两色不混）。旋钮未设时 `GTEST_SKIP` 并写明理由。

注册在 Fm 车道的三臂上，**每臂两条**（`ShaderMip1.` / `ShaderMip2.`），**各自一条 ENVIRONMENT**——
不是车道级翻转：旋钮改变本进程内**每一次** mipmap 的生成方式，放进共享 Fm 环境会把
`GenerateMipmapPixels`、`GenerateMipmapPackedFloatPixels`、`GenerateMipmapDepthPixels` 与两条
base/view 用例一起改道，四条绿条目会不再测量它们被写出来测量的那条臂。

### 1.4 red-once（R-16，已执行并还原）

把 scratch 臂关掉（`viaScratch` 恒假）重建，`ctest -R ShaderMip`：

```
CTEST_RC=8
MGPipe: Fatal{UnmigratedVerb, "Magma:mipmap-shader-format-or-shape format=37 extent=8x8x1
    levels=4 aspect=0x1 view=1 usage=0x17 layers=0+1/1 missing=DEPTH_STENCIL_ATTACHMENT"}
75% tests passed, 2 tests failed out of 8
```

即 tier-2 条目**按名死亡**，且诊断把格式与形状都说出来了。还原后同一过滤器 8/8 绿。

### 1.5 门

`integration-magma-split` **75**（+2）、`-spawn` **54**（+2）、`-tcp` **56**（+2），三臂 0 红。

---

## 2. 片 2：深度 mip 烘焙 (A) + 新鲜度门 (D)

### 2.1 为什么 monolith 的回落不能直接用

monolith 臂从拆分之前就带着一条深度 mip 的 shader 回落（`GenerateDepthMipmapWithShader`），因为
`VK_FORMAT_FEATURE_BLIT_DST` 对深度格式**是可选的**，相当多的设备不给。wire 臂没有这条回落：深度纹理
一旦不能原生 blit，第一个析取项 `resource->aspect != COLOR` 就把它送进
`mipmap-shader-format-or-shape`（片 1 的诊断现在会把它指名为 `aspect=0x2 missing=COLOR_ATTACHMENT`）。

monolith 那条回落**不可复用**，这正是 CONTRACT-P7 §5.1 (A) 的全部内容：它要造一个 GL `ProgramObject`、
两个 `ShaderObject` 和一个 GL sampler，经 uniform manager 的 global UBO 绑定，把 `uSrcTexelSize` 当默认块
uniform 写——四个前端对象加一次 GL 编译器调用，落在 server 的 apply 线程上。

### 2.2 烘焙 (A)

- `WireDepthMipmap.vert`：几何与 `WireColorBlit.vert` **逐字相同**（同一个 quad、同一个 push-constant
  rect、同一个 GL-bottom-left → native-image 映射），只多一个成员 `ivec2 srcTexelSize`。push-constant 块
  必须在同一管线的每个 stage 里声明一致，所以顶点阶段也声明它，而不是拆出第二个 range。
- `WireDepthMipmap.frag`：`kDepthMipmapFragmentShaderSource` 的**移植**而非重写——同样的 2×2
  `texelFetch` 盒、同样的 clamp、同样的平均、同样的 `gl_FragDepth`。§3.2 的「退役」= 与 monolith 臂同
  可观测，所以连它那半个 texel 的偏移（`ivec2(texCoord * srcTexelSize)` 落在目标中心偏半 texel）也照搬。
- `WireDepthMipmap.inc`：深度附件 render pass（`loadOp = DONT_CARE`，pass 写满整级）+ 管线
  （`depthTest/Write = TRUE`、`compareOp = ALWAYS`、无颜色附件）+ 按格式/布局缓存的 target 表，形状与
  `WireColorBlit.inc` 逐条对齐（同样的懒建模块、同样的每提交描述符池、同 image 时两侧都 GENERAL）。
  sampler 用 NEAREST：frag 自己做 2×2 盒，任何采样器过滤都会被算两遍，而且多数设备不给深度格式
  linear。
- `GenerateWireMipmap` 深度分支接入；`WireDepthMipmapSpirv.h` 由下面的门自己烘。

### 2.3 `depth-stencil-mipmap@P7` 拆成退役 + decline

DEPTH-only 走上面的烘焙程序（**退役**）。D|S 合并与 stencil-only 改为 **decline**（`MGLOG_E_ONCE` +
return，§3.2），并且**在 `GrowWireTextureMipChain` 之前**就 return——否则会先长出一条这个动词不会填的
链，随后又把它记成 defined。

decline 而非 Fatal 的理由：**monolith 也做不到**。它的 shader 回落自己断言 depth-only
（"shader fallback only supports depth-only textures"），而断言在两条臂都发的 release 构建里等于零。GL
也没承诺 stencil mip；这条 pass 写 `gl_FragDepth`，会把它刚产出的那一级的 stencil aspect 留成未定义——
比整级不动还糟。

### 2.4 新鲜度门 (D)：`MOBILEGL_BAKED_INTERNAL_SHADERS`

`MG_Test/SelfTest/BakedInternalShadersTest`，表驱动，8 行（`WireColorBlit.{vert,frag}`、
`WireDepthMipmap.{vert,frag}`、`DriverPostIterationRPWitness.comp`、
`PrimitivesGeneratedNoXfbProbe.{vert,tesc,tese}`），用**树内 glslang** 重编每一行并逐字比较。

**这个门就是烘焙器。** `scripts/bake_internal_shaders.py` 自己不编译任何东西：它用
`MOBILEGL_BAKE_INTERNAL_SHADERS` 跑这个二进制，拿它吐出的字，splice 进头文件。方向是刻意的——门走
glslang **库**、烘焙器 shell 出一个 glslangValidator **二进制**，就是两个编译器、没有哪个说了算；等它们
的默认值哪天分叉，门会在一个没人能重新生成的头上变红。格式**按头保留**而不是统一：树里的头对每行几个字
和 `u` 后缀意见不一，统一会把一个字的改动埋进整文件 diff。

**首轮实测（这是事先没人能知道的一件事）：**

| 头 | 结论 |
|---|---|
| `WireColorBlitSpirv.h` | **已新鲜**——手跑 glslangValidator 的字与树内 glslang 逐字相同 |
| `PrimitivesGeneratedNoXfbProbeSpv.h` | **已新鲜**（三个数组全部） |
| `DriverPostIterationRPWitnessSpv.h` | **过期**，按其自己的散文所说；本片重烘（2142 → 2143 字），witness magic `0x50323033` 仍在（它是 `.comp` 里的常量，不是字的属性） |

三处 provenance 注释改为指向脚本与门。

### 2.5 场景

- `F1WireScenario.GenerateMipmapDepthWithoutNativeBlitPixels`（旋钮 tier 1，三臂）：
  `GL_DEPTH_COMPONENT24`（→ `X8_D24_UNORM_PACK32`）与 `GL_DEPTH_COMPONENT32F`（→ `D32_SFLOAT`）各一遍，
  16×16、上下两半 0.75 / 0.25，`glGenerateMipmap` 后按**采样附件**读回 level 1 的 8 行。**第 3 行不断言**：
  移植来的滤波器带半个 texel 偏移，两种读法下第 0..2 行只来自下半、第 4..7 行只来自上半，第 3 行是跨界
  带——钉它等于钉那半个 texel 而不是钉 mip。
- `F1WireScenario.GenerateMipmapDepthStencilDeclinesAndKeepsTheSession`（**无旋钮**，三臂）：
  `GL_DEPTH24_STENCIL8` 上 `glGenerateMipmap` 不报 GL 错、不带走会话（回读是一次穿到 server 的往返，
  一并回答会话还活着与 level 0 还在）。

### 2.6 red-once（R-16，三条，均已执行并还原）

1. **新鲜度门**：把 `WireDepthMipmap.frag` 的 `0.25` 改成 `0.2500001`（**不需要重建**——门在运行时读源），
   `WireDepthMipmapFragment` 行在**第 279 个字**变红并指名重新生成的办法；还原后 8/8 绿。
2. **深度退役**：关掉深度分支 → `DirectVulkan.Split.DepthMip.*` 死于
   `Fatal{UnmigratedVerb, "Magma:mipmap-shader-format-or-shape format=125 extent=16x16x1 levels=5
   aspect=0x2 view=1 usage=0x27 layers=0+1/1 missing=COLOR_ATTACHMENT"}`（格式 125 =
   `X8_D24_UNORM_PACK32`，诊断把它指名了）。
3. **D|S decline**：把 `MagmaWireFatal` 放回去 → 该用例死于
   `Fatal{UnmigratedVerb, "Magma:depth-stencil-mipmap@P7"}`。

还原后两条 2/2 绿。

### 2.7 门

`integration-magma-split` **77**（+2）、`-spawn` **56**（+2）、`-tcp` **58**（+2），三臂 0 红；
`ctest -L unit` **2410**（+8 = 新鲜度门的八行）全绿；信息档 `integration-magma-full-split` **513**，
0 红 / 59 skip（基线 510，0 红 / 57 skip——两条新增用例在无旋钮的全量档自 skip）。

**G1 实测过了，而且这一条事先不是显然的**（片 2）：`DriverPostIterationRPWitnessSpv.h` 是
`MG_Util/SelfTest` 的头，pull 构建也编它，重烘改了 2142 → 2143 个字。pull `.text` 仍是 **`0xa52203`**，
`nm --defined-only` 的名字表与 `~/w7/p7-before/pull-syms.txt` 逐行相同（30570 行）——字长的变化落在
`.rodata`，没有改动任何一条指令的编码宽度。`fatal_census` 绿（79 abort 站点 / 43 家族词 / 0 无标记，
本片**退掉一处** `Fatal{}` 用法但不动 abort 站点数）；`link_ratchet --assert-monotone` 186 不变，
`p7-magma` 桶 101。

---

## 3. 片 3：`copy-image-in-place@P7`

### 3.1 那条 Fatal 说的是真话，但只对它下面的代码成立

`glCopyImageSubData` 在**同一个纹理的两个子资源**之间拷是普通的、有定义的 GL（4.6 core 18.3.2 还专门
写明纹理与它的 view 是同一 image 上的两个对象、允许互拷）。wire 臂为它带走整个会话，理由写在注释里：
下面那对 `TRANSFER_SRC` / `TRANSFER_DST` **描述不了一个 image**——两端共用同一个被跟踪的
`VkImageLayout`，第二次转换会把第一次撤销。

`VK_IMAGE_LAYOUT_GENERAL` 能描述它（`vkCmdCopyImage` 两侧都接受 GENERAL）。于是：**一次**把整个 image
转成 GENERAL（源级与目标级都在里面）、命令两侧都 GENERAL、**一次**还原；access mask 带上读写两个方向，
因为这一道 barrier 是两边共同的边。

monolith 臂至今仍把所有 in-place 拷贝整体 decline（同函数上方的 `!wire &&` 分支），所以这一片**让 wire
臂比 monolith 臂多做一件事**——这正是 §3.2 要求的形状，而不是对齐。

`inPlace` 在 pull 构建里是 `constexpr false`，它守的每个分支都会折掉：这个函数也编进 monolith 镜像，
而 G1 钉着那个镜像的 `.text`。

### 3.2 留下来的那一种 decline

同 image + 同 level + 同 layer + **矩形重叠**：GL 4.6 core 18.3.2 说重叠处结果未定义。GENERAL 让这条
命令**可记录**，不让它**有意义**——`vkCmdCopyImage` 对自己在同一 region 内的读写不排序，tiler 产出的是
每个 texel 先到的那一半。所以 `MGLOG_E_ONCE` + return，把这句话说一次，而不是把它发出去。

判据是**三个维度同时成立**：同 level、slice 区间相交、矩形相交。level 0→1、层与层不相交、同一级上两个
不相交的矩形，都是有定义的，都照做。

### 3.3 场景

`F1WireScenario` 里两条（split-only，因为 monolith 没有可对照的读数），一张 8×8×2、两级的
`GL_TEXTURE_2D_ARRAY`，**每一个被碰到的子资源都填成互不相同的颜色**（level 0 layer 0 四象限、
level 0 layer 1 洋红、level 1 layer 0 青、level 1 layer 1 白）——铺平色会让「拷到了错的 level / 错的
layer / 根本没拷」都读成对的。

- `CopyImageInPlaceAcrossLevelsAndLayers`：level 0 → level 1（同层）后 level 1 layer 0 变成源矩形那个
  全红象限、layer 1 仍是白；再 level 0 layer 0 → layer 1（同级、层不相交）后两层都是四象限，源不变。
- `CopyImageInPlaceOverlapDeclinesAndLeavesTheLevelAlone`：同级同层 (0,0)-(4,4) → (2,2)，无 GL 错，
  level 0 layer 0 逐 texel 不变（若真执行，(4,4) 会从黄变红，断言分得出来）。

三臂各注册两条。

### 3.4 red-once（R-16，已执行并还原）

把 `MagmaWireFatal("copy-image-in-place@P7")` 放回去 → 两条都死于
`Fatal{UnmigratedVerb, "Magma:copy-image-in-place@P7"}`；还原后 2/2 绿。

### 3.5 门

`integration-magma-split` **79**（+2）、`-spawn` **58**（+2）、`-tcp` **60**（+2），三臂 0 红。

> **本树的 tcp 端口改成了 `40913`。** 跑本片的门时 `TcpServer.Start` 整条 tcp 车道红了 59/60，
> 原因是 `40613`（`~/w7/pipe/build-split` 的默认值）上已经有一个兄弟树的 `libMobileGLServer` 在监听。
> 这是本地 configure 选项，不影响车道语义；`magma-two-process-first-run.md` 里 wave 0 也为同一原因
> 用了专用端口。

---

## 4. 片 4：`default-color-blit-shape@P7`

### 4.1 一条 Fatal 盖住了三种互不相干的情况

那条 shader pass **是为旋转存在的**：它只能**采样**自己的源（`sampler2D`、float 结果、两种 2D view
type 之一）。于是 RGBA8UI 附件、3D 切片、数组层都会让 `BlitWireColorToDefault` 返回 false——**而且在完全
没有旋转的表面上也会**，尽管二十行之下那条普通的 `vkCmdBlitImage` 臂本来就能正确完成。

拆成三块：

1. **恒等变换 → 回落**到普通 `vkCmdBlitImage` 臂（它已经把 GL 的底行映射到原生图像的末行）；
2. **非恒等旋转 + 采不到的源** → 把源矩形 `vkCmdCopyImage` 进一张自有的 2D、单采样、与源**同尺寸兼容类**
   的采样浮点格式 scratch，再对 scratch 做 shader blit；
3. **四个旋转之外**（镜像 / 镜像旋转）→ `MGLOG_E_ONCE` **decline** 并写明是哪个 transform：两条臂都表达
   不了反射，而本包没有一台会报这种 transform 的设备来核对结果。

对整数源的**重解释就是重点**，GL 也同意：整数与归一化格式之间的 `glBlitFramebuffer` 是
`INVALID_OPERATION`（4.6 core 18.3.1），所以没有「正确结果」可言——只有一种以前会结束会话、现在会产出源
比特的形状。对 view type 不对的浮点源，那次 copy 就是一次切片抽取，结果**完全正确**。

### 4.2 退役顺带挖出一个真 bug

普通 blit 臂把 3D 源当成层来寻址：`region.srcSubresource = {aspect, source.level, source.layer, 1}`，
`srcOffsets[].z = 0..1`。**3D 图像的切片在 z 上，它唯一的 subresource 是 layer 0**——copy 端点
（`CopyImageSliceMapping`）与上面的 multisample resolve 臂早就是这么寻址的。所以从 3D 纹理第 1 片 blit
会去读第 0 片、并用一个该图像根本没有的层。

**在此之前看不见**：这种形状先撞上那条 Fatal，根本走不到这条臂。本片第一次跑场景时回落臂的四个象限全错、
scratch 臂全对，bug 就是这么掉出来的。两条臂现在都对。

### 4.3 场景

`F1WireScenario.ColorBlitToDefaultFromANonSampleableSourceDegrades`：源是 **3D 纹理的第 1 片**
（普通合法 GL，正是 shader pass 采不到的形状），第 0 片填一个任何象限都不会出现的颜色（青）——一次忘了 z
原点的 scratch copy 会去读另一片，而那样四个象限断言**仍然是在断言某些真 texel**，所以必须让它可分辨。

**同一条用例、每臂注册两遍**：不带旋钮（走恒等回落臂）与 `MGITEST_MAGMA_FORCE_DEFAULT_BLIT_SCRATCH=1`
（在恒等表面上走旋转臂的 scratch 路径）。**两者必须产出同样的像素**，这条等式就是断言——也是 scratch 臂
唯一可被主机车道到达的方式：本车道能创建的每一个表面都是 `VK_SURFACE_TRANSFORM_IDENTITY`，本簇的 Redmi
证据也是 pbuffer。

### 4.4 red-once（R-16，已执行并还原）

把 `MagmaWireFatal("default-color-blit-shape@P7")` 放回去 → 两条都死于
`Fatal{UnmigratedVerb, "Magma:default-color-blit-shape@P7"}`；还原后 2/2 绿。

### 4.5 门与设备债

`integration-magma-split` **81**（+2）、`-spawn` **60**（+2）、`-tcp` **62**（+2），三臂 0 红。

**欠设备核对**：非恒等旋转下的 scratch 臂只在恒等表面上被旋钮验证过（像素与回落臂相同）；真正的
ROTATE_90/180/270 几何要在手机上看一眼。四旋转之外的 decline 无设备可核（没有会报镜像 transform 的
设备），按 decline 收口。

---

## 5. 片 5（集成者追加，最高优先）：出口门 3 的 OpenRA 回读竞态

### 5.1 机制（`file:line`，按本片落地前的树）

两句话：

1. **`WireFramebuffer.inc` 的 `ReadWirePixels` 先 `FlushPendingCommands()`，那一步把整帧的 draw 以
   自己的 pooled fence 提交上队列后就返回、不等**（`VulkanRenderer.cpp:13534-13598`，成功路径上没有
   `vkWaitForFences`）。随后回读的 `vkCmdCopyImageToBuffer` 被录进**另一个**命令缓冲、成为**后一次
   提交**。
2. **`SubmitReadbackCommandsAndWait`（`VulkanRenderer.cpp:10707-10748`）只等自己那一次提交的 fence**，
   然后 `OnSubmitsCompletedUpTo(frame.lastSubmitIndex)` **把更早那次也记成完成并退休它的 fence**。
   原注释写着「the wait proved every submission complete」——那句话对 monolith 臂成立（它不先 flush，
   draw 与 copy 同一次提交、同一个 fence），对 wire 臂不成立。

把两次提交绑在一起的**唯一**东西，是回读前那道 `TransitionWireImage` 的 barrier；而它的**源作用域来自
图像被跟踪的布局**（`GetImageTransitionSourceState`，`VulkanRenderer.cpp:1784-1825`）。其中两条臂——
`VK_IMAGE_LAYOUT_UNDEFINED` 与 **`VK_IMAGE_LAYOUT_PRESENT_SRC_KHR`**（`:1787-1791`）——返回
`VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT` 且 access 为 0，**这是 Vulkan 里「什么都不等」的写法**。
刚 present 过的默认帧缓冲正处在后一种布局。

于是：pipeline 创建越慢、draw 那次提交拖得越久，copy 越可能在它还没画完时就读走——**帧里最后画的那些
对象缺失**，正是设备上看到的样子。

### 5.2 主机实测（决定性，且不依赖竞态碰运气）

在 `ReadWirePixels` 记录 copy 的那一刻打点（OpenRA / DirectVulkan / inproc）：

```
MBPROBE ReadWirePixels/pre: isDefault=1 layout=1 aspect=0x1 submit=72 completed=71
```

`submit=72 completed=71`——**承载整帧 draw 的第 72 次提交此刻在飞、且无人等它**。这不是概率事件，是
代码结构，主机上确定性可复现。

主机**不**发散的原因也在这一行里：`layout=1` 是 `VK_IMAGE_LAYOUT_GENERAL`，走
`GetImageTransitionSourceState` 的 `default:` 臂（`ALL_COMMANDS` / `MEMORY_WRITE`），**碰巧**把 copy 排
在了 draw 之后。设备上的默认帧缓冲不是 GENERAL。

**集成者给的复现路径（pipeline 延迟旋钮）在主机上不成立，这里如实记录**：新增
`MOBILEGL_TEST_PIPELINE_CREATE_DELAY_MS`（split-only、未设即惰性，
`VulkanRenderer.cpp` `GetOrCreatePipelineWithInput` 的 miss 路径）确实生效——OpenRA DirectVulkan SPLIT
从 1.88 s 变 4.53 s——但 **ssim 一直是 1.000000**。原因就是上一段：主机那条臂的 barrier 是满的。所以
red-once 用的是 §5.4 的计数，而不是一张发散的图。

### 5.3 修复（两半，都只在 wire 臂）

1. **`RecordWireReadbackWriteBarrier()`**（`WireFramebuffer.inc`，新）：回读在 copy 之前**自己记一道**
   `ALL_COMMANDS → TRANSFER`、`MEMORY_WRITE → TRANSFER_READ|MEMORY_READ` 的内存 barrier，不再从布局
   **推断**该等谁。barrier 的第一作用域是本队列**提交序**里更早的每一条命令，所以它跨得过提交边界。
2. **`SubmitReadbackCommandsAndWait` 等齐所有在飞提交**（`VulkanRenderer.cpp`，`#if
   MOBILEGL_BUILD_DISAGGREGATED`）：收集 `m_inFlightSubmits` 里 `submitIndex <= frame.lastSubmitIndex`
   的每一个 fence 一起等。代价是零——回读本来就是一次完整的 CPU 停顿。
   **pull 构建保持单 fence 原样**：它没有 wire 臂、回读前不会 flush，而且是 G1 钉住的那个镜像。

### 5.4 red-once（R-16，已执行并还原）

打点回读实际等了几个 fence：

| | `fences` | `upTo` | `completed` |
|---|---|---|---|
| 修复后 | **2** | 73 | 71 |
| 修复前 | **1**（只有自己那一个） | 73 | 71 |

`completed=71` 两边都一样——即第 72 次提交确实在飞。修复前只等 1 个 fence 然后把 72 记成完成；修复后
两个都等。把 §5.3 的第 2 半还原即回到 `fences=1`，**这是确定性的、主机上的红**，不靠时序。

### 5.5 车道

`MOBILEGL_TEST_PIPELINE_CREATE_DELAY_MS=25` 下的三条回读用例（`VertexIdSamplerAndScalarUniformPixels`
draw→回读、`ComputeImageStoreFramebufferPixels` compute→回读、`GenerateMipmapPixels`）注册到三臂，
前缀 `.PipelineDelay.`。它们是**回归门而不是复现**：钉住「路径上每次 pipeline 创建都多花 25 ms 时，回读
仍然返回那次 draw 的像素」——即答案不随时序移动。

`integration-magma-split` **84**（+3）、`-spawn` **63**（+3）、`-tcp` **65**（+3），三臂 0 红；
OpenRA DirectVulkan SPLIT retrace ssim 1.000000（带与不带 25 ms 延迟各两遍）。

### 5.6 欠设备核对

集成者要在 Redmi 上重跑三遍冷 cache（`pm clear` 后）。预期：0.976494 → 1.000000。若仍发散，下一个嫌疑
按序是 `WireDraw.inc` 里十一处**静默丢弃 draw** 的 `return false`（尤其 `pipeline == VK_NULL_HANDLE`
那处，它与设备日志里那三行 `handing vkCreateShaderModule an INVALID module for program 2 stage 4`
在同一条路径上），以及 `SetImageContentDefined` 在 wire 臂是**画之前**就置位（monolith 是
`EndRenderPass` 之后），这会让「没有写入记录」那条警告恰好在它该响的时候不响。两条都不在本片修的范围
内，按名记在这里。
