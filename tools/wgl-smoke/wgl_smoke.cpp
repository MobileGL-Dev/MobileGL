// WGL smoke test for the MobileGL opengl32.dll drop-in.
// Replicates GLFW 3.3.3's WGL bootstrap exactly as Minecraft 1.21.4 drives it:
//   1. dummy window: gdi32 ChoosePixelFormat/SetPixelFormat -> wglCreateContext
//      -> wglMakeCurrent -> probe wglGetProcAddress/extension strings
//      -> wglMakeCurrent(NULL) -> wglDeleteContext
//   2. real window: SetPixelFormat -> wglCreateContextAttribsARB (3.2 core,
//      forward-compatible) -> wglMakeCurrent -> render loop with SwapBuffers
// Verifies rendering via glReadPixels. Exit code 0 = pass.
//
// Build: cl /nologo /EHsc /W3 wgl_smoke.cpp user32.lib gdi32.lib
// Run with opengl32.dll (MobileGL) in the exe directory.

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef float GLfloat;
typedef unsigned char GLubyte;
typedef char GLchar;
typedef signed long long GLsizeiptr;
typedef unsigned int GLbitfield;

#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_TRIANGLES 0x0004
#define GL_FLOAT 0x1406
#define GL_FALSE 0
#define GL_ARRAY_BUFFER 0x8892
#define GL_STATIC_DRAW 0x88E4
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_MAJOR_VERSION 0x821B
#define GL_MINOR_VERSION 0x821C
#define GL_NO_ERROR 0

#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_FLAGS_ARB 0x2094
#define WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB 0x0002
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001
#define WGL_CONTEXT_OPENGL_NO_ERROR_ARB 0x31B3
#define WGL_NUMBER_PIXEL_FORMATS_ARB 0x2000
#define WGL_SUPPORT_OPENGL_ARB 0x2010
#define WGL_DRAW_TO_WINDOW_ARB 0x2001
#define WGL_PIXEL_TYPE_ARB 0x2013
#define WGL_TYPE_RGBA_ARB 0x202B
#define WGL_DEPTH_BITS_ARB 0x2022
#define WGL_STENCIL_BITS_ARB 0x2023
#define WGL_DOUBLE_BUFFER_ARB 0x2011

static HMODULE g_opengl32;

typedef HGLRC(WINAPI* PFN_wglCreateContext)(HDC);
typedef BOOL(WINAPI* PFN_wglDeleteContext)(HGLRC);
typedef PROC(WINAPI* PFN_wglGetProcAddress)(LPCSTR);
typedef BOOL(WINAPI* PFN_wglMakeCurrent)(HDC, HGLRC);
typedef HGLRC(WINAPI* PFN_wglGetCurrentContext)(void);
typedef HDC(WINAPI* PFN_wglGetCurrentDC)(void);

static PFN_wglCreateContext p_wglCreateContext;
static PFN_wglDeleteContext p_wglDeleteContext;
static PFN_wglGetProcAddress p_wglGetProcAddress;
static PFN_wglMakeCurrent p_wglMakeCurrent;
static PFN_wglGetCurrentContext p_wglGetCurrentContext;
static PFN_wglGetCurrentDC p_wglGetCurrentDC;

typedef HGLRC(WINAPI* PFN_wglCreateContextAttribsARB)(HDC, HGLRC, const int*);
typedef const char*(WINAPI* PFN_wglGetExtensionsStringARB)(HDC);
typedef BOOL(WINAPI* PFN_wglSwapIntervalEXT)(int);
typedef BOOL(WINAPI* PFN_wglGetPixelFormatAttribivARB)(HDC, int, int, UINT, const int*, int*);

// GL function pointers, loaded LWJGL-style: wglGetProcAddress first, then
// GetProcAddress on the opengl32 module.
static void* GetGLProc(const char* name) {
    PROC proc = p_wglGetProcAddress(name);
    if (!proc) {
        proc = ::GetProcAddress(g_opengl32, name);
    }
    return reinterpret_cast<void*>(proc);
}

typedef const GLubyte*(WINAPI* PFN_glGetString)(GLenum);
typedef void(WINAPI* PFN_glGetIntegerv)(GLenum, GLint*);
typedef void(WINAPI* PFN_glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void(WINAPI* PFN_glClear)(GLbitfield);
typedef void(WINAPI* PFN_glViewport)(GLint, GLint, GLsizei, GLsizei);
typedef GLenum(WINAPI* PFN_glGetError)(void);
typedef void(WINAPI* PFN_glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
typedef void(WINAPI* PFN_glFinish)(void);
typedef GLuint(WINAPI* PFN_glCreateShader)(GLenum);
typedef void(WINAPI* PFN_glShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*);
typedef void(WINAPI* PFN_glCompileShader)(GLuint);
typedef void(WINAPI* PFN_glGetShaderiv)(GLuint, GLenum, GLint*);
typedef void(WINAPI* PFN_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
typedef GLuint(WINAPI* PFN_glCreateProgram)(void);
typedef void(WINAPI* PFN_glAttachShader)(GLuint, GLuint);
typedef void(WINAPI* PFN_glLinkProgram)(GLuint);
typedef void(WINAPI* PFN_glGetProgramiv)(GLuint, GLenum, GLint*);
typedef void(WINAPI* PFN_glGetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
typedef void(WINAPI* PFN_glUseProgram)(GLuint);
typedef void(WINAPI* PFN_glGenVertexArrays)(GLsizei, GLuint*);
typedef void(WINAPI* PFN_glBindVertexArray)(GLuint);
typedef void(WINAPI* PFN_glGenBuffers)(GLsizei, GLuint*);
typedef void(WINAPI* PFN_glBindBuffer)(GLenum, GLuint);
typedef void(WINAPI* PFN_glBufferData)(GLenum, GLsizeiptr, const void*, GLenum);
typedef void(WINAPI* PFN_glVertexAttribPointer)(GLuint, GLint, GLenum, unsigned char, GLsizei, const void*);
typedef void(WINAPI* PFN_glEnableVertexAttribArray)(GLuint);
typedef void(WINAPI* PFN_glDrawArrays)(GLenum, GLint, GLsizei);

static PFN_glGetString glGetString_;
static PFN_glGetIntegerv glGetIntegerv_;
static PFN_glClearColor glClearColor_;
static PFN_glClear glClear_;
static PFN_glViewport glViewport_;
static PFN_glGetError glGetError_;
static PFN_glReadPixels glReadPixels_;
static PFN_glFinish glFinish_;
static PFN_glCreateShader glCreateShader_;
static PFN_glShaderSource glShaderSource_;
static PFN_glCompileShader glCompileShader_;
static PFN_glGetShaderiv glGetShaderiv_;
static PFN_glGetShaderInfoLog glGetShaderInfoLog_;
static PFN_glCreateProgram glCreateProgram_;
static PFN_glAttachShader glAttachShader_;
static PFN_glLinkProgram glLinkProgram_;
static PFN_glGetProgramiv glGetProgramiv_;
static PFN_glGetProgramInfoLog glGetProgramInfoLog_;
static PFN_glUseProgram glUseProgram_;
static PFN_glGenVertexArrays glGenVertexArrays_;
static PFN_glBindVertexArray glBindVertexArray_;
static PFN_glGenBuffers glGenBuffers_;
static PFN_glBindBuffer glBindBuffer_;
static PFN_glBufferData glBufferData_;
static PFN_glVertexAttribPointer glVertexAttribPointer_;
static PFN_glEnableVertexAttribArray glEnableVertexAttribArray_;
static PFN_glDrawArrays glDrawArrays_;

#define FAIL(...)                                                                                                      \
    do {                                                                                                               \
        std::fprintf(stderr, "SMOKE FAIL: " __VA_ARGS__);                                                              \
        std::fprintf(stderr, "\n");                                                                                    \
        return 1;                                                                                                      \
    } while (0)

// CRT-free crash dumper: runs during process teardown when stdio is already
// dead, so it may only touch raw Win32.
static void RawPrint(HANDLE f, const char* s) {
    DWORD written = 0;
    WriteFile(f, s, (DWORD)lstrlenA(s), &written, nullptr);
}

static void RawHex(HANDLE f, DWORD64 v) {
    char buf[20];
    for (int i = 0; i < 16; ++i) {
        int nib = (int)((v >> ((15 - i) * 4)) & 0xF);
        buf[i] = (char)(nib < 10 ? '0' + nib : 'a' + nib - 10);
    }
    buf[16] = 0;
    RawPrint(f, buf);
}

static LONG WINAPI CrashDump(EXCEPTION_POINTERS* ep) {
    static volatile LONG entered = 0;
    if (ep->ExceptionRecord->ExceptionCode != 0xC0000005) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedExchange(&entered, 1)) return EXCEPTION_CONTINUE_SEARCH;
    HANDLE f = CreateFileA(
        "C:\\Users\\yello\\AndroidStudioProjects\\FoldCraftLauncher\\MobileGL\\.claude\\worktrees\\wgl-host\\wgl-smoke\\crash-stack.txt",
        GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        TerminateProcess(GetCurrentProcess(), 0x43);
    }
    RawPrint(f, "AV at ");
    RawHex(f, (DWORD64)ep->ExceptionRecord->ExceptionAddress);
    RawPrint(f, "\n");
    const DWORD64* sp = reinterpret_cast<const DWORD64*>(ep->ContextRecord->Rsp);
    for (int i = 0; i < 512; ++i) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(sp + i, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) == 0)
            break;
        DWORD64 v = sp[i];
        HMODULE mod = nullptr;
        if (v > 0x10000 &&
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(v), &mod) &&
            mod) {
            char name[MAX_PATH];
            name[0] = 0;
            GetModuleFileNameA(mod, name, MAX_PATH);
            RawPrint(f, "stack ");
            RawHex(f, v);
            RawPrint(f, " ");
            RawPrint(f, name);
            RawPrint(f, "+");
            RawHex(f, v - (DWORD64)mod);
            RawPrint(f, "\n");
        }
    }
    CloseHandle(f);
    TerminateProcess(GetCurrentProcess(), 0x42);
    return EXCEPTION_EXECUTE_HANDLER;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static HWND MakeWindow(const char* title, int w, int h, bool visible) {
    WNDCLASSA wc{};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "MGWGLSmoke";
    static bool registered = false;
    if (!registered) {
        RegisterClassA(&wc);
        registered = true;
    }
    DWORD style = WS_OVERLAPPEDWINDOW | (visible ? WS_VISIBLE : 0);
    RECT rect{0, 0, w, h};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    return CreateWindowExA(0, "MGWGLSmoke", title, style, CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left,
                           rect.bottom - rect.top, nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
}

int main() {
    AddVectoredExceptionHandler(1, CrashDump);
    // Load exactly like GLFW: the app-dir opengl32.dll must win the search.
    g_opengl32 = LoadLibraryA("opengl32.dll");
    if (!g_opengl32) FAIL("LoadLibraryA(opengl32.dll) failed");
    char modulePath[MAX_PATH]{};
    GetModuleFileNameA(g_opengl32, modulePath, MAX_PATH);
    std::printf("opengl32.dll -> %s\n", modulePath);

    p_wglCreateContext = (PFN_wglCreateContext)::GetProcAddress(g_opengl32, "wglCreateContext");
    p_wglDeleteContext = (PFN_wglDeleteContext)::GetProcAddress(g_opengl32, "wglDeleteContext");
    p_wglGetProcAddress = (PFN_wglGetProcAddress)::GetProcAddress(g_opengl32, "wglGetProcAddress");
    p_wglMakeCurrent = (PFN_wglMakeCurrent)::GetProcAddress(g_opengl32, "wglMakeCurrent");
    p_wglGetCurrentContext = (PFN_wglGetCurrentContext)::GetProcAddress(g_opengl32, "wglGetCurrentContext");
    p_wglGetCurrentDC = (PFN_wglGetCurrentDC)::GetProcAddress(g_opengl32, "wglGetCurrentDC");
    if (!p_wglCreateContext || !p_wglDeleteContext || !p_wglGetProcAddress || !p_wglMakeCurrent ||
        !p_wglGetCurrentContext || !p_wglGetCurrentDC)
        FAIL("missing classic wgl exports");

    // ---- Phase 1: GLFW-style dummy context on a hidden helper window ----
    HWND helper = MakeWindow("helper", 1, 1, false);
    if (!helper) FAIL("helper window creation failed");
    HDC helperDC = GetDC(helper);

    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 24;

    int dummyFormat = ChoosePixelFormat(helperDC, &pfd); // gdi32 -> wglChoosePixelFormat
    if (dummyFormat <= 0) FAIL("gdi32 ChoosePixelFormat returned %d", dummyFormat);
    if (!SetPixelFormat(helperDC, dummyFormat, &pfd)) FAIL("gdi32 SetPixelFormat failed");
    std::printf("dummy pixel format: %d (DescribePixelFormat count=%d)\n", dummyFormat,
                DescribePixelFormat(helperDC, 1, sizeof(pfd), nullptr));

    HGLRC dummyCtx = p_wglCreateContext(helperDC);
    if (!dummyCtx) FAIL("wglCreateContext (dummy) failed");
    if (!p_wglMakeCurrent(helperDC, dummyCtx)) {
        typedef int(WINAPI * PFN_eglGetError)(void);
        auto eglGetError_ = (PFN_eglGetError)::GetProcAddress(g_opengl32, "eglGetError");
        FAIL("wglMakeCurrent (dummy) failed (GetLastError=%lu eglError=0x%x)", GetLastError(),
             eglGetError_ ? eglGetError_() : -1);
    }

    auto wglGetExtensionsStringARB =
        (PFN_wglGetExtensionsStringARB)p_wglGetProcAddress("wglGetExtensionsStringARB");
    auto wglCreateContextAttribsARB =
        (PFN_wglCreateContextAttribsARB)p_wglGetProcAddress("wglCreateContextAttribsARB");
    auto wglSwapIntervalEXT = (PFN_wglSwapIntervalEXT)p_wglGetProcAddress("wglSwapIntervalEXT");
    auto wglGetPixelFormatAttribivARB =
        (PFN_wglGetPixelFormatAttribivARB)p_wglGetProcAddress("wglGetPixelFormatAttribivARB");
    if (!wglGetExtensionsStringARB) FAIL("wglGetExtensionsStringARB unavailable");
    if (!wglCreateContextAttribsARB) FAIL("wglCreateContextAttribsARB unavailable");
    if (!wglSwapIntervalEXT) FAIL("wglSwapIntervalEXT unavailable");
    if (!wglGetPixelFormatAttribivARB) FAIL("wglGetPixelFormatAttribivARB unavailable");
    std::printf("WGL extensions: %s\n", wglGetExtensionsStringARB(helperDC));

    glGetString_ = (PFN_glGetString)GetGLProc("glGetString");
    if (!glGetString_) FAIL("glGetString unavailable");
    std::printf("dummy ctx GL_VERSION: %s\n", glGetString_(GL_VERSION));
    std::printf("dummy ctx GL_RENDERER: %s\n", glGetString_(GL_RENDERER));

    int nfmtAttrib = WGL_NUMBER_PIXEL_FORMATS_ARB;
    int nativeCount = 0;
    if (!wglGetPixelFormatAttribivARB(helperDC, 1, 0, 1, &nfmtAttrib, &nativeCount) || nativeCount < 1)
        FAIL("WGL_NUMBER_PIXEL_FORMATS_ARB query failed (%d)", nativeCount);
    std::printf("ARB pixel formats: %d\n", nativeCount);

    if (!p_wglMakeCurrent(nullptr, nullptr)) FAIL("wglMakeCurrent(NULL) failed");
    if (!p_wglDeleteContext(dummyCtx)) FAIL("wglDeleteContext (dummy) failed");

    // ---- Phase 2: real window + 3.2 core forward-compatible context ----
    HWND window = MakeWindow("MobileGL WGL smoke", 640, 480, true);
    if (!window) FAIL("main window creation failed");
    HDC dc = GetDC(window);

    // Pick the depth24/stencil8 RGBA format via the ARB query, GLFW-style.
    int chosenFormat = 0;
    for (int i = 1; i <= nativeCount; ++i) {
        const int attribs[] = {WGL_SUPPORT_OPENGL_ARB, WGL_DRAW_TO_WINDOW_ARB, WGL_PIXEL_TYPE_ARB,
                               WGL_DOUBLE_BUFFER_ARB, WGL_DEPTH_BITS_ARB, WGL_STENCIL_BITS_ARB};
        int values[6] = {};
        if (!wglGetPixelFormatAttribivARB(dc, i, 0, 6, attribs, values)) continue;
        if (values[0] && values[1] && values[2] == WGL_TYPE_RGBA_ARB && values[3] && values[4] >= 24 &&
            values[5] >= 8) {
            chosenFormat = i;
            break;
        }
    }
    if (!chosenFormat) FAIL("no ARB pixel format with depth24+stencil8");
    PIXELFORMATDESCRIPTOR realPfd{};
    if (!DescribePixelFormat(dc, chosenFormat, sizeof(realPfd), &realPfd))
        FAIL("DescribePixelFormat(%d) failed", chosenFormat);
    if (!SetPixelFormat(dc, chosenFormat, &realPfd)) FAIL("SetPixelFormat (real) failed");

    const int ctxAttribs[] = {
        WGL_CONTEXT_OPENGL_NO_ERROR_ARB, 1,
        WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
        WGL_CONTEXT_MINOR_VERSION_ARB, 2,
        WGL_CONTEXT_FLAGS_ARB, WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB,
        WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
        0,
    };
    HGLRC ctx = wglCreateContextAttribsARB(dc, nullptr, ctxAttribs);
    if (!ctx) FAIL("wglCreateContextAttribsARB failed (err=0x%lx)", GetLastError());
    if (!p_wglMakeCurrent(dc, ctx)) FAIL("wglMakeCurrent (real) failed");
    if (p_wglGetCurrentContext() != ctx || p_wglGetCurrentDC() != dc) FAIL("current context/DC mismatch");
    if (!wglSwapIntervalEXT(0)) FAIL("wglSwapIntervalEXT(0) failed");

#define LOAD(fn)                                                                                                       \
    fn##_ = (PFN_##fn)GetGLProc(#fn);                                                                                  \
    if (!fn##_) FAIL("missing GL function " #fn);
    LOAD(glGetIntegerv)
    LOAD(glClearColor)
    LOAD(glClear)
    LOAD(glViewport)
    LOAD(glGetError)
    LOAD(glReadPixels)
    LOAD(glFinish)
    LOAD(glCreateShader)
    LOAD(glShaderSource)
    LOAD(glCompileShader)
    LOAD(glGetShaderiv)
    LOAD(glGetShaderInfoLog)
    LOAD(glCreateProgram)
    LOAD(glAttachShader)
    LOAD(glLinkProgram)
    LOAD(glGetProgramiv)
    LOAD(glGetProgramInfoLog)
    LOAD(glUseProgram)
    LOAD(glGenVertexArrays)
    LOAD(glBindVertexArray)
    LOAD(glGenBuffers)
    LOAD(glBindBuffer)
    LOAD(glBufferData)
    LOAD(glVertexAttribPointer)
    LOAD(glEnableVertexAttribArray)
    LOAD(glDrawArrays)
#undef LOAD

    GLint major = 0, minor = 0;
    glGetIntegerv_(GL_MAJOR_VERSION, &major);
    glGetIntegerv_(GL_MINOR_VERSION, &minor);
    std::printf("real ctx: GL %d.%d | %s | %s\n", major, minor, glGetString_(GL_RENDERER),
                glGetString_(GL_VERSION));
    if (major < 3 || (major == 3 && minor < 2)) FAIL("context version below 3.2");

    const char* vsSrc = "#version 150 core\n"
                        "in vec2 aPos;\n"
                        "void main() { gl_Position = vec4(aPos, 0.0, 1.0); }\n";
    const char* fsSrc = "#version 150 core\n"
                        "out vec4 fragColor;\n"
                        "void main() { fragColor = vec4(1.0, 0.5, 0.0, 1.0); }\n";

    GLuint vs = glCreateShader_(GL_VERTEX_SHADER);
    glShaderSource_(vs, 1, &vsSrc, nullptr);
    glCompileShader_(vs);
    GLint ok = 0;
    glGetShaderiv_(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog_(vs, sizeof(log), nullptr, log);
        FAIL("vertex shader compile failed: %s", log);
    }
    GLuint fs = glCreateShader_(GL_FRAGMENT_SHADER);
    glShaderSource_(fs, 1, &fsSrc, nullptr);
    glCompileShader_(fs);
    glGetShaderiv_(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog_(fs, sizeof(log), nullptr, log);
        FAIL("fragment shader compile failed: %s", log);
    }
    GLuint prog = glCreateProgram_();
    glAttachShader_(prog, vs);
    glAttachShader_(prog, fs);
    glLinkProgram_(prog);
    glGetProgramiv_(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog_(prog, sizeof(log), nullptr, log);
        FAIL("program link failed: %s", log);
    }

    const GLfloat verts[] = {-0.8f, -0.8f, 0.8f, -0.8f, 0.0f, 0.8f};
    GLuint vao = 0, vbo = 0;
    glGenVertexArrays_(1, &vao);
    glBindVertexArray_(vao);
    glGenBuffers_(1, &vbo);
    glBindBuffer_(GL_ARRAY_BUFFER, vbo);
    glBufferData_(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glVertexAttribPointer_(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray_(0);

    glViewport_(0, 0, 640, 480);
    glUseProgram_(prog);

    unsigned char centerPixel[4] = {}, cornerPixel[4] = {};
    for (int frame = 0; frame < 60; ++frame) {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        glClearColor_(0.1f, 0.2f, 0.4f, 1.0f);
        glClear_(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDrawArrays_(GL_TRIANGLES, 0, 3);
        if (frame == 58) {
            glFinish_();
            glReadPixels_(320, 240, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, centerPixel);
            glReadPixels_(5, 5, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, cornerPixel);
        }
        if (!SwapBuffers(dc)) FAIL("gdi32 SwapBuffers failed at frame %d", frame); // gdi32 -> wglSwapBuffers
    }

    GLenum err = glGetError_();
    std::printf("center pixel: %u,%u,%u,%u  corner pixel: %u,%u,%u,%u  glGetError=0x%x\n", centerPixel[0],
                centerPixel[1], centerPixel[2], centerPixel[3], cornerPixel[0], cornerPixel[1], cornerPixel[2],
                cornerPixel[3], err);

    if (err != GL_NO_ERROR) FAIL("GL error 0x%x", err);
    // Orange triangle at center (255,127±3,0), clear color at corner (~25,51,102).
    if (!(centerPixel[0] > 200 && centerPixel[1] > 90 && centerPixel[1] < 160 && centerPixel[2] < 50))
        FAIL("center pixel is not the triangle color");
    if (!(cornerPixel[2] > 60 && cornerPixel[0] < 60)) FAIL("corner pixel is not the clear color");

    if (!p_wglMakeCurrent(nullptr, nullptr)) FAIL("final wglMakeCurrent(NULL) failed");
    if (!p_wglDeleteContext(ctx)) FAIL("final wglDeleteContext failed");

    std::printf("SMOKE PASS\n");
    return 0;
}
