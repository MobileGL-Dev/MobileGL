// MobileGL - MobileGL/MG_Remote/Server/DisplayServerJni.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P12 (on-screen server window), D3/D5/D6/D7. THE JNI GLUE OF THE IN-PROCESS DISPLAY SERVER.
//
// The trace APK's MobileGLDisplayActivity (android:process=":mglwin") owns a SurfaceView and runs
// the TCP server on a thread of its own process, so a headless client (MOBILEGL_IPC_SURFACE=server)
// renders straight onto that SurfaceView. This file is the whole native half of that Activity:
//
//   nativeInstallDisplay   ServerDisplayInstance().Install(): ANativeWindow acquire/release plus the
//                          geometry up-call below. Before the server serves.
//   nativeServe            mobilegl_server_serve_inprocess(listen) on the CALLING (Java) thread;
//                          returns its exit code when the server stops or fails.
//   nativeStop             mobilegl_server_stop_inprocess(): non-blocking, safe on the UI thread.
//   nativeAttachWindow     surfaceCreated / surfaceChanged: ANativeWindow_fromSurface, Attach (which
//                          takes its OWN reference), then this call's reference is released.
//   nativeDetachWindow     surfaceDestroyed: ServerDisplay::Detach, which BLOCKS (bounded) until the
//                          session rendering into the window has released its backend surface on the
//                          apply thread. Returns the result's name for the Activity's log.
//   nativeUninstallDisplay after the server returned: the display is gone.
//
// THE GEOMETRY UP-CALL. ServerDisplay calls RequestGeometry on the APPLY THREAD, from AcquireFor,
// during a ServerOwned CreateWindowSurface, with the size the client asked for. That thread is a
// native thread the JVM does not know, so it is attached for the call and detached after it (a
// native thread that exits while still attached aborts the runtime). The Java side only POSTS the
// request to the UI thread (SurfaceHolder.setFixedSize / setSizeFromLayout must run there) and
// returns: this must not block and must not call back into ServerDisplay. The class and method are
// resolved here, on the Java thread that installs the display, because a native thread's FindClass
// would search the system class loader, not the app's.
//
// Compiled only for Android with MOBILEGL_BUILD_DISAGGREGATED (CMakeLists.txt, beside
// DriverPostJni.cpp); the guard keeps it inert anywhere else.
#ifdef __ANDROID__

#include "InProcessServer.h"
#include "ServerDisplay.h"

#include <MG_Util/Debug/Log.h>

#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <pthread.h>

#include <mutex>

namespace {
    using MobileGL::Bool;
    using MobileGL::Uint32;
    using MobileGL::MG_Remote::Server::ServerDisplayInstance;
    using MobileGL::MG_Remote::Server::ServerWindowDetach;
    using MobileGL::MG_Remote::Server::ServerWindowDetachName;

    // Set once by nativeInstallDisplay (a Java thread), read by the apply thread's up-call.
    std::mutex g_upcallMutex;
    JavaVM* g_vm = nullptr;
    jclass g_activityClass = nullptr; // a global reference
    jmethodID g_onGeometryRequest = nullptr;

    void RequestGeometry(void* /*user*/, Uint32 width, Uint32 height) {
        JavaVM* vm = nullptr;
        jclass activityClass = nullptr;
        jmethodID onGeometryRequest = nullptr;
        {
            const std::lock_guard<std::mutex> lock(g_upcallMutex);
            vm = g_vm;
            activityClass = g_activityClass;
            onGeometryRequest = g_onGeometryRequest;
        }
        if (vm == nullptr || activityClass == nullptr || onGeometryRequest == nullptr) {
            MGLOG_W("MG_Remote server: display geometry %ux%u requested, but no Java display is installed to "
                    "apply it; the window keeps its own size",
                    width, height);
            return;
        }
        JNIEnv* env = nullptr;
        Bool attached = false;
        const jint got = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        if (got == JNI_EDETACHED) {
            // ART RENAMES the thread it attaches (and a detach does not rename it back), so the
            // apply thread is attached under its OWN name - otherwise every later log line of the
            // session would be tagged with the up-call's name instead of mgl-srv-apply.
            char threadName[16] = "mgl-srv-apply";
            (void)pthread_getname_np(pthread_self(), threadName, sizeof(threadName));
            JavaVMAttachArgs args{JNI_VERSION_1_6, threadName, nullptr};
            if (vm->AttachCurrentThread(&env, &args) != JNI_OK || env == nullptr) {
                MGLOG_E("MG_Remote server: could not attach the apply thread to the JVM to request display "
                        "geometry %ux%u; the window keeps its own size",
                        width, height);
                return;
            }
            attached = true;
        } else if (got != JNI_OK || env == nullptr) {
            MGLOG_E("MG_Remote server: JavaVM::GetEnv failed (%d) requesting display geometry %ux%u",
                    static_cast<int>(got), width, height);
            return;
        }
        env->CallStaticVoidMethod(activityClass, onGeometryRequest, static_cast<jint>(width),
                                  static_cast<jint>(height));
        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
            MGLOG_E("MG_Remote server: the display's geometry up-call (%ux%u) threw; the window keeps its own size",
                    width, height);
        } else {
            MGLOG_I("MG_Remote server: display geometry %ux%u requested (%s)", width, height,
                    width != 0 && height != 0 ? "SurfaceHolder.setFixedSize" : "SurfaceHolder.setSizeFromLayout");
        }
        if (attached) vm->DetachCurrentThread();
    }
} // namespace

extern "C" JNIEXPORT void JNICALL Java_top_mobilegl_plugin_MobileGLDisplayActivity_nativeInstallDisplay(JNIEnv* env,
                                                                                                     jclass clazz) {
    JavaVM* vm = nullptr;
    if (env->GetJavaVM(&vm) != JNI_OK) vm = nullptr;
    jmethodID onGeometryRequest = env->GetStaticMethodID(clazz, "onGeometryRequest", "(II)V");
    if (onGeometryRequest == nullptr) {
        // NoSuchMethodError is pending; the display still works, at the window's own size.
        env->ExceptionClear();
        MGLOG_E("MG_Remote server: MobileGLDisplayActivity has no static onGeometryRequest(int, int); "
                "geometry requests will not reach the SurfaceHolder");
    }
    {
        const std::lock_guard<std::mutex> lock(g_upcallMutex);
        if (g_activityClass != nullptr) env->DeleteGlobalRef(g_activityClass);
        g_vm = vm;
        g_activityClass = static_cast<jclass>(env->NewGlobalRef(clazz));
        g_onGeometryRequest = onGeometryRequest;
    }
    ServerDisplayInstance().Install(
        MobileGL::MG_Remote::Server::AndroidNativeWindowHooks(&RequestGeometry, /*user=*/nullptr));
}

extern "C" JNIEXPORT jint JNICALL Java_top_mobilegl_plugin_MobileGLDisplayActivity_nativeServe(JNIEnv* env, jclass,
                                                                                           jstring listen) {
    if (listen == nullptr) return mobilegl_server_serve_inprocess(nullptr);
    const char* endpoint = env->GetStringUTFChars(listen, nullptr);
    if (endpoint == nullptr) return 71; // OutOfMemoryError pending
    const int result = mobilegl_server_serve_inprocess(endpoint);
    env->ReleaseStringUTFChars(listen, endpoint);
    return static_cast<jint>(result);
}

extern "C" JNIEXPORT void JNICALL Java_top_mobilegl_plugin_MobileGLDisplayActivity_nativeStop(JNIEnv*, jclass) {
    mobilegl_server_stop_inprocess();
}

extern "C" JNIEXPORT jboolean JNICALL Java_top_mobilegl_plugin_MobileGLDisplayActivity_nativeAttachWindow(
    JNIEnv* env, jclass, jobject surface, jint width, jint height) {
    if (surface == nullptr) return JNI_FALSE;
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (window == nullptr) {
        MGLOG_E("MG_Remote server: ANativeWindow_fromSurface returned no window (the Surface is gone)");
        return JNI_FALSE;
    }
    ServerDisplayInstance().Attach(window, width > 0 ? static_cast<Uint32>(width) : 0u,
                                   height > 0 ? static_cast<Uint32>(height) : 0u);
    // Attach took its own reference (the hooks' ANativeWindow_acquire) or already holds one for this
    // window (surfaceChanged); this call's reference from ANativeWindow_fromSurface goes now.
    ANativeWindow_release(window);
    return JNI_TRUE;
}

extern "C" JNIEXPORT jstring JNICALL Java_top_mobilegl_plugin_MobileGLDisplayActivity_nativeDetachWindow(JNIEnv* env,
                                                                                                     jclass) {
    const ServerWindowDetach detached = ServerDisplayInstance().Detach();
    return env->NewStringUTF(ServerWindowDetachName(detached));
}

extern "C" JNIEXPORT void JNICALL Java_top_mobilegl_plugin_MobileGLDisplayActivity_nativeUninstallDisplay(JNIEnv* env,
                                                                                                      jclass) {
    ServerDisplayInstance().Uninstall();
    const std::lock_guard<std::mutex> lock(g_upcallMutex);
    if (g_activityClass != nullptr) env->DeleteGlobalRef(g_activityClass);
    g_activityClass = nullptr;
    g_onGeometryRequest = nullptr;
}

#endif // __ANDROID__
