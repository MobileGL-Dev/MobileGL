// GLFW-driven smoke test: exercises the exact GLFW WGL bootstrap Minecraft uses
// (helper-window dummy context, ARB pixel format, 3.2 core forward-compatible
// no-error context) against the MobileGL opengl32.dll drop-in.
//
// Build: cl /nologo /EHsc /W3 glfw_smoke.cpp /I<GLFW>/include <GLFW>/lib-vc2022/glfw3dll.lib user32.lib gdi32.lib
// Run with opengl32.dll + glfw3.dll in the exe directory.

#include <GLFW/glfw3.h>
#include <windows.h>
#include <cstdio>

typedef const unsigned char*(WINAPI* PFN_glGetString)(unsigned int);
typedef void(WINAPI* PFN_glClearColor)(float, float, float, float);
typedef void(WINAPI* PFN_glClear)(unsigned int);
typedef void(WINAPI* PFN_glReadPixels)(int, int, int, int, unsigned int, unsigned int, void*);
typedef void(WINAPI* PFN_glFinish)(void);
typedef unsigned int(WINAPI* PFN_glCreateShader)(unsigned int);
typedef void(WINAPI* PFN_glShaderSource)(unsigned int, int, const char* const*, const int*);
typedef void(WINAPI* PFN_glCompileShader)(unsigned int);
typedef unsigned int(WINAPI* PFN_glCreateProgram)(void);
typedef void(WINAPI* PFN_glAttachShader)(unsigned int, unsigned int);
typedef void(WINAPI* PFN_glLinkProgram)(unsigned int);
typedef void(WINAPI* PFN_glUseProgram)(unsigned int);
typedef void(WINAPI* PFN_glGenVertexArrays)(int, unsigned int*);
typedef void(WINAPI* PFN_glBindVertexArray)(unsigned int);
typedef void(WINAPI* PFN_glGenBuffers)(int, unsigned int*);
typedef void(WINAPI* PFN_glBindBuffer)(unsigned int, unsigned int);
typedef void(WINAPI* PFN_glBufferData)(unsigned int, long long, const void*, unsigned int);
typedef void(WINAPI* PFN_glVertexAttribPointer)(unsigned int, int, unsigned int, unsigned char, int, const void*);
typedef void(WINAPI* PFN_glEnableVertexAttribArray)(unsigned int);
typedef void(WINAPI* PFN_glDrawArrays)(unsigned int, int, int);
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_VERSION 0x1F02
#define GL_RENDERER 0x1F01
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_TRIANGLES 0x0004
#define GL_FLOAT 0x1406
#define GL_ARRAY_BUFFER 0x8892
#define GL_STATIC_DRAW 0x88E4
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30

static void RawPrint(HANDLE f, const char* s) {
    DWORD written = 0;
    WriteFile(f, s, (DWORD)lstrlenA(s), &written, nullptr);
}

static void RawHex(HANDLE f, unsigned long long v) {
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
    HANDLE f = CreateFileA("glfw-crash-stack.txt", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) TerminateProcess(GetCurrentProcess(), 0x43);
    RawPrint(f, "AV at ");
    RawHex(f, (unsigned long long)ep->ExceptionRecord->ExceptionAddress);
    RawPrint(f, " reading ");
    RawHex(f, (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
    RawPrint(f, "\n");
    const unsigned long long* sp = (const unsigned long long*)ep->ContextRecord->Rsp;
    for (int i = 0; i < 512; ++i) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(sp + i, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) == 0)
            break;
        unsigned long long v = sp[i];
        HMODULE mod = nullptr;
        if (v > 0x10000 &&
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)v, &mod) &&
            mod) {
            char name[MAX_PATH];
            name[0] = 0;
            GetModuleFileNameA(mod, name, MAX_PATH);
            RawPrint(f, "stack ");
            RawHex(f, v);
            RawPrint(f, " ");
            RawPrint(f, name);
            RawPrint(f, "+");
            RawHex(f, v - (unsigned long long)mod);
            RawPrint(f, "\n");
        }
    }
    CloseHandle(f);
    TerminateProcess(GetCurrentProcess(), 0x42);
    return EXCEPTION_EXECUTE_HANDLER;
}

int main() {
    AddVectoredExceptionHandler(1, CrashDump);
    if (!glfwInit()) {
        std::fprintf(stderr, "GLFW-SMOKE FAIL: glfwInit\n");
        return 1;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_CONTEXT_NO_ERROR, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    GLFWwindow* win = glfwCreateWindow(854, 480, "MobileGL GLFW smoke", nullptr, nullptr);
    if (!win) {
        const char* desc = nullptr;
        int code = glfwGetError(&desc);
        std::fprintf(stderr, "GLFW-SMOKE FAIL: glfwCreateWindow (0x%x %s)\n", code, desc ? desc : "?");
        return 1;
    }
    glfwMakeContextCurrent(win);
    glfwShowWindow(win);
    glfwSwapInterval(0);

    auto glGetString_ = (PFN_glGetString)glfwGetProcAddress("glGetString");
    auto glClearColor_ = (PFN_glClearColor)glfwGetProcAddress("glClearColor");
    auto glClear_ = (PFN_glClear)glfwGetProcAddress("glClear");
    auto glReadPixels_ = (PFN_glReadPixels)glfwGetProcAddress("glReadPixels");
    auto glFinish_ = (PFN_glFinish)glfwGetProcAddress("glFinish");
    std::printf("GL_VERSION: %s\nGL_RENDERER: %s\n", glGetString_(GL_VERSION), glGetString_(GL_RENDERER));

    auto glCreateShader_ = (PFN_glCreateShader)glfwGetProcAddress("glCreateShader");
    auto glShaderSource_ = (PFN_glShaderSource)glfwGetProcAddress("glShaderSource");
    auto glCompileShader_ = (PFN_glCompileShader)glfwGetProcAddress("glCompileShader");
    auto glCreateProgram_ = (PFN_glCreateProgram)glfwGetProcAddress("glCreateProgram");
    auto glAttachShader_ = (PFN_glAttachShader)glfwGetProcAddress("glAttachShader");
    auto glLinkProgram_ = (PFN_glLinkProgram)glfwGetProcAddress("glLinkProgram");
    auto glUseProgram_ = (PFN_glUseProgram)glfwGetProcAddress("glUseProgram");
    auto glGenVertexArrays_ = (PFN_glGenVertexArrays)glfwGetProcAddress("glGenVertexArrays");
    auto glBindVertexArray_ = (PFN_glBindVertexArray)glfwGetProcAddress("glBindVertexArray");
    auto glGenBuffers_ = (PFN_glGenBuffers)glfwGetProcAddress("glGenBuffers");
    auto glBindBuffer_ = (PFN_glBindBuffer)glfwGetProcAddress("glBindBuffer");
    auto glBufferData_ = (PFN_glBufferData)glfwGetProcAddress("glBufferData");
    auto glVertexAttribPointer_ = (PFN_glVertexAttribPointer)glfwGetProcAddress("glVertexAttribPointer");
    auto glEnableVertexAttribArray_ = (PFN_glEnableVertexAttribArray)glfwGetProcAddress("glEnableVertexAttribArray");
    auto glDrawArrays_ = (PFN_glDrawArrays)glfwGetProcAddress("glDrawArrays");

    const char* vsSrc = "#version 150 core\nin vec2 aPos;\nvoid main(){ gl_Position = vec4(aPos, 0.0, 1.0); }\n";
    const char* fsSrc = "#version 150 core\nout vec4 c;\nvoid main(){ c = vec4(1.0, 0.5, 0.0, 1.0); }\n";
    unsigned int vs = glCreateShader_(GL_VERTEX_SHADER);
    glShaderSource_(vs, 1, &vsSrc, nullptr);
    glCompileShader_(vs);
    unsigned int fs = glCreateShader_(GL_FRAGMENT_SHADER);
    glShaderSource_(fs, 1, &fsSrc, nullptr);
    glCompileShader_(fs);
    unsigned int prog = glCreateProgram_();
    glAttachShader_(prog, vs);
    glAttachShader_(prog, fs);
    glLinkProgram_(prog);
    glUseProgram_(prog);
    const float verts[] = {-0.9f, -0.9f, 0.9f, -0.9f, 0.0f, 0.9f};
    unsigned int vao = 0, vbo = 0;
    glGenVertexArrays_(1, &vao);
    glBindVertexArray_(vao);
    glGenBuffers_(1, &vbo);
    glBindBuffer_(GL_ARRAY_BUFFER, vbo);
    glBufferData_(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glVertexAttribPointer_(0, 2, GL_FLOAT, 0, 0, nullptr);
    glEnableVertexAttribArray_(0);

    unsigned char px[4] = {};
    for (int i = 0; i < 60 && !glfwWindowShouldClose(win); ++i) {
        glClearColor_(0.1f, 0.2f, 0.4f, 1.0f);
        glClear_(GL_COLOR_BUFFER_BIT);
        glDrawArrays_(GL_TRIANGLES, 0, 3);
        if (i == 58) {
            glFinish_();
            glReadPixels_(427, 240, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        }
        glfwSwapBuffers(win);
        glfwPollEvents();
    }
    std::printf("pixel: %u,%u,%u,%u\n", px[0], px[1], px[2], px[3]);
    glfwDestroyWindow(win);
    glfwTerminate();
    const bool ok = px[0] > 200 && px[1] > 90 && px[1] < 160 && px[2] < 50;
    std::printf(ok ? "GLFW-SMOKE PASS\n" : "GLFW-SMOKE FAIL: wrong pixel\n");
    return ok ? 0 : 1;
}
