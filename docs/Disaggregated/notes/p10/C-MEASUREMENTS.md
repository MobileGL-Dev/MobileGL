# P10 C：真机测量与往返普查

2026-09-29。真机：Redmi `2f7cbe2e`（Adreno 830），代码 `8d0a1b6c`；CPU 定频 1555200 / 1958400 并回读核验，GPU 未定频。只记录、不设门。原始数据在 `~/w7/notes/p10/evidence/c/`（报告 `~/w7/notes/p10/c-report.md`）。

## 1. fence bench 在 Adreno 830 上（Split / inproc，600 帧，3 轮交错）

`FencePollScenario.PacedFrameFenceWaitsBench`，每帧对 2 帧前的 fence 做 timed wait。

| 后端 | esc=0 ms/帧 | esc=64 ms/帧 | esc=64 fence 往返 / 本地作答 | 回复槽记录 0 → 64 |
|---|---|---|---|---|
| Espryt | 0.163 / 0.153 / 0.145 | 0.156 / 0.140 / 0.157 | **0 / 598** | 601 → 3 |
| Magma（tier 2） | 0.167 / 0.176 / 0.181 | 0.191 / 0.175 / 0.174 | 0 / 598 | 601 → 3 |
| Magma（`.Fence.` 带状环境，与主机 bench 同） | 0.171 / 0.167 / 0.164 | 0.182 / 0.160 / 0.182 | 0 / 598 | 601 → 3 |

- 延迟 4 帧：Espryt 0 / 596；延迟 1 帧：两后端都是 0 / 599。
- **结论：真 GPU 上 Espryt 与 Magma 一样，每次等待都在本地作答。** [`A-FENCE-POLL.md`](A-FENCE-POLL.md) 里"fence 只有阻塞等待后才可见"是主机 llvmpipe 独有的行为。inproc 的每帧耗时在噪声内不变（每次运行约 0.1 s，10% 以内的差都是噪声）；收益在有往返延迟的链路上（主机 Magma·tcp −37%）。
- Spawn 臂没跑：Android 测试框架总是画进 `ANativeWindow`，spawn 出来的 server 拒收（`Fatal{UnmigratedSurface}`）。
- **Android 上的 itest 本身跑不了这个 bench**：`SplitRuntimePeek` 在 `__ANDROID__` 下编译掉，itest 又链接的是隐藏符号的 `libMobileGL.so`。测量用的是本地构建补丁（`-DMGITEST_ANDROID_STATIC_PEEK=ON`：Android 上改链静态库并开启 peek，库代码与编译选项不变），未提交，存为 `evidence/c/peek-patch.diff`（ID-P10-14）。

## 2. `MOBILEGL_IPC_PRESENT_CREDIT` 1 / 2 / 3，设备上 spawn + tcp（127.0.0.1），trace APK，Espryt，3 次交错

| 臂 · 负载 | credit 1 | credit 2 | credit 3 | 散布 | credit 等待 ms/帧 | 传输等待 ms/帧 | client CPU ms/帧 |
|---|---|---|---|---|---|---|---|
| tcp · rd12（稳态 fps） | 51.4 | 51.6 | 52.4 | 1–3% | 0.18–0.24 | 11.2 | 9.9 |
| tcp · openra | 399.0 | 384.9 | 391.8 | 最多 8% | 0.16–0.19 | 2.1 | 1.8 |
| shm · rd12 | 120.2 | 120.1 | — | ≤ 0.4% | 0 | 0.1 | 8.2 |

- **结论：loopback 上 credit 没有噪声以外的影响。** rd12·tcp 的 19.4 ms 帧由 client CPU（~9.9 ms）和生产者在传输里的停车（~11.2 ms，发送 + 环 / stage / ack 等待）组成，credit 等待只有 ~0.2 ms。五臂报告（09-28）建议 #2"credit 2"在 loopback 上没有收益；要追的是 stream 传输的停车（性能债，[`../DEBTS.md`](../DEBTS.md)）。
- 跨机（Wi-Fi）credit 1 → 2 的 +26% 仍以 P12 的数字为准（[`../p12/CROSSHOST-ACCEPTANCE.md`](../p12/CROSSHOST-ACCEPTANCE.md)），那里瓶颈是往返延迟。
- 五臂报告里 tcp-loopback rd12 的 21–23 fps（74% 在 credit 等待）是旧构建、未定频的结果，与本次不可直接比较。
- Magma 没测：`profile_matrix.sh` / `run_matrix.sh` 在两处写死了 DirectGLES。
- P12 同款明细（`evidence/c/m2/m2.p12style.md`）：rd12 加载帧 10.6–10.8 s、稳态 73–74 MB/s；`wait_replies` 全在第 1 帧（`ResourceCreate`）。

## 3. 主机 retrace 往返普查（`MOBILEGL_PIPE_STATS=1`，全部 SPLIT / SPAWN 条目）

集成头 `47bbf5e6`，156 条（39 个 trace × 两后端 × SPLIT / SPAWN），`P65LinkMetrics` 的逐帧 `wait_replies` 按 op 分。全表在 `~/w7/notes/p10/evidence/census.md`，脚本 `~/w7/notes/p10/census.py`。

| 稳态（第 2 帧起）的回包等待 | 全语料 | 出处 |
|---|---|---|
| draw / state / upload 路径 | **0** | 没有一条 draw、状态设置或 `ResourceSubData` 记录等待回复 |
| `ResourceCreate` | 243,172 | 每个资源一次（modernui-inventory 每帧约 6.5 次）；P12 已裁定保持 `kWaitReply`（[`../p12/DEVICELOST-AND-UPLOAD-WAITS.md`](../p12/DEVICELOST-AND-UPLOAD-WAITS.md) §2.4） |
| `FenceWait` | 10,685 | **只有** `improved-transparency-minecraft-26.3` × Espryt（每帧约 4.4 次 timed wait），即 llvmpipe 的现象（ID-P10-6）；同一 trace 在 Magma 上是 0 |
| `MapPersistent` | 192 | 每个 trace 1–9 次 |

- 39 个 trace 里有 34 个建 fence（多数每帧一个），其中 33 个在任何一条臂上都**没有**发出 `FenceWait` / `FenceStatus` 记录，也就是轮询全部在本地作答；唯一的例外是上表中 llvmpipe 上那一条。
- iterationrp 的 6 条 retrace 因宿主 JIT 崩溃判红，但崩溃前的帧照常计数。
