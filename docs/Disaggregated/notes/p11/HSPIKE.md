# P11 B1 预探针：无 Context 的 `app_process` helper 能否经 Binder 拿到连 server 的套接字（真机）

2026-09-29，Redmi `2f7cbe2e`（Android 16 / HyperOS），代码 `2fcde6eb`（PAIR 之前）。报告 `~/w7/notes/p11/hspike-report.md`，证据 `~/w7/notes/p11/evidence/hspike/`，工具提交 `59a5ceeb`（`p11/hspike`，保留到 B 落地），未提交的 `fd:` 端点补丁 `evidence/hspike/fd-endpoint.diff`。

## 结论：两条路都通，原生路线可行

| 路线 | helper 所在上下文 | 结果 |
|---|---|---|
| (a) | `adb shell app_process`（`u:r:shell:s0`） | 通；openra ssim 1.0，rd12 0.99988（Espryt）/ 0.999884（Magma） |
| (b) | 从 APK #2 的进程 exec `app_process`（`untrusted_app:c84`，代表 Termux 一类） | 通；同上 |
| 令牌错或缺 | 两条路 | 按名拒绝（"token mismatch" / "no expected token configured"），不给 fd、不起会话 |

**链路**：helper 自建 Binder → 经隐藏接口 `IActivityManager.broadcastIntentWithFeature`（与 `am` 同路，参数按类型填）发**显式广播** → server app 里导出的 broker 核对 IPC 令牌 → 连两次自己的 `@mglb0`（同 app 允许）→ 两个 client 端作为 `ParcelFileDescriptor` 回给 helper → helper 清 `FD_CLOEXEC`、带 `MOBILEGL_IPC_CONTROL=fd:<control>,<aux>` `execve` 原生 retrace client。跨类别把 c83 建的套接字交给 c84 使用，无任何 avc 拒绝（补上 B0 §6 #1 未验证的一项）。

**依赖 ROM 的前提**：直接跑的 `app_process` 不是 zygote fork 出来的，不受 app 的隐藏 API 黑名单约束，所以 `IActivityManager` 可用；若某个 ROM 对它也强制执行，原生路线就断——`am broadcast` 这类命令带不了 Binder，没有替代。

## 顺带查出（B1 要处理）

| # | 现象 | 对 B1 的意义 |
|---|---|---|
| 1 | 从 app uid 调 `pm path` 查 server APK 被包可见性过滤；但 c84 的 `app_process` 用 APK #1 的 `base.apk` 作 `CLASSPATH` 能正常加载，显式组件广播也不需要可见性 | 无 Context 的 client 缺的只是"发现"；由 server 界面生成可复制的启动命令（APK 路径、组件、令牌） |
| 2 | HyperOS 熄屏约 7 s 后冻结 server uid，在飞会话 120 s 后 `Fatal{BarrierTimeout}`；helper 的 Binder 在 `execve` 之后就没了，保不住 server。冻结也会落到 client app 上，路线 (b) 的 rd12 要靠 root 解冻才跑完 | 防冻结要另想办法（前台服务 + 文档化的每 app 电池设置），并且门要在用户能达到的拓扑下量 |
| 3 | 路线 (a) 默认落位 23.5 fps → 手工定核 118.5（与 B0 的 26 → 118 一致，helper 与 `fd:` 在数据面上零成本）；路线 (b) 两种落位都约 49：退到后台的 client app 被 HyperOS 关进小核 `/background` cpuset，调用方的 `taskset` 无效 | server 侧的亲和 / 自旋策略是最大的杠杆；client 在前台时才有大核可用 |
