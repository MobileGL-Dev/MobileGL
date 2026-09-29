# P11 计划：persistent map 与 ≥16 MiB 采纳（重划范围）

> 2026-09-29，基于 `feat/disaggregated@74ae652e`（P10 收官头）。路线图行（原文在 [`README.md`](README.md)）写于 09-22，下表逐项与树的现状核对。档位名按 `design/07-memory-readback-verification.md`：T0 = client 分配 AHB、server 导入；T1 = server 导出 opaque fd；T2 = 拒绝，由 client 推送。

## 1. 路线图每一项的现状

| 路线图写的 | 现状（核对） | 处置 |
|---|---|---|
| `MOBILEGL_IPC_ADOPT_TIER` 0/1 在使用处 Fatal | 成立，但只在 server 角色判定（`AdoptTierIsEmulate`，调用者 `PipeApply.cpp`、`PipeWireCodec.cpp`）；client 只打一行日志。spawn 下 server 在首个 `map_persistent` 才死；tcp 的 server 读不到 client 的值，静默走 T2 | **A1** |
| `dataPlane = Stream` 上点名 T0 / T1 → 具名拒绝并强制 T2 | **没实现**，但 `CONTRACT-P6.md`、`design/06`、`design/07` 都说它存在 | **A1** |
| `kSegAdopt` / `SEG_ADOPT` | 只有枚举和预留槽，生产代码里没有 `Install(kSegAdopt, …)`；`protocol.fbs` 的注释写的是 T1 的形状 | T0 不用这个段；枚举保留，注释统一（**C**） |
| POST 探针选档（T0 主、Adreno 可选 T1、T2 回退） | 没有会话期探针；DriverPost 在 APK 进程里、会话之外。spike B（shell 域）：T0 在 Adreno / Mali 全链 OK；T1 只有 Adreno Vulkan 过 | T1 关闭（**C**）；T0 的选档并入 **B** |
| `SEG_ADOPT` 生命周期绑 `completedFrameSerial` | 链路的 `completedFrameSerial` 没有生产者（`AdvanceCompletedFrame` 只有测试调用）；两个后端各有自己的完成序号 | 并入 **B**（T0 靠 AHB 引用计数 + server 本地 fence 门控释放） |
| `gPipeInputs` 版本化 / 双缓冲 | 已由 P5e（ID-135 更正）与 P5f f1 的双块机制落地 | 关闭（**C**） |
| 前置开放问题 3（从 `untrusted_app` 重跑 extmem 探针） | 从没在 app 域跑过；exec 钩子（trace APK 的 `mobilegl_spike_spawn`）存在，但 extmem_probe 不在构建图里；持续 lock 下 GPU 并发访问从没测过 | **A3**（设备） |
| 门：`LargeArenaAdoptionScenario` 在所选档下绿 | 场景 6 例，split 下只经 inproc 环境条目跑到；两种档位下都绿，没有用例断言数据落在哪一档 | **A2** |
| 门：26.3 与两个 Create fixture SSIM ≥ 0.99 | T2 下已有 SPLIT / SPAWN 重放；create-indirect 在 Redmi 上 monolith 就坏（ID-P7-4） | T2 已满足；T0 臂随 **B** |
| 门：Adreno 830 上 p99 与峰值 RSS 不比采纳基线差 10% 以上 | 基线（163 → 21 ms / 40 → 115 fps / ~400 MB）没有构建号、设备、臂、定频记录，115 疑为 vsync 封顶；本分支从没测过峰值 RSS | 撤下，**A4** 同会话重建（只记录，不设门） |

## 2. 工作包

### A — 档位诚实化 + 复核 + 重建基线（先做）

- **A1**（主机，两后端）：握手拿到 LinkTerms 后由 client 算出本会话的实际档位。Stream 上设 0 / 1 → 一行具名 `Refuse` 并强制 T2；SharedSegments 上设 0 / 1 → 握手期具名 Fatal（B 之前）。档位作为协商值追加进 LinkTerms（flatbuffers 只追加），tcp 的 server 不再靠自己的环境。门：单元（Stream + 0 → 会话起、一行 Refuse、`map_persistent` 得 DECLINED；SharedSegments + 0 → 第一条 `map_persistent` 之前死）；集成（`LargeArenaAdoptionScenario` 的 Tcp 条目带 `ADOPT_TIER=0`，两后端）。red-once：删 Stream 分支；把检查挪回使用处。
- **A2**（主机，两后端）：新用例断言 arena 落在车道声明的档（`PersistentMapPeek` + `MGITEST_PERSISTENT_MAP_ARM`：monolith 声明 adopted，split 声明 emulated）；`LargeArenaAdoptionScenario.*` 经 `mgl_itest_register_split_arms` 登记三臂，并加进 Magma Buffers 车道。red-once：monolith 车道加 `MOBILEGL_DISABLE_LARGE_BUFFER_ADOPTION=1`；删 server 端的拒绝（Espryt inproc 真采纳）。
- **A3**（设备）：extmem_probe 打包成 `lib*.so`，经 exec 钩子在 app 进程里跑，逐行对照 shell 域；补两条腿：持续 lock 下 GPU 读 / CPU 写交替、128 MiB。任何一行与 shell 域不同 → 对应后端的 B 判 no-go。
- **A4**（设备，只记录）：同一 APK、同一会话、Redmi 定频、各臂交错：monolith 采纳 / monolith + `MOBILEGL_DISABLE_LARGE_BUFFER_ADOPTION=1` / spawn+shm T2 / inproc T2 × 两后端 × {26.3、create-instancing、rd12}；逐线程 CPU p50、wall p99、峰值 RSS（split = client + server VmHWM − 共享，monolith = VmHWM；外部 adb 采样即可）。产出 B 的 go / no-go 表。

### B — 同机外部 client（Termux 一类）走共享内存，再上 T0（两后端）

**消费者（用户 2026-09-29）**：从 Termux 之类的其他 app 里启动的 GL 程序，连同一台设备上的 render server。这类 client 是另一个 app（另一个 uid、另一组 SELinux 类别），今天只能走 TCP loopback——Stream 数据面，T0 在上面不可能；render server 的 `@abstract` / 文件路径 unix 端点按其注释"只有本 app 能连"（`ServerControlActivity.java`）。所以 B 先要一条跨 app、能传 fd 的通道，数据面才能换成共享内存：

- **B0（设备探针，先做）**：跨 app 的 client 用 **retrace client + server app** 实现（用户 2026-09-29；不走 Termux 的路子）——trace APK 里的 retrace client 是一个 app，render server 跑在另一个包名的 APK 里（不同 uid、不同 SELinux 类别）。测 ① retrace client 能否连上 server app 的 abstract unix 套接字并用 `SCM_RIGHTS` 传 fd（MLS 类别下预计被拒，要实测）；② 不通时由两个 app 自己的 Java 侧引导：server app 暴露一个 bound Service，返回 socketpair 一端的 `ParcelFileDescriptor`，client app 绑定后交给原生 client；③ 经这条通道传 AHardwareBuffer 并导入（接 A3）。
- **B1（跨 app 共享内存数据面）**：控制面走 B0 选出的通道、数据面 `SharedSegments`（memfd 经 fd 传）——P10 设备实测同一 rd12 在 tcp loopback 51 fps、spawn+shm 120 fps（`notes/p10/C-MEASUREMENTS.md` §2），这一步本身就是外部 client 最大的收益，与 T0 无关。
- **B2（T0）**：在 B1 的通道上做 AHB 导入，形状如下。

B2 的形状：控制 op `AdoptStore` 经 unix 套接字传 AHB（Stream 上 A1 拒绝）；Espryt `glBufferStorageExternalEXT`、Magma `VK_ANDROID_external_memory_android_hardware_buffer` 导入；server 起角色时自测、发布 `kCapAdoptT0`；client 取 min(旋钮, caps, 数据面规则)；释放由 server 本地 fence 门控；CPU 读前 server 排空常驻写并等 GPU。主机只能验协议与回退（Linux 没有 AHB），GPU 正确性靠设备门。控制协议修订 3 → 4。预期：T0 省的是 RSS 与 Create 类持久映射推送，不减少 26.3 的 stage 字节。

### C — 只改文档

关闭 `gPipeInputs` 版本化与 T1；`completedFrameSerial` 生命周期与探针选档并入 B；撤下采纳基线三个数（改指 A4）；修文档漂移：`Config.h` 的 "plain memcpy"（`0ee3384b2` 起是 GPU 有序落地）、`MG_Backend/Init.cpp` 的 "DirectVulkan 不注册资源表"、三处档位定义（`protocol.fbs`、`PersistentMapTracker.h` 注释）、`notes/p0/README.md` 里基线的 "Adreno" 归属。

## 3. 待裁定（集成者先按建议走，可推翻）

| 事项 | 建议 |
|---|---|
| B 是否排期 | **排**（用户 2026-09-29：Termux 一类的外部 GL 程序）。顺序 B0 → B1 → B2；B0 的结果决定通道形状，A3 决定 B2 在 app 域是否可行 |
| inproc 的 `local` 档 | 与外部 client 无关；随 A4 的数据再议 |
| 门的口径 | 性能与 RSS 只记录（`MEASUREMENTS.md` 规矩）；正确性门照常 |

## 4. 门与规矩

同 P10：`~/w7/notes/p9/gate.sh` 全套 + verify 三车道；G1 同机；LinkTerms / 控制协议只追加；每个新门 red-once；设备只用红米（锁 `~/w7/locks/2f7cbe2e.lock`）；不拉 LFS；提交一行、无署名尾注。
