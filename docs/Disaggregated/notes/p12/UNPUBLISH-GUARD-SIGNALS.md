# 「①」该挂哪个信号：三个候选的判定（handoff-2 §3 调查记录）

> 2026-09-26。分支 `feat/disaggregated`。对应 [`MOBILEGL-CS-HANDOFF-2.md`](../../../../MOBILEGL-CS-HANDOFF-2.md)
> 的 §3「把『①』挂到已有信号上」。**结论是 handoff §3.2 的前两个候选都不是信号**，成本仍在协议面；
> 本文把三条理由落到符号，并把真正的缺口缩到一句话。

## 0. 一句话

- **§3.2 候选 1（caps 代次）不是信号**：caps Adopt 是「快照重发」，而 `MGPipeApplierReset`（make-current 的
  服务端半边）**故意保留**对象记录——代次动了 ≠ 记录没了。
- **§3.2 候选 2（FreshlyPrimed）是同一个理由，而且它就是空的**：`TextureEmit.h:1257` 的
  `void Reset() {}` 函数体为空，注释写明「Only LATCHES reset here - the applier's object records survive a
  make-current」。
- **缺口的真实大小比 handoff §3.1 说的更大**：不是「清闩不够」，而是**已经没有东西会去清闩**。
  P12 的首用化把两个曾经读答案的行都变成了「已确认对象不回读」（respecify 走
  `MGPipeResourceRespecifyWantsItsReply` 的 fire-and-forget、params 走 `Wire_SetTextureParams` 的临时
  `kStatusOk`），而「已确认」= `MGPipeHandleIsPublished`——**它回答的是「create 发出去过」，在记录被丢掉之后
  仍然为真**。于是 applier 的拒绝再也到不了 emitter，`RespecifyOnce` 那条自愈链**今天对任何描述符都不成立**。
- **今天不可达**：`MGPipeApplierReleaseObjectRecords` 在生产代码里没有调用者（只有 MG_Test，实测 grep 见 §5）。
  所以这是债而不是现网缺陷——但它比 handoff 估计的宽：**修它需要协议面**，不是清闩、也不是挂代次。

## 1. 失效时要清的三样，逐条落到符号

| handoff §3.1 | 符号 | 今天的状态 |
|---|---|---|
| 发布闩 | `MGPipeNoteHandleUnpublished(kind, handle)`（`PipeFill.cpp:1890`），按 `{kind, slot, gen}` 键 | 对象销毁会清；跨 make-current 保留（设计如此） |
| 各 emitter 的镜像 | `TextureEmit` 的 `entry.HasLastDesc` / `entry.HasParamsLatch` | **只清闩不够**：去重比闩更早，见 §4 |
| create window 的 suspect 表 | `g_createSuspects`（`WireTables.cpp`） | 窗口默认已关（见 `CREATE-WINDOW-MEASURED.md`），这一条随之为空 |

## 2. 候选 1：`CapsMirror::Generation()` —— 动的是快照，不是记录

`CapsMirror::Generation()` 每次 `Adopt` 自增，而 Adopt 的触发点是**caps 快照重发**：`ServerMakeEGLCurrent`
对「它没持有的 tuple」转发绑定时会重发一次（ID-67，`MakeCurrentRepublishCount`）。也就是说代次动
**只说明服务端重新回答了一次能力问题**，不说明它的记录表空了。反证在 `MGPipeApplierReset` 自己身上：
它是 make-current 的服务端半边，`PipeApply.cpp:1584-1590` 清的是 working state 与三个 binding-point 窗口，
**texture / sampler / view / shader-CSO 记录一个都不清**——注释把理由写死了：「a GL object lives in a
share group, and re-publishing one would move its Serial for nothing」。

照代次清 = 把「记录还活着」的每一次 make-current 也当成失效，每次切上下文重发全部存活纹理。方向安全
（`PipeApply.h:822`：「Over-firing is the safe」），但这是**为一个今天不可达的路径付每次 make-current 的钱**。

## 3. 候选 2：`FreshlyPrimed` —— 同一个理由，且这个函数是空的

`PipeFill.cpp:3761` 的 FreshlyPrimed 臂确实在 reset 五个 emitter，但纹理那一个的 reset 是：

~~~cpp
// A fresh context: what the server has is no longer what this emitter last sent. Only
// LATCHES reset here - the applier's object records survive a make-current and
// re-publishing them would move their serials for nothing.
void Reset() {}          // TextureEmit.h:1257
~~~

空实现就是「这里没有可清的东西」的落地写法。同一个文件 `TextureEmit.h:1267-1271` 还把话说到另一头：
「a texture handle and the applier record it names are SHARE-GROUP OBJECT STATE, so nothing here is
per-context and **no re-publication path exists or may exist**」。所以候选 2 不是「一行」，是**推翻一条设计**。

## 4. 缺口：从「去重吞掉」到「没有任何行会读到拒绝」

### 4.1 第一步：去重在发射端，比闩更早

`TextureEmit.h:838`：

~~~cpp
const Bool unchanged = entry.HasLastDesc && std::memcmp(&entry.LastDesc, &desc, sizeof(desc)) == 0;
~~~

**它不读闩。** 所以描述符**没变**的 respecify 会在发射端直接返回（`if (unchanged) return;`），
一条记录都不过线，服务端也就没有机会拒绝——只清闩治不了这一半，要清的是镜像
（`entry.HasLastDesc` / `entry.HasParamsLatch`）。这一条是 handoff §3.1 已经点到的，这里是它的符号。

### 4.2 第二步（handoff 没写，本轮实测）：描述符变了也不自愈了

handoff §3.3 假定「变了描述符」那一半今天能自愈：respecify 被拒 → `RespecifyOnce` 补 create 再重试。
**P12 的首用化把这半也拿掉了**，因为那条路径要读答案，而两个行现在都自己回答自己：

| 行 | P12 之后 | 拒绝还能不能到 emitter |
|---|---|---|
| `resource_respecify` | 对象已确认时 `MGPipeResourceRespecifyWantsItsReply` 返回 false（`PipeRoute.h:297-302`），`Wire_Escape_ResourceRespecify` 发 provisional accept | **不能**（调用方读到 true，`RespecifyOnce` 不进 heal 分支） |
| `set_texture_params` | `Wire_SetTextureParams` 对 confirmed 对象直接把 `status` 写成 `kStatusOk`（`WireTables.cpp`），不读答案 | **不能** |

而「已确认」是 `MGPipeHandleIsPublished`，它的语义是**「这个 handle 的 create 发出去过」**，
在 `MGPipeApplierReleaseObjectRecords` 之后**依然为真**（`TextureEmit.h:1381-1389` 的注释自己写着这件事）。
于是唯一还会读真答案的行只剩 create，而 create 只在闩为假时才发——**闩为假这件事，正好是被丢掉记录之后不再发生的事**。

实测（`RemoteClientControls.DroppedApplierRecordsAreNotRepairedTodayAndThisCaseIsTheDebt`）：
丢掉之后 reshape 同一个对象，`creates=0 applier_records=0 published=1`——没有 create、applier 手里没有记录、
而客户端仍然认为它是已发布的。

### 4.3 所以「①」到底是什么

它不是一个「清三样」的卫生动作，而是**一条让 applier 能说出「我手里没有这个 handle」的通道**——
也就是 handoff §3.2 的第 3 条（反向通道第 10 个回调），成本是协议面。前两条便宜候选见 §2/§3。

## 5. 可达性（本轮复核）

~~~
$ grep -rn "MGPipeApplierReleaseObjectRecords" MobileGL/ --include=*.cpp --include=*.h | grep -v MG_Test
PipeApply.cpp:1593   void MGPipeApplierReleaseObjectRecords() {     <- 定义
（其余全部是注释）
~~~

与 handoff §3.4 一致：**生产调用者为零**，唯一调用者是 MG_Test（`PipeWireCodecTest`/`StagedTextureStoreTest`
各一处、若干 `*EmitTest` 的 SetUp/TearDown）。闩按 `{kind, slot, gen}` 键、对象销毁会清闩，槽位复用安全。

## 6. 建议（本轮做了第 3 条，其余不做，理由在下面）

1. **不加反向回调**：那是协议面（两端重出 APK），而触发者今天不存在。handoff §7 的判词是「今天是潜伏，
   别为它付协议面的钱」。**注意这一条的价钱现在知道了**：§4.2 说明它不是一个可选的美化，
   而是唯一的修法——所以「不值得付」的前提是「调用者确实不会出现」，一旦有人给
   `MGPipeApplierReleaseObjectRecords` 加生产调用者，就必须付。
2. **不挂代次**：理由见 §2/§3——它会在每次 make-current 上 over-fire，为一个不可达路径付真实成本；
   而且按 §4.2 它也**治不好**（闩清了、行还是不读答案）。
3. **已做**：`RemoteClientControls.DroppedApplierRecordsAreNotRepairedTodayAndThisCaseIsTheDebt`
   用 `MGPipeApplierReleaseObjectRecords` 造「记录被丢掉」，读的是**applier 手里有没有记录**和**闩的状态**
   （不是计数——handoff §3.3 那句「只看计数看不出这类错」），并把今天的三个数（`creates=0`、
   `applier_records=0`、`published=1`）**当成债务 pin 住**：谁把信号接上，这条用例就红，
   红得正好是「修复开始生效」的地方。
4. 真要做的话，形状是**给 entry 记一个「本代次内确认过」的戳**（caps 代次或会话代次），代次一动就重发一次
   ——它治 §4.1（去重），**治不了 §4.2**，因为 §4.2 缺的是「applier 说它没有」这件事本身。

## 7. 复算

~~~sh
grep -rn "MGPipeApplierReleaseObjectRecords" MobileGL/ --include=*.cpp --include=*.h | grep -v MG_Test
sed -n '1249,1258p' MobileGL/MG_Impl/Pipe/TextureEmit.h     # void Reset() {}
sed -n '834,840p'   MobileGL/MG_Impl/Pipe/TextureEmit.h     # unchanged，不读闩
sed -n '1575,1591p' MobileGL/MG_Pipe/PipeApply.cpp          # MGPipeApplierReset 清什么、不清什么
~~~
