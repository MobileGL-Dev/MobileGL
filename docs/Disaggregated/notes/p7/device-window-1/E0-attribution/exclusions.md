# E0 · 出口门 3 分母之外的 case（按名单列，不在汇总表里悄悄删行）

E0a monolith 臂（DirectVulkan × `--use-pbuffer` × 1 遍，APK `p7w1-9be62cbc`，Redmi `2f7cbe2e`）39 例全部回放、全部逐位确定（`ssim vs first = 1.0`），**3 例低于阈值**。它们都在 **monolith** 上就红，所以按 runbook §4 的判别「monolith 也分歧 ⇒ 问题不在分离」，从出口门 3 的分母里排除；分母 = 39 − 3 = **36**（另：rd12 in-world 本来不在 `--matrix` 里，ID-P7-4）。

| case | monolith SSIM | 阈值 | 原因 | 归属 |
|---|---|---|---|---|
| `minecraft-1.21.1-neoforge-create-indirect-in-world` | 0.000005 | — | ROADMAP 开放问题 17：`dev@81b17c0b` 起在 Adreno 830 上就坏（基线 APK 复现），非本分支 | **ID-P7-4 预先排除**；`dev` 侧 |
| `minecraft-1.21.11-main-menu` | 0.164984 | — | trace 自身的 UBO offset 不满足本机 32 字节对齐（`../../p65/mainmenu-trace-difference.md`；DirectGLES 上同一原因给 0.890） | trace 内容，非分离缺陷；不改阈值 |
| `minecraft-1.21.4-fabric-iris-photon-v1.3b-in-world` | 0.008865（mismatch 403024 px） | 0.99 | **`vkCreateGraphicsPipelines` 在 Adreno 830 上对 photon 的多个程序返回 `VK_ERROR_UNKNOWN (-13)`**（`Renderer/PipelineFactory.cpp:660`，`programHash=0xf25a0f918681264f / 0xecb48583183184c4 / 0x7f5d6d38463579e2 / 0x733247df7346812c / …`，两个 color attachment 的 pass 与单 attachment 的 pass 都有，共数百次；`GetOrCreatePipeline ... not caching the failure` 后每 draw 重试），随后默认 framebuffer 无写入、回读原始像素 | **Magma × Adreno 驱动的 monolith 级失败**，不是分离缺陷。桌面 lavapipe 上该 case 是绿的，所以是 Adreno 的 SPIR-V/管线编译拒绝——按 OQ-17 的先例归 `dev` 侧独立条目（拆分不借机顺手修）；若 wave 2-B 顺路碰到 photon 的 shader 形状可以带上，但不进 P7 出口门 |

证据：`../E0-attribution/E0a-monolith-summary.{txt,json}`；photon 的原始日志在 `.trace-work/p7w1/E0a-monolith/minecraft-1.21.4-fabric-iris-photon-v1.3b-in-world-DirectVulkan/repeat-01/{mobilegl.log,logcat.txt,result.json}`（不入库，>2 MiB；上面引用的行可用 `grep -c 'PipelineFactory.cpp:660' mobilegl.log` 复核，本次为 390）。

**monolith 臂的其余 36 例**：SSIM 对 golden 在 0.996–1.000 之间（iris 光影 case 最低：makeup-ultrafast 0.99663、sundial-lite 0.99617、bsl 0.99701），都在各自阈值内；OpenRA 1.000000、mismatch 0。这说明「pbuffer monolith = 1.0」（`CURRENT_STAGE_PROGRESS.md:158`）只对 OpenRA 成立——**出口门 3 的判据是逐 case 「inproc / spawn 的 SSIM 与 monolith 臂的 SSIM 一致（同阈值、逐位或 ssim-vs-monolith ≥ 阈值）」，不是对 golden 的平 1.0**。这一句写进 `CONTRACT-P7.md` §7.2。
