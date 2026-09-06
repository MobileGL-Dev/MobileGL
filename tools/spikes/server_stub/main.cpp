// P0 spike A - the Android delivery chain for a second native executable.
//
// The disaggregated design (PLAN-B.md §8.1, inheriting PLAN.md §11.1-§11.6) needs a
// second process on Android. Android has no exec-able install location an application
// can write to, so the only supported way to ship an executable inside an APK is to
// name it lib*.so, let the packager put it in lib/<abi>/ and exec it out of
// getApplicationInfo().nativeLibraryDir. Whether that actually works from the app's own
// untrusted_app SELinux domain - as opposed to from an adb `run-as` shell, which runs in
// a different domain and proves nothing - is the question this spike answers.
//
// This binary is deliberately the smallest thing that can answer it: it prints one line
// describing the process it ended up being (pid, uid, and its own SELinux context) to
// stdout and writes the same line to the file named by argv[1], then exits 0. The parent
// reads both back; see RunSpawnSpike() in
// android-plugin/app/src/trace/cpp/trace_replay_core.cpp.
//
// It is built only when MOBILEGL_BUILD_SERVER_SPIKE=ON on an ANDROID configure, so it is
// absent from every shipping build. It is not the future server, and nothing links it.

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <unistd.h>

namespace {

// The single fact that makes this spike conclusive rather than suggestive: the exec'd
// child reports the domain it is running in. `u:r:untrusted_app:s0:...` means an ordinary
// application process really did exec this file; anything else (shell, adb, a platform
// domain) means the test was run the wrong way and its verdict does not transfer.
void ReadSelinuxContext(char* out, size_t size) {
    out[0] = '\0';
    FILE* file = std::fopen("/proc/self/attr/current", "r");
    if (file == nullptr) {
        std::snprintf(out, size, "<unreadable>");
        return;
    }
    const size_t read = std::fread(out, 1, size - 1, file);
    std::fclose(file);
    out[read] = '\0';
    // The kernel returns the context NUL-terminated inside the read; trim anything after.
    for (size_t index = 0; index < read; ++index) {
        if (out[index] == '\n' || out[index] == '\0') {
            out[index] = '\0';
            break;
        }
    }
    if (out[0] == '\0') {
        std::snprintf(out, size, "<empty>");
    }
}

} // namespace

int main(int argc, char** argv) {
    char context[256];
    ReadSelinuxContext(context, sizeof(context));

    char line[1024];
    std::snprintf(line, sizeof(line),
                  "MobileGLServer-spike ok argv0=%s pid=%d ppid=%d uid=%d gid=%d argc=%d "
                  "selinux=%s\n",
                  argc > 0 && argv[0] != nullptr ? argv[0] : "<null>",
                  static_cast<int>(getpid()), static_cast<int>(getppid()),
                  static_cast<int>(getuid()), static_cast<int>(getgid()), argc, context);

    std::fputs(line, stdout);
    std::fflush(stdout);

    if (argc < 2 || argv[1] == nullptr || argv[1][0] == '\0') {
        std::fputs("MobileGLServer-spike: argv[1] (marker path) missing\n", stderr);
        return 2;
    }

    FILE* marker = std::fopen(argv[1], "w");
    if (marker == nullptr) {
        std::fprintf(stderr, "MobileGLServer-spike: cannot open marker %s: %s\n", argv[1],
                     std::strerror(errno));
        return 3;
    }
    std::fputs(line, marker);
    if (std::fclose(marker) != 0) {
        std::fprintf(stderr, "MobileGLServer-spike: cannot write marker %s: %s\n", argv[1],
                     std::strerror(errno));
        return 4;
    }
    return 0;
}
