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
| 3B159D009VZ00000 | improved-transparency-minecraft-26.3 | DirectGLES | finish | 41.062 | 44.227 | +3.165 | 82.357 | 85.087 | +2.730 | 200/200 | run 2/3 / run 3/2 | 8.39 | 8.39 | P@47.9/P@48.9 | P@48.3/P@49.7 |
| 3B159D009VZ00000 | improved-transparency-minecraft-26.3 | DirectGLES | nofinish | 41.252 | 45.022 | +3.770 | 79.054 | 91.729 | +12.675 | 200/200 | run 2/3 / run 3/3 | 8.39 | 8.39 | P@47.2/P@49.3 | P@47.7/P@48.7 |
| 3B159D009VZ00000 | improved-transparency-minecraft-26.3 | DirectVulkan | finish | 50.657 | 59.829 | +9.172 | 68.068 | 100.402 | +32.334 | 200/200 | run 1/3 / run 3/3 | 6.52 | 6.52 | P@48.0/P@49.7 | P@49.1/P@49.4 |
| 3B159D009VZ00000 | improved-transparency-minecraft-26.3 | DirectVulkan | nofinish | 54.478 | 60.108 | +5.630 | 91.733 | 111.789 | +20.056 | 200/200 | run 1/3 / run 2/3 | 6.52 | 6.52 | P@48.7/P@48.3 | P@48.9/P@49.6 |
| 3B159D009VZ00000 | minecraft-1.21.4-fabric-iris-bsl-in-world | DirectGLES | finish | 6.543 | 7.065 | +0.522 | 2170.464 | 3387.902 | +1217.438 | 123/123 | run 2/3 / run 2/3 | 40.95 | 40.95 | P@46.4/P@46.8 | P@46.1/P@47.1 |
| 3B159D009VZ00000 | minecraft-1.21.4-fabric-iris-bsl-in-world | DirectGLES | nofinish | 6.615 | 7.185 | +0.570 | 2491.897 | 2268.920 | -222.977 | 123/123 | run 3/3 / run 3/3 | 40.95 | 40.95 | P@47.4/P@46.8 | P@46.5/P@46.9 |
| 3B159D009VZ00000 | minecraft-1.21.4-fabric-iris-bsl-in-world | DirectVulkan | finish | 4.058 | 4.464 | +0.406 | 1271.897 | 1295.258 | +23.361 | 123/123 | run 1/3 / run 2/3 | 14.25 | 14.25 | P@47.5/P@47.7 | P@47.0/P@48.1 |
| 3B159D009VZ00000 | minecraft-1.21.4-fabric-iris-bsl-in-world | DirectVulkan | nofinish | 4.087 | 4.452 | +0.365 | 1264.620 | 1295.368 | +30.748 | 123/123 | run 3/3 / run 2/3 | 14.25 | 14.25 | P@46.4/P@48.1 | P@46.8/P@47.6 |
| 3B159D009VZ00000 | minecraft-1.21.4-in-world | DirectGLES | finish | 8.366 | 9.700 | +1.334 | 10.774 | 11.935 | +1.161 | 200/200 | run 1/3 / run 3/3 | 9.4 | 9.4 | P@44.7/P@45.3 | P@46.1/P@47.2 |
| 3B159D009VZ00000 | minecraft-1.21.4-in-world | DirectGLES | nofinish | 8.297 | 9.803 | +1.506 | 10.446 | 11.817 | +1.371 | 200/200 | run 1/3 / run 2/3 | 9.4 | 9.4 | P@45.8/P@45.0 | P@46.0/P@47.8 |
| 3B159D009VZ00000 | minecraft-1.21.4-in-world | DirectVulkan | finish | 5.349 | 5.931 | +0.582 | 6.765 | 7.376 | +0.611 | 200/200 | run 1/3 / run 3/3 | 8.76 | 8.76 | P@47.0/P@48.3 | P@47.6/P@47.2 |
| 3B159D009VZ00000 | minecraft-1.21.4-in-world | DirectVulkan | nofinish | 5.341 | 5.941 | +0.600 | 6.785 | 7.287 | +0.502 | 200/200 | run 3/3 / run 1/3 | 8.76 | 8.76 | P@46.1/P@45.8 | P@48.2/P@47.3 |
| 3B159D009VZ00000 | minecraft-1.21.4-startup | DirectGLES | finish | 1.473 | 1.641 | +0.168 | 959.017 | 946.165 | -12.852 | 59/59 | run 3/3 / run 2/3 | 24.28 | 24.28 | P@50.2/P@50.0 | P@50.4/P@50.5 |
| 3B159D009VZ00000 | minecraft-1.21.4-startup | DirectGLES | nofinish | 1.471 | 1.740 | +0.269 | 966.939 | 956.334 | -10.605 | 59/59 | run 2/3 / run 3/3 | 24.28 | 24.28 | P@49.0/P@50.1 | P@50.1/P@50.3 |
| 3B159D009VZ00000 | minecraft-1.21.4-startup | DirectVulkan | finish | 0.419 | 0.471 | +0.052 | 1131.164 | 1124.003 | -7.161 | 59/59 | run 3/3 / run 3/3 | 22.0 | 22.0 | P@49.8/P@50.3 | P@49.7/P@50.7 |
| 3B159D009VZ00000 | minecraft-1.21.4-startup | DirectVulkan | nofinish | 0.416 | 0.460 | +0.044 | 1129.083 | 1124.650 | -4.433 | 59/59 | run 2/3 / run 3/3 | 22.0 | 22.0 | P@49.5/P@50.3 | P@49.9/P@50.5 |

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
- 3B159D009VZ00000 / improved-transparency-minecraft-26.3 / DirectGLES / finish: d p50 +3.165 ms, d p99 +2.730 ms
- 3B159D009VZ00000 / improved-transparency-minecraft-26.3 / DirectGLES / nofinish: d p50 +3.770 ms, d p99 +12.675 ms
- 3B159D009VZ00000 / improved-transparency-minecraft-26.3 / DirectVulkan / finish: d p50 +9.172 ms, d p99 +32.334 ms
- 3B159D009VZ00000 / improved-transparency-minecraft-26.3 / DirectVulkan / nofinish: d p50 +5.630 ms, d p99 +20.056 ms
- 3B159D009VZ00000 / minecraft-1.21.4-fabric-iris-bsl-in-world / DirectGLES / finish: d p50 +0.522 ms, d p99 +1217.438 ms
- 3B159D009VZ00000 / minecraft-1.21.4-fabric-iris-bsl-in-world / DirectGLES / nofinish: d p50 +0.570 ms, d p99 -222.977 ms
- 3B159D009VZ00000 / minecraft-1.21.4-fabric-iris-bsl-in-world / DirectVulkan / finish: d p50 +0.406 ms, d p99 +23.361 ms
- 3B159D009VZ00000 / minecraft-1.21.4-fabric-iris-bsl-in-world / DirectVulkan / nofinish: d p50 +0.365 ms, d p99 +30.748 ms
- 3B159D009VZ00000 / minecraft-1.21.4-in-world / DirectGLES / finish: d p50 +1.334 ms, d p99 +1.161 ms
- 3B159D009VZ00000 / minecraft-1.21.4-in-world / DirectGLES / nofinish: d p50 +1.506 ms, d p99 +1.371 ms
- 3B159D009VZ00000 / minecraft-1.21.4-in-world / DirectVulkan / finish: d p50 +0.582 ms, d p99 +0.611 ms
- 3B159D009VZ00000 / minecraft-1.21.4-in-world / DirectVulkan / nofinish: d p50 +0.600 ms, d p99 +0.502 ms
- 3B159D009VZ00000 / minecraft-1.21.4-startup / DirectGLES / finish: d p50 +0.168 ms, d p99 -12.852 ms
- 3B159D009VZ00000 / minecraft-1.21.4-startup / DirectGLES / nofinish: d p50 +0.269 ms, d p99 -10.605 ms
- 3B159D009VZ00000 / minecraft-1.21.4-startup / DirectVulkan / finish: d p50 +0.052 ms, d p99 -7.161 ms
- 3B159D009VZ00000 / minecraft-1.21.4-startup / DirectVulkan / nofinish: d p50 +0.044 ms, d p99 -4.433 ms

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
- 3B159D009VZ00000 / improved-transparency-minecraft-26.3 / DirectGLES / finish: gates[ers=78960/1367 etl=75378/4949 eub=75199/5128 mfp=0/0 mpm=0/0 mdt=0/0] resid=185.36 csom/csob=csom=0/csob=1157
- 3B159D009VZ00000 / improved-transparency-minecraft-26.3 / DirectGLES / nofinish: gates[ers=78960/1367 etl=75378/4949 eub=75199/5128 mfp=0/0 mpm=0/0 mdt=0/0] resid=185.36 csom/csob=csom=0/csob=1157
- 3B159D009VZ00000 / improved-transparency-minecraft-26.3 / DirectVulkan / finish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=10740/68928 mpm=67660/1268 mdt=78800/868] resid=185.36 csom/csob=csom=0/csob=1157
- 3B159D009VZ00000 / improved-transparency-minecraft-26.3 / DirectVulkan / nofinish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=10740/68928 mpm=67660/1268 mdt=78800/868] resid=185.36 csom/csob=csom=0/csob=1157
- 3B159D009VZ00000 / minecraft-1.21.4-fabric-iris-bsl-in-world / DirectGLES / finish: gates[ers=113/133 etl=85/161 eub=82/164 mfp=0/0 mpm=0/0 mdt=0/0] resid=370.67 csom/csob=csom=1/csob=122
- 3B159D009VZ00000 / minecraft-1.21.4-fabric-iris-bsl-in-world / DirectGLES / nofinish: gates[ers=113/133 etl=85/161 eub=82/164 mfp=0/0 mpm=0/0 mdt=0/0] resid=370.67 csom/csob=csom=1/csob=122
- 3B159D009VZ00000 / minecraft-1.21.4-fabric-iris-bsl-in-world / DirectVulkan / finish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/177 mpm=86/91 mdt=81/96] resid=370.67 csom/csob=csom=1/csob=122
- 3B159D009VZ00000 / minecraft-1.21.4-fabric-iris-bsl-in-world / DirectVulkan / nofinish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/177 mpm=86/91 mdt=81/96] resid=370.67 csom/csob=csom=1/csob=122
- 3B159D009VZ00000 / minecraft-1.21.4-in-world / DirectGLES / finish: gates[ers=6020/1850 etl=6884/986 eub=6962/908 mfp=0/0 mpm=0/0 mdt=0/0] resid=178.31 csom/csob=csom=2/csob=1719
- 3B159D009VZ00000 / minecraft-1.21.4-in-world / DirectGLES / nofinish: gates[ers=6020/1850 etl=6884/986 eub=6962/908 mfp=0/0 mpm=0/0 mdt=0/0] resid=178.31 csom/csob=csom=2/csob=1719
- 3B159D009VZ00000 / minecraft-1.21.4-in-world / DirectVulkan / finish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/7278 mpm=6030/1248 mdt=5928/1350] resid=178.31 csom/csob=csom=2/csob=1719
- 3B159D009VZ00000 / minecraft-1.21.4-in-world / DirectVulkan / nofinish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/7278 mpm=6030/1248 mdt=5928/1350] resid=178.31 csom/csob=csom=2/csob=1719
- 3B159D009VZ00000 / minecraft-1.21.4-startup / DirectGLES / finish: gates[ers=174/185 etl=305/54 eub=305/54 mfp=0/0 mpm=0/0 mdt=0/0] resid=25.08 csom/csob=csom=9/csob=181
- 3B159D009VZ00000 / minecraft-1.21.4-startup / DirectGLES / nofinish: gates[ers=174/185 etl=305/54 eub=305/54 mfp=0/0 mpm=0/0 mdt=0/0] resid=25.08 csom/csob=csom=9/csob=181
- 3B159D009VZ00000 / minecraft-1.21.4-startup / DirectVulkan / finish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/118 mpm=0/118 mdt=0/118] resid=25.08 csom/csob=csom=9/csob=181
- 3B159D009VZ00000 / minecraft-1.21.4-startup / DirectVulkan / nofinish: gates[ers=0/0 etl=0/0 eub=0/0 mfp=0/118 mpm=0/118 mdt=0/118] resid=25.08 csom/csob=csom=9/csob=181
