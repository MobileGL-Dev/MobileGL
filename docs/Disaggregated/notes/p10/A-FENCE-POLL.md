# P10 A：fence 轮询本地作答——配对测量与 Espryt 发现

2026-09-29，`p10/main@8d0a1b6c`，主机 WSL Arch（Espryt = mesa llvmpipe ES 3.2；Magma = lavapipe）。协议与门见 [`CONTRACT-P10.md`](../../../../MobileGL/MG_Remote/CONTRACT-P10.md) §1。只记录、不设门。

## 负载

`FencePollScenario.PacedFrameFenceWaitsBench`（`MGITEST_FENCE_PACE_BENCH=1`）：每帧 clear + `glFenceSync` + 交换，再对 N 帧前（`MGITEST_FENCE_PACE_LAG`，默认 2）的 fence 做 `glClientWaitSync(FLUSH, 1 s)`；600 帧。也就是 Minecraft 的形状，这里单独拿出来测（回放语料里只有 `improved-transparency-minecraft-26.3` 带 timed wait，见 [`C-MEASUREMENTS.md`](C-MEASUREMENTS.md) §3）。A/B = `MOBILEGL_IPC_POLL_ESCALATE=0`（每次都往返，P10 之前的行为）对默认 64。

## 结果（3 轮，ms/帧 与 600 帧里的 fence 往返次数）

| 后端 · 传输 | esc=0 ms/帧 | esc=64 ms/帧 | esc=64 往返 / 本地作答 |
|---|---|---|---|
| Magma · inproc | 0.201 / 0.206 / 0.217 | 0.200 / 0.206 / 0.214 | 0 / 598（回复槽记录 601 → 3） |
| Magma · tcp | 1.017 / 1.007 / 1.004 | **0.631 / 0.651 / 0.606**（−37%） | 0 / 598 |
| Espryt · inproc | 0.816 / 0.672 / 0.816 | 0.808 / 0.843 / 0.848 | 595–598 / 0–3 |
| Espryt · tcp | 0.891 / 0.939 / 0.908 | 0.796 / 0.891 / 0.800 | 497–519 / 79–101 |

表中是 `625f4db4` 的配置。证据：`~/w7/notes/p10/evidence/bench-after-present-report.log`；改动前的一组在 `bench-before-present-report.log`，数字相近。

## present 时报告（`625f4db4`）

server 在 `OnPresent` 里、归还 credit **之前**查一次未报告的 fence（状态查询，不提交）。原因：credit 放行的 client 立刻去等一两帧前的 fence，批次结束后才发的报告会输掉这场竞争。实测只有 Espryt·tcp 有变化，本地作答从 36–54 升到 79–101；Magma 在改动前就已经是 0 往返。

## Espryt 发现：llvmpipe 上 fence 只有被阻塞等待过才可见

Espryt 的 timed wait 几乎全部过线，这**不是**报告晚到：

| 探针 | 结果 |
|---|---|
| lag 1 / 2 / 3 / 4 / 6（`benchlag-present-status.log`） | inproc 本地作答都在 0–6，与 lag 无关——六帧前的 fence 也看不到 |
| server 端追踪（临时 `P10DBG`） | 队首 fence 在约 4 帧里的每次检查（含 flush 检查）都是 pending；client 的 timed wait 过线、server 执行 `ClientWaitSync(…, 1 s)` 之后，下一批才报告它。Magma 同一追踪里报告全部早于等待 |
| flush 检查改成冲刷最新的 fence | 不变（0–24） |
| flush 检查改用 1 ns 的 timed wait | 不变（本地 17 / 596） |

结论：在这台主机的 mesa llvmpipe（ES 3.2）上，`glGetSynciv(GL_SYNC_STATUS)` 与零超时 `glClientWaitSync` 看不到早已完成的 fence，只有真正阻塞的等待才能让它变成 signaled。这是宿主驱动的行为，与传输无关。A 在这里只是**没有收益**，并没有错：过线的等待语义不变，esc=64 与 esc=0 持平。探针都已撤掉，`ReportSignaledFences` 保持门禁通过时的原样。

**真机上的答案（C，[`C-MEASUREMENTS.md`](C-MEASUREMENTS.md) §1）**：在红米（Adreno 830）上同一个 bench，Espryt esc=64 时 0 次往返、598 / 598 次本地作答，与 Magma 相同。上面的现象只出现在 llvmpipe 上。
