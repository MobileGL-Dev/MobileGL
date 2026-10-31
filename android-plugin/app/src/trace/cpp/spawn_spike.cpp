// P0 spike A - the Android half of the delivery chain (PLAN-B.md §8.1, inheriting
// PLAN.md §11.1-§11.6). See spawn_spike.hpp for what the spike is asking.
//
// Android-only on purpose: this TU is listed only by
// android-plugin/app/src/trace/cpp/CMakeLists.txt. Its sibling trace_replay_core.cpp is
// shared with the DESKTOP mobilegl_trace_replay runner, which has no <android/log.h>,
// so nothing Android-specific may live there.

#include "spawn_spike.hpp"

#if !defined(__ANDROID__)
#error "spawn_spike.cpp is Android-only; do not add it to the desktop trace replay build"
#endif

#include <android/log.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

// execve needs the environment the parent already has: a server process started from
// the app must inherit it, and handing it an empty one would change what is being tested.
extern "C" char** environ;

namespace mobilegl_trace {
namespace {

constexpr const char* kSpikeLogTag = "MobileGLTraceRunner";

std::string ReadWholeFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    std::string text = contents.str();
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == '\0')) {
        text.pop_back();
    }
    return text;
}

// The domain this process is in. `u:r:untrusted_app:s0:...` is the whole point of the
// spike: an exec that works from an `adb run-as` shell says nothing about whether the
// app itself is allowed to do it, because that shell is a different SELinux domain.
std::string ReadSelfSelinuxContext() {
    const std::string context = ReadWholeFile("/proc/self/attr/current");
    return context.empty() ? "<unreadable>" : context;
}

// Starts the child with its stdout and stderr redirected into `outputPath`, reports the
// child pid through `childPid` and, when the exec itself was refused, the child's errno
// through `execErrno`. Returns 0, or the errno of a failure that happened before the
// child existed at all.
//
// fork/execve, not posix_spawn: bionic only declares posix_spawn from API 28 while
// MobileGL ships at minSdk 26 (the root CMakeLists.txt pins MOBILEGL_ANDROID_API_LEVEL
// to 26 and refuses to configure lower), so posix_spawn is not available to the shipping
// build and this is the shape the production spawn path has to take. Nothing happens
// between fork and execve except open/dup2/execve/write/_exit, all async-signal-safe,
// because the parent is a multi-threaded JVM process.
int SpawnSpikeChild(const std::string& serverPath,
                    const std::string& markerPath,
                    const std::string& outputPath,
                    pid_t* childPid,
                    int* execErrno) {
    *execErrno = 0;
    char* argv[] = {const_cast<char*>(serverPath.c_str()),
                    const_cast<char*>(markerPath.c_str()), nullptr};

    // The errno of a refused exec is the answer this spike is here to bring back, and it
    // is raised in a process that cannot return anything: by the time the parent sees a
    // wait status the reason has been flattened into an exit code. So the child writes
    // the raw errno into a close-on-exec pipe. A successful exec closes the write end for
    // free and the parent reads EOF; a refused one leaves the four bytes behind. EACCES
    // (SELinux, or a noexec mount) and ENOEXEC (a mangled or non-PIE file) are entirely
    // different verdicts for the design and this is the only thing that separates them.
    int report[2] = {-1, -1};
    if (pipe2(report, O_CLOEXEC) != 0) {
        return errno;
    }

    const pid_t forked = fork();
    if (forked < 0) {
        const int forkErrno = errno;
        close(report[0]);
        close(report[1]);
        return forkErrno;
    }
    if (forked == 0) {
        close(report[0]);
        // Without this the child's output is unobservable: an Android app process has
        // stdout on /dev/null, so a printed line would vanish and the spike could not
        // tell "ran and printed" apart from "never ran".
        const int outputFd = open(outputPath.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0664);
        if (outputFd >= 0) {
            dup2(outputFd, STDOUT_FILENO);
            dup2(outputFd, STDERR_FILENO);
            if (outputFd != STDOUT_FILENO && outputFd != STDERR_FILENO) {
                close(outputFd);
            }
        }
        execve(serverPath.c_str(), argv, environ);
        const int failure = errno;
        // Only reached when the exec was refused - the one outcome this spike is about.
        const ssize_t written = write(report[1], &failure, sizeof(failure));
        static_cast<void>(written);
        // 127 is the shell's convention for "could not exec" and is distinguishable from
        // every status the stub itself can return.
        _exit(127);
    }

    close(report[1]);
    int failure = 0;
    ssize_t got = 0;
    // Blocks until the child either execs (the write end closes, read returns 0) or
    // reports why it could not.
    while ((got = read(report[0], &failure, sizeof(failure))) < 0 && errno == EINTR) {
    }
    close(report[0]);
    if (got == static_cast<ssize_t>(sizeof(failure))) {
        *execErrno = failure;
    }
    *childPid = forked;
    return 0;
}

} // namespace

SpawnSpikeResult RunSpawnSpike(const SpawnSpikeRequest& request) {
    SpawnSpikeResult result;
    result.parentSelinuxContext = ReadSelfSelinuxContext();

    if (request.serverPath.empty() || request.markerPath.empty()) {
        result.message = "spike-spawn: serverPath and markerPath are both required";
        return result;
    }

    // A stale marker from a previous run would otherwise be read back as this run's
    // proof. Remove it first, so "the marker exists" can only mean the child wrote it.
    unlink(request.markerPath.c_str());
    const std::string childOutputPath = request.markerPath + ".stdout";
    unlink(childOutputPath.c_str());

    struct stat serverStat {};
    if (stat(request.serverPath.c_str(), &serverStat) != 0) {
        result.spawnErrno = errno;
        result.message = "spike-spawn: " + request.serverPath + " does not exist: " +
                         std::strerror(errno);
        __android_log_print(ANDROID_LOG_ERROR, kSpikeLogTag, "%s", result.message.c_str());
        return result;
    }

    pid_t childPid = -1;
    int execErrno = 0;
    const int spawnStatus = SpawnSpikeChild(request.serverPath, request.markerPath,
                                            childOutputPath, &childPid, &execErrno);
    if (spawnStatus != 0) {
        result.spawnErrno = spawnStatus;
        result.message = "spike-spawn: could not start " + request.serverPath +
                         ": spawnErrno=" + std::to_string(spawnStatus) + " (" +
                         std::strerror(spawnStatus) + ")";
        __android_log_print(ANDROID_LOG_ERROR, kSpikeLogTag, "%s (parentSelinux=%s)",
                            result.message.c_str(), result.parentSelinuxContext.c_str());
        return result;
    }

    result.spawned = true;
    result.childPid = static_cast<int>(childPid);
    result.execErrno = execErrno;

    int waitStatus = 0;
    while (waitpid(childPid, &waitStatus, 0) < 0) {
        if (errno != EINTR) {
            result.message = "spike-spawn: waitpid failed: " + std::string(std::strerror(errno));
            __android_log_print(ANDROID_LOG_ERROR, kSpikeLogTag, "%s", result.message.c_str());
            return result;
        }
    }
    result.waitStatus = waitStatus;
    if (WIFEXITED(waitStatus)) {
        result.exitCode = WEXITSTATUS(waitStatus);
    }
    if (WIFSIGNALED(waitStatus)) {
        result.termSignal = WTERMSIG(waitStatus);
    }

    result.markerContent = ReadWholeFile(request.markerPath);
    result.childOutput = ReadWholeFile(childOutputPath);
    result.succeeded =
            result.execErrno == 0 && result.exitCode == 0 && !result.markerContent.empty();

    std::ostringstream message;
    message << "spike-spawn: " << (result.succeeded ? "OK" : "FAILED")
            << " server=" << request.serverPath
            << " pid=" << result.childPid
            << " exit=" << result.exitCode
            << " signal=" << result.termSignal
            // Always printed, including on the success path, so a reader never has to
            // guess whether the field was collected or merely absent.
            << " execErrno=" << result.execErrno
            << " (" << (result.execErrno == 0 ? "exec succeeded"
                                              : std::strerror(result.execErrno)) << ")"
            << " parentSelinux=" << result.parentSelinuxContext
            << " marker=[" << result.markerContent << "]"
            << " childStdout=[" << result.childOutput << "]";
    result.message = message.str();
    __android_log_print(result.succeeded ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, kSpikeLogTag,
                        "%s", result.message.c_str());

    // The Activity is normally gone as soon as the run finishes, so the verdict also goes
    // to a file next to the marker; that is what a device lane copies out.
    std::ofstream report(request.markerPath + ".report", std::ios::trunc);
    if (report) {
        report << result.message << "\n";
    }
    return result;
}

} // namespace mobilegl_trace
