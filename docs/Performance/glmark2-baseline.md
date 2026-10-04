# glmark2 baseline: MobileGL (Espryt, Magma) vs the stock kgsl driver stack on anland

Measured 2026-10-04 on the anland Plasma desktop. This is the phase-1 baseline for the
performance work: approximate numbers, how they were taken, where MobileGL loses, and a ranked list
of general fixes. The goal was "where does MobileGL fall short", not lab-grade accuracy, so the
scores are **approximate** (1-3 valid runs per configuration, see n in the tables); the profiling
evidence is the load-bearing part.

**Headline (pinned clocks, glmark2 score; ratio to stock over the scenes all three stacks ran):**

| | stock | Espryt | Magma |
|---|---|---|---|
| es2, 800x600 window | ~1076 | ~1319 (1.19x) | ~778 (0.69x) |
| es2, fullscreen 2560x1600 | ~1177 | ~572 (0.47x) | ~354 (0.29x) |
| desktop GL, 800x600 window | ~1161 | ~1283 (1.12x) | ~743 (0.64x) |
| desktop GL, fullscreen | ~1152 | ~559 (0.49x) | ~353 (0.31x) |

The windowed averages hide two stories. On light scenes (one or two draws per frame) Espryt is
already 1.2-1.5x the stock stack and Magma 0.7-0.9x. Every scene with many draws, client-side
vertex arrays or many render-target switches, and everything at full screen, falls far behind:

| es2 window, FPS | stock | Espryt | Magma |
|---|---|---|---|
| desktop (blur) | 976 | 275 (0.28) | 157 (0.16) |
| desktop (shadow) | 1044 | 234 (0.22) | 125 (0.12) |
| ideas | 688 | 138 (0.20) | 66 (0.10) |
| terrain | 443 | 149 (0.34) | 115 (0.26) |
| build use-vbo=false | 1095 | 467 (0.43) | 472 (0.43) |
| refract | 838 | 981 (1.17) | 331 (0.39) |
| texture, fullscreen | ~1430 | ~726 (0.51) | ~445 (0.31) |

## Stacks and environment

| | stock | MobileGL |
|---|---|---|
| Android app | `com.anland.consumer` 5.22-367a523 (the original anland) | `com.anland.consumer.mobilegl` 5.22-yuv-video-decode (anland `legacy-mobilegl-unified`) |
| Container | `arch-kde` | `arch-kde-mgl` (a clone of it, plus `/opt/mobilegl`) |
| Display daemon | the anland module's, `/data/local/tmp/display_daemon.sock` | the app's, `/data/local/tmp/anland-mobilegl/display.sock` |
| Compositor | `/usr/bin/kwin_wayland` 6.7.4 (anland producer patch) | `/opt/mobilegl/kwin/bin/kwin_wayland` 6.7.4 (anland backend, renders through the server) |
| Client GL | the stock kgsl driver stack: Mesa 26.3.0 freedreno over kgsl (`MESA_LOADER_DRIVER_OVERRIDE=kgsl`, `GALLIUM_DRIVER=kgsl`, `FD_FORCE_KGSL=1` from `/etc/environment`); glvnd vendor `50_mesa.json` only | `/opt/mobilegl/lib/libMobileGL.so` via glvnd `10_mobilegl.json` (sorts before `50_mesa.json`); `MOBILEGL_TRANSPORT=spawn`, `MOBILEGL_IPC_DATA=shm`; run-ahead armed, present credit 1 |
| Server | - | the app's `:mobilegl` service; Espryt = DirectGLES, Magma = DirectVulkan (app setting) |
| Window presentation | Mesa's Wayland EGL, its own dma-bufs | 3 server-allocated AHB shared images per window over `zwp_linux_dmabuf_v1` (client log: `presented through linux-dmabuf shared images (ABGR8888)`) |

- Both containers run **byte-identical glmark2 binaries** (glmark2 2023.01-2: md5 `048dcd34...`
  `glmark2-es2-wayland`, `fde07ceb...` `glmark2-wayland` in both rootfs images) and the same Mesa
  package; only the GL stack under the client changes.
- MobileGL server in the APK: built from `18a5dba0`, code-identical to `3a289222` (the only commit
  between them is a doc). Client deployed from the same tree.
- One desktop owns the display at a time. stock -> MobileGL: `am force-stop com.anland.consumer`,
  `droidspaces -C /data/local/Droidspaces/Containers/arch-kde/container.config stop`,
  `sh /data/local/tmp/anl/switch.sh DirectGLES|DirectVulkan` (after a server crash `switch.sh`
  does not restart a dead `:mobilegl`: send the STOP intent, force-stop the app, open it).
  MobileGL -> stock: STOP intent + force-stop `com.anland.consumer.mobilegl`,
  `droidspaces -C .../arch-kde/container.config start`,
  `monkey -p com.anland.consumer -c android.intent.category.LAUNCHER 1`.
- The MobileGL app shows its extra-keys bar by default (`extra_keys_mode` unset = `always`), which
  shrinks the desktop to about 2560x1410; the original app has `with_keyboard`. For equal fullscreen
  sizes the MobileGL app got `extra_keys_mode=with_keyboard` in `shared_prefs/anland_settings.xml`
  for the measurements. Fullscreen = 2560x1600; the tablet was turned to portrait for the late
  stock runs (1600x2560, same pixel count; the stock app follows auto-rotate).
- The MobileGL session restores a System Settings window at login; it was closed before each batch.

### Device state and clocks

Lenovo TB321FU (Snapdragon 8 Gen 3, Adreno 750), Android 15, AC power (battery 98-100%), screen on,
auto brightness, Plasma idle, no Chrome. **All numbers are at pinned clocks** (the user allowed it;
originals restored afterwards):

| | original | pinned |
|---|---|---|
| cpufreq policy0 (cpu0-1) | walt 902400-2265600 | min = max = 1804800 |
| policy2 (cpu2-4) | walt 614400-3148800 | min = max = 2246400 |
| policy5 (cpu5-6) | walt 499200-2956800 | min = max = 2246400 |
| policy7 (cpu7) | walt 672000-3302400 | min = max = 2246400 |
| kgsl-3d0 devfreq | msm-adreno-tz, 231-903 MHz | min_freq = max_freq = 680000000 |

```sh
C=/sys/devices/system/cpu/cpufreq; G=/sys/class/kgsl/kgsl-3d0/devfreq
for e in policy0:1804800 policy2:2246400 policy5:2246400 policy7:2246400; do
  p=${e%%:*}; f=${e##*:}
  echo $f > $C/$p/scaling_max_freq; echo $f > $C/$p/scaling_min_freq; echo $f > $C/$p/scaling_max_freq
done
echo 680000000 > $G/max_freq; echo 680000000 > $G/min_freq; echo 680000000 > $G/max_freq
```

kgsl idle collapse was left on (`idle_timer` 80, `force_*` 0); sampled `cur_freq` read 680 MHz in
every run, the CPU clocks held, CPU/GPU zones stayed at 40-62 C, nothing throttled. **Pinning is
required:** unpinned, the stock stack scored 425 (es2 window) with the GPU at 310 MHz and the big
cores at 672 MHz, every scene stuck at 330-440 fps; the stacks' different load patterns drive DVFS
differently, so unpinned numbers do not compare.

### Method

- glmark2 default scene list with `-b :duration=5` (5 s per scene; same list). Configurations:
  `glmark2-es2-wayland` and `glmark2-wayland`, each 800x600 windowed and `--fullscreen`.
- MobileGL runs: `MGLOG=/tmp/pb-<tag>.log mgrun offscreen bash -c '<glmark2 command>'` (as root in
  the container; `mgrun` is the anland-mobilegl-plasma skill's). Stock runs:
  `sudo -u swung0x48 bash -c 'set -a; . /etc/environment; set +a; export XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0; <glmark2 command>'`.
- Runs interleaved across stacks; before each run all CPU/GPU zones below 46 C (min 45 s wait).
- Swap interval: glmark2 sets interval 0 on Wayland (`--swap-mode default`). Neither stack throttles
  the client: Mesa's interval 0 ignores frame callbacks; MobileGL's `FrameThrottle` waits only after
  a callback has been outstanding 200 ms. Both compositors repaint at ~125 Hz on the 165 Hz panel.
- Frames shown: two screenshots 1 s apart 25 s into each run must differ, and SurfaceFlinger must
  latch buffers for the anland `SurfaceView` (64-126 per second during runs):
  `dumpsys SurfaceFlinger --latency "<SurfaceView[com.anland.consumer...](BLAST)#N>"`, counting
  present times within the last second. **The stock desktop intermittently turns black mid-run** (a
  pure-black 21707-byte screenshot) while SurfaceFlinger keeps latching ~125 buffers/s, and its score
  then jumps to ~1420-1500; a powerdevil inhibit (`kde-inhibit --power --screenSaver` +
  `kscreen-doctor --dpms on`) did not stop it, restarting the stock container + app clears it. Stock
  runs were therefore probed every 15 s and every run with a black probe was discarded (3 of 8, plus
  a first pinned round with a black screen throughout), so stock n is 1-2 per configuration.
- Overall glmark2 score = mean scene FPS of the scenes that ran. MobileGL does not run the two
  `buffer ... update-method=map` scenes (`GL_OES_mapbuffer` is not in its ES extension string),
  whose stock FPS is low (330-408), so its raw score is flattered; the "common scenes" ratio is the
  fair one.
- `--off-screen` is **not comparable**: on MobileGL glFinish does not wait for the GPU (C1 below), so
  `glmark2-wayland --off-screen` reported 6846 (Espryt) and 12117 (Magma) against 791 on stock, and
  Magma's server crashed in it (C2); `glmark2-es2-wayland --off-screen` does not start on MobileGL
  ("Could not initialize canvas": glmark2 picks `GL_RGBA4` and no depth because `GL_OES_rgb8_rgba8`
  and `GL_OES_depth24` are not advertised).

### Profiling method

Single scenes, 18-30 s (`-b <scene>:duration=18`), after 7-9 s of warm-up:
`simpleperf record -e cpu-clock -f 4000 -p <client>,<server>,<kwin> --duration 8`, `top -H -d 8`,
`/sys/class/kgsl/kgsl-3d0/gpubusy`, and `MOBILEGL_PIPE_STATS=1` on the client
(`P65LinkMetrics kind=frame|frame-op|frame-sent` per-frame lines: wall, client CPU, transport wait,
blocking replies by wire op, records by wire op). Call graphs: `simpleperf record --trace-offcpu -g`.
The container client is an ordinary process on the same kernel, so Android's simpleperf samples it.
Symbolization: the APK's server lib is stripped; a RelWithDebInfo build of the same tree
(`build-android.sh`) has every function at +0x80 (checked on all 7970 exported functions), so a
server `vaddr_in_file` + 0x80 goes into `llvm-symbolizer`. For the container client simpleperf cannot
read the ELF and prints offsets from the start of the executable mapping: add 0x4fb000 (the r-x
segment's page-aligned vaddr) for the deployed client lib.

## Scores (approximate)

### Overall scores (median of runs; min-max in brackets; n = runs)

| Configuration | stock | Espryt | Magma | Espryt/stock | Magma/stock | Espryt/stock (common scenes) | Magma/stock (common scenes) |
|---|---|---|---|---|---|---|---|
| glmark2-es2-wayland, 800x600 window | 1076 [1076-1076] n=1 | 1319 [1298-1363] n=3 | 778 [743-813] n=2 | 1.23 | 0.72 | 1.19 | 0.69 |
| glmark2-es2-wayland, fullscreen | 1177 [1172-1182] n=2 | 572 [556-577] n=3 | 354 [352-357] n=2 | 0.49 | 0.30 | 0.47 | 0.29 |
| glmark2-wayland (desktop GL), 800x600 window | 1161 [1156-1166] n=2 | 1283 [1248-1299] n=3 | 743 [718-768] n=2 | 1.11 | 0.64 | 1.12 | 0.64 |
| glmark2-wayland (desktop GL), fullscreen | 1152 [1152-1152] n=1 | 559 [558-566] n=3 | 353 [350-356] n=2 | 0.49 | 0.31 | 0.49 | 0.31 |

### Per-scene FPS: glmark2-es2-wayland, 800x600 window (median of runs)

| Scene | stock | Espryt | Magma | Espryt/stock | Magma/stock |
|---|---|---|---|---|---|
| build use-vbo=false | 1095 | 467 | 472 | 0.43 | 0.43 |
| build use-vbo=true | 1287 | 1675 | 1062 | 1.30 | 0.83 |
| texture texture-filter=nearest | 1276 | 1482 | 948 | 1.16 | 0.74 |
| texture texture-filter=linear | 1269 | 1672 | 915 | 1.32 | 0.72 |
| texture texture-filter=mipmap | 1283 | 1689 | 860 | 1.32 | 0.67 |
| shading shading=gouraud | 1157 | 1433 | 873 | 1.24 | 0.75 |
| shading shading=blinn-phong-inf | 1143 | 1779 | 997 | 1.56 | 0.87 |
| shading shading=phong | 1137 | 1582 | 966 | 1.39 | 0.85 |
| shading shading=cel | 1237 | 1786 | 854 | 1.44 | 0.69 |
| bump bump-render=high-poly | 1265 | 1777 | 984 | 1.40 | 0.78 |
| bump bump-render=normals | 1225 | 1647 | 1018 | 1.34 | 0.83 |
| bump bump-render=height | 1232 | 1738 | 920 | 1.41 | 0.75 |
| effect2d kernel=0,1,0;1,-4,1;0,1,0; | 1270 | 1706 | 929 | 1.34 | 0.73 |
| effect2d kernel=1,1,1,1,1;1,1,1,1,1;1,1,1,1,1; | 1217 | 1707 | 908 | 1.40 | 0.75 |
| pulsar light=false:quads=5:texture=false | 1298 | 1531 | 688 | 1.18 | 0.53 |
| desktop blur-radius=5:effect=blur:passes=1:separable=true:windows=4 | 976 | 275 | 157 | 0.28 | 0.16 |
| desktop effect=shadow:windows=4 | 1044 | 234 | 125 | 0.22 | 0.12 |
| buffer columns=200:interleave=false:update-dispersion=0.9:update-fraction=0.5:update-method=map | 330 | unsupported | unsupported | - | - |
| buffer columns=200:interleave=false:update-dispersion=0.9:update-fraction=0.5:update-method=subdata | 441 | 573 | 482 | 1.30 | 1.09 |
| buffer columns=200:interleave=true:update-dispersion=0.9:update-fraction=0.5:update-method=map | 408 | unsupported | unsupported | - | - |
| ideas speed=duration | 688 | 138 | 66 | 0.20 | 0.10 |
| jellyfish <default> | 1185 | 1438 | 841 | 1.21 | 0.71 |
| terrain <default> | 443 | 149 | 115 | 0.34 | 0.26 |
| shadow <default> | 975 | 1139 | 644 | 1.17 | 0.66 |
| refract <default> | 838 | 981 | 331 | 1.17 | 0.39 |
| conditionals fragment-steps=0:vertex-steps=0 | 1291 | 1671 | 1023 | 1.29 | 0.79 |
| conditionals fragment-steps=5:vertex-steps=0 | 1173 | 1649 | 886 | 1.41 | 0.76 |
| conditionals fragment-steps=0:vertex-steps=5 | 1263 | 1512 | 1042 | 1.20 | 0.83 |
| function fragment-complexity=low:fragment-steps=5 | 1280 | 1461 | 993 | 1.14 | 0.78 |
| function fragment-complexity=medium:fragment-steps=5 | 1175 | 1745 | 1046 | 1.49 | 0.89 |
| loop fragment-loop=false:fragment-steps=5:vertex-steps=5 | 1246 | 1647 | 985 | 1.32 | 0.79 |
| loop fragment-steps=5:fragment-uniform=false:vertex-steps=5 | 1178 | 1697 | 976 | 1.44 | 0.83 |
| loop fragment-steps=5:fragment-uniform=true:vertex-steps=5 | 1234 | 1531 | 1046 | 1.24 | 0.85 |

### Per-scene FPS: glmark2-es2-wayland, fullscreen (median of runs)

| Scene | stock | Espryt | Magma | Espryt/stock | Magma/stock |
|---|---|---|---|---|---|
| build use-vbo=false | 1355 | 366 | 322 | 0.27 | 0.24 |
| build use-vbo=true | 1518 | 798 | 491 | 0.53 | 0.32 |
| texture texture-filter=nearest | 1396 | 726 | 449 | 0.52 | 0.32 |
| texture texture-filter=linear | 1430 | 726 | 442 | 0.51 | 0.31 |
| texture texture-filter=mipmap | 1437 | 723 | 449 | 0.50 | 0.31 |
| shading shading=gouraud | 1438 | 679 | 423 | 0.47 | 0.29 |
| shading shading=blinn-phong-inf | 1495 | 687 | 259 | 0.46 | 0.17 |
| shading shading=phong | 1431 | 682 | 438 | 0.48 | 0.31 |
| shading shading=cel | 1478 | 679 | 435 | 0.46 | 0.29 |
| bump bump-render=high-poly | 1481 | 745 | 463 | 0.50 | 0.31 |
| bump bump-render=normals | 1434 | 780 | 465 | 0.54 | 0.32 |
| bump bump-render=height | 1467 | 790 | 475 | 0.54 | 0.32 |
| effect2d kernel=0,1,0;1,-4,1;0,1,0; | 1346 | 582 | 390 | 0.43 | 0.29 |
| effect2d kernel=1,1,1,1,1;1,1,1,1,1;1,1,1,1,1; | 698 | 468 | 354 | 0.67 | 0.51 |
| pulsar light=false:quads=5:texture=false | 1379 | 631 | 337 | 0.46 | 0.24 |
| desktop blur-radius=5:effect=blur:passes=1:separable=true:windows=4 | 671 | 219 | 108 | 0.33 | 0.16 |
| desktop effect=shadow:windows=4 | 1031 | 207 | 99 | 0.20 | 0.10 |
| buffer columns=200:interleave=false:update-dispersion=0.9:update-fraction=0.5:update-method=map | 266 | unsupported | unsupported | - | - |
| buffer columns=200:interleave=false:update-dispersion=0.9:update-fraction=0.5:update-method=subdata | 410 | 474 | 281 | 1.15 | 0.68 |
| buffer columns=200:interleave=true:update-dispersion=0.9:update-fraction=0.5:update-method=map | 351 | unsupported | unsupported | - | - |
| ideas speed=duration | 813 | 117 | 57 | 0.14 | 0.07 |
| jellyfish <default> | 1346 | 616 | 277 | 0.46 | 0.21 |
| terrain <default> | 141 | 113 | 92 | 0.80 | 0.65 |
| shadow <default> | 796 | 656 | 401 | 0.82 | 0.50 |
| refract <default> | 157 | 225 | 189 | 1.43 | 1.20 |
| conditionals fragment-steps=0:vertex-steps=0 | 1457 | 649 | 421 | 0.45 | 0.29 |
| conditionals fragment-steps=5:vertex-steps=0 | 1522 | 647 | 402 | 0.43 | 0.26 |
| conditionals fragment-steps=0:vertex-steps=5 | 1445 | 649 | 430 | 0.45 | 0.30 |
| function fragment-complexity=low:fragment-steps=5 | 1614 | 648 | 417 | 0.40 | 0.26 |
| function fragment-complexity=medium:fragment-steps=5 | 1456 | 647 | 408 | 0.44 | 0.28 |
| loop fragment-loop=false:fragment-steps=5:vertex-steps=5 | 1517 | 646 | 415 | 0.43 | 0.27 |
| loop fragment-steps=5:fragment-uniform=false:vertex-steps=5 | 1561 | 651 | 416 | 0.42 | 0.27 |
| loop fragment-steps=5:fragment-uniform=true:vertex-steps=5 | 1545 | 649 | 425 | 0.42 | 0.28 |

### Per-scene FPS: glmark2-wayland (desktop GL), 800x600 window (median of runs)

| Scene | stock | Espryt | Magma | Espryt/stock | Magma/stock |
|---|---|---|---|---|---|
| build use-vbo=false | 1216 | 457 | 474 | 0.38 | 0.39 |
| build use-vbo=true | 1327 | 1805 | 1043 | 1.36 | 0.79 |
| texture texture-filter=nearest | 1360 | 1557 | 923 | 1.14 | 0.68 |
| texture texture-filter=linear | 1366 | 1781 | 1019 | 1.30 | 0.75 |
| texture texture-filter=mipmap | 1372 | 1700 | 938 | 1.24 | 0.68 |
| shading shading=gouraud | 1413 | 1324 | 990 | 0.94 | 0.70 |
| shading shading=blinn-phong-inf | 1309 | 1768 | 930 | 1.35 | 0.71 |
| shading shading=phong | 1349 | 1558 | 1021 | 1.15 | 0.76 |
| shading shading=cel | 1268 | 1598 | 1068 | 1.26 | 0.84 |
| bump bump-render=high-poly | 1319 | 1851 | 923 | 1.40 | 0.70 |
| bump bump-render=normals | 1315 | 1451 | 1004 | 1.10 | 0.76 |
| bump bump-render=height | 1363 | 1839 | 986 | 1.35 | 0.72 |
| effect2d kernel=0,1,0;1,-4,1;0,1,0; | 1376 | 1533 | 663 | 1.11 | 0.48 |
| effect2d kernel=1,1,1,1,1;1,1,1,1,1;1,1,1,1,1; | 1320 | 1743 | 603 | 1.32 | 0.46 |
| pulsar light=false:quads=5:texture=false | 1302 | 1488 | 705 | 1.14 | 0.54 |
| desktop blur-radius=5:effect=blur:passes=1:separable=true:windows=4 | 966 | 273 | 127 | 0.28 | 0.13 |
| desktop effect=shadow:windows=4 | 1070 | 233 | 118 | 0.22 | 0.11 |
| buffer columns=200:interleave=false:update-dispersion=0.9:update-fraction=0.5:update-method=map | 355 | 462 | 437 | 1.30 | 1.23 |
| buffer columns=200:interleave=false:update-dispersion=0.9:update-fraction=0.5:update-method=subdata | 418 | 596 | 437 | 1.43 | 1.05 |
| buffer columns=200:interleave=true:update-dispersion=0.9:update-fraction=0.5:update-method=map | 413 | 501 | 522 | 1.21 | 1.26 |
| ideas speed=duration | 747 | 129 | 67 | 0.17 | 0.09 |
| jellyfish <default> | 1218 | 1434 | 835 | 1.18 | 0.69 |
| terrain <default> | 448 | 145 | 112 | 0.32 | 0.25 |
| shadow <default> | 1056 | 1208 | 607 | 1.14 | 0.57 |
| refract <default> | 831 | 928 | 310 | 1.12 | 0.37 |
| conditionals fragment-steps=0:vertex-steps=0 | 1362 | 1773 | 1008 | 1.30 | 0.74 |
| conditionals fragment-steps=5:vertex-steps=0 | 1353 | 1527 | 937 | 1.13 | 0.69 |
| conditionals fragment-steps=0:vertex-steps=5 | 1317 | 1873 | 1047 | 1.42 | 0.80 |
| function fragment-complexity=low:fragment-steps=5 | 1369 | 1545 | 961 | 1.13 | 0.70 |
| function fragment-complexity=medium:fragment-steps=5 | 1322 | 1701 | 977 | 1.29 | 0.74 |
| loop fragment-loop=false:fragment-steps=5:vertex-steps=5 | 1384 | 1779 | 863 | 1.28 | 0.62 |
| loop fragment-steps=5:fragment-uniform=false:vertex-steps=5 | 1370 | 1556 | 1028 | 1.14 | 0.75 |
| loop fragment-steps=5:fragment-uniform=true:vertex-steps=5 | 1376 | 1757 | 877 | 1.28 | 0.64 |

### Per-scene FPS: glmark2-wayland (desktop GL), fullscreen (median of runs)

| Scene | stock | Espryt | Magma | Espryt/stock | Magma/stock |
|---|---|---|---|---|---|
| build use-vbo=false | 1293 | 372 | 301 | 0.29 | 0.23 |
| build use-vbo=true | 1479 | 797 | 500 | 0.54 | 0.34 |
| texture texture-filter=nearest | 1368 | 725 | 440 | 0.53 | 0.32 |
| texture texture-filter=linear | 1387 | 721 | 449 | 0.52 | 0.32 |
| texture texture-filter=mipmap | 1411 | 726 | 446 | 0.51 | 0.32 |
| shading shading=gouraud | 1484 | 676 | 426 | 0.46 | 0.29 |
| shading shading=blinn-phong-inf | 1490 | 685 | 429 | 0.46 | 0.29 |
| shading shading=phong | 1382 | 677 | 431 | 0.49 | 0.31 |
| shading shading=cel | 1497 | 682 | 418 | 0.46 | 0.28 |
| bump bump-render=high-poly | 1533 | 751 | 458 | 0.49 | 0.30 |
| bump bump-render=normals | 1476 | 785 | 449 | 0.53 | 0.30 |
| bump bump-render=height | 1418 | 789 | 465 | 0.56 | 0.33 |
| effect2d kernel=0,1,0;1,-4,1;0,1,0; | 1340 | 582 | 389 | 0.43 | 0.29 |
| effect2d kernel=1,1,1,1,1;1,1,1,1,1;1,1,1,1,1; | 656 | 467 | 329 | 0.71 | 0.50 |
| pulsar light=false:quads=5:texture=false | 1404 | 627 | 353 | 0.45 | 0.25 |
| desktop blur-radius=5:effect=blur:passes=1:separable=true:windows=4 | 673 | 232 | 108 | 0.34 | 0.16 |
| desktop effect=shadow:windows=4 | 1027 | 196 | 95 | 0.19 | 0.09 |
| buffer columns=200:interleave=false:update-dispersion=0.9:update-fraction=0.5:update-method=map | 267 | 331 | 284 | 1.24 | 1.06 |
| buffer columns=200:interleave=false:update-dispersion=0.9:update-fraction=0.5:update-method=subdata | 423 | 494 | 263 | 1.17 | 0.62 |
| buffer columns=200:interleave=true:update-dispersion=0.9:update-fraction=0.5:update-method=map | 345 | 301 | 300 | 0.87 | 0.87 |
| ideas speed=duration | 818 | 120 | 67 | 0.15 | 0.08 |
| jellyfish <default> | 1345 | 613 | 277 | 0.46 | 0.21 |
| terrain <default> | 148 | 111 | 91 | 0.75 | 0.62 |
| shadow <default> | 801 | 663 | 400 | 0.83 | 0.50 |
| refract <default> | 157 | 223 | 189 | 1.42 | 1.20 |
| conditionals fragment-steps=0:vertex-steps=0 | 1348 | 645 | 423 | 0.48 | 0.31 |
| conditionals fragment-steps=5:vertex-steps=0 | 1480 | 645 | 417 | 0.44 | 0.28 |
| conditionals fragment-steps=0:vertex-steps=5 | 1411 | 649 | 418 | 0.46 | 0.30 |
| function fragment-complexity=low:fragment-steps=5 | 1425 | 650 | 431 | 0.46 | 0.30 |
| function fragment-complexity=medium:fragment-steps=5 | 1398 | 647 | 411 | 0.46 | 0.29 |
| loop fragment-loop=false:fragment-steps=5:vertex-steps=5 | 1446 | 645 | 418 | 0.45 | 0.29 |
| loop fragment-steps=5:fragment-uniform=false:vertex-steps=5 | 1403 | 646 | 413 | 0.46 | 0.29 |
| loop fragment-steps=5:fragment-uniform=true:vertex-steps=5 | 1546 | 648 | 395 | 0.42 | 0.26 |

## Where the time goes

Per-frame profile of the scenes that lose (es2 window unless noted, pinned; client = the glmark2
thread, server = its `mgl-srv-apply` thread, % of one core over 8 s; waits = blocking
client-to-server replies per frame; present RTT = wait for the shared-image present reply):

| Espryt | fps | records/frame | waits/frame (op) | client CPU | server apply thread | GPU busy | present RTT |
|---|---|---|---|---|---|---|---|
| texture | 1394 | 11 | 1 (SharedImage present) | 31% | 44% | 40% | 0.30 ms |
| texture fullscreen | 721 | 11 | 1 | 24% | 35% | 98% | 0.60 ms |
| desktop blur | 228 | 299 | 29 (28 ResourceCreate + present) | 40% | 51% | 13% | - |
| desktop shadow | 173 | 395 | 53 (52 ResourceCreate + present) | 40% | 50% | 7% | - |
| ideas | 118 | 587 (305 draws) | 1 | 30% | 45% | 7% | 5.6 ms |
| terrain | 130 | 224 | 1 | 29% | 45% | 36% | 0.63 ms |
| build use-vbo=false | 448 | 21 | 3 (2 ResourceCreate + present) | 33% | 36% | 15% | - |

| Magma | fps | waits/frame | client CPU | server apply thread | GPU busy | present RTT |
|---|---|---|---|---|---|---|
| texture | 916 | 1 | 28% | 50% | 37% | 0.58 ms |
| texture fullscreen | 418 | 1 | 17% | 29% | 75% | 1.25 ms |
| desktop blur | 118 | 29 | 35% | 52% | 9% | - |
| desktop shadow | 115 | 53 | 38% | 53% | 9% | - |
| ideas | 53 | 1 | 24% | 60% | 14% | 15.2 ms |
| terrain | 108 | 1 | 31% | 49% | 27% | 0.91 ms |
| build use-vbo=false | 498 | 3 | 42% | 49% | 26% | - |

**The common shape: nothing is saturated.** Outside fullscreen the GPU is 7-40% busy, and neither
the client thread nor the server apply thread is near 100%. Both sides spend most of the frame
waiting for each other, and each handoff costs a wake-up. Evidence that the handoffs, not the work,
set the frame time:

- With **`MOBILEGL_IPC_SPIN_US=1000`** on the client only (Espryt; the server keeps its 50 us), the
  client spins through short replies instead of parking: ideas 94 -> 185 fps, terrain 132 -> 315,
  desktop shadow 187 -> 322, texture 1543 -> 1837 (8 s single-scene runs).
- On Espryt's server in ideas the apply thread is 50% off-CPU, almost all of it parked in
  `SocketDoorbell::Park` waiting for records, and 18% of its on-CPU time is `SocketDoorbell::Notify`
  (a socket write that wakes the client). The client is 75% off-CPU in `poll`. The server's own
  frame work is ~2 ms (P65ServerFrame `apply_ms` for the ~600-record frames) of an 8.5 ms frame.

## Ranked bottlenecks and proposed general fixes

Gains are estimates from the evidence above, at the pinned clocks; none of the fixes is
glmark2-specific.

### Shared: client library and pipe (both backends)

1. **Blocking ResourceCreate for client-memory vertex/index arrays.** Every draw that sources a
   client array makes a brand-new owned buffer per array (`MG_Impl/Pipe/OwnedDrawInputs.h`
   `MakeOwnedBuffer` -> `BufferObject` ctor -> `MGPipeEmitResourceCreate` -> `EmitCreateBlocking` ->
   `EmitAndWait`): one round trip per array per draw, plus a respecify, a subdata and a destroy
   record. desktop blur pays 28 and shadow 52 of them per frame; 55% of the client's samples are the
   spin in `SessionProducer::WaitForAppliedOrEventBacklog` under `EmitCreateBlocking`. The fetch plan
   also sorts the referenced vertex list per draw (`std::sort` = 27% of the client in build
   use-vbo=false) even for a `glDrawArrays` range that is contiguous.
   **Fix:** a per-context streaming upload buffer for client arrays (one long-lived server resource,
   sub-allocated per draw, reused once the frames that read it retired; grows on overflow), or
   client-minted creates that need no reply; plus a contiguous-range fast path (one memcpy of
   `[first, first+count)` per attribute, no sort). **Gain:** blocking waits/frame -> ~1; desktop
   blur/shadow 2-3x, build use-vbo=false ~2x, on both backends; helps every GLES2-style app.
   **Risk:** sub-allocation lifetime vs in-flight draws on both backends; validate with the
   OwnedDrawInputs/Pipe unit tests and the retrace CI.
2. **Wake-up latency of every blocking wait and every ring handoff.** Doorbells are one byte on a
   unix socket; a parked waiter needs a socket write plus a scheduler wake-up (hundreds of us to ms
   at these clocks), and the server rings the client on every applied-watermark advance while the
   client is parked, even if the client waits for a much later sequence (18% of Espryt's apply
   thread is `SocketDoorbell::Notify`). **Fix:** (a) the waiter publishes the sequence it waits for
   and the server rings only when `applied >= target`; (b) a bounded adaptive spin sized from the
   recently measured reply latency, only while a reply is expected (never when idle); (c) optionally
   futex wait/wake on the shared ring page instead of socket bytes (same kernel). **Gain:** spin alone
   gave 1.2x (light) to 2.4x (terrain); expect 1.3-2x on draw-heavy scenes, both backends.
   **Risk:** lost wake-ups (keep the doorbell's two-fence protocol), CPU and power at idle; RingTest,
   MultiSession tests, idle CPU of the desktop.
3. **Synchronous per-frame present.** `WindowSurface::Present` -> `PresentToSharedImage` ->
   `EmitSharedImage` is `EmitAndWait`: the client waits until the server has applied the whole frame
   and queued the blit, and eglSwapBuffers' Present record also holds a present credit of 1. The
   client cannot record frame N+1 while the server finishes N (texture: 0.71 ms frame = 0.26 ms of
   client CPU + 0.30 ms of wait). **Fix:** make the shared-image present asynchronous: commit the
   `wl_buffer` from the reply on the event ring (or let the server order the compositor's acquire of
   that image after the producer's present record), and allow credit 2. **Gain:** 1.3-1.8x on light
   scenes. **Risk:** ordering with the compositor's import (Chrome flicker test, KWin, desktop-verify).
4. **Fullscreen pays an extra full-frame copy and full composition.** Every frame the server copies
   the client's default framebuffer (a server pbuffer / Magma swapchain image) into the shared image
   (Espryt `glBlitFramebuffer`, Magma `vkCmdCopyImage` in a second submission), and KWin composites
   the whole screen (85 buffers/s latched during Espryt fullscreen). At 2560x1600 the GPU is 98% busy
   (Espryt) on texture for 726 fps against ~1430 on stock, whose fullscreen is even faster than its
   windowed run. **Fix:** let the client render straight into the window's shared images (default
   framebuffer = the current back image; the row order can be expressed with linux-dmabuf's
   `Y_INVERT` flag instead of the flipping blit); separately, direct scanout of fullscreen clients in
   the anland KWin backend. **Gain:** fullscreen 1.5-2x on both backends. **Risk:** buffer age and
   damage, resize, device loss; desktop-verify and Chrome.
5. **Per-record fixed cost on the server.** The server is built for `android-26`, so every
   `thread_local` goes through emulated TLS: `__emutls_get_address` + `pthread_getspecific` are 7-12%
   of Espryt's and ~3% of Magma's apply thread. `ServerLoop::DrainRing` takes two
   `steady_clock::now()` per record for the P65ServerFrame statistics even when they are off (5% on
   Espryt), and `MGPipeApplier()` / `ResolveThreadApplierKey` / `MGPipeApplierShareGroupKeyFor` (TLS
   + mutex) run per record (~5%). **Fix:** resolve the applier/session once per drain and pass it
   down; time records only with the statistics on; consider ELF TLS (API 29+) for the anland server
   build. **Gain:** 10-15% of server CPU. **Risk:** low.
6. **Small:** a one-draw frame sends 11 records; `CreateVertexElements` is re-sent per draw (ideas:
   76 per frame) instead of being deduplicated by content on the client.

### Espryt (DirectGLES)

1. **Per-draw state synchronization** (`DG::PrepareForDraw` 28% of the apply thread in ideas:
   `SyncCurrentProgramByHandle`, `BindCurrentProgramWithResources`, `SyncNeccessaryBuffers`, VAO
   `SyncToBackendFromApplier`, `ResolveVaoTwin` / `ResolveProgramTwin` registry lookups, texture
   sync). The same per-draw redundancy as on Minecraft: version-gated early-outs keyed on monotonic
   lifetime ids. **Gain:** 10-20% on draw-heavy scenes once the pipe is fixed. **Risk:** stale
   caches; the retrace CI.
2. Parity: `GL_OES_mapbuffer`, `GL_OES_rgb8_rgba8`, `GL_OES_depth24` missing from ES contexts (two
   Unsupported scenes; es2 off-screen does not start).

### Magma (DirectVulkan)

1. **A VkRenderPass per draw.** `VulkanRenderer::SetupWireDraw` (`WireDraw.inc`) calls
   `RetireWireDrawPass()` unconditionally, which ends the active render pass, and begins a new one
   at the end of setup, for every draw. In ideas (305 draws/frame) `SetupWireDraw` is 41% of the
   apply thread and End/BeginRenderPass ~19%; on a tiler every pass also loads and stores its
   attachments. **Fix:** keep the pass open while the `WireDrawPassKey` (attachments, extent, epochs)
   is unchanged and nothing that must run outside a pass came in between (clear outside the pass,
   transfer, barrier, readback, layout change of an attachment, query); retire lazily. **Gain:**
   ideas, terrain, desktop 1.5-2x on Magma; less load/store bandwidth at full screen. **Risk:**
   attachment layout tracking and feedback loops; Magma unit tests (never the DirectVulkan
   integration tests), retrace CI.
2. **Per-frame submission overhead.** Per frame: `FlushPendingCommands` for the frame, a second
   submission for the shared-image copy, and `Present()` of the client's own offscreen default
   surface (swapchain acquire + submit through libgui). `GpuProgressMarkers::SubmitBracketed` (hang
   watch) is 12% of texture fullscreen, and `FreeRetiredCommandBuffersCompletedUpTo` +
   `EndCommandRecording` ~25% of ideas (command buffers freed and reallocated rather than reset and
   reused). **Fix:** one submission per frame (append the copy), skip `Present` for a surface whose
   frames only go to shared images, recycle command buffers per frame slot, coalesce markers.
   **Gain:** light scenes 1.3-1.5x on Magma.
3. **Allocation churn.** scudo malloc/free with lock contention (`__aarch64_cas4_acq` 20% + scudo
   ~15% of the ideas apply thread): per-draw vectors (`BeginRenderPass` clear values, vertex input
   state objects, retired wire objects). **Fix:** per-frame arenas / small vectors. **Gain:** 10-20%.
4. **A VkRenderPass + VkFramebuffer created per clear** (`WireFramebuffer.inc` ~306-311): 7% of
   texture fullscreen, and the objects pile up when nothing retires them (C2).

### Correctness found on the way

- **C1 glFinish does not wait for the GPU** on the remote path: `ClientSession::Finish` only waits
  for the server to apply (`WaitForApplyToCatchUp`). Off-screen glmark2 then reports 6846 / 12117
  and nothing bounds the GPU queue. It must reach the backend (driver finish / fence wait) and reply.
- **C2 Magma server crash** under `glmark2-wayland --off-screen`: `VK_ERROR_OUT_OF_HOST_MEMORY` FATAL
  at `WireFramebuffer.inc:311` (`vkCreateFramebuffer` in `ClearWireFramebuffer`), then SIGSEGV in
  `vkCmdBeginRenderPass` (two server crashes in a row); the per-clear objects accumulate when the
  client runs ahead with no backpressure (C1).
- **C3** ES extension parity (Espryt item 2).

## Next steps

In this order, each measured on the affected scenes at the pinned clocks, frames shown, Plasma /
Chrome / lock-unlock checked on both backends: C1-C3, shared 1 (client arrays), shared 2
(wake-ups), shared 3/4 (present), Magma 1 (render pass), shared 5 (per-record cost), then each
backend's per-draw items.
