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
  臂先走、render pass 退为回落；其他厂商保持 B2 的顺序。

## 2. 提交与 red-once

| 提交 | 内容 | red-once |
|---|---|---|
| `57181250` | F2 / PH-4 回归：dEQP `resetStateGLCore` 的 `glTexImage1D(width 0)` 被 server 当 `Fatal{ProtocolCorruption,"ResourceRespecify.ExtentCarrier"}`；零 extent 现在被接受 | 回退后 32 条中 26 条红（`StagedTextureStoreTest` 两条 + `EmptyTextureLevelScenario` 各 split 臂）。**已被 cherry-pick 进兄弟分支**（`fdd40f73`、`01f95c7b`、`8cc63c0b`），集成时去重 |
| `7c1a3b74` | wire 臂 1:1 单采样深 / 模板 blit 改走 `vkCmdCopyImage`（packed 格式按 aspect 经 buffer 往返），与 monolith 同形 | 旧 blit 代码下 14 条中 6 条红（`DsBlit.` 三臂 + 三条 `Full.`）；只在 lavapipe 上测过 |
| `e80392ec` | Qualcomm 上 shader resolve 臂先走 | 主机单测 `PipelineQuirkTest.WireDepthResolvePrefersTheShaderArmOnQualcommOnly` 只钉谓词；真机前后对照是证据 |
| 审查轮 | 服务端旋钮 `MGITEST_MAGMA_DEPTH_RESOLVE_VENDOR_ID` 只替换臂策略读的厂商 id；`ResolveWireDepthStencil` 每臂一次 INFO 行说明哪条臂 resolve 了；`DirectVulkan.{Split,Spawn}.MsResolveQ.` / `.MsFlipQ.` 设 Qualcomm id，断言像素**且** server 私有日志里 shader 臂 resolve 过、render pass 臂从未 resolve；`MsResolve1.` / `MsFlip1.` 同断言 | 去掉 `shaderFirst` 里的 `m_wirePreferShaderDepthResolve \|\|`：四条 Q 条目红（只红臂断言，像素仍绿）；把成员赋值的谓词短路成 `false`：同样四条红 |

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

1. **Qualcomm 上 shader 臂 decline 后回落到 render pass**：shader 臂对无 `VK_EXT_shader_stencil_export` 的模板 aspect
   （`WireMultisampleResolve.inc:65`）、非 `SAMPLED_IMAGE | DEPTH_STENCIL_ATTACHMENT` 的格式（`:75`）、pipeline 创建失败
   （`:224`）decline，此时走的正是在 Adreno 上不写目标的 render pass——读回 0、无 GL 错误。审查轮加了
   `MGLOG_W_ONCE`（`WireFramebuffer.inc` 的 `ResolveWireDepthStencil`，「this device prefers the shader pass, which did not
   resolve …」），不是修复。Redmi 有 stencil export（设备 logcat 可见），门 5 不受影响。
2. **Mali 等其他 tiler 未测**：仍是 render pass 先走。厂商表只按实测增长。
3. **按厂商而不是按驱动版本**：若某个 Adreno 驱动修好了 no-draw resolve，本策略察觉不到（只是少走一条快路径，不会错）。
4. **非 1:1 的单采样深度 blit**（缩放、翻转、default framebuffer）在 Adreno 上对无 `BLIT_DST` 的深度格式仍调
   `vkCmdBlitImage`——超出 Vulkan 规范，驱动照做，monolith 同样如此；现在只有 `WireFramebuffer.inc` 的
   `BlitWireFramebuffers` 里一条 `MGLOG_W_ONCE`。
5. **跨格式的 wire 深度 blit** 不走 monolith 的 `BlitDepthAcrossFormats`（`VulkanRenderer.cpp:9691`），仍是 `vkCmdBlitImage`。
6. **tcp 带不了 `MsResolveQ.` / `MsFlipQ.`**：与 B2 记录债 (4) 同一原因（tcp server 是车道 fixture，吃不到每条目的
   server 环境），`spawn_lane_parity.py` 的 `MAGMA_SERVER_ENV_KNOB_NO_TCP` 具名。
