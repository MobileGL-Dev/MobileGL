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
| `ccf8eb65` | 客户端：多线程 GL 流串行化——每个 GL/EGL 出口持同一把流锁，写入前按「最后真正发出的 `bind_context`」把 server 绑到调用线程的 context（Qt 每个窗口一个 QSGRenderThread，此前两线程记录在同一 ring 上交错、落进错的 applier、甚至撕裂记录）；`MGPipeTracker` 按 context 的永不复用 id 判断换 context（指针 ABA）；导出 `eglCreateImageKHR`/`eglDestroyImageKHR`（KWin 在 shm client 断开时调空指针 → 首个 KWin 约 2 秒必 SEGV）；`eglGetPlatformDisplayEXT` 的 EGLint 属性表加宽；多窗口：surface 激活不再清空别的线程的 current 记录，swap 呈现本线程的 surface；Wayland 窗口 `wl_egl_window_resize` 在下一次 make-current/swap 生效 |
| `0b314dda` | server：所有 session 的 apply 线程在一把 FIFO「backend turn」上串行（DirectGLES 是按单 GL 线程写的：单一 owner 线程、进程级绑定影子），换手时接管 owner 并清掉绑定影子；全部原生 context 进同一 share group（环、池、scratch 纹理是进程级 GL 对象）；每个客户端 surface 一个原生 surface（激活时重绑而非新建），建 surface 后恢复当前 context 自己原来的 surface，释放 surface 只销毁它（此前 `DestroyEGLContext` 带走整个 session 的 context），`bind_context` 恢复 context 自己的 surface；pbuffer 背后的窗口 resize = 换新 pbuffer；session 结束时若别的 session 的 context 还活着，只清本 session 的东西（此前进程级 generation 被推进，所有活 session 的纹理/缓冲被当成死的空重建 → 关弹窗/退出 client 后全屏噪点）；分块上传（stage chunk 切片）的 region 偏移按整层图像校验（此前 KWin 的 wl_shm 纹理在第 819 行后读到第 0 行）；`SetSamplerViews.Count` 改为 per-session latch |
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
- **Plasma 桌面可用（2026-10-02，`ccf8eb65`：server 与容器 client 同源）**：壁纸、面板、托盘、时钟、桌面图标、Kickoff 开始菜单、通知窗口都正确渲染；鼠标/触摸输入可用（点击打开 Kickoff）；KWin 过渡动画正常；开/关托盘弹窗不再花屏；按返回键收起快捷键栏时输出 resize 跟随。三线程/三 context/三 surface 并发回读探针（`anland-egl-mt-readback-probe.c`）与大纹理分块上传探针（`anland-egl-upload-probe.c`）全过。
- **Plasma 会话（早期，`30fef23d` 的 server + `ff212061` 的容器 client）**：`desktop-session.service` 持续 active（>1 分钟，此前约 7 秒必拆）；面板完整渲染（启动器、固定应用、托盘、时钟）；光标尺寸正常；多个 session 并发、逐个 reap 而其余继续服务。截图里壁纸有横向条带错位（见下面第 1 条）。

## 未完成 / 已知问题（按优先级）

1. **Plasma：桌面已落地、基本可用，剩余问题**（按优先级）：
   - ~~启动 Chrome 时 KWin 崩溃~~ 已修（anland `kwin.patch`）：无 DRM 设备分支里 `waylandServer()->linuxDmabuf()` 会创建 linux-dmabuf global，而空 tranche 不建格式表，Chrome 请求 default feedback 时 KWin 解引用空表（`LinuxDmaBufV1FeedbackPrivate::send` → `FileDescriptor::get`）。现在该分支不碰 dmabuf，client 回落 wl_shm；Chrome 新标签页正常渲染。KWin shim 现在在 `/tmp/kwin-cores/<pid>/` 下运行，core 不会再被覆盖。
   - **某个 client 退出时 KWin 偶发 SEGV**（rc=139，core 被覆盖未取到）；KWin 在 server 进程被杀时也会 SEGV（设备丢失路径），后者是预期外但可接受。
   - **ksplash 启动动画闪黑**：开机时 ksplash 帧间偶有整帧黑；单独 `ksplashqml --test` 在会话内不复现。约 13–15 fps（每帧 2560x1412 全尺寸回读 + wl_shm + KWin 重新上传），见第 3 条。
   - 合成成本：每个 Wayland 窗口每帧整幅 glReadPixels（2 MiB 一片）+ KWin 整幅 glTexSubImage，是当前帧率上限。
   - DirectGLES 里少量进程级「容器」对象（resolve FBO、scratch FBO、XFB helper、timer query 探测）不在 share group 里共享，多 session 交替使用时理论上会拿到别的 context 的名字——尚未观察到症状。
2. **光标巨大化**：2026-10-02 的 Plasma 会话截图里光标尺寸正常，未再复现；若再现，查 output scale 与 cursor layer 的 buffer scale。
3. **CPU blit 禁令**：用户明确要求最终形态上屏路径无 CPU blit——当前 Wayland wl_shm client 走 readback（`32c79f41`），只是过渡。目标：应用窗口内容也由 server 在 GPU 侧直接给 KWin 合成（server 侧 GPU buffer → 同进程共享给 compositor 的 share group → 合成上屏），不经过 CPU 回读。设计要进 11-state-ownership.md 的后续切片。
4. **GLX 无上屏**：X drawable 仅远端 pbuffer 查询，无 X11 readback/present。
5. **surface loss**：Wayland 窗口 resize 已跟随（`ccf8eb65`）；surface loss 仍结束活动 session（无损挂起未实现）；server 进程被杀后，新进程下 KWin 拿不到窗口 surface（KWin 不会重连，需重启会话）。
6. **AVC 审计未做**：全程在 Enforcing 下跑通（容器侧有 droidspacesd 的既有 permissive 规则），SCM_RIGHTS/memfd 的精确 AVC 采集没做。
7. **wire stamp 漂移**：容器构建的 `MGGitHash.h` 因 tar 无 .git 而陈旧（日志 WARN "compatible wire with different build"）；两端应同 commit + 显式 `-DMOBILEGL_BUILD_STAMP` 重建。

## 设备与环境要点（复现用）

- 设备 HA27Q3LQ（Y700 TB321FU），clone `arch-kde-mgl`（arch-kde 保持不动），容器内 `/opt/mobilegl/{lib,bin,kwin}`。
- **KernelSU grant**：`ksud profile` 无直授命令；管理器专属 ioctl 的解法：root 下 setuid 到 manager UID（10255）→ `reboot(0xDEADBEEF,0xCAFEBABE)` 拿 `[ksu_driver]` fd → `SET_APP_PROFILE`。helper 源码在 anland `producers/kde/Arch_v5/mobilegl-tools/ksu_grant.c`。已持久化进 `/data/adb/ksu/.allowlist`。
- 实验 daemon：独立实例 `display_daemon /data/local/tmp/anland-mobilegl/display.sock`（原 daemon 不动）；socket 需 666、目录 777（容器内 uid1000 要连）。
- clone config 的目录 bind：`/data/local/tmp/display_daemon.sock:/run/display.sock`、`/data/local/tmp/anland-mobilegl:/run/anland-mobilegl`。
- KWin 启动：`desktop-session.service` + drop-in ExecStart=mobilegl-startup.sh compositor|plasma；Qt 日志默认只出 warning（info 要 QT_LOGGING_RULES）。
- **当前设备上的 server 库是手工替换的**：`libMobileGL.so` 直接 root 覆盖到已装 APK 的 `/data/app/.../lib/arm64/`（保留 owner/SELinux context，原件备份在 `/data/local/tmp/anland-mobilegl/libMobileGL.so.orig`）。这样不用重签 APK（换签名要卸载重装，UID 会变，KernelSU grant 要重做）。本机构建：NDK 27.2 + `cmake -DCMAKE_TOOLCHAIN_FILE=.../android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DMOBILEGL_BUILD_DISAGGREGATED=ON -DMOBILEGL_BUILD_DISAGGREGATED_INPROC=ON -DMOBILEGL_BUILD_STAMP=<commit>`，`llvm-strip --strip-debug` 后推送。
- **复现用探针**（anland `producers/kde/Arch_v5/mobilegl-tools/`）：`anland-egl-upload-probe.c`（2560x1412 纹理整幅 + 跨分片的局部 TexSubImage 回读校验）、`anland-egl-mt-readback-probe.c`（三线程各自 context/pbuffer 并发绘制+整幅回读）；在容器里以 swung0x48 身份、带 MobileGL 的 `__EGL_VENDOR_LIBRARY_FILENAMES`/`MOBILEGL_*` 环境运行。注入点击要用 `input mouse tap`（anland 抓了触摸屏 EVIOCGRAB，`input tap` 无效）；不要对容器进程用 `debuggerd -b`（发 signal 35，glibc 进程直接被杀）。
- **诊断用的容器改动**：`/opt/mobilegl/kwin/bin/kwin_wayland` 现在是 shim（设 `MOBILEGL_LOG_FILE_PATH=/tmp/mgl-kwin-$$.log` 后 exec `/opt/mobilegl/kwin/real/kwin_wayland`；真二进制必须仍叫 `kwin_wayland`，否则 KWin 内置 QPA 插件拒绝加载）；`~swung0x48/.config/systemd/user/plasma-plasmashell.service.d/mobilegl-log.conf` 给 plasmashell 单独日志 `/tmp/mgl-plasmashell.log`。MobileGL 打开日志是截断写，所以必须每进程一个文件。
- 构建（之前那台机器的环境，本机没有 `/root/anland-mgl-build` 也没有 gitignored 的 `build_bundled_apk.sh`；APK 按 `consumers/anland_v5/android_consumer/MOBILEGL.md` 用 gradle + `MOBILEGL_DIST` 构建）：Android dist 用 WSL（NDK r27c，stamp 显式指定）；APK 用 anland worktree 的 `build/build_bundled_apk.sh`；容器 client 用 `producers/kde/Arch_v5/mobilegl-tools/anland-sync-source.sh build`（`MOBILEGL_REPO` 指向已 init 子模块的 MobileGL worktree；注意 `tools/trace_replay` 789MB 必须排除）。
- 设备 30 秒无操作熄屏——熄屏可能 kill SurfaceView；长跑验证前 `svc power stayon true` 或调 screen_off_timeout。
