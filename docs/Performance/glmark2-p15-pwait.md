# glmark2 on anland: the P15 present-wait stack, before and after

Measured 2026-10-10 on the anland Plasma desktop of the Lenovo TB321FU (Adreno 750). **Single runs, one
per arm and configuration**: windowed light scenes move about +-15 % run to run (see the per-scene
tables), so only differences well beyond that mean anything. Method and traps:
[glmark2-handoff.md](glmark2-handoff.md).

## Arms

| arm | commit | notes |
|---|---|---|
| before | `a759b4b7` (the `origin/feat/disaggregated` of that day) | server and container client built from the same commit |
| after | `eb08b0b1` (branch `p15stack`) | present-wait levers, hot-path TLS cut, small-store drain, share-group dirty-bit fix. The pushed tip `bbd555fd` is `eb08b0b1` plus one test-only line in `MG_Test/Pipe/TrackerTest.cpp`, so the measured runtime code is the pushed code |
| after+x2 | `eb08b0b1` | Magma only, `MOBILEGL_MAGMA_EXTRA_SWAPCHAIN_IMAGES=2` in the server process |
| stock | the original anland app and its container | the stock kgsl driver stack, same glmark2 binaries |

Server: NDK RelWithDebInfo for `android-30` (native TLS segment present in both libs). Client: Release
cross build in WSL. The server and client of an arm are a matched pair: a client of one commit against the
other commit's server is refused (`Refuse{WireFingerprint}` for every session, black desktop), so the
arms were switched as pairs (libs swapped, container restarted, app reopened).

## Device state and validity

- CPU pinned to 1.80 / 2.25 / 2.25 / 2.25 GHz (policy0/2/5/7), GPU 680 MHz (kgsl `thermal_pwrlevel` 4
  is that pin, no cap below it). The `msm_performance` requests were released, and thermal-engine, the
  perf HAL, the vendor perf service, performance and hyperschedule were stopped for the whole session
  (all restarted afterwards).
- Sampled every 2 s during every run (CPU cur/max per policy, GPU cur/max/pwrlevel, zone and battery
  temperatures): **every sample of every run shows the pinned frequencies exactly** (min = max = pin,
  GPU 680 MHz, no cap). Peak CPU zone 72-88 C, GPU zone 62-75 C, battery 36.5-40.7 C. Battery 93-94 %
  throughout (USB attached).
- Before each run the clocks fell to their lowest OPP and the CPU/GPU zones had to be below 46 C
  (at least 45 s). The "battery <= 33 C between reps" cool-down was **not** applied (the battery sat at
  35-40 C and the job was told to start without waiting); the frequency samples above are what validate
  the runs.
- Screen probes every 15 s (two screenshots 1 s apart): no black frame in any scored MobileGL run, nor
  in the scored stock runs. Two more stock fullscreen runs (1365, 1322) showed the known 19838-byte black
  desktop mid-run and were discarded; the scored stock fullscreen run (1325) had none.
- glmark2 reported Surface Size 800x600 windowed and 2560x1600 fullscreen on every arm (tablet in
  landscape, auto-rotate untouched). The MobileGL app's `extra_keys_mode` was set to `with_keyboard` for
  the session so its fullscreen size equals stock's (the default shows the keys bar: 2560x1412); restored
  afterwards.
- One run per arm and configuration.

## Scores (glmark2-es2-wayland, `-b :duration=5`)

| | stock | Espryt before | Espryt after | Magma before | Magma after | Magma after+x2 |
|---|---:|---:|---:|---:|---:|---:|
| es2 window 800x600 | 1038 | 1900 | 1839 (-3 %) | 1278 | 1289 (+1 %) | 1343 (+5 %) |
| ratio to stock | 1.00 | 1.83 | 1.77 | 1.23 | 1.24 | 1.29 |
| es2 fullscreen 2560x1600 | 1325 | 521 | 518 (-1 %) | 428 | 423 (-1 %) | 426 (0 %) |
| ratio to stock | 1.00 | 0.39 | 0.39 | 0.32 | 0.32 | 0.32 |

**Reading:** no before/after difference is outside run-to-run noise. Fullscreen runs are steady (scene
by scene within about 5 %) and the stack changes nothing on either backend. Windowed, the whole-score
deltas are within +-5 % and the per-scene swings flip sign between arms (Magma "build vbo=true" is -35 %
after but the same scene is -6 % in the +x2 arm, linear texture +47 % and cel shading +47 % after), which is
noise in 5 s samples, not an effect.

## Per-scene FPS

**windowed 800x600** (FPS, one run each; `*` = after differs from before by more than 10 %)

| scene | stock | Espryt before | Espryt after | Magma before | Magma after | Magma after+x2 |
|---|---:|---:|---:|---:|---:|---:|
| build vbo=false | 1185 | 1340 | 1359 | 1660 | 1676 | 1447* |
| build vbo=true | 1237 | 2688 | 2472 | 1782 | 1150* | 1677 |
| texture nearest | 1153 | 2184 | 2331 | 1377 | 1628* | 1512 |
| texture linear | 1173 | 2405 | 2226 | 1168 | 1712* | 1678* |
| texture mipmap | 1391 | 2065 | 2330* | 1641 | 1367* | 1504 |
| shading gouraud | 1335 | 1912 | 2359* | 844 | 1152* | 1323* |
| shading blinn-phong-inf | 1210 | 2711 | 2254* | 1667 | 1463* | 1583 |
| shading phong | 1087 | 2411 | 2394 | 1322 | 1185* | 1648* |
| shading cel | 1229 | 2572 | 2436 | 1100 | 1613* | 1530* |
| bump high-poly | 1520 | 2369 | 2128* | 1738 | 1632 | 1600 |
| bump normals | 1023 | 2375 | 2424 | 1696 | 1835 | 1645 |
| bump height | 1310 | 2451 | 2277 | 1590 | 1485 | 1695 |
| effect2d kernel=0,1,0;1,-4,1;0,1,0; | 1168 | 2033 | 2147 | 1562 | 1512 | 1350* |
| effect2d kernel=1,1,1,1,1;1,1,1,1,1;1,1,1,1,1; | 1250 | 2147 | 2208 | 1739 | 1113* | 1501* |
| pulsar light=false:quads=5:texture=false | 1131 | 2196 | 2327 | 1616 | 1761 | 1717 |
| desktop blur-radius=5:effect=blur:passes=1:sep | 881 | 947 | 620* | 746 | 804 | 764 |
| desktop effect=shadow:windows=4 | 921 | 748 | 580* | 691 | 702 | 944* |
| buffer columns=200:interleave=false:update-dis | 394 | 592 | 548 | 733 | 793 | 786 |
| buffer columns=200:interleave=false:update-dis | 395 | 868 | 556* | 821 | 821 | 794 |
| buffer columns=200:interleave=true:update-disp | 432 | 400 | 564* | 602 | 792* | 829* |
| ideas speed=duration | 755 | 522 | 668* | 686 | 681 | 634 |
| jellyfish | 1230 | 2097 | 2079 | 1651 | 1597 | 1590 |
| terrain | 402 | 479 | 477 | 423 | 428 | 429 |
| shadow | 1049 | 1829 | 1736 | 1434 | 1468 | 1351 |
| refract | 734 | 1024 | 1025 | 653 | 820* | 797* |
| conditionals f-steps=0:v-steps=0 | 1160 | 2388 | 2157 | 1497 | 1525 | 1468 |
| conditionals f-steps=5:v-steps=0 | 1109 | 2475 | 2110* | 1207 | 1610* | 1626* |
| conditionals f-steps=0:v-steps=5 | 1030 | 2308 | 2326 | 1770 | 1530* | 1582* |
| function f-complexity=low:f-steps=5 | 1091 | 2321 | 1949* | 1440 | 1283* | 1384 |
| function f-complexity=medium:f-steps=5 | 1075 | 2704 | 2414* | 1519 | 1486 | 1605 |
| loop f-loop=false:f-steps=5:v-steps=5 | 1018 | 2252 | 2454 | 1232 | 1179 | 1467* |
| loop f-steps=5:f-uniform=false:v-steps=5 | 1032 | 2335 | 2505 | 1562 | 1331* | 1379* |
| loop f-steps=5:f-uniform=true:v-steps=5 | 1188 | 2601 | 2312* | 1040 | 1440* | 1518* |
| **score** | **1038** | **1900** | **1839** | **1278** | **1289** | **1343** |

**fullscreen 2560x1600** (FPS, one run each; `*` = after differs from before by more than 10 %)

| scene | stock | Espryt before | Espryt after | Magma before | Magma after | Magma after+x2 |
|---|---:|---:|---:|---:|---:|---:|
| build vbo=false | 1658 | 613 | 621 | 476 | 473 | 468 |
| build vbo=true | 1601 | 718 | 700 | 543 | 554 | 530 |
| texture nearest | 1817 | 613 | 552 | 493 | 525 | 527 |
| texture linear | 1743 | 646 | 646 | 474 | 484 | 489 |
| texture mipmap | 1838 | 649 | 571* | 541 | 495 | 509 |
| shading gouraud | 1687 | 555 | 602 | 501 | 504 | 495 |
| shading blinn-phong-inf | 1606 | 501 | 530 | 491 | 428* | 490 |
| shading phong | 1753 | 592 | 576 | 496 | 473 | 475 |
| shading cel | 1675 | 577 | 575 | 476 | 460 | 495 |
| bump high-poly | 1629 | 525 | 574 | 514 | 500 | 510 |
| bump normals | 1628 | 606 | 579 | 496 | 493 | 533 |
| bump height | 1582 | 616 | 713* | 508 | 538 | 539 |
| effect2d kernel=0,1,0;1,-4,1;0,1,0; | 1083 | 491 | 534 | 420 | 421 | 416 |
| effect2d kernel=1,1,1,1,1;1,1,1,1,1;1,1,1,1,1; | 544 | 399 | 402 | 359 | 365 | 359 |
| pulsar light=false:quads=5:texture=false | 1498 | 564 | 504* | 383 | 368 | 383 |
| desktop blur-radius=5:effect=blur:passes=1:sep | 494 | 349 | 437* | 334 | 338 | 318 |
| desktop effect=shadow:windows=4 | 865 | 583 | 459* | 379 | 372 | 382 |
| buffer columns=200:interleave=false:update-dis | 341 | 497 | 410* | 365 | 357 | 357 |
| buffer columns=200:interleave=false:update-dis | 416 | 502 | 511 | 407 | 401 | 417 |
| buffer columns=200:interleave=true:update-disp | 373 | 507 | 453* | 411 | 412 | 396 |
| ideas speed=duration | 827 | 440 | 431 | 313 | 316 | 315 |
| jellyfish | 1462 | 476 | 538* | 294 | 289 | 282 |
| terrain | 104 | 143 | 144 | 127 | 127 | 127 |
| shadow | 628 | 569 | 583 | 453 | 444 | 437 |
| refract | 217 | 206 | 207 | 208 | 208 | 207 |
| conditionals f-steps=0:v-steps=0 | 1545 | 525 | 547 | 460 | 455 | 442 |
| conditionals f-steps=5:v-steps=0 | 2024 | 546 | 548 | 474 | 463 | 459 |
| conditionals f-steps=0:v-steps=5 | 1558 | 530 | 533 | 458 | 461 | 478 |
| function f-complexity=low:f-steps=5 | 1850 | 531 | 508 | 466 | 447 | 454 |
| function f-complexity=medium:f-steps=5 | 1965 | 550 | 547 | 466 | 468 | 466 |
| loop f-loop=false:f-steps=5:v-steps=5 | 2005 | 535 | 542 | 451 | 457 | 458 |
| loop f-steps=5:f-uniform=false:v-steps=5 | 1849 | 545 | 547 | 468 | 455 | 447 |
| loop f-steps=5:f-uniform=true:v-steps=5 | 1894 | 549 | 518 | 465 | 457 | 460 |
| **score** | **1325** | **521** | **518** | **428** | **423** | **426** |

## Engagement of the present-wait levers

- **Magma presents through a VkSwapchain.** The server logs `Set minImageCount = 5 (surface min 5, max 64,
  extra 0)` and `Swapchain created, extent = 1600x2560, swapchain imageCount = 6` for the compositor's
  on-screen session (the display surface, pre-rotated), and `surface min 11` swapchains (800x600, 2560x1600,
  664x438, 1x1, ...) for the clients' window surfaces. The clients' frames themselves go to shared images.
- **`MOBILEGL_MAGMA_EXTRA_SWAPCHAIN_IMAGES=2` applies**: `Set minImageCount = 7 (surface min 5, max 64, extra 2)`
  for the display swapchain, `13 (surface min 11 ... extra 2)` for the others. How the server gets an
  environment variable: the app's server process only takes what its own process environment holds (the app
  sets its list with `Os.setenv`; the server never reads `debug.mobilegl.env` or the config files), so the knob
  was injected with the Android wrap property
  `setprop "wrap.com.anland.consumer.mobilegl:mobilegl" "NAME=value"` before the app starts the process (empty
  string clears it). The +x2 arm scored +5 % windowed and 0 % fullscreen: within noise.
- **The Magma "off-screen work ahead of the acquire" lever never logged.** The line `a frame's off-screen work
  goes to the queue ahead of its window pass` appeared **0 times** in all three Magma "after" arms (glmark2
  runs, the compositor, a browser WebGL page, window hide/show). The pre-acquire flush with pending off-screen
  work did not happen on anland: the compositor's frames draw straight into the display image with no
  off-screen pass in front of it, and the clients present to shared images, not to a swapchain image.
- **Espryt does render to framebuffer 0 of an EGL window surface**: the server logs `this process owns a display
  (ServerDisplay installed); a ServerOwned window surface is served on its window`. The once-per-frame flush
  runs when a frame binds framebuffer 0 after a user framebuffer; it has no log line or counter, so its
  firing was not observed directly (no tracing, by request). No score effect either way.
- Net: on anland these levers cannot show up in glmark2. The wait they target (the display release gating
  the first swapchain / window-buffer use) sits on the compositor's presentation to the Android surface, which
  glmark2 only reaches through KWin's composition of its window.

## Functional check of the after build

- **Espryt after**: cold start (kwin +4.0 s, plasmashell +5.1 s, desktop on screen +6.6 s), swipe-away and
  reopen (0.2 s), a browser WebGL page at 114-120 fps, idle visible / hidden / locked at 319 % / 28 % / 29 %
  phone CPU, lock and unlock, hide and show: no fatal signal, no crash, desktop and page correct in the
  screenshots.
- **Magma after**: cold start, reopen, the WebGL page (150+ fps), idle visible and hidden, and every glmark2
  run were fine. **After waking the locked screen with the animated browser page open, the Magma server's
  memory ran away**: system swap fell from 11 GB free to under 1 GB within seconds, the server logged
  `VkBufferObject::Create failed: vmaCreateBuffer returned -1` and `Magma wire decline [UniformBufferBinding]`,
  the low-memory killer then killed it with signal 9, and the app's restart aborted with
  `DirectVulkan: the initial surface could not be created` while the screen was off. **The same sequence on
  the before build (`a759b4b7`) failed the same way** (one run each), so the stack did not cause it. It is
  worth chasing separately before trusting a long Magma desktop session with animated clients across
  lock/wake. The glmark2 measurements ran without the browser page and without lock cycles and never lost
  the server.
