# extmem_probe — disaggregation spike B (external memory)

A standalone Android command-line probe that answers one question per device:

> Can the memory behind `AcquirePersistentMap` be shared with another process
> and mapped there — for **both** backends — and by which route?

This is the P0 spike that decides the `AcquirePersistentMap` tier in plan B §8.3
(T0 = server imports a client allocation, T1 = server exports its own, T2 = give
up and return `nullptr`). It links nothing from MobileGL and is not part of the
project's CMake build graph.

Both backends are asked, because they reach a persistent map by different APIs:
DirectVulkan ("Magma") maps a `VkDeviceMemory`, while DirectGLES ("Espryt")
calls `glBufferStorageEXT` + `glMapBufferRange(PERSISTENT|COHERENT)`. A Vulkan
answer alone does not decide the tier for DirectGLES, so every tier has a GLES
leg.

## What it does

* **phase A — enumeration.** Run context (uid, pid, SELinux domain — see the
  caveat below), Vulkan device identity + memory types, and per handle type
  (`OPAQUE_FD`, `DMA_BUF`, `HOST_ALLOCATION`, `AHARDWAREBUFFER`) the
  `vkGetPhysicalDeviceExternalBufferProperties` verdict for the buffer usage
  MobileGL actually needs. Then a headless EGL pbuffer context reports
  `GL_EXT_memory_object{,_fd}`, `GL_EXT_external_buffer`, `GL_EXT_buffer_storage`,
  `GL_OES_EGL_image_external{,_essl3}`, `EGL_ANDROID_get_native_client_buffer`,
  and `GL_DEVICE_UUID_EXT` against the Vulkan `deviceUUID` (they must match for
  an fd import to be legal, so a mismatch explains a later decline).
* **T1 — server exports (Vulkan).** Allocates a `HOST_VISIBLE|HOST_COHERENT`
  buffer memory with `VkExportMemoryAllocateInfo`, maps it, writes a pattern,
  takes a **GPU access** on it (below), exports an fd with `vkGetMemoryFdKHR`
  (opaque-fd, then dma-buf), hands the fd to a second process over `SCM_RIGHTS`,
  and has that process (a) `mmap()` the fd and (b) import it into its own
  `VkDeviceMemory` and `vkMapMemory` it. Both sides write and both sides
  compare, so a one-directional or copy-on-import mapping is caught.
* **T1-gles — server exports (GLES).** The same exported fd, imported as GL
  buffer storage: `glCreateMemoryObjectsEXT` + `glImportMemoryFdEXT` +
  `glBufferStorageMemEXT`, then `glMapBufferRange(PERSISTENT|COHERENT)` — first
  **in-process** (isolates "GL can import this fd at all" from "the fd survives
  a process boundary"), then **cross-process**. Because drivers disagree about
  how the import must be phrased, each attempt walks a ladder over
  {dedicated flag} × {import size = `VkMemoryRequirements::size` or the fd's own
  size} × {buffer size}, and the report names the rung the driver accepted
  (`accepted=…`) plus every rung it rejected with its GL error (`ladder: …`), so
  a driver *preference* is never reported as a missing capability. A driver that
  backs the storage but refuses `PERSISTENT|COHERENT` is reported separately
  from one that refuses the storage — that distinction is exactly T1 vs T2 for
  DirectGLES.
* **T0 — server imports.** The second process allocates an `AHardwareBuffer` BLOB
  (`CPU_READ_OFTEN|CPU_WRITE_OFTEN|GPU_DATA_BUFFER`), writes a pattern under
  `AHardwareBuffer_lock`, and sends it with
  `AHardwareBuffer_sendHandleToUnixSocket`. The first process reads it back three
  ways — CPU lock, `VkDeviceMemory` imported through
  `VK_ANDROID_external_memory_android_hardware_buffer`, and a GL buffer created
  with `eglGetNativeClientBufferANDROID` + `glBufferStorageExternalEXT` mapped
  persistent/coherent (the DirectGLES form of T0) — takes a GPU access, writes
  through each, and the allocating process verifies every write with
  `AHardwareBuffer_lock`.
* **T3 — host pointer import.** If `VK_EXT_external_memory_host` is advertised,
  both directions are exercised: the importing process allocates the memfd
  (`T3-external-memory-host`, plus a plain cross-process memfd round trip), and —
  the direction that actually makes T3 a tier — the **client** allocates the
  memfd, writes to it, and the **server** mmaps the received fd, imports the
  client's host pointer into a `VkDeviceMemory`, reads what the client wrote,
  writes back, and takes a GPU access on the client's memory
  (`T3-client-memfd-server-import`).

**Every tier row takes a real GPU access** before it can be `OK`:
`vkCmdCopyBuffer` out of the shared allocation into a private staging buffer
(mismatch ⇒ the GPU could not read what the peer wrote) plus `vkCmdFillBuffer`
into it, `vkQueueWaitIdle`, and an explicit host-read barrier; the peer then
checks the filled region through *its* mapping. Without it an `OK` would only
mean that a map call returned a pointer, not that the tier survives GPU use.

Process topology mirrors the target design (the client spawns the server): the
probe re-execs `/proc/self/exe --child=<route>` and hands the child one end of a
`socketpair` on fd 3. A bare `fork()` is not usable — neither side's Vulkan
driver survives it, and both sides need live Vulkan.

## Reading the verdict

`status` is one of `OK`, `PARTIAL`, `UNSUPPORTED`, `FAIL`, `SKIP`, and the rule
is deliberately strict:

* **`OK`** — every *decisive* leg round-tripped bytes **in both directions**
  (the allocating side's payload was visible to the other side, and the other
  side's write came back). A successful map call with no byte ever compared is
  never `OK`.
* **`PARTIAL`** — at least one decisive leg round-tripped, but not all.
* **`FAIL`** — no decisive leg round-tripped. A `FAIL` always names the failing
  step and its driver error code in `why: …`.
* **`UNSUPPORTED`** — the route's extension is absent, or the driver never
  advertised the handle type as `EXPORTABLE` and then declined it. The same rule
  is applied at *every* export failure site: a decline on a handle type the
  driver advertised as `EXPORTABLE` is a driver bug and reports `FAIL`; the same
  decline on one it never advertised reports `UNSUPPORTED`.

Each row starts with a per-leg trace, e.g.
`rawmmap[i]=no vkimport[D]=rt gpu[D]=rt` — `[D]` decisive, `[i]` informational,
`rt` = round-tripped, `read-only`/`write-only`/`no`/`notrun` otherwise. Driver
error codes are printed verbatim (`VkResult` names, `errno`, GL enums) — that is
the payload of the spike, so do not summarise them away.

Two details worth knowing when reading T1 output:

* the child reads through *both* the plain `mmap` and the imported
  `VkDeviceMemory` before it writes through either, because a driver whose
  exported fd maps at an offset would otherwise have its payload overwritten by
  the probe's own first write, and the second read would report a false failure;
* the raw `mmap` leg is **informational for opaque-fd** and decisive only for
  dma-buf. Vulkan forbids interpreting an opaque-fd payload outside the driver,
  so a driver that refuses it is conformant and MobileGL would never take that
  route; dma-buf is the opposite — a CPU mapping is the point of the handle type.
  When the direct compare fails the child scans the mapping for the exporter's
  payload and reports `payloadAt=<offset>`; `payloadAt=4096` with a clean Vulkan
  import (lavapipe's answer) means the fd is shareable but its offset-0 is not
  the allocation's base.

## SELinux domain caveat (important)

Run as `adb shell /data/local/tmp/extmem_probe`, this executes in the **`shell`**
SELinux domain (`u:r:shell:s0`), **not** the `untrusted_app` domain MobileGL
actually runs in. `shell` and `untrusted_app` do not share the same rules for
dmabuf/ashmem allocators, gralloc, and device nodes, so a route that works here
can still be denied in the app — and, less often, the reverse. The probe prints
the domain it actually got in the run-context header and repeats the caveat in
the summary; record it with the results.

To answer the question for the real domain, the same binary has to be executed
from an app process. That is **not implemented here**: the intended vehicle is
the trace app's spike hook from the spike-A package — ship `extmem_probe` as a
`jniLib`/asset, exec it from the app's own uid with its stdout redirected to
`/sdcard/MG/extmem-probe.log`, and compare the summary table with the `adb
shell` one. Any row that differs between the two is an SELinux/domain finding,
not a driver finding.

## Build and run

```sh
ANDROID_NDK=$HOME/android-sdk/ndk/27.3.13750724 ./build_android.sh /tmp/extmem-build
```

One line to push, run and collect on a device:

```sh
S=<serial>; adb -s $S push /tmp/extmem-build/extmem_probe /data/local/tmp/extmem_probe \
  && adb -s $S shell "chmod 755 /data/local/tmp/extmem_probe && /data/local/tmp/extmem_probe; echo EXIT=\$?" \
  | tee out-$S.txt
```

There is also a host build (`cmake -S . -B <dir>` with no toolchain file). It
compiles T0 out — `AHardwareBuffer` is Android-only — and exists for exactly one
reason: running T1/T3 against a driver that is known to implement them
(lavapipe: `VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json
EGL_PLATFORM=surfaceless`) proves the harness reports a working route as
working, which is what makes a device-side `FAIL` attributable to the device
driver rather than to this program. It is not a substitute for a device run.

**Known host-build limitation.** On lavapipe + llvmpipe the two `T1-gles` rows
report `FAIL` with `glBufferStorageMemEXT -> GL_OUT_OF_MEMORY` on every rung of
the ladder, even though `GL_DEVICE_UUID_EXT` matches the Vulkan `deviceUUID`:
llvmpipe's GL does not implement importing a lavapipe opaque-fd allocation.
That is a Mesa interop gap, not a harness defect — the T1/T3 rows are the ones
the host run validates, and they must all read `OK`. The GLES legs are validated
only on the devices.

Options: `--size=BYTES` (default 65536; the payload is split into 4 KiB regions,
one per writer — A payload, B/C/D importer writes, E GPU fill, F in-process GL
write), `--only-t0` / `--only-t1` / `--only-t3` / `--only-gles`, `--no-gles`.
