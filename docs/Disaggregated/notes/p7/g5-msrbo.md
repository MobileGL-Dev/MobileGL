# P7 门 5 包 g5-msrbo：`KHR-GL46.direct_state_access.renderbuffers_storage_multisample`（inproc × DirectVulkan）

> 规范见 [`CONTRACT-P7.md`](../../../../MobileGL/MG_Remote/CONTRACT-P7.md) §7.3（门 5）与 §12（记录债）。
> 基线 = `p7/f2` 的 `f90afcd1`（pipe 头 `14e1c8b9` + 已审的四条 F2 提交）。分支 `p7/g5-msrbo`。

## 1. 根因（真机证明）

- **不是缺陷 (1)**：本例没有 `ReplyTooLarge`。
- 54 条 Fail = 6 个深 / 模板格式 × 3 个尺寸 × 恰好 3 次多重采样迭代（Adreno 上 `GL_MAX_INTEGER_SAMPLES` = 4，
  所以迭代 samples 为 1、1、2、4、4，红的是 2 / 4 / 4）；两次单采样迭代修复前就是绿的。
- `ResolveWireDepthStencil` 的 VK_KHR_depth_stencil_resolve 臂是一个**没有 draw 的 render pass**。在 Redmi
  （Adreno 830，驱动 512.800.71）上它不写 resolve 目标，深度与模板读回全是 0；同一个库加
  `MGITEST_MAGMA_FORCE_SHADER_DEPTH_RESOLVE=1`（只走烘焙 shader 臂）通过。
- 修复 `e80392ec`：`WireDepthResolveArm.h` 的 `WirePrefersShaderDepthResolve(vendorID)`，Qualcomm 上 shader
  臂先走、render pass 退为回落；其他厂商保持 B2 的顺序。**g5-msprobe 已把厂商键换成运行时探针（§5）**，
  臂顺序现在是设备上实测的结论。

## 2. 提交与 red-once

| 提交 | 内容 | red-once |
|---|---|---|
| `57181250` | F2 / PH-4 回归：dEQP `resetStateGLCore` 的 `glTexImage1D(width 0)` 被 server 当 `Fatal{ProtocolCorruption,"ResourceRespecify.ExtentCarrier"}`；零 extent 现在被接受 | 回退后 32 条中 26 条红（`StagedTextureStoreTest` 两条 + `EmptyTextureLevelScenario` 各 split 臂）。**已被 cherry-pick 进兄弟分支**（`fdd40f73`、`01f95c7b`、`8cc63c0b`），集成时去重 |
| `7c1a3b74` | wire 臂 1:1 单采样深 / 模板 blit 改走 `vkCmdCopyImage`（packed 格式按 aspect 经 buffer 往返），与 monolith 同形 | 旧 blit 代码下 14 条中 6 条红（`DsBlit.` 三臂 + 三条 `Full.`）；只在 lavapipe 上测过 |
| `e80392ec` | Qualcomm 上 shader resolve 臂先走 | 主机单测 `PipelineQuirkTest.WireDepthResolvePrefersTheShaderArmOnQualcommOnly` 只钉谓词；真机前后对照是证据 |
| 审查轮（已被 g5-msprobe 取代） | 服务端旋钮 `MGITEST_MAGMA_DEPTH_RESOLVE_VENDOR_ID` 只替换臂策略读的厂商 id；`ResolveWireDepthStencil` 每臂一次 INFO 行说明哪条臂 resolve 了；`DirectVulkan.{Split,Spawn}.MsResolveQ.` / `.MsFlipQ.` 设 Qualcomm id，断言像素**且** server 私有日志里 shader 臂 resolve 过、render pass 臂从未 resolve；`MsResolve1.` / `MsFlip1.` 同断言 | 去掉 `shaderFirst` 里的 `m_wirePreferShaderDepthResolve \|\|`：四条 Q 条目红（只红臂断言，像素仍绿）；把成员赋值的谓词短路成 `false`：同样四条红 |
| `52d1099b` | g5-msprobe：删厂商键，改由设备探针（带 shader 臂对照组）决定臂顺序；两条臂的定义抽成探针与生产共用的函数；旋钮 `MGITEST_MAGMA_DEPTH_RESOLVE_PROBE=bug\|clean` 替换**测量**；Q 条目改名 `MsResolveBug.` / `MsFlipBug.`；`MsResolve0.`（split/spawn）钉住 lavapipe 上真实探针 verdict=clean | 见 §5 |
| `d2f2971a` | g5-msprobe：Vulkan POST「Known Driver Bugs」新增该行（FIXED；无 stencil export 时 UNFIXABLE） | 单测 `DriverBugProbes.DepthStencilResolvePassRow…`（R1 下红） |

`7c1a3b74` 的第一稿由身份不明的写者写进本包工作树（`~/w7/logs/g5-msrbo/foreign.diff`）；提交前去掉了它的旋钮
`MGITEST_MAGMA_HIDE_DEPTH_STENCIL_BLIT` 与「缺 BLIT 特性就 decline」分支。集成时按外来代码审。

## 3. 证据

- 主机 CTS 单例（lavapipe，pbuffer，renderer 串已确认是 Magma）：base `f90afcd1` monolith Pass / inproc SIGABRT
  （dEQP 复位时的 ExtentCarrier fatal）；pipe 头 `14e1c8b9` 与本包头两臂都 Pass。主机**复现不了** Adreno 的
  render pass 缺陷（lavapipe 的 render pass resolve 是对的），所以审查轮的主机钉子钉的是臂**顺序**，不是 Adreno 的像素。
- 真机单例（flock 下，自己的 `/data/local/tmp/mgcts-g5-msrbo`，事后删除、回 Home）：修复 1 → 54 Fail；修复 1+2 →
  54 Fail；修复 1+2 + FORCE_SHADER → Pass；头 `e80392ec` inproc 3/3 Pass、monolith Pass。
- 真机五块（inproc，头 + `d655bfe8`）：dsa 367 P / 2 F / 1 CW / 1 X（X = `renderbuffers_storage`，归 g5-readback），
  shader-image 65 / 56 NS / 3 F / 1 X（X = `incomplete_textures`，归 g5-imgwin），ssbo 124/124，texture 823 / 221 NS / 10 F，
  packed-pixels 4732/4732。**这组数字用的是头 + `d655bfe8`（F2 的 R8 / 无字节层修复），不是头本身**：没有它 dsa 多 11 个崩溃，
  全是 R8 纹理（`TextureInternalFormat::R8 == 0` 被 PH-4 当成「层未声明」）。

## 4. 记录债（写给 CONTRACT-P7 §12 的几行）

1. **探针判坏的设备上 shader 臂 decline 后回落到 render pass**：shader 臂（`WireMultisampleResolve.inc`，
   `ResolveWireDepthStencilWithShader`）对无 `VK_EXT_shader_stencil_export` 的模板 aspect、非 `SAMPLED_IMAGE |
   DEPTH_STENCIL_ATTACHMENT` 的格式、pipeline 创建失败 decline，此时走的正是探针测出不写目标的 render pass——读回 0、
   无 GL 错误。无 stencil export 的设备在 POST 上是 UNFIXABLE 行。审查轮加了
   `MGLOG_W_ONCE`（`WireFramebuffer.inc` 的 `ResolveWireDepthStencil`，「this device prefers the shader pass, which did not
   resolve …」），不是修复。Redmi 有 stencil export（设备 logcat 可见），门 5 不受影响。
2. ~~Mali 等其他 tiler 未测~~ / 3. ~~按厂商而不是按驱动版本~~：g5-msprobe 起由每台设备自己的探针判定，两条均已关闭。
   剩余：探针只测 4x、4x4、整幅 render area 的形状；某驱动若只在别的采样数 / 尺寸 / 子矩形上坏，探针看不到。
4. **非 1:1 的单采样深度 blit**（缩放、翻转、default framebuffer）在 Adreno 上对无 `BLIT_DST` 的深度格式仍调
   `vkCmdBlitImage`——超出 Vulkan 规范，驱动照做，monolith 同样如此；现在只有 `WireFramebuffer.inc` 的
   `BlitWireFramebuffers` 里一条 `MGLOG_W_ONCE`。
5. **跨格式的 wire 深度 blit** 不走 monolith 的 `BlitDepthAcrossFormats`（`VulkanRenderer.cpp:9691`），仍是 `vkCmdBlitImage`。
6. **tcp 带不了 `MsResolveBug.` / `MsFlipBug.`（原 Q 条目）**：与 B2 记录债 (4) 同一原因（tcp server 是车道 fixture，吃不到每条目的
   server 环境），`spawn_lane_parity.py` 的 `MAGMA_SERVER_ENV_KNOB_NO_TCP` 具名。

## 5. g5-msprobe：厂商键换成运行时探针（`52d1099b`、`d2f2971a`）

规则（2026-08-22）：「驱动声称能做却做不到」一律是 POST 探针，不按厂商 / 驱动名加 quirk；每个探针带对照组。
`WirePrefersShaderDepthResolve` / `kWireVendorIdQualcomm` / `MGITEST_MAGMA_DEPTH_RESOLVE_VENDOR_ID` 已删除。

- **探针**（`WireDepthResolveProbe.{h,cpp}`，只进 disaggregated 构建）。两条臂的定义（render pass 描述、视图形状、
  shader 臂的 module / pipeline / draw）抽成共享函数，`ResolveWireDepthStencil`、`ResolveWireDepthStencilWithShader`
  与探针调同一份。格式：D24S8、D32FS8、D16、X8_D24、D32F、S8 中设备支持 4x 多采样附件（生产 usage）且 render pass 臂会被选中
  （该 aspect 有 SAMPLE_ZERO）的。每格式：两个 4x4 4x 多采样源，按 wire 清屏方式（LOAD pass + `vkCmdClearAttachments`）
  清成深度 0.25 / 模板 0x5A；单采样 resolve 目标（生产 scratch usage）预填哨兵 0.75 / 0xA5。**主体** = 生产 render pass
  （SAMPLE_ZERO，无 draw）；**对照** = 生产 shader 臂吃另一个同值源（每 aspect 一个目标，模板要 stencil export）。
  逐 aspect 拷进 host buffer、逐 texel 比较（UNORM 容差 1 LSB）。一次提交，5 s 有界等待（超时即泄漏对象，不 idle 设备）。
- **判定**（`WireDepthResolveArm.h`，纯函数）：只计对照成立的 aspect；任一 aspect 对照对而主体错 →
  `render-pass-resolve-broken` → shader 臂先走；对照成立处主体全对 → `clean`；没有任何对照成立 → `inconclusive`
  （保持默认顺序，W 日志一次）；没跑 → `not-run`。
- **运行点**：server 侧 `VulkanRenderer::ArmWireDepthResolveOrder`，设备创建末尾（`ArmPrimGenReroute` 之后，同样记录在
  `m_graphicsQueue` 上），monolith 与 `FORCE_SHADER_DEPTH_RESOLVE` 不跑；每进程一次。日志：一行 verdict + 每格式一行读数。
- **旋钮** `MGITEST_MAGMA_DEPTH_RESOLVE_PROBE=bug|clean` 替换的是**测量**（罐头读数），仍走判定函数。条目
  `DirectVulkan.{Split,Spawn}.MsResolveBug.` / `.MsFlipBug.` 断言 verdict 行、shader 臂 resolve、render pass 臂从未
  resolve；`MsResolve0.` 在 split/spawn 带 `MGITEST_EXPECT_DEPTH_RESOLVE_PROBE=clean`（只有场景读），断言真实探针在 lavapipe
  上 verdict=clean 且 render pass 臂 resolve（lavapipe 六个格式主体 / 对照都是 16/16）。
- **POST**：`DriverBugProbes` 新增 Vulkan 表 `CollectVulkanKnownDriverBugs`（disaggregated 构建），在 POST 自己的临时设备上
  跑同一个探针；行「Depth/stencil render-pass resolve writes nothing」：有 stencil export → FIXED，无 → UNFIXABLE；
  clean / inconclusive / not-run 不出行。
- **单测**：`PipelineQuirkTest.WireDepthResolveProbe*` 五条（假结果源：bug、clean、对照失败 → inconclusive、无 render pass
  臂不跑、旋钮只换测量）；`DriverBugProbes.DepthStencilResolvePassRowIsFixed…`。
- **red-once**（`~/w7/logs/g5-msprobe/red.log`）：R1 判定函数永不报缺陷 → 22 条中 8 红（四条 Bug 条目只红臂断言、像素
  0 红，另 4 条单测）；R2 探针主体读数恒为 0（lavapipe 上的假阳性）→ `MsResolve0.` split / spawn 2 红；复原后 22/22 绿。
- **真机**（Redmi 2f7cbe2e，flock，`/data/local/tmp/mgcts-g5-msprobe` 事后删除、回 Home；库 = 本头的 arm64 构建，
  未带 `d655bfe8`）：六个格式全部「render pass 0/16（哨兵 16/16 原样）/ shader control 16/16」，
  `verdict=render-pass-resolve-broken`；用例 inproc 3/3 Pass、monolith Pass（不跑探针）；同库加
  `MGITEST_MAGMA_DEPTH_RESOLVE_PROBE=clean` → Fail（臂顺序就是差别）；POST（经假 JNIEnv 调 JNI 入口）Vulkan 节出 FIXED 行。
- **门**（头 `d2f2971a`）：G1 .text 0xa52203、符号 +0/-0（探针与 POST 表都在 `MOBILEGL_BUILD_DISAGGREGATED` 内）；
  fatal_census 79/20、0 unmarked；link_ratchet 173 不变；spawn_lane_parity 0 错；wire_declines 53/53、自测 16/16。
  车道（失败 / 总数）：unit 8/2460、split 15/307、spawn 15/223、tcp 15/226、magma-split 3/107、magma-spawn 2/86、
  magma-tcp 2/74、magma-full-split 17/543——失败集合与 `e419d010` 逐名相同（新增 0、消失 0，全是基线的 F2 问题）；
  头 + `d655bfe8`（临时树，事后删除）八条车道全 100%（2462/307/223/226/107/86/74/543）。
