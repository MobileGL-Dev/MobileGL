# Magma inproc 实际游戏修复

> 历史行为头 `38919d45`，分支 `codex/magma-inproc`；本报告记录 P5f 后的实际游戏补修。
> 后续 `194382c9` 已完成 [Magma run-ahead](magma-runahead.md)，本报告保留当时 lockstep 数据。
> Redmi Magma inproc 已通过运行与人工图像检查；同 APK 的 GLES inproc 与 Magma monolith 复跑、最终 G1/G2/G14 均已通过。
> 功能修复、主机门与真机验证完成；保留以下性能和功能边界，未宣称 P7 全阶段收官。

## 1. 问题与修复范围

P5f 原设备门验证的是公开像素子集，Magma 的应用 buffer 路径仍具名拒绝。
Minecraft 26.3-rc-3 真正需要 VBO/EBO、UBO range、persistent writes 和 indirect draws；
因此“原像素门通过”不能推出游戏可用。本轮按用户要求补上这些 server record 消费路径。

`VkBufferManager` 新增按完整 `{slot,gen}` 管理的 server buffer storage 和 resource ops，
实际发布 buffer consumer bit。SubData 原位更新；busy store 使用 staging copy 或等待后精确
写入，不拿 client 旧影子覆盖 GPU 结果。GPU-written / readback 走已有反向事件，persistent
map 保持 client-owned T2 emulation，不把 server 映射地址交给 client。

vertex/index 绑定从 wire VAO、逐属性 buffer records 和 server slices 解析，支持现有
格式转换、offset/stride、base vertex/instance；indirect 与 line-loop 消费 server buffer 字节。
UBO/SSBO/atomic/texel descriptors 从记录的 handle、range 和 program reflection 解析；
短 UBO 先零填完整反射块，再只复制绑定 range 内的 GPU 字节，避免 range 外毒值进入 shader。

Magma 继续不发布 `kCapRunAheadApply`，仍按 lockstep 生命周期运行。真实 bootstrap 单测同时
检查 resource ops 与 consumer bits，不能把“支持 buffer”误写成“支持 run-ahead”。

## 2. 保留中间失败证据

最初将 17 条新 buffer 用例接到旧实现，**构建成功、17 条实际全红**，主要在旧 buffer 守卫
abort。日志 `magma-buffer-baseline-red.log/xml` 保留；没有先改成弱断言来获得绿灯。

`31dc1a73` 修好 buffer 后，17 条门、unit/GPU 都绿，手机也能进世界，**但设备结果 NOT PASS**：
画面左半黑、右侧约 90° 旋转，并有条纹和彩噪。该阶段证据保存在
`C:/Users/geekerwan/.codex/tmp/magma-inproc-fix/Magma-inproc-31dc1a73-run/`，不能由后续成功覆盖。

根因是 wire color blit 将 logical `2620×1280` 矩形直接写入 native `1280×2620`、
`ROTATE_90` swapchain。monolith 的旧 shader 路线曾处理该转换，wire 路线没有继承。
`38919d45` 加入纯 native Vulkan shader blit，使用 server-owned shader/pipeline/descriptors，
覆盖 identity/90/180/270，按 native viewport 绘制，并正确处理 GL scissor 和反向矩形；
没有恢复前端 ShaderObject/ProgramObject，也没有添加 test-only transform 开关。

## 3. 主机门与阴性对照

| 门 | 已取得结果 / 适用代码 |
|---|---|
| buffer + default-blit 严格车道 | **19 PASS / 0 skip / 0 failed**；role-split=1、strict=1；原17条全部保留。 |
| unit，`31dc1a73` | **2311：2301 PASS / 10 既有 skip / 0 failed**。 |
| 最终 integration-gpu | **1387：1119 PASS / 268 skip / 0 failed**（`magma-final-gpu.xml`）。 |
| 最终双块 split + Magma | **231：225 PASS / 6 既有 skip / 0 failed**；空字段 Fatal 棘轮（`magma-final-dualblock.xml`）。 |
| strict / 每帧 RSP，`31dc1a73` | strict **180 条零失败**；RSP 两后端 **2 PASS / 0 skip**。 |
| G1，`38919d45` | `.text` **10806051 → 10806051**；symbols **27815 → 27815**，增/删/变长/改名 **0/0/0/0**。 |
| G2，`38919d45` | pull / push 各 **3016** 名，集合相同。 |
| G14，`38919d45` | 审计基线 **3744 → 3774**，新增30、删除0；其中3768→3774是完整JSON机比，历史证据限制见下。 |

G14 的历史机读集合由旧 XML/lane JSON 重建出3741名，另3名由 `aa78f102` 注册与未变测试
源码补证；**不是对一份完整保存的3744名历史 CTest JSON 直接比较**。明细在
`~/w7/p5f-logs/magma-final-summary.log`、`magma-g14.json` 和 `magma-final-names.log`。
最终 pull/push 构建、生成器检查及字段 self-test 通过；include closure 4项、0skip/0problem。

新增 UBO 用例逐次核 offset、range rebind、handle rebind、SubData 与 block binding；
GPU SSBO 用例先直接消费 GPU 写出的 VBO/EBO 绘制，再精确回读并重画；persistent-map
复用同帧/跨帧变色场景。旧 `EnabledVertexBufferDrawKeepsNamedP7Fatal` 保留注册名，内容已
改成64个真实绿色像素正例。两条 default-blit case 使用非方形四色源，核 viewport 无关、
方向、反向目标矩形以及 scissor 内外像素；host identity 路线确实执行新 native blit。

真实 carrier 阴控：`MOBILEGL_IPC_PERSISTENT_BLOCK_KB=0` 让同一 persistent-map 变色用例
实际失败（rc=8），随后恢复默认旋钮再跑为绿；保留 `magma-persistent-{green,red,restored}.log`，
以及 red XML / 私有日志。CI 固定核19名完整执行，未放宽六项 skip。PackedFloat known-red
也实跑1失败/0skip、rc8，保留其native-format边界（`magma-final-known-red.xml`）。

## 4. Redmi 实际游戏结果

设备 `2f7cbe2e`，既有 FCL 包 `com.tungsten.fcl.mgdebug.debug`，版本 `26.3-rc-3`、世界 `test`。
最终 Magma inproc：**32.083 s 进世界，稳定65.109 s、1320帧，约20.274 fps**；PID `9993`
仍存活，所有记录的 `rsp=0`。约20fps 明显偏慢；本轮是功能修复，不能据此宣称性能合格。

主任务人工检查完整景物、方向、雨效及暂停菜单，原半黑/旋转错位/彩噪消失。
runner 自动 summary 仍保留 `PASS_PENDING_SCREENSHOT_REVIEW`，人工结论单列于此，不篡改原证据。
证据目录：`C:/Users/geekerwan/.codex/tmp/magma-inproc-fix/Magma-inproc-38919d45/`，含
`final.png`、`pause.png`、游戏/library/logcat日志、boot-id、PID生命周期与 `summary.json`。

- 最终设备 `libMobileGL.so` SHA256：`5a264098f8916e811cd76ce82c3727d4f9a4a426a7b68b2fa812221e2ea41383`。
- APK SHA256：`4b42b0f85b8f83e6648cc24665a8b5fbe83e7420b04459abe7507e32e2e8aba6`。
- 同 APK GLES inproc：32.835秒进世界、60.287秒稳定窗口、10920帧，约181.134 FPS，人工验图通过。
- 同 APK Magma monolith：32.795秒进世界、61.574秒稳定窗口、7080帧，约114.984 FPS，人工验图通过。
- 三组均无定频，天气/时段在变化；FPS仅记录，不能据此建立严格性能倍率。三组rsp窗口均为零。
- 分工独立复核覆盖了buffer同步/生命周期、binding布局、native blit资源销毁、push constants/SPIR-V及状态失效；没有放宽角色守卫。
- 额外CLI只读审查扫描未返回最终结论，已停止，**不计为通过的审查门**。工具元数据实际模型为gpt-5.6-sol，未冒称异模型族；状态保存在证据根`review/status.json`。本轮不是P7阶段收官，未替换原P5f已完成的阶段终审。

## 5. 仍保留的边界

XFB buffer capture 仍在 `buffer-legacy-arm` 边界；client vertex arrays 仍属 P8。需要 UBO
重打包但起点/复制尾部不能满足4字节对齐时，仍为 `uniform-buffer-byte-tail@P7`；不满足
native alignment 的 SSBO/atomic/texel range，以及超出 native range/format 的形状仍具名拒绝，
未实现 writable range 的额外 copyback 方案。PackedFloat mipmap 与其他已登记格式债继续保留。
这次游戏可用不等于 P7 全部完成，也没有改变 P6/P12 的跨进程真窗口边界。

## 6. 交付与设备状态

修复已合入`feat/disaggregated`；被测APK保存在
`C:/Users/geekerwan/.codex/tmp/magma-inproc-fix/FCL-magma-inproc-38919d45.apk`，已安装到指定FCL包。
每组有不同boot-id，设备实际库hash与APK一致；三组共173个stats窗口全部rsp=0，只有inproc组
有真实vbs。人工验图汇总`final-device-summary.json`保留机器原始summary，不篡改其待验图标记。

测试结束从菜单保存退出，恢复原`mg_transport.txt=inproc`和空`mg_env.txt`并核hash。
现有FCL测试接线只有Espryt选项读这两个文件，Magma inproc测试通过该入口覆盖BACKEND_TYPE；
直接选择旧内置Magma选项仍默认monolith，不能只凭UI名称判断transport。父仓原冲突索引、
用户Gradle修改和游戏数据未清理；本轮没有修改FCL源代码或推进P6。
