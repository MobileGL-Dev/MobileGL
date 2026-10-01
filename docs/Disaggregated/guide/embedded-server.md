# 在宿主 APK 中嵌入 split server

宿主应用可直接加载 `libMobileGL.so`，把自己持有的 `ANativeWindow` 交给 server。
窗口、输入和 Android Service 的生命周期由宿主 APK 管理；不需要安装 MobileGL plugin APK。
公共接口是 [`EmbeddedServer.h`](../../../MobileGL/MG_Remote/Server/EmbeddedServer.h)，
只使用 C 类型，也可通过 `dlsym` 调用。构建必须开启 `MOBILEGL_BUILD_DISAGGREGATED`。

## 有窗口的合成器服务

在加载库前设置 `MOBILEGL_IPC_ROLE=server`、`MOBILEGL_IPC_DIAL=no`，清除
`MOBILEGL_TRANSPORT`、`MOBILEGL_IPC_SERVER_PATH`、`MOBILEGL_IPC_RING_MB` 和
`MOBILEGL_IPC_STAGE_MB`。建议放在宿主 APK 的独立服务进程中，以免渲染故障结束 UI 进程。

1. 调用 `mobilegl_server_display_install_android(request_geometry, user)`。
   回调可为空；有回调时，它在 apply 线程运行，必须快速返回，把尺寸变更交给宿主线程。
2. 从宿主的 `Surface` 获取 `ANativeWindow`，调用
   `mobilegl_server_display_attach_android(window, width, height)`。
   server 独立持有引用，调用者随后释放自己的引用。同一窗口再次 attach 只更新尺寸。
3. 在服务线程调用 `mobilegl_server_serve_inprocess("@anland-mobilegl-compositor")`。
   这会在当前进程接受一个 client 会话。AF_UNIX 控制连接用 `SCM_RIGHTS` 传递共享内存段，
   数据通过 SHM 环传输。客户端设置 `MOBILEGL_TRANSPORT=spawn`、
   `MOBILEGL_IPC_CONTROL=unix:@anland-mobilegl-compositor`、`MOBILEGL_IPC_DATA=shm` 和
   `MOBILEGL_IPC_SURFACE=server`。
4. Surface 销毁时调用 `mobilegl_server_display_detach(3000)`。
   返回值 `0/1/2/3` 分别为无窗口、直接释放、会话释放后完成、超时。
   超时会保留引用，直到 apply 线程停止使用窗口；当前失窗会结束活动会话。
5. 服务停止时调用 `mobilegl_server_stop_inprocess()`，等待 serve 线程返回，再调用
   `mobilegl_server_display_uninstall(3000)`。几何回调的 user 对象应至少活到此时。

`width/height` 为零时，Android attach 从 `ANativeWindow` 查询尺寸。几何回调收到正尺寸表示
请求固定缓冲尺寸，收到 `0/0` 表示恢复宿主布局尺寸；宿主应通过后续 attach 报告实际结果。
安装、attach、detach 和 uninstall 由宿主串行调用。库拒绝替换已安装的 display 或尚未结束
的旧窗口租约，避免晚到的释放使用另一组引用回调。

## 并发的离屏应用服务

一个进程仍只有一套 backend/GL 状态。有窗口的服务只接一个合成器；其他 EGL client 可连接
宿主 APK 打包的 `libMobileGLServer.so` 可执行文件，由另一个服务进程负责启动和结束：

```text
<nativeLibraryDir>/libMobileGLServer.so @anland-mobilegl --serve --max-sessions 16
```

`--max-sessions` 是活动会话上限，接受 `1..64`，默认仍为 `1`。大于 `1` 只适用于
AF_UNIX 的 `--serve` 离屏 supervisor。每个 client 单独 fork worker，拥有独立 backend，
共享内存与其他会话隔离；worker 退出后归还名额，supervisor 结束时所有 worker 一起结束。
这些 worker 没有 display，客户端应清除 `MOBILEGL_IPC_SURFACE`，由客户端 WSI 提交窗口图像。

进程隔离解决多个应用同时连接的问题；同一应用中的真正多 context/share-group 支持仍需另行实现。
