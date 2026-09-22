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
