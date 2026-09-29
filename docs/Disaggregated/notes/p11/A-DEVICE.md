# P11 A3 / A4：app 域外存探针与采纳基线（真机）

2026-09-29，Redmi `2f7cbe2e`（Adreno 830，GLES 3.2 V@0800.71，Vulkan 1.3.284 / 512.800.71）。代码 `ffeed0da` + 三个只动工具的提交（`p11/d`：`ea410991`、`1082ff5e`、`f4ead90a`）。CPU 定频并回读核验，GPU 未定频。只记录、不设门。原始数据 `~/w7/notes/p11/evidence/d/`，报告 `~/w7/notes/p11/d-report.md`。

## A3：T0 在 app 域能不能用（开放问题 3）

同一个探针 `.so`，shell 域（`u:r:shell:s0`）与经 trace app 的 exec 钩子在 `untrusted_app`（`u:r:untrusted_app:s0:c83,c257,c512,c768`）里各跑 5 组配置 × 2 次，交错进行。

| 路线 | shell / app |
|---|---|
| T0 AHB：交接、CPU 锁、Vulkan 导入、GPU 访问、GLES 导入、写回 client（64 KiB、128 MiB 两端区域） | OK / OK |
| **T0S 持续锁定**（client 从不解锁；8 轮 GPU 读 CPU 写、CPU 经持有的指针读 GPU 填充），Vulkan 与 GLES、64 KiB 与 128 MiB | OK / OK |
| T1 opaque-fd | OK / OK |
| T1-GLES memobj | FAIL / FAIL（与 spike B 同） |
| T3 | UNSUPPORTED / UNSUPPORTED |

**结论：两域逐行无差别；Vulkan 导入与 GLES `glBufferStorageExternalEXT` 导入都是 GO。** 局限：一台设备；持续锁定是顺序的乒乓，不是真正并发的 CPU / GPU 访问。

## A4：采纳基线（中位数，5 次交错）

只有 MC 26.3（`improved-transparency-minecraft-26.3`）会触发 ≥ 16 MiB 采纳；rd12 不会，Create 的 fixture 只有 2 帧。

| MC 26.3 | 单进程（采纳） | 单进程（关采纳） | spawn+shm（T2） | inproc（T2） |
|---|---|---|---|---|
| Espryt 稳态 p99 ms | 32.6 | 33.3 | 44.8 | 46.2 |
| Espryt 峰值 RSS MB（client + server） | 1098 | 1578 | 2205 | 2108 |
| Magma 稳态 p99 ms | 30.4 | 30.6 | 77.0 | 78.6 |
| Magma 峰值 RSS MB | 1150 | 1506 | 1751 | 1661 |

- **采纳开 / 关（Adreno）**：帧时没有差别（稳态 p99、世界内 p99、加载尖峰总和都在散布内），Mali 上的"163 → 21 ms"在这里不成立；RSS 省 480 MB（Espryt）/ 356 MB（Magma），与"~400 MB"相符。路线图的三个数由这张表取代。
- **split T2 相对单进程采纳**：RSS Espryt 约 2.0× / 1.9×、Magma 约 1.5× / 1.4×（rd12 上 +200–280 MB）；p99 Espryt +37–42%、Magma 约 2.5×（世界内帧约 2.7× 慢）。
- **线程落位扭曲 split 的时间**：server apply 线程钉在 6–7 核，client GL 线程随之大多落到慢核（大核占比 0.19–0.23，单进程 0.87–0.90），每帧 CPU 约 2.3×；唯一落到 7 号核的一次 8.3 ms，与 P10 一致。RSS 不受影响。
- spawn 臂的共享段（约 40 MB）在两个进程的 RSS 里各算一次，去重后的和约少 40 MB。

## 没跑成

- Magma 单进程 rd12（两个单进程臂）在加载帧 abort：`vm.max_map_count`（65530）耗尽，其中 57–60k 是 `/dev/kgsl-3d0` 映射；不带任何 `MOBILEGL_*` 也复现，与采纳无关。split 的 Magma rd12 与单进程 Magma 的 MC 26.3 正常。已转 dev 独立任务（`../DEBTS.md`）。
- tcp 臂按计划不跑（P10 已有 loopback 数）。
