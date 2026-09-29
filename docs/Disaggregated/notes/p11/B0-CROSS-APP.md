# P11 B0：同机跨 app 的 client 能否走 unix + 共享内存（真机）

2026-09-29，Redmi `2f7cbe2e`（Adreno 830，Android 16 / HyperOS），代码 `82467823`。server = trace APK #1（`top.mobilegl.plugin.trace`，`untrusted_app:c83`）；client = adb shell 里的 retrace 可执行文件（`shell`）与 trace APK #2（`top.mobilegl.plugin.b0.trace`，`untrusted_app:c84`）。只记录、不设门。原始数据 `~/w7/notes/p11/evidence/b0/`，报告 `~/w7/notes/p11/b0-report.md`。

## 结论

**SELinux 挡的是连接这一步。** 另一个安全上下文里的 client 连不上 server app 的 `@abstract` 端点：`unix_stream_socket connectto` 被拒，`shell → untrusted_app:c83` 与 `untrusted_app:c84 → c83` 都一样。一旦连上（用 root relay 代替 broker），现有代码就走 SharedSegments（7 个 fd 经 `SCM_RIGHTS`：4 个段、3 个门铃），c84 读写、mmap c83 的段都没有拒绝，ssim 1.0。**数据面与协议都不用改，缺的是跨上下文的连接中介。**

| client → endpoint | 连接 | 数据面 | ssim（openra / rd12） |
|---|---|---|---|
| shell → `@mglb0` | **拒**（`avc: denied { connectto } … scontext=u:r:shell:s0 tcontext=u:r:untrusted_app:s0:c83,…`） | — | — |
| APK #2 → APK #1 的 `@mglb0` | **拒**（类别不同） | — | — |
| APK #1 自己的 client → 自己的 server | OK | SharedSegments | 1.0 |
| shell / APK #2 → tcp loopback | OK | Stream | 1.0 / 0.99988 |
| root → `@mglb0`；shell、APK #2 → root relay → `@mglb0` | OK | SharedSegments，7 fd | 1.0 / 0.99988 |

## 线程落位决定 shm 是否更快（Espryt rd12 稳态 fps）

| 落位 | tcp | shm |
|---|---|---|
| 默认（5 次） | 45–52 | **26** |
| server apply 线程在 cpu6、client 在 cpu7（3 次） | 54.5 | **118.0** |

默认下 apply 线程亲和两个大核（`auto` = `0xc0`）且自旋，外部 client 的 GL 线程落到同一对核上。干净落位时 shm 与 P10 的 app 内 shm（120 fps）相同，openra Espryt 491 → 903。Magma rd12 在各臂都受 server 限制（默认 19 tcp / 23 shm）。

## B1 要做的（按代码）

| # | 件 | 依据 |
|---|---|---|
| 1 | server app 里导出、校验调用方的 bound Service，把 client 的两条已连接套接字经 Binder 交出（最简：Service 自己连两次自己的端点，同 app 允许） | `MobileGLServerService.onBind` 今天返回 null；唯一入口是 `Listen` + `AcceptPair` |
| 2 | client 端点接受已连接的 fd（如 `fd:<control>,<aux>`） | `ClientSession.cpp:846-855` 今天只收 `fork` / `tcp://` / `unix:` |
| 3 | client 侧能调用 `bindService` 的引导：有 Java 的 client（FCL、启动器）直接用；Termux 一类原生程序没有 `Context`，要伴生的 helper（如 `app_process`）——**待用户裁定** | 原生进程没有 `Context`；`AServiceManager` 只到系统服务 |
| 4 | 按身份配对控制与辅助连接，不按到达顺序 | F2（下） |
| 5 | 外部 client 使用期间 server 进程不被冻结 | F1（下） |
| 6 | 外部 client 会话的亲和 / 自旋策略 | 上表 |
| 7 | broker 上的调用方认证（unix 端点今天没有令牌） | `Handshake.h` `AuthenticatePeerToken` 无令牌即通过 |

## 顺带查出

- **F1 HyperOS 熄屏冻结整个 server uid**（`frozen uid = … reason=screen off`），FGS 进程、supervisor 与会话子进程同一 cgroup，在飞的会话 120 s 后 `Fatal{BarrierTimeout}`。FCL 的同机用法也会碰到。
- **F2 `AcceptPair` 按到达顺序配对**（`SocketTransport.cpp:373-420`，TCP 与 unix 共用）：`ServerControlActivity` 的就绪探测是一次空的 `connect()`+`close()`，2 s 内会与下一个真 client 的控制连接配成一对，会话读到探测的套接字后退出；两个 client 同时连也会交错。
- 构建：`-Pmobilegl.apkSuffix` 只改文件名；包名由 `-Pmobilegl.applicationIdSuffix` 决定。
- 工具提交 `72926a88`（`p11/b0`）：`tools/device_bench/disagg/fdrelay.c`，root 的字节 + `SCM_RIGHTS` 中继，B1 broker 的替身。
