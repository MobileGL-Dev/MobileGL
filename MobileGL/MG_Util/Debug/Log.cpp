// MobileGL - MobileGL/MG_Util/Debug/Log.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "../../Includes.h"

#include <cstring> // std::strcmp, for the spawned-server role check in InitFile

namespace MobileGL {
    namespace MG_Util::Debug {
        std::mutex& LogMutex();
        static FILE* s_logFile = nullptr;

#if MOBILEGL_BUILD_DISAGGREGATED
        // The server role's own sink. Opened lazily, on the first line a server-role thread
        // writes, so a process that never plays the role never creates the file.
        static FILE* s_serverLogFile = nullptr;
        static LogForwarder s_forwarder = nullptr;
        static void* s_forwarderUser = nullptr;

        void SetLogForwarder(LogForwarder forwarder, void* user) {
            std::lock_guard<std::mutex> lock(LogMutex());
            s_forwarder = forwarder;
            s_forwarderUser = user;
        }

        void WithLogBarrier(void (*action)(void*), void* user) {
            std::lock_guard<std::mutex> lock(LogMutex());
            action(user);
        }

        void WritePeerLog(const char* message) {
            std::lock_guard<std::mutex> lock(LogMutex());
            const char* path = std::getenv("MOBILEGL_LOG_FILE_PATH");
            if (!path || !*path) path = MOBILEGL_LOG_FILE_PATH;
            if (!path || !*path) return;
            if (!s_serverLogFile) s_serverLogFile = std::fopen(RoleLogPath(path, LogRole::Server).c_str(), "w");
            if (s_serverLogFile) {
                std::fputs(message, s_serverLogFile);
                std::fflush(s_serverLogFile);
            }
        }

        // THE PROCESS DEFAULT, read once. A spawned server image is the server on every thread -
        // there is no other role in it - and MOBILEGL_IPC_ROLE is set by the launcher before
        // exec, so this is answerable before any MobileGL code runs. Under inproc it is false
        // and the apply thread opts in per-thread.
        static bool ProcessIsSpawnedServer() {
            static const bool value = [] {
                const char* role = std::getenv("MOBILEGL_IPC_ROLE");
                return role != nullptr && std::strcmp(role, "server") == 0;
            }();
            return value;
        }

        // Per THREAD, not per process: under inproc the server role is the mgl-srv-apply thread
        // and the GL thread is the client, in one address space. -1 means "not asked yet", which
        // is resolved from the process default on first use.
        static thread_local int t_logRole = -1;

        static bool ThreadIsServerRole() {
            if (t_logRole < 0) {
                t_logRole = ProcessIsSpawnedServer() ? 1 : 0;
            }
            return t_logRole == 1;
        }

        void SetThreadLogRole(LogRole role) { t_logRole = role == LogRole::Server ? 1 : 0; }

        std::string ReadRoleLogs(const char* basePath) {
            std::string all;
            for (const LogRole role : {LogRole::Client, LogRole::Server}) {
                const std::string path = RoleLogPath(basePath, role);
                if (FILE* file = std::fopen(path.c_str(), "rb")) {
                    char chunk[4096];
                    std::size_t got = 0;
                    while ((got = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
                        all.append(chunk, got);
                    }
                    std::fclose(file);
                }
            }
            return all;
        }

        void TruncateRoleLogs(const char* basePath) {
            // BOTH, because a reader that concatenates them would otherwise fold a previous
            // run's server lines into this one's - which is the stale-tail failure R-16 was
            // written after, one file over.
            for (const LogRole role : {LogRole::Client, LogRole::Server}) {
                if (FILE* file = std::fopen(RoleLogPath(basePath, role).c_str(), "w")) {
                    std::fclose(file);
                }
            }
        }

        // MOBILEGL_LOG_FILE_PATH IS A BASE NAME NOW, AND NEITHER ROLE KEEPS IT.
        //
        // `foo.log` becomes `foo.client.log` and `foo.server.log`; a path with no extension gets
        // the suffix appended. The suffix goes BEFORE the extension so the pair sorts together
        // and a `*.log` glob still finds both.
        //
        // THE CLIENT IS RENAMED TOO, ON PURPOSE. Leaving it at the unsuffixed path would have
        // been the compatible choice and the wrong one: every reader that was not updated would
        // go on finding a file, go on passing, and go on seeing only half the session - and a
        // refusal census that reports a confident zero for the half it cannot see is worse than
        // one that fails. With both names moved, an un-updated reader gets ENOENT and says so.
        // There were fourteen such readers in this tree when the split landed.
        std::string RoleLogPath(const char* basePath, LogRole role) {
            const char* roleSuffix = role == LogRole::Server ? "server" : "client";
            std::string path(basePath);
            const std::string::size_type slash = path.find_last_of("/\\");
            const std::string::size_type dot = path.find_last_of('.');
            if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
                return path + "." + roleSuffix;
            }
            return path.substr(0, dot) + "." + roleSuffix + path.substr(dot);
        }

        // P6 gate 8: the role, in the two forms a caller can hold it in. Both are thin readers of
        // state this file already keeps; neither adds a second source of truth.
        LogRole CurrentThreadRole() {
            if (t_logRole < 0) {
                t_logRole = ProcessIsSpawnedServer() ? 1 : 0;
            }
            return t_logRole == 1 ? LogRole::Server : LogRole::Client;
        }

        bool CurrentProcessIsSpawnedServer() { return ProcessIsSpawnedServer(); }
#endif

        std::mutex& LogMutex() {
            static auto* mutex = new std::mutex();
            return *mutex;
        }

        constexpr char* GetOSName() {
#if defined(_WIN32)
            return (char*)"Windows";
#elif defined(__ANDROID__)
            return (char*)"Android";
#elif defined(__APPLE__)
            return (char*)"macOS";
#elif defined(__linux__)
            return (char*)"Linux";
#else
            return (char*)"UnknownOS";
#endif
        }

        std::string GetThreadName() {
            char buffer[64] = {0};
#if defined(_WIN32)
            PWSTR desc = nullptr;
            if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &desc))) {
                WideCharToMultiByte(CP_UTF8, 0, desc, -1, buffer, sizeof(buffer), nullptr, nullptr);
                LocalFree(desc);
            }
#elif defined(__ANDROID__) || defined(__linux__) || defined(__APPLE__)
            pthread_getname_np(pthread_self(), buffer, sizeof(buffer));
#endif
            return buffer[0] ? buffer : "UnknownThread";
        }

        std::string GetCurrentTime() {
            using namespace std::chrono;
            auto now = system_clock::now();
            std::time_t tt = system_clock::to_time_t(now);
            struct tm tm{};
#if defined(_WIN32)
            localtime_s(&tm, &tt);
#else
            localtime_r(&tt, &tm);
#endif
            char buf[16];
            std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
            return buf;
        }

        void InitFile() {
#if MOBILEGL_LOG_ENABLE_FILE
            // MOBILEGL_LOG_FILE_PATH must stay a raw getenv (not MG_Config::Features):
            // InitFile() runs before MG_ConfigLoader::Init() in MobileGL::Initialize().
            const char* logPath = std::getenv("MOBILEGL_LOG_FILE_PATH");
            if (!logPath || !*logPath) {
                logPath = MOBILEGL_LOG_FILE_PATH;
            }
            if (!logPath || !*logPath) {
                return;
            }
#if MOBILEGL_BUILD_DISAGGREGATED
            // ONE FILE PER ROLE, OPENED LAZILY BY WHOEVER FIRST WRITES ONE.
            //
            // Each role truncates ITS OWN file exactly once, which is what the previous
            // single-file design could not do: there the client truncated and the server was
            // forbidden to, because a truncate from the far side would have deleted the very
            // startup lines a failed session needs. With separate files that asymmetry is gone -
            // nobody else writes the file being truncated.
            //
            // "w" IS SAFE AGAIN HERE, and the O_APPEND this replaces was never about ordering:
            // it was about two PROCESSES holding two handles to one path at two offsets, each
            // overwriting the other's bytes. Two paths, two writers, no sharing.
            if (ThreadIsServerRole()) {
                if (!s_serverLogFile) {
                    s_serverLogFile = std::fopen(RoleLogPath(logPath, LogRole::Server).c_str(), "w");
                }
                return;
            }
            if (!s_logFile) {
                s_logFile = std::fopen(RoleLogPath(logPath, LogRole::Client).c_str(), "w");
            }
#else
            if (!s_logFile) {
                s_logFile = std::fopen(logPath, "w");
            }
#endif
#endif
        }

        void WriteToFile(const char* msg) {
#if MOBILEGL_LOG_ENABLE_FILE
#if MOBILEGL_BUILD_DISAGGREGATED
            // The sink is chosen by the CALLING THREAD's role, which is the only choice that is
            // correct under inproc: there both roles are threads of one process, so anything
            // process-scoped would file the apply thread's lines under the client.
            FILE** sink = ThreadIsServerRole() ? &s_serverLogFile : &s_logFile;
            if (!*sink) InitFile();
            if (*sink) {
                std::fputs(msg, *sink);
                std::fflush(*sink);
            }
#else
            if (!s_logFile) InitFile();
            if (s_logFile) {
                std::fputs(msg, s_logFile);
                std::fflush(s_logFile);
            }
#endif
#endif
        }

        void Log(const char* levelTag, android_LogPriority androidLogLevel, const char* fmt, ...) {
            std::lock_guard<std::mutex> lock(LogMutex());

#if MOBILEGL_LOG_ENABLE_STACKTRACE
            auto trace = std::stacktrace::current();
            std::string padding(4 * trace.size(), ' ');
#endif

            std::string header =
                "[" + GetCurrentTime() + "] [" + GetOSName() + " " + GetThreadName() + "/" + levelTag + "]: ";

            static char buffer[1024000];
            va_list args;
            va_start(args, fmt);
            int n = std::vsnprintf(buffer, sizeof(buffer), fmt, args);
            const SizeT messageLength =
                (n < 0) ? 0 : std::min(static_cast<SizeT>(n), sizeof(buffer) - static_cast<SizeT>(1));
            std::string out = header +
#if MOBILEGL_LOG_ENABLE_STACKTRACE
                              padding +
#endif
                              std::string(buffer, messageLength) + "\n";

#if MOBILEGL_LOG_ENABLE_CONSOLE
            std::fwrite(out.c_str(), 1, out.size(), stdout);
            fflush(stdout);
#endif

#if MOBILEGL_LOG_ENABLE_ANDROID && defined(__ANDROID__)
            // Without the trailing newline that the file sink needs: logcat terminates
            // records itself, so handing it an already-newline-terminated string made
            // every MobileGL log occupy TWO logcat records, the second one empty. That
            // halved the useful depth of every `adb logcat -t N` window the CI
            // diagnostics read (android-plugin/trace-replay-ci.sh).
            __android_log_print(androidLogLevel, "MobileGL", "%.*s", static_cast<int>(out.size() - 1),
                                out.c_str());
#endif

            WriteToFile(out.c_str());
#if MOBILEGL_BUILD_DISAGGREGATED
            if (s_forwarder && ThreadIsServerRole()) s_forwarder(s_forwarderUser, out.c_str());
#endif

            va_end(args);
        }

        void Close() {
#if MOBILEGL_LOG_ENABLE_FILE
#if MOBILEGL_BUILD_DISAGGREGATED
            if (s_serverLogFile) {
                std::fclose(s_serverLogFile);
                s_serverLogFile = nullptr;
            }
#endif
            if (s_logFile) {
                std::fclose(s_logFile);
                s_logFile = nullptr;
            }
#endif
        }
    } // namespace MG_Util::Debug
} // namespace MobileGL
