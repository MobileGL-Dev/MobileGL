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

   A profile carries `PROFILE_VERIFIED=1` only once its sysfs nodes and OPPs have been read
   off *that* device and one pinned window has been checked against them
   (`big_cur`/`little_cur`/`gpu_cur_khz` in the result JSON must match the pins). Until then
   it says `PROFILE_VERIFIED=0` and `bench.sh` / `session.sh` refuse to run against it unless
   `--allow-unverified-profile` is passed, which labels the run unpinned in the warning.
   **A profile that omits the key entirely is refused the same way** - the guard defaults to
   unverified, so copying a verified profile and editing the serial cannot inherit its verdict.
   Only the file can answer: both scripts reset `PROFILE_VERIFIED=0` immediately before sourcing
   it, so `PROFILE_VERIFIED=1` exported in your shell does not re-open the hole. Nor does a
   profile path that cannot be read get mistaken for an unverified one - it is reported as
   unreadable, and a path relative to the directory you ran the script from is resolved.
   (`profile.sh` pins nothing - it records a simpleperf profile - so it carries no such guard.)

   That refusal exists because the pin path is silent when it is wrong: the harness writes
   through `/proc/ppm/policy/hard_userlimit_*` and `/proc/gpufreq/gpufreq_opp_freq`, which are
   MediaTek nodes, and `su -c 'echo ... > /proc/...'` against a device that has neither fails
   without a non-zero exit. The run then reports numbers it believes were taken under a pin.

## Devices

| profile | device | verified |
|---|---|---|
| `devices/odinlite.env` | AYN Odin Lite, MT6877 / Mali-G68 | yes |
| `devices/xiaomi-adreno830.env` | Xiaomi, Snapdragon 8 Elite / Adreno 830 (`35d0befa`) | **no** - Qualcomm pin path not yet taught to `bench.sh` |
| `devices/oppo-mali.env` | Oppo / ColorOS, MediaTek + Mali (`3B159D009VZ00000`) | **no** - OPPs and thermal zone not yet read off the device |

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

## Pinning on the two MGPipe campaign devices (2026-09-07)

`bench.sh`'s `pin_freqs()` writes `/proc/ppm/policy/*` and `/proc/gpufreq/gpufreq_opp_freq`; **neither
path exists on `35d0befa` (SM8750) nor on `3B159D009VZ00000` (MT6993, which dropped both legacy
interfaces for `/proc/gpufreqv2/`)**. For those two devices run `bench.sh --no-pin` and pin with
`tools/device_bench/pin_device.sh <serial> pin|unpin|check` (pure adb + su; exit 0 PINNED, 1 DRIFT,
2 UNPINNED, so `check && measure` cannot measure unpinned). The profiles under `devices/` carry
`PROFILE_VERIFIED=1` for the nodes and pins named in the file, not for `bench.sh`'s ability to drive
them; the verification evidence is `docs/Disaggregated/devices/pin-verification-2026-09-07.md`.
