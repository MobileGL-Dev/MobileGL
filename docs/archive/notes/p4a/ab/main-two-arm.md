# P2 paired A/B - per-thread CPU per frame (ms), trailing window, push - pull

pin columns: before/after verdict per run (P=PINNED, D=DRIFT = a cluster clamped by thermal management, U=UNPINNED) @ cpuss temperature C
p50/p99: the runner's best-of-N repeat (lowest mean wall frame time), trailing tailFrames window, device median rule / nearest-rank p99; 'best' says which repeat

| device | case | backend | mode | pull p50 | push p50 | d p50 | pull p99 | push p99 | d p99 | frames | best pull/push | pull acc/draw | push acc/draw | pin pull | pin push |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 35d0befa | improved-transparency-minecraft-26.3 | DirectGLES | finish | 10.797 | 12.363 | +1.566 | 25.557 | 27.004 | +1.447 | 200/200 | run 1/3 / run 1/3 | 8.39 | 6.39 | P@47.8/P@50.5 | P@50.2/P@50.5 |
| 35d0befa | improved-transparency-minecraft-26.3 | DirectGLES | nofinish | 10.759 | 12.431 | +1.672 | 25.406 | 27.013 | +1.607 | 200/200 | run 1/3 / run 1/3 | 8.39 | 6.39 | P@43.6/P@48.6 | P@49.8/P@51.3 |
| 35d0befa | improved-transparency-minecraft-26.3 | DirectVulkan | finish | 10.729 | 11.591 | +0.862 | 24.924 | 25.976 | +1.052 | 200/200 | run 3/3 / run 3/3 | 6.52 | 6.52 | P@52.1/P@50.2 | P@52.1/P@53.6 |
| 35d0befa | improved-transparency-minecraft-26.3 | DirectVulkan | nofinish | 10.671 | 11.583 | +0.912 | 25.034 | 26.117 | +1.083 | 200/200 | run 2/3 / run 1/3 | 6.52 | 6.52 | P@49.8/P@52.5 | P@50.2/P@53.2 |
| 35d0befa | minecraft-1.21.4-fabric-iris-bsl-in-world | DirectGLES | finish | 1.702 | 1.346 | -0.356 | 315.989 | 316.342 | +0.353 | 123/123 | run 3/3 / run 1/3 | 40.95 | 39.07 | P@50.2/P@52.5 | P@45.1/P@49.8 |
| 35d0befa | minecraft-1.21.4-fabric-iris-bsl-in-world | DirectGLES | nofinish | 1.304 | 1.946 | +0.642 | 317.280 | 317.310 | +0.030 | 123/123 | run 2/3 / run 2/3 | 40.95 | 39.07 | P@45.5/P@49.4 | P@45.9/P@49.0 |
| 35d0befa | minecraft-1.21.4-fabric-iris-bsl-in-world | DirectVulkan | finish | 0.722 | 0.817 | +0.095 | 459.039 | 447.908 | -11.131 | 123/123 | run 1/3 / run 1/3 | 14.25 | 14.25 | P@44.8/P@50.9 | P@44.8/P@50.9 |
| 35d0befa | minecraft-1.21.4-fabric-iris-bsl-in-world | DirectVulkan | nofinish | 0.763 | 0.794 | +0.031 | 453.873 | 451.470 | -2.403 | 123/123 | run 3/3 / run 1/3 | 14.25 | 14.25 | P@45.1/P@51.3 | P@45.1/P@49.4 |
| 35d0befa | minecraft-1.21.4-fabric-sodium-in-world | DirectGLES | finish | 1.285 | 1.446 | +0.161 | 2.436 | 2.715 | +0.279 | 200/200 | run 2/3 / run 2/3 | 15.64 | 13.52 | P@44.8/P@47.8 | P@50.2/P@50.5 |
| 35d0befa | minecraft-1.21.4-fabric-sodium-in-world | DirectGLES | nofinish | 1.380 | 1.440 | +0.060 | 2.455 | 2.493 | +0.038 | 200/200 | run 1/3 / run 3/3 | 15.64 | 13.52 | P@44.8/P@47.5 | P@45.5/P@46.3 |
| 35d0befa | minecraft-1.21.4-fabric-sodium-in-world | DirectVulkan | finish | 0.961 | 1.037 | +0.076 | 2.086 | 2.097 | +0.011 | 200/200 | run 2/3 / run 3/3 | 19.51 | 19.51 | P@47.1/P@51.7 | P@51.7/P@52.5 |
| 35d0befa | minecraft-1.21.4-fabric-sodium-in-world | DirectVulkan | nofinish | 0.944 | 0.522 | -0.422 | 1.992 | 1.337 | -0.655 | 200/200 | run 1/3 / run 1/3 | 19.51 | 19.51 | P@45.5/P@49.0 | P@50.9/P@52.1 |
| 35d0befa | minecraft-1.21.4-in-world | DirectGLES | finish | 2.362 | 2.853 | +0.491 | 5.184 | 5.683 | +0.499 | 200/200 | run 2/3 / run 3/3 | 9.4 | 7.44 | P@45.5/P@49.8 | P@45.1/P@49.8 |
| 35d0befa | minecraft-1.21.4-in-world | DirectGLES | nofinish | 2.341 | 2.841 | +0.500 | 5.183 | 5.633 | +0.450 | 200/200 | run 1/3 / run 3/3 | 9.4 | 7.44 | P@45.5/P@49.8 | P@45.1/P@49.4 |
| 35d0befa | minecraft-1.21.4-in-world | DirectVulkan | finish | 1.966 | 1.802 | -0.164 | 4.256 | 4.369 | +0.113 | 200/200 | run 1/3 / run 1/3 | 8.76 | 8.76 | P@45.9/P@49.0 | P@45.9/P@49.0 |
| 35d0befa | minecraft-1.21.4-in-world | DirectVulkan | nofinish | 1.050 | 1.735 | +0.685 | 2.363 | 5.902 | +3.539 | 200/200 | run 1/3 / run 1/3 | 8.76 | 8.76 | P@45.5/P@48.6 | P@45.9/P@49.4 |
| 35d0befa | minecraft-1.21.4-rd12-odinlite-in-world | DirectGLES | finish | 8.221 | 11.253 | +3.032 | 23.081 | 24.987 | +1.906 | 200/200 | run 3/3 / run 1/3 | 8.08 | 6.08 | P@41.3/P@49.4 | P@45.1/P@52.9 |
| 35d0befa | minecraft-1.21.4-rd12-odinlite-in-world | DirectGLES | nofinish | 8.236 | 11.204 | +2.968 | 21.918 | 24.882 | +2.964 | 200/200 | run 2/3 / run 1/3 | 8.08 | 6.08 | P@52.1/P@54.8 | P@45.1/P@53.2 |
| 3B159D009VZ00000 | improved-transparency-minecraft-26.3 | DirectGLES | finish | 9.857 | 11.404 | +1.547 | 26.125 | 27.013 | +0.888 | 200/200 | run 1/3 / run 1/3 | 8.39 | 6.39 | P@50.5/P@50.0 | P@49.3/P@47.8 |
| 3B159D009VZ00000 | improved-transparency-minecraft-26.3 | DirectGLES | nofinish | incomplete: push |
| 3B159D009VZ00000 | improved-transparency-minecraft-26.3 | DirectVulkan | nofinish | incomplete: pull |

## GO/NO-GO reading (items 3-4: p50 and p99 per-thread CPU deltas must not be negative for the push arm, i.e. push must not cost more)

Windows where push costs MORE CPU than pull (d > 0):
- 35d0befa / improved-transparency-minecraft-26.3 / DirectGLES / finish: d p50 +1.566 ms, d p99 +1.447 ms
- 35d0befa / improved-transparency-minecraft-26.3 / DirectGLES / nofinish: d p50 +1.672 ms, d p99 +1.607 ms
- 35d0befa / improved-transparency-minecraft-26.3 / DirectVulkan / finish: d p50 +0.862 ms, d p99 +1.052 ms
- 35d0befa / improved-transparency-minecraft-26.3 / DirectVulkan / nofinish: d p50 +0.912 ms, d p99 +1.083 ms
- 35d0befa / minecraft-1.21.4-fabric-iris-bsl-in-world / DirectGLES / finish: d p50 -0.356 ms, d p99 +0.353 ms
- 35d0befa / minecraft-1.21.4-fabric-iris-bsl-in-world / DirectGLES / nofinish: d p50 +0.642 ms, d p99 +0.030 ms
- 35d0befa / minecraft-1.21.4-fabric-iris-bsl-in-world / DirectVulkan / finish: d p50 +0.095 ms, d p99 -11.131 ms
- 35d0befa / minecraft-1.21.4-fabric-iris-bsl-in-world / DirectVulkan / nofinish: d p50 +0.031 ms, d p99 -2.403 ms
- 35d0befa / minecraft-1.21.4-fabric-sodium-in-world / DirectGLES / finish: d p50 +0.161 ms, d p99 +0.279 ms
- 35d0befa / minecraft-1.21.4-fabric-sodium-in-world / DirectGLES / nofinish: d p50 +0.060 ms, d p99 +0.038 ms
- 35d0befa / minecraft-1.21.4-fabric-sodium-in-world / DirectVulkan / finish: d p50 +0.076 ms, d p99 +0.011 ms
- 35d0befa / minecraft-1.21.4-in-world / DirectGLES / finish: d p50 +0.491 ms, d p99 +0.499 ms
- 35d0befa / minecraft-1.21.4-in-world / DirectGLES / nofinish: d p50 +0.500 ms, d p99 +0.450 ms
- 35d0befa / minecraft-1.21.4-in-world / DirectVulkan / finish: d p50 -0.164 ms, d p99 +0.113 ms
- 35d0befa / minecraft-1.21.4-in-world / DirectVulkan / nofinish: d p50 +0.685 ms, d p99 +3.539 ms
- 35d0befa / minecraft-1.21.4-rd12-odinlite-in-world / DirectGLES / finish: d p50 +3.032 ms, d p99 +1.906 ms
- 35d0befa / minecraft-1.21.4-rd12-odinlite-in-world / DirectGLES / nofinish: d p50 +2.968 ms, d p99 +2.964 ms
- 3B159D009VZ00000 / improved-transparency-minecraft-26.3 / DirectGLES / finish: d p50 +1.547 ms, d p99 +0.888 ms

## Memo gates, residual bytes and CSO counters (last complete window, push arm)

- 35d0befa / improved-transparency-minecraft-26.3 / DirectGLES / finish: gates[ers=78960/1367 etl=75258/5069 eub=80327/0 mfp=0/0 mpm=0/0 mdt=0/0] resid=185.36 csom/csob=csom=0/csob=1157 mpr(last window/total)=0/8
- 35d0befa / improved-transparency-minecraft-26.3 / DirectGLES / nofinish: gates[ers=78960/1367 etl=75258/5069 eub=80327/0 mfp=0/0 mpm=0/0 mdt=0/0] resid=185.36 csom/csob=csom=0/csob=1157 mpr(last window/total)=0/8
- 35d0befa / improved-transparency-minecraft-26.3 / DirectVulkan / finish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=10740/68928 mpm=67660/1268 mdt=78800/868] resid=185.36 csom/csob=csom=0/csob=1157 mpr(last window/total)=0/0
- 35d0befa / improved-transparency-minecraft-26.3 / DirectVulkan / nofinish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=10740/68928 mpm=67660/1268 mdt=78800/868] resid=185.36 csom/csob=csom=0/csob=1157 mpr(last window/total)=0/0
- 35d0befa / minecraft-1.21.4-fabric-iris-bsl-in-world / DirectGLES / finish: gates[ers=113/133 etl=80/166 eub=246/0 mfp=0/0 mpm=0/0 mdt=0/0] resid=370.67 csom/csob=csom=1/csob=122 mpr(last window/total)=0/2
- 35d0befa / minecraft-1.21.4-fabric-iris-bsl-in-world / DirectGLES / nofinish: gates[ers=113/133 etl=80/166 eub=246/0 mfp=0/0 mpm=0/0 mdt=0/0] resid=370.67 csom/csob=csom=1/csob=122 mpr(last window/total)=0/2
- 35d0befa / minecraft-1.21.4-fabric-iris-bsl-in-world / DirectVulkan / finish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/177 mpm=86/91 mdt=81/96] resid=370.67 csom/csob=csom=1/csob=122 mpr(last window/total)=0/0
- 35d0befa / minecraft-1.21.4-fabric-iris-bsl-in-world / DirectVulkan / nofinish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/177 mpm=86/91 mdt=81/96] resid=370.67 csom/csob=csom=1/csob=122 mpr(last window/total)=0/0
- 35d0befa / minecraft-1.21.4-fabric-sodium-in-world / DirectGLES / finish: gates[ers=81/859 etl=244/696 eub=940/0 mfp=0/0 mpm=0/0 mdt=0/0] resid=171.8 csom/csob=csom=2/csob=811 mpr(last window/total)=0/1
- 35d0befa / minecraft-1.21.4-fabric-sodium-in-world / DirectGLES / nofinish: gates[ers=81/859 etl=244/696 eub=940/0 mfp=0/0 mpm=0/0 mdt=0/0] resid=171.8 csom/csob=csom=2/csob=811 mpr(last window/total)=0/1
- 35d0befa / minecraft-1.21.4-fabric-sodium-in-world / DirectVulkan / finish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/657 mpm=109/548 mdt=0/657] resid=171.8 csom/csob=csom=2/csob=811 mpr(last window/total)=0/0
- 35d0befa / minecraft-1.21.4-fabric-sodium-in-world / DirectVulkan / nofinish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/657 mpm=109/548 mdt=0/657] resid=171.8 csom/csob=csom=2/csob=811 mpr(last window/total)=0/0
- 35d0befa / minecraft-1.21.4-in-world / DirectGLES / finish: gates[ers=6020/1850 etl=6411/1459 eub=7870/0 mfp=0/0 mpm=0/0 mdt=0/0] resid=178.31 csom/csob=csom=2/csob=1719 mpr(last window/total)=0/0
- 35d0befa / minecraft-1.21.4-in-world / DirectGLES / nofinish: gates[ers=6020/1850 etl=6411/1459 eub=7870/0 mfp=0/0 mpm=0/0 mdt=0/0] resid=178.31 csom/csob=csom=2/csob=1719 mpr(last window/total)=0/0
- 35d0befa / minecraft-1.21.4-in-world / DirectVulkan / finish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/7278 mpm=6030/1248 mdt=5928/1350] resid=178.31 csom/csob=csom=2/csob=1719 mpr(last window/total)=0/0
- 35d0befa / minecraft-1.21.4-in-world / DirectVulkan / nofinish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/7278 mpm=6030/1248 mdt=5928/1350] resid=178.31 csom/csob=csom=2/csob=1719 mpr(last window/total)=0/0
- 35d0befa / minecraft-1.21.4-rd12-odinlite-in-world / DirectGLES / finish: gates[ers=16038/224 etl=16104/158 eub=16262/0 mfp=0/0 mpm=0/0 mdt=0/0] resid=162.91 csom/csob=csom=0/csob=209 mpr(last window/total)=0/0
- 35d0befa / minecraft-1.21.4-rd12-odinlite-in-world / DirectGLES / nofinish: gates[ers=16038/224 etl=16104/158 eub=16262/0 mfp=0/0 mpm=0/0 mdt=0/0] resid=162.91 csom/csob=csom=0/csob=209 mpr(last window/total)=0/0
- 3B159D009VZ00000 / improved-transparency-minecraft-26.3 / DirectGLES / finish: gates[ers=78960/1367 etl=75258/5069 eub=80327/0 mfp=0/0 mpm=0/0 mdt=0/0] resid=185.36 csom/csob=csom=0/csob=1157 mpr(last window/total)=0/8
- 3B159D009VZ00000 / improved-transparency-minecraft-26.3 / DirectGLES / nofinish: gates[-] resid=- csom/csob=-/- mpr(last window/total)=-/None
