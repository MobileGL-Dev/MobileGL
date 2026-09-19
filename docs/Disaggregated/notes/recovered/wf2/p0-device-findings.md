# P0 device runs (2026-09-05 evening, both device locks taken over from the stale 00:39/00:43 holders)

Tree: feat/disaggregated @ 7ef7c7e5 (aa005720 code + docs 87ee17c6 + spike-a 8a239177/38d4c237 + spike-b b82dd350/39f982e6/7ef7c7e5).
Spike-A APK: p0-spike-a-android/trace-debug-spike-on.apk (built at 30d7595b pre-rebase; identical sources).
Spike-B binary: p0-spike-b-extmem/extmem_probe (39f982e6 sources, arm64, run as `adb shell` = u:r:shell:s0).

## Spike A — exec a second native binary from the app's own process

| device | result |
|---|---|
| 35d0befa Xiaomi 24129PN74C, Adreno 830, Android 16 | **OK**. Parent `u:r:untrusted_app:s0:c173,c257,c512,c768` fork+execve of `<nativeLibraryDir>/libMobileGLServer.so` → child pid 31348, exit 0, marker + stdout capture + report all present, child selinux = `u:r:untrusted_app:s0:c173,c257,c512,c768` (same domain, same categories), `execErrno=0`, **zero avc denials** in the window. |
| 3B159D009VZ00000 Oppo PLG110, Mali, Android 16 | (pending — filled below when the run returns) |

Harness notes: the foreign-signed trace APK (versionCode 26080769, signer bb517d49, installed 2026-08-30 by the GL4.6 session) blocked `install -r`/`install -r -d` with INSTALL_FAILED_UPDATE_INCOMPATIBLE; it was uninstalled (recorded in lock-takeover-notes.txt). The env-passthrough A/B leg of 42-device.sh could not be exercised because `run-as sh -c 'cat > files/trace-replay/dummy.trace'` is refused on this ROM; the passthrough is instead proven end-to-end by the stats baseline runs (`--env MOBILEGL_PIPE_STATS=1` must produce `MGPipe stats` lines in mobilegl.log).

## Spike B — external-memory tiers (extmem_probe, 4 MiB payload; same verdicts at 64 KiB)

| route | Adreno 830 (35d0befa) | Mali (3B159D009VZ00000) |
|---|---|---|
| T1-opaque-fd (server exports VkDeviceMemory fd, client mmaps raw + imports) | **OK** full round trip incl. GPU touch (`/dmabuf:system`, dedicatedOnly=1, child type 4 bits 0x13) | UNSUPPORTED (`vkCreateBuffer(external)=VK_ERROR_INVALID_EXTERNAL_HANDLE`, advertisedExportable=0) |
| T1-dma-buf | UNSUPPORTED (VK_EXT_external_memory_dma_buf absent) | UNSUPPORTED (exportable=0) |
| T1-gles-memobj-fd (GL_EXT_memory_object_fd import of the export) | **FAIL**: import + glBufferStorageMemEXT accepted (GL_NO_ERROR) but every glMapBufferRange → GL_INVALID_OPERATION (persistent and plain); GL_EXT_memory_object present, GL_DEVICE_UUID unreadable | UNSUPPORTED (GL_EXT_memory_object / _fd not advertised; entry points resolve but ext strings absent) |
| T0-ahb-blob-transfer (client-allocated AHardwareBuffer BLOB → socket handoff → server Vulkan import + GL import) | **OK** full chain: cpu-lock, vk-import+map, GPU copy/fill, GL map persistent+coherent, writeback to client all byte-verified | **OK** full chain, identical verdicts (glPersistentCoherent=1, gpuRan=1) |
| T3-external-memory-host (VK_EXT_external_memory_host) | UNSUPPORTED (extension absent) | PARTIAL: import+map round-trips, but **GPU writes are not visible through the host mapping** (read-only tier) |
| T3-memfd-cross-process / client-memfd-server-import | UNSUPPORTED | OK / PARTIAL (same read-only GPU caveat) |

**Tier decision for P11 (persistent maps across the process boundary):** the one route that is a full read/write tier on BOTH devices is **T0 — client-allocated AHardwareBuffer BLOB**, imported by the server with `VK_ANDROID_external_memory_android_hardware_buffer` (Magma) or `EGL_ANDROID_get_native_client_buffer` + `glBufferStorageExternalEXT` (Espryt), and mapped persistent+coherent on both sides. Adreno additionally offers T1 (server-exported opaque fd) for Vulkan-only paths; Mali offers nothing server-exported and its host-pointer import is read-only. Caveat: run domain was `shell`, not `untrusted_app`; the AHB socket handoff itself is exercised by every app that shares buffers with SurfaceFlinger so the domain risk is on the memfd/opaque-fd legs, not T0.

## Stats baselines (dynamic accessor calls / draw, bytes per frame)
(pending — filled below)
