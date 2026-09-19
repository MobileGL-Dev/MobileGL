# P2 paired A/B - per-thread CPU per frame (ms), trailing window, push - pull

pin columns: before/after verdict per run (P=PINNED, D=DRIFT = a cluster clamped by thermal management, U=UNPINNED) @ cpuss temperature C
p50/p99: the runner's best-of-N repeat (lowest mean wall frame time), trailing tailFrames window, device median rule / nearest-rank p99; 'best' says which repeat

| device | case | backend | mode | pull p50 | push p50 | d p50 | pull p99 | push p99 | d p99 | frames | best pull/push | pull acc/draw | push acc/draw | pin pull | pin push |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 35d0befa | minecraft-1.21.1-neoforge-create-instancing-in-world | DirectGLES | finish | 771.937 | 769.946 | -1.991 | 1540.659 | 1536.544 | -4.115 | 2/2 | run 2/3 / run 1/3 | 15.27 | 15.27 | U@52.9/U@52.9 | U@52.1/U@54.0 |
| 35d0befa | minecraft-1.21.1-neoforge-create-instancing-in-world | DirectGLES | nofinish | 825.877 | 775.026 | -50.851 | 1648.361 | 1546.747 | -101.614 | 2/2 | run 1/3 / run 2/3 | 15.27 | 15.27 | U@49.0/U@53.6 | U@52.1/U@55.2 |
| 35d0befa | minecraft-1.21.1-neoforge-create-instancing-in-world | DirectVulkan | finish | 838.626 | 846.970 | +8.344 | 1490.291 | 1507.965 | +17.674 | 2/2 | run 1/3 / run 3/3 | 11.27 | 11.27 | U@53.6/U@55.6 | U@52.5/U@57.1 |
| 35d0befa | minecraft-1.21.1-neoforge-create-instancing-in-world | DirectVulkan | nofinish | 839.236 | 849.688 | +10.452 | 1492.840 | 1511.462 | +18.622 | 2/2 | run 3/3 / run 2/3 | 11.27 | 11.27 | U@52.1/U@55.6 | U@52.5/U@54.4 |
| 35d0befa | minecraft-1.21.4-fabric-sodium-in-world | DirectGLES | finish | 3.546 | 2.859 | -0.687 | 6.761 | 6.936 | +0.175 | 200/200 | run 3/3 / run 2/3 | 15.64 | 15.64 | U@46.3/U@47.8 | U@46.7/U@48.2 |
| 35d0befa | minecraft-1.21.4-fabric-sodium-in-world | DirectGLES | nofinish | 3.598 | 3.828 | +0.230 | 6.881 | 6.945 | +0.064 | 200/200 | run 1/3 / run 2/3 | 15.64 | 15.64 | U@50.2/U@48.2 | U@46.3/U@47.1 |
| 35d0befa | minecraft-1.21.4-fabric-sodium-in-world | DirectVulkan | finish | 1.474 | 1.486 | +0.012 | 3.213 | 3.353 | +0.140 | 200/200 | run 1/2 / run 3/3 | 19.51 | 19.51 | U@50.2/U@48.2 | U@47.5/U@49.4 |
| 35d0befa | minecraft-1.21.4-fabric-sodium-in-world | DirectVulkan | nofinish | 0.565 | 0.647 | +0.082 | 1.411 | 2.150 | +0.739 | 200/200 | run 1/3 / run 1/3 | 19.51 | 19.51 | U@47.1/U@48.6 | U@47.8/U@49.4 |
| 35d0befa | minecraft-1.21.4-rd12-odinlite-in-world | DirectGLES | finish | incomplete: push |
| 35d0befa | minecraft-1.21.4-rd12-odinlite-in-world | DirectGLES | nofinish | incomplete: push |
| 3B159D009VZ00000 | improved-transparency-minecraft-26.3 | DirectGLES | finish | 9.625 | 10.698 | +1.073 | 25.806 | 26.407 | +0.601 | 200/200 | run 3/3 / run 3/3 | 8.39 | 8.39 | P@47.0/P@46.4 | P@45.3/P@47.7 |
| 3B159D009VZ00000 | improved-transparency-minecraft-26.3 | DirectGLES | nofinish | 9.741 | 10.542 | +0.801 | 26.312 | 27.022 | +0.710 | 200/200 | run 1/3 / run 3/3 | 8.39 | 8.39 | P@45.5/P@44.7 | P@46.1/P@45.8 |
| 3B159D009VZ00000 | improved-transparency-minecraft-26.3 | DirectVulkan | finish | 8.120 | 9.064 | +0.944 | 23.126 | 23.899 | +0.773 | 200/200 | run 3/3 / run 2/3 | 6.52 | 6.52 | P@47.8/P@50.0 | P@47.3/P@50.4 |
| 3B159D009VZ00000 | improved-transparency-minecraft-26.3 | DirectVulkan | nofinish | 8.162 | 8.973 | +0.811 | 22.851 | 23.638 | +0.787 | 200/200 | run 1/3 / run 3/3 | 6.52 | 6.52 | P@47.3/P@49.3 | P@49.5/P@47.6 |
| 3B159D009VZ00000 | minecraft-1.21.1-neoforge-create-instancing-in-world | DirectGLES | finish | 1046.285 | 1048.879 | +2.594 | 2084.086 | 2089.778 | +5.692 | 2/2 | run 1/3 / run 2/3 | 15.27 | 15.27 | P@52.2/P@53.7 | P@52.9/P@54.2 |
| 3B159D009VZ00000 | minecraft-1.21.1-neoforge-create-instancing-in-world | DirectGLES | nofinish | 1075.081 | 1060.064 | -15.017 | 2142.660 | 2110.799 | -31.861 | 2/2 | run 2/3 / run 1/3 | 15.27 | 15.27 | P@51.7/P@52.8 | P@52.6/P@53.7 |
| 3B159D009VZ00000 | minecraft-1.21.1-neoforge-create-instancing-in-world | DirectVulkan | finish | 760.555 | 776.948 | +16.393 | 1506.263 | 1538.494 | +32.231 | 2/2 | run 1/3 / run 3/3 | 11.27 | 11.27 | P@51.8/P@51.9 | P@50.1/P@49.7 |
| 3B159D009VZ00000 | minecraft-1.21.1-neoforge-create-instancing-in-world | DirectVulkan | nofinish | 758.298 | 760.035 | +1.737 | 1501.555 | 1504.494 | +2.939 | 2/2 | run 1/3 / run 1/3 | 11.27 | 11.27 | P@53.3/P@51.2 | P@51.0/P@50.4 |
| 3B159D009VZ00000 | minecraft-1.21.4-fabric-sodium-in-world | DirectGLES | finish | 1.732 | 1.839 | +0.107 | 3.085 | 2.966 | -0.119 | 200/200 | run 2/3 / run 1/3 | 15.64 | 15.64 | P@46.4/P@48.9 | P@46.5/P@47.8 |
| 3B159D009VZ00000 | minecraft-1.21.4-fabric-sodium-in-world | DirectGLES | nofinish | 1.896 | 1.841 | -0.055 | 2.993 | 3.079 | +0.086 | 200/200 | run 2/3 / run 2/3 | 15.64 | 15.64 | P@51.9/P@49.3 | P@47.9/P@47.6 |
| 3B159D009VZ00000 | minecraft-1.21.4-fabric-sodium-in-world | DirectVulkan | finish | 0.402 | 0.457 | +0.055 | 1.186 | 1.265 | +0.079 | 200/200 | run 3/3 / run 2/3 | 19.51 | 19.51 | P@49.6/P@51.0 | P@50.9/P@52.1 |
| 3B159D009VZ00000 | minecraft-1.21.4-fabric-sodium-in-world | DirectVulkan | nofinish | 0.399 | 0.437 | +0.038 | 1.206 | 1.409 | +0.203 | 200/200 | run 3/3 / run 3/3 | 19.51 | 19.51 | P@47.4/P@50.8 | P@50.2/P@51.2 |
| 3B159D009VZ00000 | minecraft-1.21.4-rd12-odinlite-in-world | DirectGLES | finish | 9.944 | 12.852 | +2.908 | 27.530 | 29.540 | +2.010 | 200/200 | run 1/3 / run 2/3 | 8.08 | 8.08 | P@50.4/P@52.3 | P@50.5/P@53.4 |
| 3B159D009VZ00000 | minecraft-1.21.4-rd12-odinlite-in-world | DirectGLES | nofinish | 9.938 | 12.963 | +3.025 | 27.137 | 29.765 | +2.628 | 200/200 | run 3/3 / run 2/3 | 8.08 | 8.08 | P@49.8/P@50.8 | P@49.7/P@51.5 |
| 3B159D009VZ00000 | minecraft-1.21.4-rd12-odinlite-in-world | DirectVulkan | finish | 7.718 | 10.226 | +2.508 | 24.451 | 27.359 | +2.908 | 200/200 | run 2/3 / run 1/3 | 6.16 | 6.16 | P@52.8/P@54.3 | P@53.6/P@52.1 |
| 3B159D009VZ00000 | minecraft-1.21.4-rd12-odinlite-in-world | DirectVulkan | nofinish | 8.001 | 10.161 | +2.160 | 24.723 | 27.355 | +2.632 | 200/200 | run 1/3 / run 2/3 | 6.16 | 6.16 | P@51.7/P@54.0 | P@53.2/P@55.7 |

## GO/NO-GO reading (items 3-4: p50 and p99 per-thread CPU deltas must not be negative for the push arm, i.e. push must not cost more)

Windows where push costs MORE CPU than pull (d > 0):
- 35d0befa / minecraft-1.21.1-neoforge-create-instancing-in-world / DirectVulkan / finish: d p50 +8.344 ms, d p99 +17.674 ms
- 35d0befa / minecraft-1.21.1-neoforge-create-instancing-in-world / DirectVulkan / nofinish: d p50 +10.452 ms, d p99 +18.622 ms
- 35d0befa / minecraft-1.21.4-fabric-sodium-in-world / DirectGLES / finish: d p50 -0.687 ms, d p99 +0.175 ms
- 35d0befa / minecraft-1.21.4-fabric-sodium-in-world / DirectGLES / nofinish: d p50 +0.230 ms, d p99 +0.064 ms
- 35d0befa / minecraft-1.21.4-fabric-sodium-in-world / DirectVulkan / finish: d p50 +0.012 ms, d p99 +0.140 ms
- 35d0befa / minecraft-1.21.4-fabric-sodium-in-world / DirectVulkan / nofinish: d p50 +0.082 ms, d p99 +0.739 ms
- 3B159D009VZ00000 / improved-transparency-minecraft-26.3 / DirectGLES / finish: d p50 +1.073 ms, d p99 +0.601 ms
- 3B159D009VZ00000 / improved-transparency-minecraft-26.3 / DirectGLES / nofinish: d p50 +0.801 ms, d p99 +0.710 ms
- 3B159D009VZ00000 / improved-transparency-minecraft-26.3 / DirectVulkan / finish: d p50 +0.944 ms, d p99 +0.773 ms
- 3B159D009VZ00000 / improved-transparency-minecraft-26.3 / DirectVulkan / nofinish: d p50 +0.811 ms, d p99 +0.787 ms
- 3B159D009VZ00000 / minecraft-1.21.1-neoforge-create-instancing-in-world / DirectGLES / finish: d p50 +2.594 ms, d p99 +5.692 ms
- 3B159D009VZ00000 / minecraft-1.21.1-neoforge-create-instancing-in-world / DirectVulkan / finish: d p50 +16.393 ms, d p99 +32.231 ms
- 3B159D009VZ00000 / minecraft-1.21.1-neoforge-create-instancing-in-world / DirectVulkan / nofinish: d p50 +1.737 ms, d p99 +2.939 ms
- 3B159D009VZ00000 / minecraft-1.21.4-fabric-sodium-in-world / DirectGLES / finish: d p50 +0.107 ms, d p99 -0.119 ms
- 3B159D009VZ00000 / minecraft-1.21.4-fabric-sodium-in-world / DirectGLES / nofinish: d p50 -0.055 ms, d p99 +0.086 ms
- 3B159D009VZ00000 / minecraft-1.21.4-fabric-sodium-in-world / DirectVulkan / finish: d p50 +0.055 ms, d p99 +0.079 ms
- 3B159D009VZ00000 / minecraft-1.21.4-fabric-sodium-in-world / DirectVulkan / nofinish: d p50 +0.038 ms, d p99 +0.203 ms
- 3B159D009VZ00000 / minecraft-1.21.4-rd12-odinlite-in-world / DirectGLES / finish: d p50 +2.908 ms, d p99 +2.010 ms
- 3B159D009VZ00000 / minecraft-1.21.4-rd12-odinlite-in-world / DirectGLES / nofinish: d p50 +3.025 ms, d p99 +2.628 ms
- 3B159D009VZ00000 / minecraft-1.21.4-rd12-odinlite-in-world / DirectVulkan / finish: d p50 +2.508 ms, d p99 +2.908 ms
- 3B159D009VZ00000 / minecraft-1.21.4-rd12-odinlite-in-world / DirectVulkan / nofinish: d p50 +2.160 ms, d p99 +2.632 ms

## Memo gates, residual bytes and CSO counters (last complete window, push arm)

- 35d0befa / minecraft-1.21.1-neoforge-create-instancing-in-world / DirectGLES / finish: gates[ers=9780/7883 etl=9341/8322 eub=6951/10712 mfp=0/0 mpm=0/0 mdt=0/0] resid=30968.0 csom/csob=csom=29/csob=7741
- 35d0befa / minecraft-1.21.1-neoforge-create-instancing-in-world / DirectGLES / nofinish: gates[ers=9780/7883 etl=9341/8322 eub=6951/10712 mfp=0/0 mpm=0/0 mdt=0/0] resid=30968.0 csom/csob=csom=29/csob=7741
- 35d0befa / minecraft-1.21.1-neoforge-create-instancing-in-world / DirectVulkan / finish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=282/13912 mpm=9408/4504 mdt=7544/6650] resid=30968.0 csom/csob=csom=29/csob=7741
- 35d0befa / minecraft-1.21.1-neoforge-create-instancing-in-world / DirectVulkan / nofinish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=282/13912 mpm=9408/4504 mdt=7544/6650] resid=30968.0 csom/csob=csom=29/csob=7741
- 35d0befa / minecraft-1.21.4-fabric-sodium-in-world / DirectGLES / finish: gates[ers=81/859 etl=405/535 eub=405/535 mfp=0/0 mpm=0/0 mdt=0/0] resid=171.8 csom/csob=csom=2/csob=811
- 35d0befa / minecraft-1.21.4-fabric-sodium-in-world / DirectGLES / nofinish: gates[ers=81/859 etl=405/535 eub=405/535 mfp=0/0 mpm=0/0 mdt=0/0] resid=171.8 csom/csob=csom=2/csob=811
- 35d0befa / minecraft-1.21.4-fabric-sodium-in-world / DirectVulkan / finish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/657 mpm=109/548 mdt=0/657] resid=171.8 csom/csob=csom=2/csob=811
- 35d0befa / minecraft-1.21.4-fabric-sodium-in-world / DirectVulkan / nofinish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/657 mpm=109/548 mdt=0/657] resid=171.8 csom/csob=csom=2/csob=811
- 35d0befa / minecraft-1.21.4-rd12-odinlite-in-world / DirectGLES / finish: gates[ers=16038/224 etl=16148/114 eub=16159/103 mfp=0/0 mpm=0/0 mdt=0/0] resid=162.91 csom/csob=csom=0/csob=209
- 35d0befa / minecraft-1.21.4-rd12-odinlite-in-world / DirectGLES / nofinish: gates[ers=16038/224 etl=16148/114 eub=16159/103 mfp=0/0 mpm=0/0 mdt=0/0] resid=0.0 csom/csob=-/-
- 3B159D009VZ00000 / improved-transparency-minecraft-26.3 / DirectGLES / finish: gates[ers=78960/1367 etl=75378/4949 eub=75199/5128 mfp=0/0 mpm=0/0 mdt=0/0] resid=185.36 csom/csob=csom=0/csob=1157
- 3B159D009VZ00000 / improved-transparency-minecraft-26.3 / DirectGLES / nofinish: gates[ers=78960/1367 etl=75378/4949 eub=75199/5128 mfp=0/0 mpm=0/0 mdt=0/0] resid=185.36 csom/csob=csom=0/csob=1157
- 3B159D009VZ00000 / improved-transparency-minecraft-26.3 / DirectVulkan / finish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=10740/68928 mpm=67660/1268 mdt=78800/868] resid=185.36 csom/csob=csom=0/csob=1157
- 3B159D009VZ00000 / improved-transparency-minecraft-26.3 / DirectVulkan / nofinish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=10740/68928 mpm=67660/1268 mdt=78800/868] resid=185.36 csom/csob=csom=0/csob=1157
- 3B159D009VZ00000 / minecraft-1.21.1-neoforge-create-instancing-in-world / DirectGLES / finish: gates[ers=9780/7883 etl=9341/8322 eub=6951/10712 mfp=0/0 mpm=0/0 mdt=0/0] resid=30968.0 csom/csob=csom=29/csob=7741
- 3B159D009VZ00000 / minecraft-1.21.1-neoforge-create-instancing-in-world / DirectGLES / nofinish: gates[ers=9780/7883 etl=9341/8322 eub=6951/10712 mfp=0/0 mpm=0/0 mdt=0/0] resid=30968.0 csom/csob=csom=29/csob=7741
- 3B159D009VZ00000 / minecraft-1.21.1-neoforge-create-instancing-in-world / DirectVulkan / finish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=282/13912 mpm=9408/4504 mdt=7544/6650] resid=30968.0 csom/csob=csom=29/csob=7741
- 3B159D009VZ00000 / minecraft-1.21.1-neoforge-create-instancing-in-world / DirectVulkan / nofinish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=282/13912 mpm=9408/4504 mdt=7544/6650] resid=30968.0 csom/csob=csom=29/csob=7741
- 3B159D009VZ00000 / minecraft-1.21.4-fabric-sodium-in-world / DirectGLES / finish: gates[ers=81/859 etl=405/535 eub=405/535 mfp=0/0 mpm=0/0 mdt=0/0] resid=171.8 csom/csob=csom=2/csob=811
- 3B159D009VZ00000 / minecraft-1.21.4-fabric-sodium-in-world / DirectGLES / nofinish: gates[ers=81/859 etl=405/535 eub=405/535 mfp=0/0 mpm=0/0 mdt=0/0] resid=171.8 csom/csob=csom=2/csob=811
- 3B159D009VZ00000 / minecraft-1.21.4-fabric-sodium-in-world / DirectVulkan / finish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/657 mpm=109/548 mdt=0/657] resid=171.8 csom/csob=csom=2/csob=811
- 3B159D009VZ00000 / minecraft-1.21.4-fabric-sodium-in-world / DirectVulkan / nofinish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/657 mpm=109/548 mdt=0/657] resid=171.8 csom/csob=csom=2/csob=811
- 3B159D009VZ00000 / minecraft-1.21.4-rd12-odinlite-in-world / DirectGLES / finish: gates[ers=16038/224 etl=16148/114 eub=16159/103 mfp=0/0 mpm=0/0 mdt=0/0] resid=162.91 csom/csob=csom=0/csob=209
- 3B159D009VZ00000 / minecraft-1.21.4-rd12-odinlite-in-world / DirectGLES / nofinish: gates[ers=16038/224 etl=16148/114 eub=16159/103 mfp=0/0 mpm=0/0 mdt=0/0] resid=162.91 csom/csob=csom=0/csob=209
- 3B159D009VZ00000 / minecraft-1.21.4-rd12-odinlite-in-world / DirectVulkan / finish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/16185 mpm=16027/158 mdt=16016/169] resid=162.91 csom/csob=csom=0/csob=209
- 3B159D009VZ00000 / minecraft-1.21.4-rd12-odinlite-in-world / DirectVulkan / nofinish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/16185 mpm=16027/158 mdt=16016/169] resid=162.91 csom/csob=csom=0/csob=209
