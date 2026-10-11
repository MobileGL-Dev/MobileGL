"""Render driver.
  python render.py still T [T ...] [--scale S] [--samples N] [--only name]   -> out/still_T.png
  python render.py sheet T0 T1 N [--scale S]                                -> out/sheet_T0_T1.jpg
  python render.py video T0 T1 [--scale S] [--samples N] [--out file]       -> mp4 (with music)
  python render.py final [--samples N]                                      -> out/mobilegl_hype.mp4
MGL_LIBGL=<path to opengl32.dll> renders through that GL implementation."""
import os
import sys
import time
import subprocess
import numpy as np
from PIL import Image

import score as S
from engine import Engine
from anim import Frame, frame_idx
import importlib
scenes = importlib.import_module(sys.argv[sys.argv.index('--module') + 1] if '--module' in sys.argv else 'scenes')

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'out')
FPS = int(sys.argv[sys.argv.index('--fps') + 1]) if '--fps' in sys.argv else 60


def opt(name, default, cast=float):
    if name in sys.argv:
        return cast(sys.argv[sys.argv.index(name) + 1])
    return default


def draw_frame(E, t, fi):
    n = E.samples
    P = None
    for s in range(n):
        ts = t + ((s + 0.5) / n - 0.5) * E.shutter / FPS if n > 1 else t
        p = scenes.draw(E, ts)
        if s == n // 2 or P is None:
            P = p
        E.accumulate(1.0 / n, s == 0)
    E.post(P or {}, fi)


def grab(E):
    a = np.frombuffer(E.read(), np.uint8).reshape(E.H, E.W, 4)
    return a


def make_engine():
    E = Engine(scale=opt('--scale', 1.0), samples=opt('--samples', 1, int), shutter=opt('--shutter', 0.5))
    if 'MobileGL' in E.renderer:
        import sc_final
        gpu = E.renderer.rsplit('(', 1)[1].split(',')[0] if '(' in E.renderer else 'GPU'
        backend = 'Magma → Vulkan' if 'Vulkan' in E.renderer else 'Espryt → OpenGL ES'
        sc_final.RENDERED_WITH['line'] = f'{backend} · {gpu} · Windows / WGL'
    return E


def still(times):
    E = make_engine()
    print('renderer:', E.renderer)
    paths = []
    for t in times:
        draw_frame(E, t, frame_idx(t))
        p = os.path.join(OUT, f'still_{t:07.2f}.png')
        Image.fromarray(grab(E)[:, :, :3]).save(p)
        paths.append(p)
    print('\n'.join(paths))


def sheet(t0, t1, n, cols=4):
    E = make_engine()
    imgs = []
    for k in range(n):
        t = t0 + (t1 - t0) * k / max(n - 1, 1)
        draw_frame(E, t, frame_idx(t))
        im = Image.fromarray(grab(E)[:, :, :3]).resize((480, 270), Image.LANCZOS)
        imgs.append((t, im))
    rows = (n + cols - 1) // cols
    sh = Image.new('RGB', (cols * 484, rows * 290), (30, 30, 30))
    from PIL import ImageDraw
    d = ImageDraw.Draw(sh)
    for k, (t, im) in enumerate(imgs):
        x, y = (k % cols) * 484 + 2, (k // cols) * 290 + 2
        sh.paste(im, (x, y))
        d.text((x + 4, y + 272), f'{t:.2f}s  bar {t / S.BAR:.2f}', fill=(220, 220, 220))
    p = os.path.join(OUT, f'sheet_{t0:.1f}_{t1:.1f}.jpg')
    sh.save(p, quality=88)
    print(p)


def video(t0, t1, out, audio=True, crf=16, preset='slow'):
    E = make_engine()
    print('renderer:', E.renderer)
    f0, f1 = int(round(t0 * FPS)), int(round(t1 * FPS))
    args = ['ffmpeg', '-y', '-loglevel', 'error', '-f', 'rawvideo', '-pix_fmt', 'rgba', '-s', f'{E.W}x{E.H}',
            '-r', str(FPS), '-i', '-']
    wav = opt('--audio', os.path.join(OUT, 'music.wav'), str)
    if audio and os.path.exists(wav):
        args += ['-ss', f'{t0:.4f}', '-t', f'{t1 - t0:.4f}', '-i', wav, '-c:a', 'aac', '-b:a', '256k']
    args += ['-c:v', 'libx264', '-preset', preset, '-crf', str(crf), '-pix_fmt', 'yuv420p',
             '-movflags', '+faststart', '-shortest' if audio else '-an', out]
    args = [a for a in args if a]
    proc = subprocess.Popen(args, stdin=subprocess.PIPE)
    start = time.time()
    for fi in range(f0, f1):
        t = fi / FPS
        draw_frame(E, t, fi)
        proc.stdin.write(E.read())
        if (fi - f0) % 300 == 0:
            el = time.time() - start
            done = fi - f0 + 1
            print(f'frame {fi} ({t:.1f}s)  {done / max(el, 1e-6):.1f} fps', flush=True)
    proc.stdin.close()
    proc.wait()
    print(out, f'{time.time() - start:.0f}s')


if __name__ == '__main__':
    os.makedirs(OUT, exist_ok=True)
    mode = sys.argv[1]
    nums = []
    for a in sys.argv[2:]:
        if a.startswith('--'):
            break
        nums.append(a)
    if mode == 'still':
        still([float(x) for x in nums])
    elif mode == 'sheet':
        sheet(float(nums[0]), float(nums[1]), int(nums[2]))
    elif mode == 'video':
        video(float(nums[0]), float(nums[1]), opt('--out', os.path.join(OUT, 'clip.mp4'), str),
              crf=opt('--crf', 18, int), preset=opt('--preset', 'veryfast', str))
    elif mode == 'final':
        video(0.0, S.END, opt('--out', os.path.join(OUT, 'mobilegl_hype.mp4'), str), crf=opt('--crf', 16, int),
              preset=opt('--preset', 'slow', str))
