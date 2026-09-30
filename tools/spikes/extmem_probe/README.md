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
from an app process. The vehicle is the trace app's spike hook from the spike-A
package: build the trace APK with `-Pmobilegl.buildExtmemProbe=ON` (packages the
probe as `libMobileGLExtmemProbe.so`), put the options in
`<output_dir>/spike-spawn.txt.args` (via `run-as`), then
`am start ... --es output_dir <dir> --es mobilegl_spike_spawn libMobileGLExtmemProbe.so`.
The hook execs `<lib> <marker>`; the probe takes the positional argument as the
marker, reads `<marker>.args`, prints to `<marker>.stdout`, and writes a one-line
verdict to the marker. Compare the summary table with the `adb shell` one (run the
same `.so` from `/data/local/tmp`). Any row that differs between the two is an
SELinux/domain finding, not a driver finding.

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
write), `--only-t0` / `--only-t1` / `--only-t3` / `--only-gles`, `--no-gles`,
`--regions-at-end` (put the A..F window in the last 32 KiB of `--size`, so a large
size proves the far end is mapped), `--sustained-lock[=ROUNDS]` (default 8) /
`--only-sustained`: the **T0S** rows — the client locks the AHB once and never
unlocks while, per round, it CPU-writes REG_A, the server's GPU reads it (Vulkan
copy; GL compute on the `glBufferStorageExternalEXT` buffer) and fills REG_E / REG_F,
and the client reads both fills through the still-held pointer; after the last
round it unlocks and relocks once more (`after unlock+relock`), which separates
"GPU writes never landed" from "the held mapping kept stale lines".

## T4 — an image allocated outside Android

Can the render server draw into an image that was allocated **outside** Android —
a GBM buffer minted in a Linux container on the same kernel? That is the
`{Owner=Platform, Storage=OfferedImage}` cell an Anland-style Wayland host needs,
and nothing in MobileGL imports a dma-buf today. It is a per-device fact, not a
code fact.

### What the device answered

Redmi 23117RK66C, Adreno 750, Android 16, Droidspaces Fedora 44 container,
2026-09-30. The answer has two halves and they point in opposite directions.

| row | verdict | what it means |
|---|---|---|
| `T4-image-import` | `UNSUPPORTED` | This driver does not advertise `EGL_EXT_image_dma_buf_import`, so a container-minted dma-buf has no spelling as an EGLImage here. A capability answer, not a failure — and the reason the second row exists. |
| `T4-ahb-image` | `FAIL`, and the failure is the finding | **Android-side allocation works end to end.** `AHardwareBuffer_allocate` → `eglGetNativeClientBufferANDROID` → `eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID)` → `glEGLImageTargetRenderbufferStorageOES` (FBO `GL_FRAMEBUFFER_COMPLETE`) → clear, `glFinish`, and a waited `glFenceSync` — all clean — and `glEGLImageTargetTexture2DOES` reads **the same bytes back on both pixels**. The container then imports the same handle's fd with `gbm_bo_import(GBM_BO_IMPORT_FD_MODIFIER, …)` and **that** is where it breaks: the bytes it maps are not the bytes the GPU wrote, and its own writes do not arrive either. |

The reason is visible in the offer: the AHB's handle carries **two** descriptors,
while the import claims `DRM_FORMAT_MOD_LINEAR` over plane 0 alone. A multi-plane,
possibly compressed allocation imported as one linear plane is not a view of that
buffer, so a CPU mapping of it agrees with nothing — and no userspace API hands the
importer the modifier it should have used, because `AHardwareBuffer` does not expose
one. (`AHardwareBuffer_Desc::stride`, for its part, is in **pixels** while every
consumer on the wire wants bytes; that trap cost a device run and is now a named
constant beside the fourcc.)

### What this decides

The cell is reachable **only** when the image is allocated where the server can use
it — on the Android side — **and** its layout travels with the offer. That is a
requirement on the host that mints the scanout buffers, not a MobileGL feature:
Anland-style hosts that allocate in the container cannot be served by this driver,
and a host that publishes plane count, strides, offsets and modifier alongside the
fd can be. Note also that the container never needs a CPU view of the image in the
product: the compositor hands the buffer to its GL, which is MobileGL, and the
server renders into it. The CPU mapping in this leg is a *verification device* —
which is exactly why `FAIL` here means "a linear view is invalid", not "the driver
cannot render into an AHardwareBuffer".

This leg's topology differs from every other one: the peer is **not** a re-exec
child. It is a glibc process inside the container, because only that side can
allocate on the device Anland hands the container. So the probe **listens**
(`--only-t4[=@endpoint]`, default `@mgl-t4`) and the peer **dials** it. The
endpoint is an abstract socket name because a Droidspaces container runs
`net_mode=host`: the abstract namespace is common to both sides, while a
filesystem pathname would have to exist in the other side's root.

Both rows run on the one connection `--only-t4` opens, so the peer never waits for
a frame by name and a skipped row is not an error.

* **`T4-image-import`** — the container-minted direction. This side has no GBM
  device, so it waits for an fd, asks the driver whether the announced
  `(fourcc, modifier)` pair is one it even lists (`eglQueryDmaBufFormatsEXT` / `eglQueryDmaBufModifiersEXT`; a pair
  the driver does not list is a capability answer, an import failure on a pair it
  *does* list is a driver bug, and the two are never reported the same way),
  imports the buffer as a texture **and** as a renderbuffer, reads two pixels on
  each path — `(0,0)` and the far corner, so a wrong stride or offset cannot
  pass as a working import — and then clears each path to a colour.
* **`T4-ahb-image`** — the Android-minted direction, and the one the measurement
  says the architecture must use. This side allocates an `AHardwareBuffer`
  (`R8G8B8A8_UNORM`, `GPU_SAMPLED_IMAGE|GPU_COLOR_OUTPUT|CPU_READ_OFTEN|CPU_WRITE_OFTEN`),
  imports **the same buffer** into GL as an EGLImage, clears it through the
  renderbuffer path behind a reported fence, hands its handle to the peer over a
  socketpair (`AHardwareBuffer_sendHandleToUnixSocket`), and then reads the peer's
  answer back through the **texture** path at `(0,0)` and at the far corner. One
  decisive leg, `ahb[D]`: it needs the peer to have seen this side's clear colour
  at both pixels *and* this side to have read the peer's pattern at both.
* **`peer/t4_gbm_peer.c`** — serves both rows. For the first it allocates with
  `gbm_bo_create(…, GBM_FORMAT_ARGB8888,
  GBM_BO_USE_RENDERING|GBM_BO_USE_LINEAR)`, fills the whole mapping with
  `--seed=RRGGBBAA` in ARGB8888 memory order (B,G,R,A; a *solid* fill, so a
  vertical flip cannot turn a working import into a reported failure), sends the
  fd over `SCM_RIGHTS`, and after each of the probe's two writes takes a **fresh**
  read mapping and answers with the bytes it actually saw. A driver that refuses
  the allocation has answered the question: the refusal is printed verbatim and
  the peer exits non-zero, with no silent fallback to another format. For the
  second it takes the descriptors off the socketpair, imports the first with
  `gbm_bo_import(GBM_BO_IMPORT_FD_MODIFIER, …)` and reports **how many** arrived —
  the plane count is itself evidence — then compares both pixels against the
  announced colour before writing a pattern of its own.

The peer **must run inside the container**: `/dev/dri/renderD128` there is the
Anland-backed `msm_drm` GBM device, and the Android side has no GBM device at all.

```sh
S=<serial>; DS="/data/local/Droidspaces/bin/droidspaces --name=Fedora"

# peer: into the container's own rootfs (adb push lands on the Android side),
# then built there with the container's own glibc gcc -- the documented line is
#   gcc -O2 -o t4_gbm_peer t4_gbm_peer.c -lgbm
base64 -w0 tools/spikes/extmem_probe/peer/t4_gbm_peer.c > /tmp/t4_gbm_peer.c.b64
adb -s $S shell "$DS run sh -c 'base64 -d > /root/t4_gbm_peer.c'" < /tmp/t4_gbm_peer.c.b64
adb -s $S shell "$DS run sh -c 'cd /root && gcc -O2 -o t4_gbm_peer t4_gbm_peer.c -lgbm'"

# probe: build and start it FIRST — it is the side that listens
ANDROID_NDK=$HOME/android-sdk/ndk/27.3.13750724 ./build_android.sh /tmp/extmem-build
adb -s $S push /tmp/extmem-build/extmem_probe /data/local/tmp/extmem_probe
adb -s $S shell "chmod 755 /data/local/tmp/extmem_probe && /data/local/tmp/extmem_probe --only-t4" \
  | tee out-t4-$S.txt

# then, with the probe listening, dial it from the container
adb -s $S shell "$DS run /root/t4_gbm_peer --endpoint=@mgl-t4"
```

Peer options: `--endpoint=@mgl-t4`, `--width=64`, `--height=64`,
`--seed=RRGGBBAA`, `--timeout=60`. The row is `T4-image-import`, with two
decisive legs `tex[D]` and `rb[D]` under the usual rule: `OK` needs each path to
have read the peer's seed bytes **and** to have had its own write observed by the
peer. The peer exits 0 only on that same two-way condition, and its last line
carries every raw value (stride, format, modifier, the fd's kernel name, the
exporter, both pixels of both paths) — read those, not the exit code.

Two things there will look like failures and are not. `gbm_bo_unmap()` of a
**WRITE** mapping segfaults on this container's mixed-version Mesa (distro
`libgbm`/`dri_gbm` 26.1.5 plus an unowned self-built
`libgallium-26.2.0-devel.so`), so the peer deliberately leaves its fill mapping
mapped instead. And the peer's fd name plus exporter (`/dmabuf:`,
`exp_name: system`) is the evidence that this is the same *kind* of object the
compositor scans out: kwin's own scanout allocations come from that same system
dma-heap.

## Route mode (P11 B2 step 1)

Does a client's AHB cross the *real* B1 routes into the server app? A spike APK built with
`-Pmobilegl.buildExtmemProbe=ON -Pmobilegl.extmemProbeAsServer=ON` packages the probe as
`libMobileGLServer.so` (the real server becomes `libMobileGLServerReal.so`), so
`MobileGLServerService` execs `libMobileGLServer.so <endpoint> --serve` = the probe's route server
in the server app's own domain, and B1's broker connects to it and writes its PairBind pair.

* `--serve` (argv[2]): listen on `<endpoint>`; pair each two connections by their PairBind frame
  length (control 64 bytes, aux 68); per client variant receive the handle, import it into
  Vulkan and GL, and run the T0S rounds on the control connection. Rows `ROUTE-<variant>-<size>`.
* `--route-client[=@endpoint]`: from `MOBILEGL_IPC_CONTROL=fd:<c>,<a>` (the B1 helper; presents
  its own PairBind pair unless `MOBILEGL_IPC_FD_PAIRED=1`) or by connecting itself (same app only).
  Two variants per run: `direct` (`AHardwareBuffer_sendHandleToUnixSocket` on the aux connection)
  and `hop` (sent on a fresh socketpair whose other end travels over aux by `SCM_RIGHTS`).
  `--size`, `--sustained-lock=N` (default 4), `--regions-at-end` as above.

Never ship an APK built with `extmemProbeAsServer`: it has no render server.
