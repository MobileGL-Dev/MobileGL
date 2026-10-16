# Magma run-ahead

**已完成，2026-09-20**。最终生产与实机行为头 `194382c96a8a5f51ef23f412fdccfbd40a82bf78`。

后续 `70fb6689` 已修复 RD32 下的 Magma inproc 性能差距：**86.85→116.73 FPS**，
同包 monolith **115.01 FPS**；根因、缓存生命周期与复测见 [性能修复报告](magma-rd32-performance-fix.md)。

后续同包的 **render distance 32 四组合性能测量**见 [rd32-fourway.md](rd32-fourway.md)。
该轮默认动态频率下，Espryt monolith/inproc 为62.18/64.02 FPS，Magma 为116.08/86.85 FPS；
不要将下面的功能验证窗口替代这份高负载性能记录。

本次是在已完成的 P5f 与 Magma inproc 游戏修复上继续实现异步排队，
不是重开 P5f 包，也不宣称 P6 spawn 或 P7/P8 全量实现。
契约见 `MobileGL/MG_Remote/CONTRACT-MAGMA-RUNAHEAD.md`。

## 实现

- Magma 独立 readiness 条件满足后发布 `kCapRunAheadApply`，复用既有客户端闩、
  wait class、反向事件流控和 present credit。`MOBILEGL_IPC_RUN_AHEAD=0` 保留为配对对照。
  reply、显式同步和 credit 耗尽的等待仍保留。
- wire draw/clear/color blit 不再逐 draw 提交并 `vkQueueWaitIdle`。render pass、
  framebuffer、view 和 blit descriptor pool 按实际 submit index 延迟释放；
  Present、重建和 shutdown 处理最后一批对象。独立 blit 不复写在途 descriptor set。
- texture 上传和 GPU 内容保留提交不能越过旧 draw；draw/dispatch 在取得当前
  command buffer 前准备 image、sampled texture 和 FBO attachments。native blit
  后同时失效 pipeline/dynamic state 与 uniform manager 的 descriptor 绑定缓存。
- GPU buffer readback 后保留后续 draw 已预约的 slice；CPU draw preparation 期间
  不允许 idle-drain 回卷 transient arena。同 layout image transition 保留内存依赖。
- compute work-group 上限由本地 CapsMirror 回答，不再通过三个 getter 插入无谓
  barrier。修改限于 transport 的两个 compute COUNT/SIZE 查询。
- Vulkan 验证层发现并修复 descriptor indexing API/extension 启用条件、含 dynamic
  UBO layout 的非法 UPDATE_AFTER_BIND、HOST_WRITE stage 和 Present layout transition
  的写后写依赖。wire 使用已有按内容版本分配的 descriptor set，不原地修改在途 set。
- 最小化窗口丢弃 recording 后，只有 CPU preparation、renderer submit 和纹理独立
  upload fence 都空闲才回收 future-tag 对象。纹理 fence 只轮询，不新增 queue wait；
  同时避免对象持续累积和误把 renderer watermark 当作整个队列的 idle 证明。

## Redmi 发现的 VAO 状态漏发

初版 `3c97ea9c` run-ahead 进入世界后触发 `Magma:vertex-layout@P7`。
`6f764c42` 诊断包关闭 run-ahead 仍复现：enabled attribute 1 对应的 buffer window
却是 `0+1`。因此没有删去边界检查，也没有把它当作驱动格式限制绕过。

根因是 dirty tracker 把 `{VAO lifetime, configVersion}` 用普通 hash-combine
压成一个值判断身份变化。普通整数对会碰撞；Redmi 实际记录的两个身份为
`174/37` 与 `48/8026`。缓冲家族因 aggregate 变化发布了新窗口，CSO 家族却
漏发 bind，形成旧布局与新窗口组合。现在精确比较两个字段，变化时重新检查
三个 vertex families，各 emitter 仍可抑制自身未变化的记录。

新测试通过合法 setter 构造真实 VAO 的碰撞配置，经实际 validate 路径检查
`A(0+1 enabled) → B(0 enabled) → A` 的绑定及窗口 `2→1→2`，不改私有计数器。
候选修复 `376c04be` 已在 Redmi 捕获碰撞被修复的日志，世界连续运行 64.167 秒，
验图正常。该诊断包不是最终交付验收包；初版失败证据保留。

## 主机验证

七种实际排队场景各测 present credit 1/3，共 14 项：
buffer SubData/respecify/delete-name-reuse、program/uniform 快照、SSBO 写回读、
native 与 double-converted vertex slice、旧 texture draw 与后续上传的顺序、
GPU clear 后首次 storage image 升级、credit 用完时客户端真实停车。
用已有 apply hook 暂停真实服务端，观察真实 emit/applied watermark 和 producerParked，
不伪造 capability/sequence，也不把 ARMED 日志当作排队证明。

关闭 run-ahead 的负控同一 testcase 必须实际等 apply、失去 client lead 并失败，
恢复后转绿。同步验证必须证明 Khronos layer 被 loader 插入，完整执行 14 项，
且零 VUID / SYNC-HAZARD / validation error。两项均已接入 CI，脚本为
`scripts/ci/magma_runahead_checks.py`。

RSP 测试采样也修正了真实的 fixture race：异步 EndFrame 返回不代表 Present
统计已发布，测试现在等该条已发出的 Present applied 后再读日志；生产未加等待。

最终行为头的结果如下，skip 不计 PASS，完整发现集与 JUnit 核对：

| 门 | 总数 | PASS | skip | failed |
|---|---:|---:|---:|---:|
| unit | 2312 | 2302 | 10 | 0 |
| integration-gpu | 1401 | 1119 | 282 | 0 |
| strict integration-split | 180 | 176 | 4 | 0 |
| 双块 split + magma | 245 | 239 | 6 | 0 |
| 其中 run-ahead 专项 | 14 | 14 | 0 | 0 |
| 其中 buffer 专项 | 19 | 19 | 0 | 0 |
| 其中两后端逐帧 RSP | 2 | 2 | 0 | 0 |
| Khronos synchronization validation | 14 | 14 | 0 | 0 |

- unit / GPU / 双块的 skip 名称与前一轮完整结果集精确相同；普通 GPU 发现的 14 个
  run-ahead 副本只在专用车道运行，因此在那里按 fixture 声明 skip，专用 14 项均执行。
- strict marker 与双块 fatal 棘轮均空；初版负控与最终 run-ahead-off 负控均真实红转绿。
  Tracker 碰撞回归临时撤掉 exact-pair dirty 分支后 rc=1，恢复后 rc=0；改动已还原。
- Vulkan layer 实际插入 14 次，零 VUID / SYNC-HAZARD / validation error。日志另外保留
  28 条既有 MobileGL `GL_STENCIL_INDEX8` caps 枚举应用 ERROR；它们不是 layer 消息，
  checker 按来源分列，含 Vulkan 标识的消息和 loader ERROR 仍硬失败。
- pull / push 构建通过；G1 `.text 10806051 → 10806051`，27815 defined symbols，
  added/removed/resized/renamed **0/0/0/0**。G2 **3017 == 3017**。
  G14 完整 JSON 名称集合 **3774 → 3803，+29 / -0**。
- G1 上述结论是仓库脚本的 section size / symbol name / symbol size 比较，
  不声称不同构建的原始 `.text` 逐字节相等。额外只读反汇编核对发现原始差异全部
  来自元数据：7255 条 RIP 引用因生成的 SPIRV-Cross version 字符串缩短7字节而
  位移，解引用字符串相同；107 条 immediate 对应同一源码文本的 `__LINE__` 位移。
  生成的版本字符串和时间戳也不同，未把这种构建可复现性差异记成运行逻辑变化。
- wire/field/dirty generators、field self-test 与四个 include closure probes 全过。
  证据目录 `final-gates-194382c9/`、`negative-194382c9/`、`syncval-194382c9/`、
  `tracker-red-once-194382c9/`，均在上述主机日志根目录下。

## FCL 实机验证

设备限定 Redmi `2f7cbe2e`，包名 `com.tungsten.fcl.mgdebug.debug`，
Minecraft `26.3-rc-3`，世界 `test`。复现脚本与准备/恢复步骤见
`tools/device_bench/magma_runahead/README.md`。

最终 APK `FCL-magma-runahead-194382c9.apk` 已覆盖安装，保留游戏数据：

- APK SHA256：`69973e2f51b87bd0128420da62b92fa2fb862a11067baf1b54a142f971d13357`
- `libMobileGL.so` SHA256：`046d86c288ce891b16770148578ba1a6fea4057783b69c2359f5830e58b7b223`
- APK 内与 `/data/app/.../lib/arm64/libMobileGL.so` 哈希一致，记录在
  `final-artifact-identity.json`。三臂均使用此 APK，分别重启设备后运行。

| 臂 | 实际能力 | 世界连续运行 | 帧增量 / 近似 FPS | 图像 |
|---|---|---:|---:|---|
| Magma inproc RA=1 | ARMED | 64.472 s | 7680 / 119.122 | 地形、天空、手、hotbar 与方向正常 |
| Magma inproc RA=0 | 实际 Config run-ahead=0，无 ARMED | 60.589 s | 7320 / 120.813 | 同上 |
| GLES inproc RA=1 | ARMED | 60.865 s | 7080 / 116.323 | 同上 |

三臂数据与截图位于 `final-Magma-on/`、`final-Magma-off/`、`final-GLES-on/`。
Magma-on 有一次 `adb pidof` 查询超时（rc=124），随后仍为同一 PID `10141`，
帧与日志持续推进、无进程重启或 crash；不将它写成应用退出。
三臂共 193 个 stats windows 全部 `rsp=0`，strict=1、role-split-state=1，
六张世界截图均已检查；证据结论见各目录 `screenshot-review.json` 和汇总
`final-device-verdict.json`。三个不同 boot-id 分别为
`300c0044-058f-4c34-9e88-6e888d834877`、`bf9a1b6c-ce58-4ca6-bc71-8da52a856684`、
`f61e1c51-e354-4867-b708-72ca463e2119`。

原 `mg_env.txt`（空文件）和 `mg_transport.txt`（`inproc\n`）已恢复并逐一核对
SHA256，世界已保存退出；没有回滚世界内容。用户两份 Gradle 配置及父 FCL 的
原有未合并索引未改动。本轮未将库/APK 二进制提交到 Git。

所有主机原始日志保留在 `/home/swung/w7/magma-ra-logs/`；设备证据在
`C:/Users/geekerwan/.codex/tmp/magma-runahead/`。静态镜头、未锁频、天气和温度不受控，
帧计数只能作本次运行记录，不能把所有提升都归因于客户端 run-ahead；
本次还同时取消了 GPU 逐 draw 等待。具名 P7/P8 shape refusals 继续保留。

本次 RA=1 与 RA=0 的近似均值接近，不能据此宣称客户端 run-ahead 有特定 FPS 收益；
能确认的是所测世界已能在约 120 FPS 水平运行，真实排队与 credit 语义由主机专项证明。
