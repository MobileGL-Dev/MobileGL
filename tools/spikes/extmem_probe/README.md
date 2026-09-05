# extmem_probe — disaggregation spike B (external memory)

A standalone Android command-line probe that answers one question per device:

> Can a server-allocated `HOST_VISIBLE|HOST_COHERENT` `VkDeviceMemory` be shared
> with another process and mapped there, and by which route?

This is the P0 spike that decides the `AcquirePersistentMap` tier in plan B §8.3
(T0 = server imports a client allocation, T1 = server exports its own, T2 = give
up and return `nullptr`). It links nothing from MobileGL and is not part of the
project's CMake build graph.

## What it does

* **phase A — enumeration.** Vulkan device identity + memory types, and per
  handle type (`OPAQUE_FD`, `DMA_BUF`, `HOST_ALLOCATION`, `AHARDWAREBUFFER`) the
  `vkGetPhysicalDeviceExternalBufferProperties` verdict for the buffer usage
  MobileGL actually needs. Then a headless EGL pbuffer context reports
  `GL_EXT_memory_object{,_fd}`, `GL_EXT_external_buffer`, `GL_EXT_buffer_storage`,
  `GL_OES_EGL_image_external{,_essl3}` and `EGL_ANDROID_get_native_client_buffer`.
* **T1 — server exports.** Allocates a `HOST_VISIBLE|HOST_COHERENT` buffer memory
  with `VkExportMemoryAllocateInfo`, maps it, writes a pattern, exports an fd with
  `vkGetMemoryFdKHR` (opaque-fd, then dma-buf), hands the fd to a second process
  over `SCM_RIGHTS`, and has that process (a) `mmap()` the fd and (b) import it
  into its own `VkDeviceMemory` and `vkMapMemory` it. Both sides write and both
  sides compare, so a one-directional or copy-on-import mapping is caught.
* **T0 — server imports.** The second process allocates an `AHardwareBuffer` BLOB
  (`CPU_READ_OFTEN|CPU_WRITE_OFTEN|GPU_DATA_BUFFER`), writes a pattern under
  `AHardwareBuffer_lock`, and sends it with
  `AHardwareBuffer_sendHandleToUnixSocket`. The first process reads it back three
  ways — CPU lock, `VkDeviceMemory` imported through
  `VK_ANDROID_external_memory_android_hardware_buffer`, and a GL buffer created
  with `eglGetNativeClientBufferANDROID` + `glBufferStorageExternalEXT` mapped
  persistent/coherent — writes through each, and the allocating process verifies
  every write.
* **T3 — host pointer import.** If `VK_EXT_external_memory_host` is advertised,
  imports a memfd-backed, alignment-corrected `mmap` region as a `VkDeviceMemory`
  and maps it; also passes the memfd to the second process for a cross-process
  round trip.

Process topology mirrors the target design (the client spawns the server): the
probe re-execs `/proc/self/exe --child=<route>` and hands the child one end of a
`socketpair` on fd 3. A bare `fork()` is not usable — neither side's Vulkan
driver survives it, and both sides need live Vulkan.

## Build and run

```sh
ANDROID_NDK=$HOME/android-sdk/ndk/27.3.13750724 ./build_android.sh /tmp/extmem-build
adb -s <serial> push /tmp/extmem-build/extmem_probe /data/local/tmp/p0-extmem/
adb -s <serial> shell /data/local/tmp/p0-extmem/extmem_probe
```

There is also a host build (`cmake -S . -B <dir>` with no toolchain file). It
compiles T0 out — `AHardwareBuffer` is Android-only — and exists for exactly one
reason: running T1/T3 against a driver that is known to implement them
(lavapipe: `VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json
EGL_PLATFORM=surfaceless`) proves the harness reports a working route as
working, which is what makes a device-side `FAIL` attributable to the device
driver rather than to this program. It is not a substitute for a device run.

Options: `--size=BYTES` (default 65536; the payload is split into 4 KiB regions,
one per writer), `--only-t0` / `--only-t1` / `--only-t3`.

Output is a per-route `RESULT <route> <status> <detail>` line stream plus a
summary table; `status` is one of `OK`, `PARTIAL`, `UNSUPPORTED`, `FAIL`, `SKIP`.
Driver error codes are printed verbatim (`VkResult` names, `errno`, GL enums) —
that is the payload of the spike, so do not summarise them away.

Two details worth knowing when reading T1 output:

* the child reads through *both* the plain `mmap` and the imported
  `VkDeviceMemory` before it writes through either, because a driver whose
  exported fd maps at an offset would otherwise have its payload overwritten by
  the probe's own first write, and the second read would report a false failure;
* when the direct compare fails, the child scans the mapping for the exporter's
  payload and reports `payloadAt=<offset>`. `payloadAt=4096` with a clean Vulkan
  import (lavapipe's answer) means the fd is shareable but its offset-0 is not
  the allocation's base — a route that only works if that offset is
  discoverable, which opaque-fd does not promise.
