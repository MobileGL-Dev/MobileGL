# Magma run-ahead FCL check

This runner targets only Redmi `2f7cbe2e`, package
`com.tungsten.fcl.mgdebug.debug`, Minecraft `26.3-rc-3`, existing world `test`.
It expects the launcher to Quick Play that world. The existing GLES renderer entry
reads `mg_env.txt`; use it with the explicit backend override. The launcher's built-in
Magma entry may select monolith without reading this override.

Build/install one FCL APK containing the current disaggregated/PUSH MobileGL library.
Archive its source revision and SHA256, including the packaged and installed
`libMobileGL.so`. Back up `/sdcard/FCL/mg_env.txt`, `mg_transport.txt` and the world
before running. After any reboot, wait for shared game storage as well as boot
completion. `ADB_BIN` can select the host adb executable; every call pins the serial.

Run each arm serially with a fresh evidence directory:

```sh
python run_fcl_arm.py --backend DirectVulkan --transport inproc --run-ahead 1 --out magma-on
python run_fcl_arm.py --backend DirectVulkan --transport inproc --run-ahead 0 --out magma-off
python run_fcl_arm.py --backend DirectGLES --transport inproc --run-ahead 1 --out gles-on
```

The runner requires fresh world logs, the actual runtime backend/capability,
strict role split, zero residual pulls, progressing frames, and at least 60 seconds
without crash/restart. Its success remains **pending screenshot review**: inspect
`in-world.png` and `final.png` for world/UI rendering and orientation. Save and quit
the world before starting the next arm. Restore the two original configuration
files and verify their hashes when finished; the runner intentionally leaves the
app available for inspection and does not restore configuration itself.

FPS derived from polled frame counters is approximate and includes warm-up. Without
fixed clocks, camera, weather, and workload this is a correctness run, not a benchmark.
The host lane proves actual client lead over held apply and present-credit blocking:

```sh
ctest --test-dir build-split -L integration-magma-runahead --no-tests=error
python scripts/ci/magma_runahead_checks.py negative --out negative --execute
python scripts/ci/magma_runahead_checks.py validation --out syncval --execute
```

Run these host lanes serially because they share private logs. Validation requires
the Khronos layer and proves loader insertion as well as checking VUIDs and hazards.
