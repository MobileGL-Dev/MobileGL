# P2 paired A/B - per-thread CPU per frame (ms), trailing window, push - pull

pin columns: before/after verdict per run (P=PINNED, D=DRIFT = a cluster clamped by thermal management, U=UNPINNED) @ cpuss temperature C
p50/p99: the runner's best-of-N repeat (lowest mean wall frame time), trailing tailFrames window, device median rule / nearest-rank p99; 'best' says which repeat

| device | case | backend | mode | pull p50 | push p50 | d p50 | pull p99 | push p99 | d p99 | frames | best pull/push | pull acc/draw | push acc/draw | pin pull | pin push |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 2f7cbe2e | improved-transparency-minecraft-26.3 | DirectGLES | finish | 10.727 | 12.320 | +1.593 | 26.805 | 26.972 | +0.167 | 200/200 | run 1/3 / run 2/3 | 8.39 | 6.39 | P@40.3/P@40.3 | P@41.1/P@40.7 |
| 2f7cbe2e | improved-transparency-minecraft-26.3 | DirectGLES | nofinish | 10.716 | 12.124 | +1.408 | 25.297 | 26.841 | +1.544 | 200/200 | run 3/3 / run 1/3 | 8.39 | 6.39 | P@39.1/P@41.5 | P@39.9/P@41.5 |
| 2f7cbe2e | improved-transparency-minecraft-26.3 | DirectVulkan | finish | 10.634 | 11.570 | +0.936 | 24.975 | 25.958 | +0.983 | 200/200 | run 2/3 / run 2/3 | 6.52 | 6.52 | P@41.5/P@42.6 | P@40.3/P@43.0 |
| 2f7cbe2e | improved-transparency-minecraft-26.3 | DirectVulkan | nofinish | 10.603 | 11.585 | +0.982 | 24.979 | 26.021 | +1.042 | 200/200 | run 2/3 / run 1/3 | 6.52 | 6.52 | P@41.5/P@41.8 | P@40.3/P@42.2 |
| 2f7cbe2e | minecraft-1.21.4-fabric-iris-bsl-in-world | DirectGLES | finish | 1.759 | 1.839 | +0.080 | 322.670 | 320.204 | -2.466 | 123/123 | run 1/3 / run 2/3 | 40.95 | 39.07 | P@39.9/P@41.5 | P@41.5/P@41.8 |
| 2f7cbe2e | minecraft-1.21.4-fabric-iris-bsl-in-world | DirectGLES | nofinish | 1.727 | 1.811 | +0.084 | 319.025 | 319.226 | +0.201 | 123/123 | run 2/3 / run 3/3 | 40.95 | 39.07 | P@41.1/P@42.2 | P@41.5/P@41.8 |
| 2f7cbe2e | minecraft-1.21.4-fabric-iris-bsl-in-world | DirectVulkan | finish | 0.736 | 0.788 | +0.052 | 455.587 | 462.612 | +7.025 | 123/123 | run 1/3 / run 2/3 | 14.25 | 14.25 | P@40.3/P@43.0 | P@40.3/P@42.6 |
| 2f7cbe2e | minecraft-1.21.4-fabric-iris-bsl-in-world | DirectVulkan | nofinish | 0.742 | 0.786 | +0.044 | 462.639 | 454.128 | -8.511 | 123/123 | run 1/3 / run 1/3 | 14.25 | 14.25 | P@41.5/P@42.6 | P@40.7/P@42.2 |
| 2f7cbe2e | minecraft-1.21.4-fabric-sodium-in-world | DirectGLES | finish | 1.312 | 1.458 | +0.146 | 2.370 | 2.554 | +0.184 | 200/200 | run 2/3 / run 1/3 | 15.64 | 13.52 | P@41.5/P@42.2 | P@41.1/P@42.2 |
| 2f7cbe2e | minecraft-1.21.4-fabric-sodium-in-world | DirectGLES | nofinish | 1.312 | 1.454 | +0.142 | 2.423 | 2.524 | +0.101 | 200/200 | run 3/3 / run 2/3 | 15.64 | 13.52 | P@41.1/P@42.2 | P@40.3/P@42.2 |
| 2f7cbe2e | minecraft-1.21.4-fabric-sodium-in-world | DirectVulkan | finish | 0.477 | 0.504 | +0.027 | 1.174 | 1.179 | +0.005 | 200/200 | run 3/3 / run 1/3 | 19.51 | 19.51 | P@41.1/P@43.8 | P@40.7/P@43.8 |
| 2f7cbe2e | minecraft-1.21.4-fabric-sodium-in-world | DirectVulkan | nofinish | 0.474 | 0.507 | +0.033 | 1.158 | 1.150 | -0.008 | 200/200 | run 3/3 / run 3/3 | 19.51 | 19.51 | P@41.5/P@43.8 | P@40.7/P@42.2 |
| 2f7cbe2e | minecraft-1.21.4-in-world | DirectGLES | finish | 2.343 | 2.833 | +0.490 | 5.272 | 5.674 | +0.402 | 200/200 | run 2/3 / run 3/3 | 9.4 | 7.44 | P@40.7/P@41.1 | P@41.8/P@41.8 |
| 2f7cbe2e | minecraft-1.21.4-in-world | DirectGLES | nofinish | 2.369 | 2.867 | +0.498 | 5.281 | 5.752 | +0.471 | 200/200 | run 1/3 / run 1/3 | 9.4 | 7.44 | P@40.3/P@42.2 | P@41.8/P@42.6 |
| 2f7cbe2e | minecraft-1.21.4-in-world | DirectVulkan | finish | 1.034 | 1.148 | +0.114 | 2.304 | 2.405 | +0.101 | 200/200 | run 1/3 / run 3/3 | 8.76 | 8.76 | P@40.3/P@42.6 | P@41.5/P@42.2 |
| 2f7cbe2e | minecraft-1.21.4-in-world | DirectVulkan | nofinish | 1.028 | 1.145 | +0.117 | 2.270 | 2.419 | +0.149 | 200/200 | run 1/3 / run 3/3 | 8.76 | 8.76 | P@40.3/P@42.6 | P@40.3/P@42.2 |
| 2f7cbe2e | minecraft-1.21.4-rd12-odinlite-in-world | DirectGLES | finish | 8.315 | 11.223 | +2.908 | 21.901 | 24.904 | +3.003 | 200/200 | run 2/3 / run 1/3 | 8.08 | 6.08 | P@40.7/P@41.8 | P@40.7/P@43.4 |
| 2f7cbe2e | minecraft-1.21.4-rd12-odinlite-in-world | DirectGLES | nofinish | 8.210 | 11.182 | +2.972 | 21.778 | 24.804 | +3.026 | 200/200 | run 1/3 / run 2/3 | 8.08 | 6.08 | P@40.7/P@43.4 | P@40.7/P@42.6 |

## GO/NO-GO reading (items 3-4: p50 and p99 per-thread CPU deltas must not be negative for the push arm, i.e. push must not cost more)

Windows where push costs MORE CPU than pull (d > 0):
- 2f7cbe2e / improved-transparency-minecraft-26.3 / DirectGLES / finish: d p50 +1.593 ms, d p99 +0.167 ms
- 2f7cbe2e / improved-transparency-minecraft-26.3 / DirectGLES / nofinish: d p50 +1.408 ms, d p99 +1.544 ms
- 2f7cbe2e / improved-transparency-minecraft-26.3 / DirectVulkan / finish: d p50 +0.936 ms, d p99 +0.983 ms
- 2f7cbe2e / improved-transparency-minecraft-26.3 / DirectVulkan / nofinish: d p50 +0.982 ms, d p99 +1.042 ms
- 2f7cbe2e / minecraft-1.21.4-fabric-iris-bsl-in-world / DirectGLES / finish: d p50 +0.080 ms, d p99 -2.466 ms
- 2f7cbe2e / minecraft-1.21.4-fabric-iris-bsl-in-world / DirectGLES / nofinish: d p50 +0.084 ms, d p99 +0.201 ms
- 2f7cbe2e / minecraft-1.21.4-fabric-iris-bsl-in-world / DirectVulkan / finish: d p50 +0.052 ms, d p99 +7.025 ms
- 2f7cbe2e / minecraft-1.21.4-fabric-iris-bsl-in-world / DirectVulkan / nofinish: d p50 +0.044 ms, d p99 -8.511 ms
- 2f7cbe2e / minecraft-1.21.4-fabric-sodium-in-world / DirectGLES / finish: d p50 +0.146 ms, d p99 +0.184 ms
- 2f7cbe2e / minecraft-1.21.4-fabric-sodium-in-world / DirectGLES / nofinish: d p50 +0.142 ms, d p99 +0.101 ms
- 2f7cbe2e / minecraft-1.21.4-fabric-sodium-in-world / DirectVulkan / finish: d p50 +0.027 ms, d p99 +0.005 ms
- 2f7cbe2e / minecraft-1.21.4-fabric-sodium-in-world / DirectVulkan / nofinish: d p50 +0.033 ms, d p99 -0.008 ms
- 2f7cbe2e / minecraft-1.21.4-in-world / DirectGLES / finish: d p50 +0.490 ms, d p99 +0.402 ms
- 2f7cbe2e / minecraft-1.21.4-in-world / DirectGLES / nofinish: d p50 +0.498 ms, d p99 +0.471 ms
- 2f7cbe2e / minecraft-1.21.4-in-world / DirectVulkan / finish: d p50 +0.114 ms, d p99 +0.101 ms
- 2f7cbe2e / minecraft-1.21.4-in-world / DirectVulkan / nofinish: d p50 +0.117 ms, d p99 +0.149 ms
- 2f7cbe2e / minecraft-1.21.4-rd12-odinlite-in-world / DirectGLES / finish: d p50 +2.908 ms, d p99 +3.003 ms
- 2f7cbe2e / minecraft-1.21.4-rd12-odinlite-in-world / DirectGLES / nofinish: d p50 +2.972 ms, d p99 +3.026 ms

## Memo gates, residual bytes and CSO counters (last complete window, push arm)

- 2f7cbe2e / improved-transparency-minecraft-26.3 / DirectGLES / finish: gates[ers=78960/1367 etl=75258/5069 eub=80327/0 mfp=0/0 mpm=0/0 mdt=0/0] resid=185.36 csom/csob=csom=0/csob=1157 mpr(last window/total)=0/8
- 2f7cbe2e / improved-transparency-minecraft-26.3 / DirectGLES / nofinish: gates[ers=78960/1367 etl=75258/5069 eub=80327/0 mfp=0/0 mpm=0/0 mdt=0/0] resid=185.36 csom/csob=csom=0/csob=1157 mpr(last window/total)=0/8
- 2f7cbe2e / improved-transparency-minecraft-26.3 / DirectVulkan / finish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=10740/68928 mpm=67660/1268 mdt=78800/868] resid=185.36 csom/csob=csom=0/csob=1157 mpr(last window/total)=0/0
- 2f7cbe2e / improved-transparency-minecraft-26.3 / DirectVulkan / nofinish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=10740/68928 mpm=67660/1268 mdt=78800/868] resid=185.36 csom/csob=csom=0/csob=1157 mpr(last window/total)=0/0
- 2f7cbe2e / minecraft-1.21.4-fabric-iris-bsl-in-world / DirectGLES / finish: gates[ers=113/133 etl=80/166 eub=246/0 mfp=0/0 mpm=0/0 mdt=0/0] resid=370.67 csom/csob=csom=1/csob=122 mpr(last window/total)=0/2
- 2f7cbe2e / minecraft-1.21.4-fabric-iris-bsl-in-world / DirectGLES / nofinish: gates[ers=113/133 etl=80/166 eub=246/0 mfp=0/0 mpm=0/0 mdt=0/0] resid=370.67 csom/csob=csom=1/csob=122 mpr(last window/total)=0/2
- 2f7cbe2e / minecraft-1.21.4-fabric-iris-bsl-in-world / DirectVulkan / finish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/177 mpm=86/91 mdt=81/96] resid=370.67 csom/csob=csom=1/csob=122 mpr(last window/total)=0/0
- 2f7cbe2e / minecraft-1.21.4-fabric-iris-bsl-in-world / DirectVulkan / nofinish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/177 mpm=86/91 mdt=81/96] resid=370.67 csom/csob=csom=1/csob=122 mpr(last window/total)=0/0
- 2f7cbe2e / minecraft-1.21.4-fabric-sodium-in-world / DirectGLES / finish: gates[ers=81/859 etl=244/696 eub=940/0 mfp=0/0 mpm=0/0 mdt=0/0] resid=171.8 csom/csob=csom=2/csob=811 mpr(last window/total)=0/1
- 2f7cbe2e / minecraft-1.21.4-fabric-sodium-in-world / DirectGLES / nofinish: gates[ers=81/859 etl=244/696 eub=940/0 mfp=0/0 mpm=0/0 mdt=0/0] resid=171.8 csom/csob=csom=2/csob=811 mpr(last window/total)=0/1
- 2f7cbe2e / minecraft-1.21.4-fabric-sodium-in-world / DirectVulkan / finish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/657 mpm=109/548 mdt=0/657] resid=171.8 csom/csob=csom=2/csob=811 mpr(last window/total)=0/0
- 2f7cbe2e / minecraft-1.21.4-fabric-sodium-in-world / DirectVulkan / nofinish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/657 mpm=109/548 mdt=0/657] resid=171.8 csom/csob=csom=2/csob=811 mpr(last window/total)=0/0
- 2f7cbe2e / minecraft-1.21.4-in-world / DirectGLES / finish: gates[ers=6020/1850 etl=6411/1459 eub=7870/0 mfp=0/0 mpm=0/0 mdt=0/0] resid=178.31 csom/csob=csom=2/csob=1719 mpr(last window/total)=0/0
- 2f7cbe2e / minecraft-1.21.4-in-world / DirectGLES / nofinish: gates[ers=6020/1850 etl=6411/1459 eub=7870/0 mfp=0/0 mpm=0/0 mdt=0/0] resid=178.31 csom/csob=csom=2/csob=1719 mpr(last window/total)=0/0
- 2f7cbe2e / minecraft-1.21.4-in-world / DirectVulkan / finish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/7278 mpm=6030/1248 mdt=5928/1350] resid=178.31 csom/csob=csom=2/csob=1719 mpr(last window/total)=0/0
- 2f7cbe2e / minecraft-1.21.4-in-world / DirectVulkan / nofinish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/7278 mpm=6030/1248 mdt=5928/1350] resid=178.31 csom/csob=csom=2/csob=1719 mpr(last window/total)=0/0
- 2f7cbe2e / minecraft-1.21.4-rd12-odinlite-in-world / DirectGLES / finish: gates[ers=16038/224 etl=16104/158 eub=16262/0 mfp=0/0 mpm=0/0 mdt=0/0] resid=162.91 csom/csob=csom=0/csob=209 mpr(last window/total)=0/0
- 2f7cbe2e / minecraft-1.21.4-rd12-odinlite-in-world / DirectGLES / nofinish: gates[ers=16038/224 etl=16104/158 eub=16262/0 mfp=0/0 mpm=0/0 mdt=0/0] resid=162.91 csom/csob=csom=0/csob=209 mpr(last window/total)=0/0
