# 实测记录（摘要）

> 每个阶段只留头条数字与结论。完整的表、命令、协议与读法在对应 `notes/<阶段>/README.md` 的"实测"节，**小节编号 §N.x 原样保留**（代码与旧笔记里的 `MEASUREMENTS.md §7.2` 这类引用，按下面每节的"全文"链接去找）。
>
> **设备**：`35d0befa` = Xiaomi 24129PN74C（SM8750 / Adreno 830v2）与 `3B159D009VZ00000` = Oppo PLG110（MT6993 / Mali）用于 P0–P3a；`2f7cbe2e` = Redmi M332BF（SM8750 / Adreno 830v2）是 P4a 起唯一的 A/B 设备。定频、风扇与热协议见 [`devices/pin-verification-2026-09-07.md`](devices/pin-verification-2026-09-07.md)；Redmi 的 GPU 钉频点随时期变过（2026-09-11 为 1050 MHz，P5b 时 1100 MHz，P6 起本板实测钳在 1050 MHz），跨钉频口径的活动不可比钟频，只有同会话配对可比。
>
> **口径**（用户 2026-09-08 起）：性能对着 pull 臂**记录**、不作阻塞门；正确性门是硬门；主指标是**逐线程 CPU 时间**。三条读数纪律：`acc/draw` 是静态下界，判性能只看 CPU 时间；拒绝普查必须逐用例读私有日志（`ctest -V` 是假零）；spawn 下 client 不推进帧窗口，窗口化字段只在推进帧的那条日志上有效。

## 1. P0（2026-09-05）

- spike A：Adreno 与 Mali 上，`untrusted_app` 进程 `fork`+`execve` `libMobileGLServer.so` 均 OK（同域、零 avc denial）。
- spike B：**T0**（AHB BLOB 交接）是唯一在两设备、两后端上完整读写的档；T1 opaque fd 仅 Adreno；GLES memobj 在 Adreno 上 map 失败；host-pointer 在 Mali 只读。
- 计数器基线：真机稳态动态 accessor 每 draw 6.5–9.3 次；Magma 在 MC 26.3 每帧重打包 331 KB 具名 UBO；同样 185 次纹理发射，Espryt 整 box 635 KB/帧 vs Magma rect 40 KB/帧。

全文 §1.1–§1.5：[`notes/p0/README.md`](notes/p0/README.md)。

## 2. P1（lavapipe / llvmpipe）

- 规模：277 个 `pGLContext->` 站点 + 58 行非箭头、63 个 `PipeInputs` 字段、83 条填充点。
- verify 通道找到 9 处缺填充行与 3 个"verb 内后端改前端"字段（→ push-on-mutation）。
- 验收：pull 符号 0/0/0/0；单元 1485×3；`integration-verify` 818 零 `Fatal{`；79 例 retrace verify 零分歧。

全文 §2.1–§2.3：[`notes/p1/README.md`](notes/p1/README.md)。

## 3. P2（`738b289d`）

- 门：G1 4 处认定 resize、`.text` +160 B；单元 1566×3；`integration-gpu` 916 三臂；retrace push / verify 79/79。
- DriverBench（桌面）：边界 T1 = Espryt +322 / Magma +345 ns/draw；P2 自己的 tracker + CSO 比它替掉的 P1 残余填充便宜（−228 / −354 ns）。
- 设备 A/B 的首轮是 -O0 构建（勘误，不作基准）；Release 基准见 §4.4。

全文 §3.1–§3.5：[`notes/p2/README.md`](notes/p2/README.md)。

## 4. P3a（`fde5fda3`）

- 五部分门全绿：G1 0/0/0/0、`.text +0`；G5 十一函数逐字节相同；单元 1619×3；`integration-gpu` 958 五臂。
- **Release 两机配对 A/B**：P2 边界 +6–12%（两机两后端一致）；P3a 在 26.3 / sodium 上几乎不再加价，**在 rd12 上把差距推到 +27–30%**（VAO 切换密）。MC 26.3 在 Adreno 上 p99 +3.4%。
- DriverBench：P3a 自己每 draw Espryt +699 / Magma +422 ns。

全文 §4.1–§4.5：[`notes/p3a/README.md`](notes/p3a/README.md)。

## 5. P4a（`8c458cd5`）

- 全门：G1 0/0/0/0；G5 17 区；单元 1785×3；`integration-gpu` 1117 七臂；verify 920；八族拒绝普查 0。
- **Redmi 三臂 A/B**：P4a 自己在 Espryt 上 +3.4 – +5.7 pt（0.05–0.38 ms/帧），Magma 在噪声内（消费者门让它一条不发）；MC 26.3 p99 +6.1%；上传形状 pull 与 push 逐项相同。
- 桌面 DriverBench 分辨不出 P4a 这一档；可引用的是总边界 ≈ +1.1 µs/draw（Espryt）。

全文 §5.1–§5.4：[`notes/p4a/README.md`](notes/p4a/README.md)。

## 6. P5（`e61d0012` → `37fc4fdb`）

- joint：OpenRA inproc 2/2 SSIM 1.0；`integration-split` 21/21；E1 barrier 阴性对照 14/14 全红；E2 丢 758 条 `DrawVbo` 后 SSIM 0.000036（丢 clear 被 overdraw 掩盖，不能作控制）。
- 普查：inproc 1149 项中 27 个 wrong-answer（后续去向见 §7.2 末：2026-09-22 重跑时 24 绿、3 条变为具名停止）。
- `maxrec` = 784 B（默认 cap 4 MiB 的 0.019%）；ledger SEG_CMD 8 MiB / SEG_STAGE 32 MiB / SEG_REPLY 16 MiB / SEG_EVENT 256 KiB。
- Redmi 四臂：split 臂全部停在索引 draw，barrier tax 未测。

全文 §6.1–§6.5：[`notes/p5/README.md`](notes/p5/README.md)。

## 7. P5b（主机门 `348d22a4`，设备 `82683d4a`）

- 主机：`integration-split` 107/107；合并 inproc 普查 1267 项 811 / 203 / 62 / 191，旧 passes 零回退；79 trace 72 / 6 / 1。同名重跑 @ `3c80cd62`（2026-09-22）：27 个 wrong-answer → 24 绿 / 3 具名停止；1267 项 → 1051 / 172 / 37 / 7（非绿全是钉了非默认 `MOBILEGL_PIPE_PUSH` 的对照臂）；trace 77 / 0 / 0。
- **Redmi**：正确性 8/8；**barrier tax**（split − push 逐线程 CPU p50）+5.9% – +18.2%。

全文 §7.1–§7.4：[`notes/p5b/README.md`](notes/p5b/README.md)。

## 8. P5c（`11ac3de6..b88e8487`）

- 守卫开启下 unit 2187/2187、`integration-split` 111/111，每层守卫 red-once；纹理 `0xDD` audit 在两条 in-world trace 零 Fatal。
- `rsp` 按帧：bsl 948.5/帧、complementary 1591.9/帧，残留全为对象类 15 行（值类 0）。
- G1：3 个认定 resize、`.text` −16 B。Redmi 四臂复测未做。

全文：[`notes/p5c/README.md`](notes/p5c/README.md)。

## 9. P5d（Redmi，FCL + Minecraft 26.3，VD12，DirectGLES）

| 臂 | fps p50 | client ms/帧 | apply ms/帧 |
|---|---|---|---|
| inproc 起点（`56a77348`） | 64.3 | 15.03 | 12.42 |
| inproc 三轮（`1f8de61b`） | 103-106 | 9.2 | 7.0-7.4 |
| monolith | 115（120 Hz 封顶）/ 206（未封顶一次） | 5.0-6.2 | — |

全文：[`notes/p5d/README.md`](notes/p5d/README.md)（报告 [`notes/p5d/P5D-INPROC-PERFORMANCE.md`](notes/p5d/P5D-INPROC-PERFORMANCE.md)）。

## 10. P5e 设备验证一轮（`3cc4e1ec`）

- `integration-gpu` 两条运行时臂从 1157/1294（137 SEGFAULT，当时没有任何门会看到）修到 1294/1294——此后它是常设步。
- 设备：monolith 修复前 65 s 后进程消失，修复后两臂都存活进世界。

全文：[`notes/p5e/README.md`](notes/p5e/README.md)。

## 11. P5e run-ahead 武装后的设备矩阵（`25fba0d5`）

- VD12（~850 draws/帧，CPU 定频）：run-ahead 相对 lockstep client −18%、apply −33%、fps +6%；monolith 与 run-ahead 都撞 120 Hz 上限，只有 lockstep 撞不到。
- **VD32（~3550 draws/帧）**：monolith 58.5 / run-ahead **59.9** / lockstep 35.5 fps（中位数）——**inproc 与 monolith 齐平**；run-ahead 相对 lockstep p50 +69%、client CPU −40%。两侧不平衡（apply 11–12 ms vs client 15.7 ms）是下一个优化方向（开放问题 18）。
- 未定频的逐线程 CPU 不能跨臂比较（walt governor 会把 policy0 从 2745 拉到 748 MHz）。

全文 §11、§11.1：[`notes/p5e/README.md`](notes/p5e/README.md)。

## 12. P6 出口测量（2026-09-22，Redmi + WSL）

- **门 7 配对 A/B**（rd12 in-world，~1471 draws/帧，两会话各一次 reboot）：inproc vs spawn 的 client CPU p50 **−0.15% / +0.16%**，在臂内散布内，判读 **tie**；monolith 约贵 30%。
- **门 8①** 门铃：client 121.1 waits/帧两传输逐字相同；server park 率 0.042–0.06%；socket 阻塞读既不更便宜也不更贵。
- **门 8②③**（inproc 桌面 retrace）：`SEG_STAGE` 稳态中位数 OpenRA 130.8 KB/帧、rd12 816.2 KB/帧；分块后记录/帧 211 / 7 497；`maxrec=1808`，默认 8 MiB chunk 预算从未被打到。负控 `STAGE_MB=1` 下 `seg` 运行总量逐字节不变。

全文 §12.1–§12.7：[`notes/p6/README.md`](notes/p6/README.md)。

## 13. P3b/P4b wave 2-D 包 D2（`cf7ca59f`）

- 上传形状金标四臂（monolith / inproc / spawn / tcp）同为 `emit=9 box=9 rect=0 jobs=9`；零补丁 red-once：`MOBILEGL_ESPRYT_DISABLE_UNPACK_RING=1` 翻成 `box=3 rect=6 jobs=183`。
- Mali 帧时增量不可调度（无 Mali 设备），按 Adreno 记录。

全文 §13.1–§13.3：[`notes/p34b/README.md`](notes/p34b/README.md)。

## 14. P6.5 第一波（`fe28bdb5`）

- 主机 unit 2399、integration-tcp loopback 102/102；**Redmi device-102 102/102，逐条 run-ahead ARMED**；代表性 retrace OpenRA 1.0、startup 0.999999511、in-world 0.999995438。
- 真实远端 kill **133 ms**、真实 Wi-Fi 中断 **5053 ms** 闩住 device loss。G1 0/0/0/0。
- 未测：39 例 device golden 矩阵、逐帧 `kWaitReply` / RTT、`PRESENT_CREDIT` 1/2/3、tcp-vs-spawn 逐线程 CPU。

全文：[`notes/p65/README.md`](notes/p65/README.md)（`performance.md`、`validation-status.md`、`wire-and-validation.md`）。

## 15. P7（2026-09-23）

- 门 3（终局窗口 3，`p7w8-78e71f2b`）真机 ssim **36/36**：33 例四臂逐位同，三例 Iris 按分布判据（ID-P7-62），OpenRA 四臂 1.000000。
- 门 5 CTS 五块 AFTER：shader-image +4.35 pp、texture +0.96 pp、其余持平，0 新 crash。
- Magma 两进程车道 split / spawn / tcp 102 / 81 / 71；M2 后主机 bsl spawn server 825 → 546 MiB、映射 41k → 5.8k。

全文：[`notes/p7/README.md`](notes/p7/README.md)（证据 `notes/p7/device-window-1/`、`device-window-2/`）。

## 16. P12 子集（2026-09-24，未收官）

- 真机七项检查（审查轮之前的 `29b7b284`）：Espryt / Magma 上屏 OpenRA ssim 1.000000，同进程第二会话 0.999999511；失窗后新会话 1.0；离屏 service 1.0。审查轮之后未复测。
- 主机门（审查轮）unit 2605/2605；G1 同机前后 `.text` 一致、符号 0/0。

全文：[`notes/p12/README.md`](notes/p12/README.md)、[`notes/p12/PLAN-P12.md`](notes/p12/PLAN-P12.md) §2。
