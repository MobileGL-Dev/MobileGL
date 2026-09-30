# 容器当 client，手机当 server（上屏）

> 2026-10-01 在 Redmi 23117RK66C（Adreno 750，Android 16）与它的 Droidspaces Fedora 44
> 容器上实测通过。本文记的是那条链路上**真正要设的东西**，以及两个会让人白跑一次的坑。

## 为什么不需要 `adb forward`

Droidspaces 容器与 Android **共用网络命名空间**（实测：容器里 `127.0.0.1:22` 就是 Android 的
sshd，`ip addr` 列出的是手机的 `wlan0` / `rmnet_data5`）。所以服务端监听手机回环即可，
容器直接用 `tcp://127.0.0.1:<port>` 连上去，不需要 `adb forward`，也不需要局域网地址。

## 起服务端（手机侧，上屏形态）

```bash
TOKEN=<>=16 字节>
adb shell am start -n top.mobilegl.plugin/top.mobilegl.plugin.MobileGLDisplayActivity \
    --es listen tcp://127.0.0.1:40613 --es token "$TOKEN"
# 就绪行：
#   MG_Remote server: pid=… listening on tcp://127.0.0.1:40613 (in-process display server, display installed)
```

**不要**给这次启动加 `--es env MOBILEGL_IPC_REQUIRE_SAME_BUILD=1`，除非两端确实同 build：
按契约，`Dial == Connect` 时 build stamp 不同**只应警告**；加上它，服务端会因为 app 的戳（发布
APK 里是 0）与容器客户端的戳（源码树短哈希）不同而 `Refuse{BuildFingerprint}`——症状是客户端
在 `eglCreateWindowSurface` 之前就被 abort，日志里只有一条与真正原因无关的 caps 门。

## 容器侧（客户端）

```bash
export MOBILEGL_TRANSPORT=spawn            # 两进程；endpoint 由下面这条给
export MOBILEGL_IPC_CONTROL=tcp://127.0.0.1:40613
export MOBILEGL_IPC_DATA=stream            # 跨进程字节流；同机共享段是另一条轴
export MOBILEGL_IPC_SURFACE=server         # 窗口是服务端的：客户端传 NULL 窗口
export MOBILEGL_IPC_TOKEN=<同一个>
export LD_LIBRARY_PATH=/root/build-cont    # 容器里那份 split 构建
./onscreen_client                          # eglCreateWindowSurface(dpy, cfg, NULL, {EGL_WIDTH,EGL_HEIGHT})
```

客户端侧不需要 GPU、不需要 `/dev/dri`、不需要 Wayland：它只记录，画的是服务端。

## 实测结果

```
EGL 1.5 vendor=MobileGL version=1.5 MobileGL
surface created: 1440x3200 (asked 1440x3200)
GL_RENDERER=Espryt (MobileGL Core) (Adreno (TM) 750, OpenGL ES 3.2)
frame 0..11 swap=1
```

服务端逐帧：

```
P65ServerFrame frame=8..12 records=4 apply_ms=1.0..1.7
```

抓屏为整屏 `#72e3d8`，即客户端第 3 种清屏色 `(0.20, 0.90, 0.85)` 经 sRGB 转换后的值——
容器里那个没有 GPU 的进程要求画的东西，出现在手机屏幕上。

## 待办（本页不含）

- kwin：让 Plasma 的合成也走这条客户端（`LD_PRELOAD` 或直接链接容器里那份 `libEGL`），
  窗口面仍是服务端形态；此时 Anland 只剩 Wayland 协议与输入。
- 两端同 build：容器里的源码树与 APK 从同一 revision 构建后，`REQUIRE_SAME_BUILD=1` 才有意义。
