"""Bars 32-48: how it works. Pipeline, frontend, shaders, two backends."""
import math
from anim import *
from kit import *
from devices import *
from engine import LW, LH, hexc

STAGES = [('YOUR APP', 'OpenGL 4.6 calls'), ('FRONTEND', 'state + validation'), ('MGPIPE', 'records'),
          ('BACKEND', 'Vulkan · OpenGL ES'), ('GPU', 'pixels')]
SX = [220, 590, 960, 1330, 1700]
TOKENS = ['glDrawElements(...)', 'valid · state Δ', 'record #4217', 'vkCmdDrawIndexed', 'pixels!']


def pipeline(E, f, lb):
    plate(E, f, 'ink', drift=(30, 0))
    E.text('THE PIPELINE', 130, 150, 34, a(GLUE, 0.9), font='mono', wght=800, align='left')
    y = 520
    for i, (name, sub) in enumerate(STAGES):
        bi = 4 + i
        if lb < bi:
            continue
        u = out_back((lb - bi) / 0.35)
        hot = 0.0
        hop = (lb - 8) / 2.0
        if 0 <= hop and int(hop) == i and lb >= 8:
            hot = 1 - (hop % 1)
        iso = Iso(SX[i], y + 40, 54 * u)
        top = mix(FACE_DARK[0], scale_rgb(GLUE, 1.3), hot * 0.8)
        iso.box(E, -1.6, -1.6, 0, 3.2, 3.2, 1.4, top, FACE_DARK[1], FACE_DARK[2], OUTL, 5)
        E.text(name, SX[i], y + 210, 40, a(BONE, u), wght=800, wdth=105)
        E.text(sub, SX[i], y + 258, 22, a(BONE, 0.6 * u), font='mono')
        if i < 4 and lb >= bi + 0.3:
            arrow(E, SX[i] + 120, y + 40, SX[i + 1] - 120, y + 40, 6, a(GLUE, 0.8), u=(lb - bi - 0.3) / 0.4)
    if lb >= 8:  # the call rides a glue drop through the stages
        hop = min((lb - 8) / 2.0, 4.0)
        i = int(hop)
        fr = hop - i
        j = min(i + 1, 4)
        x = lerp(SX[i], SX[j], in_out_cubic(fr / 0.6) if fr < 0.6 else 1.0)
        yy = y - 120 - 140 * bump(min(fr / 0.6, 1.0)) - 6 * f.kick
        stage = j if fr >= 0.6 else i
        goo(E, [(x, yy, 34 + 6 * f.kick)], k=20, outline=5, bevel=20, glow=0.3)
        chip(E, x, yy - 80, TOKENS[stage], 26, INK, GLUE if stage < 4 else PALE)
        if lb >= 16 - 0.3:
            for k in range(14):
                ang = k * 2 * math.pi / 14 + f.lt
                rr = 90 + 260 * out_expo((lb - 15.7) / 0.6)
                E.rect(SX[4] + math.cos(ang) * rr, y + 40 + math.sin(ang) * rr * 0.6, 26, 26,
                       [GLUE, MINT, AMBER, CORAL, PALE][k % 5], r=4, rot=ang)


STATE = ['DEPTH_TEST on', 'BLEND off', 'CULL_FACE back', 'VAO 3', 'PROGRAM 12', 'TEX0 2D:7', 'VIEWPORT 1920×1080',
         'STENCIL off', 'FBO 2', 'SCISSOR off', 'UBO0 5', 'SSBO1 9', 'DEPTH_FUNC less', 'COLOR_MASK rgba',
         'BLEND_EQ add', 'FRONT ccw', 'SAMPLER0 4', 'TEX1 cube:2', 'DRAW_BUF 0', 'PRIM_RESTART off', 'POLYGON fill',
         'PATCH_VERTS 3', 'SRGB on', 'LINE 1.0']
ALT = ['DEPTH_TEST off', 'BLEND on', 'CULL_FACE front', 'VAO 8', 'PROGRAM 4', 'TEX0 2D:11', 'VIEWPORT 512×512',
       'STENCIL on', 'FBO 0', 'SCISSOR on', 'UBO0 2', 'SSBO1 3', 'DEPTH_FUNC lequal', 'COLOR_MASK rgb_',
       'BLEND_EQ max', 'FRONT cw', 'SAMPLER0 9', 'TEX1 cube:6', 'DRAW_BUF 1', 'PRIM_RESTART on', 'POLYGON line',
       'PATCH_VERTS 4', 'SRGB off', 'LINE 2.0']


def frontend(E, f, lb):
    plate(E, f, 'mint', drift=(0, 20))
    E.text('FRONTEND', 120, 170, 150, INK, wght=900, align='left', wdth=105)
    E.text('MG_State · MG_Impl', 130, 270, 28, a(INK, 0.7), font='mono', wght=700, align='left')
    if lb < 24:
        slam(E, f, 'TRACKS EVERY BIT OF STATE', 1800, 290, f.t - (lb - 16.5) * S.BEAT, 54, MOSS, align='right')
        step = int(lb * 4)
        for k in range(24):
            r, c = k // 4, k % 4
            x = 300 + c * 440
            yy = 420 + r * 95
            flips = sum(1 for s in range(int(16 * 4), step + 1) if rnd('flip', s) * 24 // 1 == k)
            alt = flips % 2 == 1
            recent = any(rnd('flip', s) * 24 // 1 == k for s in range(step - 1, step + 1))
            col = NEON if recent else (MOSS if alt else INK)
            E.rect(x, yy, 410, 78, a(PALE, 0.95) if not recent else a(INK, 0.95), r=12)
            E.text((ALT if alt else STATE)[k], x, yy, 26, col if recent else INK, font='mono', wght=600)
    else:
        slam(E, f, 'VALIDATES EVERY CALL', 1800, 290, f.t - (lb - 24) * S.BEAT, 54, MOSS, align='right')
        calls = ['glBindBuffer(GL_ARRAY_BUFFER, 4);', 'glBufferSubData(GL_ARRAY_BUFFER, 0, 256, data);',
                 'glUseProgram(12);', 'glUniformMatrix4fv(0, 1, GL_FALSE, mvp);', 'glBindVertexArray(3);',
                 'glBindVertexArray(999);', 'glBindTexture(GL_TEXTURE_2D, 7);',
                 'glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);']
        BAD = 5
        E.rect(960, 680, 1640, 600, INK, r=24)
        # the checker steps down the list, one call per 8th; it stops dead on the bad one
        step = (lb - 24.5) * 2
        cur = BAD if step >= BAD else int(max(step, -1))
        after = lb - (24.5 + BAD * 0.5)            # beats since the bad call was hit
        if after >= 2.0:                           # then carries on past it
            cur = min(len(calls) - 1, BAD + 1 + int((after - 2.0) * 2))
        for i, c in enumerate(calls):
            yy = 420 + i * 66
            checked = i < cur or (i == cur and (step - int(step)) > 0.5) or (i < BAD + 1 and after >= 0 and i != BAD and i < cur + 1)
            if i == BAD and after >= 0:
                shake = 10 * math.exp(-after * 6) * math.sin(after * 60)
                E.rect(960 + shake, yy, 1612, 56, a(CORAL, 0.35 + 0.35 * math.exp(-after * 3)), r=8)
                code_line(E, c, 210 + shake, yy, 30)
                cross(E, 172 + shake, yy, 15, CORAL, 5)
                continue
            if i == cur:
                E.rect(960, yy, 1612, 56, a(GLUE, 0.22), r=8, stroke=a(GLUE, 0.9), sw=3)
            code_line(E, c, 210, yy, 30, alpha=1.0 if i <= cur else 0.4)
            if i < cur or (i > BAD and i <= cur and i != cur):
                check(E, 172, yy, 16, GLUE, 5)
        if after >= 0.5:
            stamp(E, f, 'GL_INVALID_OPERATION', 1250, 420 + BAD * 66 - 8, f.t - (after - 0.5) * S.BEAT, 54, CORAL,
                  rot=-0.05)
        if after >= 1.0:
            E.text('caught in the frontend, before the driver ever sees it', 960, 1020, 30,
                   a(INK, clamp01((after - 1.0) / 0.5)), font='mono', wght=600)


GLSL = ['#version 460 core', 'layout(location = 0) in vec3 pos;', 'uniform mat4 mvp;', 'void main() {',
        '  gl_Position = mvp * vec4(pos, 1.0);', '}']
ESSL = ['#version 320 es', 'precision highp float;', 'layout(location = 0) in vec3 pos;', 'uniform mat4 mvp;',
        'void main() { ... }']
SPV = ['0x07230203', '0x00010600', '0x0008000b', '0x0000002e', '0x00000000', '0x00020011', '0x00000001',
       '0x0006000b', '0x00000001', '0x4c534c47', '0x6474732e', '0x3035342e', '0x00000000', '0x0003000e']


def shaders(E, f, lb):
    plate(E, f, 'ink')
    E.text('SHADERS', 120, 150, 120, BONE, wght=900, align='left', wdth=105)
    E.text('one compiler front, two ways out', 130, 245, 28, a(GLUE, 0.85), font='mono', wght=600, align='left')
    # GLSL panel
    E.rect(390, 640, 680, 420, INK2, r=20, stroke=a(GLUE, 0.6), sw=3)
    E.text('GLSL 4.60', 80, 465, 26, GLUE, font='mono', wght=800, align='left')
    total = sum(len(s_) for s_ in GLSL)
    n = int(total * clamp01((lb - 32) / 2.0))
    for i, s_ in enumerate(GLSL):
        before = sum(len(x) for x in GLSL[:i])
        code_line(E, s_, 80, 525 + i * 52, 25, chars=n - before)
    if lb >= 35:
        arrow(E, 745, 640, 850, 640, 7, GLUE, u=(lb - 35) / 0.3)
        chip(E, 800, 570, 'glslang', 26, INK, GLUE, sc=out_back((lb - 35) / 0.3))
        E.rect(1010, 640, 300, 420, INK2, r=20, stroke=a(GLUE, 0.6), sw=3)
        E.text('SPIR-V', 880, 465, 26, GLUE, font='mono', wght=800, align='left')
        k = int((lb - 35) * 6)
        for i in range(9):
            w = SPV[(i + k) % len(SPV)]
            E.text(w, 1010, 520 + i * 40 - ((lb * 6) % 1) * 40, 26, a(AMBER, 0.9), font='mono', wght=600)
        E.rect(1010, 452, 300, 30, INK)      # crop the scrolling words at the panel top
        E.text('SPIR-V', 880, 465, 26, GLUE, font='mono', wght=800, align='left')
    if lb >= 40:
        u1 = (lb - 40) / 0.4
        arrow(E, 1170, 560, 1290, 420, 7, GLUE, u=u1)
        arrow(E, 1170, 720, 1290, 830, 7, GLUE, u=u1)
        uu = out_back((lb - 40.3) / 0.35)
        if lb >= 40.3:
            E.rect(1580, 380, 520 * uu, 210 * uu, GLUE, r=22)
            E.text('VULKAN', 1580, 345, 70 * uu, INK, wght=900)
            E.text('SPIR-V, as-is · Magma', 1580, 420, 28 * uu, INK, font='mono', wght=700)
            E.rect(1580, 830, 520 * uu, 330 * uu, INK2, r=22, stroke=a(MINT, 0.8), sw=3)
            E.text('OPENGL ES', 1580, 710, 54 * uu, MINT, wght=900)
            for i, s_ in enumerate(ESSL):
                code_line(E, s_, 1345, 770 + i * 38, 22, alpha=uu, chars=int(len(s_) * clamp01((lb - 41) / 1.5)))
            chip(E, 1580, 1030, 'SPIRV-Cross → GLSL ES · Espryt', 24, INK, MINT, sc=uu)


def backends(E, f, lb):
    """Split screen: Magma/Vulkan vs Espryt/GLES, same cube, the switch flips each beat."""
    side = int(lb) % 2      # 0 = Vulkan active
    E.rect(480, 540, 960, 1080, INK)
    E.rect(1440, 540, 960, 1080, MINT)
    dot_grid(E, f, a(MOSS, 0.5), 60, 2.0)
    yaw = f.lt * 0.9
    sq = 0.18 * f.kick
    cubes(E, f, [(480, 520, 150, yaw, sq, 0.4 * f.kick, 0.0, 1.0)], deep=hexc('#123a18'))
    cubes(E, f, [(1440, 520, 150, yaw, sq, 0.4 * f.kick, 0.0, 1.0)], deep=hexc('#2e6e2f'))
    E.text('MAGMA', 480, 160, 120, BONE, wght=900, wdth=110)
    E.text('→ VULKAN', 480, 250, 40, GLUE, font='mono', wght=800)
    E.text('ESPRYT', 1440, 160, 120, INK, wght=900, wdth=110)
    E.text('→ OPENGL ES', 1440, 250, 40, MOSS, font='mono', wght=800)
    # dim the inactive half
    if lb < 60:
        E.rect(480 + 960 * (1 - side), 540, 960, 1080, a(INK, 0.45))
    # the switch
    E.rect(960, 870, 980, 110, INK2, r=55, stroke=a(GLUE, 0.7), sw=3)
    E.text('MOBILEGL_BACKEND_TYPE=', 560, 870, 30, BONE, font='mono', wght=700, align='left')
    E.text('DirectVulkan' if side == 0 else 'DirectGLES', 1000, 870, 30, NEON, font='mono', wght=800, align='left')
    E.text('identical frames, either way', 960, 980, 26, a(BONE, 0.7), font='mono')
    if lb >= 60:
        words = [('SAME APP.', 60), ('SAME FRAME.', 61), ('YOUR PICK.', 62)]
        k = min(int(lb - 60), 2)
        w, b = words[k]
        E.rect(960, 540, 1920, 330, a(INK, 0.85))
        slam(E, f, w, 960, 540, f.t - (lb - b) * S.BEAT, fit_size(E, w, 1500, cap=240), NEON if k == 2 else BONE)


def how(E, f):
    lb = f.lbeat
    P = {'bloom': 0.5, 'grain': 0.032, 'ca': 0.25 * f.kick}
    if lb < 4:
        plate(E, f, 'ink')
        slam(E, f, 'SO…', 960, 300, f.t - lb * S.BEAT, 200, a(BONE, 0.8))
        if lb >= 1:
            slam(E, f, 'HOW?', 960, 640, f.t - (lb - 1) * S.BEAT, 440, NEON, punch=0.3)
    elif lb < 16:
        pipeline(E, f, lb)
    elif lb < 32:
        frontend(E, f, lb)
    elif lb < 48:
        shaders(E, f, lb)
    else:
        backends(E, f, lb)
    return P
