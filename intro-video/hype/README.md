# MobileGL hype reel

A 3:33 code-rendered, beat-synced showreel at 1080p60. The track is a 128 BPM electro-pop instrumental,
synthesised from the same score the picture is cut to. The palette is sampled from the MobileGlues icon.

The final cut was rendered **through MobileGL itself**. The renderer is an ordinary OpenGL 4.3 core program, and
pointing it at MobileGL's `opengl32.dll` (Magma → Vulkan) sends every draw, framebuffer and readback through
MobileGL. The outro credits the GL implementation that actually drew the frames.

## How it is put together

- **One score, two consumers.** `score.py` holds the tempo grid (128 BPM, 112 bars), the sections, harmony and
  melodies, and every drum/fx event. `music.py` synthesises the track from it. Scenes read the same events
  (kick, clap, roll, crash, impact…) as decaying pulses, so cuts, zooms, shakes and squashes land exactly on the
  music.
- **A frame is a pure function of time.** Each frame averages 8 sub-frames across a 180° shutter (motion blur),
  then goes through HDR post: bloom, chromatic aberration, vignette, a fixed print grain.
- **Pacing.** Each drop speeds up toward a climax: 4-beat ideas, then 2, then 1, then 8ths, with growing zoom,
  shake and speed lines. Full-screen colour never changes faster than once a beat.

| file | what |
|---|---|
| `score.py` | tempo grid, sections, chords, hook melody, all events, `pulse()` |
| `dsp.py` | numba DSP kit: PolyBLEP oscillators, TPT state-variable filter, ping-pong delay, compressor, look-ahead limiter |
| `music.py` | the instrumental: kick/clap/hats/rolls/risers/impacts, supersaw chords with sidechain, plucks, FM keys, a mono lead with glide, convolution reverb, stutters, mastering to −10.5 LUFS |
| `analyze.py` | per-section loudness/band report and a spectrogram of a rendered mix |
| `engine.py` | the GL engine (moderngl): instanced SDF shapes, HarfBuzz + FreeType variable-font text with a texture cache, offscreen layers, sub-frame accumulation, bloom and final post |
| `shaders/block.glsl` | the MobileGlues mark, raymarched: a crafting-table / command-block block in translucent, slime-like glue that can be dropped in (splash, ripples, tongues erupting) |
| `shaders/goo.glsl` | 2D glue metaballs (smooth-union circles and tapered capsules, dome-shaded) |
| `anim.py` | palette, easing, `Frame` (local beats and pulses) |
| `kit.py` | backgrounds, HUD, chips, slams, stamps, checks, arrows, speed lines, isometric boxes, glue, the mark |
| `devices.py` | isometric phone / monitor / laptop / tower / board and the screens painted into them |
| `sc_open.py` … `sc_final.py`, `scenes.py` | one module per section; `scenes.py` is the timeline and the stutter glitch |
| `render.py` | `still`, `sheet`, `video`, `final`; pipes raw frames to ffmpeg and muxes the audio |
| `sting.py`, `sting_audio.py` | a 4.5 s standalone sting of the mark (block drops into glue), no text, with its own sound |

## Setup

```sh
pip install -r requirements.txt
python fetch_fonts.py        # Archivo (+ Italic) and JetBrains Mono, SIL OFL
```

`ffmpeg` must be on `PATH`. Any OpenGL 4.3 core driver works for previews.

## Render

```sh
python music.py out/music.wav                          # the soundtrack (~40 s)
python render.py sheet 30 60 24 --scale 0.5            # contact sheet of a time range
python render.py still 31.5 86 --scale 0.5             # single frames
python render.py video 30 60 --scale 0.5 --fps 30      # quick preview clip with audio
python render.py final --samples 8 --crf 18            # out/mobilegl_hype.mp4, 1080p60
```

Through MobileGL (Windows, `opengl32.dll` drop-in built from this repo):

```sh
MOBILEGL_BACKEND_TYPE=DirectVulkan MGL_LIBGL=C:\path\to\opengl32.dll \
  python render.py final --samples 8 --crf 18
```

On the RTX 3080 this runs at about 8 fps (≈28 min for the full reel). The final credit line names the backend and
GPU from `GL_RENDERER`. To check MobileGL against the native driver, render the same frames both ways and diff
them. They match to well under 1/255 on average (the film grain uses an integer hash so it is identical on every
implementation).

The sting:

```sh
python sting_audio.py out/sting.wav
python render.py video 0 4.5 --module sting --audio out/sting.wav --samples 8 --out out/glue_drop.mp4
```

Grain options: `GRAIN_K` scales the grain (default 0.6), and `GRAIN_RATE=n` re-rolls it every n frames
(default 0 = a fixed print texture; animated grain makes the encode several times larger).
