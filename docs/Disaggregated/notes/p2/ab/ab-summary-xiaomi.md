# P2 paired A/B - per-thread CPU per frame (ms), trailing window, push - pull

pin columns: before/after verdict per run (P=PINNED, D=DRIFT = a cluster clamped by thermal management, U=UNPINNED) @ cpuss temperature C
p50/p99: the runner's best-of-N repeat (lowest mean wall frame time), trailing tailFrames window, device median rule / nearest-rank p99; 'best' says which repeat

| device | case | backend | mode | pull p50 | push p50 | d p50 | pull p99 | push p99 | d p99 | frames | best pull/push | pull acc/draw | push acc/draw | pin pull | pin push |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 35d0befa | improved-transparency-minecraft-26.3 | DirectGLES | finish | 37.222 | 42.422 | +5.200 | 54.879 | 60.401 | +5.522 | 200/200 | run 2/3 / run 2/3 | 8.39 | 8.39 | P@52.5/P@53.2 | P@52.1/P@53.2 |
| 35d0befa | improved-transparency-minecraft-26.3 | DirectGLES | nofinish | 37.263 | 42.436 | +5.173 | 54.965 | 60.409 | +5.444 | 200/200 | run 1/3 / run 3/3 | 8.39 | 8.39 | P@50.9/P@53.2 | P@52.1/P@52.5 |
| 35d0befa | improved-transparency-minecraft-26.3 | DirectVulkan | finish | 61.446 | 68.252 | +6.806 | 79.244 | 88.357 | +9.113 | 200/200 | run 3/3 / run 3/3 | 6.52 | 6.52 | D@52.5/D@50.9 | D@51.7/D@52.5 |
| 35d0befa | improved-transparency-minecraft-26.3 | DirectVulkan | nofinish | 61.549 | 68.118 | +6.569 | 79.403 | 88.111 | +8.708 | 200/200 | run 3/3 / run 3/3 | 6.52 | 6.52 | P@52.5/D@51.3 | D@51.3/D@51.7 |
| 35d0befa | minecraft-1.21.4-fabric-iris-bsl-in-world | DirectGLES | finish | 4.972 | 5.370 | +0.398 | 2312.476 | 2322.192 | +9.716 | 123/123 | run 2/3 / run 3/3 | 40.95 | 40.95 | P@49.8/P@48.6 | P@49.8/P@50.5 |
| 35d0befa | minecraft-1.21.4-fabric-iris-bsl-in-world | DirectGLES | nofinish | 4.911 | 5.353 | +0.442 | 2314.806 | 2315.703 | +0.897 | 123/123 | run 2/3 / run 1/3 | 40.95 | 40.95 | P@49.8/P@50.2 | P@49.0/P@50.9 |
| 35d0befa | minecraft-1.21.4-fabric-iris-bsl-in-world | DirectVulkan | finish | 4.158 | 4.583 | +0.425 | 1614.021 | 1646.905 | +32.884 | 123/123 | run 2/3 / run 1/3 | 14.25 | 14.25 | P@50.5/P@50.9 | P@51.7/P@50.5 |
| 35d0befa | minecraft-1.21.4-fabric-iris-bsl-in-world | DirectVulkan | nofinish | 4.158 | 4.525 | +0.367 | 1604.480 | 1624.190 | +19.710 | 123/123 | run 3/3 / run 3/3 | 14.25 | 14.25 | P@50.2/P@51.3 | P@50.9/P@52.9 |
| 35d0befa | minecraft-1.21.4-in-world | DirectGLES | finish | 7.433 | 8.383 | +0.950 | 10.969 | 11.789 | +0.820 | 200/200 | run 2/3 / run 1/3 | 9.4 | 9.4 | P@49.0/P@44.0 | P@46.3/P@48.6 |
| 35d0befa | minecraft-1.21.4-in-world | DirectGLES | nofinish | 7.392 | 8.187 | +0.795 | 11.086 | 13.192 | +2.106 | 200/200 | run 3/3 / run 2/3 | 9.4 | 9.4 | P@47.1/P@46.7 | P@44.4/P@46.7 |
| 35d0befa | minecraft-1.21.4-in-world | DirectVulkan | finish | 5.237 | 5.842 | +0.605 | 6.767 | 7.380 | +0.613 | 200/200 | run 3/3 / run 3/3 | 8.76 | 8.76 | P@49.8/P@50.2 | P@49.8/P@51.3 |
| 35d0befa | minecraft-1.21.4-in-world | DirectVulkan | nofinish | 5.230 | 5.849 | +0.619 | 6.772 | 7.396 | +0.624 | 200/200 | run 2/3 / run 2/3 | 8.76 | 8.76 | P@47.8/P@50.2 | P@49.4/P@50.9 |
| 35d0befa | minecraft-1.21.4-startup | DirectGLES | finish | 0.822 | 0.841 | +0.019 | 1271.643 | 1284.265 | +12.622 | 59/59 | run 1/3 / run 1/3 | 24.28 | 24.28 | P@50.9/D@53.2 | D@51.3/D@52.5 |
| 35d0befa | minecraft-1.21.4-startup | DirectGLES | nofinish | 0.941 | 1.068 | +0.127 | 1180.356 | 1291.243 | +110.887 | 59/59 | run 3/3 / run 2/3 | 24.28 | 24.28 | D@52.1/D@51.3 | D@52.1/D@51.3 |
| 35d0befa | minecraft-1.21.4-startup | DirectVulkan | finish | 0.368 | 0.422 | +0.054 | 1485.426 | 1459.702 | -25.724 | 59/59 | run 2/3 / run 1/3 | 22.0 | 22.0 | D@52.1/D@52.5 | D@52.1/D@52.1 |
| 35d0befa | minecraft-1.21.4-startup | DirectVulkan | nofinish | 0.382 | 0.415 | +0.033 | 1462.984 | 1487.087 | +24.103 | 59/59 | run 1/3 / run 1/3 | 22.0 | 22.0 | D@52.1/D@50.9 | D@51.7/D@52.5 |

## GO/NO-GO reading (items 3-4: p50 and p99 per-thread CPU deltas must not be negative for the push arm, i.e. push must not cost more)

Windows where push costs MORE CPU than pull (d > 0):
- 35d0befa / improved-transparency-minecraft-26.3 / DirectGLES / finish: d p50 +5.200 ms, d p99 +5.522 ms
- 35d0befa / improved-transparency-minecraft-26.3 / DirectGLES / nofinish: d p50 +5.173 ms, d p99 +5.444 ms
- 35d0befa / improved-transparency-minecraft-26.3 / DirectVulkan / finish: d p50 +6.806 ms, d p99 +9.113 ms
- 35d0befa / improved-transparency-minecraft-26.3 / DirectVulkan / nofinish: d p50 +6.569 ms, d p99 +8.708 ms
- 35d0befa / minecraft-1.21.4-fabric-iris-bsl-in-world / DirectGLES / finish: d p50 +0.398 ms, d p99 +9.716 ms
- 35d0befa / minecraft-1.21.4-fabric-iris-bsl-in-world / DirectGLES / nofinish: d p50 +0.442 ms, d p99 +0.897 ms
- 35d0befa / minecraft-1.21.4-fabric-iris-bsl-in-world / DirectVulkan / finish: d p50 +0.425 ms, d p99 +32.884 ms
- 35d0befa / minecraft-1.21.4-fabric-iris-bsl-in-world / DirectVulkan / nofinish: d p50 +0.367 ms, d p99 +19.710 ms
- 35d0befa / minecraft-1.21.4-in-world / DirectGLES / finish: d p50 +0.950 ms, d p99 +0.820 ms
- 35d0befa / minecraft-1.21.4-in-world / DirectGLES / nofinish: d p50 +0.795 ms, d p99 +2.106 ms
- 35d0befa / minecraft-1.21.4-in-world / DirectVulkan / finish: d p50 +0.605 ms, d p99 +0.613 ms
- 35d0befa / minecraft-1.21.4-in-world / DirectVulkan / nofinish: d p50 +0.619 ms, d p99 +0.624 ms
- 35d0befa / minecraft-1.21.4-startup / DirectGLES / finish: d p50 +0.019 ms, d p99 +12.622 ms
- 35d0befa / minecraft-1.21.4-startup / DirectGLES / nofinish: d p50 +0.127 ms, d p99 +110.887 ms
- 35d0befa / minecraft-1.21.4-startup / DirectVulkan / finish: d p50 +0.054 ms, d p99 -25.724 ms
- 35d0befa / minecraft-1.21.4-startup / DirectVulkan / nofinish: d p50 +0.033 ms, d p99 +24.103 ms

## Memo gates, residual bytes and CSO counters (last complete window, push arm)

- 35d0befa / improved-transparency-minecraft-26.3 / DirectGLES / finish: gates[ers=78960/1367 etl=75378/4949 eub=75199/5128 mfp=0/0 mpm=0/0 mdt=0/0] resid=185.36 csom/csob=csom=0/csob=1157
- 35d0befa / improved-transparency-minecraft-26.3 / DirectGLES / nofinish: gates[ers=78960/1367 etl=75378/4949 eub=75199/5128 mfp=0/0 mpm=0/0 mdt=0/0] resid=185.36 csom/csob=csom=0/csob=1157
- 35d0befa / improved-transparency-minecraft-26.3 / DirectVulkan / finish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=10740/68928 mpm=67660/1268 mdt=78800/868] resid=185.36 csom/csob=csom=0/csob=1157
- 35d0befa / improved-transparency-minecraft-26.3 / DirectVulkan / nofinish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=10740/68928 mpm=67660/1268 mdt=78800/868] resid=185.36 csom/csob=csom=0/csob=1157
- 35d0befa / minecraft-1.21.4-fabric-iris-bsl-in-world / DirectGLES / finish: gates[ers=113/133 etl=85/161 eub=82/164 mfp=0/0 mpm=0/0 mdt=0/0] resid=370.67 csom/csob=csom=1/csob=122
- 35d0befa / minecraft-1.21.4-fabric-iris-bsl-in-world / DirectGLES / nofinish: gates[ers=113/133 etl=85/161 eub=82/164 mfp=0/0 mpm=0/0 mdt=0/0] resid=370.67 csom/csob=csom=1/csob=122
- 35d0befa / minecraft-1.21.4-fabric-iris-bsl-in-world / DirectVulkan / finish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/177 mpm=86/91 mdt=81/96] resid=370.67 csom/csob=csom=1/csob=122
- 35d0befa / minecraft-1.21.4-fabric-iris-bsl-in-world / DirectVulkan / nofinish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/177 mpm=86/91 mdt=81/96] resid=370.67 csom/csob=csom=1/csob=122
- 35d0befa / minecraft-1.21.4-in-world / DirectGLES / finish: gates[ers=6020/1850 etl=6884/986 eub=6962/908 mfp=0/0 mpm=0/0 mdt=0/0] resid=178.31 csom/csob=csom=2/csob=1719
- 35d0befa / minecraft-1.21.4-in-world / DirectGLES / nofinish: gates[ers=6020/1850 etl=6884/986 eub=6962/908 mfp=0/0 mpm=0/0 mdt=0/0] resid=178.31 csom/csob=csom=2/csob=1719
- 35d0befa / minecraft-1.21.4-in-world / DirectVulkan / finish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/7278 mpm=6030/1248 mdt=5928/1350] resid=178.31 csom/csob=csom=2/csob=1719
- 35d0befa / minecraft-1.21.4-in-world / DirectVulkan / nofinish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/7278 mpm=6030/1248 mdt=5928/1350] resid=178.31 csom/csob=csom=2/csob=1719
- 35d0befa / minecraft-1.21.4-startup / DirectGLES / finish: gates[ers=174/185 etl=305/54 eub=305/54 mfp=0/0 mpm=0/0 mdt=0/0] resid=25.08 csom/csob=csom=9/csob=181
- 35d0befa / minecraft-1.21.4-startup / DirectGLES / nofinish: gates[ers=174/185 etl=305/54 eub=305/54 mfp=0/0 mpm=0/0 mdt=0/0] resid=25.08 csom/csob=csom=9/csob=181
- 35d0befa / minecraft-1.21.4-startup / DirectVulkan / finish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/118 mpm=0/118 mdt=0/118] resid=25.08 csom/csob=csom=9/csob=181
- 35d0befa / minecraft-1.21.4-startup / DirectVulkan / nofinish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/118 mpm=0/118 mdt=0/118] resid=25.08 csom/csob=csom=9/csob=181
