#!/usr/bin/env python3
"""Measure one real 64 MiB buffer upload consumed by a TCP server draw.

The timed interval includes client staging, stream delivery, backend work, and
an applied fence. Its result is end-to-end staged throughput, not raw network
capacity. Start the same-build stats-enabled server before running this tool.
"""
import argparse
import ctypes as c
import json
import os
from pathlib import Path
import time

from measure_transport_cpu import digest
from measure_loopback_cpu import frame_rows
from run_tcp_matrix import actual_arm_from_text


UPLOAD_BYTES = 64 * 1024 * 1024


def run(args):
    args.out.mkdir(parents=True)
    os.environ.update(
        MOBILEGL_TRANSPORT='spawn', MOBILEGL_IPC_CONTROL=args.endpoint,
        MOBILEGL_IPC_DATA='stream', MOBILEGL_IPC_TOKEN=args.token,
        MOBILEGL_IPC_REQUIRE_SAME_BUILD='1', MOBILEGL_IPC_LOG_FORWARD='1',
        MOBILEGL_BACKEND_TYPE='DirectGLES', MOBILEGL_PIPE_STATS='1',
        MOBILEGL_PIPE_STATS_PERIOD='1', MOBILEGL_IPC_RUN_AHEAD='1',
        MOBILEGL_IPC_VERB_BARRIER='1', MOBILEGL_IPC_PRESENT_CREDIT='1',
        MOBILEGL_LOG_FILE_PATH=str(args.out / 'mobilegl.log'), EGL_PLATFORM='surfaceless')
    for name in ('MOBILEGL_IPC_DIAL', 'MOBILEGL_IPC_SERVER_PATH'):
        os.environ.pop(name, None)
    lib = c.CDLL(str(args.library), mode=c.RTLD_GLOBAL)
    P, I, U, F = c.c_void_p, c.c_int, c.c_uint, c.c_float

    def fn(name, result, parameters):
        function = getattr(lib, name)
        function.restype, function.argtypes = result, parameters
        return function

    get_display = fn('eglGetDisplay', P, [P])
    initialize = fn('eglInitialize', U, [P, c.POINTER(I), c.POINTER(I)])
    bind_api = fn('eglBindAPI', U, [U])
    choose = fn('eglChooseConfig', U, [P, c.POINTER(I), c.POINTER(P), I, c.POINTER(I)])
    context = fn('eglCreateContext', P, [P, P, P, c.POINTER(I)])
    surface = fn('eglCreatePbufferSurface', P, [P, P, c.POINTER(I)])
    current = fn('eglMakeCurrent', U, [P, P, P, P])
    swap = fn('eglSwapBuffers', U, [P, P])
    terminate = fn('eglTerminate', U, [P])
    display = get_display(None)
    major, minor = I(), I()
    if not display or not initialize(display, c.byref(major), c.byref(minor)):
        raise RuntimeError('eglInitialize failed')
    try:
        if not bind_api(0x30A2):  # EGL_OPENGL_API
            raise RuntimeError('eglBindAPI failed')
        attributes = (I * 15)(0x3033, 1, 0x3024, 8, 0x3023, 8, 0x3022, 8,
                              0x3021, 8, 0x3025, 24, 0x3040, 8, 0x3038)
        config, count = P(), I()
        if not choose(display, attributes, c.byref(config), 1, c.byref(count)) or not count.value:
            raise RuntimeError('eglChooseConfig failed')
        ctx = context(display, config, None, (I * 5)(0x3098, 3, 0x30FB, 3, 0x3038))
        surf = surface(display, config, (I * 5)(0x3057, 4, 0x3056, 4, 0x3038))
        if not ctx or not surf or not current(display, surf, surf, ctx):
            raise RuntimeError('context/surface setup failed')

        create_shader = fn('glCreateShader', U, [U])
        shader_source = fn('glShaderSource', None, [U, I, c.POINTER(c.c_char_p), c.POINTER(I)])
        compile_shader = fn('glCompileShader', None, [U])
        get_shader = fn('glGetShaderiv', None, [U, U, c.POINTER(I)])
        create_program = fn('glCreateProgram', U, [])
        attach = fn('glAttachShader', None, [U, U])
        link = fn('glLinkProgram', None, [U])
        get_program = fn('glGetProgramiv', None, [U, U, c.POINTER(I)])
        use = fn('glUseProgram', None, [U])
        error = fn('glGetError', U, [])
        program = create_program()
        for kind, source in (
            (0x8B31, b'#version 330 core\nlayout(location=0) in vec2 p;void main(){gl_Position=vec4(p,0,1);}'),
            (0x8B30, b'#version 330 core\nout vec4 c;void main(){c=vec4(1);}'),
        ):
            shader, data, ok = create_shader(kind), c.c_char_p(source), I()
            shader_source(shader, 1, c.byref(data), None)
            compile_shader(shader)
            get_shader(shader, 0x8B81, c.byref(ok))
            if not ok.value:
                raise RuntimeError('shader compilation failed')
            attach(program, shader)
        link(program)
        ok = I()
        get_program(program, 0x8B82, c.byref(ok))
        if not ok.value:
            raise RuntimeError('program link failed')
        use(program)

        gen_vao = fn('glGenVertexArrays', None, [I, c.POINTER(U)])
        bind_vao = fn('glBindVertexArray', None, [U])
        gen_buffer = fn('glGenBuffers', None, [I, c.POINTER(U)])
        bind_buffer = fn('glBindBuffer', None, [U, U])
        buffer_data = fn('glBufferData', None, [U, c.c_ssize_t, P, U])
        subdata = fn('glBufferSubData', None, [U, c.c_ssize_t, c.c_ssize_t, P])
        enable = fn('glEnableVertexAttribArray', None, [U])
        pointer = fn('glVertexAttribPointer', None, [U, I, U, c.c_ubyte, I, P])
        draw = fn('glDrawArrays', None, [U, I, I])
        finish = fn('glFinish', None, [])
        vao, buffer = U(), U()
        gen_vao(1, c.byref(vao))
        bind_vao(vao)
        gen_buffer(1, c.byref(buffer))
        bind_buffer(0x8892, buffer)
        buffer_data(0x8892, UPLOAD_BYTES, None, 0x88E8)
        vertices = (F * 6)(-1, -1, 1, -1, 0, 1)
        subdata(0x8892, 0, c.sizeof(vertices), vertices)
        enable(0)
        pointer(0, 2, 0x1406, 0, 0, None)
        draw(4, 0, 3)
        finish()
        if not swap(display, surf) or error() != 0:
            raise RuntimeError('warmup draw failed')

        payload = c.create_string_buffer(b'\x3f' * UPLOAD_BYTES)
        start = time.monotonic_ns()
        subdata(0x8892, 0, UPLOAD_BYTES, payload)
        draw(4, 0, 3)
        finish()
        elapsed = (time.monotonic_ns() - start) / 1e9
        if error() != 0 or not swap(display, surf):
            raise RuntimeError('64 MiB consuming draw failed')
        fn('MGPipeSyncPeerLog', None, [])()
    finally:
        current(display, None, None, None)
        terminate(display)

    client_path = args.out / 'mobilegl.client.log'
    proof = client_path.read_text(errors='replace')
    actual_arm = actual_arm_from_text(proof)
    if (actual_arm != {'armed': True, 'lockstep': False, 'disarmed': False}
            or 'control=tcp data=stream server=' + args.endpoint.removeprefix('tcp://') not in proof):
        raise ValueError('no continuously armed TCP runtime proof')
    frames = frame_rows(client_path, 'P65LinkMetrics ')
    if set(frames) != {1, 2} or int(frames[2]['stage_bytes']) < UPLOAD_BYTES:
        raise ValueError('frame 2 does not prove that at least 64 MiB crossed Stage')
    return {'application_upload_bytes': UPLOAD_BYTES, 'seconds': elapsed,
            'end_to_end_staged_mib_per_second': 64 / elapsed,
            'frame_2_stage_bytes': int(frames[2]['stage_bytes']),
            'actual_arm': actual_arm, 'endpoint': args.endpoint,
            'library_sha256': digest(args.library), 'script_sha256': digest(Path(__file__)),
            'client_log_sha256': digest(client_path),
            'note': 'glBufferSubData plus consuming draw and applied fence; includes client copy and backend work, not raw network capacity'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--library', required=True, type=Path)
    parser.add_argument('--endpoint', required=True)
    parser.add_argument('--token', default=os.environ.get('MOBILEGL_IPC_TOKEN', ''))
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    if not args.endpoint.startswith('tcp://'):
        parser.error('a TCP endpoint is required')
    if args.out.exists():
        parser.error('output already exists; choose a fresh evidence directory')
    result = run(args)
    (args.out / 'result.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result), flush=True)


if __name__ == '__main__':
    main()
