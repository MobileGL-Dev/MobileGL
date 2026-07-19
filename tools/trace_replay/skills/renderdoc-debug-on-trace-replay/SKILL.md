---
name: renderdoc-debug-on-trace-replay
description: Capture and validate an exact frame from a MobileGL apitrace retrace on a connected Android device with RenderDoc/rdc-cli. Use for DirectVulkan or DirectGLES trace replay, mapping a target API call to an eglSwapBuffers frame, producing an .rdc plus a complete command manifest, checking capture stability, or troubleshooting Android TargetControl timing and replay failures.
---

# Capture a RenderDoc Trace Frame

Use the repository tool to queue TargetControl before launching the replay activity, keep the connection alive until capture completion, pull the RDC, and write a reproducible JSON manifest.

## Prepare

1. Work from the MobileGL repository root.
2. Confirm `python`, `adb`, `apitrace`, and `rdc` are on `PATH`.
3. Confirm the trace replay APK is installed and debuggable:

```powershell
adb devices -l
adb -s SERIAL shell pm path top.mobilegl.plugin.trace
rdc doctor
```

4. Pass the unpacked `trace.trace`, its golden PNG, the fixture target call, backend, and output path to `tools/trace_replay/skills/renderdoc-debug-on-trace-replay/scripts/capture_android_retrace.py`.

## Capture

Let the tool infer the zero-based target swap from `eglSwapBuffers` calls:

```powershell
python tools/trace_replay/skills/renderdoc-debug-on-trace-replay/scripts/capture_android_retrace.py --trace .trace-work/case/trace.trace --golden tools/trace_replay/fixtures/case.0002667619.png --target-call 2667619 --backend DirectVulkan --output captures/case-vulkan.rdc --serial SERIAL --json
```

Change only the backend and output for GLES:

```powershell
python tools/trace_replay/skills/renderdoc-debug-on-trace-replay/scripts/capture_android_retrace.py --trace .trace-work/case/trace.trace --golden tools/trace_replay/fixtures/case.0002667619.png --target-call 2667619 --backend DirectGLES --output captures/case-gles.rdc --serial SERIAL --json
```

Use `--target-swap N` when the mapping is already known. Use `--capture-frame N` only to override the backend rule deliberately.

The default mapping is:

- DirectGLES: capture the zero-based target swap.
- DirectVulkan: capture `target swap - 1`, so the capture closes at the terminal target Present. Queueing the terminal Vulkan swap itself can never finish when the retrace stops immediately after that Present.

The tool applies a 256 MiB RenderDoc `softMemoryLimit` for Vulkan, installs the GLES layer only for GLES, queues before Activity launch, drains asynchronous TargetControl registration messages, and restores device properties and forwards afterward. Do not replace it with a late `rdc script` call.

## Inspect the Output

Read `<output>.rdc.json`. Require all of the following:

- `success` is `true`.
- `capture.frame` equals the requested capture frame.
- `capture.api` matches Vulkan or OpenGLES.
- `byteSize` is non-zero and `sha256` is present.
- `retraceResult` says the replay completed; treat its SSIM separately from RDC validity.
- `commands` and `commandLines` contain the full reproducible command history.

A DirectVulkan SSIM below a GLES golden threshold can be a backend rendering difference. Do not reject the RDC solely for that reason if RenderDoc can replay it and the target render output is present.

## Validate with RenderDoc

Validate GLES locally:

```powershell
rdc close
rdc open captures/case-gles.rdc
rdc info --json
rdc count events
rdc count draws
rdc count passes
rdc assert-clean --min-severity high --json
```

Validate device-specific Vulkan captures on the original Android GPU:

```powershell
rdc android setup --json
adb -s SERIAL forward --list
rdc close
rdc open captures/case-vulkan.rdc --proxy 127.0.0.1:FORWARDED_REMOTE_PORT
rdc info --json
rdc count events
rdc count draws
rdc count passes
rdc assert-clean --min-severity high --json
```

Use the local TCP port mapped to `localabstract:renderdoc_39920` as `FORWARDED_REMOTE_PORT`. Prefer Android remote replay when a desktop GPU lacks the Android capture's memory types.

## Diagnose Failures

Read [references/android-renderdoc-troubleshooting.md](references/android-renderdoc-troubleshooting.md) when capture metadata is missing, a capture times out, the GLES layer does not load, Vulkan stalls on initial contents, `rdc android setup --serial` cannot select a listed device, or desktop replay rejects a Vulkan RDC.
