# W4 复验 · p7w4（X1+F1+C+A+B+D1）真机：门 3 AFTER 矩阵、iterationt、OpenRA 改判为纯竞态

APK `p7w4-8c5dd6fc`（`apk-p7w4.sha256` = `22472dc5…`；`apk-p7w4-install.txt`：`versionName=26.09.8c5dd6f-trace`），= `feat/disaggregated@8c5dd6fc` = wave 0 + X1 + F1 + C + A + B + D1。并排包 `top.mobilegl.plugin.p7w1.trace` 原地升级。全部 DirectVulkan × inproc × pbuffer；`--archive-dir .trace-work/p7w4/<run>`（不入库）。

## 1. iterationt ×2：`Fatal{UnmigratedVerb,"Magma:mipmap-shader-format-or-shape"}` 退役（`iterationt.log`）

| case | E0a（修前） | W4 | monolith（E0a） |
|---|---|---|---|
| `iris-iterationt-in-world` | Fatal | **0.999419883** | 0.999421604 |
| `iris-iterationt-nodsa-in-world` | Fatal | **0.998933123** | 0.998936755 |

B 的 mip 形状半退役 + 深度 mip 烘焙（ID-P7-23）在真机上兑现；两例与 monolith 差 ≤ 4e-6。

## 2. 门 3 AFTER：39 例 inproc 矩阵（`matrix-inproc.log`、`matrix-inproc-summary.{txt,json}`）

分母 36（ID-P7-12）逐 case 对 E0a 的 monolith 读数：**34/36 差 ≤ 0.0005**（23 例到第 9 位相同，其余差 ≤ 5e-6：create-instancing −1e-7、bliss +3e-9、derivative +1.4e-6、iterationt −1.7e-6/−3.6e-6、sundial +5.2e-6）。未过的两例：

| case | W4 | monolith | 读法 |
|---|---|---|---|
| `OpenRA` | 0.976493989（本轮 1 遍） | 1.000000 | 见 §3，竞态 |
| `iris-bsl-esc-menu-854` | 无 result.json（`1 error`） | 0.999791667 | inproc 下 scudo OOM（E0a 起如此），待 spawn 臂复测（wave 4） |

分母外三例与 E0a 一致：create-indirect 0.832（OQ-17）、1.21.11-main-menu 0.165（trace 伪差）、photon-v1.3b 0.0089（ID-P7-13）。矩阵本身 1 遍；§7.2 要求的「三遍逐位相同 + spawn 臂」留 wave 4 终局跑。

## 3. OpenRA：ID-P7-19 的「冷驱动缓存」读法撤回——是与温度无关的每次约 50% 的竞态

同一 APK 16 遍（`openra-cold-{1,2,3}.log`、`audit-*.log`、`fif1-*.log`、`plain-*.log`、`stats-*.log`）。cold = 跑前 `pm clear`，warm = 不清。

| run | 冷/热 | 额外环境 | ssim | 错像素 | actual sha256（前 8） |
|---|---|---|---|---|---|
| openra-cold-1 | cold | – | 1.000000 | 0 | `ace2af04` |
| openra-cold-2 | cold | – | 0.976494 | 14658 | `fb75d412` |
| openra-cold-3 | cold | – | 0.976494 | 14658 | `fb75d412` |
| audit-cold-1 | cold | `MOBILEGL_IPC_AUDIT=1` | （Windows adb daemon 短暂失联，无结果） | | |
| audit-cold-2 | cold | `MOBILEGL_IPC_AUDIT=1` | 0.976494 | 14658 | `fb75d412` |
| audit-warm-1 | warm | `MOBILEGL_IPC_AUDIT=1` | 0.976494 | 14658 | `fb75d412` |
| fif1-cold-1 | cold | `MOBILEGL_MAGMA_FRAMESINFLIGHT=1` | 1.000000 | 0 | `ace2af04` |
| fif1-cold-2 | cold | `MOBILEGL_MAGMA_FRAMESINFLIGHT=1` | 0.976494 | 14658 | `fb75d412` |
| plain-cold-4 | cold | – | 1.000000 | 0 | `ace2af04` |
| plain-cold-5 | cold | – | 0.976494 | 14658 | `fb75d412` |
| plain-warm-1 | warm | – | 0.976494 | 14658 | `fb75d412` |
| stats-cold-1 | cold | `MOBILEGL_PIPE_STATS=1 …PERIOD=1` | 1.000000 | 0 | `ace2af04` |
| stats-cold-2 | cold | 同上 | 1.000000 | 0 | `ace2af04` |
| stats-warm-1 | warm | 同上 | 1.000000 | 0 | `ace2af04` |
| stats-audit-warm-1 | warm | 同上 + `IPC_AUDIT=1` | 1.000000 | 0 | `ace2af04` |
| stats-lockstep-cold-1 | cold | 同上 + `MOBILEGL_IPC_RUN_AHEAD=0` | **0.988998** | **9836** | **`4e5ba514`** |

读法：

1. **run-ahead 臂只产生两张图**：`ace2af04`（= golden）与 `fb75d412`（14658 像素错）。冷 5/10 过、热 2/4 过——温度无关；`IPC_AUDIT=1`（退役 staging 填 `0xDD`）不改变错图、`FIF=1` 不改变错图。错图与 E7（p7w3，B 修复前）的错图**逐字节相同**：B 的回读 fence 修复（ID-P7-23）堵的是真洞，但不是这一个。E7 的「冷三红、热一绿」是 4 次样本的巧合。
2. **lockstep（`RUN_AHEAD=0`）确定性地给第三张图** `4e5ba514`（0.988998 = E1 的数）。任何机制都要同时解释三张图。
3. 目标调用 31249 = `glXSwapBuffers`；apitrace 在 swap **之前**用普通 `glReadPixels` 取快照（`apitrace_glproc_android.cpp:49`）→ 记录流 → `ReadWirePixels`；present 是带 credit 的 class-B 记录（`ServerLoop.cpp:1048-1052`），没有旁路能把 swap 排到回读前面。
4. diff 只在会逐帧动画的精灵矩形里（侧栏建造面板、顶部 HUD、若干单位、tooltip）——最像「上一帧的画面」或「本帧最后几批没画」。两臂 client/server 日志逐行相同、零 Fatal。
5. `PIPE_STATS=1`（apply 线程每帧多一行日志）下 4/4 绿——与「时序窗口被额外日志关上」一致，样本小。

归因交 wave 2 的 B3 调查（四镜头 + 裁判 workflow，证据副本 `~/w7/notes/evidence/w4/`，含 `FACTS-2.md`）。**门 3 对 OpenRA 的验收改为**：run-ahead 与 lockstep 各 6 遍全部 `ace2af04`（1.000000 / 0 像素），冷热各半。

## 3b. 后续三段会话：不是旧帧、不是错纹理，是**被静默丢掉的 draw**

| run | 臂 | 额外 | ssim / 错像素 | actual | 调色板 dump（texture 3 @31249） |
|---|---|---|---|---|---|
| prevframe-monolith | monolith，快照改在上一帧 swap（call 30194） | – | 1.000000 / 0 | `ace2af04` | – |
| spawn-1 / 2 / 3 | **spawn（两进程）** | – | 0.976494 / 1.0 / 0.976494 | `fb75d412` / `ace2af04` / `fb75d412` | – |
| dump-inproc-1..4 | inproc | `--dump-texture-2d 31249,3,0,…` | 0.976494 ×4 | `fb75d412` ×4 | （未留存） |
| dump-monolith-1 | monolith | 同上 | 1.000000 | `ace2af04` | `de4b5f9d91c9`（hash `71d5b6a28af0d3e2`） |
| dump2-inproc-1 / 2 / 3 | inproc | 同上 | **0.988998** / 0.976494 / 0.976494 | `4e5ba514` / `fb75d412` / `fb75d412` | `de4b5f9d91c9` ×3 |
| dump2-spawn-1 / 2 | spawn | 同上 | 0.976494 / **0.988998** | `fb75d412` / `4e5ba514` | `de4b5f9d91c9` ×2 |

读法：

1. **不是旧帧**：monolith 在 call 30194 的图 = golden（场景静态），所以两张错图都是本帧、少了东西。
2. **不是错纹理**：调色板（texture 3，256×32 BGRA，每帧 call 30232+1055k 用 `glTexImage2D` 重指定）在快照时刻的内容在所有失败跑里与 monolith 逐字节相同。
3. **是被丢掉的 draw**：调查镜头 A 对 `fb75d412` 的 diff 做连通域——正好 9 块 = 14658 像素，每块 golden 有精灵（油井、管线、石堆、单位、贴花）而 actual 是干净的地形；同一油井贴图在 (240,120)/(560,70) 缺、在 (395,378) 在——**按 draw 丢，不按纹理丢**。两臂日志逐行相同 = 丢失路径没有任何日志（rule I 的反例）。镜头 A 指认的无日志路径：`SetupWireDraw`（`VulkanRenderer.cpp:12744-12747` 裸 `return;`）← `PrepareWireTextureResources`（`WireDraw.inc:230`）← `SyncTextureResourceByHandle`（`UniformManager.cpp:2971`）← `UploadPendingWireLevels` 五个 false 出口（`VkTextureManager.cpp:2404-2508`）。每帧重指定的调色板正好在这条路上。
4. **两进程臂同样掷硬币**（spawn 3 遍 红/绿/红），且第三张图 `4e5ba514`（9836 像素）在 run-ahead 下也出现——丢掉的 draw 段有两种量化长度。归因与修复交 **B3**（`p7-magma-b3`）；主机 red-once 走 `integration-magma-spawn`。
5. 附带：Magma 从不发布 `kCapRunAheadApply`，DirectVulkan 客户端本来就是逐记录 lockstep（`ClientSession.cpp:1984-1988`），`RUN_AHEAD=0` 改变的只是时序。

## 4. 会话卫生

每次 `pm clear` 都杀掉设备上的 TCP supervisor（窗口 1b 用）；每段会话末尾用 `tcp_device_server.py start …`（runbook §7 的命令）重启并确认 `0.0.0.0:40613` 在听。`audit-cold-1` 的空结果是 Windows adb daemon 在 WSL adb 启动 supervisor 后短暂重启造成，与被测无关。
