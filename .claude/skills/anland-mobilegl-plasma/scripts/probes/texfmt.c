/* texfmt: does a texture keep its alpha? One opaque red texel and one transparent texel per
 * format, sampled across a 64x64 ES3 pbuffer cleared white with SRC_ALPHA blending. Expect the
 * left half red and the right half WHITE; a black right half means the alpha was lost. */
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define P(T, n) T n = (T)eglGetProcAddress(#n)
static const char* vs = "#version 300 es\nlayout(location=0) in vec2 p; out vec2 uv; void main(){uv=p*0.5+0.5; gl_Position=vec4(p,0,1);}";
static const char* fs = "#version 300 es\nprecision mediump float; uniform sampler2D t; uniform int mode; in vec2 uv; out vec4 o;"
                        "void main(){vec4 c=texture(t,uv); o = mode==1 ? vec4(1.0,0.0,0.0,c.a) : (mode==2 ? vec4(c.r,0.0,0.0,c.r) : c);}";
int main(void) {
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY); eglInitialize(d, 0, 0); eglBindAPI(EGL_OPENGL_ES_API);
    EGLint ca[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig c; EGLint n; eglChooseConfig(d, ca, &c, 1, &n);
    EGLint xa[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext x = eglCreateContext(d, c, EGL_NO_CONTEXT, xa);
    EGLint pa[] = {EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE};
    EGLSurface s = eglCreatePbufferSurface(d, c, pa); eglMakeCurrent(d, s, s, x);
    P(PFNGLCREATESHADERPROC, glCreateShader); P(PFNGLSHADERSOURCEPROC, glShaderSource); P(PFNGLCOMPILESHADERPROC, glCompileShader);
    P(PFNGLCREATEPROGRAMPROC, glCreateProgram); P(PFNGLATTACHSHADERPROC, glAttachShader); P(PFNGLLINKPROGRAMPROC, glLinkProgram);
    P(PFNGLUSEPROGRAMPROC, glUseProgram); P(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation); P(PFNGLUNIFORM1IPROC, glUniform1i);
    P(PFNGLGENBUFFERSPROC, glGenBuffers); P(PFNGLBINDBUFFERPROC, glBindBuffer); P(PFNGLBUFFERDATAPROC, glBufferData);
    P(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer); P(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray);
    P(PFNGLDRAWARRAYSPROC, glDrawArrays); P(PFNGLCLEARCOLORPROC, glClearColor); P(PFNGLCLEARPROC, glClear);
    P(PFNGLREADPIXELSPROC, glReadPixels); P(PFNGLVIEWPORTPROC, glViewport); P(PFNGLGENTEXTURESPROC, glGenTextures);
    P(PFNGLBINDTEXTUREPROC, glBindTexture); P(PFNGLTEXIMAGE2DPROC, glTexImage2D); P(PFNGLTEXPARAMETERIPROC, glTexParameteri);
    P(PFNGLENABLEPROC, glEnable); P(PFNGLBLENDFUNCPROC, glBlendFunc); P(PFNGLGETERRORPROC, glGetError);
    P(PFNGLTEXSTORAGE2DPROC, glTexStorage2D); P(PFNGLTEXSUBIMAGE2DPROC, glTexSubImage2D); P(PFNGLGETSTRINGPROC, glGetString);
    printf("GL_VERSION %s\n", glGetString(GL_VERSION));
    printf("BGRA ext: %s\n", strstr((const char*)glGetString(GL_EXTENSIONS), "GL_EXT_texture_format_BGRA8888") ? "yes" : "no");
    GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(v, 1, &vs, 0); glCompileShader(v); glShaderSource(f, 1, &fs, 0); glCompileShader(f);
    GLuint pr = glCreateProgram(); glAttachShader(pr, v); glAttachShader(pr, f); glLinkProgram(pr); glUseProgram(pr);
    float q[] = {-1,-1, 1,-1, -1,1, 1,1};
    GLuint vbo; glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STATIC_DRAW); glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, 0); glEnableVertexAttribArray(0);
    GLint lm = glGetUniformLocation(pr, "mode");
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    struct { const char* name; GLint ifmt; GLenum fmt; int bpp; int mode; int storage; GLenum sized; unsigned char px[8]; } cases[] = {
        {"RGBA/TexImage", GL_RGBA, GL_RGBA, 4, 0, 0, 0, {255,0,0,255, 0,0,0,0}},
        {"BGRA_EXT/TexImage", GL_BGRA_EXT, GL_BGRA_EXT, 4, 0, 0, 0, {0,0,255,255, 0,0,0,0}},
        {"BGRA8_EXT/TexStorage+Sub", 0, GL_BGRA_EXT, 4, 0, 1, GL_BGRA8_EXT, {0,0,255,255, 0,0,0,0}},
        {"RGBA8/TexStorage+Sub", 0, GL_RGBA, 4, 0, 1, GL_RGBA8, {255,0,0,255, 0,0,0,0}},
        {"ALPHA", GL_ALPHA, GL_ALPHA, 1, 1, 0, 0, {255, 0}},
        {"LUMINANCE_ALPHA", GL_LUMINANCE_ALPHA, GL_LUMINANCE_ALPHA, 2, 1, 0, 0, {255,255, 0,0}},
        {"R8/TexStorage+Sub", 0, GL_RED, 1, 2, 1, GL_R8, {255, 0}},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        while (glGetError()) {}
        if (cases[i].storage) {
            glTexStorage2D(GL_TEXTURE_2D, 1, cases[i].sized, 2, 1);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 2, 1, cases[i].fmt, GL_UNSIGNED_BYTE, cases[i].px);
        } else {
            glTexImage2D(GL_TEXTURE_2D, 0, cases[i].ifmt, 2, 1, 0, cases[i].fmt, GL_UNSIGNED_BYTE, cases[i].px);
        }
        GLenum err = glGetError();
        glUniform1i(lm, cases[i].mode);
        glViewport(0, 0, 64, 64); glClearColor(1, 1, 1, 1); glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        unsigned char l[4], r[4];
        glReadPixels(16, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, l); glReadPixels(48, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, r);
        const int ok = l[0] > 200 && l[1] < 60 && l[2] < 60 && r[0] > 200 && r[1] > 200 && r[2] > 200;
        printf("%-26s err=0x%04x left=%3d,%3d,%3d right=%3d,%3d,%3d %s\n", cases[i].name, err, l[0], l[1], l[2], r[0], r[1], r[2], ok ? "ok" : "BAD");
    }
    /* ANGLE's emulation of ALPHA8 / LUMINANCE_ALPHA8 on a desktop-GL driver: R8 / RG8 + swizzle. */
    P(PFNGLTEXPARAMETERIPROC, glTexParameteri2);
    struct { const char* name; GLenum sized, fmt; int bpp; GLint sw[4]; unsigned char px[4]; unsigned char wantL[3], wantR[3]; } sz[] = {
        {"R8 swizzled as ALPHA8", GL_R8, GL_RED, 1, {GL_ZERO, GL_ZERO, GL_ZERO, GL_RED}, {255, 0}, {0,0,0}, {255,255,255}},
        {"RG8 swizzled as LUM_ALPHA", GL_RG8, GL_RG, 2, {GL_RED, GL_RED, GL_RED, GL_GREEN}, {255,255, 0,0}, {255,255,255}, {255,255,255}},
        {"RG8 as LUM_ALPHA (lum 0)", GL_RG8, GL_RG, 2, {GL_RED, GL_RED, GL_RED, GL_GREEN}, {0,255, 0,0}, {0,0,0}, {255,255,255}},
    };
    glUniform1i(lm, 0);
    for (unsigned i = 0; i < sizeof sz / sizeof sz[0]; ++i) {
        GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
        glTexParameteri2(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri2(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexStorage2D(GL_TEXTURE_2D, 1, sz[i].sized, 2, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 2, 1, sz[i].fmt, GL_UNSIGNED_BYTE, sz[i].px);
        glTexParameteri2(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, sz[i].sw[0]); glTexParameteri2(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, sz[i].sw[1]);
        glTexParameteri2(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, sz[i].sw[2]); glTexParameteri2(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, sz[i].sw[3]);
        GLenum err = glGetError();
        glClearColor(1, 1, 1, 1); glClear(GL_COLOR_BUFFER_BIT); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        unsigned char l[4], r[4];
        glReadPixels(16, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, l); glReadPixels(48, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, r);
        int ok = 1;
        for (int k = 0; k < 3; ++k) ok = ok && abs(l[k] - sz[i].wantL[k]) < 40 && abs(r[k] - sz[i].wantR[k]) < 40;
        printf("%-26s err=0x%04x left=%3d,%3d,%3d right=%3d,%3d,%3d %s\n", sz[i].name, err, l[0], l[1], l[2], r[0], r[1], r[2], ok ? "ok" : "BAD");
    }
    return 0;
}
