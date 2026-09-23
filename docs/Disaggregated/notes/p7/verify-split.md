# P7 wave 3 · V1：出口门 2 —— verify × split（分支 `p7/verify-split`）

> 计划见 [`PLAN-PH-P34B-P7.md`](PLAN-PH-P34B-P7.md) 「wave 3」与 §4 分母第 (7) 项；规范见
> [`CONTRACT-P7.md`](../../../../MobileGL/MG_Remote/CONTRACT-P7.md) §6（门 2）、§0（规则 I / J）、§9。
> 基线 = `3c2867d3`。
>
> 主机口径：WSL Arch + lavapipe（`/usr/share/vulkan/icd.d/lvp_icd.json`，钉在 `MOBILEGL_ITEST_VK_ICD`），
> `build-split` = Release / clang / ccache / `DISAGGREGATED=ON` `INPROC=ON` `PIPE_PUSH=ON`
> **`PIPE_VERIFY=ON`** `BUILD_INTEGRATION_TEST=ON`（trace 一轮时另开 `BUILD_TRACE_REPLAY=ON`）。
> 棘轮另在同配置、`PIPE_VERIFY=OFF` 的 `build-split-nv` 上读（§5 说明为什么必须如此）。

## 0. 提交

| # | 提交 | 内容 |
|---|---|---|
| 1 | `70f86064` | 前置债：`PipeRespecifyScope` 放宽为「按 level 的 producer 覆盖其宣告范围」，正反单测各一 |
| 1′ | `0d4043d4` | 1 的补丁：新单测的 helper 在 pull 构建里引用了不存在的 `MGPipeApplier()`，pull 构建编不过；改为只在 `MOBILEGL_PIPE_PUSH` 下编译 |
| 2 | `25e64de2` | `integration-verify-split` 车道（双后端 × inproc）、逐条目私有日志、CI 作业与逐条目臂证明；比对器在 server `read_pixels` 窗口内对 pack 半字段的预言改为 ID-49 中性 pack（§2.2 发现 A） |
| 3 | `da3718c4` | 两条负控在 split 臂上的登记与 CI 红一次步骤；POISON_OMIT 的 split 臂配对与 server 侧 stamp 半边（§3.2 发现 C）；spawn 不注册的实测理由（§3.3） |
| 4 | 本文 | note + `CONTRACT-P7.md` §6 状态句与 §8 第 (7) 项 |
| 5 | V1 修复轮（8 个提交，各一项） | 评审 land-with-fixes 的八项：①read hook 的负控（§3.1 A′）②poison 正形条目断言角色半边（§3.1）③中性 pack 单一拼法（§2.2）④逐条目臂证明 + 具名例外表（§2.1）⑤spawn 措辞更正（§3.3）⑥`CONTRACT-P7.md` §6 / §8 ⑦CI 车道日志被负控覆盖（§2.1）⑧盲区收窄（§2.2 / §6） |

---

## 1. 撞了什么，放宽成什么

### 1.1 撞的那一行

verify 构建在 `inproc` 下还没有一个用例能 arm，server 的 apply 线程就在 bring-up 里死了：

```
Fatal{PipeRespecifyScope} resource_respecify {slot=12, gen=0}: the descriptor carries a
per-level respecify scope (target=258, level=0), and no path in this phase may set one
```

P5 的这根针写于「载体（`MGPResourceDesc` 的 `HasRespecifiedLevel` + 对）已有、producer 没有」的时候，
所以**任何**带载体的描述符都被当成一个没人宣告的 producer。producer 后来落地了：
`Wire_Escape_ResourceRespecify`（`Client/WireTables.cpp:425-433`）用 `MGPipeSetRespecifiedLevel`
从指针写载体，server codec（`Wire/PipeWireCodec.cpp:1677-1683`）再从载体重建指针。于是 split 下**每一个**
按 level 的 `glTexImage*D` 都带着载体到达，这根针让 verify × split 不可测——门 2 的第二列不是「未满足」，
是「无法测量」。

### 1.2 放宽前 / 放宽后（`MG_Pipe/PipeApply.cpp`，`#if MOBILEGL_PIPE_VERIFY` 内）

| | 条件 | 文本 |
|---|---|---|
| 前 `PinWholeResourceRespecifyScope` | 描述符带 per-level scope（`!MGPipeRespecifyIsWholeResource(desc)`）即 Fatal | `Fatal{PipeRespecifyScope} %s {slot=%u, gen=%u}: the descriptor carries a per-level respecify scope (target=%u, level=%u), and no path in this phase may set one` |
| 后 `PinRespecifyScopeCoversItsDeclaration` | 描述符带 per-level scope，**且**本次调用的 `MGPRespecifiedLevel*` 为空或不等于宣告的 `(uploadTarget, level)` 才 Fatal | `Fatal{PipeRespecifyScope} %s {slot=%u, gen=%u}: the descriptor declares a per-level respecify scope (target=%u, level=%u) and this call covers %s(target=%u, level=%u), so a per-level producer is not covering the range it declares` |

### 1.3 为什么不变量还在

这根针从来不在乎载体有没有置位；它在乎的是**作用域的两半能不能不一致**——原注释自己说「最先出错的会是两者之一
单独移动」。producer 落地后这个危险不是消失了，而是**活了**，而且是静默的：applier 按**指针**丢弃 pending
upload，所以描述符宣告 `(target, level)` 而调用递空指针，会走整资源臂吃掉其它每一级的 texel；递一个不同的对，
会擦错键、留下 client 已经不再欠的那一个。两者在屏幕上都只是一帧后某一级缺像素。放宽后的针守的正是这件事：
**载体置位 ⇒ 调用必须递指针，且指针恰为宣告的对**。

**单向是刻意的。** 整资源描述符对指针什么也不说：`MG_Impl/Pipe/TextureEmit.h` 构造值初始化的描述符（载体
清空，`MGPipeClearRespecifiedLevel` 自己称之为「安全默认」），只在尾随指针里陈述作用域——这是 monolith 形状，
合法、承重、本包不动。反向也查会把树里每个 monolith `glTexImage*D` 染红，断言的是约定而不是不变量。

**G1 不受影响**：两个函数与唯一调用点都在 `#if MOBILEGL_PIPE_VERIFY` 内（非 verify 分支是空函数体，签名多
一个参数），pull 构建 `.text` 与符号表逐字相同（§5）。

### 1.4 正反单测（`MG_Test/Pipe/ResourceEmitTest.cpp`）

- **正** `APerLevelRespecifyCoveringItsDeclaredRangeKeepsTheOtherLevelsTexels`：两级各有 pending texel，按 level
  respecify level 1（载体与指针一致）→ 接受；level 0 的 pending 活着，level 1 的被清；日志无
  `PipeRespecifyScope`。检查的是这根针挡在前面的那件事本身，而不是断言它没响。
- **反** `APerLevelRespecifyThatDoesNotCoverItsDeclaredRangeIsRefusedByName`：宣告 `(258, 1)`，三种不覆盖
  （空指针 / 错 level / 错 upload target）各在子进程里 `SIGABRT`，日志含
  `Fatal{PipeRespecifyScope} resource_respecify {slot=13, gen=2}`、宣告范围 `(target=258, level=1)`，与各自的
  `covers NOTHING (target=0, level=0)` / `covers (target=258, level=0)` / `covers (target=514, level=1)`。
  非 verify 构建按名 skip（针被编译掉了，不能期待一个不会发生的死亡）。

`unit` 2429 → **2431**（+2），100 %。

---

## 2. `integration-verify-split` 车道

### 2.1 形状

- 标签 `integration-verify-split`；只在 `MOBILEGL_BUILD_DISAGGREGATED AND MOBILEGL_PIPE_VERIFY` 下注册，所以
  `--no-tests=error` 会让丢了任一选项的构建变红，而不是报告一次什么都没跑的绿。
- 环境 = `MOBILEGL_TRANSPORT=inproc` + `MOBILEGL_IPC_SERVER_PATH` + `MOBILEGL_PIPE_VERIFY=1` +
  `MOBILEGL_ITEST_REQUIRE_GPU=1`，GLES 接 `MGL_ITEST_COMMON_ENV`，Magma 接 `MGL_ITEST_VULKAN_ENV`（ICD 钉 +
  三个 lavapipe iterationRP 修正，与 monolith `DirectVulkan.` 臂同一驱动）。
- 用例集 = monolith verify 车道的用例集减三样：(1) `MGL_ITEST_MONOLITH_MECHANISM_FILTER`（机制预言不能继承
  传输）；(2) `DualBlockScenario.*`（它的绿需要 `ROLE_SPLIT_STATE=1`，那会把本车道的主题换成另一个实验）；
  (3) 仅 DirectGLES 半边的四条 Magma decline 用例（§2.3 发现 B）。GLES 的 texture-view 用例照 monolith
  verify 车道的拼法带 `MOBILEGL_ESPRYT_ENABLE_TEXTURE_VIEW=1` 另注册一次。
- 手工注册（机制过滤器把它们一起拿掉了）：每后端 `VerifySplitArming.`（arming 断言，私有日志）、
  `VerifySplitCorrupted.` / `VerifySplitReadCorrupted.` / `VerifySplitPoisonOmitted.`（§3）、
  `VerifySplit.PoisonOmissionScenario.WithoutOmissionCompletes`（负控 B 的 CI 靶子）。
- **不设 `MGITEST_SPLIT_LANE=1`**：它武装 ScenarioFixture 析构的「本用例必须推动过 client 编码器序号」，对整二进制
  集合是错的（tier 3 实测 13 红全是 CapsMirror 回答的查询）。取而代之的是**逐条目私有日志**
  （`SplitLogPaths.cmake.in` 的 `MGL_VERIFY_SPLIT_TEST_LISTS`）和 CI 的逐条目臂证明：每个 client 日志都必须有
  ConfigLoader 的 `MOBILEGL_TRANSPORT=inproc` 行；`MGPipe: verify armed` 是进程首次 fill 时才打的，所以从不到达
  动词的进程诚实地没有它。
- **臂证明是逐条目的普查，不是计数（V1 修复轮改）。** 落地形状是「≥1000 个日志里 ≥850 个 armed」，剩下的
  ~155 条无名——车道掉 50 个 arm、多 50 个 skip，这个下限一动不动。现在用 `dualblock-expected-fatals.txt` 那条
  两侧规则（ID-119）：每个 client 日志要么 **armed**，要么属于 **ctest 报 SKIPPED 的条目**（gtest skip = 起了会话
  没到动词，而且 skip 集合随驱动变，点名会变成对驱动上棘轮），要么**按名**列在
  `MG_IntegrationTest/Harness/verify-split-unarmed-expected.txt` 里。两侧都红：列了却 armed、列了却本轮没有日志、
  没列却既没 armed 也没 skip。读表的是 `split_log_paths.py` 新增的 `verify-split-arming` 模式（`test_split_log_paths.py`
  里六条控制用例覆盖四种错法）；车道步骤为此加 `--output-junit`。
- **本机实测（1064 个私有 client 日志）：909 armed、99 unarmed 且 skip、56 unarmed 且按名列出。** 五类，逐类原因
  写在文件里：caps/limits/扩展广告（22，`glGet*` 由 client 的 CapsMirror 回答，不经填充点）；shader / program 对象
  与其错误面（16，编译、链接、specialize、uniform 回读、编译线程池 settle，都不画）；对象状态写了再查回、或调用在
  成为动词之前就被拒（14，含两条 `glCopyImageSubData` level 拒绝与 arena 的 `glGetBufferSubData` 回读）；framebuffer
  完整性查询（2，只有 DirectGLES 半边——DirectVulkan 上这两条 skip，由 skip 规则吸收）；只 fork 的 poison 父进程（2，
  帧画在子进程里、写子进程自己的日志）。

- **CI 上传的是两套日志（V1 修复轮改）。** 两个负控步骤各自用旋钮重跑 12 条 ambient `VerifySplit.` 条目，而
  ambient 条目写的就是 `verify-split-logs/` 下它自己那份私有路径（库以 `"w"` 打开），于是上传步骤收走的那 12 个
  文件是**负控那次红跑**的日志、却顶着车道条目的名字——绿跑的证据已经没了，而且 `if: always()` 让这件事在**失败
  排查时**最严重。现在臂证明之后先 `cp -a` 出 `verify-split-logs-lane/`（连同 JUnit），两套一起上传。

### 2.2 发现 A（真实分歧，已定位；改的是预言，不是规则）

**用例与首个分歧记录。** 第一次整车道运行（`MOBILEGL_PIPE_VERIFY_FATAL=0` 普查）：1068 条里 **220** 条、
**8530** 次读、两后端都有，而且**全车道只有这一个字段**：

```
[mgl-srv-apply/ERROR]: MGPipe: verify read of GetPixelStoreParameters (index 0, 0) differs from the live context
[mgl-srv-apply/FATAL]: MGPipe: Fatal{PipeVerifyDiffer, "GetPixelStoreParameters@ReadPixels", verb=6, where=read}
```

最小复现是 `{DirectGLES,DirectVulkan}.VerifySplitArming.PipeVerifyArmingScenario.Armed`（清、画、1×1
`glReadPixels`）：server apply 线程上第一个 `ReadPixels` 记录的 backend 读 pack 状态即死。

**机制。** ID-49：pack 状态不塑形 wire 上的答案，所以 `MG_Remote/Server/PipeApplier.cpp` 的 `read_pixels` 体
经 `MGPipeApplySetPixelPackState` 往 applier 那份字段里写**中性** pack（`RowLength/Skip* = 0`、`Alignment = 1`），
做 backend 读，再还原；client 再用应用自己的 pack 把 tight 回复 scatter 出去。比对器的预言是前端上下文，它仍持有
应用的 pack（默认 `Alignment = 4`），于是 backend 对 pack 半边的读回答 `{Alignment 1}` 而活上下文说 `{Alignment 4}`
——逐字节探针（`MGPipeDIAG`，已丢弃）确认只差 `Alignment`。monolith 臂上同一个中性窗口是开在前端上下文本身的
（`GL_Texture.cpp` 的 `ScopedNeutralPackState`），所以 monolith verify 车道从来看不见它。**像素是对的**：
同一次 `FATAL=0` 普查（分歧只记数、进程不死）里，1068 条只红了 6 条：两条 `VerifySplitArming.`（红在「日志里
不许有分歧」这条断言上，不在像素上）和 §2.3 的四条（比对器关掉也红）——220 个分歧条目没有一个因像素而红。
走 harness 读回路径的用例（`HeadlessGL.cpp:750` 先 `glPixelStorei(GL_PACK_ALIGNMENT, 1)`）根本不分歧：应用值
恰好等于中性值；分歧的是直接用默认 pack 调 `glReadPixels` 的那些。

**改了什么（`MG_Impl/Pipe/PipeFill.cpp` 的 `MGPipeVerifyReadHook`，`#if MOBILEGL_PIPE_VERIFY` 且
`#if MOBILEGL_BUILD_DISAGGREGATED`）。** 只在这个窗口里换预言：server stamp 之下、`ReadPixels` 动词上，pack 半边
的期望值是 applier 读用的中性 pack；unpack 半边和其它每个字段仍以活上下文为预言。整字段比较失败后，把 scratch
里的 pack 半边换成中性值**再比一次整字段**——pack 既不是应用的值、也不恰为中性值的 server 读，仍是分歧、仍 Fatal。
中性常量**只有一种拼法**：`MG_Pipe/MGPipeTypes.h` 的 `MGPipeNeutralReadPixelsPack()`，applier 的 `read_pixels`
与这里的预言是同一个函数的两个调用点。它一开始是逐字段重打的第二份（为了不从 `MG_Impl` 伸手进 `MG_Remote/Server`），
但 `MGPPixelPackState` 的属主本来就是 `MG_Pipe`、两边本来就 include 它，而两份拼法分叉的代价是**一次假分歧**而不是
一次编译错误——失败方向是哑的，所以改成一处。

**放弃了什么，写明——盲区比第一稿说的窄（V1 修复轮更正）。** 第一稿写的是「在 server 的 `ReadPixels` 上，比对器
看不见一个推送了**错误** pack 状态的 client」。不对：`PipeApplier.cpp` 的 `read_pixels` 在写中性值**之前**先经
**accessor** 把 `savedPack` 读出来（`gPipeInputs.GetPixelStoreParameters(false)` → `MGP_INPUT_VERIFY_READ`），
所以 hook 在那一次就已经把**推送的 pack 对着活上下文的 pack**比过一遍了：

- 推送值 == 活值（正确推送）→ 第一次比较相等，直接返回；
- 推送值 ≠ 活值且 ≠ 中性值（错误推送）→ 第一次比较不等，中性窗口那次比较也不等 → **仍然是分歧、仍然 Fatal**；
- 只有**恰好等于中性 pack 的错误推送**是看不见的（推送值 ≠ 活值但 == 中性值：中性窗口把它认成「applier 自己写的」）。

盲区因此是**一个点**，不是整个字段：`{SwapBytes=0, LSBFirst=0, RowLength=0, ImageHeight=0, Skip*=0, Alignment=1}`
这一个值，且只在应用自己的 pack 不等于它的时候。消费真实 pack 的 client 侧 scatter 另由 readback 矩阵用例按字节
覆盖，不按字段。

### 2.3 发现 B（与 verify 无关，按名减掉）

DirectGLES 半边首轮另有 4 红，全是 P7 wave 2-B 的 Magma wire 臂 decline 契约用例，**比对器关掉也红**（同一构建、
同一环境、`MOBILEGL_PIPE_VERIFY=0` 实测）：

| 用例 | 断言 | 实得 |
|---|---|---|
| `F1WireScenario.GenerateMipmapDepthStencilDeclinesAndKeepsTheSession` | decline 不记 GL 错 | `0x0502` |
| `F1WireScenario.MultisampleBlitOntoAMultisampleDestinationDeclinesAndKeepsTheSession` | decline 记 `0x0502` | 0 |
| `F1WireScenario.MultisampleBlitFromASingleSampleSourceDeclinesAndKeepsTheSession` | 同上 | 0 |
| `F1WireScenario.CopyImageInPlaceOverlapDeclinesAndLeavesTheLevelAlone` | level 0 不动 | 被写（`(4,2)` 通道 0 = 255） |

它们在每条 monolith 臂上 skip，唯一注册它们的是 DirectVulkan 的 split 家族；Espryt 根本不 decline 这些形状。
**之前没有任何 DirectGLES split 车道跑过它们**——这是「DirectGLES 整二进制上 split」这个集合的发现，不是 verify
的。按名从 DirectGLES 半边减掉（`MGL_ITEST_VERIFY_SPLIT_GLES_MAGMA_DECLINE_FILTER`，原因写在 CMake 里），
DirectVulkan 半边保留且绿。

### 2.4 发现 D（monolith verify 在 split 构建上的旧伤，顺手修掉）

`PoisonOmissionScenario` 把 `LibraryLogPath()` + `.poison-child` 交给子进程当它的 `MOBILEGL_LOG_FILE_PATH`；
`LibraryLogPath()` 在 disaggregated 构建里已经是 `<base>.client.log`，子进程的库再套一次角色规则，写到
`<base>.client.client.log.poison-child`，父进程读 `<base>.client.log.poison-child` 读到空——abort 明明按设计发生
了，报的却是「child aborted, but not with Fatal{UnmigratedPipeInput…}」。今天没有 CI 作业同时开两个选项，所以
只有 `build-split` + verify 才看得见（monolith verify 车道 2/1136 红）。改为把**基名**交给子进程，父进程读两个
角色半边（`ReadChildLog`：split 下 poison 可以在 seam 的任一侧触发）。

### 2.5 车道计数（每臂，前 → 后）

| 车道 | 臂 | 基线 `3c2867d3` | 本包之后 | 结果 |
|---|---|---|---|---|
| `integration-verify` | monolith，DirectGLES / DirectVulkan | 568 / 568 = 1136（本配置下 2 红，§2.4） | 569 / 569 = **1138** | 1138/1138，273 skip |
| `integration-verify-split` | inproc，DirectGLES / DirectVulkan | 0 / 0（不存在） | 534 / 538 = **1072** | 1072/1072，117 skip（GLES 60、Magma 57） |

DirectGLES 534 = 530 环境 + Arming + Corrupted + ReadCorrupted + PoisonOmitted；DirectVulkan 538 = 534 + 4；
两者差 4 = §2.3。（V1 修复轮把 `VerifySplitReadCorrupted.` 加进来，每后端 +1；split 车道 1070 → **1072**，monolith verify 车道 1136 → **1138**——新 case 同样被 ambient `Verify.` 注册发现，在那里因为没有 CORRUPT 旋钮而 skip，与 `CorruptedFieldIsReported` 同形。）

---

## 3. 两条负控在 split 臂上各红一次（规则 J）

### 3.1 登记与红行

每后端两条正形条目（控制**报告**时绿，控制**停止**到达 split 臂的那天红）+ CI 两个「ctest 必须失败」步骤。
下面是 CI 形状的红行，落地时在本机执行：

**A（G4）** `MOBILEGL_PIPE_VERIFY_CORRUPT=GetRenderStateParameters` 导出到作业环境，打
`(DirectGLES|DirectVulkan)\.VerifySplit\..*ClearThenReadPixels`：**10/10 红**（`ctest rc=8`），逐条目 client 日志：

```
8 × MGPipe: Fatal{PipeVerifyDiffer, "GetRenderStateParameters@Clear", verb=1, where=entry}
2 × MGPipe: Fatal{PipeVerifyDiffer, "GetRenderStateParameters@DrawArrays", verb=1, where=entry}
```

**A′（G4，读臂；V1 修复轮补）** 入口比对跑在 **client 线程的动词边界**上，报 `where=entry`；**compare-at-read
hook 跑在 server 的 apply 线程上**，是这条臂每次 backend 读唯一运行的比对器，而在本轮之前 `VERIFY_CORRUPT`
**根本到不了它**——一个停止比对的 hook 和一个没东西可报的 hook 长得一模一样。修复：hook 在
`CopyField(g_readScratch, …)` 之后按旋钮扰动**预言**，并在 ID-49 中性 pack 覆写之后**再扰动一次**（覆写是整
个 pack 半边的，连扰动一起抹掉——`CorruptStorage` 扰的正是数组的第 0 个元素，也就是 pack 半边）。新条目
`{DirectGLES,DirectVulkan}.VerifySplitReadCorrupted.`（`MOBILEGL_PIPE_VERIFY_CORRUPT=GetPixelStoreParameters`、
`FATAL=0`，因为该字段在 `kReadback` 填充掩码里、client 的入口比对会先死），用例只读**server 半边**：

```
[mgl-srv-apply/FATAL]: MGPipe: Fatal{PipeVerifyDiffer, "GetPixelStoreParameters@ReadPixels", verb=6, where=read}
```

本机实测：**2/2 绿**，server 半边 DirectGLES 3 行 / DirectVulkan 2 行。**「≥ 2 行」是断言的一部分（修复轮 2）**：
server 在 ReadPixels 窗口内读这个字段**两次**——`PipeApplier::read_pixels` 装中性 pack **之前**先经 accessor 保存应用的
pack，backend 的 `ReadPixels` 在装好**之后**再读——两次都落在 `ServerReadsInsideTheNeutralPackWindow` 里。hook 的第一处
扰动只能把前者变红（过了首次比对之后，应用 pack 对中性预言本来就不等）；覆写之后再扰一次的那处只能把后者变红（那里
存的值**就是**中性 pack）。只接受一行的断言，两处扰动任留一处都满足，所以哪一处都证伪不了；用例与 CI 步现在都要求
server 半边 `where=read` **≥ 2 行**。反证（只去掉其中一处、重编、再跑、再还原）：只去覆写之后那处，两条条目 **2/2 红**、
两个 server 半边各降到 **1 行**；只去第一处，DirectVulkan 降到 **1 行**（红）、DirectGLES 仍 **2 行**（绿——GLES backend 装好之后读两次，3 = 1 + 2），所以第一处的证伪者是 DirectVulkan 那条条目、CI 步两后端都要过；两处都去（修复轮 1 的反证）：**各 0 行**。

这条红线**只能来自读臂**，理由是两个字段：`where=read`（`ReportDivergence` 标的是哪个比对器在说话）加上它落在
**server 角色的文件**里（`Log.cpp` 按写入线程的角色选文件，入口比对跑在 client 线程）。**不是**因为 client 半边空——
修复轮 1 在这里写的「入口比对在这个字段上不报」是错的：入口比对**确实报**它（`kReadback` 的填充掩码带这个字段，
client 的 summary 行自己就写着 DirectGLES `4 divergence(s)` / DirectVulkan `3`）。client 半边当时 0 行，是因为文件在
**退出时被截断**（修复轮 2 修掉的一个既有 bug）：`MobileGL::Destroy`（`Init.cpp` 的 `DestroyImpl`）调 `Debug::Close()`
关掉 sink，之后静态 `~VerifyState` 才写 summary，`Log.cpp` 的 `WriteToFile` 见 sink 为空就重跑 `InitFile()`、以 `"w"`
重开 client 文件——于是每个 FATAL=0 运行的 client 半边只剩 summary 这一行（121 B；monolith 臂的 `VerifyCorrupted.`
日志同样被抹）。修法在 verify 专属代码里（`Log.cpp` 在 pull 构建里，G1 不许动）：`MGPipeVerifyFlushSummary()` 由
`DestroyImpl` 在 `Close()` 之前调用（`#if MOBILEGL_PIPE_VERIFY`），静态析构随后无事可做。修前后同一条目
`pipe-verify-split-read-corrupt-DirectGLES.client.log`：**121 B / 1 行 → 7461 B / 85 行**
（DirectVulkan：121 B / 1 行 → 7386 B / 85 行），其中 `where=entry` 各 @GLES_ENTRY@ / @VK_ENTRY@ 行，
summary 行仍在、且现在在文件**末尾**而不是独占一文件。

**B（G5）** `MOBILEGL_PIPE_POISON_OMIT=ReadPixels:GetPixelStoreParameters` 打
`(DirectGLES|DirectVulkan)\.VerifySplit\.PoisonOmissionScenario\.WithoutOmissionCompletes`：**2/2 红**，子进程
server 半边日志：

```
[mgl-srv-apply/FATAL]: MGPipe: Fatal{UnmigratedPipeInput, "GetPixelStoreParameters@ReadPixels"}
```

**正形条目断言的是「哪个角色」，不是「有没有人」（V1 修复轮补）。** `ReadChildLog()` 把子进程的 client 与 server
两个半边连起来，原断言查的是并集——而这一对配对存在的理由恰恰是 **server 自己的 verb stamp 遵守 knob**
（`ServerPoisonOmission`），一个 client 侧的触发会同样满足并集、却证明了相反的事。现在 `kOmittedPairs` 每对带
`FiresOnTheServer`，split 那一对额外断言 Fatal 出现在**子进程的 server 半边**里。实测两后端：client 半边
`Fatal{UnmigratedPipeInput` **0 行**，server 半边**各 1 行**——并集断言此前确实分辨不出来。monolith 那一对只有一个
角色，仍查并集。

正形条目 `VerifySplitCorrupted.` / `VerifySplitReadCorrupted.` / `VerifySplitPoisonOmitted.` 两后端 6/6 绿。

### 3.2 发现 C：monolith 的 POISON_OMIT 配对在 split 臂上是哑的

首次把 `GenerateMipmap:GetActiveTextureUnit` 搬到 split 臂：两后端子进程的 `glGenerateMipmap` 都**返回了**，
`exited with status 0`——控制是绿的。两层原因，都实测：

1. server 的 verb stamp（`MG_Backend/MGPipe/PipeInputs.cpp` 的 `MGPipeServerStampVerbBoundary`）把本动词类里
   能从已应用记录回答的字段**全部重新盖章**，client 扣下的那一个章被盖回来了；
2. 更根本：split 的 `generate_mipmap` 自 P5c hd 起按动词自带的句柄（`MGPipeApplier().VerbMipRes`）解析纹理，
   **根本不读** active unit。扣一个没人读的章，什么也不会发生（加了 (1) 的修正后仍 exit 0，证实了这一点）。

所以 split 臂用自己的配对：`ReadPixels:GetPixelStoreParameters`——server 的 `read_pixels` backend 调用确实经
accessor 在 server 自己的 stamp 下读 pack 半边。为使 knob 在 split 上有牙，server stamp 在 verify 构建里也遵守
它（`ServerPoisonOmission`，自带解析，因为 server 库里没有 MG_Impl；只在 `#if MOBILEGL_PIPE_VERIFY` 内，push
构建的 stamp 仍是 P5d profile 要求的无分支填充）。被注入的扣章按它所模拟的东西报——
`Fatal{UnmigratedPipeInput, "<Field>@<Verb>"}`——而不是下面那条会把锅甩给 wire 的 `RoleViolation`。
`PoisonOmissionScenario` 的 `kOmittedPairs` 同时认两对；序列在 `glGenerateMipmap` 之后加了一次 1×1
`glReadPixels`（monolith 配对仍在 mipmap 处先死；「更早的动词不许触发」逐对检查）。

反证：split 臂上导出 monolith 配对打同两条条目，**2/2 绿**——即哑，与上面一致。

### 3.3 spawn：不注册，原因是仪器本身

比对器是**同地址空间**的预言：两臂是 applier 写的 `gPipeInputs` 与前端 `GLContext`，只有 inproc 把它们放在一个
进程里。实测（`DirectGLES.VerifySplitArming` 换成 `MOBILEGL_TRANSPORT=spawn`）：

```
MGPipe: Fatal{PipeVerifyDiffer, "GetClearColor@Clear", verb=1, where=entry}
```

client 的 entry 比对在第一个动词上就死——fill 把每个 record-supplied 字段留给 applier，而 applier 现在写的是
**另一个进程**的块；server 进程不跑任何比对（server 日志里零行 verify）。spawn verify 车道会是「前一半 bring-up
即红、后一半瞎」。两条负控因此只登记 inproc。

**后一半的机制不是「server 里没有 MG_Impl」（V1 修复轮更正）。** 它在：`MobileGLServer` 是 `ServerEntry.cpp` 的
可执行文件，链的是**整个库**（顶层 `CMakeLists.txt` 的 `target_link_libraries(MobileGLServer PRIVATE
${CMAKE_PROJECT_NAME})`），`PipeFill.cpp` 在内，所以 `MGPipeVerifyReadHook` 在那个进程里**是有的**、每次 backend 读
**也确实被调用**。缺的是它的**预言**：server 镜像的 main 走到 `MG_Backend::InitServerRoleForSpawn()` 就停了，从不调
`MG_State::Init()`，于是 `MG_State::pGLContext` 为空，hook 在 `ctx == nullptr` 那行就返回。结论不变（spawn 上没得比），
但区别是承重的：要补它需要一个**跨进程的比对形状**，不是一次链接改动。

---

## 4. trace 侧：8 个 verify case × DirectVulkan × inproc，一轮，lavapipe

`MobileGLTraceReplay.<case>.DirectVulkan.SPLIT`，作业环境同 `retrace-verify` 的 DirectVulkan 行
（`MOBILEGL_PIPE_VERIFY=1`、`MOBILEGL_MAGMA_R11G11B10F_FALLBACK=1`；`improved-transparency` 另加
`MOBILEGL_MAGMA_DISABLE_BLENDED_DEPTH_WRITE=1`）。`run_trace_case.cmake` 自己断言三件事（arm 行在、无
`Fatal{PipeVerifyDiffer|UnmigratedPipeInput|PipeVerifyBadKnob`、transport 解析为 inproc 且无 `Fatal{`）；分歧计数
另从该条目私有的 client + server 日志数 `PipeVerifyDiffer`。

| case | 结果 | 用时 | SSIM（阈值） | mismatch px | arm 行 | `PipeVerifyDiffer` | `Fatal{` |
|---|---|---|---|---|---|---|---|
| `OpenRA` | PASS | 2.5 s | 1.000000（0.99） | 0 | 1 | **0** | 0 |
| `minecraft-1.21.4-startup` | PASS | 0.7 s | 1.000000（0.99） | 184 | 1 | **0** | 0 |
| `minecraft-1.21.4-main-menu` | PASS | 13.4 s | 0.997009（0.99） | 106838 | 1 | **0** | 0 |
| `minecraft-1.21.11-main-menu` | PASS | 12.5 s | 0.993475（0.99） | 43254 | 1 | **0** | 0 |
| `minecraft-1.17-main-menu-854` | PASS | 5.8 s | 0.999961（0.99） | 137232 | 1 | **0** | 0 |
| `minecraft-1.21.4-in-world` | PASS | 19.0 s | 0.999976（0.99） | 30679 | 1 | **0** | 0 |
| `minecraft-1.21.4-fabric-sodium-in-world` | PASS | 28.5 s | 0.999986（0.99） | 49399 | 1 | **0** | 0 |
| `improved-transparency-minecraft-26.3` | PASS | 336.1 s | 0.999914（0.995） | 3259 | 1 | **0** | 0 |

**8/8，分歧计数 0**，每条的 `run_trace_case.cmake` 都打出 `MGPipe verify: <case> DirectVulkan armed, zero
divergences, zero unmigrated reads` 与 `MGPipe split: <case> DirectVulkan transport=inproc`（头 `da3718c4`）。
同一 `OpenRA.DirectVulkan.SPLIT` 条目再导出 `MOBILEGL_PIPE_VERIFY_CORRUPT=GetRenderStateParameters` 即红
（`ctest rc=8`，`Fatal{PipeVerifyDiffer, "GetRenderStateParameters@Clear", verb=1, where=entry}`）——trace 侧的比对器
在 split 臂上确实在比。

---

## 5. 门

| 门 | 读数 |
|---|---|
| G1 pull 构建 | rc 0；`.text` **0xa52203**；`nm --defined-only` 对 `~/w7/p7-before/pull-syms.txt` **+0 / −0** |
| `fatal_census.py` | rc 0，**79** 个 abort 站点 / 20 文件，0 unmarked |
| `link_ratchet.py --assert-monotone` | `build-split-nv`（同配置、`PIPE_VERIFY=OFF`）：**173**，PASS。`build-split`（`PIPE_VERIFY=ON`）读 176 并 FAIL，多出的 3 个全是 verify-only 引用、基线上同样存在：`MGPipeVerifyReadHook`（`MGP_INPUT_VERIFY_READ` 展开进 backend）与 `Encode/DecodeProgramArtifacts`（`PipeApply.cpp:1330` 的 `#if MOBILEGL_PIPE_VERIFY` 往返绊线）。棘轮基线描述的是出货构建，verify 构建永不出货（D-B5），所以读数取前者 |
| `spawn_lane_parity.py build-split` | rc 0 |
| `^unit$` | 2431/2431（基线 2429，+2 = §1.4） |
| `^integration-split$` | 303/303 |
| `^integration-spawn$` | 219/219 |
| `^integration-tcp$` | 222/222 |
| `^integration-verify$`（monolith） | **1138/1138**（V1 修复轮 +2 skip） |
| `^integration-verify-split$` | **1072/1072** |
| `^integration-magma-split$` / `-spawn$` / `-tcp$` | 93/93 · 72/72 · 74/74 |

以上 lane 均在 `build-split`（verify 构建）上跑，比基线口径更严（每个 backend 读都过比对器的 hook）。

---

## 6. 没做的 / 剩下的

- **§2.3 的四条**：DirectGLES × split 上 Magma decline 契约用例红。要么这些用例在非 Magma 后端上按名 skip，要么
  Espryt 学会 decline 这些形状；归 P3b/P4b 的 Espryt 流（或用例作者），本包只按名减掉并记于此。
- **server `ReadPixels` 上 pack 半边的字段级盲区**（§2.2 末段，V1 修复轮已收窄）：唯一看不见的是**推送值恰好等于
  ID-49 中性 pack 的错误推送**——`read_pixels` 在覆写之前就经 accessor 读过一次 `savedPack`，hook 在那一次已把推送值
  对着活上下文比过，任何其它错误推送都仍然是分歧、仍然 Fatal。若要连这一个点也补上，需要 server 侧把中性窗口显式
  告诉比对器（动 `MG_Remote/Server`，本包分区外）。
- **spawn 上的 verify 需要一个跨进程的比对形状**（§3.3，已记入 `CONTRACT-P7.md` §12 记录债）：hook 在 server 进程里
  是有的、也被调用，缺的是预言（`MG_State::pGLContext` 为空，因为 server 的 main 不调 `MG_State::Init()`）。补它要让
  server 把它读到的字段值随回复带回、由 client 比，或在 server 侧立一个等价的状态镜像；不是注册问题，也不是链接问题。
  门 2 的文本是 `{monolith, split(inproc)}`，不含 spawn。
- CI 作业 `integration-verify-split` 首次在 GitHub runner 上的读数（lavapipe / llvmpipe 版本不同，skip 数与
  arm 计数可能不同；arm 下限 850 留了余量）。
