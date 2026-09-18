# P5d 报告：inproc 性能专项（2026-09-18）

> 状态：**已落地两个提交**（`cb06538c` 一轮、`56a77348` 二轮，均在 `feat/disaggregated` 并已推送）。
> 本报告回答三个问题：要做什么、做到了什么、什么没完成。

## 定位与目标

**插入点**：P5c 之后、P6 之前。路线图的既有纪律是"性能对着 pull 臂只记录、不作阻塞门，专门的优化阶段排在路线图推完之后"——但 P5c 收官后在真实负载上的实测把这件事提前了：FCL + Minecraft 26.3-rc-3、视距 12、Redmi `2f7cbe2e`，inproc 世界内只有 **7-13 fps**，monolith 同场景 **~116 fps**，约 9 倍差距，inproc 不可用。

**目标**：在不改变 wire 语义、不动正确性门（双车道、G1/G2/G5/G14、双生成器门）的前提下，把 `inproc` 与 `monolith` 的逐帧帧率差拉到尽可能接近 1。性能数字的采集纪律与其他阶段一致：reboot-clean、同热窗口配对 A/B、只用 Redmi `2f7cbe2e`，设备侧风扇开自动档；A/B 切换靠 `/sdcard/FCL/mg_transport.txt` 运行时开关，免 APK 重编。

## 起点的瓶颈链（全部实测，非推测）

- persistent-map 推送：每个 verb 把每个映射的**全范围**推上线（实测 ~25 MB/帧，每 64 KB 块一条记录一次 barrier 往返）。
- 脏检查：每 64 KB 块做内容哈希，**每个 draw 对全部 arena 全范围扫**——XXH64/XXH3 一度占整个进程 73%（绑定过滤后仍 32.8%）。
- barrier：每条 verb 记录一次 `appliedSeq` 往返。
- `Doorbell::Wait` 的自旋每次迭代读一次时钟——该设备上 `clock_gettime` 是真系统调用（无 vDSO），占进程 ~20%。

## 做了什么

### 一轮 `cb06538c`（15 文件，+1100/-25）

- **persistent-map 推送改为三级臂，按 buffer 在 map 时挑选**：
  - **mprotect 脏页追踪**（主臂）：映射范围的页对齐内部注册 `PROT_READ`，应用写入触发 SIGSEGV，handler 在槽位位图上标脏页并放行，推送时只发含脏页的 64 KB 块、按连续段一次 mprotect 重新武装。归属判定三条，任一不满足即原样链给前一个 handler：只认内核 `SEGV_ACCERR`（用户态 raise 与未映射页不算）、必须在存活跟踪区间内、位图位鉴别重放（已置位页上的立即重 fault 只应答一次，再犯即非写访问，诚实崩溃）。对齐**向内收**——外向对齐的 edge 页含 malloc 元数据，信号被屏蔽的驱动/运行时 worker 线程 fault 即进程死亡（ctest 下 SmallRing 必崩、gdb 下必过的根因）。首推全量一次（与哈希臂 "fresh pushes everything once" 同规则）。
  - **哈希抑制臂**（`MOBILEGL_IPC_PERSISTENT_HASH_SUPPRESS`，默认 1）：未追踪 buffer 的逐块 XXH3 对比，只推变化块。
  - **绑定过滤推送**（`PushDrawConsumers`）：只推当前 draw/dispatch 实际消费的 buffer（VAO attribs/element、索引绑定点、indirect/param 槽、**TexBuffer 背存**——顺手修了高速飞行云的抽动，用户已确认）。
- **barrier 批处理**（`MOBILEGL_IPC_BATCH_WAITS`，默认 1；`MOBILEGL_PIPE_VERIFY` 强制 0）：无 reply slot 的 kCtxState/kCtxCso/kCtxObject 值类记录发布即返回，barrier 推迟到下一个拉取类 verb——R-1 的围栏柱不动，只减少往返。两个连带问题被钉死而非口头论证：kCtxObject 类记录依赖同步语义的测试加显式围栏（GenerateMipmap、FramebufferDeath 两条）；批处理下才可能出现的"apply 线程成为前端对象最后一个引用"新增**延期销毁队列**（入队 `(kind, lifetimeId)` POD，GL 线程在 verb 钩子重放七个 DestroyAndFree helper）——没有它 CopyImageLayered 两条必炸 `Fatal{RoleViolation, "MGPipeSlots"}`。
- **自旋时钟批处理**：`Doorbell::Wait` 的自旋改为每 64 次 yield 读一次时钟（clock_gettime 家族 ~20% → ~2%）。

### 二轮 `56a77348`（2 文件，+34/-11）

一轮上设备后发现 epoch skip 从未生效：负载里总有子页 persistent map，任何"未追踪"成员都否决跳过，绑定游走照样每 draw 跑（~8%），而直接扫所有未追踪成员的替代方案实测更差（41-43 vs 58-62 fps）。

- **零页注册**：子页范围不再落哈希臂，注册为"空内部 + hasEdges"的追踪槽（不保护任何页），全范围由两个有界的边块哈希服务——不再否决任何跳过。
- **shadow 页对齐分配**（`SHADOW_ALLOCATION_ALIGNMENT = 4096`；`GL_MIN_MAP_BUFFER_ALIGNMENT` 查询仍答 64）：shadow 占有的每一页字节全是自己的，追踪器永远不必碰邻居分配的字节；边块与零页形态在真实负载中基本消失。monolith 共用该分配器，A/B 无影响。

## 数字（VD12，同场景，in-world 稳定段）

| 阶段 | split (inproc) | monolith | pmap 线流量/帧 | 关键 profile 行 |
|---|---|---|---|---|
| 起点 | 7-13 fps | ~116 fps | ~25 MB | XXH64/3 73%→32.8%，clock_gettime ~20% |
| 一轮 `cb06538c` | 60-66 fps | 112-115 | ~0.23 MB | WaitForApplied 11.6%，绑定游走 ~8% |
| 二轮 `56a77348` | **63-65 fps** | 110-118 | ~0.23 MB | 绑定游走**消失**，XXH3 2.6%，clock ~2% |

帧率比从 ~1:9 收敛到 ~1:1.7。云不闪了（TexBuffer 覆盖），画面与 monolith 无可见差异。

## 验证

- 双车道：unit **2188/2188**、integration-split **111/111** 全绿（期间唯一反复出现的一条红是 `GenerateMipmapDepthPixels` 的环境性 flake：其 DirectGLES/DirectVulkan **monolith** 兄弟变体同样 flaky，而本包全部代码路径在 monolith 下可证明惰性——`PushIsArmed` false、无 ClientSession、deferral 首行 monolith 早退）。
- `gen_pipe.py --check` / `--self-test` 绿，生成文件与生成器一致。
- 每处关键修复都有 red-once 形态的证据：SmallRing SEGFAULT（外向对齐）、GenerateMipmapFieldsCross exit 101（kCtxObject 免等）、CopyImageLayered `Fatal{RoleViolation}`（apply 线程末引用）、余数块计数 3 vs 2（边块未合并）、117 MB/帧（边块无条件推）。

## 什么没完成

1. **后端 sync 锁复合（最大遗留，~18-20% 进程时间）**：apply 线程自身约 **48%** 的时间在 mutex lock/unlock 上——`SyncNeccessaryBuffers` 系路径在每个 draw 对每个 buffer 拿 per-resource `pendingMutex`（外加 `g_poolMutex` / `g_deferredBufferReleasesMutex`），~900 draws/帧放大成毫秒级。monolith profile 里**没有**这个复合（同一份后端代码在单线程下跑）——是锁调用频率 × 流水线序列化的可见性，还是真实竞争，尚未定位到具体站点。这是下一轮的第一目标。
2. **R-1 序列化（`WaitForApplied` ~11%）**：每个 draw 等 `appliedSeq` 的往返。根本解是 `gPipeInputs` 版本化/双缓冲——属于 **P11** 的既定设计范围，本阶段刻意不动。
3. **小项**：`__emutls_get_address` ~3%（emutls 路径上的 thread_local 读）；`SetHashSuppressor` 的 XXH3 ~2.6%（P5 遗留的 set 记录去重哈希，另一包的地界）。
4. **验证债**：
   - 79-trace 全集本机未跑（无此环境）；Redmi 复测窗口未排。
   - x86 32 位的 `MGHostSpan` 32B 断言：本包设计是 arm64-only，未做 32 位验证。
   - FCL 侧测试接线**未提交**（按约定）：`settings.gradle.kts` 的 `:MobileGL` 指向、`FCLauncher.java` 的 renderer env 注入（含 `mg_transport.txt` 开关，该文件在 git 里还处于未解决合并状态 UU，内容正常）、`MobileGL-disagg/build.gradle` 属性映射与 arm64 abiFilters、gradle wrapper 回退 8.13。插件 APK 构建路径里的 `MOBILEGL_TRANSPORT=inproc` 行在打插件包前必须拿掉。
5. **平台可移植性备注**：mprotect+SIGSEGV 臂只覆盖 POSIX（Android/Linux 验证）。Windows 桌面若需要，应加 `GetWriteWatch` 实现臂而非 `#ifdef` 信号路径；哈希臂作为全平台兜底永久保留。handler 已按 JVM 隐式空指针检查的要求链式转发非己 fault。

## 下一轮的入口

按收益排序：后端锁复合（pendingMutex 调用站点定位，先插一次性计数器确认频率，再决定是去锁、合并锁还是改 dirty 探测粒度）→ `SetHashSuppressor` 哈希 → emutls 热点。R-1 序列化不动，等 P11。
