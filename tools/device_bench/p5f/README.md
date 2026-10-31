# P5f Android public-pixel gate

Build `bash build_android.sh <Linux-source-tree> <full-source-SHA>` with NDK 27.3/API26/arm64.
This produces one disaggregated/PUSH library and a runner using only public GL/EGL APIs.
Original symbols stay in `library/` and `pixels/`; bundle copies have only debug data stripped.
No FCL/APK configuration or private diagnostic export is required.

The coordinator reboots Redmi `2f7cbe2e` before each arm and waits for `sys.boot_completed=1`.
Run `run_pixel_arm.sh <bundle> <new-output-dir> <DirectGLES|DirectVulkan> <monolith|inproc> <0|1>`
from Windows Git Bash; the script itself never reboots. Set `ADB_BIN` if the Windows adb path differs.
Then run `python3 verify_arm.py <output-dir>`.

Arms: GLES monolith, GLES inproc role0/role1; Magma monolith, Magma inproc role1/role0.
All use the same bundle. Check six distinct boot IDs and equal host/device hashes.
Performance is recorded only; this short pixel suite is not an MC/FPS benchmark.

11 selected cases cover clip distance, six layered image copies and two mipmap cases.
The verifier requires the exact case set with no missing, extra, duplicate or unexecuted entries.
It allows only Magma's two pre-existing per-distance-enable limitations, matched by
exact case and skip reason. They are never counted as passes and must be identical across arms.
Every other skip/failure is an error. Every actual stats window must have `rsp=0`; inproc must
publish `vbs>0` and correct runtime/capability logs, monolith must keep `vbs=0`.
The current verifier requires `run-ahead ARMED` on both inproc backends. To replay
the historical P5f bundle where Magma withheld that capability, use the verifier
from the bundle's source revision.

Android's normal integration target omits F1/Ct/DualBlock and private peek bridges. This runner
therefore does not claim those host cases ran on Android. See `device-report.md` in the P5f notes
for the completed six-arm run and immutable artifact hashes.
