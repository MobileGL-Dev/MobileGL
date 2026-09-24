# P0 — 卫生、度量、门、骨架（2026-09-05）

> 本页是该阶段的**阶段汇总**：阶段表行、实测与落地形状的完整文本（2026-09-24 从上层索引移入，原文照录）都在这里；上层只留摘要——[`ROADMAP.md`](../../ROADMAP.md)、[`MEASUREMENTS.md`](../../MEASUREMENTS.md)、[`ARCHITECTURE.md`](../../ARCHITECTURE.md)。文中 `file:line` 与"今天""当前"按各段写成时的头理解；文中的 `MEASUREMENTS.md §N` / `ARCHITECTURE.md §17.x` 编号保留，对照表见 [`../README.md`](../README.md)。

## 摘要

- 落地：边界计数器（`PipeStats`）、`PipeCalls.def` 完整目录 + payload POD + 生成器 G1–G7 + CI `pipe-gates`、`gen_pipe_dirty_surface.py`、`check_doc_citations.py`、`MOBILEGL_PIPE_*` 开关、`MG_Remote/{Protocol,Transport}` 骨架 + `protocol.fbs` + `flatc-check`、retrace `--env` 透传。
- spike A：Android 应用进程 `fork`+`execve` 第二个原生可执行文件，两台设备都成立（P6 / P12 的交付链前提）。
- spike B：跨进程外部内存只有 **T0（AHB BLOB 交接）** 在两台设备、两个后端上都是完整读写；Adreno 另有 T1（Vulkan opaque fd）。
- 基线：真机稳态动态 accessor 每 draw 6.5–9.3 次；Magma 在 26.3 每帧重打包 331 KB 具名 UBO；同 185 次纹理发射 Espryt 整 box 635 KB/帧 vs Magma rect 40 KB（"server 选上传形状"的依据）。

## 阶段表行（原 `ROADMAP.md`）

- **阶段**：**P0** 卫生、度量、门、骨架
- **状态**：✅
- **落地什么 / 范围**：边界计数器；`PipeCalls.def` 完整目录 + payload POD + 生成器 G1–G7 + CI `pipe-gates`；`gen_pipe_dirty_surface.py`；`check_doc_citations.py`；`MOBILEGL_PIPE_*` 开关；`MG_Remote/{Protocol,Transport}` 骨架 + `protocol.fbs` + `flatc-check` + `MG_Test/Wire`；三个严格 no-op 收益；spike A / B；retrace `--env` 透传
- **验收门 / 证据**：单元 / 集成 / trace 逐名不变；wire 层测试绿；两台设备字节 / 调用基线在案；spike 结论。`MEASUREMENTS.md` §1

## 实测：P0（2026-09-05，trace APK `7ef7c7e5`，spike `8a239177`）（原 `MEASUREMENTS.md` §1）

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

## 本目录

| 文件 | 内容 |
|---|---|
| [`BRIEF-DOCS.md`](BRIEF-DOCS.md) | Brief: rewrite `docs/Disaggregated/` into a clear, concise design + architecture document set |
| [`BRIEF-P0.md`](BRIEF-P0.md) | P0 implementation brief (MobileGL disaggregation, plan "MGPipe") |
| [`p0-device-findings.md`](p0-device-findings.md) | P0 device runs (2026-09-05 evening, both device locks taken over from the stale 00:39/00:43 holders) |

从失败的 workflow 捞回的 P0 期材料（与本目录重复的副本已删除，只留独有的）：
[`../recovered/wf1/final/`](../recovered/wf1/final/)（设计定稿前的实施计划草案四部分）、
[`../recovered/wf2/`](../recovered/wf2/)（方案 B / MGPipe 实施计划 v2 的分部与综合稿、`p0-polish-findings.md`、citation lint 自测）。
