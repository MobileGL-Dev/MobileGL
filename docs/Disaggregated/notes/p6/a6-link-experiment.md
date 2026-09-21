# a6 第 5 项：server 能否不链 `MG_Impl`——实验结果

> 头 `b3b9c2ee`。`P6-SPAWN-PLAN.md` §5 的 a6 第 5 项、`P6-CONTRACT-DRAFT.md` §3.3 都把这条挂着；
> `MG_Backend/MGPipe/PipeInputs.h` 与 `P5F-WIRE-COMPLETENESS.md` §1.4 **断言**它成立。
> 三个阶段没人试过。这次试了。
>
> 原始数据（184 个符号逐个按定义文件分组）见 [`a6-link-experiment-data.md`](a6-link-experiment-data.md)。

## 结论

**不能。** 今天没有任何一种配置能让 server 镜像不链 `MG_State` / `MG_Impl` / `MG_Remote/Client`。

而且这个问题**在今天根本不是一个链接行能回答的问题**，这是第一个发现。

---

## 1 结构性发现：构建系统里一道缝都没有

`CMakeLists.txt` 里 `SOURCE_FILES` 是**一张平的源文件列表**（211 个 `.cpp`），
`add_library(MobileGL SHARED ${SOURCE_FILES})`（`:695`）与
`add_library(MobileGL_s STATIC ${SOURCE_FILES})`（`:762`）都从它构建。

`MG_Impl` / `MG_State` / `MG_Backend` / `MG_Pipe` / `MG_Remote` 之间**没有任何 target 边界**——
没有 object library、没有 static lib、没有 interface target。

唯一叫 `MobileGLServer` 的 target（`:926`）是 P0 spike 桩：Android-only、默认 OFF、
只编 `tools/spikes/server_stub/main.cpp`，**一个 MobileGL 库都不链**。
`mobilegl_server_main` 这个符号在整棵树上**不存在**，只存在于 `ARCHITECTURE.md:488-496` 的描述里。

> 所以"改链接行试一次"这个做法本身不成立：没有可分的 target。
> `c6` 必须把"建立模块 target 边界"当成一件**有成本的具名工作**，而不是 `sm` 包里顺手的一行。

## 2 方法（保持 a6 只读）

不加 target、不改一行源码。构建 `MobileGL` 单个 target，然后对**逐个 object 文件**做符号闭包：

```
SERVER   集 = MG_Backend/** + MG_Pipe/** + MG_Remote/{Transport,Protocol,Wire,Server}/** + CapsCodec.o   （45 个 object）
FRONTEND 集 = MG_Impl/** + MG_State/** + MG_Remote/Client/**                                              （71 个 object）
SHARED   集 = MG_Util/** + Init/GlobalObjects/ConfigLoader                                                （87 个 object）
```

取 SERVER 集的未定义符号，减去 SERVER 自己定义的，减去 SHARED 定义的，
与 FRONTEND 定义的取交集——**这就是一次真链接会报出来的那份未定义符号表**。

构建：`clang 22.1.6`、Release、`DISAGGREGATED=ON`、`DISAGGREGATED_INPROC=ON`、`PIPE_PUSH=ON`。

## 3 数字

| | |
|---|---:|
| SERVER 集未定义符号总数 | 1531 |
| 其中 SERVER + SHARED 自己满足不了的 | 552 |
| **其中由 FRONTEND 定义的** | **184** |

184 个里：

| 类别 | 数量 |
|---|---:|
| **真函数调用** | **177** |
| 析构函数 | 4 |
| typeinfo | 2 |
| vtable | 1 |

**耦合是行为性的，不是类型性的。** 不是"server 持有一个 `SharedPtr<ITextureObject>` 所以需要 vtable"，
是 server 在**调用前端对象的方法**。

### 3.1 按定义方

| 定义模块 | 符号数 |
|---|---:|
| `MG_State/GLState` | **157** |
| `MG_Impl/Pipe`（`PipeFill`、`SlotAllocator`） | 16 |
| `MG_Remote/Client` | 9 |
| `MG_Impl/GLImpl` | 2 |

前五个文件：`TextureObject.cpp` 59、`SamplerObject.cpp` 30、`BufferObject.cpp` 18、
`FramebufferObject.cpp` 16、`PipeFill.cpp` 9。

### 3.2 按引用方——**债主是 P7**

| 引用方 | 符号引用数 |
|---|---:|
| `DirectVulkan`（Magma） | **227** |
| `DirectGLES`（Espryt） | 111 |
| `MG_Remote/{Wire,Server}` + `MG_Pipe` | 6 |

去重后按后端归属：

| | 符号数 |
|---|---:|
| **只有 Magma 要** | **101** |
| 两个后端都要 | 60 |
| 只有 Espryt 要 | 14 |

单个最大引用方是 `DirectVulkan/Renderer/UniformManager.cpp`（90），其次
`VulkanRenderer.cpp`（60）、`DirectGLES/Managers.cpp`（53）、`DirectGLES/DirectGLES.cpp`（51）。

**这份分布精确地落在路线图已经说没做完的两个阶段上**：P7（Magma 全量迁移）和 P3b/P4b（Espryt 深化）。
链接耦合不是一笔新债,它是那两笔旧债在链接器上的影子。

## 4 这和 "P5c/P5f 已经把跨角色读归零" 矛盾吗？不矛盾

两句话都成立，因为它们量的不是一回事：

- P5c / P5f 关掉的是**运行时**跨角色内存访问：`rsp` 逐帧为 0、`BARRIER_PULLED` 分类为 0。
- 这次量的是**链接时**符号引用。

但**这也不只是"死臂的链接残留"**。取样核实过一个站点：

`DirectVulkan/Renderer/UniformManager.cpp:1039` 一带是

```cpp
const auto& samplerOverride = MGB_CTX->GetTextureUnitObject(unit).GetSamplerObject();
const auto* effectiveSampler = samplerOverride ? samplerOverride.get()
                                               : texture->GetSamplerObject().get();
…effectiveSampler->GetMagFilter() == SamplerFilterMode::Linear…
```

`MGB_CTX` 在 `PIPE_PUSH` 下是 `&gPipeInputs`，所以这是 `PipeInputs::GetTextureUnitObject`——
而它在 `MG_Pipe/FieldOwnership.def:33` 被分类为 **FATAL**：

> `X(GetTextureUnitObject, FATAL, "-", "server reads sampler/view records or verb texture handles; never a client TextureUnit")`

也就是说：**split 下走到这里是一条具名 Fatal，不是一次错误的读**。P5f 的归零是真的。
但"具名拒绝"意味着**这条功能没有迁移**，代码还在、符号还在、链接还需要它。

所以准确的说法是：

> 184 个符号是 P5f 那 25 条 FATAL 分类 + 未迁移的对象类字段在链接层面的残留。
> 运行时是干净的（要么走记录臂，要么 Fatal）；链接时不是，而且**链接时是 server 镜像能不能存在的那个判据**。

## 5 传输层自己的 6 个——单独拎出来

其余 178 个是后端的债，这 6 个不是，它们在 `MG_Remote` / `MG_Pipe` 内部：

| 引用方 | 需要的符号 | 说明 |
|---|---|---|
| `Server/PipeApplier.cpp` | `Client::ClientSession::NoteApplyThreadEnteredApplier()` / `NoteApplyThreadLeftApplier()` | **server 侧 applier 在调 client 的会话对象**。inproc 下同进程所以成立；spawn 的 server 进程里根本没有 `ClientSession` |
| `Server/PipeApplier.cpp` | `MG_Pipe::MGPipeApplierIsUnbarrieredApply()` | 定义在 `MG_Impl/Pipe` |
| `Wire/PipeWireCodec.cpp` | `MG_State::GLState::DecodeProgramArchive(...)` | 解码器需要一个住在 `MG_State` 里的解码助手 |
| `Wire/PipeWireCodec.cpp`、`MG_Pipe/PipeApply.cpp` | `Client::AdoptTierIsEmulate()` | 采纳档位判据住在 client 侧 |
| `MG_Pipe/PipeApply.cpp` | `Client::ClientSession::Active()` | 同上 |

**这 6 个是 P6 自己的账**，不能推给 P7/P3b/P4b。它们数量小、形状清楚，应当在 `sm` 或 `lk` 里解决：
前三个是"把函数搬到正确的模块"，后三个是"server 不该问 client 要答案"。

## 6 给 `c6` 的结论

1. **契约不能再写 "server 不链 `MG_Impl`"**。`PipeInputs.h` 与 `P5F-WIRE-COMPLETENESS.md` §1.4
   的断言按今天的树是**假的**，`c6` 要按 `CONTRACT` §3.3 自己写的规矩"做不到就记账"把它记成债。
2. **债的归属是 P7（101）+ P3b/P4b（14）+ 两者共有（60）+ P6 自己（6）**，不是一笔含糊的"以后再说"。
3. **P6 不需要为此阻塞**。P6 的 server 是 fork 出来的同一个二进制，本来就链着整份库——
   `ARCHITECTURE.md:496` 的"一份共享库、两个角色"正是这个意思。
   所以 P6 可以照常推进，但 `c6` 必须明确写下：**P6 的 server 镜像 = 整份 `libMobileGL.so`**，
   而"一个不含前端的 server 镜像"是 P7 / P3b/P4b 完成之后才谈得上的东西。
4. **`c6` 要新增一条具名工作**：建立模块 target 边界（object library 或 static lib）。
   没有它，这个数字永远只能靠 `nm` 事后量，不能变成一道会变红的门。
   建议形状：`lk` 包顺带把 `MG_Remote/Transport` 切成自己的 target（它本来就是纯度门 A/B/C 的对象），
   其余模块边界记成 P13 的账。
5. **一道可以现在就立的门**：把本文的 184 记成棘轮，CI 上重算并断言"只减不增"。
   它能因 P7/P3b/P4b 的真实进展而下降，也能因新写的后端代码又去读前端对象而变红——
   这正是这个项目要的那种门。

## 7 附带发现：ABI 指纹的 build stamp 会**静默变成空串**

这是实验过程中撞出来的，不在 a6 的清单上，但它直接打在 `hs` 包上，而且是个真洞。

**机制**。`CMakeLists.txt:188-195`：

```cmake
execute_process(
    COMMAND git rev-parse HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_COMMIT_HASH_FULL
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
string(SUBSTRING "${GIT_COMMIT_HASH_FULL}" 0 7 GIT_COMMIT_HASH_SHORT)
```

**没有 `RESULT_VARIABLE`，没有 `ERROR_QUIET` 之外的任何处理，没有兜底。**
`git` 失败时 `GIT_COMMIT_HASH_FULL` 是空的，`SUBSTRING` 给出空串，配置继续，构建成功。

本次实测：配置阶段打印 `fatal: not a git repository: (null)`，
生成的 `generated/MGGitHash.h` 是

```c
#define GIT_COMMIT_HASH_SHORT ""
#define GIT_COMMIT_HASH_FULL  ""
```

**为什么会失败**：这棵树是 git worktree，`.git` 是一个文件，内容是
`gitdir: C:/Users/geekerwan/.../FoldCraftLauncher/.git/modules/MobileGL/worktrees/MobileGL-disagg`
——一条 **Windows 路径**。WSL 的 git 解析不了它（要 `/mnt/c/...`）。
本项目正是用 worktree 并行推进各阶段的，所以"从 WSL 构建某个 worktree"这件事会**稳定复现**。

**为什么要紧**。`AbiFingerprintInputs::BuildStamp`（`SessionRings.h:563-565`）就是它，注释写明它的职责：

> *"GIT_COMMIT_HASH_SHORT: two builds of the same sizes can still disagree about a FIELD ORDER,
> which no sizeof can see. nullptr and "" are distinct inputs and neither equals a real stamp."*

设计时**想过**空值——但只想到"空串是一个与真 stamp 不同的输入"。
没想到的是**构建会静默地把所有构建的 stamp 都变成同一个空串**。
于是：**两份来自不同 commit、都在 git 取不到版本的环境里构建的库，ABI 指纹相同**，
而指纹里唯一能看见字段重排的那一项就此失效。

> **更正（`lk` 跑基线测试时发现）。** 本节原先写的是"没有任何日志、任何门会说一声"。**这句是错的，**
> 门是存在的：`MG_Test/Wire/SessionHandshakeTest.cpp:140` 断言 `inputs.BuildStamp[0] != '\0'`，
> `:180` 断言扰动 `BuildStamp = ""` 必须改变指纹。在这个配置下**两条都实测为红**：
> `Expected: (inputs.BuildStamp[0]) != ('\0'), actual: '\0' vs '\0'`。
> 所以项目预见到了这个情形，本节的发现因此更强而不是更弱——**不是"没人会注意到"，
> 是"会注意到它的门已经存在，而且现在就是红的"**。真正静默的是**构建期**：CMake 无声地产出空串，
> 红只在跑测试时才出现。`hs` 要补的是构建期的具名失败，不是一道新门。

这不是一个奇异场景。会命中它的至少有：worktree 跨 WSL 构建（已实测）、
源码 tarball 构建、CI 上不带 `.git` 的 checkout、把 repo 拷进去的 Docker 构建。

**给 `hs` 的要求**（在拆分 `wireFingerprint` / `buildFingerprint` 之外新增一条）：
取 stamp 失败必须是**具名的**——要么配置期失败并说明，要么写进一个明确表示"本构建无版本标识"的哨兵值
并让握手对它具名拒绝（`Dial == Fork` 下两端本来就该是同一二进制，无版本标识意味着这个前提无法验证）。
**不能继续是空串。**
red-once：在一个 `git` 不可用的环境里构建，握手必须因为"build stamp 缺失"而具名变红，而不是悄悄通过。

## 8 另一件顺带记录的事

`.gitmodules` 里 `3rdparty/DiligentCore` 有重复配置（`path` 与 `url` 各两份），
`git fetch` 会 warn 并跳过第二份。不影响构建，但迟早要清。
