#include "trace_replay_core.hpp"

#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <string>

extern "C" void mobilegl_trace_set_native_window(ANativeWindow *window);
extern "C" void mobilegl_trace_set_requested_size(int width, int height);

namespace {

std::string ToString(JNIEnv* env, jstring value) {
    if (value == nullptr) {
        return {};
    }
    const char* chars = env->GetStringUTFChars(value, nullptr);
    std::string out = chars == nullptr ? "" : chars;
    if (chars != nullptr) {
        env->ReleaseStringUTFChars(value, chars);
    }
    return out;
}

jobject MakeResult(JNIEnv* env, const mobilegl_trace::Result& result) {
    jclass clazz = env->FindClass("top/mobilegl/plugin/trace/TraceReplayActivity$TraceReplayResult");
    if (clazz == nullptr) {
        return nullptr;
    }
    jmethodID ctor = env->GetMethodID(clazz, "<init>",
                                      "(ZILjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
    if (ctor == nullptr) {
        return nullptr;
    }
    jstring message = env->NewStringUTF(result.message.c_str());
    jstring resultPath = env->NewStringUTF(result.resultPath.c_str());
    jstring actualPath = env->NewStringUTF(result.actualPath.c_str());
    jstring diffPath = env->NewStringUTF(result.diffPath.c_str());
    jobject object = env->NewObject(clazz, ctor, result.passed ? JNI_TRUE : JNI_FALSE, result.statusCode, message,
                                    resultPath, actualPath, diffPath);
    env->DeleteLocalRef(message);
    env->DeleteLocalRef(resultPath);
    env->DeleteLocalRef(actualPath);
    env->DeleteLocalRef(diffPath);
    return object;
}

} // namespace

extern "C" JNIEXPORT jobject JNICALL
Java_top_mobilegl_plugin_trace_TraceReplayActivity_nativeRunTraceReplay(JNIEnv* env,
                                                                        jclass,
                                                                        jobject surface,
                                                                        jstring tracePath,
                                                                        jstring goldenPath,
                                                                        jstring alternateGoldenPath,
                                                                        jstring outputDir,
                                                                        jstring diffPath,
                                                                        jstring backend,
                                                                        jint targetFrame,
                                                                        jlong targetCall,
                                                                        jint width,
                                                                        jint height,
                                                                        jint tolerance,
                                                                        jint cropX,
                                                                        jint cropY,
                                                                        jint cropWidth,
                                                                        jint cropHeight,
                                                                        jint fuzzPercent) {
    mobilegl_trace::Request request;
    request.tracePath = ToString(env, tracePath);
    request.goldenPath = ToString(env, goldenPath);
    std::string alternateGolden = ToString(env, alternateGoldenPath);
    if (!alternateGolden.empty()) {
        request.alternateGoldenPaths.push_back(alternateGolden);
    }
    request.outputDir = ToString(env, outputDir);
    request.diffPath = ToString(env, diffPath);
    request.backend = ToString(env, backend);
    request.targetFrame = targetFrame;
    request.targetCall = targetCall;
    request.width = width;
    request.height = height;
    request.tolerance = tolerance;
    request.cropX = cropX;
    request.cropY = cropY;
    request.cropWidth = cropWidth;
    request.cropHeight = cropHeight;
    request.fuzzPercent = fuzzPercent;

    mobilegl_trace_set_requested_size(request.width, request.height);
    const bool needsNativeWindow = request.backend == "DirectVulkan";
    ANativeWindow *window = needsNativeWindow && surface != nullptr ? ANativeWindow_fromSurface(env, surface) : nullptr;
    mobilegl_trace_set_native_window(window);
    if (window != nullptr) {
        ANativeWindow_release(window);
    }

    mobilegl_trace::Result result = mobilegl_trace::RunTraceReplay(request);
    mobilegl_trace_set_native_window(nullptr);
    mobilegl_trace_set_requested_size(0, 0);
    if (!result.resultPath.empty()) {
        mobilegl_trace::WriteResultJson(request, result);
    }
    return MakeResult(env, result);
}
