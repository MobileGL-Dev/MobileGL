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

**G1 实测过了，而且这一条事先不是显然的**：`DriverPostIterationRPWitnessSpv.h` 是
`MG_Util/SelfTest` 的头，pull 构建也编它，重烘改了 2142 → 2143 个字。pull `.text` 仍是 **`0xa52203`**，
`nm --defined-only` 的名字表与 `~/w7/p7-before/pull-syms.txt` 逐行相同（30570 行）——字长的变化落在
`.rodata`，没有改动任何一条指令的编码宽度。`fatal_census` 绿（79 abort 站点 / 43 家族词 / 0 无标记，
本片**退掉一处** `Fatal{}` 用法但不动 abort 站点数）；`link_ratchet --assert-monotone` 186 不变，
`p7-magma` 桶 101。
