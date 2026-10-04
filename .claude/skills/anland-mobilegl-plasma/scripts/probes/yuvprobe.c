// yuvprobe.c - pixel check of YUV dma-buf sampling through MobileGL (GL_OES_EGL_image_external).
//
//   gcc -O1 yuvprobe.c -o /usr/local/bin/mgl-yuvprobe -lEGL -lGLESv2 -lgbm
//   mgrun offscreen mgl-yuvprobe            # exit 0 = every check passed
//
// What it does, in an ES 3.0 context on a pbuffer:
//   1. FOREIGN NV12 (and P010): a dma-buf from /dev/dma_heap/system - a buffer the MobileGL server
//      did not allocate, as a video decoder's would be - with padded pitches and a gap before the
//      chroma plane, filled with four vertical bars (red, green, blue, white) in BT.709 narrow
//      range. Imported with eglCreateImage(EGL_LINUX_DMA_BUF_EXT) and its colour hints, bound to
//      GL_TEXTURE_EXTERNAL_OES, drawn through a samplerExternalOES shader into an RGBA8 FBO and read
//      back: each bar must come out its colour. The buffer is then rewritten with the bars in
//      reverse order and, after a native fence (what ends a frame for an offscreen producer), drawn
//      again: the new order must show (the server refreshes a foreign buffer at each frame).
//   2. GBM NV12: gbm_bo_create(NV12) through the mobilegl GBM backend (a server-allocated YUV
//      buffer, zero-copy), its planes printed, imported and sampled once (content undefined; the
//      check is that every call succeeds).
// Also prints the dma-buf formats/modifiers EGL lists for NV12/P010.
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <fcntl.h>
#include <gbm.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef DRM_FORMAT_NV12
#define DRM_FORMAT_NV12 0x3231564E
#endif
#ifndef DRM_FORMAT_P010
#define DRM_FORMAT_P010 0x30313050
#endif

static int g_failures = 0;
#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL: " __VA_ARGS__);                                                                              \
            printf("\n");                                                                                              \
            ++g_failures;                                                                                              \
        }                                                                                                              \
    } while (0)

static PFNEGLCREATEIMAGEKHRPROC pCreateImage;
static PFNEGLDESTROYIMAGEKHRPROC pDestroyImage;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC pTargetTexture;
static PFNEGLQUERYDMABUFFORMATSEXTPROC pQueryFormats;
static PFNEGLQUERYDMABUFMODIFIERSEXTPROC pQueryModifiers;
static PFNEGLCREATESYNCKHRPROC pCreateSync;
static PFNEGLDESTROYSYNCKHRPROC pDestroySync;
static EGLDisplay g_display;

enum { W = 256, H = 128, BARS = 4 };
static const unsigned char kBars[BARS][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}};

// RGB -> BT.709 narrow-range codes at `bits` (8 or 10).
static void Encode709(const unsigned char rgb[3], int bits, int out[3]) {
    const double kr = 0.2126, kb = 0.0722, kg = 1.0 - kr - kb;
    const double r = rgb[0] / 255.0, g = rgb[1] / 255.0, b = rgb[2] / 255.0;
    const double y = kr * r + kg * g + kb * b;
    const double cb = (b - y) / (2.0 * (1.0 - kb)), cr = (r - y) / (2.0 * (1.0 - kr));
    const double scale = (double)(1 << (bits - 8));
    out[0] = (int)lround((16.0 + 219.0 * y) * scale);
    out[1] = (int)lround((128.0 + 224.0 * cb) * scale);
    out[2] = (int)lround((128.0 + 224.0 * cr) * scale);
}

// Writes the bars (`reverse`d or not) into a semi-planar buffer: 1 byte samples, or P010's 16-bit
// samples with the 10 significant bits high.
static void FillBars(uint8_t* base, uint32_t pitchY, uint32_t offsetC, uint32_t pitchC, int bytes, int reverse) {
    const int bits = bytes == 1 ? 8 : 10;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            int bar = x * BARS / W;
            if (reverse) bar = BARS - 1 - bar;
            int code[3];
            Encode709(kBars[bar], bits, code);
            uint8_t* py = base + (size_t)y * pitchY + (size_t)x * bytes;
            if (bytes == 1) py[0] = (uint8_t)code[0];
            else *(uint16_t*)py = (uint16_t)(code[0] << 6);
            if ((x & 1) == 0 && (y & 1) == 0) {
                uint8_t* pc = base + offsetC + (size_t)(y / 2) * pitchC + (size_t)(x / 2) * 2 * bytes;
                if (bytes == 1) {
                    pc[0] = (uint8_t)code[1];
                    pc[1] = (uint8_t)code[2];
                } else {
                    ((uint16_t*)pc)[0] = (uint16_t)(code[1] << 6);
                    ((uint16_t*)pc)[1] = (uint16_t)(code[2] << 6);
                }
            }
        }
    }
}

static GLuint Compile(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        printf("shader: %s\n", log);
    }
    return shader;
}

static GLuint g_program, g_fbo, g_rgba;

static void SetUpDraw(void) {
    const char* vs = "#version 300 es\n"
                     "out vec2 uv;\n"
                     "void main() {\n"
                     "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
                     "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
                     "  uv = p;\n"
                     "}\n";
    const char* fs = "#version 300 es\n"
                     "#extension GL_OES_EGL_image_external_essl3 : require\n"
                     "precision mediump float;\n"
                     "uniform samplerExternalOES tex;\n"
                     "in vec2 uv;\n"
                     "out vec4 color;\n"
                     "void main() { color = texture(tex, uv); }\n";
    g_program = glCreateProgram();
    glAttachShader(g_program, Compile(GL_VERTEX_SHADER, vs));
    glAttachShader(g_program, Compile(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(g_program);
    GLint linked = 0;
    glGetProgramiv(g_program, GL_LINK_STATUS, &linked);
    CHECK(linked, "the samplerExternalOES program did not link");
    GLint type = 0, size = 0;
    char name[64];
    glGetActiveUniform(g_program, 0, sizeof(name), NULL, &size, (GLenum*)&type, name);
    printf("uniform 0: %s type 0x%04x\n", name, type);
    CHECK(type == GL_SAMPLER_EXTERNAL_OES, "the sampler reports type 0x%04x, not GL_SAMPLER_EXTERNAL_OES", type);
    glGenTextures(1, &g_rgba);
    glBindTexture(GL_TEXTURE_2D, g_rgba);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, W, H);
    glGenFramebuffers(1, &g_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_rgba, 0);
    CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "the readback FBO is incomplete");
    GLuint vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
}

// Draws the external texture `texture` and checks each bar's center (row `y` of the image).
static void DrawAndCheck(GLuint texture, const char* what, int reverse, int tolerance) {
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glViewport(0, 0, W, H);
    glUseProgram(g_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
    glUniform1i(glGetUniformLocation(g_program, "tex"), 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    static unsigned char pixels[W * H * 4];
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    GLenum error = glGetError();
    CHECK(error == GL_NO_ERROR, "%s: GL error 0x%04x", what, error);
    if (tolerance < 0) {
        printf("%s: center pixel %d %d %d %d (content undefined)\n", what, pixels[(H / 2 * W + W / 2) * 4],
               pixels[(H / 2 * W + W / 2) * 4 + 1], pixels[(H / 2 * W + W / 2) * 4 + 2],
               pixels[(H / 2 * W + W / 2) * 4 + 3]);
        return;
    }
    // Rows: GL row 0 is the image's first row (the uv mapping above), and the bars are vertical,
    // so any row does; take two to catch a broken chroma row pitch.
    const int rows[2] = {H / 4, (3 * H) / 4};
    for (int r = 0; r < 2; ++r) {
        for (int bar = 0; bar < BARS; ++bar) {
            const int x = bar * (W / BARS) + W / (2 * BARS);
            const unsigned char* p = &pixels[(rows[r] * W + x) * 4];
            const unsigned char* want = kBars[reverse ? BARS - 1 - bar : bar];
            int worst = 0;
            for (int c = 0; c < 3; ++c) {
                int d = abs((int)p[c] - (int)want[c]);
                if (d > worst) worst = d;
            }
            printf("%s: row %d bar %d -> %3d %3d %3d %3d (want %3d %3d %3d)%s\n", what, rows[r], bar, p[0], p[1], p[2],
                   p[3], want[0], want[1], want[2], worst > tolerance ? "  <-- off" : "");
            CHECK(worst <= tolerance, "%s: bar %d is off by %d", what, bar, worst);
        }
    }
}

static int AllocateDmaHeap(size_t size) {
    int heap = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
    if (heap < 0) {
        perror("open /dev/dma_heap/system");
        return -1;
    }
    struct dma_heap_allocation_data data = {.len = size, .fd_flags = O_RDWR | O_CLOEXEC};
    if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &data) != 0) {
        perror("DMA_HEAP_IOCTL_ALLOC");
        close(heap);
        return -1;
    }
    close(heap);
    return (int)data.fd;
}

static void Sync(int fd, int start) {
    struct dma_buf_sync sync = {.flags = DMA_BUF_SYNC_WRITE | (start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END)};
    ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

static EGLImageKHR Import(int fd, uint32_t fourcc, uint32_t pitchY, uint32_t offsetC, uint32_t pitchC, int modifier) {
    EGLint attribs[64];
    int n = 0;
#define A(k, v) (attribs[n++] = (k), attribs[n++] = (EGLint)(v))
    A(EGL_WIDTH, W);
    A(EGL_HEIGHT, H);
    A(EGL_LINUX_DRM_FOURCC_EXT, fourcc);
    A(EGL_DMA_BUF_PLANE0_FD_EXT, fd);
    A(EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0);
    A(EGL_DMA_BUF_PLANE0_PITCH_EXT, pitchY);
    A(EGL_DMA_BUF_PLANE1_FD_EXT, fd);
    A(EGL_DMA_BUF_PLANE1_OFFSET_EXT, offsetC);
    A(EGL_DMA_BUF_PLANE1_PITCH_EXT, pitchC);
    if (modifier) {
        A(EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, 0);
        A(EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, 0);
        A(EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, 0);
        A(EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT, 0);
    }
    A(EGL_YUV_COLOR_SPACE_HINT_EXT, EGL_ITU_REC709_EXT);
    A(EGL_SAMPLE_RANGE_HINT_EXT, EGL_YUV_NARROW_RANGE_EXT);
    attribs[n] = EGL_NONE;
#undef A
    EGLImageKHR image = pCreateImage(g_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
    if (image == EGL_NO_IMAGE_KHR) printf("eglCreateImage failed: 0x%04x\n", eglGetError());
    return image;
}

static GLuint ExternalTexture(EGLImageKHR image) {
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
    pTargetTexture(GL_TEXTURE_EXTERNAL_OES, image);
    GLenum error = glGetError();
    CHECK(error == GL_NO_ERROR, "glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES) -> 0x%04x", error);
    GLint bound = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_EXTERNAL_OES, &bound);
    CHECK((GLuint)bound == texture, "GL_TEXTURE_BINDING_EXTERNAL_OES reads %d", bound);
    return texture;
}

// A frame boundary as an offscreen producer makes one: a native fence.
static void EndFrame(void) {
    EGLint attribs[] = {EGL_NONE};
    EGLSyncKHR sync = pCreateSync(g_display, EGL_SYNC_NATIVE_FENCE_ANDROID, attribs);
    CHECK(sync != EGL_NO_SYNC_KHR, "eglCreateSync(native fence) failed");
    if (sync != EGL_NO_SYNC_KHR) pDestroySync(g_display, sync);
}

static void ForeignTest(uint32_t fourcc, int bytes, int tolerance) {
    const char* what = fourcc == DRM_FORMAT_NV12 ? "foreign NV12" : "foreign P010";
    // Padded pitches and a gap before the chroma plane, as decoders lay buffers out.
    const uint32_t pitchY = W * bytes + 64, pitchC = W * bytes + 128;
    const uint32_t offsetC = pitchY * (H + 16);
    const size_t size = offsetC + (size_t)pitchC * (H / 2) + 4096;
    int fd = AllocateDmaHeap(size);
    if (fd < 0) {
        CHECK(0, "%s: no dma-buf to test with", what);
        return;
    }
    uint8_t* map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(map != MAP_FAILED, "%s: mmap failed", what);
    if (map == MAP_FAILED) return;
    Sync(fd, 1);
    FillBars(map, pitchY, offsetC, pitchC, bytes, 0);
    Sync(fd, 0);
    EGLImageKHR image = Import(fd, fourcc, pitchY, offsetC, pitchC, fourcc == DRM_FORMAT_NV12);
    CHECK(image != EGL_NO_IMAGE_KHR, "%s: import refused", what);
    if (image == EGL_NO_IMAGE_KHR) return;
    GLuint texture = ExternalTexture(image);
    DrawAndCheck(texture, what, 0, tolerance);
    EndFrame();
    // New content, same buffer: shows after the frame boundary.
    Sync(fd, 1);
    FillBars(map, pitchY, offsetC, pitchC, bytes, 1);
    Sync(fd, 0);
    EndFrame();
    DrawAndCheck(texture, what, 1, tolerance);
    glDeleteTextures(1, &texture);
    pDestroyImage(g_display, image);
    munmap(map, size);
    close(fd);
}

static void GbmTest(void) {
    int node = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (node < 0) node = open("/dev/null", O_RDWR | O_CLOEXEC);
    struct gbm_device* gbm = gbm_create_device(node);
    if (gbm == NULL) {
        CHECK(0, "gbm_create_device failed");
        return;
    }
    printf("gbm backend: %s\n", gbm_device_get_backend_name(gbm));
    const int supported = gbm_device_is_format_supported(gbm, DRM_FORMAT_NV12, 0);
    printf("gbm NV12 supported: %d, P010: %d\n", supported,
           gbm_device_is_format_supported(gbm, DRM_FORMAT_P010, 0));
    CHECK(supported == 1, "gbm does not support NV12");
    struct gbm_bo* bo = gbm_bo_create(gbm, W, H, DRM_FORMAT_NV12, 0);
    CHECK(bo != NULL, "gbm_bo_create(NV12) failed");
    if (bo != NULL) {
        const int planes = gbm_bo_get_plane_count(bo);
        printf("gbm NV12 bo: planes %d, stride %u/%u, offset %u/%u, modifier 0x%llx\n", planes,
               gbm_bo_get_stride_for_plane(bo, 0), gbm_bo_get_stride_for_plane(bo, 1), gbm_bo_get_offset(bo, 0),
               gbm_bo_get_offset(bo, 1), (unsigned long long)gbm_bo_get_modifier(bo));
        CHECK(planes == 2, "the NV12 bo has %d planes", planes);
        int fd = gbm_bo_get_fd(bo);
        EGLImageKHR image =
            Import(fd, DRM_FORMAT_NV12, gbm_bo_get_stride_for_plane(bo, 0), gbm_bo_get_offset(bo, 1),
                   gbm_bo_get_stride_for_plane(bo, 1), 0);
        CHECK(image != EGL_NO_IMAGE_KHR, "the server's own NV12 buffer was not imported");
        if (image != EGL_NO_IMAGE_KHR) {
            GLuint texture = ExternalTexture(image);
            DrawAndCheck(texture, "gbm NV12", 0, -1);
            glDeleteTextures(1, &texture);
            pDestroyImage(g_display, image);
        }
        close(fd);
        gbm_bo_destroy(bo);
    }
    gbm_device_destroy(gbm);
    close(node);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major, minor;
    if (!eglInitialize(g_display, &major, &minor)) {
        printf("FAIL: eglInitialize\n");
        return 2;
    }
    const char* eglExtensions = eglQueryString(g_display, EGL_EXTENSIONS);
    printf("EGL %d.%d dma_buf_import: %s\n", major, minor,
           strstr(eglExtensions, "EGL_EXT_image_dma_buf_import") ? "yes" : "no");
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint configAttribs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                                    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
    EGLConfig config;
    EGLint count = 0;
    eglChooseConfig(g_display, configAttribs, &config, 1, &count);
    if (count < 1) {
        printf("FAIL: no config\n");
        return 2;
    }
    const EGLint pbufferAttribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(g_display, config, pbufferAttribs);
    const EGLint contextAttribs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
    EGLContext context = eglCreateContext(g_display, config, EGL_NO_CONTEXT, contextAttribs);
    if (context == EGL_NO_CONTEXT || !eglMakeCurrent(g_display, surface, surface, context)) {
        printf("FAIL: no ES 3 context\n");
        return 2;
    }
    pCreateImage = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    pDestroyImage = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    pTargetTexture = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    pQueryFormats = (PFNEGLQUERYDMABUFFORMATSEXTPROC)eglGetProcAddress("eglQueryDmaBufFormatsEXT");
    pQueryModifiers = (PFNEGLQUERYDMABUFMODIFIERSEXTPROC)eglGetProcAddress("eglQueryDmaBufModifiersEXT");
    pCreateSync = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
    pDestroySync = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
    printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));
    const char* glExtensions = (const char*)glGetString(GL_EXTENSIONS);
    const int external = strstr(glExtensions, "GL_OES_EGL_image_external_essl3") != NULL;
    printf("GL_OES_EGL_image_external_essl3: %s\n", external ? "yes" : "no");
    CHECK(external, "GL_OES_EGL_image_external_essl3 is not advertised");

    EGLint formats[32], formatCount = 0;
    pQueryFormats(g_display, 32, formats, &formatCount);
    int nv12 = 0;
    for (int i = 0; i < formatCount; ++i) {
        printf("dma-buf format %.4s", (const char*)&formats[i]);
        EGLuint64KHR modifiers[4];
        EGLBoolean externalOnly[4];
        EGLint modifierCount = 0;
        pQueryModifiers(g_display, formats[i], 4, modifiers, externalOnly, &modifierCount);
        for (int m = 0; m < modifierCount; ++m)
            printf(" [modifier 0x%llx%s]", (unsigned long long)modifiers[m], externalOnly[m] ? " external-only" : "");
        printf("\n");
        nv12 |= formats[i] == (EGLint)DRM_FORMAT_NV12;
    }
    CHECK(nv12, "NV12 is not a listed dma-buf format");

    SetUpDraw();
    ForeignTest(DRM_FORMAT_NV12, 1, 12);
    ForeignTest(DRM_FORMAT_P010, 2, 16);
    GbmTest();

    printf("%s (%d failure(s))\n", g_failures ? "FAILED" : "PASSED", g_failures);
    eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglTerminate(g_display);
    return g_failures ? 1 : 0;
}
