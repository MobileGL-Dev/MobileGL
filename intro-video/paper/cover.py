"""Cover / thumbnail for the MobileGL intro: one 16:9 image whose centre 4:3 crop keeps everything
important; the side bands only carry decoration. MGL_LANG=zh for the Chinese cover."""
from video import *
import paper

paper.TR.update({
    'Desktop OpenGL, anywhere': '桌面 OpenGL，处处可用',
    'How it works, in 5 minutes': '5 分钟看懂它如何工作',
} if LANG == 'zh' else {})

SAFE_W = H * 4 // 3            # 1440: width kept by a centred 4:3 crop
OX = (W - SAFE_W) // 2         # 240: left edge of the safe area


@cache
def spr_big_tile(ch, color, k=1.22):
    w, h = int(160 * k), int(180 * k)
    a = Art(w, h, f'bigtile|{ch}|{color}|{k}')
    c = 46 * k
    a.poly([(0, 14 * k), (14 * k, 0), (w - c, 0), (w, c), (w, h - 14 * k), (w - 14 * k, h), (14 * k, h),
            (0, h - 14 * k)], color, rough=0.9)
    a.poly([(w - c, 0), (w - c, c), (w, c)], shade(color, 1.3), rough=0.4)
    a.text(w / 2 - 2, h / 2 + 4, ch, ('fredoka', int(122 * k), 'Bold'), WHITE, shade=True)
    return a.build()


def put(c, spr, x, y, rot=0.0, s=1.0, elev=1.2):
    c.put(spr, x, y, rot, s, None, 1, elev, boil=0)


def side_decor(c):
    """Decoration confined to the two bands outside the 4:3 crop (x < 240, x > 1680)."""
    put(c, spr_plane(MUSTARD), 125, 330, 14, 0.8, 3.0)
    put(c, spr_plane(CORAL), 110, 790, -10, 0.7, 3.0)
    put(c, spr_crane(CORAL), 1800, 300, -6, 0.5, 3.0)
    put(c, spr_plane(TEAL), 1805, 770, -16, 0.75, 3.0)
    rng = random.Random(7)
    for band in ((25, OX - 25), (OX + SAFE_W + 25, W - 25)):
        n = 0
        while n < 7:
            x, y = rng.uniform(*band), rng.uniform(60, H - 60)
            size = rng.randrange(24, 46)
            if not (band[0] + size / 2 < x < band[1] - size / 2):
                continue
            if any(abs(y - yy) < 150 for yy in (330, 790, 300, 770)) and abs(x - (band[0] + band[1]) / 2) < 90:
                continue   # keep clear of the planes / crane
            put(c, spr_confetti(rng.randrange(4), rng.choice((CORAL, MUSTARD, TEAL, BLUE, PLUM, ORANGE)), size),
                x, y, rng.uniform(-40, 40), 1, 0.6)
            n += 1


def safe_content(c):
    """Everything that matters, laid out inside the centre 1440x1080."""
    cx = W / 2
    rng = random.Random(43)
    n = 0
    while n < 12:   # a few confetti pieces inside the safe area's own edges
        x, y = rng.uniform(OX + 40, OX + SAFE_W - 40), rng.uniform(40, H - 40)
        if OX + 90 < x < OX + SAFE_W - 90 and 90 < y < H - 60:
            continue
        put(c, spr_confetti(rng.randrange(4), rng.choice((CORAL, MUSTARD, TEAL, BLUE, PLUM, ORANGE)),
                            rng.randrange(24, 46)), x, y, rng.uniform(-40, 40), 1, 0.6)
        n += 1
    step = 166
    for i, ch in enumerate('MobileGL'):
        put(c, spr_big_tile(ch, TITLE_COLS[i], 0.98), cx - step * 3.5 + step * i, 240 + (5 if i % 2 else -5),
            (3.5 if i % 2 else -3.5), 1, 1.3)
    put(c, spr_label('Desktop OpenGL, anywhere', ('fredoka', 64, 'SemiBold'), WHITE, INK, padx=44, pady=20),
        cx, 465, -1.5, 1, 1.4)
    put(c, spr_label('How it works, in 5 minutes', ('hand', 42), CORAL, WHITE, padx=28, pady=10),
        cx, 565, 2, 1, 1.4)
    y = 835
    put(c, spr_appwin(), OX + 225, y, -4, 0.5)
    put(c, spr_arrow(90, INK, 18, 40), OX + 445, y, 0, 1, 0.6)
    put(c, spr_crane(BRAND), OX + 655, y - 45, -4, 0.85, 3.5)
    put(c, spr_slab('MobileGL', 230, BRAND, h=64, size=36), OX + 655, y + 75, 2)
    put(c, spr_arrow(90, INK, 18, 40), OX + 870, y, 0, 1, 0.6)
    put(c, spr_phone(BG_PEACH), OX + 1035, y + 10, 4, 0.5)
    put(c, spr_tablet(BG_MINT), OX + 1250, y + 25, -3, 0.52)
    put(c, spr_bubble('OpenGL ES', ('fredoka', 36, 'SemiBold'), TEAL, WHITE), OX + 1020, y - 140, -3, 1, 1.5)
    put(c, spr_bubble('Vulkan', ('fredoka', 36, 'SemiBold'), PLUM, WHITE, 'br'), OX + 1285, y - 115, 3, 1, 1.5)
    put(c, spr_sticker('OpenGL\n4.6 Core', MUSTARD, r=80, size=29, fg=INK), OX + 1310, 462, 12, 1, 1.6)


def cover():
    frame = background(BG_CREAM).copy()
    c = Ctx(frame, 100.0, 0, 1)
    side_decor(c)
    safe_content(c)
    return frame.convert('RGB')


if __name__ == '__main__':
    suffix = '' if LANG == 'en' else '_' + LANG
    img = cover()
    img.save(os.path.join(OUT, f'cover{suffix}.png'))
    img.resize((1280, 720), Image.LANCZOS).save(os.path.join(OUT, f'cover{suffix}_1280x720.jpg'), quality=92)
    if '--guides' in sys.argv:   # preview with the 4:3 crop lines drawn in
        g = img.copy()
        d = ImageDraw.Draw(g)
        for x in (OX, OX + SAFE_W):
            d.line([(x, 0), (x, H)], fill=(220, 0, 0), width=4)
        g.resize((1280, 720), Image.LANCZOS).save(os.path.join(OUT, f'cover{suffix}_guides.jpg'), quality=88)
        img.crop((OX, 0, OX + SAFE_W, H)).resize((960, 720), Image.LANCZOS).save(
            os.path.join(OUT, f'cover{suffix}_crop43.jpg'), quality=88)
    print('ok')
