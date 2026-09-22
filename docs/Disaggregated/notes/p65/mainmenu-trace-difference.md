# P6.5：1.21.11 主菜单 GLES 像素差异诊断

**状态：正式设备矩阵仍记 FAIL；独立主机负控已复现同位置缺失，强烈支持 trace 不满足目标 GPU 的 UBO 对齐要求；相同 APK 的设备本地对照仍待补。**
本记录不修改源码、trace fixture、golden 或 SSIM 阈值，也不把该失败归入既有 Vulkan 债务。

## 正式失败与可比阳性

| 对照 | 结果 | SSIM / 阈值 | 用时 | 实际 arming |
|---|---|---|---|---|
| Linux client → Android/Adreno TCP+stream，credit=2 | FAIL，完成回放后图像不符 | 0.890161090 / 0.99 | 272.684 s | ARMED，无 lockstep/DISARMED |
| WSL 同 ABI/驱动 TCP+stream loopback，credit=2 | PASS | 0.993474922 / 0.99 | 14.018 s | ARMED，无 lockstep/DISARMED |

两次目标 call 均为 `205347`，分辨率 `854×480`。正式失败的 `statusCode=6`、
`returncode=1`、`timeout_kind=null`，不是 watchdog 超时；其 mismatchPixels 为 79,246。
客户端和服务端没有 `Fatal{`、device loss、Refuse 或握手失败证据。客户端完成 38,884 次 reply wait；
服务端最后一帧计 11,551 draws。没有遗留进程。

目视检查可见背景、标题与文字仍在，主要缺失 Singleplayer、Multiplayer、Realms、Options、Quit
的灰色按钮底图，以及语言/右侧辅助功能按钮的图标或底图。loopback 中这些元素正常。
图中 Mali/Tensor 文字属于捕获的游戏画面，不能据此判定当前服务端 GPU；实际服务端日志为 Adreno。

正式 attempt：
`/home/swung/p65-final-matrix-gles/DirectGLES/credit-2/minecraft-1.21.11-main-menu/attempt-a1c1263d9b814620b04ff4c8b8d6cbf1`。
原始结果、图像和角色日志位于其 `work/output/`，golden 副本位于 `artifacts/`。
loopback 证据索引为 `/home/swung/p65-mainmenu-loopback-control/control-summary.json`。
该索引记录主机 library SHA256 为
`25ca45d756bc5a4e5bd56ad4ce231e4ef89d45ba41131f389575cda0842cd6be`。
仅 loopback 阳性还不能单独区分 ABI/wire 与设备驱动差异。

## 只读 trace 与源码证据

设备 `mobilegl.server.log:145` 及 `:358` 报告
`GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT: 32`（SSBO 为 64）。
trace 的 UBO offset 有 16 字节粒度，但未找到录制时 alignment 查询值；不能声称捕获日志明确报告 16。

`MobileGL/MG_Impl/GLImpl/Buffer/GL_Buffer.cpp:1553` 的
`ValidateBufferRangeOffsetAndSize` 查询当前 context 的 alignment；`:1580` 判断 offset 整除性，
不符合时记录 `InvalidValue` 并返回 false。`:1627` 在 `TouchBufferBindingPoint` 和修改绑定之前返回，
因此保留之前合法的 buffer/range。远端 client 采用远端能力，不能用 WSL 原生 GPU 的较小对齐要求解释该分支。

`MobileGL/MG_State/GLState/ErrorState/Error.cpp:20` 仅以 `MGLOG_D` 打印 GL error。
现有 INFO 日志没有逐次 `RecordError` 文本；下列“应被拒绝”是调用参数加现行代码得到的预测，
不是声称已有逐 call 动态错误日志。

完整只读 `apitrace dump` 中有 14,149 次 `glBindBufferRange`，没有 `glBindBuffersRange`。
其中 **3,878** 次满足 `target=GL_UNIFORM_BUFFER && offset % 32 != 0`，首 call `9683`，
末 call `205116`；全部发生在 program 158（2,808 次）或 program 15（1,070 次）。
program 158/15 的 uniform block 0 都映射到 UBO binding 0（call `6734`/`6661`）。

这两个 program 的顶点 shader 13（call `6651`）明确声明 `SpriteAnimationInfo`，内含
`ProjectionMatrix`、`SpriteMatrix`、`UPadding`、`VPadding`、`MipMapLevel`，并计算
`gl_Position = ProjectionMatrix * SpriteMatrix * vec4(positions[index], 0, 1)`。
program 158 的 fragment shader 157（call `6727`）通过 `textureLod(Sprite, texCoord0, MipMapLevel)`
填充 sprite；program 15 通过两个 sprite 采样做动画插值。由此可将不合对齐的绑定定位到 atlas 生成，
而非笼统归因于最终主菜单 draw。

与缺失按钮直接相关的调用链如下：

| call | 事实 |
|---|---|
| `80415` | FBO 16 的 COLOR_ATTACHMENT0 设为 texture 868，后续 viewport 1024×1024 |
| `80426`, `80428` | UBO buffer 44 / offset 0 / size 140，随后 atlas draw |
| `80434`, `80436` | UBO buffer 44 / offset 144 / size 140（不满足 32），随后 atlas draw |
| `80442`, `80444` | UBO buffer 44 / offset 288 / size 140，随后 atlas draw |
| `205215`, `205221` | program 181 采样 texture 868，DrawElementsBaseVertex count 90 / basevertex 4 |
| `205254`, `205262` | program 181 再采样 texture 868，count 12 / basevertex 68 |

FBO 16 一共 607 次 atlas draw，其中 286 次前置 UBO bind 不满足 32 字节对齐；
该 FBO 最后一次不合对齐绑定为 `197198`（buffer 43 / offset 144 / size 144）。
最终 GUI program 181 使用的 UBO offset 是 0 或 160，本身满足 32。
它的 fragment shader 27 对 `texture(Sampler0, texCoord0)` 的零 alpha 执行 discard。

**获得独立负控支持的机制：** atlas 生成时，非法绑定保留旧 `SpriteMatrix`，新 sprite 仍写入旧区域，目标格留空或错误；
最终 GUI 从 texture 868 采样时因零 alpha 丢弃，从而出现按钮文字正常、底图/图标缺失。
下面的 blob/UV 对应进一步把缺失区域映射到具体 atlas draw。独立负控已复现主要视觉缺陷，
与设备失败图 SSIM 为 0.9987292763；这不声称两个驱动的全部像素完全一致。

只读 `apitrace dump --blobs --calls=80413,205170,205177` 把诊断副本写到
`/tmp/p65-mainmenu-blobs-ub31dw3f`。`80413` 为 buffer 44 的 69,264 字节初始化内容，
每个 `SpriteAnimationInfo` record 以 144 字节步进；`205170`/`205177` 分别是最终 buffer 39
中 15 个按钮底图 quad 和两个图标 quad 的顶点内容（每顶点 24 字节）。
将顶点 UV 乘 atlas 尺寸 1024，再与 UBO 内的 SpriteMatrix 比较，得到：

| GUI 元素 / 最终采样像素区域 | buffer 44 record / atlas 矩形 | 生成调用与对齐 |
|---|---|---|
| 15 个底图 quad，包括三行主按钮及底部四个按钮切片；x=723…923, y=1…21 | record 279，矩形 (722, 0, 202, 22) | bind `82658` offset=40176，余数 16；draw `82660`，源 texture 1168 |
| 语言图标；x=659…674, y=122…137 | record 125，矩形 (658, 121, 17, 17) | bind `81426` offset=18000，余数 16；draw `81428`，源 texture 1014 |
| 右侧辅助功能图标；x=659…674, y=88…103 | record 148，矩形 (658, 87, 17, 17) | bind `81610` offset=21312 合法；draw `81612` 后紧跟 record 149 |

record 149 原定矩形为 (658, 155, 17, 17)，bind `81618` offset=21456 余数 16，
draw `81620` 的源 texture 1038。如果该 bind 被拒绝，它沿用 record 148 的合法矩阵，
因而会覆盖右侧图标刚生成的位置。按钮背景被拒绝时沿用 record 278 的矩形 (376, 194, 22, 22)，
语言图标则沿用 record 124 的矩形 (610, 287, 18, 18)，都不是最终 GUI 预期采样的区域。

## 独立控制与证据边界

原 trace 的同 ABI TCP loopback 已通过。独立代理根据
`/home/swung/p65-mainmenu-ubo32-rejected-calls.json` 的 3,878 个 call 编号，
仅在临时 trace 副本中移除这些本应被设备 client 拒绝的绑定，保留对应 draw，然后做主机 TCP 负控。
该主机结果复现了设备的按钮矩形、语言按钮和右侧图标缺失。

| 图像比较 | 全图 RGB SSIM | 对原 golden 的判断 |
|---|---:|---|
| 原 trace 主机 loopback vs 原 golden | 0.9934749217 | PASS |
| 原 trace 设备 TCP vs 原 golden | 0.8901610899 | FAIL |
| 临时删绑定 trace 主机 TCP vs 原 golden | 0.8892457298 | FAIL；仅诊断负控 |
| 临时删绑定 trace 主机 TCP vs 原 trace 设备 TCP | 0.9987292763 | 比较两张失败图，不是 golden 验收 |

比较使用项目 `trace_replay_core.cpp` 的全图 RGB SSIM（C1=6.5025、C2=58.5225），
不 crop、忽略 alpha，且已校验原设备和原 loopback 的 runner 结果。四张图像的路径与 SHA256
均保存在 `/home/swung/p65-mainmenu-ubo32-diagnostic/comparison-summary.json`。

处理过程和逐 call 验证保存在
`/home/swung/p65-mainmenu-ubo32-diagnostic/preparation-summary.json`。
安装的 apitrace 14.0 不支持 `--exact`；实际使用显式保留 call 补集的
`trim --calls=@keep-calls.txt`，并核实该版本直接写出所选调用，没有增加依赖闭包。
随后完整 dump 两份 trace，逐项比较所有保留调用的顺序、thread、参数和每个 blob 的 SHA；
仅规范化 call 编号前缀和 blob 文件名，确认除名单中的 3,878 个绑定之外没有变化，headers 也一致。
原 trace 共 205,348 个 call，临时 trace 共 201,470 个，最后的 `eglSwapBuffers` 从
`205347` 重编号为 `201469`。临时 trace SHA256 为
`92e47f93ddcb7b5f3eaece05229fa8980e92adc5d24bbff3d6fe1c1b948ef6b4`。

同目录 `replay-summary.json` 记录负控用时 12.188 秒、returncode=6，实际 ARMED，
无 lockstep/DISARMED，双角色均为 TCP+stream 且 Fatal=0；结束后 supervisor 正常停止、
state 移除且端口释放。该负控使用与原 loopback 相同 SHA256 的主机库。
`apitrace-tool-evidence.json` 保留版本和 `--exact` 不支持的实际输出，`README.md` 汇总复现步骤。
原 trace 与 golden 在负控回放后重新计算 SHA256，确认均未改变。

这一负控、远端 alignment=32、frontend 拒绝路径以及精确 UV 对应共同强烈支持
**捕获 trace 对目标 GPU 的 UBO 对齐有效性问题**。它是独立于原设备 TCP 回放的机制复现，
不能把修改后的 trace 当作正式 GLES 阳性，也不能将正式 case 改记 PASS。
**相同 APK/后端的设备本地 inproc/spawn 原 trace 对照尚未执行**；需待正式设备矩阵释放设备后再补，
本记录没有将它写成已完成，也不把负控宣称为完全排除所有跨机器差异。

原 trace 保持不动，SHA256：
`7abd871cce098e93dd15a5e799fdfe093d9c85784716b00fd97c94833b8b83af`。
原 golden SHA256：`ea8db1c1c1414d3b564d65b1559a708277417b06895900bc947644d2f658bb86`。

两条 `GL_STENCIL_INDEX8` NormalizePixelFormat ERROR 及 layered-blit 探测 warning 也见于通过的
1.21.4 startup/main-menu，因此不把它们单独作为此失败原因。历史 P4a 的 1.21.11 depth-sampler
失败有独立的 sampler refusal 证据，现日志没有同一 refusal；不能把旧原因直接套用到本次。
