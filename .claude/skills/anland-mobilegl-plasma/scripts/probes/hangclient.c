// hangclient - a Wayland xdg-toplevel, EGL + desktop GL 3.3 core client that animates and, on request,
// submits fragment work that never ends (the driver has to reset it).  Prints per-frame gaps so the same
// binary measures "other app" frame pacing (frame callbacks come from the compositor) when run with -H -1.
//
//   hangclient -T tag [-H hang_at_frame] [-N hang_repeat] [-I iters] [-S seconds] [-W w] [-X h] [-F]
//     -H n   frame at which the hanging draw is submitted (-1: never; default -1)
//     -N k   how many consecutive hanging frames (default 1)
//     -E e   re-arm a hang every e frames after the first (0: once)
//     -I i   loop count of the hanging shader (default 2^31-1)
//     -S s   run time in seconds (default 30)
//     -F     glFinish after every frame (puts the wait inside the client's own session)
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include "xdg-shell-client-protocol.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static struct wl_compositor* g_comp;
static struct xdg_wm_base* g_wm;
static int g_configured, g_closed;
static int g_w = 480, g_h = 360;

static void reg_global(void* d, struct wl_registry* r, uint32_t name, const char* iface, uint32_t ver) {
    (void)d;
    if (!strcmp(iface, "wl_compositor")) g_comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, "xdg_wm_base")) g_wm = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
}
static void reg_remove(void* d, struct wl_registry* r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = {reg_global, reg_remove};
static void wm_ping(void* d, struct xdg_wm_base* wm, uint32_t s) { (void)d; xdg_wm_base_pong(wm, s); }
static const struct xdg_wm_base_listener wm_l = {wm_ping};
static void xs_conf(void* d, struct xdg_surface* s, uint32_t serial) { (void)d; xdg_surface_ack_configure(s, serial); g_configured = 1; }
static const struct xdg_surface_listener xs_l = {xs_conf};
static void tl_conf(void* d, struct xdg_toplevel* t, int32_t w, int32_t h, struct wl_array* st) { (void)d; (void)t; (void)st; (void)w; (void)h; }
static void tl_close(void* d, struct xdg_toplevel* t) { (void)d; (void)t; g_closed = 1; }
static const struct xdg_toplevel_listener tl_l = {tl_conf, tl_close};

static double now_ms(void) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6; }

static GLuint sh(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[2048]; glGetShaderInfoLog(s, sizeof log, NULL, log); fprintf(stderr, "shader: %s\n", log); exit(2); }
    return s;
}

int main(int argc, char** argv) {
    const char* tag = "hc"; int hangAt = -1, hangN = 1, every = 0, seconds = 30, finish = 0; long iters = 2147483647L;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-T") && i + 1 < argc) tag = argv[++i];
        else if (!strcmp(argv[i], "-H") && i + 1 < argc) hangAt = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-N") && i + 1 < argc) hangN = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-E") && i + 1 < argc) every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-I") && i + 1 < argc) iters = atol(argv[++i]);
        else if (!strcmp(argv[i], "-S") && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-W") && i + 1 < argc) g_w = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-X") && i + 1 < argc) g_h = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-F")) finish = 1;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct wl_display* dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "no wayland\n"); return 1; }
    struct wl_registry* reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_l, NULL);
    wl_display_roundtrip(dpy);
    if (!g_comp || !g_wm) { fprintf(stderr, "no compositor/xdg_wm_base\n"); return 1; }
    xdg_wm_base_add_listener(g_wm, &wm_l, NULL);
    struct wl_surface* surf = wl_compositor_create_surface(g_comp);
    struct xdg_surface* xs = xdg_wm_base_get_xdg_surface(g_wm, surf);
    xdg_surface_add_listener(xs, &xs_l, NULL);
    struct xdg_toplevel* tl = xdg_surface_get_toplevel(xs);
    xdg_toplevel_add_listener(tl, &tl_l, NULL);
    xdg_toplevel_set_title(tl, tag);
    wl_surface_commit(surf);
    while (!g_configured) wl_display_dispatch(dpy);

    EGLDisplay ed = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, dpy, NULL);
    if (!eglInitialize(ed, NULL, NULL)) { fprintf(stderr, "eglInitialize\n"); return 1; }
    eglBindAPI(EGL_OPENGL_API);
    const EGLint ca[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
    EGLConfig cfg; EGLint n = 0;
    if (!eglChooseConfig(ed, ca, &cfg, 1, &n) || n < 1) { fprintf(stderr, "no config\n"); return 1; }
    const EGLint xa[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
    EGLContext ctx = eglCreateContext(ed, cfg, EGL_NO_CONTEXT, xa);
    if (ctx == EGL_NO_CONTEXT) { fprintf(stderr, "no context 0x%x\n", eglGetError()); return 1; }
    struct wl_egl_window* ew = wl_egl_window_create(surf, g_w, g_h);
    EGLSurface es = eglCreatePlatformWindowSurface(ed, cfg, ew, NULL);
    eglMakeCurrent(ed, es, es, ctx);
    eglSwapInterval(ed, 1);

    const char* vs = "#version 330 core\nlayout(location=0) in vec2 p; uniform float t; out vec2 uv;\n"
                     "void main(){ uv=p*0.5+0.5; gl_Position=vec4(p*0.4+vec2(0.5*sin(t),0.0),0.0,1.0);}";
    const char* fs = "#version 330 core\nin vec2 uv; uniform int iters; uniform float t; out vec4 c;\n"
                     "void main(){ float a=uv.x; for(int i=0;i<iters;++i){ a=fract(a*1.0001+0.37+uv.y); }\n"
                     " c=vec4(uv, 0.5+0.5*sin(t)+a*1e-6, 1.0);}";
    GLuint prog = glCreateProgram();
    glAttachShader(prog, sh(GL_VERTEX_SHADER, vs));
    glAttachShader(prog, sh(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(prog);
    GLint ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) { fprintf(stderr, "link failed\n"); return 2; }
    GLint uIt = glGetUniformLocation(prog, "iters"), uT = glGetUniformLocation(prog, "t");
    GLuint vao, vbo; glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    const float q[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo); glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);

    const double t0 = now_ms(); double last = t0, maxGap = 0; long frames = 0, gaps100 = 0;
    printf("[%s] start %.0f renderer=%s hangAt=%d N=%d every=%d iters=%ld\n", tag, t0, glGetString(GL_RENDERER), hangAt, hangN, every, iters);
    while (!g_closed && now_ms() - t0 < seconds * 1000.0) {
        wl_display_dispatch_pending(dpy);
        const double t = (now_ms() - t0) / 1000.0;
        int hang = 0;
        if (hangAt >= 0 && frames >= hangAt) {
            long rel = frames - hangAt;
            if (every > 0) rel %= every;
            hang = rel < hangN;
        }
        glViewport(0, 0, g_w, g_h);
        glClearColor(0.5f + 0.5f * (float)sin(t * 3.0), 0.2f, 0.4f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(prog);
        glUniform1i(uIt, hang ? (int)iters : 0);
        glUniform1f(uT, (float)t);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        if (hang) printf("[%s] HANG submitted frame %ld at %.0f\n", tag, frames, now_ms());
        if (finish) glFinish();
        if (!eglSwapBuffers(ed, es)) printf("[%s] swap failed 0x%x at %.0f\n", tag, eglGetError(), now_ms());
        const GLenum rs = glGetError();
        if (rs != GL_NO_ERROR) printf("[%s] glGetError 0x%x at %.0f\n", tag, rs, now_ms());
        const double nowv = now_ms(), gap = nowv - last;
        last = nowv; ++frames;
        if (gap > maxGap) maxGap = gap;
        if (gap > 100.0) { ++gaps100; printf("[%s] GAP %.0f ms ending at %.0f (frame %ld)\n", tag, gap, nowv, frames); }
        if (frames % 120 == 0) printf("[%s] frame %ld t=%.1fs fps=%.1f\n", tag, frames, (nowv - t0) / 1e3, frames * 1e3 / (nowv - t0));
    }
    const double total = now_ms() - t0;
    printf("[%s] END frames=%ld fps=%.1f maxGap=%.0f ms gaps>100ms=%ld\n", tag, frames, frames * 1e3 / total, maxGap, gaps100);
    return 0;
}
