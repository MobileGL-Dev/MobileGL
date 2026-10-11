# MobileGL intro — paper-cutout video

A ~4:50 explainer of what MobileGL is and how it works, in a handmade paper-cutout / origami style:
hand-cut shapes with paper grain and layered shadows, stop-motion "boil" at 12 fps, torn-paper wipes, a
procedural ukulele-and-marimba soundtrack, burned-in captions plus `.srt` files. English and Chinese versions are
rendered from the same code.

Everything is plain Python on the CPU (Pillow + numpy + scipy), deterministic, and rendered in parallel worker
processes.

## Files

| file | what |
|---|---|
| `paper.py` | the paper engine: palette, fonts and mixed Latin/CJK text runs, hand-cut polygon roughening, periodic paper grain, sprites with inner/drop shadows, stop-motion jitter, torn-paper wipes, translation lookup (`T()`) |
| `video.py` | the twelve scenes (title, the problem, meet MobileGL, why it works, frontend, shaders, records, backends, anywhere, perks, platforms, outro), their captions, and the `stills` / `meta` / `render` entry points |
| `music.py` | the soundtrack: Karplus-Strong ukulele, marimba, bass and light percussion (120 BPM, I-V-vi-IV in C), plus paper swooshes on wipes; reads the scene timings written by `video.py meta` |
| `zh.py` | the Chinese strings (`MGL_LANG=zh`) |
| `cover.py` | a 16:9 thumbnail whose centred 4:3 crop keeps everything important |
| `fetch_fonts.py` | downloads the fonts into `fonts/` |

## Setup

```sh
pip install -r requirements.txt
python fetch_fonts.py        # Fredoka, Patrick Hand, ZCOOL KuaiLe, LXGW WenKai, Cascadia Mono (all SIL OFL)
```

`ffmpeg` must be on `PATH`.

## Render

```sh
python video.py stills 3.0 45.5         # spot-check frames -> out/still_*.png
python video.py render ffmpeg           # out/video_noaudio.mp4 + out/mobilegl_intro.json/.srt (scene timings)
python music.py out/mobilegl_intro.json out/music.wav
ffmpeg -i out/video_noaudio.mp4 -i out/music.wav -c:v copy -c:a aac -b:a 192k -shortest out/mobilegl_intro.mp4
```

Chinese version: set `MGL_LANG=zh` for the `video.py` and `cover.py` steps; output goes to `out_zh/`
(`video_noaudio_zh.mp4`, `mobilegl_intro_zh.srt`). The scene timings are identical, so the same `music.wav` is used:

```sh
MGL_LANG=zh python video.py render ffmpeg
ffmpeg -i out_zh/video_noaudio_zh.mp4 -i out/music.wav -c:v copy -c:a aac -b:a 192k -shortest out_zh/mobilegl_intro_zh.mp4
```

Cover: `python cover.py` writes `out/cover.png` and a 1280×720 JPEG; `--guides` also writes a preview with the
4:3 crop lines and the cropped image.

The full render uses every core but two; expect a few minutes per language.
