# P0 实测

> 每张表都写明设备、提交与命令，以便复现。设备：`35d0befa` = Xiaomi 24129PN74C，Adreno 830，Android 16；`3B159D009VZ00000` = Oppo PLG110，Mali，Android 16（ColorOS）。设备运行日期 2026-09-05。设备锁协议照旧。

## 1. Spike A — 从应用自身进程 exec 第二个原生可执行文件

问题：Android 上能否把 server 以 `lib*.so` 打进 APK，并从应用自己的 `untrusted_app` 域 `fork`+`execve` 它（`adb run-as` 跑在别的域，证明不了）。

| 设备 | 结果 |
|---|---|
| Adreno 830 | **OK**。父进程 `u:r:untrusted_app:s0:c173,c257,c512,c768` `fork`+`execve` `<nativeLibraryDir>/libMobileGLServer.so` → 子进程 pid 31348，exit 0；子进程 SELinux `u:r:untrusted_app:s0:c173,c257,c512,c768`（同域同 category）；marker 文件、stdout 捕获、报告全在；`execErrno=0`；窗口内**零 avc denial** |
| Mali | **OK**，同形：子进程 pid 28433，exit 0，`execErrno=0`，`u:r:untrusted_app:s0:c94,c257,c512,c768`，零 avc denial |

主机侧已证的三条（随 `8a239177`）：AGP 会把改名成 `lib*.so` 的 `add_executable` 打进 `lib/arm64-v8a/`，前提是把 `RUNTIME_OUTPUT_DIRECTORY` 重定向到 `CMAKE_LIBRARY_OUTPUT_DIRECTORY`；`posix_spawn` 在 minSdk 26 不可用（bionic API 28 起），出货臂是 `fork`+`execve`；应用进程 stdout/stderr 是 `/dev/null`，子进程用 marker 文件证明自己活过。

- 代码：`tools/spikes/server_stub/main.cpp`（stub：打印并写 marker 自己的 pid/uid/SELinux 上下文）、`android-plugin/app/src/trace/cpp/spawn_spike.cpp`（`RunSpawnSpike`）、`CMakeLists.txt:784-808`（`MOBILEGL_BUILD_SERVER_SPIKE`）。
- APK：`p0-spike-a-android/trace-debug-spike-on.apk`（在 `30d7595b` 构建，与 `7ef7c7e5` 源码相同）。
- ColorOS 陷阱：首次 `adb install` 一个未安装的包会卡在 `com.oplus.appdetail InstallGuideActivity` 确认页，直到点"继续安装"（1272×2772 面板上 `input tap 353 2349`）；同签名重装静默通过。另一台设备上一个外来签名的 trace APK（versionCode 26080769）会让 `install -r` 报 `INSTALL_FAILED_UPDATE_INCOMPATIBLE`，需先卸载。
- 42-device.sh 的 env 透传 A/B 腿在该 ROM 上跑不了（`run-as sh -c 'cat > files/…'` 被拒）；透传由下面的 stats 基线端到端证明（`--env MOBILEGL_PIPE_STATS=1` 必须在 `mobilegl.log` 里产生 `MGPipe stats` 行）。

## 2. Spike B — 跨进程外部内存分档

问题：`AcquirePersistentMap` 背后的内存能否共享给另一个进程并在那里映射，两个后端各走哪条路。探针 `tools/spikes/extmem_probe/`（`39f982e6` 源码，arm64，`adb shell` = `u:r:shell:s0` 域），4 MiB payload，64 KiB 同判决。每一行都取一次真 GPU 访问（`vkCmdCopyBuffer` + `vkCmdFillBuffer` + host-read barrier）并两侧字节校验才算 OK。

| 路线 | Adreno 830 | Mali |
|---|---|---|
| T1-opaque-fd（server 导出 `VkDeviceMemory` fd，client 裸 `mmap` + 导入） | **OK** 完整往返含 GPU 访问（`/dmabuf:system`，dedicatedOnly=1） | UNSUPPORTED（`vkCreateBuffer(external)=VK_ERROR_INVALID_EXTERNAL_HANDLE`，advertisedExportable=0） |
| T1-dma-buf | UNSUPPORTED（`VK_EXT_external_memory_dma_buf` 缺） | UNSUPPORTED |
| T1-gles-memobj-fd（`GL_EXT_memory_object_fd` 导入导出的 fd） | **FAIL**：导入 + `glBufferStorageMemEXT` 接受（`GL_NO_ERROR`）但每次 `glMapBufferRange` → `GL_INVALID_OPERATION`（persistent 与 plain 都是）；`GL_DEVICE_UUID` 不可读 | UNSUPPORTED（扩展字符串缺，入口点可解析） |
| **T0-ahb-blob-transfer**（client 分配 `AHardwareBuffer` BLOB → socket 交接 → server Vulkan 导入 + GL 导入） | **OK** 全链：cpu-lock、vk-import+map、GPU copy/fill、GL map persistent+coherent、写回 client 全部字节校验 | **OK** 全链，判决相同（glPersistentCoherent=1，gpuRan=1） |
| T3-external-memory-host（`VK_EXT_external_memory_host`） | UNSUPPORTED（扩展缺） | PARTIAL：导入 + map 往返，但 **GPU 写对宿主映射不可见**（只读档） |
| T3-memfd-cross-process / client-memfd-server-import | UNSUPPORTED | OK / PARTIAL（同样的 GPU 只读 caveat） |

**P11 的分档决定**：唯一在两台设备、两个后端上都是完整读写的档是 **T0**——client 分配 `AHardwareBuffer` BLOB，server 以 `VK_ANDROID_external_memory_android_hardware_buffer`（Magma）或 `EGL_ANDROID_get_native_client_buffer` + `glBufferStorageExternalEXT`（Espryt）导入，两侧 persistent+coherent 映射。Adreno 另有 T1（Vulkan 路径）；Mali 无任何 server 导出路线，host-pointer 导入只读。Caveat：运行域是 `shell` 不是 `untrusted_app`；AHB 的 socket 交接是每个与 SurfaceFlinger 共享 buffer 的应用都在走的路径，域风险在 memfd/opaque-fd 腿上。

复现：

```sh
ANDROID_NDK=$HOME/android-sdk/ndk/27.3.13750724 tools/spikes/extmem_probe/build_android.sh /tmp/extmem-build
S=<serial>; adb -s $S push /tmp/extmem-build/extmem_probe /data/local/tmp/extmem_probe \
  && adb -s $S shell "chmod 755 /data/local/tmp/extmem_probe && /data/local/tmp/extmem_probe; echo EXIT=\$?" | tee out-$S.txt
```

判决语义（OK / PARTIAL / FAIL / UNSUPPORTED）与逐腿 trace 格式见 `tools/spikes/extmem_probe/README.md`。主机构建（lavapipe）用来证明探针本身报得对：T1/T3 在 lavapipe 上全 OK；T1-gles 在 llvmpipe 上 `GL_OUT_OF_MEMORY` 是 Mesa interop 缺口，不是探针缺陷。

## 3. 边界计数器基线（双设备、双后端、四条 trace）

`MOBILEGL_PIPE_STATS=1` 经 retrace 通道的 `--env` 透传；trace APK 从 `7ef7c7e5` 构建，spike OFF。取每次运行的**最后一个完整 120 帧窗口**。accessor/draw 与 memo 门数字是软件确定的（同一 trace 在两台设备上完全相同：它们数的是代码路径不是硬件），只有墙钟/CPU 时间随设备变。

| trace（窗口内帧数） | 后端 | draws/f | **acc/draw** | buf B/f | tex B/f（发射 box/rect） | ubo-global B/f | **ubo-named B/f** | memo 门（hit/miss） |
|---|---|---|---|---|---|---|---|---|
| `minecraft-1.21.4-in-world`（360） | Espryt | 91.6 | **9.28** | 13.5 K | **635 K**（185 box / 0 rect） | 16.7 K | 0 | ers 9257/2577，etl 10538/1296，eub 10720/1114 |
| `minecraft-1.21.4-in-world`（360） | Magma | 91.6 | **8.56** | 13.5 K | 39.9 K（97 box / 89 rect） | 16.7 K | 0 | mfp 0/10994，mpm 9240/1754，mdt 9120/1874 |
| `minecraft-1.21.4-fabric-iris-bsl-in-world`（120，memo 冷） | Espryt | 23.2 | 21.04 | 32.6 K | 8.8 K | 1.8 K | 0 | ers 1958/1843，etl 722/3079 |
| `minecraft-1.21.4-fabric-iris-bsl-in-world`（120，memo 冷） | Magma | 23.2 | 11.26 | 313 K | 256 K | 1.8 K | 0（vtxc 1.7 K） | mpm 1890/895，mdt 1573/1212 |
| `improved-transparency-minecraft-26.3`（1200） | Espryt | 1320 | **8.44** | 333 K | 0 | 0 | 0 | ers 156925/2791，etl 148606/11110，eub 148246/11470 |
| `improved-transparency-minecraft-26.3`（1200） | Magma | 1320 | **6.53** | 173 K | 0 | 0 | **331 K** | mfp 21360/137036，mpm 134421/2615，mdt 156611/1785 |
| `minecraft-1.21.1-neoforge-create-indirect-in-world` | 两者 | — | — | — | — | — | — | 两台设备都失败（§5），且不足 120 帧 |

门缩写：ers = `EsprytRenderState`，etl = `EsprytTextureSyncList`，eub = `EsprytUnitBindingsEpoch`，mfp = `MagmaDrawFastPath`，mpm = `MagmaPipelineMemo`，mdt = `MagmaDynamicTail`（`MobileGL/MG_Util/Metrics/PipeStats.h:46-122`）。`accessor-calls` 是约 10 个热入口的静态计数，是每 draw accessor 数的**下界**（站点清单 `MobileGL/MG_Util/Metrics/PipeStats.cpp:16-100`）。

对设计的读法：

- **真机稳态动态 accessor 成本是每 draw 6.5–9.3 次**（预测区间 10–25 的下沿；llvmpipe 的 15.5/20.7 是 memo 冷的）。推送要打败的是 ~8 次 accessor + memo 探测，不是 124/169 的静态调用点数。GO/NO-GO 的 tracker 绝对 ns 上限从这里定。
- **`stage-ubo-named`（D-B8）**：Magma 在 26.3 世界每帧重打包 **331 KB** 具名 UBO 字节，Espryt 直接绑定为 0——host payload 决定的第一个真数字。
- **union box vs region list**：vanilla 世界同样 185 次发射，Espryt 的整 box 路径移动 **635 K** 纹素字节/帧，Magma 的 rect 路径 **40 K**，16×——"server 选上传形状"这一条的量化依据（Mali 侧的 +6 ms/frame 作业数悬崖在另一个方向）。

复现（一台设备一次；两台必须**串行**，见 §4）：

```sh
ANDROID_SERIAL=<serial> MSYS_NO_PATHCONV=1 \
python3 tools/trace_replay/run_android_retrace_local.py \
  --case minecraft-1.21.4-in-world --backend DirectGLES \
  --env MOBILEGL_PIPE_STATS=1 --env MOBILEGL_PIPE_STATS_PERIOD=120
# 数字在结果目录的 mobilegl.log 里，grep 'MGPipe stats:'，取最后一个完整窗口
```

## 4. 桌面数据点与语料事实

- **llvmpipe / lavapipe 动态 accessor**（`GuiBatchScenario`，14 帧 / 26 draw，memo 冷）：Espryt 20.65 / Magma 15.54 次/draw——落在预测区间内，且因场景太短偏高；真机稳态数字见 §3。
- **dirty-surface 面**（`python3 scripts/gen_pipe_dirty_surface.py --summary`，本树）：`MG_Impl/GLImpl` 41 个文件，926 次 mutator 调用，73 个不同 mutator；92 次（36 个即时发布点、7 个 mutator，836 次里绝大多数是 `RecordError`）位于同函数内也到达后端的入口，其余 834 次由紧随的 verb 发布。映射表是 73 条目的问题。
- **读点覆盖**（`python3 scripts/gen_pipe.py`）：71 条调用（11 screen / 60 context）、63 个 verify payload、61 个 `PipeInputs` 字段；477 行后端读点清单 → 299 调用、5 client 自答、6 反向通道、167 结构性句柄、**0 UNMAPPED**。
- **OOM 探测惯用法**：41 个 trace fixture 中 0 例——全部语料只有 9 次 `glRenderbufferStorage` 调用散在 5 个 fixture，无一在其后 3 个调用内跟 `glGetError`；语料里真实的成功性检查是 `glCheckFramebufferStatus`。→ `glRenderbufferStorage*` 不 ack。
- **`FramebufferSrgb` / `DepthClamp`**：`FramebufferSrgb` 的六个后端读点全部消费一个编译期常量 `false`，`DepthClamp` 零读点；两者的 `glEnable` 落到 `RenderState.cpp` 的 `default:` 分支既不存储也不报 `GL_INVALID_ENUM`；41 个 fixture 无一开启任一项（补真存储不会改动任何既有 fixture 的输出）。
- **`GetIntegeri_v` 族**：Espryt 实现里是 `GetIntegeri_v` 的 9 个分支 + `GetInteger64i_v` 的 2 个（不是"15 个 case"）；`GL_COMPUTE_WORK_GROUP_SIZE` 由 `GL_Program.cpp` 用 `ProgramObject::GetComputeLocalSize` 纯前端回答。
- **payload 尺寸**（`MG_Pipe/MGPipeTypes.h` 的 `static_assert`，arm64 与 x86-64 一致）：`MGPDrawInfo` **56**、`MGHostSpan` 32、`MGPBindRenderState` **12**、`MGPResourceDesc` 88、`MGPFramebufferState` 304、`MGPProgramDesc` 192、`MGPSubData` 72、`MGPPixelPackState` 28、`ResidualValueBlock` **1248**（其中 `RenderStateParameters` 1168）。`SEG_CMD` 按 56 B 固定头定尺：MC 帧 1000–4000 draw 时每帧 56–224 KiB 头字节。
- **persistent map 采纳的既有基线**（`dev`，MC 26.3，Adreno）：≥16 MiB 可变 store 定义时采纳为 coherent persistent map 后 p99 163→21 ms、稳态 40→115 fps、省 ~400 MB。P11 的回归上限对着它。
- **Mali 上传作业数悬崖**（Espryt 代码注释记录的既有实测）：~100 个精灵 rect 对一个 union box 是 +6 ms/frame。

## 5. Harness 事实与陷阱

1. trace app 从不到达 `MobileGL::DestroyImpl`，所以 `MOBILEGL_PIPE_STATS_FILE` 的 JSON 转储在设备上永远不会写——只有 `mobilegl.log` 里的周期汇总行；短于一个周期的 trace 什么都不产出。`MOBILEGL_PIPE_STATS_PERIOD`（`458ccde1`）为此而加：需要数字的运行把它设到足够小。
2. `run_android_retrace_local.py` 每棵树共用一个 `.trace-work/android-retrace-result` 根并在每次调用时 `rmtree`，所以两台设备必须从一棵树**串行**跑。
3. `--env` 值里嵌入的 `/data/...` 会被 runner 的 bash.exe 做 MSYS 路径转换（`MSYS2_ARG_CONV_EXCL="/data/*"` 只覆盖开头匹配）——用 `MSYS_NO_PATHCONV=1` 跑。
4. `coherent_as_flush` 管线完好：`--ez coherent_as_flush true` → `trace_replay_core.cpp` 的 `setenv`，独立于 `--env` 透传。
5. **`minecraft-1.21.1-neoforge-create-indirect-in-world` 在两台设备上都失败**（Adreno 830：Espryt ~4.5 分钟后黑帧，Magma 纹理上传提交时 `VK_ERROR_DEVICE_LOST`；Mali：SSIM 0.85 / 0.45）。Adreno 830 上用 `dev@81b17c0b` 基线 APK 复现，**是基线就有的问题，不是本分支造成**；它是 P3a/P8 验收清单里的用例，需先在 `dev` 修。
