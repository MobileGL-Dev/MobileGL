# device_bench — in-game FPS benchmark harness

Scripted, repeatable in-game FPS measurement for MobileGL's two Android backends
(Espryt/DirectGLES and Magma/DirectVulkan) plus a MobileGlues reference run,
driven through the FCL fordebug flavor. Intended for A/B performance work and
release regression gates on real devices.

## How it measures

FCL's in-game FPS overlay counts `eglSwapBuffers` calls natively (renderer-
agnostic, not vsync-capped when the game runs with vsync off). When the overlay
is enabled, FCL's FPS thread logs one `FCLFPS: <n>` logcat line per second;
`bench.sh` collects those lines during the measurement window and reports
mean / median / min / max / stdev, alongside GPU busy%, SoC temperature, and
frequency-pin integrity.

## One-time setup (per device / world)

1. Install the FCL **fordebug** flavor (`com.tungsten.fcl.mgdebug.debug`). Its
   splash auto-launches the selected profile into the prepared world after a 5 s
   countdown.
2. In-game menu: enable **show FPS** (persists in `files/menu_setting.json`).
3. Prepare the benchmark world: fixed camera position, gamerules
   `doMobSpawning/doDaylightCycle/doWeatherCycle=false`, then save & quit once.
   `bench.sh` always `am force-stop`s the game (never saves), so every run
   replays the same state.
4. `options.txt`: desired `renderDistance`, `enableVsync:false`, high `maxFps`,
   `inactivityFpsLimit:"minimized"` (the "afk" default locks 30 fps after 60 s
   without input and ruins the window).
5. Root required (frequency pinning, GPU busy sampling).
6. Write a device profile under `devices/` (see `devices/odinlite.env`).

## Usage

```
./bench.sh --device devices/odinlite.env --backend magma          # 30 samples, 180 s warmup
./bench.sh --device devices/odinlite.env --backend espryt --label after-fix-X
./bench.sh --device devices/odinlite.env --backend mobileglues    # reference
```

Results append to `results/results.jsonl`; per-run screenshots (`pre.png`,
`post.png`) land in `results/<timestamp>-<backend>[-label]/` — always eyeball
them: the pre/post pair must show the same scene, or the run is invalid.

## Protocol discipline (hard-won, do not skip)

- **Thermal gate**: the script waits for the profile's start-temperature
  threshold. Runs started hot are not comparable to runs started cool.
- **Warmup 180 s**: ART JIT takes ~3 min to plateau (62→67→84 fps ramp was
  measured); short warmups underestimate by 10-20%.
- **Pins can be overridden by the thermal engine.** The result JSON records
  `big_cur/little_cur/gpu_cur_khz` sampled at window end — discard the run if
  they do not match the profile pins.
- **Paired runs**: absolute FPS drifts across sessions (camera angle, world
  state). A/B comparisons must be back-to-back runs in the same session.
- **F3 off** for standard numbers (the F3 debug overlay multiplies per-draw
  overhead and skews backends differently).
- The FPS overlay itself must be ON (it is what produces the FCLFPS lines).
