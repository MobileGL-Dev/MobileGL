# Android RenderDoc retrace troubleshooting

## TargetControl timing

- Start `tools/trace_replay/skills/renderdoc-debug-on-trace-replay/scripts/queue_android_frame.py` before launching `TraceReplayActivity`. A fast retrace can pass the requested frame before a late client connects.
- Keep TargetControl connected until `NewCapture` arrives. A queued request alone is not sufficient evidence that the RDC finished.
- Drain the asynchronous `RegisterAPI` and `CapturableWindowCount` messages before calling `QueueCapture`; otherwise `NewCapture` can be lost.
- Do not use the daemon-backed `rdc script` path for a capture that may exceed 30 seconds. Its outer RPC times out even when the device later writes a valid RDC. The repository helper imports the RenderDoc module discovered by `rdc` directly and has an independent capture timeout.

## Frame selection

- Map the fixture target call to the exact `eglSwapBuffers` call, or to the first swap after a non-swap target call.
- Use the zero-based swap index for DirectGLES.
- Use `target swap - 1` for DirectVulkan when the target swap is the retrace's terminal Present. RenderDoc needs that Present to close the queued frame.
- Override with `--capture-frame` only after inspecting the trace structure.

## Vulkan capture and replay

- Keep the default 256 MiB `softMemoryLimit`. Large Vulkan initial contents can otherwise create a device memory spike and stall or kill capture. The tool encodes and restores CaptureOptions automatically.
- Replay Adreno captures on the original Android GPU when a desktop GPU reports an unavailable memory type. This is a replay compatibility issue, not proof that the RDC is corrupt.
- Use the local port from `adb forward --list` whose remote endpoint is `localabstract:renderdoc_39920` with `rdc open --proxy`.
- Expect an Android Vulkan render target export to appear vertically flipped when the surface transform is preserved.

## GLES layer injection

The GLES loader must be able to load `libVkLayer_GLES_RenderDoc.so` from the debuggable app directory. The tool copies it from:

```text
/data/local/debug/vulkan/libVkLayer_GLES_RenderDoc.so
```

If that source is missing, run `rdc android setup` first. Verify injection with:

```powershell
adb -s SERIAL shell getprop debug.gles.layers
adb -s SERIAL shell run-as top.mobilegl.plugin.trace ls -l /data/user/0/top.mobilegl.plugin.trace/libVkLayer_GLES_RenderDoc.so
```

## Android debug settings

Require these global settings for the replay package:

```text
gpu_debug_app=top.mobilegl.plugin.trace
enable_gpu_debug_layers=1
gpu_debug_layers=VK_LAYER_RENDERDOC_Capture
```

Some Xiaomi ROMs reject shell writes with `WRITE_SECURE_SETTINGS`. Configure the RenderDoc GPU debug layer through the device developer options or RenderDoc setup flow. The tool avoids rewriting settings that are already correct and restores only keys it actually changed.

## rdc-cli device selection

RenderDoc 1.41 can enumerate a device as a raw serial while `rdc android setup --serial` compares against `adb://SERIAL`. If the explicit command says the listed serial is not found and exactly one device is connected, retry without `--serial`. The repository tool performs this bounded fallback automatically.

## Completion checks

Treat capture as successful only when all checks pass:

1. TargetControl returns `NewCapture` with the expected frame and API.
2. `adb pull` produces a non-empty RDC and the manifest records its SHA-256.
3. RenderDoc opens the RDC and reports more than zero events and draws.
4. `rdc assert-clean --min-severity high` passes.
5. The retrace result reaches the target call. Evaluate fixture SSIM according to the selected backend.
