# 实测记录

> 每张表写明设备、提交与命令。设备：`35d0befa` = Xiaomi 24129PN74C（SM8750 / Adreno 830v2）与 `3B159D009VZ00000` = Oppo PLG110（MT6993 / Mali）用于 P0–P3a；`2f7cbe2e` = Redmi M332BF（SM8750 / Adreno 830v2，与小米同 SoC 同定频点）是 P4a 起唯一的 A/B 设备。定频、风扇与热协议见 `devices/pin-verification-2026-09-07.md`。
>
> 口径（用户 2026-09-08 起）：性能对着 pull 臂**记录**、不作阻塞门；正确性门是硬门。两条读数纪律：`acc/draw` 是约 10 个热入口的**静态**计数，把读点搬走与把工作去掉在它上面长得一样，判性能只看 CPU 时间序列与六个 memo 门；`ctest -V` 做拒绝普查是假零（console sink 在发布配置里被编译掉），必须逐用例读私有日志。

## 1. P0（2026-09-05，trace APK `7ef7c7e5`，spike `8a239177`）

### 1.1 Spike A — 从应用自身进程 exec 第二个原生可执行文件

| 设备 | 结果 |
|---|---|
| Adreno 830 | **OK**：`untrusted_app` 父进程 `fork`+`execve` `<nativeLibraryDir>/libMobileGLServer.so`，子进程同域同 category、exit 0、`execErrno=0`、零 avc denial |
| Mali | **OK**，同形 |

主机侧已证：AGP 会把改名成 `lib*.so` 的 `add_executable` 打进 `lib/arm64-v8a/`（`RUNTIME_OUTPUT_DIRECTORY` 重定向到 `CMAKE_LIBRARY_OUTPUT_DIRECTORY`）；`posix_spawn` 在 minSdk 26 不可用；应用进程 stdout/stderr 是 `/dev/null`，子进程用 marker 文件证明自己活过。代码：`tools/spikes/server_stub/`、`android-plugin/app/src/trace/cpp/spawn_spike.cpp`、`MOBILEGL_BUILD_SERVER_SPIKE`。

### 1.2 Spike B — 跨进程外部内存分档（`tools/spikes/extmem_probe/`，`shell` 域，4 MiB payload，每行一次真 GPU 访问 + 两侧字节校验）

| 路线 | Adreno 830 | Mali |
|---|---|---|
| T1-opaque-fd（server 导出 `VkDeviceMemory` fd，client `mmap` + 导入） | **OK** 完整往返 | UNSUPPORTED（`VK_ERROR_INVALID_EXTERNAL_HANDLE`） |
| T1-dma-buf | UNSUPPORTED | UNSUPPORTED |
| T1-gles-memobj-fd（`GL_EXT_memory_object_fd`） | **FAIL**：导入接受，但每次 `glMapBufferRange` → `GL_INVALID_OPERATION` | UNSUPPORTED |
| **T0-ahb-blob-transfer**（client 分配 `AHardwareBuffer` BLOB → socket 交接 → server Vulkan + GL 导入） | **OK** 全链 | **OK** 全链 |
| T3-external-memory-host | UNSUPPORTED | PARTIAL：GPU 写对宿主映射不可见（只读档） |

决定：唯一在两台设备、两个后端上都是完整读写的档是 **T0**；Adreno 另有 T1（Vulkan 路径）；Mali 无任何 server 导出路线。Caveat：运行域是 `shell` 不是 `untrusted_app`（`ROADMAP.md` 开放问题 3）。

```sh
ANDROID_NDK=$HOME/android-sdk/ndk/27.3.13750724 tools/spikes/extmem_probe/build_android.sh /tmp/extmem-build
adb -s $S push /tmp/extmem-build/extmem_probe /data/local/tmp/ && adb -s $S shell "chmod 755 /data/local/tmp/extmem_probe && /data/local/tmp/extmem_probe"
```

### 1.3 边界计数器基线（`MOBILEGL_PIPE_STATS=1`，最后一个完整 120 帧窗口；accessor / memo 数字是软件确定的，两台设备相同）

| trace（窗口内帧数） | 后端 | draws/f | acc/draw | buf B/f | tex B/f（发射 box / rect） | ubo-global B/f | ubo-named B/f | memo 门（hit/miss） |
|---|---|---|---|---|---|---|---|---|
| `minecraft-1.21.4-in-world`（360） | Espryt | 91.6 | **9.28** | 13.5 K | **635 K**（185 box / 0 rect） | 16.7 K | 0 | ers 9257/2577，etl 10538/1296，eub 10720/1114 |
| 同上 | Magma | 91.6 | **8.56** | 13.5 K | 39.9 K（97 box / 89 rect） | 16.7 K | 0 | mfp 0/10994，mpm 9240/1754，mdt 9120/1874 |
| `minecraft-1.21.4-fabric-iris-bsl-in-world`（120，memo 冷） | Espryt | 23.2 | 21.04 | 32.6 K | 8.8 K | 1.8 K | 0 | ers 1958/1843，etl 722/3079 |
| 同上 | Magma | 23.2 | 11.26 | 313 K | 256 K | 1.8 K | 0（vtxc 1.7 K） | mpm 1890/895，mdt 1573/1212 |
| `improved-transparency-minecraft-26.3`（1200） | Espryt | 1320 | **8.44** | 333 K | 0 | 0 | 0 | ers 156925/2791，etl 148606/11110，eub 148246/11470 |
| 同上 | Magma | 1320 | **6.53** | 173 K | 0 | 0 | **331 K** | mfp 21360/137036，mpm 134421/2615，mdt 156611/1785 |

门缩写：ers = `EsprytRenderState`，etl = `EsprytTextureSyncList`，eub = `EsprytUnitBindingsEpoch`，mfp = `MagmaDrawFastPath`，mpm = `MagmaPipelineMemo`，mdt = `MagmaDynamicTail`。读法：真机稳态动态 accessor 成本是每 draw 6.5–9.3 次（预测区间 10–25 的下沿），推送要打败的是 ~8 次 accessor + memo 探测；Magma 在 26.3 每帧重打包 **331 KB** 具名 UBO 字节（D-B8）；同样 185 次发射，Espryt 整 box 路径 635 K 纹素字节 / 帧、Magma rect 路径 40 K，16×（"server 选上传形状"的依据）。

```sh
ANDROID_SERIAL=<serial> MSYS_NO_PATHCONV=1 python3 tools/trace_replay/run_android_retrace_local.py \
  --case minecraft-1.21.4-in-world --backend DirectGLES --env MOBILEGL_PIPE_STATS=1 --env MOBILEGL_PIPE_STATS_PERIOD=120
# 数字在结果目录的 mobilegl.log 里，grep 'MGPipe stats:'
```

### 1.4 桌面数据点与语料事实

- llvmpipe / lavapipe 动态 accessor（`GuiBatchScenario`，memo 冷）：Espryt 20.65 / Magma 15.54 次 / draw。
- dirty-surface 面：`MG_Impl/GLImpl` 41 个文件、926 次 mutator 调用、73 个不同 mutator（`RecordError` 占 836 次）；92 次位于同函数内也到达后端的即时发布点，其余由紧随的 verb 发布。
- 读点覆盖：63 个 `PipeInputs` 字段；后端读点清单 → 调用映射 **0 UNMAPPED**。
- OOM 探测惯用法：41 个 trace fixture 中 0 例（9 次 `glRenderbufferStorage` 无一在 3 个调用内跟 `glGetError`）→ `glRenderbufferStorage*` 不 ack。
- `FramebufferSrgb` / `DepthClamp`：六个后端读点消费编译期常量 `false` / 零读点；`glEnable` 落到 `default:` 分支既不存储也不报错；41 个 fixture 无一开启。
- payload 尺寸（`MGPipeTypes.h` 的 `static_assert`，arm64 与 x86-64 一致）：`MGPDrawInfo` 56、`MGHostSpan` 32、`MGPBindRenderState` 12、`MGPResourceDesc` 88、`MGPFramebufferState` 304、`MGPProgramDesc` 192、`MGPSubData` 72；`SEG_CMD` 按 56 B 头定尺，MC 帧 1000–4000 draw 时每帧 56–224 KiB 头字节。
- persistent map 采纳的既有基线（`dev`，MC 26.3，Adreno）：p99 163→21 ms、稳态 40→115 fps、省 ~400 MB；P11 的回归上限对着它。Mali 上传作业数悬崖：~100 个精灵 rect 对一个 union box 是 +6 ms/frame。

### 1.5 Harness 事实与陷阱

- trace app 从不到达 `MobileGL::DestroyImpl`，`MOBILEGL_PIPE_STATS_FILE` 在设备上永远不会写；只有 `mobilegl.log` 的周期汇总行，`MOBILEGL_PIPE_STATS_PERIOD` 为此而加。
- `run_android_retrace_local.py` 每棵树共用一个 `.trace-work/android-retrace-result` 根并 `rmtree`，两台设备必须从一棵树**串行**跑。
- `--env` 值里的 `/data/...` 会被 MSYS 路径转换，用 `MSYS_NO_PATHCONV=1`。
- ColorOS 首次 `adb install` 卡在 `InstallGuideActivity` 确认页；streamed install 进行中点它会让设备掉线几秒。
- `minecraft-1.21.1-neoforge-create-indirect-in-world` 在两台设备上都失败，`dev@81b17c0b` 基线 APK 复现——`dev` 侧问题（`ROADMAP.md` 开放问题 17）。

## 2. P1（lavapipe / llvmpipe）

### 2.1 规模

| 量 | 值 |
|---|---|
| 后端 `pGLContext->` 箭头站点 | 277（Espryt 113、Magma 164） |
| 非箭头行 | 58（Espryt 9、Magma 49，其中 43 条是 Magma 的逐 verb `MOBILEGL_ASSERT`） |
| `PipeInputs` 字段 / 不同访问器 | 63 / 62（Espryt 32、Magma 56） |
| 填充点 | 83 条 `MGP_FILL`，覆盖 69 个 verb、9 个类 |
| `SyncPersistentMappedRange` / `SyncGpuWrites` | **21**（Espryt 9 + Magma 12；P5 b1 复核，原记 20 漏了共享 helper `ResolveIndirectCommandBytes`）/ 6 |

### 2.2 verify 通道发现的两类真问题

**缺填充行（9 处）**：`kReadback` 缺 `IsTransformFeedback{Active,Paused}`、`kTextureOp` 与 `kDispatch` 缺 `IsCapabilityEnabled`、`kBlitOrCopy`/`kTextureOp` 缺着色器 blit 用到的 viewport 与顶点 / 缓冲绑定。其中 8 行是静态过近似（代码路径可达但通道未跑到），有意保留——能退役它们的证据只能是动态的（完整 CTS caselist + `MOBILEGL_PIPE_POISON_OMIT`，未做）。

**verb 内后端改前端（3 个字段）**：Magma 在 draw 里写前端对象（合成回退纹理、材质化排队清除、覆写 sampler filter），8 条 DirectVulkan 用例与 2 条 trace 报 `Fatal{PipeVerifyDiffer, "GetSamplingResolutionGeneration@Draw*"}`。解法 **push-on-mutation**：前端计数器移动时用 `MGP_NOTE_MUTATION(Field)` 刷新推送块里那一个字段——钩子挂在三个计数器上（`BumpSamplingResolutionGeneration`、`BumpTextureBindGeneration` / `NoteUnitTouched`、高水位分支），不挂在四十个写入点上。

### 2.3 验收

| 门 | 结果 |
|---|---|
| pull 构建符号与 `.text` | 0 增 / 0 删 / 0 改尺寸 / 0 重命名，`.text` 不变 |
| `MG_Backend` 里的 `pGLContext` | 0 |
| 单元（pull / push / verify） | 1485 × 3 |
| `integration-gpu`（pull） / `integration-verify` | 878 / 818 零 `Fatal{` |
| 79 例 retrace（`MOBILEGL_PIPE_VERIFY=1`） | 79/79，79/79 armed，零 `Fatal{` |
| 两个阴性对照 | 篡改字段变红、抽掉一个填充戳记在那条 verb 上变红 |
| 测试名 | 0 删除，+29 |

verify 构建的代价：`integration-verify` 与 `integration-gpu` 同量级；79 例 retrace 在 4 路下约 20 分钟。

## 3. P2（`738b289d`）

### 3.1 门

| 门 | 结果 |
|---|---|
| G1 pull 构建符号 | 0 增 / 0 删 / 0 重命名；**4 处认定 resize**（`RenderState::{RenderState, SetCapability, IsCapabilityEnabled}`、`_GLOBAL__sub_I_DirectGLES.cpp`）；`.text` +160 B |
| G5 `SyncRenderState` | `RenderStateImpl` 段 sha 逐字节相同 |
| G2 / G14 | 名差 0；0 删除 / +119 |
| 单元 | 1566 × {pull, push, verify} |
| `integration-gpu` | 916/916 × {pull, push, `MOBILEGL_PIPE_PUSH=0`}；渲染状态敏感子集 72/72 |
| `integration-verify` | 828 零 `Fatal{` |
| 79 例 retrace | push 79/79；verify 79/79 armed 零分歧 |
| G7 setter 一致性阴性对照 | 变红并点名 `SetColorMask` |
| `CsoContentAddressing` / verify 三组对照 | 6/6 / 44/44（`PoisonOmitted`、`VerifyCorrupted`、`HandleRecycle`） |

### 3.2 设备配对 A/B（-O0 勘误）

两机 64 次运行（小米 + Oppo，四 trace × 两后端 × finish 开/关，reboot-clean、钉频、best-of-3、尾 200 帧）所用的两臂 APK 是**未优化构建**（`libMobileGL.so` 43.2 MB / `.text` 21.8 MB，对 Release 15.5 / 9.4 MB），绝对值与差值都是 -O0 读数，不作基准线；Release 基准线以 §4.4 为准。-O0 读数：小米 p50 **+8–14%**（四 trace 两后端），Oppo Espryt **+16–18%** / Magma **+10–11%**，p99 同向；finish 开关两臂几乎一致（多出的是客户端 CPU）；计数器两机逐字相同。原始 32 行表在 git 历史（`ea34bccb` 版本的本文件 §10）。harness 误杀记录：`trace-replay-ci.sh` 对 `pidof` 单次采样失败即 force-stop，`dev` 侧跟进。

### 3.3 DriverBench：T1 / T2（桌面 llvmpipe / lavapipe，`wsl_p2_bench.sh`，Release，240 帧 × 5 次取中位数，`ns_per_op`）

| 臂 | `mc_vanilla_draw`（ns/draw） | `mc_state_toggle`（ns/开关对） | `mc_pass_switch`（ns/pass） |
|---|---|---|---|
| native | 4771 | 22968 | 437759 |
| Espryt pull / push / push `PIPE_PUSH=0` / push 位 63 | 5121 / 5443 / 5671 / 5374 | 23753 / 24869 / 24584 / 24630 | 443300 / 446017 / 443807 / 435454 |
| Magma pull / push / push `PIPE_PUSH=0` / push 位 63 | 16900 / 17245 / 17599 / 17444 | 32705 / 33857 / 34082 / 34780 | 438406 / 446067 / 445006 / 444758 |

| `mc_vanilla_draw`，ns/draw | Espryt | Magma |
|---|---|---|
| **T1** = push − pull（整个边界） | **+322**（+6.3%） | **+345**（+2.0%） |
| **T2** = push(`PIPE_PUSH=0`) − pull（P1 残余填充） | +550 | +699 |
| **T1 − T2**（P2 自己加的减的） | **−228** | **−354** |
| 位 63 − push（CSO 内容寻址净值） | −69（≈ 0） | +200（内容寻址每 draw 省 200） |
| blend-toggle / pass switch | +4.7% / +0.6% | +3.5% / +1.7% |

读法：P2 的 tracker + CSO 比它替掉的 P1 残余填充便宜；剩下的 T1 是尚未句柄化的填充与 dirty 走查。`mc_state_toggle` 由 `DriverBenchStateToggle` 钉住每帧 46 对开关；位 63 对照的开关 `CsoContentAddressingScenario` 常开（内容寻址臂 `csom ≤ 4` 且 `csom < csob`，位 63 臂 `csom == csob`）。

### 3.4 计数器读数

`resid=` 已非零且棘轮压到底（1248 → 8）：桌面 push retrace `iris-bsl`，`resid=197.07` B/帧 = 每 draw 0.64 块；`csom/csob` = 8 / 1415（CSO 复用比）。小米 push 臂（-O0，计数器与优化无关）：

| trace | Espryt `ers` / `etl` / `eub` | Magma `mfp` / `mpm` / `mdt` | `resid=` B/帧 | `csom` / `csob`（每 120 帧） |
|---|---|---|---|---|
| `minecraft-1.21.4-in-world` | 6020/1850 · 6884/986 · 6962/908 | 0/7278 · 6030/1248 · 5928/1350 | 178.31 | 2 / 1719 |
| `improved-transparency-minecraft-26.3` | 78960/1367 · 75378/4949 · 75199/5128 | 10740/68928 · 67660/1268 · 78800/868 | 185.36 | 0 / 1157 |
| `minecraft-1.21.4-fabric-iris-bsl-in-world` | 113/133 · 85/161 · 82/164 | 0/177 · 86/91 · 81/96 | 370.67 | 1 / 122 |
| `minecraft-1.21.4-startup` | 174/185 · 305/54 · 305/54 | 0/118 · 0/118 · 0/118 | 25.08 | 9 / 181 |

CSO 内容寻址在稳态窗口里几乎不再铸造；memo 门形状与 §1.3 的拉取基线一致。**未测**：逐 dirty 位触发率（`FireCount`/`WalkCount` 已实现但不在汇总行）、每 draw payload 直方图（只在 teardown JSON 里）。

### 3.5 遗留判定

- `MOBILEGL_ESPRYT_DISABLE_INVALIDATE_FLUSH=1` 的 186 条过滤不进比对器（P3a 在 push 下跑了一次 958/958；进比对器推迟到 P3b）。
- Track H 单位成本的日历口径未记录（产出侧在案：11 条 memo 删除 + 2 条重键零回归）。
- 句柄 ABA 对照"重键前红"：修复前 `HandleRecycle` 28 条中 1 条红（`AbaControl` 期望死对象的红却看到替换对象的绿）；修后 32/32，且关掉 `MOBILEGL_PIPE_HANDLE_ABA_CONTROL` 两臂都 `observed=FRESH` 并失败——污染只由被打掉的身份产生。

## 4. P3a（`fde5fda3`，基线 `5cb826b0`，44 个提交，37 文件 / +9273 / −230）

### 4.1 五部分门（本地 `~/w7/pipe`）

| 门 | 结果 |
|---|---|
| 构建 pull / push / verify | rc 0 / 0 / 0（fail-fast，绝不拿陈旧二进制下判决） |
| A 门 include 闭包 / C 门 `pGLContext` / G13 | 4 probes 0 problem / 空 / 空 |
| **G1** | `.text` 10806323 → 10806323（+0）；27811 符号 **0 / 0 / 0 / 0** |
| **G5** 十一函数（pool / 延迟释放 / ring / `FlushPendingRangesNow` 与 `FlushPendingRangesFrom`） | 对 `44c2b5cf` 与 `5cb826b0` 都逐字节相同；self-test 两个阴性对照按名变红 |
| `integration-verify` / 79 retrace verify | 842/842 零 `Fatal{` / 79/79 armed 零分歧 |
| G2 / G14 | 差 0 / 0 删除 +58 |
| 单元 | 1619 × 3 |
| `integration-gpu` | 958/958 × {pull, push, `PIPE_PUSH=0`, `0x7f`（G12 子系统关闭臂）, `ESPRYT_DISABLE_INVALIDATE_FLUSH=1`} |
| buffer/VAO 族 / `RenderStateSpans`+`Residual`+`VertexInputEmit`+`ResourceEmit` | 202/202 / 48/48 |
| `HandleRecycle`（verify） / 子系统对照 / verify 对照组 | 60/60 / 10/10 / 64/64 |
| G7 vertex-input 阴性对照 | 变红并点名 `IsBgra` |
| 79 retrace push | 79/79 |
| G3b 具名五条 | 桌面全绿（都在 79 例扫描内），但单独的具名扫描因门脚本把正则写成逗号清单而选中 0 例；设备侧按开放问题 17 排除 `create-indirect` → 记为"桌面全绿、设备侧部分" |
| `gen_pipe` / `gen_pipe_dirty_surface`（扫描根扩到 `MG_State/GLState`） | self-test 7 / 21 个阴性对照全部触发；75 个 mutator 全映射，0 COARSE / 0 UNDECIDED |

### 4.2 "重键前红"与验证轮找到的三个缝隙缺陷

buffer 类的红前证据（package C 尚未注册 `MGPipeResourceOps` 时）：`HandleRecycle` 的 `.AbaControl` 臂通过（它断言的就是被污染的内容）、`.Legacy` 通过、`.Handles` 可见地 skip；C 注册后两条 skip 转为断言，最终 60/60。

| # | 缺陷 | 表现与修法 |
|---|---|---|
| F1 | vertex buffer 条目按属性的 GL 绑定点解析，而不是按属性下标 | 两者只在 `KHR-GL43.vertex_attrib_binding` 题材上分叉；`VertexAttribBindingScenario` 6 → 3 |
| F2 | ensure 路径读 `MGPResourceDesc::HasDefinedContent`（上一次 respecify 时的事实）判断影子有无字节 | `glBufferData(size, NULL)` + `glBufferSubData` 后应用字节被静默丢掉；改为持有前端对象时读 `HasDefinedContent()` |
| F3 | 惰性生成的 twin 从不发布影子基址，fp64 收窄拒绝每个 draw | `hostBytes` 终身 null → Adreno workaround 禁用属性；在算出基址处补发布，只对非采纳资源 |

方法学：包 worktree 里 `tools/trace_replay/fixtures` 是 LFS 指针，对着指针跑 retrace 报 `passed 2 / 79`、`ssim=None`——**假红**；base-instance 对照在 llvmpipe 上要先关掉原生 `GL_EXT_base_instance` 才可证伪。

### 4.3 Track H 单位成本

日历：四个包（contract/wire、client、espryt、gates）各一轮实现 + 对抗性评审 + 至多两轮返工，全部在 2026-09-08 一天内，对 27 天绊线 1/27。memo 账：直接删除普查 11 条里的最后一条（`ConvertedVertexStreamKey::sourcePin`）；句柄臂上退役 VAO twin 五个同步 memo 成员；重键 VAO twin 索引槽 memo、`ResolvedDrawBuffers`、twin 同步门、`ConvertedFloat64Stream`；**保留**（`MOBILEGL_PIPE_LEGACY_MEMOS` 下仍编译）：以上全部成员、`g_pendingFetchBaseInstance` 族、D-K 那组——真删会移走 pull 符号或改 `sizeof`，G1 的 0/0/0/0 就是这条的度量，随 P13 退役。

### 4.4 两机 Release 配对 A/B、MC 26.3 的 p99、DriverBench（记录项）

协议：reboot-clean、`pin_device.sh` 钉频并前后 `check`、两臂背靠背、`--benchmark-repeats 3 --benchmark-tail-frames 200`、best-of-3、`--benchmark-no-finish` 主臂；APK 都是 `3e298c9a` 的 Release trace 构建（`.text` 9.4 MB）。臂：pull = pull 库；P2 = push APK `MOBILEGL_PIPE_PUSH=0x7f`；P3a = push APK 默认 `0x1ff`。`create-indirect` 排除（开放问题 17）。单位 ms/帧。

| 设备 | trace | 后端 | pull p50 | P2 p50 | P3a p50 | pull p99 | P3a p99 | 备注 |
|---|---|---|---|---|---|---|---|---|
| 小米 | `improved-transparency-minecraft-26.3` | Espryt | 10.810 | 11.485（+6.2%） | 11.697（**+8.2%**） | 25.457 | 26.322 | |
| 小米 | 同上 | Magma | 10.675 | 11.473（+7.5%） | 11.459（**+7.3%**） | 25.014 | 26.035 | |
| 小米 | `minecraft-1.21.4-rd12-odinlite-in-world` | Espryt | 8.220 | 9.117（+10.9%） | 10.650（**+29.6%**） | 21.895 | 24.487 | |
| 小米 | 同上 | Magma | — | — | — | — | — | 三臂都 SIGABRT（`scudo::reportMapError`，`dev` 侧） |
| 小米 | `minecraft-1.21.4-fabric-sodium-in-world` | Espryt | 1.292 | 1.349（+4.4%） | 1.382（**+7.0%**） | 2.418 | 2.443 | |
| 小米 | 同上 | Magma | 0.485 | 0.504 | 1.010 | 1.469 | 2.187 | 亚毫秒帧，噪声 |
| 小米 | `create-instancing` | Espryt / Magma | 944.4 / 1003.9 | 945.2 / 1019.5 | 949.9 / 1015.6 | — | — | 两帧 fixture |
| Oppo | `improved-transparency-minecraft-26.3` | Espryt | 9.741 | 10.570（+8.5%） | 10.542（**+8.2%**） | 26.312 | 27.022 | |
| Oppo | 同上 | Magma | 8.162 | 8.920（+9.3%） | 8.973（**+9.9%**） | 22.851 | 23.638 | |
| Oppo | `minecraft-1.21.4-rd12-odinlite-in-world` | Espryt | 9.938 | 11.009（+10.8%） | 12.963（**+30.4%**） | 27.137 | 29.765 | |
| Oppo | 同上 | Magma | 8.001 | 8.933（+11.6%） | 10.161（**+27.0%**） | 24.723 | 27.355 | |
| Oppo | `minecraft-1.21.4-fabric-sodium-in-world` | Espryt / Magma | 1.896 / 0.399 | 1.770 / 0.431 | 1.841 / 0.437 | 2.993 / 1.206 | 3.079 / 1.409 | 噪声内 |
| Oppo | `create-instancing` | Espryt / Magma | 1075.1 / 758.3 | 1045.1 / 731.0 | 1060.1 / 760.0 | — | — | 两帧 fixture |

读法：(1) **P2 边界在 Release 下的真实代价是 +6–12%**，两机两后端一致；(2) P3a 在 26.3 与 sodium 上几乎不再加价，**但在 rd12 上把差距从 +11% 推到 +27–30%**——rd12 每帧 VAO/buffer 绑定切换远多于 26.3，每次切换走一遍 `set_vertex_buffers` 构造 + `ContentHash` + applier 记录 + 逐属性走查（P4a 补的 `csob-blob` 计数器给了字节口径：rd12 1.06 MB/帧，其余用例中位数 2.9 KB/帧；DirectVulkan 恒 0）；(3) **MC 26.3 在 Adreno 上的 p99**：25.46 → 26.32 ms（+3.4%），仍在采纳基线的 21–26 ms 档；(4) `mpr` 在设备上成立（26.3 两机都是 8，P2 臂恒 0）；(5) 计数器三臂逐字相同。

小米 rd12 + Magma 三臂（含 pull）都 `SIGABRT`：`scudo::reportMapError` ← `scudo_calloc`，pull 库与 P3a 前的 Magma 路径符号一致——既有 bug，`dev` 侧任务。

DriverBench（`wsl_p3a_bench.sh`，`c20e2f2b`，`mc_vanilla_draw` ns/draw；T2 = `0x7f` 臂）：

| | Espryt | Magma |
|---|---|---|
| **T1** = push(`0x1ff`) − pull | **+1048**（+20.5%） | **+611**（+3.6%） |
| **T2** = push(`0x7f`) − pull | +349 | +189 |
| **T1 − T2 = P3a 自己加的** | **+699** | **+422** |
| 位 63 − push | −6 | +20 |
| blend-toggle / pass switch | +5.2% / ≈ 0 | +2.8% / ≈ 0 |

读法：P3a 的 buffer/VAO 句柄路径每 draw 多花 Espryt 699 / Magma 422 ns（Magma 没有句柄化的 VAO 消费者，全是 client 侧发射 + applier 记录）；进优化清单：vertex-input 发射的 per-draw 抑制与 applier 记录就地更新。`DriverBench` 必须经 `DRIVERBENCH_EGL_LIB` 显式 `dlopen` provider，不得靠 `LD_LIBRARY_PATH`（glvnd 会把 GPU 悄悄换成 llvmpipe）。

### 4.5 决定与遗留

- **ID-15**：`Managers.cpp` 的三层 flush 排水梯每个预处理臂各一份——push 构建跑 `FlushPendingRangesFrom`，pull 构建跑逐字节不变的 `FlushPendingRangesNow`；两个名字都是 G5 行，各对自己的 pin 比。转发函数不可行（G5 提取器不认预处理）。
- **M-3**：fp64 顶点数组收窄在句柄臂上少做一处 `SyncGpuWrites`（`SyncFloat64AttributeAsFloat32ByHandle` 手里只有句柄）——默认掩码下两臂唯一的行为差异（fp64 数组 + shader 写过的源 buffer + 无显式回读），P8 把拉取搬到 client 侧即关闭。
- 整体 diff 终审抓出两个跨包接缝：client 为每个 VAO 铸的 `VertexElementsCso` 槽在 Magma 下泄漏（修为 `~VertexArrayObject` 走后端无关的死亡路径）；`FlushPendingRangesFrom` 在 G5 覆盖之外（→ ID-15）。
- CI 独有的 teardown 崩溃（`exit()` 时静态析构顺序 UAF）：`MGPipeSlots()` 等单例与四个持有前端 `SharedPtr` 的静态 `PipeInputs` 改为退出时泄漏（`d54ec57a`、`6515c8e6`、`fde5fda3`），ASan 10/10 干净；复现钥匙 `GLIBC_TUNABLES=glibc.malloc.tcache_count=0`。
- `dev` 侧跟进（拆分不顺手修）：`g_uploadRing` 不被 `OnBackendContextDestroyed` 重置（良性，`RingAvailable` 自愈）；`ScopedDefaultUnpackState::s_synced` 没有失效路径。
- `MG_Test/Buffer/BufferTest.cpp` 的 fixture 只 scope 了 `BufferBackendOps` 不 scope `MGPipeResourceOps`，26 条被路由进 pipe——修后 86/86；给它一个 pipe 形 mock 是跟进项。
- 峰值 RSS（push vs pull，79 例）：`retrace_gate.py` 不报，没有数。

## 5. P4a（`8c458cd5`，基线 `37da3c3a`，101 个提交，86 文件 / +30066 / −384）

### 5.1 全门（`6035c9d7` 全量 + `8c458cd5` 复跑）

| 门 | `6035c9d7` | `8c458cd5` |
|---|---|---|
| **G1**（认定 resize 集为空） | 0 / 0 / 0 / 0，`.text` 不变 | 同上 |
| **G5** | P3a 十一函数 rc 0；P4a **17 区 / 3 文件** rc 0，self-test 8 个阴性对照按名变红 | 同上 |
| G2 / G14 | 差 0 / +275 | 差 0（2902 条）/ +314 |
| 单元 | 1772 × 3 | **1785 × 3** |
| `integration-gpu` | 1091/1091 × 七臂 | **1117/1117 × 七臂**（默认 `0x1fff`、`0x1ff`、`0`、`0x9ff`、`0x5ff`、pull、`ESPRYT_DISABLE_INVALIDATE_FLUSH=1`；DirectVulkan 559/559） |
| `integration-verify` | 896/896 零 `Fatal{` | **920/920** |
| retrace（79 例） | verify 79/79 armed 零分歧；push 79/79；G3b 12/12 | push 79/79 |
| 阴性对照脚本 | `g7_negative_control.sh`、`p3a_vertex_input_negative_control.sh`（`IsBgra`）、**`p4a_descriptor_negative_control.sh`（`Layered` 与 `borderColorForm`）** rc 0 | 同上 |
| 子系统对照套件 | `CsoContentAddressing` + `ResourceSubsystemControl` + `ObjectSubsystemControl` 24/24；`0x9ff` 依赖拒绝臂 14/14；`HandleRecycle`（verify）180/180 | 同上 + 184/184 controls |
| 八族拒绝普查 | — | **0**（逐用例读私有日志；`ctest -V` 是假零） |

G9 的"落地前必须红"没能通过公共 GL 达成（回读模拟自己会设 `GL_DEPTH_STENCIL_TEXTURE_MODE`）：改成 `PipeApplyPeek` 白盒断言，变异下 4/4 变红。真正红过再绿的两条来自终审：per-level respecify 丢上传（`0x1fff` 下读回全黑）与 delete-then-draw（`pure virtual method called`）。CTS `direct_state_access.framebuffers*` / `packed_pixels`（G15）未跑，随 P3b/P4b 补。

### 5.2 缝的分类（契约七次修正 `c0b`…`c0g` + 一轮缝类审计 + 一轮终审修复）

| 类 | 实例 | 症状 | 预防 |
|---|---|---|---|
| 编码没定死 | `MGPSubData::Target`、`DepthStencilMode`、`MGPSurface::Kind`、缺 `TextureTarget` | 两侧各自发明一套；`Texture1D == 0` 与 `kMGPipeResourceTargetBuffer == 0` 撞上 | 编码表放进契约 |
| 身份 vs 内容 | 内置 sampler：client 按身份铸、cache 按内容铸 | 查找永远落空 → 17 条 Iris trace 用驱动默认采样器 | 每 kind 两侧 handle 规则表 |
| 记录键错了维度 | framebuffer 记录按"当前绑定"存，DSA 按名字来 | 打进从没收到附件的 FBO | 记录按对象存；`Named = 3` |
| 进程级单例 vs 每上下文命名 | `CompositeResolver` 按管线 GL 名记忆 | 释放另一个上下文还活着的合成体 | 单例的键含上下文身份 |
| 破坏性客户端动作缺前置条件 | 按 acceptance 清 dirty，但 Magma 无消费者、依赖位只在服务端拒 | 上传丢失：66 条 DirectVulkan 用例、`0x7ff` 下 438/491 | 消费者门 + 依赖门都在客户端：一条不发 |
| 快门看不见自己的主体 | `glBindSampler`、SSO 下 `GetCurrentProgram()`、`glBindImageTexture` 只换 level | 记录停在上一次的值（`create-indirect` SSIM 0.887） | "记录字段 → setter → 快门"完备表；优先混入已有世代 |
| 清得太宽 | `resource_respecify` 清掉整张待上传表 | 已接受的那一级永久丢失 | 作用域随调用走 |
| 死亡没通知发射方 | 六个死亡 helper 只释放 slot | 已删句柄解析到已释放对象 → UAF | 死亡转发给每个 emitter |
| 门不能变红 | G7 脚本永远编译不过、`HighWater(ShaderCso)` 取段顶、G9 红前态公共 GL 不可见 | 绿得毫无意义 | 每个门带阴性对照并真跑过一次红 |

方法论结论：六个包的 v1 全部通过自己的门、六份对抗性复审全部判 REWORK，最贵的两个缺陷（丢上传、delete 后 UAF）是**整体 diff 终审**才抓到的——它们跨包，各方自洽。终审是唯一能看见跨包契约的那一轮。

### 5.3 Redmi 三臂 A/B、MC 26.3 的 p99、上传形状（记录项）

设备换为 Redmi `2f7cbe2e`（与小米同 SoC 同定频点，数值可比；GPU 当时钉 1050 MHz；主动风扇 level 2，40 个样本 40 个 PINNED）。APK `8c458cd5` Release 双臂；`--benchmark-no-finish`，尾 200 帧、best-of-3、逐线程 CPU p50 ms；`0x1ff` = P2+P3a 边界，`0x1fff` = P4a 默认。

| 用例 | 后端 | pull | `0x1ff` | `0x1fff` | Δ P2+P3a | Δ 合计 | **P4a 自己** |
|---|---|---|---|---|---|---|---|
| improved-transparency-26.3 | Espryt | 10.716 | 11.754 | 12.124 | +9.7% | +13.1% | **+3.4 pt / +0.37 ms** |
| improved-transparency-26.3 | Magma | 10.603 | 11.543 | 11.585 | +8.9% | +9.3% | +0.4 pt（噪声） |
| rd12-odinlite | Espryt | 8.210 | 10.798 | 11.182 | +31.5% | +36.2% | **+4.7 pt / +0.38 ms** |
| rd12-odinlite | Magma | — | — | — | — | — | 三臂全 `rc=1`（`scudo`，`dev` 侧，换机仍复现） |
| fabric-sodium | Espryt | 1.312 | 1.406 | 1.454 | +7.2% | +10.8% | +3.6 pt / +0.05 ms |
| fabric-sodium | Magma | 0.474 | 0.502 | 0.507 | +5.9% | +7.0% | +1.1 pt（噪声） |
| 1.21.4-in-world | Espryt | 2.369 | 2.732 | 2.867 | +15.3% | +21.0% | **+5.7 pt / +0.14 ms** |
| 1.21.4-in-world | Magma | 1.028 | 1.147 | 1.145 | +11.6% | +11.4% | −0.2 pt（噪声） |
| fabric-iris-bsl | Espryt | 1.727 | 1.740 | 1.811 | +0.8% | +4.9% | +4.1 pt / +0.08 ms |
| fabric-iris-bsl | Magma | 0.742 | 0.788 | 0.786 | +6.2% | +5.9% | −0.3 pt（噪声） |

读法：P4a 自己在 Espryt 上是 +3.4 – +5.7 pt（0.05–0.38 ms/帧），大头仍是 P2+P3a 的边界；Magma 两臂在四个用例上落在噪声内——消费者门在设备上的读数（Magma 一条 P4a 记录都不发）；`vanilla` 是 P4a 占比最高的用例（draw 少、状态切换密）。**MC 26.3 在 Adreno 上的 p99**：pull 25.297 → P4a 26.841 ms（+6.1%），仍在 21–26 ms 档。**上传形状 pull 与 push 逐项相同**（79 例两臂 `tex[emit/box/rect/jobs]`：18451/16060/2391/39926 对 18453/16062/2391/39928，唯一差异是 2 次 `trp` 带来的重放上传）。线索：设备上 26.3 Espryt 的 `sve` ≈ draw 数（9143 / 9138），桌面 ~0.07/draw——每 draw 重发一次 sampler-view 集合，进 P3b/P4b 优化清单。`acc/draw` 在这一波普遍下降而 CPU 上升，不是矛盾而是该计数器的定义。

### 5.4 DriverBench（`wsl_p4a_bench.sh`，`8c458cd5`，`mc_vanilla_draw` ns/draw）

Espryt T1（`0x1fff` − pull）= +1076.1，T2（`0x1ff` − pull）= +1064.7，T1 − T2 = +11.4；Magma T1 = +629.0、T2 = +452.6、T1 − T2 = +176.4。两臂绝对值在两轮之间各漂 ~200 ns 而差只有 10–130 ns，**桌面 bench 分辨不出 P4a 这一档**；可引用的是 T1 ≈ +1.1 µs/draw 的总边界（Espryt）与设备侧三臂表。Magma 的 +176 ns 不是"Magma 在跑 P4a"（消费者门让它一条不发），是 tracker 多算的快门加噪声。

## 6. P5（joint `e61d0012` → 落地 `eec0e836` → 收尾头 `37fc4fdb`）

### 6.1 五部分门（joint）与落地快门

| 部分 | joint 实测 |
|---|---|
| 1 接口纯度 | include closure 4 probes / 0 problems；pull `MG_Remote` 符号 0、split 610；G1 `.text` 10806611 → 10806611、27814 符号 0/0/0/0；P3a/P4a G5 对 `ff2994d9` byte-identical |
| 5 覆盖 / 生成器 | `gen_pipe` self-test 9/9；dirty-surface 27/27（`UseProgram` 一项明确 UNDECIDED）；field ownership 15/15；emitter/CSO 185/185；verify controls 4/4 |
| 3 行为 A/B | G2 差 0；G14 0 / +42；unit 1816×3、split 2038；Wire 58/58；`integration-gpu` pull 1128、push 1128、split-monolith 1149；split-inproc 普查 426 pass / 185 skip / 511 abort / 27 fail；`integration-split` 21/21（19 run、2 design-skip）；persistent arm 2/2，split `pmap=2160.00, mpr=1`，push `pmap=0.00, mpr=1` |
| 2 语义 / retrace | `integration-verify` 930/930 零 `Fatal{`；OpenRA inproc 2/2 双后端 SSIM 1.0；push retrace 79/79；verify retrace 79/79 armed |
| 4 设备 | 未跑（joint 禁止 adb）；见 §6.4 |

落地快门（`eec0e836`，ID-66）：split unit 2038；`integration-split` 22/22；push `integration-gpu` 1128；G1 0/0/0/0 `.text +0`。

### 6.2 退出门 E1–E6 与 27 个 wrong-answer

| 门 | 实测与"为什么会红" |
|---|---|
| E1 barrier | reduced path 19 pass / 2 skip / 0 fail；`MOBILEGL_IPC_VERB_BARRIER=0` 选中的 14/14 全 abort，每个私有日志有自己的 `Fatal{BarrierViolation, "<slot>"}` |
| E2 OpenRA | 2/2 SSIM 1.0；x2：丢 29 clears 仍 SSIM 1.0（被后续地形 draw 覆盖），改为丢 758 `DrawVbo` 后 SSIM 0.000036；r2 验证真实 CTest 顺序 baseline → pull-library control → 恢复原库 → draw-drop，并断言库身份 |
| E3 persistent map | (a) 收尾头 6 selected 中 4 pixel red、2 条 `TheMapLandsInTheArmItsLaneDeclares` 设计性 skip 被 j0 判红 → r2 精选默认 / SmallRing 下 4 条 pixel case 各带自己的 assertion + 私有诊断；(b) ID-42 emulated membership green → 3 red → green；(e) x2 证明 wrap，r2 证明真实 retirement wait（1 MiB 臂 `ringwraps=1 ringwaits=1`，8 MiB 对照同时红） |
| E4 field ownership | 生成器 15/15；strict lane 19 abort / 2 skip，首条 `Fatal{UnmigratedPipeInput, "GetTextureContextId@Clear"}`——BARRIER-PULLED 债务的响亮读法 |
| E5 honest inproc | 19 pass / 2 skip；v1-r3 的 `StagedShadowProductionTest` 证明 ensure 上传 server shadow（删保护会读 client A 变红），覆盖 unmapped 对象；coherent-map 的 server 侧绕过由 r1 #1 关闭 |
| E6 phase gate | 普查在 v1-r3 修掉六条过宽 whole-store 拒绝后 **432 / 185 / 505 UnmigratedVerb abort / 27 failed / 0 segfault**（1149 项，`~/w7/p5b-c0b-census-logs/results.json`） |

| 27 个 wrong-answer 家族 | 条 | 去向 |
|---|---:|---|
| `LayeredAttachmentShapeScenario` | 14 | P4b/P7 layered texture readback |
| packed depth/stencil `GetTexImage` | 3 | P4b/P7 texture-shadow readback |
| framebuffer `HandleRecycleScenario` | 5 | P4b/P7 readback（不是已证明的 handle recycle bug） |
| `PrimitivesGeneratedNoXfbScenario` | 3 | P7 query |
| `TextureParamsWithoutASamplerView` | 1 | P6 inspection forwarder |
| `P4aFinalFixScenario` FBO/RBO delete | 1 | 读回用 ReadPixels，完整像素因果未隔离；client-thread framebuffer death 归 r1 #2 |

### 6.3 R-10、逐帧 ledger 与内存

规范 ledger（`ProtocolSmokeTest` 钉住）：**SEG_CMD 8 MiB / SEG_STAGE 32 MiB / SEG_REPLY 16 MiB / SEG_EVENT 256 KiB**。`MOBILEGL_PIPE_STATS_PERIOD=1` 下 persistent-map 场景 `pmap=600.00 B/帧, mpr=1, rsp=35`（`rsp` 只是有 stamp 读点的下界）。inproc 进程峰值 RSS：server accept 时 11.5 MB、client teardown 141 MB（两角色共享一个进程，不是两个独立峰值）。

max record bytes（x2，`37fc4fdb`）：Triangle / persistent map / OpenRA 25 帧 / SmallRing 都是 **`maxrec=784 B` / `SetVertexAttribDefaults`**，默认 cap 4 MiB（占 0.019%）——这些负载不需要 chunking；oversized `DrawVbo` 尾的 unit 具名拒绝 `Fatal{RingOverrun, "DrawVbo"}`。

**内容分块落地（2026-09-20，`1e7c372e` buffer / `9469d48e` texture）**：`SEG_STAGE` 仍是"一条记录一个整 blob"的 arena，但会超出它的内容由发射侧按 stage chunk 预算 `MGPipeStageChunkBytes()` = `clamp(segment/4, 4096, segment)`（默认 32 MiB → 8 MiB）切成多条 `resource_subdata`——buffer 范围走查与纹理一级的整宽 slab（服务端 `StagedTextureStore::AdoptRun` 拼回整级）。§7.2 普查为容纳单次 128 MiB 上传而显式设的 `MOBILEGL_IPC_STAGE_MB=256` 因此不再是这两条路径的必需；未接入分片的 record 类型仍由 `Fatal{RingOverrun, "SEG_STAGE"}` 具名拒绝。上段的 `maxrec` 数字是分块前的测量，分块后的逐 blob 字节分布尚无新测量。

### 6.4 Redmi 四臂 A/B（split 未测）

会话 2026-09-16，runner `eec0e836`，三份 APK 源码头 joint `e61d0012`；臂 = pull APK / push APK / split APK + `MOBILEGL_TRANSPORT=inproc` / splitctl（split APK 不设 transport）。40 组前后 pin check 全 P/P，37.2–39.9 °C。**27 组完成 / 13 组失败：全部 10 组 split 在 benchmark 前中止**（improved-transparency 双后端 `DrawElementsInstancedBaseVertex`，其余 `DrawElements`），另 3 组是 rd12/DirectVulkan 的既有 `scudo` 崩溃。帧 p50 / p99（ms）：

| case / backend | pull | push | splitctl |
|---|---:|---:|---:|
| improved-transparency / DirectGLES | 10.535 / 25.415 | 12.106 / 26.973 | 12.235 / 27.131 |
| improved-transparency / DirectVulkan | 10.742 / 25.031 | 11.678 / 26.134 | 11.837 / 26.476 |
| fabric-sodium / DirectGLES | 8.314 / 9.573 | 8.311 / 9.701 | 8.303 / 9.486 |
| vanilla 1.21.4 / DirectVulkan | 1.043 / 2.293 | 1.159 / 2.404 | 1.179 / 2.426 |
| rd12 / DirectGLES | 7.999 / 22.089 | 11.105 / 24.865 | 11.261 / 24.771 |

splitctl 相对 push 的帧 p50 −0.1% – +1.6%（split build 的 monolith 臂与 push 同源等价）。barrier tax 在 P5 未测得，归 P5b（§7.4）。

### 6.5 收尾头停止点、r1 / r2

`~/w7/p5-final-gate.log`（`37fc4fdb`）：构建 4 × rc 0；Part 1 G1 27,814 符号 0/0/0/0、`.text +0`、两条 G5 byte-identical；Part 5 完成；Part 3 unit 1817×3 / split 2086、Wire 58、pull/push GPU 1128、split-monolith 1149 零失败、reduced 22 零失败、broad lane 1149 selected（532 non-success，含 abort）；E1 14 red；**E3(a) 因 2 条设计性 skip 被 j0-v3 的逐选项拒绝判失败，Part 2 / 4 未到达**。j0 的 skip 拒绝是对的（不能放宽），选择器才是错的。

收官审查（`p5-close-codex-review.md`，只读）：1 blocker / 10 major / 2 minor（ID-73）。r1（核心四项）：apply-role 不得进入 persistent-map producer；framebuffer death 转 apply mailbox；`PACK_SWAP_BYTES` 的 component / packed-word swap；command-ring retirement 等待与重试。r2（`37fc4fdb..d50183cb`，CI / 控制 / 跑器九项）：broad debt lane 记录后继续硬门、全 skip 则 baseline 失败；E2 库身份 SHA256 恢复；E3(a) 精选 4 pixel case；`ringwaits` 只计真正阻塞的分配；split unit 强制 `MOBILEGL_ITEST_REQUIRE_GPU=1`；普查跑器零执行拒绝、launch 前清私有日志；G5 pin 自测驱动生产选择路径；c1f 改用 ID-67 用例。r2 包门：split unit 2087、reduced 22、push 1128 零失败；E1 14 own reds、E3(a) 4 own reds；smoke 16 core + 12 private；census 432/185/505/27。r2 没有新跑 pull G1（不影响 pull production code）。

过程记录（不计入实现产量）：wave-1 跨族复审十项全部经独立 perturbation 确认（ring 空 wrap 算术、reply 几何差 16 B、blob 校验接受任意段、pad-bit 控制没发送该 bit、三个 CI 对照接受任意非零退出……）；后续 v1 / c1 各轮的 review 计数在 ID-52/58/62/64；本地 `rereview` 身份污染的 81 + 58 个提交经 parent/env rewrite 接回 GitHub `ff2994d9`（ID-40），此后每阶段首个 commit 前先跑 `git var GIT_COMMITTER_IDENT`。这些数字是流程在 P5 尾声改变（ID-66）的原因。

## 7. P5b（主机门 `348d22a4`，设备源头 `82683d4a`）

### 7.1 迁移与包证据

`348d22a4` 含 d1 / i1 / t2 / f1、r1 / r2、五槽 sync、具名 blit 与 GLES mip storage；发射表 A=2 / B=54 / C=15（目录 76 条，槽数与 opcode 数不是同一统计量）。

| 包 | 已落地行为与证据边界 |
|---|---|
| d1 | 19 个索引 / 实例 / multi-draw / indirect 槽经 `draw_vbo` 过线；独立包普查 77 个 Minecraft 后端用例 28 passed、49 first blockers，全部越过原 draw 首阻塞（默认 stage 32 MiB） |
| i1 | 七个 image / compute / barrier / copy-image / storage-block 槽；原首阻塞 239 归零；CopyImage 的 client-shadow 镜像被跳过，`GetTexImage` 前端回退仍可能读旧 shadow |
| t2 | 六个 XFB / 曲面细分槽；原首阻塞实测 140（`BeginTransformFeedback` 95 + `PatchParameteri` 43 + `BindTransformFeedback` 2）；包内基线 490 / 191 / 390 / 78，原 432 passes 全保留 |
| f1 | 11 个 clear / copy / mip 槽；原 34 个首阻塞归零；bound named-clear 接线，unbound 仍具名拒绝 |
| r1 / r2 | P5 收官 #1–13；r1 定向 10 passed / 0 skipped；client-only push suppression 两个像素控制变红 |
| 具名 blit | `BlitNamedFramebuffer` 经 scoped read/draw binding 发布，退出恢复公开绑定；两后端四个 split 像素 entry 通过 + 15 unit |
| GLES mip storage | server 只验证 applier descriptor 的 Levels/extent；三个 mip 像素控制 3/3；iris-BSL GLES 单例 SSIM 0.997496（默认 stage 32 MiB）；registry 只 Find 不 mint（barrier 债）；RGB 三通道 CPU fallback 保持 Fatal |
| sync | 五条现有 opcode 接线，wire 只传句柄，native fence 归 apply 线程；原 LOCAL guard 使 trace 的 `FenceSync` 没真正执行，所以 d1 旧普查里没有可扣除的 FenceSync 首阻塞数 |

包报告：`~/w7/notes/p5b/p5b-results/{d1-codex-v1,i1-v1,t2-codex-v1,f1-v1,r1-codex-v1,r2-v1,blit-codex-v1,mip-codex-v1,sync-codex-v1}.md`。

### 7.2 主机收尾门与合并普查（`348d22a4`，`~/w7/p5b-final-host-348d22a4/`）

| 车道 | Selected | Passed | Skipped | Failed |
|---|---:|---:|---:|---:|
| unit-linux / unit-push / unit-verify | 1817 × 3 | 1500 / 1782 / 1795 | 317 / 35 / 22 | 0 |
| unit-split | 2132 | 2122 | 10 | 0 |
| gpu-linux-monolith / gpu-push-monolith / gpu-split-monolith | 1148 / 1148 / 1178 | 895 / 953 / 953 | 253 / 195 / 225 | 0 |
| split（`integration-split`，inproc） | 107 | 105 | 2 | 0 |
| verify / audit | 950 / 10 | 804 / 9 | 146 / 1 | 0 |

G1 27,814 符号 0/0/0/0、`.text` 10,806,611 → 10,806,611；G5 两族 + pin 自测；生成器 / 纯度检查；G2 / G14 rc=0；E1 / E3(a)（精选 4 case）rc=0；E2 OpenRA 2/2、draw-drop SSIM 0.000036 / 758 records、pull-library 拒绝与库 SHA 恢复一致；smoke 16 core + 12 private。retrace-push **79/79**；retrace-verify 在 WSL 重启前完成 4/79，其余 75 条以 SHA-256 钉住的同一 verify 库串行续跑，合并 **79/79**；对账无缺步、无非零退出（`reconciliation-20260916.md`），`complete=true`、exit 0。一次 shell 异常（追加 runner 行触发 `-test-dir: command not found`）保留在原始输出，不冒充测试失败。

合并 inproc 普查（`~/w7/p5b-final-census-348d22a4/`）：integration lane 显式 32 MiB，**1267 selected = 811 passed / 203 skipped / 62 aborted / 191 failed**；旧 1149 名全部保留、新增 118；旧 432 passes 全保留、零回退；旧 505 abort → 265 passed / 18 skipped / 58 aborted / 164 failed；旧 27 failed → 1 passed / 26 failed。完整 trace 显式 256 MiB（容纳单次 128 MiB 上传，不是默认容量修复）：**79 = 72 passed / 6 aborted / 1 failed**（主跑止于 73/79，三轮续跑补齐；`create-indirect` DirectVulkan 在 llvmpipe 上内存膨胀 >60 GiB RSS 被守护杀死，两次复现，记 failed）。7 条未过项首阻塞：`rd12` DirectGLES `Fatal{InitialBytesNotCarried,"resource_respecify"}`、DirectVulkan `Fatal{BarrierTimeout,"Present"}`；`iris-photon`、`iris-derivative`、`create-indirect` 三个 DirectGLES `Fatal{UnmigratedEmulation,"texture-remint-pull"}`；`iris-bsl-esc-menu-854` DirectGLES `InitialBytesNotCarried`；`create-indirect` DirectVulkan 内存守护。通过项含 `improved-transparency-minecraft-26.3` DirectGLES SSIM 1.0 / DirectVulkan 0.999914、`iris-iterationrp` DirectVulkan 0.995833、`iris-bsl-esc-menu-854` DirectVulkan 0.998402。逐 trace 见 `joint-codex-v1.md` 的 census 块与 `identity.json` / `counts.json` / `trace-transitions.json`。

### 7.3 唯一收官审查与定向修复

审查 `p5b-close-codex-review.md`（`348d22a4`，diff base `37fc4fdb`，只读）：**0 blocker / 2 major / 1 minor**。

| 项 | 问题 | 处置 |
|---|---|---|
| Major 1 | user-index span 在段内但 Size 可能短于 Count × IndexSize | `a021e3cc`：encoder / decoder / sink 共用 shape/extent gate（单 range、宽度 1/2/4、`Uint64(Count) × IndexSize ≤ Size`），三端短 span 均拒绝、段末 exact-fit 通过；定向 9/9 |
| Major 2 | `ClientWaitSync` 保留 64-bit timeout 但 applied/reply 等待一律 30 s | `82683d4a`：预算 = ceil(timeout ns / 1e6) + 30,000 ms，有限 chunk；普通容量等待 / `FenceWaitServer` 仍 30 s，shutdown 可唤醒。**该 30 s 常量后来在 CI 修复轮改成 120 s（`68b55ea3`），原因见本表下注** |
| Minor 3 | ReadPixels exact-size reply 的未知 status 穿过 production helper | `82683d4a`：tight 与 bounce 路径拒绝 status=3，`Fatal{ReplyStatusInvalid}` |

定向 `RemoteClientTest` 7/7（0 ns、1 ns、60 s、UINT64_MAX 预算，`FenceWaitServer`、shutdown、未知状态）。合并后 quickgate 里 split 单元车道仅有的 2 个失败（`PipeWireCodecTest.UserIndexSpan*`）是测试日志捕获缺陷（库按进程截断日志，fork 子进程增量读读空），`7cb29d46` 修复后 3/3。本阶段不再做第二轮全门或审查；原始全门源头仍为 `348d22a4`，三项修复只以定向证据补齐。

> 上表 Major 2 的 30 s 是 `82683d4a` 当时的值，保留为历史记录。**当前值是 120 s**（`68b55ea3`，`ClientSession.cpp` 的 `kBarrierTimeoutMs`），因为 lavapipe 在 texture-handle 注册（`vkCreateImageView` → `llvmpipe_register_texture`）上可合法阻塞 apply 线程 29.3–39.7 s，30 s 的看门狗会把合法慢路径间歇判红；详因见 `ARCHITECTURE.md` §17.1。`RemoteClientControls.FenceWaitBudgetHonorsGlTimeoutAndFiniteTransportChunks` 的期望值（30000/30001/90000）已同步改为 120000/120001/180000。

### 7.4 Redmi 出口与四臂 A/B（`82683d4a`，APK `p5bcodex2`）

Redmi `2f7cbe2e`，证据根 `MobileGL/.trace-work/p5b-redmi/p5bcodex2/2f7cbe2e/`；全部组合显式 `MOBILEGL_IPC_STAGE_MB=256`。**钉频口径变更**：2026-09-11 的厂商 GPU 上限（1050 MHz）已消失，本次 deterministic pin 为 **1100 MHz**；与 1050 MHz 时代的活动不可比钟频，只有同场四臂配对可比。

**正确性 8/8**：`improved-transparency-minecraft-26.3`、`minecraft-1.21.4-in-world`（GLES SSIM 0.999995）、`minecraft-1.21.4-fabric-sodium-in-world`、`minecraft-1.21.4-fabric-iris-bsl-in-world` × 双后端，inproc split 臂，逐组钉频取证 + PNG / SSIM 阈值 + inproc + apply 线程 + 正 wire 记录三证，skip 即失败。**四条 A/B trace 首次在 Redmi 上以独立 apply 线程渲染——P5b 出口判据达成。**

**四臂 A/B（barrier tax 首测）**：32 组中 24 组带 200 帧尾验证全绿；三次重复取 wall-time 均值最低者、尾 200 帧（`ab-tables.md`）。逐线程 CPU p50：

| trace | 后端 | push−pull | **barrier tax（split−push）** | barrier tax 帧 p50 |
|---|---|---:|---:|---:|
| improved-transparency-26.3 | DirectGLES | +15.6% | **+10.3%** | +0.1% |
| improved-transparency-26.3 | DirectVulkan | +8.8% | **+13.1%** | +13.3% |
| minecraft-1.21.4-in-world | DirectGLES | +21.4% | **+18.2%** | +0.2% |
| minecraft-1.21.4-in-world | DirectVulkan | +11.8% | **+17.9%** | +19.7% |
| minecraft-1.21.4-fabric-sodium | DirectGLES | +10.9% | **+5.9%** | −0.1% |
| minecraft-1.21.4-fabric-sodium | DirectVulkan | +7.6% | **+8.3%** | +4.3% |

splitctl−push 全部在 ±1.6% 内（增量来自 inproc 传输与 barrier，不是 APK / 构建差）；p99 同向放大（sodium DirectVulkan 帧 p99 +107.7% 为离群尾帧）。inproc 臂 peak RSS 389 MiB – 2.5 GiB，全角色映射 560.5 MiB（256 MiB × 角色视图是虚拟映射容量，不是 RSS 增量）。

**fixture 受限的 8 组**：`iris-bsl-in-world` 只有 123 个 benchmark 帧，低于 200 帧尾规则，四臂 × 双后端验证全部失败（pull 同失败，是 fixture 上限不是回归）；123 帧完整序列按同法折算成补充表 `ab-tables-bsl-123frame-supplement.md`：barrier tax CPU p50 DirectGLES **+7.1%**、DirectVulkan **+9.1%**，不混入上表。性能仍只记录、不作阻塞门。

## 8. P5c（`11ac3de6..b88e8487`，同日收官）

出口门（E-P5c）实测：

| 门 | 结果 |
|---|---|
| 守卫开启下 unit | **2187/2187**（strict `MOBILEGL_IPC_STRICT_ERRORS=1` 下同绿） |
| 守卫开启下 `integration-split` | **111/111**（107 + ct 的 4 条 CtWireScenario，全真跑） |
| 每层守卫 red-once | layer-1（MipmapStorage 守卫短路 → `TextureMapMipmapDataFromTheApplyThreadIsFatalByName` 红）与 layer-2（`RefusePipeInputsTouchWhileApplierOwnsIt` 短路 → `ClientPipeInputsFillWhileTheApplierOwnsItIsFatalByName` 红）各红一次并复原；tx 的采纳改回丢指针 → 恰 `StagedTextureProductionTest` 红；ev 的 writeback 改回裸指针 → `AtomicCounterScenario.SubDataAfterDispatchSurvivesAnImmediateReadback` 按名红；ct 的发射短路 → Ct 4/4 按名红；rv 的发射门短路 → 7 条按名红 |
| 纹理 `0xDD` audit | bsl in-world（100000 调用）与 iris-complementary in-world 全程零 Fatal |
| `SEG_EVENT` 往返 | 三种事件 + `kEventGlError` 各有单元往返；排空点 `eventDropped == 0` |
| `rsp` 分类 | 值类 = 0（rv 的 FieldOwnershipTest 钉住）；按帧实测：bsl 948.5/帧（38.7/draw，123 帧）、complementary 1591.9/帧（151 帧）、iterationrp 286.2/帧（108 帧后止于既有具名 `texture-remint-pull`）；残留全为对象类 15 行 |
| 普查（integration-gpu @ inproc，对同机 11ac3de6 基线逐名比对） | 基线 253 红 / 收官头 423 红；183 条 newly-failing 中 11 条为三个真回归（默认 FBO 格式时序、大 writeback 溢出、XFB scatter 读前端）——已修并回归绿；其余 172 条全部带 Fatal 名证据归类为设计红（传输下旧臂具名拒绝的对照车道 14 条 + Magma P7 未迁移面 156 条）与 2 条 DirectVulkan CtWire（改按名 skip：Magma 无 object_death 生产者，P7） |
| G1（pull 符号恒等，`11ac3de6` ↔ 收官头，CI 同款 sym 构建） | 0 增 / 0 删 / **3 认定 resize** / 0 重命名，`.text` −16 B：`SwapchainObject::Create`（ev 表面事件化）、`CopyTexSubImage2D`（hd 传输臂）、`ScopedRestartIndexSubstitution`（server-shadow 臂）；pull 构建零 `MG_Remote` 符号 |
| G5（p3a / p4a 保护区） | 字节一致 |
| 生成器 / 卫生门 | 双生成器 --check/--self-test 绿；doc 引用、include 闭包、dirty-surface 绿 |
| Redmi 四臂复测 | **未做**（记录项，需设备窗口；barrier tax 重测随之） |
| 79 trace 普查 | **未重跑**（全集语料不在本机；P5b 的 72/6/1 仍以其头为准） |

审计的 59 处直接访问的终态：纹理纹素 / 形状 / 脏区改读 server staged shadow（tx）；反向通道四条事件（ev）；句柄解析全部按记录（hd）；`applier_reset` / `object_death` 上 wire（ct）；值类残余读清零（rv）；双层角色守卫 + `InBarrierWait`（gt）。**未到期的具名债**：`MGPipeReverseAnnouncementScope`（绑定记录 P4b 才发射的 ensure/通告族）与 `MGPipeFrontendKeyedRegistryScope`（G6 前端键 twin registry，P3b/P4b 重键）两个 scope 内的只读探测，以及对象类 15 行 BARRIER-PULLED——全部具名、可 grep、有退役阶段。

## 9. P5d（`cb06538c`、`56a77348`、`1f8de61b`；Redmi `2f7cbe2e`，FCL + Minecraft 26.3-rc-3 世界 "test"，VD12，DirectGLES）

协议：CPU 定频（大核 1958400 / 小核 1555200，`pin_device.sh`）、风扇二档、世界内 20 s 静置后取 30 s 窗口；fps = `MGPipe stats:` 行的帧数 / 时间（应用侧 swap 计数），逐线程 CPU = `/proc/<pid>/task/*/stat` 的 utime+stime 差 / 帧数。两个已知的不受控变量：GPU 由厂商守护进程在每次启动时把 `min_pwrlevel` 打回 12（inproc 各臂 GPU 都在 342 MHz 空转、CPU 受限，不受影响；monolith 或受 GPU 或 120 Hz vsync 封顶）；内核对 app 线程忽略 `sched_setaffinity`，client GL 线程常落在中核（cpu2/4/5）、apply 线程常驻大核 cpu7。因此配对比较以 **client 线程 CPU ms/帧** 为主指标。

| 构建 | 臂 | fps p50 | client ms/帧（核） | apply ms/帧（核） | 备注 |
|---|---|---|---|---|---|
| 二轮头 `56a77348` | inproc | 64.3 | 15.03 (cpu5) | 12.42 (cpu7) | 起点；`SPIN_US=2000` 68-73（三次），`=10000` 63，`SERVER_AFFINITY=off` 65.9 |
| 二轮头 `56a77348` | monolith | 116.5 | 5.85 (cpu7) | — | 75% 忙，vsync 120 Hz 封顶 |
| 三轮审查前构建 | inproc | 114.0 | 8.68 (cpu4) | 6.93 (cpu7) | `SPIN_US=2000` 112.3（不再有收益） |
| 三轮审查前构建 | monolith | 115.7 | 5.70 (cpu5) | — | 封顶 |
| 三轮（本提交） | inproc | 103-106 | 9.2 | 7.0-7.4 | 交错会话，见下 |
| 三轮（本提交） | monolith | 115（封顶）/ 206（一次未封顶） | 5.0-6.2 (cpu7) | — | 三次：115.3 / 115.1（120 Hz 封顶）与 205.8（未封顶） |

交错会话（三轮头，post-review 构建，同一次定频、同一天）：

| 臂（顺序） | fps p50 (min / max) | client ms/帧（核） | apply ms/帧（核） | 备注 |
|---|---|---|---|---|
| mono1 | 205.8 (110 / 280) | 5.0 (cpu7) | — | 未封顶的一次；窗口内 110→280 摆动 |
| inproc1 | 106.1 (91 / 112) | 9.24 (cpu4) | 7.0 (cpu7) | |
| mono2 | 115.3 (108 / 117) | 6.2 (cpu7) | — | 120 Hz 封顶 |
| inproc2 | 103.3 (82 / 111) | 9.2 (cpu5) | 7.4 (cpu7) | |
| inproc3 | 101.9 (83 / 109) | 9.1 (cpu5) | 7.3 (cpu7) | |
| mono3 | 115.1 (109 / 117) | 6.1 (cpu7) | — | 封顶 |

审查前构建（同一天早些、机身 50.8 °C 起）inproc 114.0 / client 8.68 ms（cpu4）/ apply 6.93；post-review 构建 pmap 线流量与之逐字节相同（364.7 KB/帧），差在运行间漂移（机身 53-54 °C 起、中核放置）之内。

`wait[]` 计数（三轮加入 stats 行）：审查前构建 inproc 30 s 窗口累计 srv=7.56M / srvpark=76k、cli=6.70M / clipark=17k；`SPIN_US=2000` 下 park 各降两个量级、fps 不变——park 已不是主项。持久映射线流量 pmap ≈ 0.36 MB/帧（二轮 0.23，三轮多了首推与脏页排空）。

## 10. P5e（wave 1 `f6cfcbd3` → wave 2 `44f91c74` → fix1 `66621767` → ID-110 `3cc4e1ec`）

P5e 尚未收官（`kMGPipeP5eRunAheadReady` 仍为 false），本节记的是**设备验证一轮**，问题是"八包合并后两条传输臂还能不能正常跑游戏"，不是配对性能。

### 主机门（WSL `~/w7/p5e-int`，split flavour，头 `3cc4e1ec`）

| 门 | `44f91c74` | `3cc4e1ec` |
|---|---|---|
| unit | 2250/2250 | **2250/2250** |
| `integration-split` | 113/113 | **116/116** |
| `integration-gpu`（两条运行时臂，ID-109 起为常设步） | **1157/1294**（137 SEGFAULT / 38 scenario） | **1294/1294** |
| `integration-split-strict` | 7/116 | 7/116（预期红） |

`integration-gpu` 那一列是本阶段最重要的一个数：`44f91c74` 的 137 个 SEGFAULT 在当时**没有任何门禁步骤会看到**——`integration-split` 的每一条都导出 `MOBILEGL_TRANSPORT=inproc`，而 push 构建有两条服务端臂。详见 `CURRENT_STAGE_PROGRESS.md` §2.5。

### 设备（Redmi `2f7cbe2e`，FCL fordebug + Minecraft 26.3-rc-3 世界 "test"，VD12，DirectGLES；进世界后 60 s 采样）

| 臂 | 进世界 | fps | draws/帧 | 进程 | Fatal / crash buffer |
|---|---|---|---|---|---|
| monolith（`44f91c74`，修复前） | — | — | — | **65 s 后进程消失** | SIGSEGV，栈见 §2.5 |
| monolith（`3cc4e1ec`） | 31 s | 263.5 | 845 | 存活 | 无 |
| inproc（`3cc4e1ec`） | 31 s | 136.1 | 853 | 存活 | 无 |
| inproc 复跑（`3cc4e1ec`） | 32 s | 140.3 | 852 | 存活 | 无 |

monolith 这一次未被 120 Hz vsync 封顶（与 §9 的 205.8 那次同类），两臂 fps 不可直接相比。inproc 的 136-140 与修复前记录的 140.0 / 144.9 同档：fix1 改的那行两臂都读，split 臂没有被拖慢。

### 测量纪律（本轮与上一轮踩到的坑，全部是污染而非代码问题）

- `adb install` 的收尾会在一两分钟后杀掉正在跑的同包进程（`am_kill … due to installPackageLI`）：装包与测量之间必须等到 `dex2oat` 退出。
- 派给外部 CLI 的任务，其进程会活过 harness 的完成通知；**派出后不得自己再跑同一件事**，否则两套实验同时对一台手机 force-stop / 清 logcat，得出假失败。
- 不得在脚本正被 bash 执行时修改它（边读边执行）；要改先冻结副本。
- 游戏内 F3 的 `GIT@` 戳记在增量构建下是旧的，**判断库版本要看 APK 里 `.so` 的符号 / 字符串**，不看戳记。

## 11. P5e wave 3 — run-ahead 武装后的设备矩阵（头 `25fba0d5`）

Redmi `2f7cbe2e`，FCL fordebug + Minecraft 26.3-rc-3 世界 `test`，VD12，DirectGLES，~850 draws/帧。
**CPU 定频**（见下），GPU 钉 pwrlevel 0 且每臂重新断言，风扇开，进世界后 20 s 稳定再取 30 s 窗口，臂交错。

### 定频，以及为什么这一节必须先讲它

第一轮矩阵没有定频，于是 **monolith 自己那一臂**的逐帧 CPU 在两次运行之间从 4.43 ms 跳到 7.07 ms，
而慢的那次**温度更低**（53.5 °C vs 58.9 °C）——不是热降频，是 walt governor 把 policy0 从 2745 MHz
拉到 748 MHz。未定频的逐线程 CPU ms/帧**不能跨臂比较**，那一轮矩阵作废。

`tools/device_bench/pin_device.sh` 不认识本机 serial，并**明确拒绝猜**（"pin path 因 SoC 而异，猜错会静默失败"）——
这是对的，也正是它没有静默半应用的原因。按本机实测节点另写 `pin_redmi.sh`：写序 min→硬件底、max→目标、
min→目标（顺序要紧，否则按 stock 相对目标的位置会半应用），并**回读 `scaling_cur_freq` 确认**，跑完再 check 一次。

### 结果（每臂两次，离散度 < 2%）

定频 little 1555200 / big 1958400：

| 臂 | fps p50 | fps max | client ms/帧 | apply ms/帧 |
|---|---|---|---|---|
| monolith | 115.1 | 117.0 | 6.19 | — |
| inproc run-ahead | 117.6 / 117.9 | 118.3 / 118.6 | 6.71 / 6.63 | 4.30 / 4.32 |
| inproc lockstep (`MOBILEGL_IPC_RUN_AHEAD=0`) | 111.1 / 110.7 | 114.9 / 115.1 | 8.27 / 8.09 | 6.44 / 6.41 |

定频 little 1996800 / big 1958400（更高会被厂商限幅器夹住）：

| 臂 | fps p50 | fps max | client ms/帧 | apply ms/帧 |
|---|---|---|---|---|
| monolith | 115.6 / 115.4 | 117.2 / 117.5 | 6.45 / 6.25 | — |
| inproc run-ahead | 117.6 / 118.0 | 118.5 / 118.6 | 6.92 / 6.87 | 4.43 / 4.35 |
| inproc lockstep | 111.6 | 113.7 | 8.16 | 6.53 |

### 读法

- **配对 A/B**：`MOBILEGL_IPC_RUN_AHEAD=0` 与默认是同一构建、同一份记录、同一定频，唯一差别是客户端等不等
  （ID-114：**不得**用 `VERB_BARRIER=0`，它是 `RunAheadArmed()` 的第一个合取项，会顺手关掉 run-ahead，
  而且实测几秒内即 `Fatal{UnmigratedPipeInput, "IsCapabilityEnabled@ClientWaitSync"}`）。
  差值：client **−18%**、apply **−33%**、fps p50 **+6%**。
- **两个定频档下 monolith 与 run-ahead 都撞到 120 Hz 面板上限**（max 117-118.6），**只有 lockstep 撞不到**
  （max 113.7-115.1）。所以"inproc 是否追平 monolith"在本机真实负载上的答案是**追平了**；阶段开始时是
  **0.62-0.74 倍**（见 §10）。
- 逐帧 client CPU：run-ahead 6.9 / monolith 6.3，多约 **8%**，两者都在上限之下有余量。
- **未回答**：面板上限之上谁更快。本机定不住更高频率；一次未定频的高频窗口给出 monolith p50 201 /
  run-ahead p50 181（约 1.10 倍），但那次不可配对，只作方向性提示。需要关 vsync 或换上限更高的设备。

### 侧写（`25fba0d5` 之前，`7c6f6886`，说明 run-ahead 为什么值得做）

symbolized `simpleperf cpu-cycles`，客户端 GL 线程：`SessionProducer::WaitForAppliedOrEventBacklog`
**31.58% 自身时间**，其后第二名只有 3.72%。设备计数器同期显示客户端每帧进入 doorbell 等待约 **916 次**
（对 ~849 次 draw），其中 99.8% 靠自旋解决——`MOBILEGL_IPC_SPIN_US=0` 让每次等待都 park，帧率塌到 20.9。
所以代价是**会合本身**，不是自旋参数，这也是为什么修法是"不再每次 draw 会合"而不是"把每次会合做便宜"。

### 11.1 渲染距离 32 的对照（同头 `25fba0d5`，~3550 draws/帧）

§11 的矩阵在 VD12 下跑，而两条臂都撞到了 120 Hz 面板上限，所以那一节回答的是"能不能撞到上限"，
不是"谁更快"。把渲染距离拉到 **32**（`options.txt` 的 `renderDistance`，备份为 `.p5e-backup`，测完已还原）
负载涨到 **~3550 draws/帧**（VD12 的 4.2 倍），**两条臂都远离任何上限**（游戏自身 `enableVsync:false`、
`maxFps:260`，实测 max 只有 37-71），这才是可比的一轮。

条件：CPU 定频 little 1555200 / big 1958400（跑前跑后各 check 一次，均 PINNED），GPU 钉 pwrlevel 0，
风扇开，进世界后**静置 75-150 s**再取 40 s 窗口——VD32 的区块流式加载远比 VD12 长，沿用 20 s 静置会把
加载期算进稳态。臂交错。

| 臂 | n | fps p50 中位数 | p50 范围 | client ms/帧 中位数 | apply ms/帧 |
|---|---|---|---|---|---|
| monolith | 5 | **58.5** | 56.3 – 67.9 | 16.19 | — |
| inproc **run-ahead** | 5 | **59.9** | 56.8 – 61.1 | **15.72** | 11.1 – 12.1 |
| inproc lockstep（`RUN_AHEAD=0`） | 2 | 35.5 | 34.5 – 36.5 | 26.17 | 19.7 – 20.5 |

**结论：在 VD32 下 inproc 与 monolith 齐平**，中位数上 run-ahead 甚至略微领先（59.9 vs 58.5，
client CPU 15.72 vs 16.19），差值远在 monolith 自身的离散度之内——**monolith 五次的范围是 56.3-67.9
（±10%），run-ahead 是 56.8-61.1（±4%）**，也就是说 split 臂不但追平，还更稳。`mono1` 的 67.9 是全场唯一
的离群值（装包后第一次运行）。

**run-ahead 相对它取代的 lockstep：p50 +69%、client CPU −40%、apply CPU −43%。**
负载越重它越值钱，这是预期内的——会合次数随 draw 数走，VD12 是每帧约 900 次，VD32 约 3600 次。

两条臂**都是 client 线程打满**（run-ahead 15.72 ms CPU / 16.7 ms 帧 ≈ 94%；monolith 同理），所以这一列
是各自真正的瓶颈，可以直接比。

**下一个机会，不是缺陷**：apply 线程只有 11.1-12.1 ms，client 有 15.7 ms——**两侧不平衡**。把工作从 client
挪到 apply 会直接降低瓶颈。这在 lockstep 下毫无意义（两侧串行相加），run-ahead 之后才成立，是本阶段
解锁出来的新优化方向，留给后续阶段。
