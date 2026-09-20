# P5f Redmi 设备门

2026-09-20，指定序列号 `2f7cbe2e`，Xiaomi `M332BF` / Android 16 / SM8750。
验证库源头 **`cfca93c7885fd8db1e881f91189ba59090ae9c52`**，包含跨模型审查的两项修复。
每臂先 reboot，等待 boot complete；六个 boot-id 不同。没有 pin 时钟、清 app 数据、安装 APK
或修改 FCL 设置。测试是独立 shell ELF + AImageReader native window，真实硬件 GPU。

P5F §6.7 的 A/B 取本阶段中心机制：同一 split binary、inproc、strict=1、role0/role1。
额外单体 sanity 也使用同一 binary。P5e 的 MC 30秒/FPS性能阈值没有被移植成 P5f 新要求；
这里的性能只记录，不宣称游戏性能改善或完整 P7 工作负载已支持。

## 身份与结果

NDK 27.3、API26、arm64、RelWithDebInfo；仅在部署副本去掉 debug 数据，原始符号保留。
三份 ELF 的设备 hash 与主机逐项一致：

- `libMobileGL.so`: `764a790d024fbe1b83ebdec5d896cc77ecda4f4a2698d63d2b2d16239f40e74c`
- `P5fDevicePixels`: `76c81e540f723782141054ecc6e99b6a92fff1a55f413d63796ff53e630cec0b`
- `libc++_shared.so`: `d523468d62d9b603cb3354294d70d4b2feabf2c3f1e43b0c96c9aabf32813708`

| 臂 | PASS | skip | vbs 合计 | stats windows | suite 秒（仅记录） | boot-id |
|---|---:|---:|---:|---:|---:|---|
| GLES-monolith | 11 | 0 | 0 | 6 | 0.518 | `9497f21b-f91f-4d95-a403-5729eb537d5f` |
| GLES-role0 | 11 | 0 | 72 | 6 | 0.741 | `02acddae-f9f8-4fff-bfed-bf1ac03ea0b2` |
| GLES-role1 | 11 | 0 | 72 | 6 | 0.314 | `17f16009-86dd-49c4-9da5-bc3bf377e827` |
| Magma-monolith | 9 | 2 | 0 | 6 | 0.259 | `db46e8b0-b74e-4c3f-a97f-a38c373fa2a9` |
| Magma-role1 | 9 | 2 | 68 | 6 | 0.369 | `8d0672c7-bfb1-45d9-8436-764a074a1087` |
| Magma-role0 | 9 | 2 | 68 | 6 | 0.543 | `bfd92cb2-7854-4e54-99ad-de911660ab5e` |

**所有六臂 failed=0，36个统计窗口全部 rsp=0**。GLES 的两 inproc 臂 vbs 同为72，Magma
同为68；单体 vbs=0。运行日志也核了实际 backend、inproc strict/role配置、Espryt run-ahead
ARMED 与 Magma 不发布能力而 lockstep 的明确记录，不能只用环境回显当跑对臂的证据。

Magma 每臂两项 skip 是同一组既有功能限制：
`ClipDistanceScenario.ADisabledClipDistanceRemovesNothing` 与
`ClipDistanceScenario.TheEnablesAreIndependentPerDistance`。程序在像素观察后明确报出
“不实现 per-distance enable masking”的原因；单体、role0、role1一致，不是 preflight/GPU
不可用，也没有计为 PASS。其余9项必须真正通过。GLES 的11项全部真正通过。

## 覆盖与局限

公开 GL 用例为 ClipDistance 三项、CopyImageLayered 六项，以及独立 public mipmap 两项：
CPU红底生成 mip 链；先GPU写绿后生成链且不得被旧CPU红影子覆盖。全部 mip/base 都核像素。
Android 原有 CMake 提前 return，F1/Ct/DualBlock 与私有 peek 不进入 Android binary，因此本次
没有冒称那些 host 私有用例在手机执行，也没有添加 shipping export ABI。

运行时间含测试初始化/驱动缓存影响，只有一次 clean-boot 样本，不能据此推导性能回归或提升。
已保留 P7 buffer/native-format、P8 client arrays、P12窗口到达等具名边界；它们不属于本门
选择的可执行子集。

## 复现与证据

可复用工具已入仓 `tools/device_bench/p5f/`，包含构建/运行/验证脚本及独立 public mipmap 源。
入仓 verifier 核对确切的 11 个唯一用例及每条 `status=run`、`result=completed/skipped`，
已只读复核全部六臂。复制证据的 missing / duplicate / disabled 三个反例均在对应断言拒绝；
原证据文件 hash 前后不变，控制结果保存在
`C:/Users/geekerwan/.codex/tmp/p5f-verify-arm-controls-7nrrujzt/`。两个 shell 脚本的
`bash -n` 与 Python 语法检查也通过。
原始六臂证据：`C:/Users/geekerwan/.codex/tmp/p5f-redmi-exit-cfca93c7/`，每臂有 XML、stdout、
库日志、boot-id、设备/主机SHA256、退出码及 crash-buffer；`summary.json` 汇总上表。
最终 bundle：`/home/swung/w7/p5f-android-cfca93c7885f/bundle`。
Android增量构建复用先前 cache，完整库与符号在
`/home/swung/w7/p5f-android-de78f47df595/library`；其最后 reconfigure/build 对应最终源头。
审查修复前的 de78 预跑证据另存，未混入上表、未代替最终库的六臂复跑。
