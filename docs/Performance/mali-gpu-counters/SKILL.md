---
name: mali-gpu-counters
description: Capture and compare Mali GPU hardware counters (fragment/vertex cycles, tiles, ZS, bandwidth, tile write elimination) on a rooted Android phone through perfetto and the vendor's libgpudataproducer, to find where one renderer spends more GPU time than another. Use when a GPU-bound workload is slower on Magma (DirectVulkan) than on the device's GLES driver, or slower after a change, and you need to know which part of the GPU pipeline the gap is in.
---

# Mali GPU performance counters

Use this to answer "where does Magma spend the GPU time that the system GLES driver (or the previous build) doesn't?" on a Mali phone. Two captures, each normalized per frame, show which pipeline stage is responsible: vertex/tiling, fragment shading, depth testing, or memory bandwidth. Do this before guessing at shader or pass-structure fixes.

The workflow was built and verified on a MediaTek MT6893 (Mali-G77 MC9, r32 driver, TrebleDroid). The vendor library path and counter IDs can differ on other devices; step 2 shows how to list them.

## Prerequisites

- A rooted phone (`adb root` or `su`). Check that `/vendor/lib64/libgpudataproducer.so` exists. This is the vendor library behind Android GPU Inspector's counters.
- The workload must be **GPU-bound**. Check `/sys/kernel/ged/hal/gpu_utilization` on MediaTek (first field ≥ 90%). Otherwise counter differences say nothing about FPS.
- Fixed clocks, so cycles are comparable across runs. On MT6893:
  - GPU: `echo 836000 > /proc/gpufreq/gpufreq_opp_freq`.
  - CPU: `echo 7 1 > /proc/ppm/policy_status`, then `echo <cluster> <kHz>` into `/proc/ppm/policy/userlimit_{min,max}_cpu_freq`.
  - The sysfs `scaling_*_freq` files are read-only there.
  - Restore everything afterwards.
- Host: the NDK, `protoc`, and a venv with `pip install perfetto` (TraceProcessor downloads its shell binary on first use).

## 1. Start the counter producer

`gpuprod.c` dlopens the vendor library and calls its exported `start()`, which registers the perfetto data source `gpu.counters`.

```sh
$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang -O2 -o gpuprod gpuprod.c
adb push gpuprod /data/local/tmp/ && adb shell chmod 755 /data/local/tmp/gpuprod
adb shell 'su -c "getenforce"'          # note the mode to restore later
adb shell 'su -c "setenforce 0; nohup /data/local/tmp/gpuprod > /data/local/tmp/gpuprod.log 2>&1 &"'
adb shell perfetto --query | grep gpu.counters   # must list /data/local/tmp/gpuprod
```

It needs SELinux permissive. **Afterwards**, kill the producer (`su -c "kill $(pidof gpuprod)"`) and restore the mode (`setenforce 1` if it was Enforcing).

## 2. List the counters this GPU offers

A capture without `counter_ids` records nothing, and the IDs are device-specific. Read them from the producer's descriptor:

```sh
adb exec-out perfetto --query-raw > qraw.bin
protoc --decode_raw < qraw.bin > qraw.txt
python3 - <<'P'
import re
s = open('qraw.txt').read()
for i, n in re.findall(r'\n\s*1: (\d+)\n\s*2: "([^"\n]{2,90})"\n\s*3: "', s):
    print(i, n)
P
```

IDs below 65535 are raw hardware counters. IDs 65535 and up are derived expressions (percentages, rates). Prefer raw counters: they add up over time, while ratios don't.

On the Mali-G77 these raw IDs were the useful set:

| ID | Counter | Use |
|---|---|---|
| 6 | GPU active cycles | total GPU cost |
| 196 / 214 | Fragment / non-fragment active cycles | which side the gap is on |
| 68 | Tiler active cycles | vertex + binning cost |
| 160 / 175 | Output external read / write beats | DRAM bandwidth |
| 210 | Tiles | how many tile passes the frame runs (≈ passes × screen tiles) |
| 211 | Constant tiles killed | tile write-backs eliminated because the tile was unchanged |
| 254 | Tile buffer write beats to L2 | tile write-back traffic |
| 203 / 202 | Rasterized / partial rasterized quads | fragment workload size |
| 204 / 206 | Early ZS tested / killed quads | early depth test working |
| 207 / 208 | Late ZS tested / killed quads | shaders forced to late ZS (discard, depth writes, clears via shader) |
| 212 | FPK occluder quads | forward pixel kill |
| 234 | Texture quads | quads that sample textures |
| 219 / 220 / 221 / 222 | FMA / CVT / SFU / message instructions | shader instruction mix |
| 242 / 243 | 32-bit / 16-bit interpolation active | fp32 vs fp16 varyings |
| 209 | Warps using more than 32 registers | register pressure |
| 70 / 75 | Triangle / visible primitives | geometry identity check |
| 85 / 101 | IDVS position / varying shading requests | vertex shading |

`counters.cfg` captures this set: 5 s at a 20 ms sampling period.

## 3. Capture each renderer

Get each renderer into the same steady scene: same camera, frozen world time, same resolution. Then capture while it runs:

```sh
adb push counters.cfg /data/local/tmp/
adb shell 'cat /data/local/tmp/counters.cfg | perfetto --txt -c - -o /data/misc/perfetto-traces/cnt.pftrace'
adb pull /data/misc/perfetto-traces/cnt.pftrace A.pftrace
```

- Record each run's FPS during the capture (for example, the on-screen FPS counter in a screenshot taken right after).
- Change only the renderer between captures.
- Cool the phone between runs and keep the clocks locked.

## 4. Compare per frame

```sh
python compare.py A.pftrace <fpsA> B.pftrace <fpsB>
```

It sums each counter over the capture, converts to per second, divides by FPS, and prints A/frame, B/frame and B/A, sorted by A's per-frame cost. Many values come out fractional, because the producer averages per-core counters across shader cores. Only the ratios matter.

## 5. Read the result

Work top-down:

1. **GPU active cycles B/A**: the size of the gap. It should roughly match the FPS ratio when GPU-bound.
2. **Fragment vs non-fragment active cycles.** Whichever ratio is above 1 holds the gap. Equal vertex counters (non-fragment, tiler, primitives, IDVS) mean the geometry and vertex shading are identical, so look only at fragment work.
3. **Tiles.** B/A ≈ 1 + k means B renders about k extra full-screen passes per frame. Typical causes:
   - a standalone clear pass;
   - a pass split by a mid-frame upload or barrier;
   - a blit, copy or clear outside a render pass (Mali runs those as fragment jobs too).

   Find them with a per-frame Vulkan command census (see [Census of Magma's Vulkan commands](#census-of-magmas-vulkan-commands)).
4. **Rasterized quads, late ZS, texture quads.**
   - With equal tiles: more rasterized quads mean extra full-screen draws inside a pass. For example, `vkCmdClearAttachments` mid-pass costs about one screen of quads each time.
   - High late-ZS counts mean fragments lost the early depth test: shaders with `discard`, `gl_FragDepth` or `gl_SampleMask` writes, or clears implemented as shader draws.
   - More early-ZS kills with equal visible primitives often mean the same thing seen from the other side.
5. **External write beats vs tile buffer write beats.**
   - If B's extra external writes ≈ extra tile write beats × shader-core count, the difference is tile write-back.
   - Check `Constant tiles killed`: the GLES driver eliminates write-back of unchanged tiles. A Vulkan path with a much lower count is losing that elimination.
   - In a static benchmark scene this is a big effect. With a moving camera it is a small one.
6. **Instruction mix and interpolation.** More FMA or CVT instructions with equal quads point at shader code quality. Much more 32-bit than 16-bit interpolation points at precision: for example, ESSL `mediump` lost when a backend recompiles the shader as desktop GLSL.

## Census of Magma's Vulkan commands

When the tile count says Magma runs extra passes, find out which ones. Use a temporary, never-committed build:

- **Wrappers:** write a header that declares counting wrappers and maps each `vkCmd*` entry point to its wrapper with a function-like macro (`#define vkCmdBeginRenderPass(...) Census_BeginRenderPass(__VA_ARGS__)`). Function-like macros leave function-pointer assignments such as `fns.vkCmdBeginRenderPass = vkCmdBeginRenderPass;` alone.
  - Include `VK_USE_PLATFORM_ANDROID_KHR` before `<vulkan/vulkan.h>` in that header.
  - Map at least render pass begin, render pass creation (to record each pass's load/store ops), blit, copy, clear (including `vkCmdClearAttachments`), resolve, dispatch, barrier, submit and present.
- **Force-include:** apply it only to the DirectVulkan sources:

  ```cmake
  file(GLOB _census ${CMAKE_CURRENT_SOURCE_DIR}/MobileGL/MG_Backend/DirectVulkan/*.cpp
                    ${CMAKE_CURRENT_SOURCE_DIR}/MobileGL/MG_Backend/DirectVulkan/Renderer/*.cpp)
  list(FILTER _census EXCLUDE REGEX "Census")   # the wrapper definitions must call the real entry points
  set_source_files_properties(${_census} PROPERTIES COMPILE_OPTIONS "-include;<path>/Census.h")
  ```

- **Recording:** in each wrapper, key the count by kind, the caller's offset in the library (`__builtin_return_address(0)` minus `dladdr`'s `dli_fbase`) and details: render area plus load/store ops for passes, area for clears and blits. In the present wrapper, every few seconds, log the counts divided by the number of frames to logcat.
- **Symbolize** the offsets against the unstripped library: `llvm-addr2line -f -C -i -e build/intermediates/cxx/RelWithDebInfo/*/obj/arm64-v8a/libMobileGL.so 0x<offset>`. The library under `stripped_native_libs` has no symbols. If a wrapper's caller is a shared helper, log `__builtin_return_address(1..5)` inside that helper; arm64 Android keeps frame pointers.

## Example: Magma vs the system driver

Minecraft 1.12.2 with OptiFine at 2400×1080 on a Mali-G77, clocks locked, SFPEW over each backend:

| Finding | Counters | Cause | Fix |
|---|---|---|---|
| Fragment active cycles 1.33×, tiles 1.46× | rasterized quads, late ZS and tiles all up; vertex counters equal | one extra full-screen depth-only pass per frame (census: `vkCmdBeginRenderPass` with only `D24S8` LOAD/STORE, plus a full-screen `vkCmdClearAttachments`) | a window clear with a window pass open now ends the pass and folds into the next one (`VulkanRenderer::DeferDefaultWireClear`) |
| Extra external writes 1.7× | tile buffer write beats ≈ 23×, constant tiles killed 0.07× | the GLES driver eliminates the write-back of unchanged tiles in a static scene; Magma's attachments get none | still open |
| Fragment cycles still 1.18× with equal tiles | rasterized quads 1.26×, late ZS 1.44× | the game's two in-pass depth clears per frame, recorded as full-screen `vkCmdClearAttachments` | under investigation |

Separately, SFPEW hands MobileGL ESSL with `precision mediump float;` inside `#ifdef GL_ES`. The source processor rewrites ESSL to desktop GLSL with `GL_ES` undefined, so that guard silently turned the shaders fp32. 16-bit interpolation counters and the RelaxedPrecision decorations in the SPIR-V show this kind of loss.

## Pitfalls

- **Unlocked clocks.** Cycle counts are only comparable at the same GPU frequency, and FPS also depends on the CPU clock when the workload is not fully GPU-bound.
- **Not checking the build.** Before comparing builds, verify that each run actually ran the build you think it did. `adb install -r` of an APK with a lower versionCode fails with `INSTALL_FAILED_VERSION_DOWNGRADE`, and the run silently uses whatever was installed.
- **Different scenes.** Counters from different scenes or camera angles are not comparable at all. Freeze world time and camera position between runs.
- **Leaving the phone modified.** The producer and permissive SELinux stay in place until you undo them. Restore both, and the clock locks, when you are done.
