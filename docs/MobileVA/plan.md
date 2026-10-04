# MobileVA: a vendor-agnostic VA-API driver on Android MediaCodec (plan, 2026-10-04)

> Status: **plan only. No code, no build and no device work has been done for it.** Facts in this
> note are tagged **[verified]** (observed on the device, or read from code in this repo, with the
> source named) or **[unverified]** (expected from documentation or memory of the client sources;
> each one has a check in the M1 spikes). Anything that says "will", "proposed" or "TODO" is design.
>
> Context: [`../Disaggregated/notes/anland/plan-ahb-dmabuf.md`](../Disaggregated/notes/anland/plan-ahb-dmabuf.md)
> (shared images), the YUV import work in MobileGL `7218bacb` / `18a5dba0`, and the skill
> `.claude/skills/anland-mobilegl-plasma/` (build, deploy, probes).

## 1. Goal and why

Hardware video decode in the anland KDE Plasma container comes from `/usr/lib/dri/msm_drm_drv_video.so`
today. That driver:

- is an unpackaged third-party binary ("DroidSpaces V4L2 VA-API driver 0.4.7", copied in on
  2026-09-21). It comes from no pacman package and from nothing in the anland repo. **[verified]**
- is **Qualcomm-only**. It drives the kernel's V4L2 stateful decoder at a hardcoded `/dev/video32` with
  Qualcomm-private modes and UBWC formats (STREAM_OUTPUT_MODE=SECONDARY, Q10C/TP10/Q12A). **[verified]**
- **CPU-copies** every decoded frame into NV12/P010 dma-bufs it allocated itself (`nv12_copy`).
  **[verified]**
- loads only because the render node's DRM driver name is `msm_drm`. No `LIBVA_DRIVER_NAME` is
  set. **[verified]**

Because neither the phone's EGL (no `EGL_EXT_image_dma_buf_import`) nor its Vulkan (no
`VK_EXT_external_memory_dma_buf`) imports a raw dma-buf, MobileGL then copies those foreign buffers a
**second** time on the CPU, into server YUV AHardwareBuffers (`SharedImages::ImportForeignYuv` /
`RefreshForeign`, 7218bacb). **[verified]** That costs about 31% of one server core at 720p30/60.
**[verified, 2026-10-04]**

So each frame takes two CPU copies, and the path works only on Qualcomm.

**Goal.** A libva driver of our own in the container. It forwards the compressed bitstream to the
MobileGL server process on Android, which decodes it with NDK `AMediaCodec` into AHardwareBuffers
that MobileGL owns. Decoded frames then reach GL with **zero CPU copies on any Android SoC**.

## 2. User decisions (binding)

| Topic | Decision |
|---|---|
| Location | A new top-level directory **`MobileVA/`** beside `MobileGL/`, with its own CMake targets. It is **never linked into the GL library**. It may reuse MobileGL's transport and shared-image registry. |
| Where decode runs | Inside the existing `:mobilegl` server process of the anland APK. |
| Codec priority | H.264, then VP9, then AV1, then HEVC (including 10-bit / P010; VP9 profile 2 and AV1 10-bit where the device supports them). **Only H.264 (milestone 1) is in scope for the first implementation.** |
| Old driver | Stays installed as an A/B fallback. Ours is the default. One documented knob switches back, and Chrome's GPU process must honour it too. |
| Clients | Chrome, mpv and GStreamer now; ffmpeg is the test harness. Firefox comes later. Nothing may block it or any other client: no app-name checks, no assumptions about Chrome's call order, and the VA semantics implemented honestly. |
| Vendor-agnostic | Only public Android NDK/SDK and Linux facilities: AMediaCodec, AImageReader, AHardwareBuffer, sync_file, dma-buf. Capabilities come from what the device's MediaCodec reports, never hardcoded limits. No vendor gralloc handles, no qcom/kgsl, no vendor format constants or vendor MediaFormat keys. |
| Zero copy | No CPU copy on the main path. An exported dma-buf's declared layout (fourcc, offsets, pitches, modifier) must be truthful, so an opaque or compressed layout is never exported as linear NV12. CPU readback (vaDeriveImage / vaGetImage / vaMapBuffer) may copy; that copy is inherent. |
| MobileGL | Core profile only, so no fixed-function test apps. |

## 3. What we already know

### 3.1 How clients drive VA

| # | Fact | Status |
|---|---|---|
| C1 | Chrome 153's VaapiVideoDecoder uses **ALLOCATE mode**. It calls `vaCreateSurfaces(n=1)` once per frame-pool frame (about 22 frames), then `vaExportSurfaceHandle` on each surface once: one fd, `SEPARATE_LAYERS`, R8 + GR88 (e.g. 1280x720, stride 1280, slice 736, uv_offset 942080). It imports each export as a **single NV12 EGLImage** bound to `GL_TEXTURE_EXTERNAL_OES` and caches the import. It never allocates decode buffers through GBM. | **[verified]** (memory `chrome-vaapi-yuv-foreign-copy`, DMD_VA_LOG traces) |
| C2 | Chrome accepts a VA driver whose vendor string is neither Intel nor AMD. The DroidSpaces driver gives `kVideoDecoderName = VaapiVideoDecoder` and `kIsPlatformVideoDecoder = true`. | **[verified]** (skill `probes/cdpmedia.py`) |
| C3 | Chrome's GPU sandbox is off by default in `mobilegl-startup.sh chrome` (`--disable-gpu-sandbox`). With the sandbox on, the GPU process may not call `socket()`/`connect()` after start-up. | **[verified]** (startup script comment, anland 41ec066) |
| C4 | The old driver exports P010 only as `COMPOSED_LAYERS` ("SEPARATE_LAYERS not implemented"). | **[verified]** |
| C5 | MobileGL imports **no single-channel dma-bufs** (R8, GR88). So KWin's own NV12 path, which splits a buffer into R8 + GR88 imports, is not served, and KWin advertises no YUV format over linux-dmabuf. | **[verified]** (skill SKILL.md) |
| C6 | mpv's `--hwdec=vaapi` GL interop imports **each layer separately** as R8 / GR88 EGLImages. With C5, it would fail on MobileGL today. | **[unverified]**; check with LIBVA_TRACE and mpv `-v` |
| C7 | ffmpeg, mpv (it uses ffmpeg's hwaccel), GStreamer `vah264dec` and Chrome pass H.264 slice data as the **whole slice NAL unit without its start code**: it starts at the NAL header byte and keeps the emulation-prevention bytes. | **[unverified]** (memory of the client sources) |
| C8 | Whether Chrome calls `vaSyncSurface` before it samples a surface, or relies on implicit dma-buf sync. ffmpeg syncs inside `vaDeriveImage`/`vaGetImage` downloads. | **[unverified]**; record with `LIBVA_TRACE` |
| C9 | Chrome reads `LIBVA_DRIVER_NAME` through libva. Chrome itself picks a driver name only for some Intel cases. | **[unverified]** |
| C10 | GStreamer's `va` plugin registers elements only for drivers on its allowlist unless `GST_VA_ALL_DRIVERS=1` is set. | **[unverified]** |
| C11 | `LIBVA_TRACE=/path` in libva logs every call with its parameters, for **either** driver. That makes it the A/B observation tool. | **[unverified]** for this libva build |

### 3.2 Platform and repo facts

| # | Fact | Status |
|---|---|---|
| P1 | Device: Lenovo TB321FU (Y700), Adreno 750, Android 15, kernel 6.1. Container: Arch Linux ARM, glibc. libva headers in the cross sysroot are **VA-API 1.24** (`~/sysroots/arch-kde-mgl/usr/include/va`). | **[verified]** |
| P2 | Server YUV shared images are AHBs: `Y8Cb8Cr8_420` / `YCbCr_P010`, usage `GPU_SAMPLED_IMAGE \| CPU_WRITE_OFTEN \| CPU_READ_RARELY`. Plane offsets come from `AHardwareBuffer_lockPlanes`. They are exported with modifier `DRM_FORMAT_MOD_INVALID`. A returning fd is identified by `(st_dev, st_ino)`, so a re-import of our own buffer is **zero-copy** (no foreign path). | **[verified]** (`MG_Remote/Server/SharedImageRegistry.cpp`, yuvprobe GBM NV12 case) |
| P3 | Espryt converts YUV to RGBA with `GL_EXT_YUV_target` (Adreno 750 has it). Magma converts with `VkSamplerYcbcrConversion` on the AHB external format. Both convert once per reader frame epoch. | **[verified]** (`DirectGLES.cpp ConvertYuvForSampling`, `WireYuvImage.inc`) |
| P4 | The foreign-copy fallback is labelled. Its first use logs "a foreign ... dma-buf is sampled through image N, refreshed by a CPU copy", and `Image::ForeignSource::Copies` counts each copy. That is the proof hook for "our buffers never take the fallback". | **[verified]** (code) |
| P5 | The YUV conversion paths call `RefreshForeign` but, from a read of the code, do **not** wait on the image's write fence (`DupWriteFence`) the way the RGBA shared-image path does. | **[unverified]**: code read only; confirm before relying on write fences for YUV |
| P6 | The client shared-image C ABI (`MG_Impl/SharedImageApi.h`) makes its round trips on the **process's GL client session**, the same ring its GL calls ride, so "like any GL call they must not race the GL calls of another thread". A VA driver therefore **cannot** use it from a decode thread. It needs its own connection. | **[verified]** (header comment) |
| P7 | The server listener pairs a client's two connections by a `PairBind` first frame (`MG_Remote/Server/PairAcceptor.h`) and refuses any other first frame by name. | **[verified]** (code) |
| P8 | A hung shader stalls every GPU user on the device, and the server process is shared by every desktop client. Process-global state in the server is a cross-session hazard (memory `multi-session-server-cross-session-hazards`). | **[verified]** |
| P9 | NDK `AMediaCodecStore` / `AMediaCodecInfo` (the codec list) need API 36. On Android 15 (API 35) the codec list comes from Java `MediaCodecList` through JNI. | **[unverified]**: from NDK headers; check `__ANDROID_API__` guards |
| P10 | `JNI_GetCreatedJavaVMs` is callable from NDK code on API 31+. That lets a lazily loaded .so reach the app's JavaVM. | **[unverified]** |
| P11 | Codec2 decoders give each output buffer the `presentationTimeUs` of the input that produced it. | **[unverified]**; spike S2 |
| P12 | `AImage_deleteAsync(image, releaseFence)` (API 26) returns a codec output buffer once a GPU read finishes, with no CPU wait. | **[unverified]** |

## 4. Architecture

```
container (glibc)                                     Android :mobilegl process (bionic)
───────────────────────────────────────────           ──────────────────────────────────────────────────────
client (Chrome GPU proc / mpv / gst / ffmpeg)
  └─ libva ─ /usr/lib/dri/mobileva_drv_video.so        libMobileGL.so (server)        libMobileVAService.so
       • VA objects (config/context/surface/buffer)      listener @anland-mobilegl ──▶ ServiceBind "mobileva"
       • H.264 header rebuild → Annex-B access unit       SharedImageRegistry  ◀───────  C ABI: allocate decode-
       • own socket connection + reader thread           (AHB images, fences)           target YUV images, fences
       │                                                                                • per-connection session
       └──── unix:@anland-mobilegl (own connection, ───────────────────────────────▶    • AMediaCodec (async) →
             MobileVA protocol, fds via SCM_RIGHTS)                                       AImageReader (PRIVATE)
                                                                                        • copy thread: own EGL ctx,
  GL consumer (Chrome/mpv via MobileGL EGL) ── imports exported fd ──▶ registry           GL_EXT_YUV_target Y2Y copy
                                               Identify(inode) → same AHB, zero-copy     codec AHB → surface AHB
```

### 4.1 Directory and targets

```
MobileVA/
  CMakeLists.txt     option MOBILEVA_BUILD_DRIVER (Linux), MOBILEVA_BUILD_SERVICE (Android), MOBILEVA_BUILD_TESTS (host)
  Common/            protocol (POD, versioned, length-prefixed; SCM_RIGHTS helpers), BitReader/BitWriter,
                     exp-Golomb, emulation-prevention insert/strip, capability types
  Codec/H264/        slice-header pre-parse, SPS/PPS synthesis from VA params, access-unit assembly
                     (pure C++, no libva/Android deps beyond va.h structs; host-testable)
  Driver/            mobileva_drv_video.so: __vaDriverInit_1_<minor>, the VA vtable, objects, connection
  Service/           libMobileVAService.so: session, MediaCodec decoder, AImageReader, GPU copy, caps (JNI)
  Test/              gtest host tests
```

- The top-level `CMakeLists.txt` adds `MobileVA/` behind an option. No MobileGL target links any
  MobileVA target.
- The driver has no link dependency on libva (drivers implement `VADriverVTable`) and none on
  libMobileGL. It reads the endpoint from `/etc/mobilegl/client.conf` (`MOBILEGL_IPC_CONTROL`) the way
  the GL client does, overridable by `MOBILEVA_ENDPOINT`.
- The service links `mediandk`, `nativewindow`, `EGL`, `GLESv3`, `android`, `log`. It reaches the
  registry through a narrow C ABI that libMobileGL.so (server build) exports (§4.3).

### 4.2 Where the H.264 rebuild runs: in the client

The driver rebuilds the headers and assembles a complete Annex-B access unit **in the client
process**. The server receives a codec-generic request: `Decode{context, target surface, seq, config
bytes if changed, AU bytes, flags}`.

**Why:**
- A parser bug then fails one client's decode, not the shared server process that every desktop client
  depends on (P8).
- The rebuild is pure and host-testable.
- The wire protocol stays codec-agnostic for VP9/AV1/HEVC.

All parsing uses bounds-checked readers and returns a VA error; it never asserts.

### 4.3 Transport, sessions and the server hooks

- **Connection.** One connection per `VADriverContext` (one per `vaInitialize`), opened at
  `vaInitialize`. Chrome opens its VA display before any sandbox (C3). The first frame is a new
  `CtrlMsg` **`ServiceBind{name:"mobileva", version}`**, appended to `protocol.fbs`'s union. The
  existing `PairAcceptor` hands such a connection to a registered service by name, and refuses
  unknown names by name.
  - This is the only MobileGL transport change. Everything after the first frame is MobileVA's own
    protocol, defined in `MobileVA/Common`.
  - Fallback if the acceptor change turns out invasive: the service listens on its own abstract socket
    `@anland-mobilegl-va` in the same process. It still uses the same registry.
- **Protocol (draft).** Length-prefixed frames `{magic, version, type, length, requestId}`; fds ride
  `SCM_RIGHTS` on the same socket.

  | Request | Answer |
  |---|---|
  | `Hello{version, pid}` | `Welcome{version, caps[]}` |
  | `CreateContext{codec, profile, w, h}` | `{ctx}` or error |
  | `CreateSurfaces{w, h, fourcc, n}` | per surface `{id, fd, offsets, pitches, modifier, allocation size}` |
  | `ImportSurface{fd, layout}` | `{id}`, only for buffers the registry identifies as its own; anything else gets `VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE` |
  | `DestroySurfaces` / `DestroyContext` | |
  | `Decode{...}` | no answer on success; errors come back as events |
  | `Sync{surface}` | answered when the surface's latest decode is complete, or on timeout or error |
  | `QueryStatus{surface}` | answered at once |
  | `Readback{surface}` | the CPU fallback for `vaGetImage` (§6.4) |
  | server events | `SurfaceDone{id, gen, status}`, `ContextError{ctx, reason}` |

  A reader thread in the driver dispatches answers by request id, so a blocking `vaSyncSurface` on one
  thread never stalls `vaEndPicture` on another. No assumption is made about which threads a client
  uses.
- **Handles are per connection.** Surface, context and buffer ids live in maps owned by the
  connection, never in globals (P8). Each connection owns a `SharedImages::SessionHolder` for the
  images it allocated. A GL session that imports an exported fd takes its own `ImageRef`, so images
  outlive the VA connection while they are still sampled.
- **Server C ABI** (new, in `MG_Remote/Server`, exported from the server libMobileGL.so):
  - `mobilegl_server_register_service(name, version, accept_fn, user)`
  - `mobilegl_server_holder_create/destroy`
  - `mobilegl_server_image_allocate(holder, w, h, fourcc, flags, out{id, AHardwareBuffer*, fd, layout})`
    with a flag `DECODE_TARGET` that adds `GPU_COLOR_OUTPUT` to the YUV usage
  - `mobilegl_server_image_release(holder, id)`
  - `mobilegl_server_image_begin_write(id)` / `publish_write(id, sync_file)` / `dup_read_fences(id, fds, cap)`
- **Loading.** The server `dlopen`s `libMobileVAService.so` from its own directory (`dladdr`) on the
  first `ServiceBind{"mobileva"}`. If the library is missing, it refuses cleanly. The service reaches
  the JavaVM with `JNI_GetCreatedJavaVMs` (P10) for the codec list. No Java change is needed in the
  APK; anland only packs the extra `.so` into jniLibs.
- **Failure isolation.**
  - The driver sends with `MSG_NOSIGNAL` and treats `EPIPE`/EOF as "server gone": every pending and
    future call returns `VA_STATUS_ERROR_OPERATION_FAILED` (Sync: `..._DECODING_ERROR`). It never
    aborts.
  - Objects created after a loss reconnect lazily, so a long-lived VADisplay (Chrome keeps one per
    GPU process) can recover.
  - Server side, a malformed request ends that connection only.
- **The service never touches backend state.**
  - It has its own EGL context on its own thread, never in the backend's share group.
  - It **never calls `eglTerminate`** on the default display, which Espryt shares in-process.
  - Its only interface with Espryt/Magma is the registry's sync_file fences.

## 5. Capabilities

At service load, enumerate `MediaCodecList(REGULAR_CODECS)` over JNI. For each **decoder** that is
`isHardwareAccelerated()` (API 29) and not an alias, and for each supported type (`video/avc`, later
`video/x-vnd.on2.vp9`, `video/av01`, `video/hevc`), record:

- the codec name
- `profileLevels`
- `VideoCapabilities` width/height ranges and alignment
- `getMaxSupportedInstances()`
- the `FEATURE_LowLatency` (API 30) and `FEATURE_AdaptivePlayback` features

Log the table once.

Translation to VA, with host tests:

| MediaCodec AVC profile | VA profile |
|---|---|
| `AVCProfileConstrainedBaseline` or `AVCProfileBaseline` | `VAProfileH264ConstrainedBaseline` |
| `AVCProfileMain` | `VAProfileH264Main` |
| `AVCProfileHigh` | `VAProfileH264High` |
| `AVCProfileHigh10` | `VAProfileH264High10`, only once P010 output works |

- Entrypoint: `VAEntrypointVLD` only.
- `VAConfigAttribRTFormat`: `YUV420` (`YUV420_10` with a 10-bit profile).
- `VAConfigAttribMaxPictureWidth/Height` and `vaQuerySurfaceAttributes` `MaxWidth/MaxHeight`: the
  codec's upper bounds.
- Surface pixel formats: NV12 (P010 later).
- Memory types: `VA` and `DRM_PRIME_2`.
- If no hardware decoder exists for a type, its profiles are simply not listed.
- If `vaCreateContext` exceeds `getMaxSupportedInstances`, or `AMediaCodec_configure` fails, it returns
  `VA_STATUS_ERROR_ALLOCATION_FAILED`, so the client falls back to software for that stream.

## 6. The four hard problems

### 6.1 Parsed parameters vs elementary stream (H.264)

VA gives `VAPictureParameterBufferH264`, `VAIQMatrixBufferH264`, and per slice
`VASliceParameterBufferH264` plus the slice data. That slice data is the whole slice NAL (C7, to
confirm). MediaCodec wants Annex-B access units with SPS/PPS. The client never hands VA the original
SPS/PPS, so we **synthesize** them.

1. **Slice NALs go in verbatim** with a `00 00 00 01` start code; a start code already present is not
   duplicated. Slices with `VA_SLICE_DATA_FLAG_BEGIN/MIDDLE/END` are concatenated. One `vaEndPicture`
   produces one access unit.
2. **Pre-parse each slice header** with a bounds-checked RBSP reader over a de-escaped copy of the
   first ~64 bytes. The parse is driven by fields we know from the VA picture parameters:
   - `first_mb_in_slice`, `slice_type`, `pic_parameter_set_id`, `frame_num` (u(v),
     log2_max_frame_num), `field_pic_flag`/`bottom_field_flag`, `idr_pic_id`,
     `pic_order_cnt_lsb`/`delta_pic_order_cnt_bottom`, `delta_pic_order_cnt[0..1]`,
     `redundant_pic_cnt`, `direct_spatial_mv_pred_flag`, `num_ref_idx_active_override_flag`.
   - We need **`pic_parameter_set_id`** to give our PPS the same id the slices name.
   - We need **`num_ref_idx_active_override_flag`**: VA gives only the *effective*
     `num_ref_idx_l0/l1_active_minus1` per slice, and the PPS defaults
     (`num_ref_idx_l{0,1}_default_active_minus1`) are **not** in VA. PPS default = the effective value
     of a slice with override = 0; any value if every slice overrides.
3. **SPS** (id 0):
   - `profile_idc` and constraint flags come from the config's VAProfile: CB = 66 with
     constraint_set0+1, Main = 77, High = 100, High10 = 110.
   - For profile ≥ 100: `chroma_format_idc` and `bit_depth_*_minus8` from VA,
     `qpprime_y_zero_transform_bypass_flag` = 0, `seq_scaling_matrix_present_flag` = 0 (scaling lists
     go in the PPS, see 4).
   - From VA: `log2_max_frame_num_minus4`, `pic_order_cnt_type`, `log2_max_pic_order_cnt_lsb_minus4`,
     `delta_pic_order_always_zero_flag`, `max_num_ref_frames = num_ref_frames`,
     `gaps_in_frame_num_value_allowed_flag`, `pic_width_in_mbs_minus1`, `frame_mbs_only_flag`,
     `mb_adaptive_frame_field_flag`, `direct_8x8_inference_flag`.
   - **Gotcha:** VA's `picture_height_in_mbs_minus1` is the *frame* height in MBs, so
     `pic_height_in_map_units_minus1 = ((h+1) >> !frame_mbs_only) - 1`.
   - `frame_cropping_flag` = 0. VA surfaces are coded-size; the client crops.
   - `level_idc` is **not in VA**. Pick the smallest level in Table A-1 whose MaxFS ≥ frame MBs,
     MaxDpbMbs ≥ MBs × max(num_ref_frames, 1) and MaxMBPS ≥ MBs × 60, capped at the codec's maximum
     level. This is a heuristic (open question Q3).
   - **VUI** carries only `bitstream_restriction_flag` = 1 with `max_num_reorder_frames` = 0,
     `max_dec_frame_buffering` = max(num_ref_frames, 1), motion-vector limits at their maximum and no
     byte or bit limits. This is deliberate; see §6.2. Colour description is irrelevant to the decoded
     samples, so it is not emitted.
   - **POC type 1** needs `offset_for_non_ref_pic`, `offset_for_top_to_bottom_field` and the
     `offset_for_ref_frame[]` cycle, and **none of them are in VA**. VA passes resolved POCs instead.
     POC drives temporal direct and implicit weights, so it changes pixels. M1 **refuses** POC type 1
     at `vaEndPicture` (`VA_STATUS_ERROR_DECODING_ERROR`, logged once); see TODO H264-2.
4. **PPS** (id from the slice):
   - `seq_parameter_set_id` = 0.
   - From VA: `entropy_coding_mode_flag`, `bottom_field_pic_order_in_frame_present_flag` (VA
     `pic_order_present_flag`), `weighted_pred_flag`, `weighted_bipred_idc`, `pic_init_qp_minus26`,
     `pic_init_qs_minus26`, `chroma_qp_index_offset`, `deblocking_filter_control_present_flag`,
     `constrained_intra_pred_flag`, `redundant_pic_cnt_present_flag`.
   - `num_slice_groups_minus1` = 0. FMO is refused.
   - Then, when any of them is not the default: `transform_8x8_mode_flag`, the scaling lists and
     `second_chroma_qp_index_offset`.
   - **Scaling lists:** when the IQ matrix is not all-flat-16, set `pic_scaling_matrix_present_flag`
     = 1 and write all 6 (+2 with 8x8) lists **explicitly**. VA gives effective lists, so the
     fall-back rules never matter.
   - **Gotcha (Q4):** whether `VAIQMatrixBufferH264` lists are in zigzag or raster order differs in our
     memory of the client sources. Verify with an x264 `--cqm jvt` stream and the A/B driver before
     trusting either.
5. **Emit** SPS/PPS before the access unit only when their bytes differ from the last sent with the
   same id, and again after every (re)configure or flush.
6. **Configure** MediaCodec lazily at the first `vaEndPicture`, when SPS/PPS exist: `csd-0` = SPS,
   `csd-1` = PPS, plus width/height. Re-configure, after draining the old codec, when the coded size,
   profile or bit depth changes (§6.2).
7. **M1 refuses** (logged once, `VA_STATUS_ERROR_UNSUPPORTED_PROFILE` or `_DECODING_ERROR`):
   - field pictures (`field_pic_flag` = 1; MBAFF frames are fine)
   - FMO/ASO
   - chroma formats other than 4:2:0
   - POC type 1

### 6.2 Output order and surface mapping

- **Tagging.** Every queued input carries `presentationTimeUs = seq`, a per-context monotonic decode
  counter. The context keeps `seq → {surface id, surface generation}`. An output buffer's
  `presentationTimeUs` names its surface (P11, to confirm). MediaCodec's own output **order is
  irrelevant**: we never present in its order. Clients that reorder (Chrome, ffmpeg) display in their
  own order.
- **Release.** Release every output with `render=true` into the AImageReader. Match the acquired
  `AImage` to the released buffer FIFO, asserting `AImage_getTimestamp == pts × 1000`.
- **Latency is the deadlock risk.** ffmpeg (framemd5) downloads a frame, which calls `vaSyncSurface`
  on its only thread, after its own reorder delay. If MediaCodec holds that frame waiting for more
  input, nothing more comes: deadlock. Mitigations, in order:
  1. The VUI rewrite (`max_num_reorder_frames = 0`). A conforming decoder then bumps every picture as
     soon as it is decoded, so decode-order output gives the lowest latency. This does not change
     samples: POC values in slice headers are untouched.
  2. `KEY_LOW_LATENCY = 1` where `FEATURE_LowLatency` is listed, plus `KEY_PRIORITY = 0` and a high
     `KEY_OPERATING_RATE`.
  3. **Spike S2 must measure input→output lag** on the device (H.264 with B-pyramids). If any
     hardware decoder still holds frames, document it and bound `vaSyncSurface` by a timeout
     (`VA_STATUS_ERROR_TIMEDOUT`, default 2 s; `vaSyncSurface2` gets the client's own timeout). Never
     wait forever.
- **Surface state.** Each surface has `pendingGen`, which `Decode` increments, and `doneGen`, set when
  its copy fence is published. `vaSyncSurface` waits for `doneGen ≥ pendingGen` *as of the call*. It
  answers only after the copy's fence has **signalled**, because after `vaSyncSurface` a client may
  `mmap` the exported dma-buf. `vaQuerySurfaceStatus` returns `VASurfaceRendering` or
  `VASurfaceReady` without blocking.
- **Readers that never sync** (C8). At `Decode` the service calls `begin_write(image)`, which opens a
  pending write generation with no fence yet. At copy completion it calls `publish_write(image,
  fence)`.
  - **TODO (P5):** both backends' YUV sampling must honour the write fence and, for a pending write
    without a fence, CPU-wait on the registry with a bound (e.g. 100 ms, logged once). That makes the
    GL path correct whether or not a client syncs.
  - It does not deadlock: the decode input comes from the client's VA connection (another server
    thread), not from the GL session that waits.
- **Lost outputs.** If an output with `seq = N` arrives while older entries are still pending, the
  decoder dropped them (decode-order output is expected). Complete them with
  `VA_STATUS_ERROR_DECODING_ERROR` (`vaQuerySurfaceError`), log, and never leave a waiter hanging.
- **Seek / flush.** VA has no flush; clients restart at an IDR. Feed it as is; MediaCodec's references
  reset at the IDR. Late outputs of pre-seek frames still land in their own surface generation. A
  surface re-targeted since then has a newer generation, so the stale output is **dropped, not
  copied**. The generation check matters because "last write wins" is not guaranteed.
- **Resolution change.**
  - A new coded size inside the same context drains the old codec: queue EOS, wait for all outputs,
    copy them. Then configure a new one. The change happens at an IDR, so no references are lost.
  - Chrome and ffmpeg usually recreate the VA context instead, which simply creates a new codec.
- **Destroy mid-stream.** `vaDestroyContext` stops and deletes the codec, drops pending outputs, and
  completes all waiters with an error. `vaDestroySurfaces` on a surface with a pending decode marks the
  entry dead; its output is discarded.
- **Hidden frames** do not exist in H.264. VP9/AV1 notes are in §9.

### 6.3 Buffer ownership: zero-copy and truthful export

**Constraint.** VA surfaces are created up front and exported once, and clients cache the import (C1).
MediaCodec decodes into buffers that the AImageReader BufferQueue allocates and picks per frame. No
public API lets us hand MediaCodec our own buffers.

| Option | Verdict |
|---|---|
| **A. Aliasing**: re-point a surface's shared image at the codec's output AHB | **Rejected.** It is correct only for importers that go through MobileGL by inode. A client that `mmap`s the exported fd (GStreamer downstream of DMABuf caps, ffmpeg `hwmap`, a future direct KWin/Firefox import) would read the surface's own, stale memory. The codec's buffer is typically opaque or compressed (UBWC-like), so it can never be exported as linear NV12 either. |
| **B. Server GPU copy** into the surface's own linear NV12 AHB | **Chosen.** Correct for every consumer, keeps the export truthful, and costs one small GPU pass (1080p NV12 ≈ 3 MB read + 3 MB written; 4K60 ≈ 1.5 GB/s, small against the GPU's bandwidth). Measure it in M1. |

**Copy path (B):**
- The AImageReader uses `AIMAGE_FORMAT_PRIVATE` with usage `GPU_SAMPLED_IMAGE`, so the codec keeps its
  preferred (possibly compressed) layout, and `maxImages` ≈ 6.
- The copy thread owns an EGL context (GLES 3) and imports the codec AHB (`AImage_getHardwareBuffer`
  → `eglGetNativeClientBufferANDROID` → `EGL_NATIVE_BUFFER_ANDROID` → `GL_TEXTURE_EXTERNAL_OES`). It
  imports the surface AHB the same way, as an FBO attachment.
- It draws with **`GL_EXT_YUV_target`**: a `__samplerExternal2DY2YEXT` source written to a
  `layout(yuv) out` target, which is a raw YUV→YUV copy with no colour conversion. NEAREST sampling
  gives every 2x2 quad the same chroma sample, so the target's chroma subsampling writes it back
  exactly. 8-bit and 10-bit values survive the float round trip. **Bit-exact by construction**, to be
  proven by spike S3.
- Copy region = min(surface, codec crop rect); the rest of the surface is untouched.
- **Write after read.** Before overwriting a surface, take `dup_read_fences` and make the GL context
  wait on them (`EGL_ANDROID_native_fence_sync` import + `eglWaitSyncKHR`).
- **Write publish.** Create an EGL native fence, `publish_write(image, fence)`, and
  `AImage_deleteAsync(image, dup(fence))` so the codec buffer returns as soon as the GPU has read it.
  No CPU wait on the main path. The only CPU wait is the one `vaSyncSurface` does for its answer.
- **Decode-target images** are the registry's YUV images plus `GPU_COLOR_OUTPUT` (new allocation flag).
  Check `AHardwareBuffer_isSupported` with exactly that usage. CPU usage stays on, which keeps
  allocators off compressed layouts and makes `lockPlanes` meaningful.
- **Truthful export.** Once per format at service start, a **layout probe** checks that the CPU view
  equals the raw dma-buf view: write a pattern through `lockPlanes` and read it back through `mmap` of
  the exported fd at the claimed offsets (spike S3).
  - Pass: surfaces are exported as `DRM_FORMAT_NV12`, two layers or one composed object, with the
    probed offsets and pitches and modifier **`DRM_FORMAT_MOD_LINEAR`**. MobileGL's importer already
    accepts LINEAR or INVALID.
  - Fail: export with `DRM_FORMAT_MOD_INVALID`, and route `vaDeriveImage` to "unsupported" so clients
    use `vaGetImage`, which the server serves by `Readback`.
- **Export flags.** `vaExportSurfaceHandle` honours `SEPARATE_LAYERS` (R8 + GR88 / R16 + GR1616) and
  `COMPOSED_LAYERS` (NV12 / P010) for every client. It returns a `dup` of the surface's fd each call.
- **Fallbacks when there is no `GL_EXT_YUV_target`** (non-Adreno devices; availability unverified):
  1. A Vulkan pass: read with `VkSamplerYcbcrConversion` in `RGB_IDENTITY` / full range / NEAREST, and
     write through per-plane views of the surface AHB, if it maps to `G8_B8R8_2PLANE_420_UNORM` with
     the needed usage.
  2. A **labelled, counted CPU copy** from a `YUV_420_888` + `CPU_READ_OFTEN` AImageReader. Never
     silent; logged once with the reason. Chosen at service start and reported in the caps log.
- **Proof of "no CPU copy on the GL path."**
  - The server log must have no "foreign ... refreshed by a CPU copy" line (P4) during playback.
  - MobileVA's stats line (`MOBILEVA_STATS=1`, every 5 s per context) shows `decoded`, `gpu_copies`,
    `cpu_copies = 0` and `readbacks = 0`.

### 6.4 CPU access (vaDeriveImage / vaGetImage / vaMapBuffer)

- `vaDeriveImage` (only with a passing layout probe) returns an NV12 `VAImage` whose buffer maps the
  surface's dma-buf in the client (`mmap`, `DMA_BUF_IOCTL_SYNC` START/END around map/unmap). It
  implies a `vaSyncSurface`.
- `vaGetImage` copies from that mapping, or from `Readback` when no mapping is allowed.
- Parameter and slice buffers are client memory only. `vaMapBuffer` on them is plain memory.
- `vaPutImage` / `vaPutSurface` / VPP / encode: `VA_STATUS_ERROR_UNIMPLEMENTED`.

## 7. A/B knob, install, environment

- **Install.**
  - `xdeploy.sh` installs `/usr/lib/dri/mobileva_drv_video.so` by rename-into-place, beside the old
    `msm_drm_drv_video.so`, which stays.
  - The APK packs `libMobileVAService.so` next to `libMobileGL.so`.
  - The driver exports the init symbol for the sysroot's libva (`__vaDriverInit_1_24`, built from
    `VA_DRIVER_INIT_FUNC`). libva also tries lower minors.
- **Default and knob.**
  - `/etc/environment.d/10-mobilegl.conf` and `/etc/profile.d/mobilegl.sh` set
    `LIBVA_DRIVER_NAME=mobileva` and `GST_VA_ALL_DRIVERS=1` (C10).
  - The one documented knob is `MOBILEGL_VA_DRIVER`, which `mobilegl-startup.sh` and a helper
    `mobilegl-va-driver {mobileva|msm_drm}` (it rewrites both files) turn into `LIBVA_DRIVER_NAME`.
    `msm_drm` is the old driver. A per-process `LIBVA_DRIVER_NAME=msm_drm mpv ...` works too.
  - `mobilegl-startup.sh chrome` exports the chosen name explicitly, so the GPU process (which
    inherits the browser's environment) uses it (C9).
  - `MOBILEGL_CHROME_VIDEO_DECODE=software` keeps its meaning.
- **Proof of which driver a process uses:** `grep _drv_video /proc/<pid>/maps`; the driver's
  `vaQueryVendorString` ("MobileVA (MediaCodec <codec name>)"); `MOBILEVA_TRACE=1` logs every VA call.
- **Render node.** The driver never `ioctl`s the DRM fd libva hands it, and accepts DRM, Wayland and
  X11 display types (Chrome uses DRM; mpv may use Wayland).

## 8. Verification plan (per codec, on the device, both backends)

**Bit-exactness.**
- Generate streams in the container with ffmpeg `libx264`:
  - Baseline / Main / High
  - CAVLC (`-coder 0`), weighted prediction (`-weightp 2`), B-pyramid with `-bf 3 -refs 4`
  - multiple slices (`-slices 4`), custom CQM (`-x264opts cqm=jvt`)
  - odd sizes (1278x718), long GOP (`-g 600`)
  - a mid-stream resolution change: two encodes concatenated into `.ts` with `-c copy`
- For each: `ffmpeg -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 -i X -pix_fmt nv12 -f framemd5 hw`
  against `ffmpeg -i X -pix_fmt nv12 -f framemd5 sw`.
- Expect identical hashes. Report every mismatch. Repeat with `LIBVA_DRIVER_NAME=msm_drm` (A/B).

**vainfo.** `vainfo --display drm --device /dev/dri/renderD128` profiles and entrypoints equal the
service's logged MediaCodec caps table, nothing more.

**mpv.**
- `--hwdec=vaapi-copy` correct: render to a raw file (`--o`) and framemd5 it against software.
- `--hwdec=vaapi` (GL interop on MobileGL) plays with correct colours. Prerequisite: TODO R3,
  per-plane R8/GR88 import (C5/C6).

**GStreamer.** `filesrc ! qtdemux ! h264parse ! vah264dec ! videoconvert ! checksumsink` equals the
same pipeline with `avdec_h264`.

**Chrome.**
- `cdpmedia.py` shows `VaapiVideoDecoder` / `kIsPlatformVideoDecoder = true`, and
  `/proc/<gpu>/maps` shows `mobileva_drv_video.so`. YouTube is forced to AVC by a `CDP_EXPR` override
  of `MediaSource.isTypeSupported`; also local files.
- Colours exact: a pattern clip with known BT.709 limited-range patches, pixel-checked in screenshots.
- No flicker: two screenshots plus a screen recording.
- No GPU-process restarts: no `exit_code` in Chrome's log.
- Seeking works: repeated `video.currentTime = t`.
- `getVideoPlaybackQuality()` dropped/total frames at 720p60, 1080p60 and 4K60 (local H.264 files;
  YouTube AVC only goes to 1080p).

**Cost.**
- Server-process CPU (`/proc/<pid>/stat` over 30 s, skill `device/measure.sh`) and the Chrome GPU
  process's CPU, for ours vs `msm_drm` on the same content.
- No foreign-copy log line and `cpu_copies = 0` (§6.3).
- GPU copy time per frame (`GL_EXT_disjoint_timer_query` if present, else fence-to-fence).

**Desktop.** Still fine on both backends: `desktop-verify.sh`, scrolling, resize, lock/unlock
(`kscreenlocker_greet --testing`), panels.

**Host tests (WSL archlinux, clang, `ctest -E DirectVulkan`, never two suites at once).**
- Bit writer/reader, exp-Golomb, emulation prevention.
- Slice-header pre-parse on real x264 slices.
- SPS/PPS synthesis: golden bytes for fixed VA inputs. Also a round trip: parse a real stream's
  SPS/PPS into VA structs the way a client would, rebuild, re-parse, and compare every semantic field
  except VUI.
- Surface/order mapping state machine: reorder, drops, generation-stale outputs, timeouts, destroy
  mid-stream.
- Protocol encode/decode round trips and malformed-frame rejection.
- Capability translation.

Device rules: battery ≥ 40%, USB serial HA27Q3LQ, leave the device on the merged build, on Magma, with
Plasma up.

## 9. Milestone TODO list

### M1: H.264, small and simple (the only milestone in scope now)

**Spikes first.** Throwaway probes; keep them as skill probes only if they stay useful.
- **S1.** `LIBVA_TRACE` with the **old** driver for ffmpeg, mpv (both hwdec modes), GStreamer and
  Chrome: call order, threads, whether and when `vaSyncSurface` comes, slice-data framing, export
  flags. Settles C6–C11.
- **S2.** NDK MediaCodec latency probe (adb-shell ELF). Feed an x264 B-pyramid stream as is vs with
  the VUI rewrite, ± `KEY_LOW_LATENCY`, through an AImageReader. Log input→output lag in frames and
  check pts propagation (P11). **Go/no-go for the design in §6.2.**
- **S3.** Y2Y copy probe: codec output (PRIVATE) → `GL_EXT_YUV_target` copy into an NV12 AHB with
  `GPU_COLOR_OUTPUT | CPU_*`. Compare bytes with a ByteBuffer-mode decode of the same frame. Also the
  layout probe (lockPlanes vs `mmap`). Settles §6.3 and the truthful-export check.

**Then:**
1. `MobileVA/` skeleton + CMake options, host test target; nothing links into MobileGL.
2. `Common/`: protocol, framing, SCM_RIGHTS, bit I/O, emulation prevention + tests.
3. `Codec/H264/`: pre-parse, SPS/PPS synthesis, AU assembly, change detection + golden tests.
4. MobileGL hooks:
   - `ServiceBind` in `PairAcceptor` and a service registry
   - the server C ABI (§4.3)
   - the `DECODE_TARGET` allocation flag
   - **YUV sampling waits on write fences and on pending writes** (P5), in both backends
   - tests in `MG_Test` (SharedImageTest, pairing tests)
5. `Service/`: lazy load, JNI caps, session per connection, codec + reader per context, seq→surface
   map, copy thread + fences, Sync/Query/Readback, stats, failure isolation.
6. `Driver/`: the vtable, objects, connection + reader thread + lazy reconnect, export/derive/get
   image, `MOBILEVA_TRACE`.
7. Packaging: anland jniLibs, `xbuild.sh`/`xdeploy.sh` install, environment files, knob + helper,
   `mobilegl-startup.sh chrome`.
8. Verification (§8) on Espryt and Magma, with numbers.
9. Land:
   - README (knobs, A/B switch)
   - skill SKILL.md / reproduce.md (build + install the driver and service on a new machine)
   - memory files for non-obvious findings
   - remove worktrees and build dirs

### M2: VP9 (TODO, not started)

- VA VP9 slice data is the **whole frame** (uncompressed header + compressed header + tiles), so there
  is no header rebuild. Feed each frame as one input; clients split superframes (unverified per
  client).
- **Profiles:** 0 (8-bit 4:2:0), 2 (10-bit → P010). Profiles 1/3 (4:4:4) only if MediaCodec lists
  them.
- **Hidden frames** (`show_frame = 0`, typically the alt-ref).
  - The target surface must hold the hidden frame, because the client later displays it by
    `show_existing_frame` **without** a VA call (Chrome's VP9 decoder does this; unverified for
    others). MediaCodec never outputs a hidden frame.
  - Plan: after the hidden frame, queue a **synthesized 1-byte `show_existing_frame` header**
    (frame_marker, profile bits, `show_existing_frame = 1`, `frame_to_show_map_idx` = a slot from the
    frame's `refresh_frame_flags`). Map its output to the hidden frame's surface.
  - **Gotcha to verify first:** in some decoders, showing an existing frame may update
    `last_show_frame` / previous-frame MV state, which feeds `use_prev_frame_mvs` of the next frame.
    Injecting between a hidden ARF and the next frame could then change pixels. Test with libvpx ARF
    streams and framemd5. If it breaks, inject only after the next shown frame, or keep the hidden
    frame's surface pending until a later output can be routed to it (open question Q6).
- A client-submitted `show_existing_frame` (some clients do submit it) maps to "copy the slot's last
  known surface" with no codec input.
- Resolution changes on non-key frames (reference scaling) are legal in VP9; adaptive playback needs
  `KEY_MAX_WIDTH/HEIGHT` = context size.

### M3: AV1 (TODO, the hardest)

- VA AV1 slice data carries **tile data only** (tile-group OBU payloads, with per-tile
  offsets/sizes in `VASliceParameterBufferAV1`). The **sequence header OBU, frame header OBU and tile
  group framing must all be synthesized** from `VADecPictureParameterBufferAV1`.
- **Sequence header:**
  - From VA: profile, `still_picture`, `use_128x128_superblock`, the tool-enable flags,
    `enable_order_hint` + `order_hint_bits`, bit depth, `mono_chrome`, colour range, subsampling,
    chroma position, `film_grain_params_present`.
  - Chosen: one operating point (idc 0), `seq_level_idx` from size (open question Q3),
    `frame_id_numbers_present_flag = 0`, `reduced_still_picture_header = 0`, no timing/decoder-model
    info.
- **Frame header:** the full `uncompressed_header()` from VA:
  - frame type, show/showable flags, `error_resilient_mode`, `disable_cdf_update`, screen content /
    integer MV, `frame_size_override_flag = 1` with explicit sizes, render size, superres,
    `allow_intrabc`, explicit `ref_frame_idx[]` (`frame_refs_short_signaling = 0`),
    `allow_high_precision_mv`, interpolation filter, `is_motion_mode_switchable`, `use_ref_frame_mvs`,
    `refresh_frame_flags`, `primary_ref_frame`, `disable_frame_end_update_cdf`
  - `tile_info()` from VA tile counts/sizes + `context_update_tile_id`; our own `TileSizeBytes = 4`
  - quantization (deltas, qmatrix), segmentation (write every feature explicitly), delta q/lf
  - loop filter **with every ref/mode delta written explicitly**, CDEF, loop restoration, `tx_mode`,
    reference select, skip mode, warped motion, reduced tx set
  - global motion params, film grain (`update_grain = 1`, full params)
- **Gotchas:**
  - **Global motion params are coded relative to the primary reference frame's params**
    (`PrevGmParams`), so the rebuilder must track every frame's GM params per slot exactly as the
    decoder does.
  - `ref_order_hint[]` is written only when error-resilient and order hints are enabled.
  - Multiple tile groups per frame are merged into one tile group OBU with re-written
    `tile_size_minus_1` fields.
- **Hidden frames:** as for VP9, synthesize a frame-header OBU with `show_existing_frame = 1` (temporal
  delimiter first). **Never for a hidden KEY frame:** showing an existing key frame runs the reference
  refresh process and changes decoder state. That case is open question Q7.
- **Film grain:** MediaCodec outputs grain-applied frames. Copy them into
  `current_display_picture`'s surface. The grain-free reference surface is only a handle for us,
  because MediaCodec keeps its own references. framemd5 against libdav1d is expected to match (grain
  synthesis is normative).
- **10-bit:** P010 surfaces; S3's copy proof must be repeated for 10-bit.

### M4: HEVC + 10-bit (TODO)

- VA HEVC slice data is the whole slice NAL. **VPS/SPS/PPS** are synthesized from
  `VAPictureParameterBufferHEVC` (+ `...HEVCRext` / SCC).
- **Missing from VA:** `profile_tier_level` level (heuristic), VPS fields (derive from SPS values) and,
  above all, **the SPS's short-term RPS candidate sets**. VA gives only `num_short_term_ref_pic_sets`
  and the slice-level `st_rps_bits`, and the long-term SPS candidates are missing too.
- **Approach:** synthesize an SPS with `num_short_term_ref_pic_sets = 0` and
  `long_term_ref_pics_present` as needed. **Rewrite every slice segment header** with an explicit
  `st_ref_pic_set` (and explicit long-term entries) built from the picture's RPS in VA
  `ReferenceFrames`:
  - `ST_CURR_BEFORE`/`AFTER` give used entries.
  - DPB pictures with neither flag give "foll" entries with `used_by_curr = 0`.
  - `LT_CURR` gives long-term entries via `poc_lsb_lt` and an explicit MSB.
  - This is feasible because HEVC slice data starts **byte-aligned** after `byte_alignment()`
    (`slice_data_byte_offset` in VA). Re-serialize the header, re-escape it, and append the original
    slice data bytes.
  - Entry points (tiles/WPP) are copied as parsed.
- **Scaling lists:** write `VAIQMatrixBufferHEVC` explicitly, including the 16x16/32x32 DC values.
- **10-bit:** Main10 → P010; verify 10-bit Y2Y precision in S3.
- `sps_max_num_reorder_pics = 0` / `sps_max_latency_increase_plus1 = 0` for low latency, as in H.264.

### Later TODOs (from M1 deferrals)

| Id | TODO |
|---|---|
| H264-1 | Field pictures (PAFF): both fields arrive as two VA pictures with the same target. Assemble both field NALs into one access unit, or feed them separately and map the single output to the shared target. Verify the codec's behaviour. |
| H264-2 | POC type 1: rewrite slice headers to POC type 0 (CABAC slice data is byte-aligned after `cabac_alignment_one_bit`; CAVLC needs a bit shift), or recover the offsets from the resolved POCs the client sends. |
| H264-3 | High10 / 4:2:2 only if a device lists them. |
| FF | **Firefox:** it decodes with ffmpeg's VA hwaccel in its RDD process and exports with `vaExportSurfaceHandle`, probably `SEPARATE_LAYERS` imported per plane (needs R3). Checks: whether the RDD sandbox allows our `connect()` (connect at `vaInitialize`; `MOZ_DISABLE_RDD_SANDBOX=1` only for diagnosis), how its `vaapitest` probe process reacts (vaInitialize + profile query must be fast and side-effect free), and which prefs are needed for a non-allowlisted GL vendor. No app-specific code. |
| R1 | **Copy elision** for MobileGL-internal readers: let GL readers sample the codec AHB directly and write the surface copy only when someone can see the raw memory. Blocked: a third party may `mmap` the exported fd with no VA call, so we cannot know. Revisit only with a safe signal. |
| R2 | **Explicit fences end-to-end:** attach the copy's sync_file to the exported dma-buf with `DMA_BUF_IOCTL_IMPORT_SYNC_FILE` (kernel ≥ 6.0; the device has 6.1), so implicit-sync consumers wait on the GPU and `vaSyncSurface` can answer before the fence signals. |
| R3 | **Per-plane imports in MobileGL:** R8/GR88 (R16/GR1616) EGLImages that alias a plane of a server YUV image. Needed by mpv's GL interop, Firefox and KWin's YUV linux-dmabuf path. Espryt: GPU plane extraction via Y2Y sampling; Magma: per-plane views. |
| R4 | One GPU pass instead of two for MobileGL readers (Y2Y copy + RGBA conversion), once R1 is solved. |
| R5 | Shared-memory ring for large access units instead of socket writes, if profiling shows socket cost at 4K. |

## 10. Open questions

- **Q1.** Do Adreno's (and other vendors') hardware AVC decoders honour `max_num_reorder_frames = 0`
  / `KEY_LOW_LATENCY` with immediate output? (S2.) If not, what bounded strategy do we take for
  single-threaded sync-after-decode clients?
- **Q2.** Is `GL_EXT_YUV_target` rendering into a `Y8Cb8Cr8_420 | GPU_COLOR_OUTPUT | CPU_*` AHB
  allowed and bit-exact? (S3.) How common is the extension outside Adreno?
- **Q3.** Level selection for synthesized SPS / sequence headers when VA gives none: smallest
  sufficient level, or the codec's maximum? Some decoders size their DPB by level.
- **Q4.** `VAIQMatrixBufferH264` list order convention. Verify against the old driver with a CQM
  stream.
- **Q5.** Does Chrome sync surfaces at all (C8)? If not, the bounded server-side wait in P5 is
  load-bearing for correctness, not just a safety net.
- **Q6.** VP9 hidden-frame injection vs `last_show_frame` / previous-MV state. Where may a synthesized
  `show_existing_frame` be inserted safely?
- **Q7.** AV1 hidden key frames: how do we obtain their pixels without a state-changing
  show-existing?
- **Q8.** `ServiceBind` on the shared listener vs a separate socket: confirm the acceptor change stays
  small and keeps the PairAcceptor tests' guarantees.
- **Q9.** Concurrency limits: how many simultaneous hardware decoders the device allows
  (`getMaxSupportedInstances`), and how Chrome reacts to `ALLOCATION_FAILED` on the N+1th video.
- **Q10.** Server restart: Chrome's GPU process restarts with the GL session anyway. Do mpv and
  GStreamer recover from `OPERATION_FAILED` mid-stream, or must the driver reconnect transparently and
  re-create contexts? (It cannot rebuild MediaCodec references; it would have to wait for the next
  IDR.)
