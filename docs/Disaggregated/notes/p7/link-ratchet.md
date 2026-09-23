# 链接闭包棘轮（P7 wave 0，ID-P7-7）

> 脚本 `scripts/link_ratchet.py`，基线 `scripts/data/link_ratchet_baseline.txt`，
> CI 步骤在 `.github/workflows/test.yml` 的 `build-linux-split` 作业里。
> 被量的那件事见 [`a6-link-experiment.md`](../p6/a6-link-experiment.md)
> 与 [`a6-link-experiment-data.md`](../p6/a6-link-experiment-data.md)。

## 0 这道门归谁

契约 `CONTRACT-P6.md` §12.1 把 184 记成债并写下一句义务：「一道棘轮在 CI 上重算这 184 并断言它只减不增」。
路线图把它写成**「移交 P6.5」——P6.5 行从未接收**：P6.5 落地的是传输栈两轴、归档 v2、发布边界、半关闭与
`AcceptPair`/角色修复，没有一行是这道门。`scripts/ci/` 下也确实没有任何脚本算这个数
（`link_seam_purity.py` 量的是源码文本里的所有权/派发纪律，`p65_link_metrics.py` 量的是帧日志）。

**ID-P7-7 把它归 P7 wave 0**，并定了两条：基线存**符号列表**而不是计数；`--assert-monotone`
只对**新增**符号变红，符号消失只提示重基线。第二条是这道门的全部意义——
裸计数的棘轮在「清掉一个、又加一个」的那天是绿的，而那天正是它该红的那天。

## 1 怎么量

没有模块 target 可链（`SOURCE_FILES` 是一张平的源文件列表，同时喂 `MobileGL` 与 `MobileGL_s`；
建立模块边界本身是 P13 的具名工作），所以 a6 的办法是对**单个 `MobileGL` target 的逐个 object**
施加一个纸面上的分区，然后做集合运算：

```
(SERVER 未定义) − (SERVER 已定义) − (SHARED 已定义)  ∩  (FRONTEND 已定义)
```

这就是一次真链接会打印的那份未定义符号表。脚本照做，**唯一的区别是那张分区表现在写在脚本里**
（`link_ratchet.py` 的 `PARTITION`），一行一条路径前缀、一个角色、一句理由，最长前缀匹配。
它就是构建系统没有的那道模块边界，只不过写下来了。

两条纪律：

- **任何 object 匹配不到 `PARTITION` 的任何一行 = 硬失败**，不是静默忽略。
  树会长，新目录会出现；一个悄悄被漏掉的 object 会让这个数字悄悄停止度量它声称度量的东西。
- **`CMakeCache.txt` 里三个选项不全为 ON = 硬失败**（`--no-require-flags` 可关）。
  选项为 OFF 时 `MG_Remote` 根本不编译，SERVER 集少掉整个传输半边，脚本会给出一个
  很小、很绿、毫无意义的数。

`nm` 带 `-C`，所以集合运算的单位是**解构后的名字**——a6 发表的就是它，人也只读得懂它。
代价写在脚本的 docstring 里：基线树上 FRONTEND 集有 14974 个不同的 mangling 落在 14880 个不同的
解构拼写上，那 94 处重合几乎全是 C1/C2 构造与 D1/D2 析构对（按定义解构成同一串文本）。
**解构棘轮是链接的一个可读的近似，不是链接。** 要在 mangling 上较真的工具是 `symbol_report.py`。

只有 `U` 算引用；弱未定义（`w`）在无人定义时解析为 0，不构成对任何人的义务。
基线树上没有任何 `w` 触及前端定义，所以这条过滤今天不花钱。

## 2 今天的数字

基座 `e8b2c4bd`，`build-split`（Release，`-DMOBILEGL_BUILD_DISAGGREGATED=ON
-DMOBILEGL_BUILD_DISAGGREGATED_INPROC=ON -DMOBILEGL_PIPE_PUSH=ON`），211 个 object：

| | |
|---|---:|
| 分区 | SERVER 52 / FRONTEND 71 / SHARED 88 |
| SERVER 集未定义符号总数 | 1579 |
| 其中 SERVER + SHARED 自己满足不了的 | 593 |
| **其中由 FRONTEND 定义的** | **186** |

分桶（`--bucket`），与 a6 手做的那份逐个对照：

| 桶 | 符号数 | a6 | delta | 含义 |
|---|---:|---:|---:|---|
| `p6-core` | 11 | 6 | +5 | 没有任何后端 object 引用它：传输/applier 自己的账 |
| `p7-magma` | 101 | **101** | **+0** | 只有 `DirectVulkan` 引用：P7 的账 |
| `p3b-p4b-espryt` | 14 | **14** | **+0** | 只有 `DirectGLES` 引用：P3b/P4b 的账 |
| `both-backends` | 60 | **60** | **+0** | 两个后端都引用：两边都迁完才掉 |
| **合计** | **186** | **184** | **+2** | |

**三个后端桶逐一精确复现 a6 手做的 101 / 14 / 60。** 引用方与定义方的分布也逐一对上：
`UniformManager.cpp` 90、`VulkanRenderer.cpp` 60、`DirectGLES/Managers.cpp` 53、`DirectGLES.cpp` 51；
`TextureObject.cpp` 59、`SamplerObject.cpp` 30、`BufferObject.cpp` 18、`FramebufferObject.cpp` 16、
`PipeFill.cpp` 9。a6 的 §3.1 / §3.2 一个数都没差。

### 2.1 +2 的来源：两个符号，都是 P6.5 的

对 a6 发表的 184 逐符号取差集：**新增 2 个，消失 0 个**。

| 新增符号 | 引用方 | 是什么 |
|---|---|---|
| `MG_Remote::Client::ClientSession::StartSpawned()` | `MG_Backend/Init.cpp.o` | P6.5 的 fork-per-session 启动路径 |
| `MG_State::GLState::ProgramArtifactsSchemaFingerprint()` | `MG_Remote/CapsCodec.cpp.o` | P6.5 的归档 v2，caps 里带上 schema 指纹 |

SERVER 集本身也长了：a6 时 45 个 object，现在 52 个（`Transport` +5、`Server` +2，全是 P6.5）。
Magma 各 wave 改了 `DirectVulkan` 的内容但没有改它要的前端符号集合——这一点本身值得记一笔。

### 2.2 a6 自己的四个总数加不齐

a6 §3.2 / §5 发表的四个数 **101 + 14 + 60 + 6 = 181**，而它的头条是 **184**，差 3。
本脚本的四个桶是**严格划分**，恒等于头条。差额落在 `p6-core`：a6 §5 的表只列了它觉得有意思的
6 个，实际「没有任何后端引用」的是 9 个（184 − 175）。今天是 11 个（9 + §2.1 那 2 个）。

对得上的算法是 **9 = 6 − 1 + 4**：a6 §5 列的 6 个里有 1 个（`MGPipeApplierIsUnbarrieredApply()`）
今天已经不在 core 桶（见下）；漏列的 4 个是 `BackendObject_Remote::BackendObject_Remote()`、
`ClientSession::Start(...)`、`ClientSession::Stop()`、`ClientSessionInstance()`——
四个全由 `MG_Backend/Init.cpp.o` 引用，即**后端工厂在挑 remote 后端时调 client 的会话对象**。
a6 §5 的叙述（「server 不该问 client 要答案」）本来就该把它们算进去，只是表里没写。

另外 a6 §5 说 `Server/PipeApplier.cpp` 需要 `MGPipeApplierIsUnbarrieredApply()`。
今天它落在 `p3b-p4b-espryt` 桶里：引用它的只剩 `DirectGLES/DirectGLES.cpp.o`，
`MG_Remote/Server` 已经不引用了。这是 P6.5 之后的真实变化，不是分类差异。

## 3 怎么读这张表

- **`p7-magma` 的 101 是出口门 4 的分母。** 它只会因为 Magma 真的把功能迁到 server 侧而下降。
- **`p6-core` 的 11 不是后端的债**，推给 P7/P3b/P4b 是错的。它的形状很清楚：
  **9 个是 server 侧在问 client 要答案**（`MG_Backend/Init.cpp.o` 5 个、
  `MG_Pipe/PipeApply.cpp` + `Wire/PipeWireCodec.cpp` 2 个、`Server/PipeApplier.cpp` 2 个），
  1 个是需要搬到正确模块的解码助手（`DecodeProgramArchive`），
  1 个是 P6.5 的 schema 指纹（`ProgramArtifactsSchemaFingerprint`）。
- **`both-backends` 的 60 是最硬的一块**：两个后端都要，所以任何一边单独迁完它都不动。

### 3.1 给集成者的一条：`p3b-p4b-espryt` 的 14 个里有 5 个 P3b/P4b 清不掉

基线里带 `# P13` 标注的是**由 `MG_Impl/Pipe/SlotAllocator.cpp.o` 定义的全部 7 个符号**。
规则是机械的、可复核的：要清掉其中任何一个，都得让 slot allocator 不再住在 `MG_Impl/` 下面——
那是一次模块边界搬迁，契约 §12.1 与 a6 §6.4 都把它记在 P13 头上，**任何后端迁移都够不到它**。

这 7 个的落桶是 **`p3b-p4b-espryt` 5 个 + `both-backends` 2 个**：

| 符号 | 桶 |
|---|---|
| `MG_Pipe::MGPipeApplierIsUnbarrieredApply()` | `p3b-p4b-espryt` |
| `MG_Pipe::MGPipeRefuseAllocatorFromApplyThread(char const*)` | `p3b-p4b-espryt` |
| `MG_Pipe::MGPipeRefuseFrontendKeyedRegistryFromApplyThread(char const*)` | `p3b-p4b-espryt` |
| `MG_Pipe::MGPipeSlotAllocator::Acquire(...)` | `p3b-p4b-espryt` |
| `MG_Pipe::MGPipeSlotAllocator::Free(...)` | `p3b-p4b-espryt` |
| `MG_Pipe::MGPipeSlotAllocator::FindByLifetimeId(...) const` | `both-backends` |
| `MG_Pipe::MGPipeSlots()` | `both-backends` |

也就是说：**`p3b-p4b-espryt` 这个桶的地板是 5，不是 0。**
计划正文把「D12 的 14 个符号」写成 Tier 1（0.5 天）在棘轮落地后做——
按今天的数据，其中 5 个不在 P3b/P4b 的能力范围内，那一行的估算应按 9 个重做。
标注只是给人看的，解析器忽略它，门的行为不受影响。

## 4 red-once（R-16）

在 `~/w7/p7-ratchet`（`e8b2c4bd`，`build-split`）里往一个**被分类为 SERVER 的** object
加一次前端触及，只重编那一个 object，跑门，然后还原。

**注意选哪个符号。** 计划书里举的例子 `SamplerObject s(0); s.GetMinFilter();` **不会让门变红**——
`GetMinFilter()` 已经在基线里（两个后端都在调它）。棘轮只对**新增**符号红，
所以负控必须挑一个今天不在那 186 个里的符号。挑的是
`SamplerObject::GetBorderColorForm() const`（在 `SamplerObject.cpp.o` 里是 `T`，外部调用，
不会被内联掉；今天没有任何 server object 引用它）。

加在 `MG_Backend/DirectGLES/Utils.cpp`（SERVER，`espryt` 族）：

```cpp
    unsigned P7RatchetRedOnceProbe() {
        MG_State::GLState::SamplerObject s(0);
        return static_cast<unsigned>(s.GetBorderColorForm());
    }
```

```
$ ninja -C build-split CMakeFiles/MobileGL.dir/MobileGL/MG_Backend/DirectGLES/Utils.cpp.o
$ python3 scripts/link_ratchet.py --build-dir build-split \
      --baseline scripts/data/link_ratchet_baseline.txt --assert-monotone
```

**实测红线，逐字**：

```
link-ratchet:   OF WHICH defined by FRONTEND: 187  (a6 measured 184)
link-ratchet: baseline scripts/data/link_ratchet_baseline.txt: 186 symbol(s), 7 annotated
link-ratchet: 1 symbol(s) are NEW frontend reach: a server object now needs a frontend definition the baseline did not record. This is what the gate exists for - either stop reaching, or land the reach with the reason in the commit and re-baseline deliberately:
link-ratchet:   NEW: MobileGL::MG_State::GLState::SamplerObject::GetBorderColorForm() const
link-ratchet:        defined by:  MobileGL/MG_State/GLState/SamplerState/SamplerObject.cpp.o
link-ratchet:        referred by: MobileGL/MG_Backend/DirectGLES/Utils.cpp.o
link-ratchet: FAIL --assert-monotone: 1 new frontend symbol(s) (named above)
$ echo $?
1
```

三项计数同步动了：1579 → 1580 未定义、593 → 594 未满足、186 → 187 前端定义。
还原并重编那一个 object 之后门重新为绿（`ratchet: unchanged at 186 symbol(s)`，退出 0）。

**这条红线的价值在第三行和第四行**：它不是「数字变了」，它点名了**哪个符号**、
**谁定义的**、**谁在够它**。红的那一刻就知道该去看哪个文件。

## 5 日常用法

```bash
# 看今天的数和分桶
python3 scripts/link_ratchet.py --build-dir build-split --bucket

# 门（CI 就是这一行）
python3 scripts/link_ratchet.py --build-dir build-split \
    --baseline scripts/data/link_ratchet_baseline.txt --assert-monotone \
    --json link-ratchet.json

# 清掉符号之后，在同一个提交里重基线（标注会被带过去）
python3 scripts/link_ratchet.py --build-dir build-split \
    --baseline scripts/data/link_ratchet_baseline.txt \
    --write-baseline scripts/data/link_ratchet_baseline.txt

# 不需要构建
python3 scripts/link_ratchet.py --self-test
```

**符号消失不是失败**，脚本会把它们逐个列出来并要求在同一个提交里重基线。
**符号新增是失败**，脚本会对每一个打印它的定义方 object 与全部引用方 object，
所以红出来的那一刻就知道是谁在够谁。

## 6 没有验证的（读之前先读这一条）

**基线是在参考构建 `~/w7/pipe/build-split` 上生成的，不是在 CI 的机器上。**
两边都是 clang + **libstdc++**（CI 装了 `libc++-20-dev` 但 `CMakeLists.txt` 与工作流都没有传
`-stdlib=libc++`，所以 Linux 上落的是 libstdc++；基线里的 `std::__cxx11::basic_string` 拼写
就是这么来的），配置三选项与 `CMAKE_BUILD_TYPE=Release` 也一致——**但 clang 版本不同**
（参考构建是 Arch 的 clang，CI 是 `clang++-20`）。

内联决策随版本变：某个方法在一边被内联、在另一边留成外部调用，就会让那个符号在 reach 集里
出现或消失。**所以这道门在 CI 上的第一次运行有可能是红的，且那种红不是代码变坏。**
处理办法是明确的，不要绕过门：看第一次运行上传的 `link-ratchet.json` artifact，
逐个核对 `new_since_baseline` / `cleared_since_baseline`，确认它们全都是内联差异
（同一个类、同一族方法、没有新的引用方 object），然后用一个单独的提交重基线并在提交信息里
写明是工具链差异。**如果里面混着一个新的引用方 object，那就不是内联差异。**

其余：

- Android NDK 构建没试过，而且**会**不一样：NDK 用 libc++，解构拼写变成 `std::__1::…`，
  整份基线都会算作新增。这道门今天只对 Linux 主机构建有意义。
- `MobileGL_s`（同源的静态库 target）没量；`--target` 可以指过去，但没跑过。
- 解构名重合（§1 那 94 处 C1/C2）今天不影响结果，但脚本没有为此加断言——
  要加的话应当是「reach 集里的每个解构名在 FRONTEND 侧只对应一个 mangling」。
- `check_flags` 的「选项为 OFF」分支是手工用一个假的 `CMakeCache.txt` 验的，
  不在 `--self-test` 覆盖范围内。
