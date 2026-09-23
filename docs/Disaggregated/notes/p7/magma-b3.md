# P7 wave 2-B3：Magma wire 臂的「已完成帧序号地板」不可证（分支 `p7/magma-b3`）

> 计划见 [`PLAN-PH-P34B-P7.md`](PLAN-PH-P34B-P7.md) §3 wave 2；规范见
> [`CONTRACT-P7.md`](../../../../MobileGL/MG_Remote/CONTRACT-P7.md) §0（规则 I / J）、§3.2、§7、§9；
> 裁定 ID-P7-14 / 19 / 20 / 21 / 23。基线 = `feat/disaggregated` 上 `~/w7/pipe` 的头 `ae3f43ab`。
> 本文所有 `file:line` 均在该提交上读取。
>
> 主机口径：WSL Arch + lavapipe（`/usr/share/vulkan/icd.d/lvp_icd.json`），`build-split` =
> Release / clang / ccache / `DISAGGREGATED=ON` `INPROC=ON` `PIPE_PUSH=ON` `BUILD_INTEGRATION_TEST=ON`，
> ICD 与 `~/w7/pipe/build-split` 逐项相同（`p7_worktree.sh` 钉）。
>
> 基线门读数（`~/w7/logs/p7-gate-d1.log`）：unit **2420**、`integration-magma-split` **88**、
> `-spawn` **67**、`-tcp` **69**、`integration-magma-full-split` **526**、`integration-split` **226**。

出口门 3 的锚点：OpenRA / DirectVulkan / pbuffer 在 Redmi（Adreno 830）上，wire 臂（inproc **与**
spawn）每跑一次是金图 `ace2af04` 与错图 `fb75d412`（14658 px）之间的 ~50% 抛硬币，
另有第三张 `4e5ba514`（9836 px），而**通过与失败两次运行的 client/server 日志逐行相同**。

结论一句话：**服务端把「帧 N-1 已完成」这件事建立在一次它没有证明过的 fence 上**，于是帧 N 的第一次
流式 `glBufferSubData` 走了无同步的 host memcpy，而**上一帧尚未执行完的 `vkCmdCopyBuffer` 随后盖了上去**。

---

## 0. 每片一条提交

| 片 | 名 | 内容 |
|---|---|---|
| 1 | 规则 I 的具名 + 计数 | wire draw 路径上每一处静默 `return false` 得名、得计数、得一行 decline 日志；`UploadPendingWireLevels` 的五个出口与两个「不失败但留着 pending」的 `continue` 分开计；pending 上传 gauge；`MOBILEGL_MAGMA_WIREBUF_PROBE` 探针 |
| 2 | 判据加 fence 项（纵深） | `WriteWireBuffer` 的 busy 判据从「帧计数」改为「帧计数 **或** 提交 fence」；`MGITEST_MAGMA_FORCE_STALE_BUFFER_SERIAL` |
| 3 | 车道 | `F1WireScenario.StreamedBufferSubDataBeforeEachDrawIsOrdered` × 三臂 × 两档 |
| 4 | **可证的地板（主修）** | `OnSubmitsCompletedUpTo` 只把地板推到**没有任何在飞提交仍持有**的序号；`unsound-serial-complete` 计数器 |
| 5 | 本文 | — |
| 6 | 复审（ID-P7-34）第 4 条 | `.def` 53 行**每行有站点、每站点有 W/E 行**；`scripts/ci/wire_declines_audit.py` 守住两半（§1.5） |
| 7 | 复审第 1 条 | `StaleSerial.` 只注册 split + spawn；`spawn_lane_parity.py` 的具名例外 `MAGMA_SERVER_ENV_KNOB_NO_TCP`（与 B2 的五条 server 端旋钮同一张表，§4.2） |
| 8 | 复审第 3 条 | 注释改正：**地板是保证、提交项是探针/纵深**；`WaitForWireBufferHostAccess` 早返回加提交项、`ReadWireBuffer` 补盖戳（§3.2） |
| 9 | 复审第 2 条 | `run_trace_case.cmake`：split retrace 见 `MGWIRE-FLOOR unsound-serial-complete` 即红——主修有了车道（§4.1） |
| 10 | 本文（复审轮） | — |
| 11–13 | 修正轮（fable 复审，land with fixes） | 审计：去注释/字符串后再匹配（§1.5）；裸 `Count` 的日志须在**本块**同缩进；`.def` 行可带尾注释；`--self-test` 负对照进 CI |
| 14 | 修正轮 | `run_trace_case.cmake`：spawn 无服务端日志即 FATAL（§4.1） |
| 15–16 | 修正轮 | 注释/日志措辞：image-flags 出口的注释；`Magma wire decline [Site]`（§1.5） |
| 17–19 | 本文（修正轮） | §5 标明包树/落地树；§7.2 行号；§7.2 `WaitForFrameSerial` 答 true 的债 |
| 20 | 本文（修正轮） | 修正轮的描述：审计的 strip / 块走查 / 自测、spawn 服务端日志 FATAL、修正轮的门 |
| 21–23 | 修正轮 2（fable 对修正轮的复审，land with fixes） | 审计：站点的日志必须是**以** `MGLOG_W/E` 开头的语句（`if (g) MGLOG_W(...)` 不算）；带前缀的字符字面量 `L'"'` / `u8'"'`；块走查在同缩进 `return`/`break;`/`case …:` 处停、跨过第 0 列 `#if`/`#endif`（§1.5） |
| 24 | 修正轮 2 | `run_trace_case.cmake`：服务端日志缺失即 FATAL 移到 marker / ARMED 检查**之后**，inproc 也要（§4.1） |
| 25 | 修正轮 2 | `retrace_pull_library_control.sh`：接受 P6 日志改名后 pull 库实际得到的那句 FATAL（§4.5） |
| 26 | 修正轮 2 | 注释：`WireDeclineTally.h` 引用了不存在的 `MGL_WIRE_DECLINE_FALSE`；`.def` 横幅说 wire 臂而非 draw |
| 27 | 本文（修正轮 2） | — |
| 28 | 修正轮 3（fable 对修正轮 2 的复审，land with fixes） | 审计：块走查只对整行的 `#if MOBILEGL_BUILD_DISAGGREGATED` 这一种守卫 fail-open（`#if 0` / `#ifdef X` 之下的日志不算站点的日志）；任何缩进的 `#else`/`#elif` 都是停点（§1.5） |
| 29 | 修正轮 3 | `run_trace_case.cmake`：并入的服务端日志 FATAL 也报 `MOBILEGL_IPC_CONTROL`，tcp 臂的红仍说 tcp（§4.1） |
| 30 | 修正轮 3 | `retrace_pull_library_control.sh`：`EVIDENCE` 里的空 `grep -F` 模式即 `::error::`；`stub_ctest.sh` 恢复可执行位（§4.5） |
| 31 | 本文（修正轮 3） | §7.2 B4 那句给出包树/落地树两套行号；§7.2 两条债（tcp pid 证明检查的次序、`LOG_WINDOW` 数被跳过的行）；§5 本轮的门 |

---

## 1. 片 1：先把「静默丢 draw」变成可读的数（规则 I）

### 1.1 做了什么

`Renderer/WireDeclines.def`（新）列出 wire draw 路径上每一个静默出口的**名字**，
`Renderer/WireDeclineTally.h`（新）展开成计数表 + `MGL_WIRE_DECLINE_AT` 宏（计数**永远**动，
`MGLOG_E_ONCE` 每站点一次），形状取 `PipeApplier.h:229-234` 的 `ServerVerbSink` tally，
理由也一样（R-16：探针不能对着桩武装）。落点：`SetupWireDraw` / `DispatchWireCompute` **25 处**、
`PrepareWireTextureResources` 3 处、`SyncTextureResourceByHandle` 4 处、
`UploadPendingWireLevels` 侦查提名的五个 `incomplete`/false 出口（`:2410` `:2436` `:2453` `:2499` `:2508`）
**加上两个只 `continue` 的**——后者**报告成功却把条目留在 pending 集合里**，单独一个名字。

同包加了 pending 上传 gauge（条目数 / 字节数 / 峰值），在 `UploadPendingWireLevels` 的漏斗处取样。
**一处如实更正给 M1**：`VkTextureManager.cpp:2485-2490` 的 `region.owned` 是 `items` 里的局部
`Vector<Uint8>`，随函数返回析构，**不**是泄漏点；活下来的是 `StagedTextureStore` 的 level 影子与
`record.PendingUploads` 的条目。

### 1.2 未强制的 tally：**全零**，lens A 的首选假说被**证伪**

OpenRA / DirectVulkan / `MOBILEGL_TRANSPORT=inproc` / lavapipe：retrace `PASS ssim=1.0`，
`transport=inproc`，`Fatal{` 0 行，`grep MGWIRE-DECLINES` **空**——当时**已接线**的 40 个站点一次都没走过，pending gauge 恒 0。
（本段初版写「32 个出口」，与树不符；复审轮的审计给出的实数是：52 行里 40 行有站点、12 行**无站点**。
那 12 行在此次回放中是否被走过，初版的读数**回答不了**——它们当时不会计数。复审轮补齐后的读数见 §1.5。）

这与裁判的独立判定一致：**lens A 的「静默丢 draw」出局**。裁判给的两条更硬的理由：
`SetupDraw` 的每一个 bail-out 都 `MGLOG_E_ONCE`（而通过/失败日志逐行相同）；且
`4e5ba514` **只**移除了调用 30376 的四边形 q00..q30，q31..q52 **逐字节完好**——
一个被丢掉的 draw 要么全丢要么不丢，不会丢一个前缀。

### 1.3 tally 不是桩（R-16，已执行并还原）

把 `WireDraw.inc` 的 `PointSizeCapability` 条件临时改成恒真重建，跑
`DirectVulkan.Split.Fm.F1WireScenario.StreamedBufferSubDataBeforeEachDrawIsOrdered`：

```
***Failed
[mgl-srv-apply/ERROR]: Magma wire draw declined [PointSizeCapability]: the program writes
    gl_PointSize and the device cannot express it
[mgl-srv-apply/INFO]: MGWIRE-DECLINES[shutdown] total=4 ...
[mgl-srv-apply/INFO]: MGWIRE-DECLINES[shutdown]   PointSizeCapability=4
```

四个 draw、四次计数、一行具名日志、用例按名变红。还原后绿。

### 1.4 这些名字**留在树上**

规则 I 说 wire 臂的拒绝只有 decline 与具名 Fatal 两种形状；裸 `return false` 是第三种。
本包把这条路径上的每一个都变成了 **decline**：具名、计数、每站点一行日志。
（初版这句话**言过其实**，复审轮改正，见 §1.5。）

### 1.5 复审轮：每一行都有站点，每个站点都有一行（ID-P7-34 第 4 条）

复审数出初版的 `.def` 52 行里 **12 行没有任何站点**——`TexViewFormat` / `TexNoApplierRecord` /
`TexRecordDeadOrStale` 与九个 `Shape*`——还有只 `Count` 不打日志的站点；`SyncWireTextureShape`
的 ViewOf 出口仍是裸 `return false`。复审轮：

- `SyncWireTextureShape` 的九个 `Shape*` 出口全部计数；ViewOf 与 preserve-flush 两个裸出口改为
  `MGL_WIRE_DECLINE_AT`；image-flags 出口原来只有 `MGLOG_D`（Release 编译掉），同样提升。
- preserve-copy 出口是这个函数里**唯一一个没有行**的出口：按 append-only 追加 `ShapePreserveCopyFailed`，共 **53** 行。
- `SyncTextureResourceByHandle` / `UploadPendingWireLevels` 里没有自己日志的 count-only 站点提升为
  `MGL_WIRE_DECLINE_AT`；已经站在一条 W/E `_ONCE` 日志下面的保留那条日志并补上计数。
- **分布**：`SetupWireDraw` 21 + `DispatchWireCompute` 4、`PrepareWireTextureResources` 3、
  `SyncTextureResourceByHandle` 7、`UploadPendingWireLevels` 8、`SyncWireTextureShape` 10 = **53**。
- `scripts/ci/wire_declines_audit.py`（进 CI，与 fatal census 同一个只读树的 job）：
  行无站点、站点不在 `.def`、裸 `Count()` 上方 8 行内没有 `MGLOG_W`/`MGLOG_E`——任一即 rc 1。
  定义宏的 `WireDeclineTally.h` 不算站点（初版审计的误报就在这里）。
  **修正轮（fable 复审）**：上面那条规则有三个洞——注释或字符串里的 `MGL_WIRE_DECLINE_AT(...)` 也算站点、
  8 行窗口不分分支（上一个 `if` 里的日志也算）、带尾注释的 `.def` 行被丢掉。现在审计先把 `//`、`/* */`、
  字符串与字符字面量抹成空格（保留换行，行号不变）再匹配；裸 `Count()` 的日志必须在**同一块**、**同缩进**、
  8 行之内（向上遇到缩进更浅的行即停）；`.def` 同样去注释后再认行。`--self-test` 的 8 个内联夹具
  （// 注释站点、/* */ 注释站点、字符串站点、别的分支的日志、嵌套分支的日志、注释里的日志、`MGLOG_D` → rc 1；
  好站点 → rc 0，且都要打出对应的那句话）在 CI 同一步里先跑。树上仍是 53 / 53 / 0。
  **修正轮 2（fable 对修正轮的复审）**：又三个洞。`LOGGED` 没锚定行首，`if (g) MGLOG_W("x");` /
  `g ? MGLOG_W("x") : (void)0;` 这种只在某些时候才打的单行守卫日志，站在 `Count` 的缩进上就算成了站点自己的日志
  ——计数每次动、日志只偶尔打，正是审计要拒的那种站点；带前缀的字符字面量 `L'"'` / `u8'"'` 里的 `'` 被当成数字分隔符，
  里面的 `"` 打开一个幻影字符串把本行剩下的部分（连同站点）抹掉（fail-closed 的误红）；块走查遇到第 0 列的
  `#if`/`#endif` 就停（误红），而同缩进的 `return`/`break;`/`case …:` **之上**的日志（控制流到不了 `Count`）却算数。
  现在：`LOGGED` 锚定 `^\s*MGLOG_[WE]`，走查用 `match` 不用 `search`；`CHAR_PREFIX` 与 `RAW_PREFIX` 同形（前缀之前
  须是非标识符字符或行首，所以 `0xAB'CD` 不受影响）；走查跨过 `#if`/`#ifdef`/`#ifndef`/`#endif`（`#else`/`#elif`
  仍是停点——另一侧的日志属于另一个构建），在同缩进的 `case`/`default:`/`break;`/`return`/`continue;`/`goto`/`throw`
  处停。夹具 8 → **14**，新夹具在旧审计（`53471e7b`）与新审计上的读数：

  | 夹具 | 期望 | 旧审计 | 新审计 |
  |---|---|---|---|
  | guarded one-liner log at the Count's indentation | rc 1 | rc 0 **误绿** | rc 1 |
  | log on the far side of a same-indent return | rc 1 | rc 0 **误绿** | rc 1 |
  | log in the previous case of a same-indent switch | rc 1 | rc 0 **误绿** | rc 1 |
  | log only on the other side of a #else | rc 1 | rc 1 | rc 1（钉住 `#else` 仍是停点） |
  | prefixed char literals (L'"', u8'"') before a site on the same line | rc 0 | rc 1 **误红** | rc 0 |
  | count under its log across a column-0 #if/#endif | rc 0 | rc 1 **误红** | rc 0 |

  14/14，树上仍是 **53 / 53 / 0**（树上今天没有带前缀的字符字面量，也没有跨 `#if` 的裸 `Count`，所以两处误红没有真的红过）。
  **修正轮 3（fable 对修正轮 2 的复审）**：又两个洞，都在块走查对预处理行的处理上。跨过 `#endif` 是**不看守卫**的：
  `#if 0` / `MGLOG_W(...)` / `#endif` / `Count(...)` 读成有日志（rc 0），`#ifdef MOBILEGL_VERBOSE_DECLINES` 包着的日志也一样
  ——计数每次动、日志在任何发布构建里都不打；`#else`/`#elif` 只在第 0 列是停点（作为「缩进更浅的行」），
  缩进到 `Count` 缩进上的 `#else` 直接穿过去。现在：走查**数**它跨过的 `#endif`，同缩进的日志若站在一个未闭合的
  `#endif` 之下，只有当**继续向上**找到的每一个包着它的开启行都是整行的 `#if MOBILEGL_BUILD_DISAGGREGATED`
  （`&& X` 也不算）才算数——那是本 tally 所在的构建，也是走查**唯一** fail-open 的守卫（注释写明）；
  没有未闭合 `#endif` 的 `#if`/`#ifdef`/`#ifndef` 是 `Count` 自己的守卫（上方的日志无条件），照旧跨过，
  所以「跨第 0 列 `#if/#endif`」那个夹具仍绿；`#else`/`#elif` 在**任何**缩进都在缩进判断之前就停。夹具 14 → **16**：

  | 夹具 | 期望 | 旧审计（`32edcc46`） | 新审计 |
  |---|---|---|---|
  | log only under a #if 0 above the Count | rc 1 | rc 0 **误绿** | rc 1 |
  | log only on the other side of an INDENTED #else at the Count's indentation | rc 1 | rc 0 **误绿** | rc 1 |

  16/16，树上仍是 **53 / 53 / 0**（树上没有裸 `Count` 站在别的守卫之下的日志后面）。没有做成夹具但跑过的形状
  （`~/w7/b3bin/r3/f3/probe_shapes.py`，8/8）：日志在 `#if MOBILEGL_BUILD_DISAGGREGATED` 下、`Count` 在外 → 绿（fail-open 的那一种）；
  日志与 `Count` 之间夹一个闭合的 `#if FOO … #endif` 组 → 绿；日志在守卫开启行的 `#else` 一侧、守卫嵌在 `#if VERBOSE` 里、
  `#if MOBILEGL_BUILD_DISAGGREGATED && !defined(X)` → 红。`LOG_WINDOW` 数被跳过的行的债见 §7.2。

| 审计 | 行 | 有站点 | 未记日志 | rc |
|---|---|---|---|---|
| 复审前（`0d425499`） | 52 | 40 | 10（W/E 口径） | 1 |
| 复审后 | 53 | **53** | **0** | **0** |

**tally 仍然不是桩**（R-16，已执行并还原）：把 `ShapeNoVkFormat` 的条件临时改成「1×1 纹理恒真」
重建，跑 `DirectVulkan.Split.Fm.F1WireScenario.VertexIdSamplerAndScalarUniformPixels`：像素断言失败，
服务端日志

```
[mgl-srv-apply/WARN]: Magma wire texture {slot=16, gen=0}: no backing VkFormat for internal format 0x14
[mgl-srv-apply/ERROR]: Magma wire draw declined [TexShapeSync]: texture {slot=16, gen=0}: no backing image; see the Shape* tally
[mgl-srv-apply/INFO]: MGWIRE-DECLINES[shutdown] total=6 ...
[mgl-srv-apply/INFO]: MGWIRE-DECLINES[shutdown]   TextureResourcesUnprepared=2
[mgl-srv-apply/INFO]: MGWIRE-DECLINES[shutdown]   TexShapeSync=2
[mgl-srv-apply/INFO]: MGWIRE-DECLINES[shutdown]   ShapeNoVkFormat=2
```

两次 draw、三层名字逐层对上。还原后绿。（先试过「对所有纹理恒真」：FBO 附件也跟着失去 image，
先撞上 `Fatal{UnmigratedVerb, "Magma:framebuffer-resource"}`——那是 framebuffer 路径自己的具名 Fatal，
不是本表的出口，所以把强制收窄到只命中被采样的 1×1 源纹理。）

**修正轮（fable 复审 nit）：`Tex*` / `Shape*` / `Upload*` 计数是「每次纹理同步」，不是「每个 draw」。**
`SyncTextureResourceByHandle` 除了 draw/dispatch 的纹理准备，还从描述符解析、framebuffer 附件、
`CopyTexSubImage`、mipmap 生成、`CopyImageSubData`（这几处同步失败后立刻具名 Fatal）以及纹理回读（退回 unbacked 影子）
进来：一个采样三张不可同步纹理的 draw 计三次；附件同步失败计一次，随后是 Fatal，不是被跳过的 draw。
所以 `MGL_WIRE_DECLINE_AT` 的日志行从 `Magma wire draw declined [Site]` 改为 **`Magma wire decline [Site]`**
（`WireDeclineTally.h` 宏上的注释写明同样的事）。§1.3 与上面引用的日志是改名前的原样。

---

## 2. 机制（裁判 H1，本包在主机上按计数器确认）

### 2.1 现象：这是一次 draw 的顶点区间的**前缀撕裂**

lens D + 裁判的逐字节取证：错图 100.0% 的失配像素落在**一个** draw
（调用 30376，`glDrawArrays(GL_TRIANGLES, first=0, count=318)`）的屏幕四边形并集内，并集外一个都没有。
该 draw 的顶点来自调用 30366 写进 GL buffer 1 的 **11448 字节**（`off=0`，`GL_DYNAMIC_DRAW`，从不 orphan）。

- 318 顶点 / 6 顶点每四边形 = **53 个四边形**，`11448 / 53` = **216 字节/四边形**；
- `4e5ba514` = 丢 q00..q30 = **31** 个四边形 = 前 `31 × 216` = **6696 字节**；
- `fb75d412` = 整个 11448 字节都丢。

**6696 与 11448 都是 buffer 1 自己的写尺寸**，本包的探针在主机上直接读到（末帧对 slot=1 的写序列）：

```
216 216 11448 216 8640 12096 216 11880 2376 1296 2592 2592 2592 2376 2592 1728
2592 1296 2592 3024 2592 864 6696 3456 216 216
```

全程 `size=6696` 出现 **26** 次、`size=11448` 出现 **5** 次——即「前一帧写过的某一笔又盖了上来」。

### 2.2 链条（`file:line`）

1. **一个帧序号上有两次提交**（wire 臂），monolith 只有一次。本包实测（OpenRA 主机回放，
   `MOBILEGL_MAGMA_WIREBUF_PROBE=1`）：

   | 臂 | `BeginFrame`（帧序号自增） | 最大提交序号 | 最大帧序号 | 退休时仍有在飞记录 |
   |---|---|---|---|---|
   | monolith | 33 | **30** | 32 | 27 |
   | inproc | 33 | **73** | 32 | 26 |
   | spawn | 33 | **73** | 32 | 26 |

   即 split 上 **~2 次提交/帧序号**、monolith **~1 次**——与 lens C 的 +2 / +1 一致。
   - **S1**：每帧的调色板 `glTexImage2D`（调用 30232）在第一次采样调色板的 draw 上经
     `UploadPendingWireLevels` → `FlushWirePendingCommandsForTextureUpdate`
     （`VkTextureManager.cpp:2508`；`VulkanRenderer.h:149-151`）→ `FlushPendingCommands`
     （`VulkanRenderer.cpp:13676`）提交，带 **pooled fence**。
   - **S2**：本帧的 draw，以及 `StagedWireRangeCopy`（`VkBufferManager.cpp:780-824`）为每一次流式
     `glBufferSubData` 录下的带 barrier 的 `vkCmdCopyBuffer`，在 Present 提交，带帧槽 fence。
   - 两者**帧序号相同**：`RegisterSubmit`（`VulkanRenderer.cpp:13458-13461`）记的是提交时刻的
     `m_bufferManager.GetFrameSerial()`。
2. **S1 很小，几乎立刻 signal。** `RefreshCompletedSubmits`（`:13463-13478`，前缀扫描）单独退休它，
   而 `OnSubmitsCompletedUpTo`（`:13480-13504`）**对每一条退休记录**调
   `m_bufferManager.NotifyFrameSerialComplete(record.frameSerial)`（`:13488`）——
   于是地板被推到 N-1，**而真正携带帧 N-1 缓冲拷贝的 S2(N-1) 还在执行**。
3. `GetCompletedSerial()`（`VkBufferManager.cpp:572-576`）=
   `max(m_frameSerial - frameCount, m_completedSerialFloor)`，现在返回 N-1。
4. 帧 N 对 buffer 1 的**第一次**写（调用 30366，`lastUseSerial = N-1`）于是没过
   `WriteWireBuffer` 的 busy 判据（`:254`），走了 `:268` 的**无同步 host memcpy**。
5. **S2(N-1) 排队中的 `vkCmdCopyBuffer` 随后落下，盖在刚写进去的字节上。**
   盖多少，取决于 memcpy 落地时 S2(N-1) 还剩哪些拷贝没执行——即**上一帧写列表的一个后缀**，
   撕裂长度就是那个后缀里的**最大尺寸**：
   - 后缀只剩 `…864, 6696, 3456, 216, 216` → 最大 6696 → 毁前 6696 字节 = q00..q30 = **`4e5ba514`**；
   - 后缀还含 12096 / 11880 / 12744（≥ 11448）→ 整个 11448 全毁 = **`fb75d412`**。

   这正是裁判说的「上一帧写列表的后缀极大值」，两档落在 6696 与 ≥11448 上。
6. **为什么日志逐行相同**：这条路径上没有任何日志语句——没有 decline、没有 GL 错误、没有 Fatal。
   draw 照常执行，只是读到了被覆盖的字节。

### 2.3 三条独立佐证

- **抛硬币**：是否出错，取决于 apply 线程走到 30366 时 S2(N-1) 有没有跑完。
  `MOBILEGL_PIPE_STATS=1`（每帧多一行日志）4/4 通过、harness 纹理 dump（多一次回读）把结果推到 0/5 金图，
  都是时序位移。
- **`RUN_AHEAD=0` 给第三张固定的图**：lockstep 让 client 在每条记录后等服务端，
  apply 线程到达 30366 的时刻相对 S2(N-1) 固定，后缀因此每次相同 → 一张确定的图。
- **`FRAMESINFLIGHT=4 / 8` 6/6 全是 `fb75d412`**（设备 E5）：帧在飞得更多 → GPU 更落后 →
  memcpy 落地时 S2(N-1) 剩的后缀更长、含更大的写 → **整笔全丢**，永远不是 6696 的部分丢。
  这条同时**排除**了计数项 `m_frameSerial - frameCount`：该项随 FIF 变大只会**更严**，
  所以被推错的是 `m_completedSerialFloor`，不是计数项——正如 H1 所说。

### 2.4 一条早先的错误诊断，如实记录

本包第一版把机制记在 `TryDrainFrameTransients` 每第 8 次 drain 的帧中 `BeginFrame` 上。
**那是错的**，而且本包**自己的探针就否掉了它**：整个 OpenRA 主机回放里 drain 只成功 **1** 次、
帧边界工作 **0** 次，33 次 `BeginFrame` **全部来自 Present**。裁判亦将该族（H4）判为在本 trace 不可达
（没有 sync object / map / PBO）。保留此段是因为该误诊曾写进过本文。

---

## 3. 修复

### 3.1 主修（强制）：让地板可证明

`VulkanRenderer::OnSubmitsCompletedUpTo`，全部在 `#if MOBILEGL_BUILD_DISAGGREGATED` 内
（`#else` 分支**逐字**保留今天的语句，这是 G1 的保证）：

```cpp
// 循环内：只累计，不通知
retiredAny = true;
advanceTo = std::max(advanceTo, record.frameSerial);
// 循环后：钳到「没有任何在飞提交仍持有」的序号
if (retiredAny) {
    for (const auto& remaining : m_inFlightSubmits)
        advanceTo = std::min(advanceTo, remaining.frameSerial > 0 ? remaining.frameSerial - 1 : 0);
    m_bufferManager.NotifyFrameSerialComplete(advanceTo);
}
```

`NotifyFrameSerialComplete` 本就单调且拒绝「仍在录制的序号」，所以这只会让地板**推得更晚**，
永不更远。**disaggregated 构建的 monolith 臂行为逐位不变**（一个序号一次提交——本包实测
monolith 30 次提交 / 32 个序号，且 `unsound` 计数为 0）。**复审轮限定**：这是 OpenRA 上的读数；
monolith 臂一旦有 mid-frame flush（一个序号两次提交），钳制同样生效，disagg 构建的 monolith 臂在那种
trace 上**比 pull 构建更可靠**——见 §7.2 (a)。

### 3.2 纵深（rule I 形状）：判据自己也带一条 fence

`WriteWireBuffer` 的 busy 判据加第二项（`VkBufferManager.cpp`，全部 disagg-only）：

```cpp
Bool busyBySerial = resource->lastUseSerial > GetCompletedSerial();          // 旧的计数项，保留
const Bool busyBySubmission =                                                // 新的 fence 项
    pVulkanRenderer != nullptr && !pVulkanRenderer->IsSubmitIndexComplete(resource->lastUseSubmitIndex);
if (busyBySerial || busyBySubmission) { /* 有序拷贝，或等 */ }
```

`WireBufferResource` 加 `lastUseSubmitIndex`，在 `AcquireWireSlice` / `StagedWireRangeCopy` 置为
`pVulkanRenderer->GetWireNextSubmitIndex()`（= `m_submitCounter + 1`，
`WireDraw.inc:113-118` 的 `RetireWireObjects` 早就用同一个数；**刻意不用**
`GetSyncPointSubmitIndex()`——它在「当前没有录制」时答 `m_submitCounter`，而
`SetupWireDraw` 取顶点切片早于 `BeginCommandRecording()`，正好落在这个洞里），
在 `WaitForWireBufferHostAccess` 等齐后清零。`IsSubmitIndexComplete` 对**尚未提交**的序号答 false，
对已提交的 poll 真 fence。

这一项在地板可证之后**本不该再触发**，它是纵深与设备探针。`GetCompletedSerial()` 本身**一行未动**
——它也服务 monolith 的 `VkBufferResource` 路径，改它会动 pull `.text`。

**复审轮更正（ID-P7-34 第 3 条）：提交项单独不可证，它不是修复。** `02bb5789` 的代码注释把它写成
「the fix」，且仍带着 §2.4 已否定的每第 8 次 drain 机制；这两点在 `VkBufferManager.cpp`
（`WriteWireBuffer` 的块注释、`AcquireWireSlice` 的盖戳注释）、`VkBufferManager.h` 的字段注释、
`MG_IntegrationTest/CMakeLists.txt` 与 `F1WireScenario.cpp` 的用例注释里全部改写为：
**地板是保证，提交项是探针与纵深**，drain 的故事删掉（drain 探针的注释改为「它否定了这个机制」）。
不可证的原因：`lastUseSubmitIndex` 在 `AcquireWireSlice` 盖戳，而 `BindProgramUniformBuffers` 之后仍可能经
`SyncWireTextureShape` 的 preserve 路径 → `FlushWirePendingCommandsForTextureUpdate` →
`FlushPendingCommands` 把**盖戳的那个序号**提交出去而**不带这个 draw**——戳名 S1，draw 坐 S2。
复审同时指出的两处漏项已补：

- `WaitForWireBufferHostAccess` 的早返回原来只看 serial 与 `gpuWritesPending`，现在还要求
  `IsSubmitIndexComplete(lastUseSubmitIndex)`（`pVulkanRenderer` 为空时退化为旧判据）；
- `ReadWireBuffer` 等待后恢复 `lastUseSerial` 时，同时把 `lastUseSubmitIndex` 重新盖成
  `GetWireNextSubmitIndex()`——保留的是同一个待录 draw 的两半预约。

### 3.3 未做的（按裁判的 MUST NOT / 债务清单）

- 裁判建议的**次修** `WaitForSubmitsUpTo(submitIndex, timeoutNs)` 聚合等待（用于
  `WaitForSubmitIndex` `:13766-13778`、`WaitForFrameSerial` `:13396-13410`、Present `:14094`）
  **本包未做**，理由与证据见 §7。
- `TryDrainFrameTransients` / `CollectWireObjects(all=true)` / `StagedTextureStore` 一律未动（记债）。
- 无新增 abort / Fatal；B 的回读聚合等待原样保留；
  `MOBILEGL_TEST_PIPELINE_CREATE_DELAY_MS` 与 `.PipelineDelay.` 条目原样保留。

---

## 4. red-once（规则 J）

### 4.1 (a) 确定性计数器：`unsound-serial-complete`

计数点就在 §3.1 的钳制处：**把地板推到某个仍有在飞提交持有的序号**。
有钳制时不可达；R-16 的还原就是删掉那个 `min` 循环。
（读数从日志取：本 trace 的渲染器**从不 `Shutdown()`**——进程直接退出——所以 teardown dump 报的是
起会上下文的 0，不是回放的计数；因此每个事件各打一行 `MGLOG_W`。）

| | monolith | inproc | spawn |
|---|---|---|---|
| **基线**（删掉 `min` 循环重建） | **0** | **26** | **26** |
| **修复后** | **0** | **0** | **0** |

三臂 retrace 全程 `PASS ssim=1.0 sha=ace2af04`（主机的 barrier 恰好把它救回来，见 §2.3；
主机上这个缺陷是**计数器可见、图不可见**——和包 B §5.2 的 `fences=2 vs 1` 同一形状）。
「退休时仍有在飞记录」monolith 也有 27 次，但那些记录持有的是**更高**的序号，所以 monolith 恒 0。

**复审轮：这个计数器现在是门（ID-P7-34 第 2 条）。** 初版的主修没有车道——删掉钳制循环，所有车道
仍绿、主机图仍金，唯一可观测的就是这行 `MGLOG_W`。`tools/trace_replay/run_trace_case.cmake`
现在在每次 split retrace（inproc / spawn / tcp）读**两个角色的日志**里的
`MGWIRE-FLOOR unsound-serial-complete`，和它读 `Fatal{` 完全同形：通过与否都打印条数，非零即
`FATAL_ERROR`。红/绿（R-16，已执行并还原）：

```
# 删掉 OnSubmitsCompletedUpTo 的钳制循环重建
MobileGLTraceReplay.OpenRA.DirectVulkan.SPLIT   ***Failed   (ssim=1.000000, mismatchPixels=0)
-- MGPipe split: OpenRA DirectVulkan transport=inproc, MGWIRE-FLOOR unsound-serial-complete lines: 26
-- [mgl-srv-apply/WARN]: MGWIRE-FLOOR unsound-serial-complete #1: advancing the completed frame-serial
   floor to 3, which submission 3 (serial 3) still carries
CMake Error at run_trace_case.cmake:421: OpenRA DirectVulkan: 26 MGWIRE-FLOOR unsound-serial-complete line(s) ...
MobileGLTraceReplay.OpenRA.DirectVulkan.SPAWN   ***Failed   (transport=spawn, 26 行，逐字同上)
# 还原
.SPLIT / .SPAWN   Passed   ssim=1.000000 / 0 px / 0 行
```

**修正轮（fable 复审）：spawn 的服务端日志缺失即红。** 这行只由**服务端**的 `OnSubmitsCompletedUpTo` 打；
复审轮的 runner 对服务端日志是 `if(EXISTS …)`，一个在第一行之前就死掉的服务端读作「0 行」= 绿。
client 日志缺失与 tcp 臂的服务端日志缺失本就是 FATAL，普通 spawn 现在也是。演示：SPAWN 的 ctest 命令行，
把 `TRACE_REPLAY_EXE` 换成「跑真回放、然后删掉 `output/mobilegl.server.log`（541 行）」的包装：

```
复审轮 runner（aab01275）   rc 0   Fatal{ lines ...: 0 / MGWIRE-FLOOR unsound-serial-complete lines: 0
修正轮 runner               rc 1
CMake Error at run_trace_case.cmake:321 (message):
  OpenRA DirectVulkan: MOBILEGL_TRANSPORT=spawn but the run wrote no
  .../output/mobilegl.server.log, so the server role's Fatal{ and MGWIRE-FLOOR
  lines cannot be counted.  A spawn retrace with no server log cannot be
  counted as a clean one.
```

**修正轮 2：这条 FATAL 移到 marker 与 ARMED 检查之后，并且 inproc 也要服务端日志。** 修正轮把它放在了
marker 检查与 `spawn ARMED … pid` 检查**之前**，于是一次启动/握手失败（没有服务端进程、也没有 ARMED 行）报的是
「服务端没留下文件」，而不是更具体的「resolved, but carries no spawn ARMED」，把读者指向错误的进程。现在它紧挨在
census 之前（`:384`），tcp 臂原来同样位置的「no forwarded server log」检查随之并入；也不再按 transport 分支：
runner 承认的每一行 transport 都有服务端角色的文件——spawn 是服务端进程写的（tcp 是夹具转发的行落在同一文件），
inproc 是 `mgl-srv-apply` 线程的行按线程角色路由到同一个 `.server.log` sink（首条服务端角色行时惰性打开），
而本车道要求的 INFO 档下 `ServerLoop.cpp:365` 在 apply 线程启动时就写一行，所以干净的 inproc 回放**一定**有这个文件
（修正轮那句「inproc 仍是 `if(EXISTS …)`」到此为止）；两条 census 的读取也不再 `if(EXISTS …)`。
演示（OpenRA DirectVulkan，包装真回放、回放之后毁掉一份日志）：

```
1. inproc，删服务端日志（536 行）
   修正轮 runner（53471e7b）  rc 0   Fatal{ lines ...: 0 / MGWIRE-FLOOR unsound-serial-complete lines: 0   ← 误绿
   修正轮 2 runner            rc 1
   CMake Error at run_trace_case.cmake:384 (message):
     OpenRA DirectVulkan: MOBILEGL_TRANSPORT=inproc but the run wrote no
     .../output/mobilegl.server.log, so the server role's Fatal{ and MGWIRE-FLOOR
     lines cannot be counted.  A split retrace with no server log cannot be
     counted as a clean one.
2. spawn，删服务端日志（541 行）
   修正轮 2 runner            rc 1   同上，`MOBILEGL_TRANSPORT=spawn but the run wrote no …`，同在 `:384`
3. spawn，删服务端日志**并**删掉 client 日志里的 `spawn ARMED` 行（启动/握手失败的形状）
   修正轮 runner（53471e7b）  rc 1   run_trace_case.cmake:321 … but the run wrote no …server.log   ← 指向服务端
   修正轮 2 runner            rc 1
   CMake Error at run_trace_case.cmake:356 (message):
     OpenRA DirectVulkan: .../output/mobilegl.client.log reports
     MOBILEGL_TRANSPORT=spawn resolved, but carries no "spawn ARMED - the server
     role runs in pid ".  That sentence is written by
     ClientSession::StartSpawned AFTER the launch, the connect and the handshake
     all succeeded, ...
4. spawn，只删 `spawn ARMED` 行（服务端日志保留）
   修正轮 2 runner            rc 1   同 3，`:356` 的 ARMED 消息
```

不毁日志时 OpenRA DirectVulkan **与 DirectGLES** 的 SPLIT / SPAWN 四条全绿（ssim 1.000000 / 0 px / `MGWIRE-FLOOR` 0 行；
inproc 的服务端日志 DirectVulkan 536 行、DirectGLES 987 行）。

**修正轮 3（nit）：并入的那条 FATAL 也报 `MOBILEGL_IPC_CONTROL`。** 修正轮 2 把 tcp 臂的「TCP session has no forwarded
server log」并进了上面这条对所有 transport 的检查，消息里只剩 `MOBILEGL_TRANSPORT=…`——而 tcp 臂就是
`MOBILEGL_TRANSPORT=spawn` 加 `MOBILEGL_IPC_CONTROL=tcp://…`，所以 tcp 臂丢了夹具转发的服务端日志时，读起来和普通 spawn 一样。
消息现在在 transport 之后带 `MOBILEGL_IPC_CONTROL='…'`（inproc 与普通 spawn 为空）；只改消息，检查没有动。
演示 1（inproc，删服务端日志）在新 runner 上重跑：rc 1，
`OpenRA DirectVulkan: MOBILEGL_TRANSPORT=inproc MOBILEGL_IPC_CONTROL='' but the run wrote no …/mobilegl.server.log, …`（`:388`）。
tcp 臂本轮没有跑（要起 `tcp_server_fixture.py`；消息只是 `$ENV{}` 的展开）。

**图是金的、用例是红的**——这正是这条门要的：它红在「地板断言了一次没等过的完成」，
不管这台驱动的时序有没有把它变成像素。

### 4.2 (b) 车道用例：像素级的红，在两进程臂上

`F1WireScenario.StreamedBufferSubDataBeforeEachDrawIsOrdered`：8×8 FBO，一个
`GL_DYNAMIC_DRAW` VBO（不 orphan），四批顶点依次写进 **offset=0 的同一区间**再立刻 `glDrawArrays`，
每批画自己那两列、用自己的颜色；回读断言每一列是自己的颜色。
用例**不能**只断言「无 GL 错误」——这条路径上没有错误可断言，只有像素。

三臂各注册 `Fm.`（无旋钮，回归门）；`StaleSerial.`（带
`MGITEST_MAGMA_FORCE_STALE_BUFFER_SERIAL=1`，即 red-once）**只注册 split 与 spawn**。该旋钮让 §3.2 的
**serial 项**说谎（连同地板一起绕过），**不**绕过有序拷贝，所以它钉的是**纵深的提交项**，不是地板；
地板的车道是 §4.1 的 retrace 门。

**复审轮（ID-P7-34 第 1 条）：tcp 臂的 `StaleSerial.` 是一个永远不会红的名字，已撤。** 旋钮由**服务端**读；
tcp 臂的服务端是车道夹具（`scripts/ci/tcp_server_fixture.py`），从它**自己**的环境起一次，
条目级 `ENVIRONMENT` 到不了它——`DirectVulkan.Tcp.StaleSerial.*` 跑的是未强制的用例，却顶着旋钮的名字。
inproc 的服务端在测试进程里、spawn 的服务端继承 client 的环境，那两臂上旋钮是真的（§4.2 基线正是两臂红）。
`scripts/ci/spawn_lane_parity.py` 的 server 端旋钮例外表 **`MAGMA_SERVER_ENV_KNOB_NO_TCP`** 加入 `.StaleSerial.`（集成时并入 B2 返工的同一机制 `compare_arms(no_tcp=)` / CMake `mglItestServerEnvKnobArm`，而不是包树里单独的 `MAGMA_NOT_OVER_TCP`）
（与 ID-P7-14 的 `MAGMA_INPROC_ONLY` 同形）：只从 **tcp 臂**的比较基准里去掉，spawn 缺了它仍是错误。
B2 在自己的树里把它的旋钮条目加进同一个元组，集成者合并两处 hunk。
（落地：`pipe@6207c9c9` 上这一项并进了 B2 的 **`MAGMA_SERVER_ENV_KNOB_NO_TCP`**，打印措辞见 §5。）

**基线（把 `busyBySubmission` 临时改回 `false` 重建；R-16，已执行并还原）：**

```
ctest -R "DirectVulkan\.(Split|Spawn)\.(Fm|StaleSerial)\.F1WireScenario\.StreamedBufferSubDataBeforeEachDrawIsOrdered" --output-on-failure

1/4 Test #4274: DirectVulkan.Split.Fm.         ...   Passed
2/4 Test #4275: DirectVulkan.Split.StaleSerial....***Failed
3/4 Test #4317: DirectVulkan.Spawn.Fm.         ...   Passed
4/4 Test #4318: DirectVulkan.Spawn.StaleSerial....***Failed
    F1.StreamedSubData: batch 0 column 0 lost its own vertices (r)
    F1.StreamedSubData: batch 0 column 1 lost its own vertices (r)
    F1.StreamedSubData: batch 1 column 2 lost its own vertices (g)
    F1.StreamedSubData: batch 1 column 3 lost its own vertices (g)
    F1.StreamedSubData: batch 2 column 4 lost its own vertices (b)
    F1.StreamedSubData: batch 2 column 5 lost its own vertices (b)
50% tests passed, 2 tests failed out of 4
```

前三批丢了自己的顶点、**最后一批没丢**——「所有 draw 都读到了最后一次写的字节」。修复后 **4/4 绿**。

### 4.3 (c) 强制条件下的 OpenRA retrace（主机，真图）

同一旋钮打在 OpenRA split retrace 上（`MOBILEGL_TRANSPORT` 分别 `inproc` / `spawn`，
两进程那遍 client 日志有 `transport=spawn`、`server pid=…`）：

| | selector | inproc | spawn |
|---|---|---|---|
| **基线** | `=1`（全毒） | FAIL ssim 0.270612 / 152216 px / sha `8302218f` | 逐字相同 |
| **基线** | `=594`（只毒一对） | FAIL ssim 0.987890 / **8102 px** / sha `69b2a2fe` | 逐字相同 |
| **基线** | `=601`（只毒一对） | FAIL ssim 0.970775 / **2115 px** / sha `fa6b6730` | 逐字相同 |
| **修复后** | `=1` / `=594` / `=601` | PASS ssim **1.000000** / **0 px** / sha **`ace2af04`** | 逐字相同 |

毒**一次**写 = 丢一块**有界**的像素、每个 selector 一张确定的图（扫描 N=586…601 给出 2115…29490 px
的一串互不相同的 sha）——与设备上 0 / 9836 / 14658 的量化形状同类。
inproc 与 spawn **sha 逐字相同**，证明机制在**服务端**。

按 lens A 的口径做连通域分析，强制图给出两种解码结局：`N=601` 最大块金图是 “Quit” 按钮、
实际画出**字体图集的整条字形带**（梯度比 6.6，读到了别批的顶点）；`N=594` 的块金图是 “Multiplayer”、
实际**几乎空白、底色完好**（梯度比 0.200，什么都没画）。设备上 lens D 量到的是后一种。

### 4.4 裁判建议的 (b) GPU-drag 旋钮：**未做**

裁判建议用 `MOBILEGL_TEST_WIRE_GPU_DRAG_FILLS` 在每帧命令缓冲头部插 n 次 `vkCmdFillBuffer`
把 Present 提交拖慢，好让主机也出一张**真实**（非强制）的前缀撕裂图。
本包**没有做**，理由是它在本包的修复下已无法变红（§3.1 的地板与 §3.2 的 fence 项都不依赖时序），
也就不再是 red-once，而是一条新的回归门；而裁判自己也把「lavapipe 的队列线程能否拖够久」列为
未决。本包已有的 red-once 是 §4.1 的确定性计数器（B §5.4 的形状）加 §4.2 的两进程像素用例，
两者都不靠时序。**如实记录**：主机上因此**没有**一张自然发生的错图，只有计数器与强制图。

### 4.5 范围外：pull 库对照自 P6 日志改名起就到不了绿（复审点名，修正轮 2 修）

`scripts/ci/retrace_pull_library_control.sh`（`test.yml` retrace-split 矩阵的两行 transport 都跑它）要求 red 里带
`run_trace_case.cmake` 的 `never reported resolving it`——那是 marker 搜索那一句。P6 一角色一日志的改名（`b3155525`）
之后 pull 库到不了那一步：pull 构建只有一个角色，`RoleLogPath` 原样返回 `MOBILEGL_LOG_FILE_PATH`（`MG_Util/Debug/Log.h`
的 pull 臂），写的是 `output/mobilegl.log`；runner 读的是 `output/mobilegl.client.log`（`run_trace_case.cmake:115`），
于是在 marker 搜索**之前**一步就 FATAL（`:269-273`：「the run wrote no …client.log, so there is no evidence the transport
ever resolved」）。这句里没有对照要的话，对照的第二道门（「red 必须是本对照自己的 red」）把自己的 red 拒了，步骤 rc 1。

**本树实测**（真 ctest；`build-linux` 的 pull 库盖到 `build-split` 的冻结路径上，`SPLIT_TRANSPORT=inproc`，
未加后缀的 `MobileGLTraceReplay.OpenRA.DirectVulkan` 条目，与 CI 同形）：

```
pull control library: .../build-split/libMobileGL.so: MG_Remote=0
CMake Error at run_trace_case.cmake:270 (message):
  ... .../OpenRA/DirectVulkan/output/mobilegl.client.log,
  so there is no evidence the transport ever resolved.  A split retrace with ...
::error::the split retrace went red (ctest exit 8) with the pull library in place, but the failure
  never says the library did not resolve the transport - run_trace_case.cmake's "never reported
  resolving it" is absent from the output. ...
控制脚本 rc=1
```

修法一处：`EVIDENCE` 变成 `grep -F` 的两行模式（`never reported resolving it` 与 `no evidence the transport ever resolved`），
红检扰动的那一行（`grep -qF "${EVIDENCE}"`）原样不动；两句都是同一条 transport 身份断言以同一理由拒绝同一个库
（pull 构建没有 split 日志可供 marker 落脚），都不是 loader 失败 / 缺夹具 / 超时 / SSIM 掉——那才是第二道门要拒的。
`stub_ctest.sh` 加 `retrace-evidence-nolog`（按 CMake 折行的样子复现这句），`control_smoke_test.sh` 期望它 PASS
（22 → **23**/23），`redcheck_control_smoke_test.sh` 仍把「red without the transport-resolution message」变红。
修后本树实测 inproc 与 spawn 两臂的对照都 rc 0：「the pull library turned the split retrace red for its own reason
(ctest exit 8): 1 selected case(s) named the transport, not the picture」。CI 上这一步自改名以来的读数本包看不到，
只能说：按同一份代码它每次都会 rc 1。

**修正轮 3（nit）**：`EVIDENCE` 的每一行都是一个 `grep -F` 模式，而**空行是匹配一切的模式**——赋值时多一个尾随换行、
或两句之间空一行，第二道门就退化成「任何红都行」；smoke 的「red without the transport-resolution message」会抓到（22/23），
但那是有人跑它的时候。现在赋值之后紧跟一个 `case`：`EVIDENCE` 含空行、或以换行开头/结尾，即 `::error::` 并 rc 1，
在计数选择、换库之前。红检扰动的那一行（`grep -qF "${EVIDENCE}"`）不动；smoke 仍 **23/23**、红检 rc 0。
顺带：`stub_ctest.sh` 在修正轮 2 加 `retrace-evidence-nolog` 的那个提交里从 100755 变成了 100644（Windows 挂载的痕迹），
`git update-index --chmod=+x` 恢复；smoke 拷贝后自己 `chmod +x`，所以没有任何东西跑得不一样。

---

## 5. 门

（初版——包树 `0d425499`，合并前；`~/w7/b3-out/gate-h1.log`，脚本按 `~/w7/logs/p7-gate-c-inner.sh` 的量法。）

- **G1**：`build-linux`（pull）`.text` = **`0xa52203`**，`nm --defined-only` 对
  `~/w7/p7-before/pull-syms.txt` **added=0 / removed=0**，编译 rc 0（含测试，ID-P7-11）。
  `OnSubmitsCompletedUpTo` 的 `#else` 分支逐字保留原语句；其余改动全部在
  `#if MOBILEGL_BUILD_DISAGGREGATED` 内。
- `scripts/ci/fatal_census.py` rc 0：**79 abort / 20 文件、44 家族词、3 refusal 词、0 无标记**——与基线同数。
  本包不新增 abort 站点，也不新增家族词：所有新拒绝都是 decline。`@P7` 站点 **12**，不变。
- `scripts/link_ratchet.py --assert-monotone`：**unchanged at 186**，不上升。
- `scripts/ci/spawn_lane_parity.py build-split`：rc 0。

| 车道 | 基线 | 本包 | 差 |
|---|---|---|---|
| `unit` | 2420 | 2420 | 0 |
| `integration-split` | 226 | 226 | 0 |
| `integration-magma-split` | 88 | 90 | +2 |
| `integration-magma-spawn` | 67 | 69 | +2 |
| `integration-magma-tcp` | 69 | 71 | +2 |
| `integration-magma-full-split` | 526 | 527 | +1 |

**复审轮的门——包树 `774bc17f`，合并前**（`~/w7/b3bin/gate.log`，同一量法）：G1 编译 rc 0、`.text` **`0xa52203`**、
nm **added=0 / removed=0**；`fatal_census.py` rc 0（**79** / 20 文件 / 44 / 3 / 0 无标记）；
`wire_declines_audit.py` rc 0（53 / 53 / 0 / 0）；`link_ratchet.py --assert-monotone` **unchanged at 186**；
`spawn_lane_parity.py build-split` rc 0；`@P7` 站点 12。车道全部 100%。

**落地树 `pipe@6207c9c9`，集成者的门**：G1 rc 0、`.text` **`0xa52203`**、nm **0 / 0**；census **79**；
`link_ratchet.py` **173**；`spawn_lane_parity.py` rc 0；audit **53 行 / 53 有站点 / 0 未记日志**；
OpenRA DirectVulkan inproc + spawn retrace ssim **1.000000**、**0 px**、`MGWIRE-FLOOR unsound-serial-complete` **0** 行。
合并时集成者把本包的 `MAGMA_NOT_OVER_TCP` 并进了 B2 的 **`MAGMA_SERVER_ENV_KNOB_NO_TCP`**
（`compare_arms(no_tcp=)`、CMake `mglItestServerEnvKnobArm`），落地的 parity 脚本对该表打印
「`N server-env-knob entrie(s) excluded from the TCP comparison only`」（§4.2 的元组以落地名为准）。

| 车道 | 初版 | 复审轮（包树 `774bc17f`，合并前） | 落地树 `pipe@6207c9c9`（集成者的门） |
|---|---|---|---|
| `unit` | 2420 | 2420 | 2429 |
| `integration-split` | 226 | 226 | 304 |
| `integration-magma-split` | 90 | 90 | 96 |
| `integration-magma-spawn` | 69 | 69 | 75 |
| `integration-magma-tcp` | 71 | **70**（−1，`Tcp.StaleSerial.` 撤，§4.2） | 71 |
| `integration-magma-full-split` | 527 | 527 | 535 |
| `link_ratchet.py` | 186 | 186 | 173 |

落地树的车道数含同一波其它包合入的条目，与包树之差不是本包的增量。

**修正轮的门——包树 `88d1a913`（修正轮最后一个代码/文档提交），合并前**（`~/w7/b3bin/r2/gate2.log`）：
split 构建 rc 0；G1 rc 0、`.text` **`0xa52203`**、nm **0 / 0**（唯一的产品文件改动是 disagg-only 的注释与一条日志措辞）；
census rc 0 **79**；audit `--self-test` **8/8** rc 0、树上 **53 / 53 / 0** rc 0；ratchet **unchanged at 186**；parity rc 0；
`unit` **2420/2420**、`integration-magma-split` **90/90**、`integration-magma-spawn` **69/69**；
OpenRA DirectVulkan SPLIT / SPAWN PASS、ssim 1.000000、0 px、`MGWIRE-FLOOR` 0 行。

**修正轮 2 的门——包树 `3977901f`（修正轮 2 最后一个代码提交），合并前**（`~/w7/b3bin/r3/gate3.log`）：
split 构建 rc 0；G1 rc 0、`.text` **`0xa52203`**、nm **0 / 0**（产品文件改动只有两处 disagg-only 的注释）；
census rc 0 **79**；audit `--self-test` **14/14** rc 0、树上 **53 / 53 / 0** rc 0；ratchet **unchanged at 186**；parity rc 0；
`control_smoke_test.sh` **23/23** rc 0、`redcheck_control_smoke_test.sh` rc 0；
`unit` **2420/2420**、`integration-magma-split` **90/90**、`integration-magma-spawn` **69/69**；
OpenRA DirectVulkan SPLIT / SPAWN PASS、ssim 1.000000、0 px、`MGWIRE-FLOOR` 0 行（DirectGLES 两臂同，§4.1）；
§4.1 的四个 fail-closed 演示与 §4.5 的 pull 库对照（inproc 与 spawn 各一次，rc 0）。

**修正轮 3 的门——包树 `09cbdb41`（修正轮 3 最后一个代码提交），合并前**（`~/w7/b3bin/r3/f3/gate.log`）：
本轮**没有产品文件改动**（`git diff 32edcc46..09cbdb41 -- MobileGL/` 为空），G1 不受影响——`build-linux` 重跑为 no-op，
`.text` **`0xa52203`**、nm **0 / 0** 照旧；census rc 0 **79**；audit `--self-test` **16/16** rc 0、树上 **53 / 53 / 0** rc 0；
`control_smoke_test.sh` **23/23** rc 0、`redcheck_control_smoke_test.sh` rc 0；OpenRA DirectVulkan SPLIT / SPAWN PASS、
ssim 1.000000、0 px、`MGWIRE-FLOOR` 0 行（runner 只改了一条消息）；§4.1 的演示 1 在新 runner 上重跑（消息带 `MOBILEGL_IPC_CONTROL=''`）。
车道没有重跑：无产品改动，也没有改到任何测试或 harness 文件。

包树上外加 `MobileGLTraceReplay.OpenRA.DirectVulkan.SPLIT` / `.SPAWN`（带 §4.1 的新红条件）：
PASS、ssim 1.000000、0 px、`MGWIRE-FLOOR` 0 行。为跑这两条，本树 `build-split` 以
`-DMOBILEGL_BUILD_TRACE_REPLAY=ON` 重配，并初始化了 `3rdparty/apitrace` 的嵌套子模块（只动子模块工作树）。

---

## 6. 集成者要跑的设备验收

Redmi（Adreno 830）、OpenRA、DirectVulkan、`--use-pbuffer`：

- 臂：**inproc 与 spawn 各一组**；档：默认 run-ahead 与 `MOBILEGL_IPC_RUN_AHEAD=0` 各一组；
- 每组 **6 遍**（`pm clear` 冷跑）；
- 判据：**每一遍** sha256 = `ace2af04`、ssim **1.000000**、mismatch **0 px**——4 组 × 6 遍 = 24 遍全金，零翻转。

外加三条预测检查（H1 若成立必须成立）：

1. **带 harness 纹理 dump**（`--dump-texture-2d 31249,3,0`）再跑 3 遍。今天它把结果偏到 **0/5 金图**，
   是最灵敏的探针；修复后应当全金。
2. **`MOBILEGL_MAGMA_FRAMESINFLIGHT=8` 跑 3 遍**。今天 FIF=4/8 各 3 遍 **6/6 全是 `fb75d412`**；
   修复后应当全金。
3. **`MOBILEGL_MAGMA_WIREBUF_PROBE=1` 各跑一遍**，`grep -c unsound-serial-complete`：
   修复前应为非零（主机 26/帧序号级别），修复后应为 **0**。

H1 还预测：**修复前**每一张错图都是调用 30376 的四边形**前缀** q00..qK，
`K+1 ∈ {1, 16, 31, 53}`（对应上一帧写列表后缀极大值 216 / 3456 / 6696 / ≥11448）。
若设备上出现一张**非前缀**的错图，H1 就不完整。

---

## 7. 欠的、不声称的、给别人的

### 7.1 次修（聚合等待）未做——以及它为什么没有立刻变成红

裁判建议把 `WaitForSubmitIndex`（`:13766-13778`）、`WaitForFrameSerial`（`:13396-13410`）、
Present（`:14094`）三处「等一条 fence、把它之前的都记成完成」换成一个聚合等待
`WaitForSubmitsUpTo`。裁判同时提醒：地板变可证之后，`WaitForFrameSerial` 的注释
（`:13398-13401`）所依赖的「逐记录推进地板」没有了，**若不聚合等待，`IsFrameSerialComplete`
可能永远为假，timer-query 收割（`:13875`）会卡住**。

本包**没有**做这一步，并给出实测：本包的主修落地后，
`unit` 2420、`integration-split` 226、三条 magma 臂 90/69/71、`integration-magma-full-split` 527
**全部 100% 通过**，OpenRA 三臂 retrace 全金——即在本树的车道覆盖下，收割没有卡住。
这**不等于**该风险不存在：`IsFrameSerialComplete` 的消费者在没有 Present 的循环里才会暴露，
而本树的车道里那类循环很少。**建议集成者把聚合等待作为 B4 单独派**，连同 §7.2 的 H2 债一起做，
理由是它需要自己的 red-once（一条无 Present 的 fence 循环用例），不该搭在本包里。

### 7.2 记进 `CONTRACT-P7.md` §12 的债（裁判点名，本包不碰）

- **H2**：`WaitForSubmitIndex` / `WaitForFrameSerial` / Present `:14094` 的单 fence 退休，
  与包 B §5.3 修掉的是同一形状；在本 trace 不可达，但是真债。
- `TryDrainFrameTransients` 的 `CollectWireObjects(m_completedSubmitCounter, /*all=*/true)`（`:13526`）
  无视 `RetireWireObjects` 给未来对象打的 `m_submitCounter + 1` 标（`WireDraw.inc:113-118`）。
- `HasPendingRecordedWork`（`:13434-13440`）对 pre-pass 命令流是盲的。
- `FlushWirePendingCommandsForTextureUpdate`（`VulkanRenderer.h:149-151`）把
  「此刻没有录制」当成「没有在飞」。
- `StagedTextureStore` `AdoptRun` 缺 extent-move 复位（`StagedTextureStore.h:234-243` vs `:270-284`）。
- **陈旧注释（复审点名，本包记债不改）**：
  - `VkBufferManager::CollectAllDeferredReleases`（本树 `VkBufferManager.cpp:661-675`）的
    「**Mid-frame drains do not advance m_frameSerial**」在今天的树上**是错的**（`TryDrainFrameTransients`
    每第 8 次 drain 会调 `BeginFrame`）。结论（不在该处回收 arena 存储）仍然正确，只是理由写错了；
    改注释会和 B2 在同文件区域的改动打架。
  - `VulkanRenderer::WaitForFrameSerial` 里 fence 等待循环上方的注释（包树 `774bc17f` 为
    `VulkanRenderer.cpp:13400-13403`，落地树 `pipe@6207c9c9` 为 `:13529-13532`、引用的那句在 `:13531-13532`；
    以函数名为准）的
    「OnSubmitsCompletedUpTo calls NotifyFrameSerialComplete for every record it retires, so the
    completed-serial floor still advances correctly after one fence wait」——disagg 构建上 §3.1 之后
    **不再逐记录通知**，地板只推到无在飞持有的序号。该函数是 pull 与 disagg 共用代码，注释归 B4 一起改。
- **`WaitForFrameSerial` 在 disagg 构建上可能答 true 而序号并未完成（修正轮，fable 复审；写给集成者抄进 §12，
  本包不修）**：它等的是 `m_inFlightSubmits` 里**第一条** `frameSerial >= serial` 的记录，
  `OnSubmitsCompletedUpTo(record.submitIndex)` 之后就 `return true`。有了 §3.1 的钳制，若**更晚**的一条记录
  （S2，Present）持有**同一个**序号——正是 §2.2 的「一个序号两次提交」——地板到不了 `serial`，于是函数返回 true
  而 `IsFrameSerialComplete(serial)` 为 false。唯一的调用者 `WaitForTimerQueryResult` 返回前重查
  `IsTimerQueryResultReady(record)`，所以今天它退化为一次**假超时**（结果这次拿不到、下次再收），不是错读。
  **修法归 B4**：`OnSubmitsCompletedUpTo` 之后只在 `IsFrameSerialComplete(serial)` 为真时返回，否则**从头重扫**
  ——`OnSubmitsCompletedUpTo` 会从正在迭代的 vector 里 erase，所以是 restart，不能 continue；
  与 §7.1 的聚合等待与上面的注释一起做。**修正轮 2 补给 B4 的一句**：`OnSubmitsCompletedUpTo(record.submitIndex)`
  之后**不得再读 `record`**——`WaitForFrameSerial` 的 range-for `for (const auto& record : m_inFlightSubmits)`
  （包树 `:13404`，落地树 `:13533`）拿的是 `m_inFlightSubmits` 元素的引用，而 `OnSubmitsCompletedUpTo`
  正是从这个 vector 里 erase；要用的字段先拷出来（`RefreshCompletedSubmits` 的
  「Copy before OnSubmitsCompletedUpTo erases the front record」——包树 `:13471`，落地树 `:13600`——就是这条规则）。
- **`run_trace_case.cmake` 的 census 排在 replay 退出码之后（修正轮 2 记债，本轮不动）**：`:172-174` 对非零
  `replay_result` 直接 `FATAL_ERROR`，两条 census（`Fatal{` 与 `MGWIRE-FLOOR`，修正轮 2 之后在 `:390` 与 `:416` 起）都在它之后——一次带 abort 的
  回放只留下退出码，日志里的 `Fatal{` 名字与地板计数不会被打印出来，读者得自己去翻两份日志。census 应当先于退出码判定
  读两份日志（存在则读）；与 §4.1 的 fail-closed 顺序一起改时要分清：「无服务端日志即 FATAL」是对**退出码为 0** 的回放说的。
- **`run_trace_case.cmake` tcp 臂的「pid arm proof」检查仍排在 marker 检查之前（修正轮 3 记债，本轮不动）**：`:306-307` 对
  `control=tcp data=stream server=… pid=…` 的检查在给 `split_expected_second` 赋值的分支里就 `FATAL_ERROR`，在 `:321` 的
  marker 检查**之前**——和修正轮 2 刚给服务端日志 FATAL 改掉的是同一种误诊次序：一次从未 resolve 的 tcp 回放报的是
  「TCP session has no endpoint and server pid arm proof」而不是「never reported resolving it」。低：CI 的 retrace-split
  矩阵只跑 inproc 与 spawn，tcp 臂只在主机上跑；改法是把它挪到 `split_expected_second` 检查的位置。
- **`wire_declines_audit.py` 的 `LOG_WINDOW` 把跳过的行也计入（修正轮 3 记债，不改）**：`has_own_log` 的 8 行窗口按源文件行数走，
  空行与它跨过的 `#if`/`#endif` 都占名额——一条日志在 `Count` 上方第 8 行、中间隔两个空行，就被判成「8 行内没有日志」
  （误红，fail-closed 方向）。树上今天没有这样的站点（53 / 53 / 0）；改法是只数非空、非预处理的行。
- **disaggregated 构建的 monolith 臂现在比 pull 构建更可靠（复审 (a)）**：monolith 的
  `VkBufferResource` 路径 `OnSubData` 经 `IsResourceBusy`（`VkBufferManager.cpp:709`）读的是**同一个**
  `GetCompletedSerial()` 地板，所以 §3.1 的钳制在 disagg 构建里也保护了 monolith 臂的 mid-frame flush；
  **pull 构建**的 `#else` 分支逐字保留（G1），**潜在缺陷原样留在 pull 构建里**——由 G1 与 P13 持有，本包不碰。
- **复审的其余结论，照录**：钳制算术对所有退休次序逐一验算**精确**、无卡死模式、`#else` 逐字；
  裁判的次修（聚合等待）判为**非缺陷**——B4 是后续，不是本包的前置条件（§7.1）。

### 7.3 给 M1：OpenRA 的真实提交结构（回答「present 只发生一次吗」）

**不是**——至少在 OpenRA 上不是。本包实测（§2.2 表）：三臂 `BeginFrame` 都是 **33** 次，
最大帧序号 32，而整个回放里 `TryDrainFrameTransients` 只成功 **1** 次、其中走到第 8 次帧边界工作的
**0** 次。也就是说这 33 次帧序号自增**全部来自 Present**，OpenRA 的 pbuffer 回放**每帧都有 present**。

M1 在 bsl-esc-menu 上量到的 `present=1` 与此**不一致**，所以那是**该 trace / 该 surface 的**事实，
不是「pbuffer 一律不发 present 记录」的通则。本包**没有**改任何 client 发射路径（按要求）。
承载帧 N-1 流式拷贝的是 **Present 提交 S2(N-1)**；把地板推错的是**调色板 flush 的 pooled fence
S1(N-1) 退休**（§2.2 第 2 点）。若 bsl-esc-menu 上真的没有 per-swap present，
那么在那条 trace 上 S2 是别的东西（最近一次 `FlushPendingCommands`），
地板与 arena 的推进几乎停滞——那是**另一个**问题（服务端帧边界被饿死），
按 M1 的判断应当单独成包，本包不动。

### 7.4 不声称

- 不声称解释了设备上 9836 / 14658 两档的**逐 case 归属**——§2.1 的算术（216 B/四边形、
  6696 = 31 个四边形）与后缀极大值的读法来自裁判的取证加本包的主机写列表，**未在设备上直接验证**。
  §6 的预测检查就是为此而列。
- 不声称主机上能自然复现错图：本包在主机上只有计数器（26 → 0）与强制图。
- 规则 I 的 tally 覆盖的是 **draw/dispatch** 路径；`WireFramebuffer.inc` 的 blit/mipmap 路径由
  B / B2 持有，本包按文件分区没有碰。
