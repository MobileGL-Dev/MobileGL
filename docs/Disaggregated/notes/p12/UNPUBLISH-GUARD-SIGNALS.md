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
- **真正的缺口只有一种**：描述符**没变**的 respecify 会在发射端被去重吞掉（`TextureEmit.h:838` 的
  `unchanged` 不读闩），连「拒绝」这个唯一的客户端可见信号都产生不了。描述符变了的那一半今天**已经能自愈**。
- **今天不可达**：`MGPipeApplierReleaseObjectRecords` 在生产代码里没有调用者（只有 MG_Test，实测 grep 见 §5）。

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

## 4. 真正的缺口：去重在发射端，比闩更早

`TextureEmit.h:838`：

~~~cpp
const Bool unchanged = entry.HasLastDesc && std::memcmp(&entry.LastDesc, &desc, sizeof(desc)) == 0;
~~~

**它不读闩。** 于是 `MGPipeApplierReleaseObjectRecords` 之后两条路不一样：

| 情形 | 今天的走向 | 结果 |
|---|---|---|
| 描述符**变了** | 不去重 → 闩仍为真所以不发 create → `RespecifyOnce` 发 respecify → applier 拒绝 → **补一个 create 再重试** → 接受 | **已自愈**（handoff §3.3 要的门，今天就是绿的） |
| 描述符**没变** | `if (unchanged) return;` | **一条记录都不过线**，服务端没有机会拒绝，自愈链断在这里 |

所以「①」要清的其实是第 2 行，而它要的**不是清闩，是清镜像**（`HasLastDesc`/`HasParamsLatch`）——
只清闩的话那条「相同 desc」照样不发（handoff §3.1 已经点到，这里是它的符号）。

## 5. 可达性（本轮复核）

~~~
$ grep -rn "MGPipeApplierReleaseObjectRecords" MobileGL/ --include=*.cpp --include=*.h | grep -v MG_Test
PipeApply.cpp:1593   void MGPipeApplierReleaseObjectRecords() {     <- 定义
（其余全部是注释）
~~~

与 handoff §3.4 一致：**生产调用者为零**，唯一调用者是 MG_Test（`PipeWireCodecTest`/`StagedTextureStoreTest`
各一处、若干 `*EmitTest` 的 SetUp/TearDown）。闩按 `{kind, slot, gen}` 键、对象销毁会清闩，槽位复用安全。

## 6. 建议（本轮不做，理由在下面）

1. **不加反向回调**：那是协议面（两端重出 APK），而触发者今天不存在。handoff §7 的判词是「今天是潜伏，
   别为它付协议面的钱」。
2. **不挂代次**：理由见 §2/§3——它会在每次 make-current 上 over-fire，为一个不可达路径付真实成本。
3. **该做的是把缺口写成门**：用 `MGPipeApplierReleaseObjectRecords` 造「记录被丢掉」，
   断言**描述符变了那一半必须自愈**（今天绿，红的方式是把 `RespecifyOnce` 的补 create 去掉），
   并把**描述符没变那一半**作为已知缺口记账——它是这件事真正欠的债。
4. 如果哪天真出现生产调用者，正确形状是给 entry 记一个「本代次内确认过」的戳（caps 代次或会话代次），
   代次一动就重发一次：它是 over-fire 方向，不用动 wire。

## 7. 复算

~~~sh
grep -rn "MGPipeApplierReleaseObjectRecords" MobileGL/ --include=*.cpp --include=*.h | grep -v MG_Test
sed -n '1249,1258p' MobileGL/MG_Impl/Pipe/TextureEmit.h     # void Reset() {}
sed -n '834,840p'   MobileGL/MG_Impl/Pipe/TextureEmit.h     # unchanged，不读闩
sed -n '1575,1591p' MobileGL/MG_Pipe/PipeApply.cpp          # MGPipeApplierReset 清什么、不清什么
~~~
