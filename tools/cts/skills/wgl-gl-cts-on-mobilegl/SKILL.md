---
name: wgl-gl-cts-on-mobilegl
description: Build MobileGL as a Windows x64 WGL drop-in opengl32.dll, build or reuse VK-GL-CTS glcts, run Khronos GL30 through GL46 core suites against DirectGLES and DirectVulkan, resume after crashes or idle timeouts, and produce validated Markdown/JSON conformance reports. Use when Codex needs to compile MobileGL's WGL target, connect it to desktop OpenGL CTS on Windows, rerun selected GL core mustpass lists, or verify that CTS loaded MobileGL instead of the system OpenGL driver.
---

# WGL OpenGL CTS on MobileGL

Use `tools/cts/scripts/wgl_glcts_pipeline.py` as the single entry point. It
configures both Visual Studio builds, assembles a private runtime, verifies the
loaded WGL implementation, calls the crash-resuming runner, and generates the
multi-suite report.

## Prerequisites

- Run on Windows x64 with Git, Python 3.9 or newer, CMake, Visual Studio 2022's
  Desktop C++ workload, and a Vulkan SDK visible to MobileGL's CMake configure.
- Use the already selected MobileGL worktree. Inspect `git status` first and do
  not create another worktree unless the user explicitly asks.
- Initialize MobileGL submodules:

```powershell
git submodule update --init --recursive
```

- Prepare a VK-GL-CTS checkout at a citable release tag, then fetch its pinned
  externals:

```powershell
git -C D:\VK-GL-CTS checkout opengl-cts-4.6.8.1
python D:\VK-GL-CTS\external\fetch_sources.py
```

- For DirectGLES, provide one x64 ANGLE directory containing matching
  `libEGL.dll`, `libGLESv2.dll`, and `d3dcompiler_47.dll`. DirectVulkan does not
  need ANGLE.
- For DirectVulkan, provide a working Vulkan loader plus a GPU-vendor ICD and
  driver. The Vulkan SDK alone does not provide a usable GPU device.

## Run the full matrix

From the MobileGL worktree root:

```powershell
python tools\cts\scripts\wgl_glcts_pipeline.py `
  --cts-source D:\VK-GL-CTS `
  --work-root D:\MobileGL-WGL-CTS `
  --angle-dir C:\path\to\angle-x64 `
  --backends DirectGLES DirectVulkan `
  --versions gl30 gl31 gl32 gl33 gl40 gl41 gl42 gl43 gl44 gl45 gl46
```

The defaults deliberately reproduce the proven Windows setup:

- Visual Studio 17 2022, x64, Release;
- VK-GL-CTS `DEQP_TARGET=default`, which selects desktop WGL on Windows;
- MobileGL copied beside `glcts.exe` as `opengl32.dll`;
- WGL context, FBO surface, `rgba8888d24s8`, hidden window, watchdog and crash
  handler enabled;
- `--deqp-terminate-on-device-lost=disable` supplied by the underlying runner.

Do not replace the FBO surface with the default framebuffer when comparing
backends; it changes the readback path and invalidates comparison with the
established runs.

## Mandatory preflight

Leave preflight enabled for a new binary. It must prove all of the following
before the full suite starts:

- QPA vendor contains `MobileGL`;
- DirectGLES renderer contains `Espryt`, or DirectVulkan contains `Magma`;
- QPA command line records `--deqp-gl-context-type=wgl`;
- MobileGL's log reports a GL version at least as high as the highest selected
  suite.

Treat any preflight failure as a hard stop. It commonly means `glcts.exe` loaded
the system `opengl32.dll`, ANGLE DLLs have the wrong architecture, or the driver
still reports too low a GL version.

## Common variants

Run DirectVulkan only, without ANGLE:

```powershell
python tools\cts\scripts\wgl_glcts_pipeline.py `
  --cts-source D:\VK-GL-CTS --work-root D:\MobileGL-WGL-CTS `
  --backends DirectVulkan --versions gl43 gl44 gl45 gl46
```

Build and preflight without starting a multi-hour CTS run:

```powershell
python tools\cts\scripts\wgl_glcts_pipeline.py `
  --cts-source D:\VK-GL-CTS --work-root D:\MobileGL-WGL-CTS `
  --angle-dir C:\path\to\angle-x64 `
  --skip-run --skip-report
```

Reuse previously built binaries with `--skip-mobilegl-build`,
`--skip-cts-build`, `--mobilegl-dll`, `--glcts-exe`, and
`--cts-modules-dir`. Continue to use a modules directory containing the
`gl_cts` data tree; the executable directory alone is insufficient.

Pass extra dEQP options with the equals form so argparse does not consume the
leading dashes:

```powershell
--deqp-arg=--deqp-log-images=enable
```

## Resume and artifacts

Repeat the exact command and `--work-root` to resume. The runner recovers
completed QPA cases plus `crashed.txt` and `hung.txt`, then schedules only
unaccounted cases.

The pipeline prints full SHA-256 fingerprints and uses their first 16
hexadecimal characters as directory names: `runtime/<runtime-prefix>` and
`runs/<run-prefix>`. The run fingerprint covers the CTS data tree,
runner/report tools, caselists, dEQP arguments, explicit `--env` values, tracked
ambient GL/Vulkan environment, and the timeout settings that determine
Crash/Hang classification. The manifest records those inputs plus
`max_rounds`, suite-error continuation policy, source commits and dirty state,
preflight identity, suite errors, and report status.

The runner refuses to attach a new `run_state.json` to old QPA or sidecar files
by default. Invoke `run_cts_windows.py --adopt-legacy` directly only after
verifying that those artifacts match the backend, caselist, binaries, and dEQP
arguments. Pipeline reports require every suite state to match the current run
fingerprint.

Read the final outputs at:

```text
<work-root>/runs/<run-prefix>/reports/gl-cts-summary.md
<work-root>/runs/<run-prefix>/reports/gl-cts-summary.json
```

Use the exact run root printed by the pipeline.

Do not claim completeness when the report validation is incomplete or when the
manifest records suite errors. Keep `NotSupported` separate from hard failures
when prioritizing implementation work.
