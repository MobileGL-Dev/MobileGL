# P11：persistent map 采纳档位诚实化（A 包主机半）

2026-09-29。依据 [`PLAN-P11.md`](../../docs/Disaggregated/notes/p11/PLAN-P11.md) §2 的 A1、A2 与 C 的文档修正。档位名按 `design/07`：T0 = client 分配 `AHardwareBuffer`、server 导入；T1 = server 导出 opaque fd；T2 = 拒绝，client 保留 shadow 并推送。本文只记实现所需的行为与门；A3 / A4（设备）与 B（T0）不在本文。

## 范围

档位在握手时定下（§1）；断言 arena 落在哪一档、三臂登记（§2）；文档漂移（§3）。

## 1. 档位由知道数据面的一侧在握手时定下（A1）

### 线上格式

- 无变化。LinkTerms、`RefuseCode`、控制协议修订号（3）都不动：每一侧只判自己的旋钮，不需要把档位带过线。

### 规则（`Transport/AdoptTier.cpp` `SettleAdoptTierAtHandshake`）

| 数据面 | 侧 | `MOBILEGL_IPC_ADOPT_TIER=0/1` |
|---|---|---|
| Stream（tcp） | client / server | 一行 W `Refuse{AdoptTierOnStream, "T<n>"}`，本会话 T2；从不 Fatal |
| SharedSegments（inproc、fork / unix spawn） | client | `Fatal{UnimplementedAdoptTier, "T<n>"}`，握手期、任何记录之前 |
| SharedSegments | server | 一行 W（档位由 client 定），T2 |

- 调用点：client `StartOverTransportPair`（Welcome 校验后）与 `StartOverSocket`（link terms 定下 `stream` 后、开数据连接前）；server `ServerSession::Accept`（后端检查后、Welcome 前）。
- 使用处 `Client::AdoptTierIsEmulate`（`PipeApply.cpp`、`PipeWireCodec.cpp` 仍问它）：旋钮 2 → true；本进程已有握手定档 → true，不再打印；没有握手定过档的调用者 → 仍 `Fatal{UnimplementedAdoptTier}`（"no handshake settled"）。旋钮 > 2（解析器不收）→ Fatal。
- `ServerSpawn` 从它拉起的子进程环境剔除 `MOBILEGL_IPC_*`（五个 server 自有旋钮除外），所以 fork / unix spawn 的 server 看不到 client 的旋钮；tcp 的 server 读自己的环境。

### 普查

新的死亡走 `SessionFail`：abort 站点仍 79，family 词仍 45（复用 `UnimplementedAdoptTier`）；refusal 词 3 → 4，`AdoptTierOnStream` 进 `fatal_census.py` 的 `LOCAL_REFUSAL_WORDS`（本地拒绝，不成为 Refuse 帧）。

### 门

- 单元：`AdoptTierStream.*`（真 tcp 会话：client 0/1 → 会话起、一行拒绝、`map_persistent` 得 DECLINED、之后仍 apply；进程内显示 server 0 → 每会话一行、不死）；`RemoteClientControls.AdoptTier{Zero,One}OverSharedSegmentsDiesAtTheHandshakeBeforeAnyMapPersistent`（inproc 真握手，子进程在 `Start` 返回前死、具名）；`SplitBufferSet.OnlyAdoptTierTwoIsImplemented`（函数级矩阵）；`PipeWireCodecTest.AdoptTier{Zero,One}AfterAStreamHandshakeIsADeclineOnTheWirePath` 与 `AnUnsettledAdoptTierStillDiesByNameOnTheWirePath`。
- 集成：`{DirectGLES,DirectVulkan}.Tcp.AdoptTier0.LargeArenaAdoptionScenario.*`（`MOBILEGL_IPC_ADOPT_TIER=0`；`integration-tcp` / `integration-magma-tcp`），`AStreamSessionRefusesTheAdoptTierOnceByName` 读 client 日志恰一行，其余用例绿。tcp 专属：共享段上同一旋钮在握手期死（`spawn_lane_parity.py` 的 `MAGMA_TCP_ONLY`）。
- red-once：删 Stream 分支 → 两后端 tcp 条目 client 握手期 `Fatal{UnimplementedAdoptTier}`、无拒绝行，单元 `AdoptTierStream` 两例、codec 两例、`SplitBufferSet` 一例红；把检查挪回使用处 → inproc 会话起来、`map_persistent` 发出后才在 apply 线程死（子进程写出 `S`），spawn 臂则静默跑 T2（server 环境被剔除）。

## 2. arena 落在哪一档（A2）

- `LargeArenaAdoptionScenario.TheArenaLandsInTheTierItsLaneDeclares`：`PersistentMapPeek` 读 arena 的 `IsBackendPersistentMapped`，与车道的 `MGITEST_PERSISTENT_MAP_ARM` 比；问两次（NULL 定义后；一帧绘制 + SSBO 绑定 + dispatch 后）。未声明的车道跳过。
- 声明：monolith = adopted（`DirectGLES.ResourceSubsystemOn/Off.`、新 `DirectVulkan.ArenaArm.`，都钉 monolith）；split = emulated（Espryt 三臂经 `SplitLogPaths` 按条目声明；Magma `Arena.` 块与 Buffers 车道）。
- 登记：`mgl_itest_register_split_arms("LargeArenaAdoptionScenario.*")`（Espryt 进门、Magma 进信息层）；Magma 进门：`DirectVulkan.<arm>.Arena.`（band 口味，六例）与 Buffers 车道（加六例，`test.yml` 的 require-green 名单同步）。
- red-once：monolith 车道加 `MOBILEGL_DISABLE_LARGE_BUFFER_ADOPTION=1` → 三条红；split 车道改声明 adopted → 两后端 8 条红。删 `PipeApply.cpp` 的 R-6 拒绝**不会**让 Espryt inproc 采纳（实测仍 emulated）：split 下 client 的采纳门经线路走，codec 不调 applier 就答 DECLINED；applier 只在 server 角色自己发射时被问到（只有 Magma 的 `EnsureGpuResidentStorage`），而 Magma 的线路表 `MapPersistent` 恒返回空。

## 3. 文档漂移（C）

- `Config.h`：大 buffer 采纳下的 SubData 自 `0ee3384b` 起经 resident-subdata GPU 有序落地，不是"plain memcpy"。
- `MG_Backend/Init.cpp`：DirectVulkan 在非 monolith 下注册线路资源表（`VkBufferManager::RegisterWireResourceOps`）并声明 bit 7。
- 档位命名按 `design/07`：`PersistentMapTracker.h` 注释已改。`protocol.fbs` 的 `Adopt = 6 // server-owned adopted store` 注释**没改**：`protocol_revision_pin.py` 对整文件取 sha256，改注释要 bump 修订号。
- `CONTRACT-P6.md`、`design/06`、`design/07` 的"stream 上具名拒绝"现为真，已指向本节代码。

## 不变量

- G1：新代码全在 `MOBILEGL_BUILD_DISAGGREGATED` 下（`MG_Remote`）。
- 线上格式与控制协议修订号不变（3）。
