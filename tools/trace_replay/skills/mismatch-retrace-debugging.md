# Mismatch retrace debugging

Use this when an apitrace fixture replays correctly on one environment but
`mobilegl_trace_replay`, Android trace replay, or another backend produces a
different image.

## Goal

Find the first call where the replayed output diverges from a trusted golden
environment, then reduce the problem to one of:

- vertex input, index, transform, or VS output mismatch
- fragment shader, uniform, texture, sampler, or buffer input mismatch
- framebuffer, blend, depth/stencil, scissor, color mask, sRGB, or format
  mismatch
- missing/incorrect resource binding or unsupported backend format/path

Do not start by guessing from the final image. First identify the earliest
divergent render pass and draw call.

## Variables

```sh
export REPO="$PWD"
export WORK="$PWD/.trace-debug"
export CASE="case-name"
export TRACE="$WORK/$CASE/trace.trace"
export APITRACE="$REPO/build-apitrace/apitrace"
export GOLDEN_GLRETRACE="/path/to/golden/glretrace"
export TARGET_GLRETRACE="/path/to/target/glretrace"
export MOBILEGL_REPLAY="$REPO/build-linux/tools/trace_replay/mobilegl_trace_replay"
export MOBILEGL_LIB="$REPO/build-linux/libMobileGL.so"
export WIDTH=854
export HEIGHT=480
export TARGET_CALL=0
export BACKEND=DirectGLES
```

The golden and target environments must use the same trace, surface size, target
call numbers, and comparison crop. They do not need to report the same GL vendor,
renderer, version, or extension set. The only requirement for the golden
environment is that it can replay the case and pass the fixture's golden image
test. Disable overlays and use a strict SSIM threshold while localizing the
first divergent call unless the known golden already has small nondeterministic
noise.

## Prepare two replay environments

1. Golden environment: a known-good driver/backend that can replay the trace,
   produce snapshots, and pass the case against the committed or otherwise
   accepted golden PNG. Prefer upstream `glretrace` on desktop GL/Mesa when it
   is the simplest environment that satisfies that test.
2. Target environment: the backend being troubleshot. This can be another
   `glretrace` environment, `mobilegl_trace_replay` on Linux, or the Android
   trace APK.

First prove the golden environment is acceptable by comparing its target-call
snapshot against the accepted golden PNG:

```sh
mkdir -p "$WORK/$CASE"
"$GOLDEN_GLRETRACE" --headless -S "$TARGET_CALL" -s "$WORK/$CASE/golden." \
  --call-nos "$TRACE" > "$WORK/$CASE/golden-retrace.log" 2>&1
```

If the generated snapshot does not pass the fixture golden comparison, choose a
different golden environment before doing mismatch localization.

Then run the target with that accepted golden snapshot or with the fixture's
committed golden PNG:

```sh
mkdir -p "$WORK/$CASE"

"$MOBILEGL_REPLAY" \
  --trace "$TRACE" \
  --golden "$WORK/$CASE/golden.$(printf '%010d' "$TARGET_CALL").png" \
  --output "$WORK/$CASE/target-id" \
  --backend "$BACKEND" \
  --mobilegl-library "$MOBILEGL_LIB" \
  --target-call "$TARGET_CALL" \
  --width "$WIDTH" \
  --height "$HEIGHT" \
  --ssim-threshold 1.0 > "$WORK/$CASE/target-retrace.log" 2>&1 || true
```

Keep target GL identity lines such as `MOBILEGL_TRACE_GL_VENDOR`,
`MOBILEGL_TRACE_GL_RENDERER`, `MOBILEGL_TRACE_GL_VERSION`, and
`MOBILEGL_TRACE_GL_SHADING_LANGUAGE_VERSION` as diagnostic context only. Do not
reject a golden environment because its GL strings differ from the target.

For Android, use `android-plugin/trace-replay-ci.sh` with the same trace,
golden, target call, width, height, crop, and SSIM threshold values. Pull
`result.json`, `actual.png`, `diff.png`, `retrace.log`, and `logcat.txt` before
continuing.

## Build a call map

Dump a narrow call listing around the failing frame or target call:

```sh
"$APITRACE" dump --calls="$START_CALL-$END_CALL" "$TRACE" \
  > "$WORK/$CASE/calls-$START_CALL-$END_CALL.txt"
```

Mark:

- FBO/render-target changes: `glBindFramebuffer`, `glFramebuffer*`,
  `glDrawBuffer*`, `glReadBuffer`, `glViewport`, `glScissor`
- pass-producing calls: `glClear*`, `glDraw*`, `glMultiDraw*`,
  `glDispatchCompute`, `glBlitFramebuffer`, `glCopyTex*`,
  `glGenerateMipmap`
- sync/resource hazards: `glMemoryBarrier`, `glFenceSync`, `glWaitSync`,
  `glFlush`, `glFinish`
- texture/buffer mutation before the pass: `glTex(Sub)Image*`,
  `glCompressedTex(Sub)Image*`, `glBuffer(Sub)Data`, `glMapBuffer*`,
  `glUnmapBuffer`

Treat a render pass as a contiguous run of producer calls that writes the same
draw framebuffer attachments with the same logical viewport/scissor region,
ending just before the next framebuffer/attachment change, resolve/blit,
mipmap generation, or swap.

## Find the first divergent render pass

Use snapshots at pass endpoints rather than every draw call first. For regular
`glretrace`, snapshot call numbers are explicit:

```sh
mkdir -p "$WORK/$CASE/pass"
CALLS="1000,1250,1430,1700"
"$GOLDEN_GLRETRACE" --headless --call-nos \
  --snapshot-prefix "$WORK/$CASE/pass/golden." \
  --snapshot "$CALLS" "$TRACE"
"$TARGET_GLRETRACE" --headless --call-nos \
  --snapshot-prefix "$WORK/$CASE/pass/target." \
  --snapshot "$CALLS" "$TRACE"
```

If the target is MobileGL, run `mobilegl_trace_replay` once per endpoint:

```sh
for call in 1000 1250 1430 1700; do
  "$MOBILEGL_REPLAY" \
    --trace "$TRACE" \
    --golden "$WORK/$CASE/pass/golden.$(printf '%010d' "$call").png" \
    --diff "$WORK/$CASE/pass/mobilegl.$(printf '%010d' "$call").diff.png" \
    --output "$WORK/$CASE/pass/mobilegl-$call" \
    --backend "$BACKEND" \
    --mobilegl-library "$MOBILEGL_LIB" \
    --target-call "$call" \
    --width "$WIDTH" \
    --height "$HEIGHT" \
    --ssim-threshold 1.0 || true
done
```

Binary search the pass endpoints:

- If endpoint N matches golden, every completed pass up to N is probably OK.
- If endpoint N differs, search earlier pass endpoints.
- Stop when the previous pass endpoint matches and this pass endpoint differs.

For offscreen passes, do not force backbuffer snapshots. The default retrace
snapshot reads the current draw buffer, which is usually what is needed for FBO
attachment debugging. Use `--snapshot-force-backbuffer` only when explicitly
checking the final default framebuffer.

## Find the first divergent draw call inside the pass

List producer calls inside the failing pass and binary search those call
numbers. Include clears, blits, compute dispatches, and mipmap generation; a
non-draw producer can be the first bad write.

```sh
mkdir -p "$WORK/$CASE/draw"
DRAWS="1430,1442,1459,1490,1512,1550"
"$GOLDEN_GLRETRACE" --headless --call-nos \
  --snapshot-prefix "$WORK/$CASE/draw/golden." \
  --snapshot "$DRAWS" "$TRACE"
```

Compare the target at the same call numbers. The first bad producer is the
lowest call where:

- previous producer output matches golden
- current producer output differs from golden

Keep the previous-good call and first-bad call. Most state that matters to the
bad call was established between those two calls or immediately before the
previous-good call.

## Dump and diff GL state

Use apitrace state dumps around the boundary:

```sh
"$GOLDEN_GLRETRACE" --headless --dump-state "$PREVIOUS_GOOD_CALL" "$TRACE" \
  > "$WORK/$CASE/state-golden-prev.json"
"$GOLDEN_GLRETRACE" --headless --dump-state "$FIRST_BAD_CALL" "$TRACE" \
  > "$WORK/$CASE/state-golden-bad.json"
"$TARGET_GLRETRACE" --headless --dump-state "$FIRST_BAD_CALL" "$TRACE" \
  > "$WORK/$CASE/state-target-bad.json"
```

If both environments are standard `glretrace`, use apitrace's helper to compare
snapshots and optionally state:

```sh
python3 "$REPO/3rdparty/apitrace/scripts/retracediff.py" \
  --retrace "$GOLDEN_GLRETRACE" \
  --ref-env NAME=VALUE \
  --src-env NAME=VALUE \
  --diff-prefix "$WORK/$CASE/retracediff" \
  --snapshot-frequency "$DRAWS" \
  --diff-state \
  "$TRACE"
```

For MobileGL target state, inspect MobileGL logs and add temporary focused
logging in the relevant state sync path. Keep logs scoped to the bad call range
using call-number markers or a `MOBILEGL_TRACE_CURRENT_CALL_OVERRIDE` style
guard if available.

## Classify the bad call

Start from the bad call's command and bound pipeline. Use the call dump,
`--dump-state`, shader sources, and MobileGL backend logs.

### Vertex and VS stage

Check:

- current program, vertex shader source, translated shader source, compile/link
  log, and reflected attribute locations
- VAO binding and every enabled attribute: size, type, normalized/integer flag,
  stride, relative offset, divisor, binding index, and bound VBO
- index buffer binding, index type, byte offset, base vertex, draw count, and
  primitive mode
- UBO/SSBO bindings consumed by the VS, including offset alignment and range
  size
- transform-relevant uniforms and uniform block contents
- primitive restart, provoking vertex, clip/cull distance, front-face, cull
  mode, depth clamp, and viewport transform

Common MobileGL failures here are attribute location remapping, integer vs float
attribute path mixups, stale VAO/EBO binding, base-vertex handling, wrong buffer
range offset, and translated VS precision/type differences.

### Fragment and FS stage

Check:

- fragment shader source, translated shader source, compile/link log, reflected
  sampler/image/uniform bindings, and output locations
- all textures sampled by the FS: target, level, layer, internal format,
  dimensions, swizzle, base/max level, compare mode/function, border/wrap/filter,
  and whether the backend allocated a supported equivalent format
- sampler object binding versus texture object sampler state
- plain uniforms, UBO/SSBO ranges, image bindings, atomic counters, and texture
  buffer bindings
- discard/alpha-test-equivalent logic, precision-sensitive math, NaN/min/max
  behavior, shadow compare, and integer/unsigned sampling

Common MobileGL failures here are stale sampler binding, texture unit remap
errors, depth-compare defaults, unsupported or substituted texture formats,
incorrect swizzle, missing DSA texture parameter sync, and shader translation
differences around precision or undefined values.

### Framebuffer and composition stage

Check:

- draw framebuffer object, every color/depth/stencil attachment, attachment
  level/layer, backend object id, internal format, sample count, and dimensions
- draw buffer/read buffer mapping, MRT compacting/remapping, and default-FBO
  handling
- viewport, scissor, depth range, color mask, depth mask, stencil masks,
  clear values, and per-target blend enable
- blend equation/function/color, logic op, dither, sRGB enable, multisample
  state, sample mask, alpha-to-coverage, depth/stencil test and ops
- resolve/blit path, invalidate/discard behavior, mipmap generation, and
  readback orientation/channel order

Common MobileGL failures here are stale FBO sync, draw/read framebuffer
confusion, wrong MRT attachment order, missing per-target blend state, format
substitution, sRGB mismatch, and backend readback from the wrong buffer.

## Resource validity checks

Before changing code, rule out invalid trace/backend combinations:

- Does the target backend advertise every extension used by the trace path?
- Is the internal format renderable/filterable/sampleable on the target GLES or
  Vulkan path?
- Is the attachment complete under target rules, especially depth/stencil,
  integer color, multisample, layered, and texture-buffer cases?
- Are buffer ranges aligned to the target backend's UBO/SSBO/TBO requirements?
- Are compressed textures, texture views, immutable storage, and DSA calls fully
  implemented in the target path?
- Is any GL error logged at or before the first bad call? The first GL error is
  usually more useful than the final pixel diff.

## Confirm the root cause

Make one narrow diagnostic change at a time. Record it in `attempts.txt` when
the case is already tracked there:

- what was changed
- exact case/backend/device
- first bad pass/call before and after
- final `mismatchPixels`
- whether the change moved, fixed, or did not affect the divergence

A real fix should make the first-bad call match, not just reduce the final
mismatch. After fixing, rerun:

```sh
ctest --test-dir "$REPO/build-linux/tools/trace_replay" -V \
  -R "MobileGLTraceReplay\\.$CASE\\.$BACKEND"
```

For Android-only bugs, rerun `android-plugin/trace-replay-ci.sh` and keep
`result.json`, `actual.png`, `diff.png`, `retrace.log`, and `logcat.txt` as the
evidence bundle.
