# Magma RD32 inproc 性能修复

**已完成，2026-09-20**。生产行为头 `70fb6689`，之后仅追加 CI 脚本、契约与报告。
原 [RD32 四组合记录](rd32-fourway.md) 的 Magma inproc 86.85 FPS / monolith 116.08 FPS
是本次基线，保留原数据。修复后的同包对照已回到约115–117 FPS，同一场景的25%差距消失。

## 结果

| 模式 | 修复前平均 FPS | 修复后平均 FPS | 修复后区间中位数 | 修复后区间 P10 |
|---|---:|---:|---:|---:|
| Magma-monolith | 116.08 | 115.01 | 115.00 | 113.00 |
| Magma-inproc | 86.85 | 116.73 | 117.00 | 115.00 |

inproc 相比旧实现平均提升 **34.4%**；同包 inproc 比 monolith 高约1.5%。只有单轮，
不能据此认定稳定领先；本场景已不再呈现原先约25%的差距，两者均接近120 FPS量级。
不宣称无上限吞吐或零架构开销。
P10为约一秒计数区间的FPS分位，不是逐帧1% low。

## 原因与改动

实机30秒 `simpleperf cpu-clock` 采样中，服务端仍反复
构造和销毁 native GPU 对象：`vkCreateFramebuffer` inclusive占7.91%，
`vkDestroyFramebuffer` 6.21%，`SetupWireDraw` 21.15%。这些是各自采样份额，存在调用层级，
不能把父子项全部相加。客户端与服务端都存在工作/等待交替，不能把两个线程CPU时间相加当帧延迟。

本次修复：

1. **采样/storage image view复用**：UniformManager按帧槽缓存完整创建参数，包括root handle/gen、
   native image与allocation epoch、format/type、swizzle、aspect及mip/layer窗口。逐字段比较，
   不把相同hash当相同视图。句柄稳定后，原descriptor内容缓存可以复用。
2. **draw framebuffer对象复用**：每个frame slot最多缓存32个inactive pass/view/framebuffer组合，
   精确匹配附件身份、窗口、格式、samples、draw-buffer映射和交换链信息。命中仍执行原有
   End/Begin、image transition及memory dependency；溢出走原submit-index退休。
   清理依赖真实slot fence或完整idle证明，发生在texture manager释放旧image之前。
3. **pipeline兼容性身份修复**：原PipelineFactory把临时 `VkRenderPass` 句柄放进缓存key，
   跨帧重建容易重复创建pipeline，句柄回收也存在旧key别名风险。现在用精确formats/samples、
   有序color refs/holes与depth ref分配非零身份，renderer生命周期内不复用ID，
   与native句柄域分离。原句柄只供Vulkan创建调用；wire pipeline不再按creator pass的销毁
   事件回收，继续沿program/age/整体销毁生命周期，避免误删另一个兼容pass在途使用的pipeline。
4. **其余热路径**：缓存物理设备不变的vertex-format支持结果；退休队列由逐项erase的二次搬移
   改为一次稳定压缩。没有移除GPU同步、reply等待、present credit或角色守卫。

修复后独立post-window profile中，framebuffer创建采样份额降到3.56%；旧pipeline创建/
`inflate`热点不再出现在0.1%阈值以上的服务端采样项。仍有每帧必要对象工作，未声称零创建。
服务端样本18,017→13,106，但两份profile处于动态频率、不同运行时刻，不能读成精确同频周期倍率。
计时窗口与profile分开，profile开销没有混入上表的FPS。

## 验证

| 门 | PASS / skip / failed |
|---|---:|
| unit | 2304 / 10 / 0 |
| GPU | 1119 / 296 / 0 |
| strict split | 176 / 4 / 0 |
| 双块 split + Magma | 246 / 6 / 0 |
| cache专项（含Vulkan同步验证） | 7 / 0 / 0 |
| run-ahead专项（含Vulkan同步验证） | 14 / 0 / 0 |
| buffer专项 | 19 / 0 / 0 |

- 新7项公开GL场景覆盖sampler swizzle A→B→A、respec、mip/layer/view type、
  FBO A→B→A、重挂附件、sRGB切换及attachment respec。所有draw先排队，之后才统一读像素。
- 新2项unit覆盖兼容性身份稳定、raw句柄换代与wire/native分域。临时禁止生产ComputeHash的
  wire key分支时，same-ID/different-native-RP断言实际rc1；逐字节恢复后完整构建和该case rc0。
- cache7与run-ahead14均证明Khronos layer实际插入，零VUID/SYNC-HAZARD/validation error。
  既有caps枚举 `GL_STENCIL_INDEX8` 应用诊断仍分列，未掩盖Vulkan错误。
- run-ahead=0负控实际等apply并失去queue lead后失败，恢复1转绿。strict两类marker棘轮为空。
- GPU skip集合精确为旧282加新cache generic双后端14；专用cache7全部执行。无删名或额外skip。
- pull/push构建与生成器、field/dirty self-test、4个include probes全过。
  G1 section-size/defined-symbol比较全0，`.text 10806051`、symbols增/删/变长/改名0/0/0/0；
  不是跨构建原始机器码逐字节承诺。G2 **3019==3019**；G14 **3803→3826，+23/-0**。
- cache同步验证已加入CI，使用 `scripts/ci/magma_cache_checks.py`；入仓版本也独立跑过7/7。

## 实机条件与交付

Redmi `2f7cbe2e`，FCL `com.tungsten.fcl.mgdebug.debug`，Minecraft `26.3-rc-3`，
同一冻结初始世界副本、固定相机、RD32/simulation12、2620×1280、VSync off/maxFps260。
每臂独立重启、入场CPU温度低于40°C、进入世界后预热180秒，再采至少60秒。
最终所选两臂采样窗全部风扇观测均档位2、RPM>0；原始FCL计数按实际相邻日志间隔归一化。

| 模式 | CPU温度 °C | 大核观测 GHz | GPU观测 MHz |
|---|---:|---:|---:|
| Magma-monolith | 55.4–58.9 | 1.690–1.690 | 734–900 |
| Magma-inproc | 57.0–58.1 | 1.210–1.690 | 832–900 |

使用默认DVFS，没有关闭温控。初次候选inproc计时为116.77 FPS，但风扇被系统关闭，
该次计时不纳入最终配对，只保留其post-window profile。最终另跑一次fan2 inproc，
没有把两次结果平均或挑选最好值。新增fan变化检查会重新预热，采样窗若漂移则拒绝该次结果。

两组actual backend/transport/capability、全部stats `rsp=0`、PID与前后画面均核查；
inproc实际ARMED、strict/role-split=1；monolith无IPC配置、vbs=0。

- APK：`C:/Users/geekerwan/.codex/tmp/magma-perf-fix/FCL-magma-perf-70fb6689.apk`
- APK SHA256：`bfa352bcb3402d9a31252503bacf6fcdc21e28ffaaa929fd8c80e6f2e2781e4e`
- 已安装库SHA256：`11f26ca1f1d6c9a3d89134c31b2348306e669b1d9ebcc2fc77c7f5def2b4f818`
- 最终配对证据：`C:/Users/geekerwan/.codex/tmp/magma-perf-fix/final-evidence/`，
  结论文件 `magma-pair.json`；包含source selection、原始计数/日志、4张验图、真实恢复核验。
  通用四臂分析器的 `performance.json` 标2/4是因为本次只测Magma两臂，不能冒称又重测了Espryt。
- profile：`../magma-perf-baseline-profile/` 与 `../magma-perf-candidate70fb-bench/`，
  含原始perf.data、匹配Build ID的符号缓存及报告。测试计时不使用profile期间的FPS。
- 主机证据：`/home/swung/w7/magma-perf-logs/`，前缀 `final-gates-70fb6689`、
  `syncval-caches-70fb6689`、`syncval-runahead-70fb6689`、`negative-70fb6689`、
  `pipeline-key-red-once-70fb`、`ci-cache7-ac1f`。

原世界61个文件已逐项SHA256核对不变，options/mg_env/mg_transport逐字节恢复、风扇恢复0，
只保留新APK安装。已有Gradle修改和父FCL冲突索引未动。没有将缓存优化写成P7全量完成。
