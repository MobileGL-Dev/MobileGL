// MobileGL - MobileGL/MG_Remote/Server/ServerSpawn.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "ServerSpawn.h"

#include "../Transport/FdPassing.h"
#include "../Transport/WireLog.h"

#include <Config.h>

#include <cerrno>
#include <cstring>
#include <vector>

#if defined(__unix__) || defined(__APPLE__) || defined(__ANDROID__)
#    define MOBILEGL_SERVER_SPAWN_POSIX 1
#    include <dirent.h>
#    include <fcntl.h>
#    include <dlfcn.h>
#    include <sys/socket.h>
#    include <sys/stat.h>
#    include <sys/types.h>
#    include <sys/wait.h>
#    include <unistd.h>
#else
#    define MOBILEGL_SERVER_SPAWN_POSIX 0
#endif

namespace MobileGL::MG_Remote::Server {

#if MOBILEGL_SERVER_SPAWN_POSIX

    namespace {

        constexpr int kChildStreamFd = 3;
        constexpr int kChildAuxFd = 4;
        constexpr int kChildServerBellFd = 5;
        constexpr int kChildClientBellFd = 6;

        bool IsExecutableFile(const std::string& path) {
            if (path.empty()) {
                return false;
            }
            struct stat st {};
            if (::stat(path.c_str(), &st) != 0) {
                return false;
            }
            return S_ISREG(st.st_mode) && ::access(path.c_str(), X_OK) == 0;
        }

        // MOBILEGL_IPC_SERVER_PATH, then dladdr beside our own library.
        // The fallback exists because the integration tests link MobileGL_s
        // statically and trace replay's executable does not live in the library
        // directory - ARCHITECTURE.md §15.1 names both.
        std::string ResolveImage(const std::string& requested) {
            if (!requested.empty()) {
                return requested; // caller's explicit choice; validated below
            }
            const std::string configured(MG_Config::Ipc.ServerPath.c_str());
            if (!configured.empty()) {
                return configured;
            }
            Dl_info info{};
            if (::dladdr(reinterpret_cast<void*>(&ResolveImage), &info) != 0 &&
                info.dli_fname != nullptr) {
                const std::string self(info.dli_fname);
                const std::size_t slash = self.find_last_of('/');
                if (slash != std::string::npos) {
                    return self.substr(0, slash + 1) + "libMobileGLServer.so";
                }
            }
            return std::string();
        }

        // Every MOBILEGL_TRANSPORT and MOBILEGL_IPC_* goes. Built BEFORE fork:
        // between fork and execve only async-signal-safe calls are allowed, and
        // allocating a vector of strings is not one of them.
        //
        // EXCEPT A KNOB THE SERVER READS, named here one by one. P7 wave 4 M2's
        // MOBILEGL_IPC_WIRE_DEFERRED_MB bounds the server's own orphaned wire buffer stores;
        // nothing in the client reads it, so scrubbing it did not make the child safer - it made
        // the knob mean something under inproc and silently nothing under spawn, the worst of
        // the three states (the tcp server reads its supervisor's environment, as for every
        // knob). It names no endpoint, no role and no segment size, so neither anti-recursion
        // catch has anything to say about it.
        //
        // M2 round 2 (review): three more the server reads and that were silently defaulting in
        // the spawned child for the same reason - MOBILEGL_IPC_SPIN_US (the doorbell spin budget,
        // read by BOTH roles, so the child should spin as the parent was told to),
        // MOBILEGL_IPC_SERVER_AFFINITY (the apply thread's CPU mask, read only by the server) and
        // MOBILEGL_IPC_AUDIT (the server's 0xDD fill of retired stage bytes). Each is a tuning or
        // audit value, none names an endpoint, a role, a path or a segment size, and ServerMain's
        // catch (b) keeps verifying the four it verifies (TRANSPORT, SERVER_PATH, RING_MB,
        // STAGE_MB). Everything else under the prefix still goes.
        //
        // PH-6 (ID-P7-2) adds MOBILEGL_IPC_EVENT_WAIT_MS on the same argument: it is the server's
        // patience with a client that has stopped draining SEG_EVENT, nothing on the client reads
        // it, and scrubbing it would make a spawned server wait the default whatever the lane set.
        bool ShouldScrub(const char* entry) {
            static constexpr const char* kServerOwned[] = {
                "MOBILEGL_IPC_WIRE_DEFERRED_MB=", "MOBILEGL_IPC_SPIN_US=", "MOBILEGL_IPC_SERVER_AFFINITY=",
                "MOBILEGL_IPC_AUDIT=", "MOBILEGL_IPC_EVENT_WAIT_MS=",
            };
            for (const char* kept : kServerOwned) {
                if (std::strncmp(entry, kept, std::strlen(kept)) == 0) {
                    return false;
                }
            }
            static constexpr const char* kPrefixes[] = {"MOBILEGL_TRANSPORT=", "MOBILEGL_IPC_"};
            for (const char* prefix : kPrefixes) {
                if (std::strncmp(entry, prefix, std::strlen(prefix)) == 0) {
                    return true;
                }
            }
            return false;
        }

        struct ScrubbedEnv {
            std::vector<std::string> storage;
            std::vector<char*> pointers; // NULL-terminated, valid while `storage` lives
            int removed = 0;
        };

        ScrubbedEnv BuildChildEnv(const std::string& imageDir) {
            // `environ` is glibc/bionic's, declared by <unistd.h> at global
            // scope. Re-declaring it inside this anonymous namespace made it a
            // NEW symbol that nothing defines - which the linker caught, and
            // which is worth a comment because the error names a mangled name
            // that looks nothing like the variable.
            ScrubbedEnv env;
            for (char** e = ::environ; e != nullptr && *e != nullptr; ++e) {
                if (ShouldScrub(*e)) {
                    ++env.removed;
                    continue;
                }
                env.storage.emplace_back(*e);
            }
            // (a) of the two catches: the child is told, structurally, that it
            // must not dial. Separate from which ROLE it plays - CONTRACT-P6
            // §3.1 is the whole argument for why those are two axes.
            env.storage.emplace_back("MOBILEGL_IPC_ROLE=server");
            env.storage.emplace_back("MOBILEGL_IPC_DIAL=no");
            // LD_LIBRARY_PATH, BECAUSE THE SERVER IS EXEC'd AND NOT dlopen'd.
            //
            // It links libMobileGL.so, and an exec'd process gets a FRESH linker
            // namespace with the default search path - not the one the app built
            // for its own libraries. An APK's lib/<abi>/ is not on that path, and
            // the failure is total and immediate:
            //
            //   CANNOT LINK EXECUTABLE ".../libMobileGLServer.so":
            //   library "libMobileGL.so" not found: needed by main executable
            //
            // It is invisible on desktop, where CMake gives build-tree targets an
            // absolute RPATH of their own - so the WSL server had been resolving
            // its dependency through a path no phone has.
            //
            // $ORIGIN WOULD BE THE TIDIER ANSWER and the target asks for it, but
            // the NDK toolchain drops RPATH from the link: measured with readelf
            // on the APK's own copy, which carries NEEDED libMobileGL.so and no
            // RUNPATH at all. So the launcher states it instead - it is the one
            // component that has already resolved where the image lives, and a
            // pair that is not co-located is not a pair.
            if (!imageDir.empty()) {
                env.storage.emplace_back("LD_LIBRARY_PATH=" + imageDir);
            }
            env.pointers.reserve(env.storage.size() + 1);
            for (auto& entry : env.storage) {
                env.pointers.push_back(entry.data());
            }
            env.pointers.push_back(nullptr);
            return env;
        }

    } // namespace

    int CountOwnChildren() {
        const pid_t self = ::getpid();
        DIR* proc = ::opendir("/proc");
        if (proc == nullptr) {
            return -1; // not Linux-shaped; the gate must say so rather than read 0
        }
        int children = 0;
        while (const dirent* entry = ::readdir(proc)) {
            if (entry->d_name[0] < '0' || entry->d_name[0] > '9') {
                continue;
            }
            std::string statPath = std::string("/proc/") + entry->d_name + "/stat";
            FILE* file = std::fopen(statPath.c_str(), "r");
            if (file == nullptr) {
                continue;
            }
            // `comm` can contain spaces and parentheses, so parse from the LAST
            // ')' rather than by field index - the classic /proc/pid/stat trap.
            char buffer[512] = {};
            const std::size_t read = std::fread(buffer, 1, sizeof(buffer) - 1, file);
            std::fclose(file);
            if (read == 0) {
                continue;
            }
            const char* close = std::strrchr(buffer, ')');
            if (close == nullptr) {
                continue;
            }
            int ppid = 0;
            char state = 0;
            if (std::sscanf(close + 1, " %c %d", &state, &ppid) == 2 && ppid == self) {
                ++children;
            }
        }
        ::closedir(proc);
        return children;
    }

    MobileGLResult LaunchServer(const std::string& imagePath, const std::string& endpoint,
                                LaunchedServer* out) {
        if (out == nullptr || endpoint.empty()) {
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }
        *out = LaunchedServer{};

        const std::string image = ResolveImage(imagePath);
        if (!IsExecutableFile(image)) {
            // NAMED, never a fallback. ConfigLoader.cpp's own comment names the
            // accident this prevents: a lane that asked for spawn, silently got
            // monolith, and went green on the wrong arm.
            Transport::WireLogError(
                "MG_Remote spawn: the server image is not executable: \"%s\" - refusing. Set "
                "MOBILEGL_IPC_SERVER_PATH, or place libMobileGLServer.so beside libMobileGL.so. "
                "There is NO monolith fallback from here.",
                image.empty() ? "<unresolved>" : image.c_str());
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }

        // The image's directory, which is where its libMobileGL.so is.
        const std::string::size_type slash = image.find_last_of('/');
        ScrubbedEnv env =
            BuildChildEnv(slash == std::string::npos ? std::string() : image.substr(0, slash));
        std::string argv0 = image;
        std::string argv1 = endpoint;
        char* argv[] = {argv0.data(), argv1.data(), nullptr};

        // A close-on-exec pipe so an execve that fails reports its errno back
        // instead of vanishing: the app process's stderr is /dev/null on Android
        // (ARCHITECTURE.md §15.2), so a silent exec failure would look exactly
        // like a server that started and then died.
        int report[2] = {-1, -1};
        if (::pipe(report) != 0) {
            return MOBILEGL_ERR_UNSUPPORTED;
        }
        ::fcntl(report[1], F_SETFD, FD_CLOEXEC);

        const pid_t pid = ::fork();
        if (pid < 0) {
            Transport::WireLogError("MG_Remote spawn: fork failed: %s", std::strerror(errno));
            ::close(report[0]);
            ::close(report[1]);
            return MOBILEGL_ERR_UNSUPPORTED;
        }

        if (pid == 0) {
            // ---- child: async-signal-safe calls ONLY until execve -------------
            // NOTHING IS HANDED OVER. No dup2, no fixed fd numbers, no
            // close-on-exec dance - the child gets an endpoint name in argv and
            // finds everything else by listening. That is the whole difference
            // between a launcher and a fork-coupled pair, and it is why this
            // function is now twenty lines shorter than the version that tried
            // to be both.
            ::close(report[0]);
            ::execve(image.c_str(), argv, env.pointers.data());
            const int failure = errno;
            const ssize_t ignored = ::write(report[1], &failure, sizeof(failure));
            (void)ignored;
            ::_exit(127);
        }

        // ---- parent ---------------------------------------------------------
        ::close(report[1]);
        int childErrno = 0;
        const ssize_t got = ::read(report[0], &childErrno, sizeof(childErrno));
        ::close(report[0]);
        if (got == static_cast<ssize_t>(sizeof(childErrno))) {
            Transport::WireLogError("MG_Remote spawn: execve(\"%s\") failed in the child: %s",
                                    image.c_str(), std::strerror(childErrno));
            int status = 0;
            ::waitpid(pid, &status, 0);
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }

        out->pid = static_cast<int>(pid);
        out->endpoint = endpoint;
        Transport::WireLogError("MG_Remote spawn: server pid=%d image=\"%s\" endpoint=\"%s\" "
                                "transport=spawn (env scrubbed: %d entries removed)",
                                out->pid, image.c_str(), endpoint.c_str(), env.removed);
        return MOBILEGL_OK;
    }

    MobileGLResult ReapServer(LaunchedServer& server, std::uint32_t timeoutMs,
                              int* outExitCode) {
        if (server.pid < 0) {
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }
        // NOTHING TO CLOSE HERE ANY MORE. The launcher holds no descriptor
        // into the child: whoever owns the CONNECTION closes it, and that EOF is
        // what the child exits on. A launcher that also owned the connection is
        // what made the two processes one unit.

        const auto deadline = timeoutMs;
        std::uint32_t waited = 0;
        for (;;) {
            int status = 0;
            const pid_t done = ::waitpid(server.pid, &status, WNOHANG);
            if (done == server.pid) {
                if (outExitCode != nullptr) {
                    *outExitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status);
                }
                server.pid = -1;
                return MOBILEGL_OK;
            }
            if (done < 0) {
                return MOBILEGL_ERR_INVALID_ARGUMENT;
            }
            if (waited >= deadline) {
                return MOBILEGL_ERR_TIMEOUT;
            }
            ::usleep(1000);
            waited += 1;
        }
    }

#else // !MOBILEGL_SERVER_SPAWN_POSIX

    MobileGLResult LaunchServer(const std::string&, const std::string&, LaunchedServer*) {
        Transport::WireLogError("MG_Remote spawn: unsupported on this platform - P6 lands POSIX only "
                     "(CONTRACT-P6 §2.6); there is no fork() to build a second process from");
        return MOBILEGL_ERR_UNSUPPORTED;
    }
    MobileGLResult ReapServer(LaunchedServer&, std::uint32_t, int*) {
        return MOBILEGL_ERR_UNSUPPORTED;
    }
    int CountOwnChildren() { return -1; }

#endif

} // namespace MobileGL::MG_Remote::Server
