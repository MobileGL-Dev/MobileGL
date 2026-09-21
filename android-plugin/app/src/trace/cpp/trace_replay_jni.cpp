#include "trace_replay_core.hpp"

#include "spawn_spike.hpp"
#include "trace_env_overrides.hpp"
#include "trace_replay_lease.hpp"

#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <exception>
#include <string>
#include <vector>
#include <sys/stat.h>

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

using mobilegl_trace::SplitSemicolonList;

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

void EnsureDirectory(const std::string& path) {
    if (!path.empty()) {
        mkdir(path.c_str(), 0775);
    }
}

mobilegl_trace::Result MakeFailureResult(const mobilegl_trace::Request& request,
                                         int statusCode,
                                         const std::string& message) {
    mobilegl_trace::Result result;
    result.statusCode = statusCode;
    result.message = message;
    if (!request.outputDir.empty()) {
        EnsureDirectory(request.outputDir);
        result.resultPath = request.outputDir + "/result.json";
        result.actualPath = request.outputDir + "/actual.png";
    }
    result.diffPath = request.diffPath;
    return result;
}

class ScopedTraceReplayState {
public:
    ~ScopedTraceReplayState() {
        mobilegl_trace_set_native_window(nullptr);
        mobilegl_trace_set_requested_size(0, 0);
    }
};

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
                                                                        jdouble ssimThreshold,
                                                                        jint cropX,
                                                                        jint cropY,
                                                                        jint cropWidth,
                                                                        jint cropHeight,
                                                                        jstring angleVariant,
                                                                        jboolean useAngle,
                                                                        jboolean usePbuffer,
                                                                        jboolean avoidAngleLlvmpipeSamplerMipmapMinFilter,
                                                                        jboolean avoidAngleLlvmpipeExplicitLodBias,
                                                                        jboolean coherentAsFlush,
                                                                        jboolean fixIterationRPSubgroupScratch,
                                                                        jboolean deriveNumSubgroups,
                                                                        jboolean iterationRPFixBarrier,
                                                                        jstring texture2dDumps,
                                                                        jboolean benchmarkMode,
                                                                        jint benchmarkTailFrames,
                                                                        jboolean benchmarkFinish,
                                                                        jstring benchmarkResultPath,
                                                                        jstring envOverrides) {
    // Claim before changing GLWS globals, environment, logs or output files. The lease
    // outlives replayState, so another invocation cannot race native-window cleanup.
    mobilegl_trace::TraceReplayLease lease;
    if (!lease) {
        mobilegl_trace::Result rejected;
        rejected.statusCode = mobilegl_trace::STATUS_RETRACE_FAILED;
        rejected.message = "another trace replay is already running in this process";
        // No paths: a duplicate Activity can share outputDir with the live invocation.
        // Publishing this failure there would overwrite that invocation's real result.
        return MakeResult(env, rejected);
    }

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
    request.angleVariant = ToString(env, angleVariant);
    request.texture2dDumps = SplitSemicolonList(ToString(env, texture2dDumps));
    request.targetFrame = targetFrame;
    request.targetCall = targetCall;
    request.width = width;
    request.height = height;
    request.ssimThreshold = ssimThreshold;
    request.cropX = cropX;
    request.cropY = cropY;
    request.cropWidth = cropWidth;
    request.cropHeight = cropHeight;
    request.useAngle = useAngle == JNI_TRUE;
    request.usePbuffer = usePbuffer == JNI_TRUE;
    request.avoidAngleLlvmpipeSamplerMipmapMinFilter =
        avoidAngleLlvmpipeSamplerMipmapMinFilter == JNI_TRUE;
    request.avoidAngleLlvmpipeExplicitLodBias = avoidAngleLlvmpipeExplicitLodBias == JNI_TRUE;
    request.coherentAsFlush = coherentAsFlush == JNI_TRUE;
    request.fixIterationRPSubgroupScratch = fixIterationRPSubgroupScratch == JNI_TRUE;
    request.deriveNumSubgroups = deriveNumSubgroups == JNI_TRUE;
    request.iterationRPFixBarrier = iterationRPFixBarrier == JNI_TRUE;
    request.benchmark = benchmarkMode == JNI_TRUE;
    request.benchmarkTailFrames = benchmarkTailFrames > 0
                                          ? benchmarkTailFrames
                                          : mobilegl_trace::kDefaultBenchmarkTailFrames;
    request.benchmarkFinish = benchmarkFinish == JNI_TRUE;
    request.benchmarkResultPath = ToString(env, benchmarkResultPath);
    request.envOverrides = SplitSemicolonList(ToString(env, envOverrides));

    // THE SURFACE SHAPE DECIDES THIS, NOT THE BACKEND. DirectVulkan used to demand a native
    // window unconditionally, which made `use_pbuffer` a DirectGLES-only knob in practice: the
    // Android GLWS creates a window surface whenever gNativeWindow is non-null
    // (apitrace_glws_android.cpp's createSurface), so handing Magma a window was the same as
    // asking for one.
    //
    // P6 needs the pbuffer path on BOTH backends. Until P12 a spawned server cannot be given a
    // window at all - an ANativeWindow* is a pointer into the CLIENT's process, and
    // SetWindowHandle is refused on the way across with Fatal{UnmigratedSurface,
    // "AndroidNativeWindow@P12"} (Rule H). Measured: the DirectVulkan spawn arm died on exactly
    // that, one op after the server had come up green, while DirectGLES with the same flag passed.
    //
    // Every existing caller is unaffected: nobody asks for a Vulkan pbuffer today, and
    // !usePbuffer reproduces the old answer for both backends in every other combination.
    const bool needsNativeWindow = !request.usePbuffer;
    ANativeWindow *window = needsNativeWindow && surface != nullptr ? ANativeWindow_fromSurface(env, surface) : nullptr;
    if (needsNativeWindow && window == nullptr) {
        auto result = MakeFailureResult(request, mobilegl_trace::STATUS_RETRACE_FAILED,
                                       "render surface was destroyed before native replay started");
        if (!result.resultPath.empty()) {
            mobilegl_trace::WriteResultJson(request, result);
        }
        return MakeResult(env, result);
    }
    ScopedTraceReplayState replayState;
    mobilegl_trace_set_requested_size(request.width, request.height);
    mobilegl_trace_set_native_window(window);
    if (window != nullptr) {
        ANativeWindow_release(window);
    }

    mobilegl_trace::Result result;
    try {
        result = mobilegl_trace::RunTraceReplay(request);
    } catch (const std::exception& exception) {
        result = MakeFailureResult(request,
                                   mobilegl_trace::STATUS_RETRACE_FAILED,
                                   "trace replay failed with unhandled native exception: " +
                                           std::string(exception.what()));
    } catch (...) {
        result = MakeFailureResult(request,
                                   mobilegl_trace::STATUS_RETRACE_FAILED,
                                   "trace replay failed with unknown native exception");
    }
    if (!result.resultPath.empty()) {
        mobilegl_trace::WriteResultJson(request, result);
    }
    return MakeResult(env, result);
}

// P0 spike A: exec the packaged MobileGLServer stub from this app process and report what
// happened. Deliberately a separate entry point rather than another parameter on the
// replay call - it shares nothing with a replay, and the trace lane must be able to run
// it without a trace, a golden or a surface.
extern "C" JNIEXPORT jstring JNICALL
Java_top_mobilegl_plugin_trace_TraceReplayActivity_nativeRunSpawnSpike(JNIEnv* env,
                                                                       jclass,
                                                                       jstring serverPath,
                                                                       jstring markerPath) {
    mobilegl_trace::SpawnSpikeRequest request;
    request.serverPath = ToString(env, serverPath);
    request.markerPath = ToString(env, markerPath);

    const mobilegl_trace::SpawnSpikeResult result = mobilegl_trace::RunSpawnSpike(request);
    return env->NewStringUTF(result.message.c_str());
}
