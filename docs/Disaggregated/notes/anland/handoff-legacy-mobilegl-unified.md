# Handoff：anland 5.x + 内置 MobileGL 统一 server（post-P14）

更新时间：2026-10-02。本文替换旧的 handoff-legacy-mobilegl-unified.md（那份描述的是 P14 之前的状态，已被事实淘汰）。

## 任务与路线

anland 5.x 用 MobileGL：容器（Droidspaces，arch-kde-mgl）里的 Linux client 经 SHM pipe 连到 anland APK 内嵌的统一 server（`:mobilegl` 私有 Service，同 APK 同 UID），server 在 anland 自己的 Surface 上直接渲染（路线 B：`kgsl-as-server.md`）。不按应用 fork 渲染进程——session/context/share-group 状态归属在 MobileGL 库内解决（P14，[`../design/11-state-ownership.md`](../design/11-state-ownership.md)）。

## 远端与分支

- MobileGL：<https://github.com/MobileGL-Dev/MobileGL> 分支 `feat/disaggregated`
- anland：<https://github.com/MobileGL-Dev/anland> 分支 `legacy-mobilegl-unified`

### MobileGL 关键提交（新→旧）

| Commit | 内容 |
|---|---|
| `30fef23d` | P14 收尾（统一 server 的两处进程级状态）：server 的两个 staged store（纹理 level 声明、buffer shadow）按调用线程的 {session, share group} 分桶（与 S6 twin 表同一个探针），`DropAll` 只清本 session；`MGPipeServerSetContextLive` 的 live 标志从进程全局移进本 session 的 PipeInputs 块。各带单测（`StagedTextureStoreTest` 两例、`ServerContextLiveTest`） |
| `d554c87c` | EGL loader 缺 `eglSwapBuffersWithDamageEXT` 不再 FATAL（EXT→KHR→eglSwapBuffers 回退 + GLES 符号 dlsym 回退防自指）；stencil-only 存储格式配对修复；各带测试 |
| `7a48521d` | DirectGLES：创建 server-owned window surface 前先销毁同窗口的残留 surface（此前第二个起的 session 必死 EGL_BAD_NATIVE_WINDOW） |
| `5a0f13fd` | 广告 GL_ARB_shader_objects/vertex_shader/fragment_shader/texture_non_power_of_two（KWin `checkSupported` 的硬门槛） |
| `ff212061` | P14 S6：后端 twin 表按 {session, share group} 键控 |
| `4ed1c9ce` | P14 S1–S5：`bind_context`（opcode 84）+ CreateContext/DestroyContext 控制帧（control revision 6）；同进程多 session（`SessionRuntime`、线程域解析、per-session latch，inproc unix supervisor 并发上限 `MOBILEGL_IPC_INPROC_MAX_SESSIONS`=16）；前端 ShareGroupState/per-context GLContext/TLS pGLContext；DirectGLES per-(session,context) native tuple（不再无条件 teardown）；applier 按 {session,context}/{session,shareGroup} 分桶 |
| `ebc552a0`/`6fb880cd` | Wayland Present 失败回 EGL_BAD_SURFACE；GLX drawable 走远端 pbuffer；Linux libGL 别名 |
| `d52c031c` | P14 设计文档 |

### anland 关键提交（新→旧）

| Commit | 内容 |
|---|---|
| `56b060c` | 恢复工具链入库 `producers/kde/Arch_v5/mobilegl-tools/`（KernelSU grant helper、源码同步与容器内 client 构建、EGL smoke 探针、设备侧部署/daemon 脚本） |
| `1fb419a` | mobilegl-startup.sh：默认 socket 指向实验 daemon（/etc/environment 的 pam_env 会盖掉 systemd drop-in）；plasma 模式改走 kwin_wayland_wrapper（BusName=org.kde.KWinWrapper 只在 wrapper 注册，直跑 kwin 90 秒被 systemd 杀）；KWin 直写 surface 不加 FlipY（此前画面上下颠倒）；sync-build-kwin.sh 容器侧 canonical 同步/增量构建/原子安装管线 |
| `118ae52` | `MOBILEGL_BACKEND_TYPE=DirectGLES`（Espryt 只是别名，写错会被 pinned-backend 拒）；eglChooseConfig 收 count≥1（MobileGL 默认有两份配置）；compositor client 日志落文件 |
| `da03d3b`/`35699f3` | consumer flap 时不再停 compositor；APK consumer 侧不再因 Surface 生命周期事件重启 native 管线 |
| `33e496c` | 统一内嵌 server 宿主：`:mobilegl` Service 单 endpoint `@anland-mobilegl`；移除被否定的 MobileGLAppsService/按应用 fork 路径 |
| `2ffa1f5` | KWin anland backend 的 `ANLAND_MOBILEGL=1` 路径（跳过 GPU/DRM probing，server-owned window） |
| `6e1fa65` | 无 fd 的 MobileGL Surface 显示协议（DATA_MSG_MOBILEGL_SURFACE=201） |

## 真机已验证（TB321FU，HA27Q3LQ）

以实际画面/输出为准：

- **链路绿屏**：client clear → server（Adreno 750 厂商驱动）→ Android Surface → 屏幕，截图可见（win-smoke，60 帧 swap 无错误）。
- **egl-smoke**：pbuffer + clear + readback RGBA 精确（64,128,191,255）。
- **eglinfo**：GLVND vendor JSON 正常，EGL 1.5，两组配置。
- **glxinfo**：GLX 1.4，Espryt (Adreno 750)，GL 4.6 core，exit 0（注意：GLX 只到查询，无 X11 上屏）。
- **KWin 6.7.4**：合成器存活并呈现（黑桌面+光标可见）。
- **Konsole**：窗口完整正确渲染（标题栏、工具栏、shell 提示符，方向正确）。
- **多 session**：两个 win-smoke 连续运行同一窗口均 exit 0（stale surface 修复生效）。
- **Plasma 会话（2026-10-02，`30fef23d` 的 server + `ff212061` 的容器 client）**：`desktop-session.service` 持续 active（>1 分钟，此前约 7 秒必拆）；面板完整渲染（启动器、固定应用、托盘、时钟）；光标尺寸正常；多个 session 并发、逐个 reap 而其余继续服务。截图里壁纸有横向条带错位（见下面第 1 条）。

## 未完成 / 已知问题（按优先级）

1. **Plasma 会话：已起来，但还不稳**。原先「约 7 秒全拆」的根因是 **APK 的 `:mobilegl` server 进程 abort**（`logcat -b crash` 可见；server 一死，KWin 与全部 client 随之断开）。已修两处（`30fef23d`）：
   - `Fatal{ProtocolCorruption, "StagedTextureStore.LevelExtent"} - resource_subdata arrived before the server declared texture level`：任一 session 结束时 `DestroyEGLContext → OnBackendContextDestroyed` 对进程级 staged store 做 `DropAll()`，清掉了邻居 session 已声明的纹理 level；纹理 store 还按客户端 `{slot, gen}` 键控，两个 client 会撞键。
   - `Fatal{UnmigratedPipeInput, "GetFramebufferBindingSlot@DrawArrays"}`：live 标志是进程全局，Qt client 的 `eglMakeCurrent(NO_CONTEXT)`（ServerLoop 的 ClientRelease 分支）或任一 session 的拆除会把 KWin 的上下文标成 dead。
   仍未解决、按出现顺序：
   - **`Fatal{ProtocolCorruption, "SetSamplerViews.Count"}`（出现过一次，plasmashell 的 session）**：栈是 server 侧 `ReadPixels → CaptureDrawTextureSyncKeys → UnitBindingsEpochFromRecords`——该 context 的 applier 从未收到 sampler-view/state 窗口，而 `MaxTouchedTextureUnit >= 0`。怀疑方向：客户端 `MGPipeTracker` 用 GLContext **指针**判断换 context（`Tracker.h` 的 `m_context != &ctx`），Qt 销毁再创建 context 落在同一地址时不会重新 prime（不发 ApplierReset、不重发窗口），而 server 把新 context token 路由到窗口为空的新 applier——又一个「身份缓存按指针键控」的 ABA。未验证。
   - **首个 KWin 起来约 2 秒、Xwayland 刚起后静默退出**，wrapper 重启后的 KWin 正常工作。无日志、无 core（容器 `ulimit -c 0`）；它的 MobileGL client 日志在 `surface=window ... owner=server` 之后直接截断。下一步：打开 core dump 或在 shim 里套 `catchsegv`/strace 拿退出原因。
   - **plasmashell 约 28 秒后重启一次**（`SwapEGLBuffers failed: no current context attached` 之后），重启后正常。
   - **壁纸横向条带错位**：同一流程的第一次运行壁纸是对的，第二次是条带，非确定性。怀疑大纹理分块上传或 wl_shm 回读的行距/偏移。
   - **加固项**：server 进程里 peer 数据触发的 `MGLOG_F + abort`/`SessionFail` 会带走整个 display 进程和所有 session；统一 server 下这些站点应改成 per-session latch（`FatalFunnel.h` 的 SessionLatch），至少 staged store 与 `UnitBindingsEpochFromRecords` 这两处。
2. **光标巨大化**：2026-10-02 的 Plasma 会话截图里光标尺寸正常，未再复现；若再现，查 output scale 与 cursor layer 的 buffer scale。
3. **CPU blit 禁令**：用户明确要求最终形态上屏路径无 CPU blit——当前 Wayland wl_shm client 走 readback（`32c79f41`），只是过渡。目标：应用窗口内容也由 server 在 GPU 侧直接给 KWin 合成（server 侧 GPU buffer → 同进程共享给 compositor 的 share group → 合成上屏），不经过 CPU 回读。设计要进 11-state-ownership.md 的后续切片。
4. **GLX 无上屏**：X drawable 仅远端 pbuffer 查询，无 X11 readback/present。
5. **resize/surface loss**：wl_egl_window resize 不跟随；surface loss 仍结束活动 session（无损挂起未实现）。
6. **AVC 审计未做**：全程在 Enforcing 下跑通（容器侧有 droidspacesd 的既有 permissive 规则），SCM_RIGHTS/memfd 的精确 AVC 采集没做。
7. **wire stamp 漂移**：容器构建的 `MGGitHash.h` 因 tar 无 .git 而陈旧（日志 WARN "compatible wire with different build"）；两端应同 commit + 显式 `-DMOBILEGL_BUILD_STAMP` 重建。

## 设备与环境要点（复现用）

- 设备 HA27Q3LQ（Y700 TB321FU），clone `arch-kde-mgl`（arch-kde 保持不动），容器内 `/opt/mobilegl/{lib,bin,kwin}`。
- **KernelSU grant**：`ksud profile` 无直授命令；管理器专属 ioctl 的解法：root 下 setuid 到 manager UID（10255）→ `reboot(0xDEADBEEF,0xCAFEBABE)` 拿 `[ksu_driver]` fd → `SET_APP_PROFILE`。helper 源码在 anland `producers/kde/Arch_v5/mobilegl-tools/ksu_grant.c`。已持久化进 `/data/adb/ksu/.allowlist`。
- 实验 daemon：独立实例 `display_daemon /data/local/tmp/anland-mobilegl/display.sock`（原 daemon 不动）；socket 需 666、目录 777（容器内 uid1000 要连）。
- clone config 的目录 bind：`/data/local/tmp/display_daemon.sock:/run/display.sock`、`/data/local/tmp/anland-mobilegl:/run/anland-mobilegl`。
- KWin 启动：`desktop-session.service` + drop-in ExecStart=mobilegl-startup.sh compositor|plasma；Qt 日志默认只出 warning（info 要 QT_LOGGING_RULES）。
- **当前设备上的 server 库是手工替换的**：`libMobileGL.so` 直接 root 覆盖到已装 APK 的 `/data/app/.../lib/arm64/`（保留 owner/SELinux context，原件备份在 `/data/local/tmp/anland-mobilegl/libMobileGL.so.orig`）。这样不用重签 APK（换签名要卸载重装，UID 会变，KernelSU grant 要重做）。本机构建：NDK 27.2 + `cmake -DCMAKE_TOOLCHAIN_FILE=.../android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DMOBILEGL_BUILD_DISAGGREGATED=ON -DMOBILEGL_BUILD_DISAGGREGATED_INPROC=ON -DMOBILEGL_BUILD_STAMP=<commit>`，`llvm-strip --strip-debug` 后推送。
- **诊断用的容器改动**：`/opt/mobilegl/kwin/bin/kwin_wayland` 现在是 shim（设 `MOBILEGL_LOG_FILE_PATH=/tmp/mgl-kwin-$$.log` 后 exec `/opt/mobilegl/kwin/real/kwin_wayland`；真二进制必须仍叫 `kwin_wayland`，否则 KWin 内置 QPA 插件拒绝加载）；`~swung0x48/.config/systemd/user/plasma-plasmashell.service.d/mobilegl-log.conf` 给 plasmashell 单独日志 `/tmp/mgl-plasmashell.log`。MobileGL 打开日志是截断写，所以必须每进程一个文件。
- 构建（之前那台机器的环境，本机没有 `/root/anland-mgl-build` 也没有 gitignored 的 `build_bundled_apk.sh`；APK 按 `consumers/anland_v5/android_consumer/MOBILEGL.md` 用 gradle + `MOBILEGL_DIST` 构建）：Android dist 用 WSL（NDK r27c，stamp 显式指定）；APK 用 anland worktree 的 `build/build_bundled_apk.sh`；容器 client 用 `producers/kde/Arch_v5/mobilegl-tools/anland-sync-source.sh build`（`MOBILEGL_REPO` 指向已 init 子模块的 MobileGL worktree；注意 `tools/trace_replay` 789MB 必须排除）。
- 设备 30 秒无操作熄屏——熄屏可能 kill SurfaceView；长跑验证前 `svc power stayon true` 或调 screen_off_timeout。
