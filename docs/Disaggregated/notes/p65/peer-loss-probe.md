# P6.5 真实 peer-loss 探针

构建目标 `MobileGLPeerLossProbe`，产物位于 `MobileGL/MG_Test/Wire/MobileGLPeerLossProbe`。它是手动工具，不注册为无设备时自动 skip 的 unit。

探针复用生产 `ClientSession::StartSpawned` 的 Connect 路径及 Hello/Welcome；计时期间不发送 GL 命令、应用 heartbeat 或人为 ACK。它先观察生产 data doorbell 的 `PeerHungUp()`，随后走真实 `FenceStatus -> EmitAndWait -> ShutDown` 路径触发 session latch，并调用生产 `GetGraphicsResetStatus` handler 检查 `GL_UNKNOWN_CONTEXT_RESET`。没有直接调用 `LatchDeviceLost`。

## S2：真 kill server child

先启动与 client 同源码的 supervisor。以下杀的是 Welcome 声明的会话 child，不是 supervisor：

```bash
cmake --build /home/swung/p65-split --target MobileGLPeerLossProbe -j 12
export MOBILEGL_IPC_CONTROL=tcp://PHONE_IP:40613
export MOBILEGL_IPC_TOKEN=devtoken-0123456789abcdef   # ≥16 字节（PH-7 (3)），与 server 端一致
export MOBILEGL_IPC_REQUIRE_SAME_BUILD=1
export MGITEST_PEER_KILL_CMD='adb -s 2f7cbe2e shell run-as top.mobilegl.plugin.trace kill -9 {pid}'
/home/swung/p65-split/MobileGL/MG_Test/Wire/MobileGLPeerLossProbe --kill --deadline-ms 10000
```

loopback supervisor 的模板可以是 `MGITEST_PEER_KILL_CMD='kill -9 {pid}'`。模板必须含 `{pid}`；probe 只把它替换成经 Welcome 获取的正数 PID。命令跑在 probe 自己的 helper process group，超时会清理并回收 helper。

## Wi-Fi 半开：由父侧执行断网

该门必须用直连手机 IP；`adb forward` 走 USB，不会因为手机 Wi-Fi 关闭而断开，不能充作 Wi-Fi 门。

```bash
export MOBILEGL_IPC_CONTROL=tcp://PHONE_IP:40613
export MOBILEGL_IPC_TOKEN=devtoken-0123456789abcdef   # ≥16 字节（PH-7 (3)），与 server 端一致
export MOBILEGL_IPC_REQUIRE_SAME_BUILD=1
run=$(mktemp -d /tmp/p65-wifi.XXXXXX)
probe=/home/swung/p65-split/MobileGL/MG_Test/Wire/MobileGLPeerLossProbe
"$probe" --external --deadline-ms 10000 \
  --ready-file "$run/ready" --start-file "$run/start" >"$run/probe.log" 2>&1 &
probe_pid=$!
# READY 文件只有 Welcome PID；等待它出现后才允许注入故障。
while [ ! -s "$run/ready" ]; do kill -0 "$probe_pid" || exit 1; sleep 0.05; done
trap 'adb -s 2f7cbe2e shell svc wifi enable' EXIT
# 同一个父侧操作内先打时间起点标记，随即关闭手机 Wi-Fi。
touch "$run/start"
while ! grep -q P65_PEER_LOSS_ARMED "$run/probe.log"; do kill -0 "$probe_pid" || exit 1; sleep 0.005; done
adb -s 2f7cbe2e shell svc wifi disable
wait "$probe_pid"
cat "$run/probe.log"
```

marker 的观测周期是 5 ms。计时从 probe 观察 marker 开始；父侧写 marker 后等待 ARMED 行，再立即断网，避免故障抢在计时开始之前。该数包含少量父侧与 adb 启动延迟，对 10 s 上限是保守测量。也可省略 start-file，在 READY 后向 stdin 输入 `GO`，适合手动终端。

成功行形如：

```text
P65_PEER_LOSS_PASS pid=N mode=external peer_hung_up_ms=N device_lost_ms=N deadline_ms=10000 device_lost=1 gl_reset=0x8255 source=production-barrier
```

PASS 要求 descriptor hangup、dead bell、生产 latch、GL reset 状态与 DECLINED 回复全部成立，且总时间严格小于 deadline。只有超时不会设置 latch；没有故障时 deadline 结束应返回非零，这本身可用作阴性对照。

退出码：0 成功；2 会话/READY 失败；3 计时或触发失败；4 kill 模板失败；5 生产 latch/GL 状态失败；64 参数错误。探针没有创建真实 EGL/GL context，因此不实现 `--stage-burst`，不把人为传输 ACK 伪装成上传完成。

## 本机验证

2026-09-22 独立临时 TCP supervisor 的 S2 真 `kill -9`：`peer_hung_up_ms=4`、`device_lost_ms=4`、`gl_reset=0x8255`，exit 0。无故障的 150 ms 阴性对照：exit 3、`peer_hung_up=0 dead=0 device_lost=0`。证据目录 `/home/swung/p65-peerloss-l2ktmltm`；未用手机，这个数不代替设备/Wi-Fi 门。
