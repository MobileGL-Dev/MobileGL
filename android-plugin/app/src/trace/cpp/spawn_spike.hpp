#pragma once

// ---------------------------------------------------------------------------
// P0 spike A: exec a second packaged native executable from this process.
//
// Answers one question and nothing else: can an ordinary Android application
// process (untrusted_app, NOT an `adb run-as` shell, which runs in a different
// SELinux domain and would prove nothing) exec a binary that was shipped inside
// its own APK as lib/<abi>/lib*.so? The disaggregated design needs a server
// process on Android and this is its only supported delivery route (PLAN-B.md
// §8.1, inheriting PLAN.md §11.1-§11.6).
//
// This lives beside trace_replay_core.hpp rather than inside it because
// trace_replay_core.cpp is ALSO compiled by the desktop mobilegl_trace_replay
// runner (tools/trace_replay/CMakeLists.txt names it directly), where <android/log.h>
// does not exist. The spike is Android-only, so it gets an Android-only TU;
// spawn_spike.cpp is listed only by the trace APK's CMakeLists.
//
// Nothing in the replay path calls this; it runs only when the trace Activity is
// launched with the `mobilegl_spike_spawn` intent extra.
// ---------------------------------------------------------------------------

#include <string>

namespace mobilegl_trace {

struct SpawnSpikeRequest {
    // Absolute path of the executable, normally
    // getApplicationInfo().nativeLibraryDir + "/libMobileGLServer.so".
    std::string serverPath;
    // Marker file the child is asked to write, passed to it as argv[1]. The child's
    // stdout and stderr are captured next to it, with ".stdout" appended.
    std::string markerPath;
};

struct SpawnSpikeResult {
    bool spawned = false;
    // Exec'd, waited for, exited 0, and the marker file came back non-empty.
    bool succeeded = false;
    // errno of the pre-fork or fork failure - the parent could not even try.
    int spawnErrno = 0;
    // errno of a REFUSED execve, carried out of the child over a close-on-exec pipe.
    // This is the one datum the spike exists to produce: EACCES (SELinux or the mount's
    // noexec) and ENOEXEC (the packager mangled the file) are different verdicts, and
    // the exit status alone cannot tell them apart.
    int execErrno = 0;
    int childPid = -1;
    int waitStatus = -1;
    int exitCode = -1;
    int termSignal = -1;
    // /proc/self/attr/current of THIS process - the domain the exec was attempted from.
    std::string parentSelinuxContext;
    std::string markerContent;
    std::string childOutput;
    std::string message;
};

SpawnSpikeResult RunSpawnSpike(const SpawnSpikeRequest& request);

} // namespace mobilegl_trace
