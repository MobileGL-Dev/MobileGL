#!/usr/bin/env python3
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

# This script is bundled inside the trace-fixture-authoring-on-android-fcl skill at
# <FCL>/MobileGL/tools/trace_replay/skills/trace-fixture-authoring-on-android-fcl/scripts/.
SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_REPO = SCRIPT_DIR.parents[5]  # -> FoldCraftLauncher repo root
DEFAULT_OUTPUT = SCRIPT_DIR / "out" / "egltrace.so"


def run(cmd, cwd=None):
    print("+", " ".join(str(part) for part in cmd), flush=True)
    subprocess.run(cmd, cwd=cwd, check=True)


def find_ndk(repo):
    for key in ("ANDROID_NDK_HOME", "ANDROID_NDK_ROOT"):
        value = os.environ.get(key)
        if value:
            return Path(value)
    for key in ("ANDROID_HOME", "ANDROID_SDK_ROOT"):
        value = os.environ.get(key)
        if value:
            ndk_root = Path(value) / "ndk"
            if ndk_root.exists():
                versions = sorted([p for p in ndk_root.iterdir() if p.is_dir()])
                if versions:
                    return versions[-1]
    local = repo / "local.properties"
    if local.exists():
        sdk = None
        ndk = None
        for line in local.read_text(encoding="utf-8", errors="ignore").splitlines():
            if line.startswith("sdk.dir="):
                sdk = Path(line.split("=", 1)[1].replace("\\:", ":"))
            if line.startswith("ndk.dir="):
                ndk = Path(line.split("=", 1)[1].replace("\\:", ":"))
        if ndk:
            return ndk
        if sdk:
            ndk_root = sdk / "ndk"
            if ndk_root.exists():
                versions = sorted([p for p in ndk_root.iterdir() if p.is_dir()])
                if versions:
                    return versions[-1]
    raise SystemExit("Android NDK not found; set ANDROID_NDK_HOME or local.properties sdk.dir/ndk.dir")


def generate_and_patch(repo, build_dir):
    wrapper_dir = repo / "MobileGL" / "3rdparty" / "apitrace" / "wrappers"
    generated = build_dir / "patched" / "egltrace.cpp"
    generated.parent.mkdir(parents=True, exist_ok=True)
    with generated.open("w", encoding="utf-8", newline="\n") as out:
        subprocess.run([sys.executable, str(wrapper_dir / "egltrace.py")], cwd=wrapper_dir, stdout=out, check=True)

    text = generated.read_text(encoding="utf-8")
    helper = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static unsigned long long mobilegl_capture_swap_count = 0;

static long long mobilegl_capture_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long) ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static int mobilegl_capture_exists(const char *path) {
    return path != NULL && path[0] != '\0' && access(path, F_OK) == 0;
}

static void mobilegl_capture_record_request(void) {
    ++mobilegl_capture_swap_count;
    const char *request = getenv("MOBILEGL_TRACE_CAPTURE_REQUEST_FILE");
    const char *output = getenv("MOBILEGL_TRACE_CAPTURE_FRAME_FILE");
    if (!mobilegl_capture_exists(request) || output == NULL || output[0] == '\0') {
        return;
    }
    unlink(request);
    FILE *file = fopen(output, "w");
    if (file == NULL) {
        return;
    }
    const char *trace_file = getenv("TRACE_FILE");
    fprintf(file,
            "{\n"
            "  \"status\": \"captured\",\n"
            "  \"swapCount\": %llu,\n"
            "  \"targetFrame\": %llu,\n"
            "  \"zeroBasedFrame\": %llu,\n"
            "  \"capturedAtMs\": %lld,\n"
            "  \"traceFile\": \"%s\"\n"
            "}\n",
            mobilegl_capture_swap_count,
            mobilegl_capture_swap_count,
            mobilegl_capture_swap_count == 0 ? 0 : mobilegl_capture_swap_count - 1,
            mobilegl_capture_time_ms(),
            trace_file == NULL ? "" : trace_file);
    fclose(file);
}
'''
    insert_at = text.find("#include")
    if insert_at < 0:
        raise SystemExit("generated egltrace.cpp has no include block")
    next_block = text.find("\n\n", insert_at)
    text = text[:next_block] + "\n" + helper + text[next_block:]

    needle = "EGLBoolean EGLAPIENTRY eglSwapBuffers(EGLDisplay dpy, EGLSurface surface)"
    start = text.find(needle)
    if start < 0:
        raise SystemExit("generated egltrace.cpp has no eglSwapBuffers wrapper to patch")
    brace = text.find("{", start)
    if brace < 0:
        raise SystemExit("eglSwapBuffers wrapper has no function body")
    text = text[:brace + 1] + "\n    mobilegl_capture_record_request();" + text[brace + 1:]
    generated.write_text(text, encoding="utf-8", newline="\n")
    return generated


def patch_glproc_egl(repo, build_dir):
    source = repo / "MobileGL" / "3rdparty" / "apitrace" / "wrappers" / "glproc_egl.cpp"
    patched = build_dir / "patched" / "glproc_egl.cpp"
    patched.parent.mkdir(parents=True, exist_ok=True)
    text = source.read_text(encoding="utf-8")
    text = text.replace('#include "dlopen.hpp"\n', '#include "dlopen.hpp"\n#include <stdlib.h>\n')
    needle = """void *
_getPublicProcAddress(const char *procName)
{
    void *proc;

"""
    replacement = """void *
_getPublicProcAddress(const char *procName)
{
    void *proc;

    static void *traceLibGL = NULL;
    static bool triedTraceLibGL = false;
    if (!triedTraceLibGL) {
        triedTraceLibGL = true;
        const char *traceLibGLName = getenv("TRACE_LIBGL");
        if (traceLibGLName && traceLibGLName[0]) {
            traceLibGL = _dlopen(traceLibGLName, RTLD_GLOBAL | RTLD_LAZY | RTLD_DEEPBIND);
        }
    }
    if (traceLibGL) {
        proc = dlsym(traceLibGL, procName);
        if (proc) {
            return proc;
        }
    }

"""
    if needle not in text:
        raise SystemExit("glproc_egl.cpp patch point not found")
    text = text.replace(needle, replacement, 1)
    patched.write_text(text, encoding="utf-8", newline="\n")
    return patched


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", default=str(DEFAULT_REPO), help="FoldCraftLauncher repo root")
    parser.add_argument("--abi", default="arm64-v8a")
    parser.add_argument("--android-platform", default="android-23")
    parser.add_argument("--build-dir", default=".trace-work/build-android-egltrace")
    parser.add_argument("--output", default=str(DEFAULT_OUTPUT))
    args = parser.parse_args()

    repo = Path(args.repo).resolve()
    ndk = find_ndk(repo)
    build_dir = (repo / args.build_dir / args.abi).resolve()
    apitrace = repo / "MobileGL" / "3rdparty" / "apitrace"
    source_dir = SCRIPT_DIR / "android_egltrace"
    toolchain = ndk / "build" / "cmake" / "android.toolchain.cmake"
    if not apitrace.exists():
        raise SystemExit(f"missing apitrace checkout: {apitrace}")
    if not source_dir.exists():
        raise SystemExit(f"missing wrapper CMake project: {source_dir}")
    if not toolchain.exists():
        raise SystemExit(f"missing Android toolchain: {toolchain}")
    cache = build_dir / "CMakeCache.txt"
    if cache.exists() and "CMAKE_GENERATOR:INTERNAL=Ninja" not in cache.read_text(encoding="utf-8", errors="ignore"):
        shutil.rmtree(build_dir)
    elif cache.exists() and str(source_dir).replace("\\", "/") not in cache.read_text(encoding="utf-8", errors="ignore").replace("\\", "/"):
        shutil.rmtree(build_dir)

    ninja = shutil.which("ninja")
    if ninja is None:
        cmake_ninjas = sorted((Path(os.environ.get("ANDROID_HOME", "")) / "cmake").glob("*/bin/ninja.exe"))
        ninja = str(cmake_ninjas[-1]) if cmake_ninjas else None
    if ninja is None:
        raise SystemExit("ninja not found; install Ninja or Android SDK CMake")

    patched_egltrace = generate_and_patch(repo, build_dir)
    patched_glproc_egl = patch_glproc_egl(repo, build_dir)

    run([
        "cmake", "-G", "Ninja", "-S", str(source_dir), "-B", str(build_dir),
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
        f"-DCMAKE_MAKE_PROGRAM={ninja}",
        f"-DANDROID_ABI={args.abi}",
        f"-DANDROID_PLATFORM={args.android_platform}",
        f"-DAPITRACE_ROOT={apitrace}",
        f"-DPATCHED_EGLTRACE_CPP={patched_egltrace}",
        f"-DPATCHED_GLPROC_EGL_CPP={patched_glproc_egl}",
    ])
    run(["cmake", "--build", str(build_dir), "--target", "egltrace", "--parallel"])

    output = Path(args.output)
    if not output.is_absolute():
        output = repo / output
    output = output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    candidates = list(build_dir.rglob("egltrace.so"))
    if not candidates:
        raise SystemExit("egltrace.so was not produced")
    shutil.copy2(candidates[0], output)
    print(output)


if __name__ == "__main__":
    main()
