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
        bool ShouldScrub(const char* entry) {
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

        ScrubbedEnv BuildChildEnv() {
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

    MobileGLResult SpawnServer(const std::string& imagePath, SpawnedServer* out) {
        if (out == nullptr) {
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }
        *out = SpawnedServer{};

        const std::string image = ResolveImage(imagePath);
        if (!IsExecutableFile(image)) {
            // NAMED, never a fallback. ConfigLoader.cpp's own comment names the
            // accident this prevents: a lane that asked for spawn, silently got
            // monolith, and went green on the wrong arm.
            Transport::WireLogError("MG_Remote spawn: the server image is not executable: \"%s\" - refusing. "
                         "Set MOBILEGL_IPC_SERVER_PATH, or place libMobileGLServer.so beside "
                         "libMobileGL.so. There is NO monolith fallback from here.",
                         image.empty() ? "<unresolved>" : image.c_str());
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }

        int stream[2] = {-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, stream) != 0) {
            Transport::WireLogError("MG_Remote spawn: socketpair failed: %s", std::strerror(errno));
            return MOBILEGL_ERR_UNSUPPORTED;
        }
        int aux[2] = {-1, -1};
        if (Transport::FdPassing::CreateSocketPair(aux) != MOBILEGL_OK) {
            ::close(stream[0]);
            ::close(stream[1]);
            return MOBILEGL_ERR_UNSUPPORTED;
        }

        ScrubbedEnv env = BuildChildEnv();
        std::string argv0 = image;
        char* argv[] = {argv0.data(), nullptr};

        // A close-on-exec pipe so an execve that fails reports its errno back
        // instead of vanishing: the app process's stderr is /dev/null on
        // Android (ARCHITECTURE.md §15.2), so a silent exec failure would look
        // exactly like a server that started and then died.
        int report[2] = {-1, -1};
        if (::pipe(report) != 0) {
            ::close(stream[0]); ::close(stream[1]);
            ::close(aux[0]); ::close(aux[1]);
            return MOBILEGL_ERR_UNSUPPORTED;
        }
        ::fcntl(report[1], F_SETFD, FD_CLOEXEC);

        const pid_t pid = ::fork();
        if (pid < 0) {
            Transport::WireLogError("MG_Remote spawn: fork failed: %s", std::strerror(errno));
            ::close(stream[0]); ::close(stream[1]);
            ::close(aux[0]); ::close(aux[1]);
            ::close(report[0]); ::close(report[1]);
            return MOBILEGL_ERR_UNSUPPORTED;
        }

        if (pid == 0) {
            // ---- child: async-signal-safe calls ONLY until execve -----------
            //
            // dup2 both fixes the number and clears close-on-exec, which the aux
            // socket has set (FdPassing.cpp:89-90). But a source fd may ALREADY
            // BE the target number - in a gtest process fds 3..8 are exactly
            // where a fresh socketpair lands - and dup2(n, n) is specified to do
            // nothing at all, INCLUDING not clearing close-on-exec. Closing the
            // source afterwards then closes the descriptor we just "installed".
            // That is this function's one genuinely subtle line, so it is spelled
            // out rather than written cleverly.
            const int sources[2] = {stream[1], aux[1]};
            const int targets[2] = {kChildStreamFd, kChildAuxFd};
            for (int i = 0; i < 2; ++i) {
                if (sources[i] == targets[i]) {
                    ::fcntl(targets[i], F_SETFD, 0); // already in place: just un-CLOEXEC it
                } else {
                    ::dup2(sources[i], targets[i]);
                }
            }
            // Close the originals, but never a descriptor that IS one of the two
            // we just installed.
            const int spare[5] = {stream[0], stream[1], aux[0], aux[1], report[0]};
            for (int fd : spare) {
                if (fd != kChildStreamFd && fd != kChildAuxFd) {
                    ::close(fd);
                }
            }
            ::execve(image.c_str(), argv, env.pointers.data());
            // Only reachable when execve failed.
            const int failure = errno;
            const ssize_t ignored = ::write(report[1], &failure, sizeof(failure));
            (void)ignored;
            ::_exit(127);
        }

        // ---- parent ----------------------------------------------------------
        ::close(stream[1]);
        ::close(aux[1]);
        ::close(report[1]);

        int childErrno = 0;
        const ssize_t got = ::read(report[0], &childErrno, sizeof(childErrno));
        ::close(report[0]);
        if (got == static_cast<ssize_t>(sizeof(childErrno))) {
            // The pipe only carries bytes when execve failed.
            Transport::WireLogError("MG_Remote spawn: execve(\"%s\") failed in the child: %s",
                         image.c_str(), std::strerror(childErrno));
            ::close(stream[0]);
            ::close(aux[0]);
            int status = 0;
            ::waitpid(pid, &status, 0);
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }

        out->pid = static_cast<int>(pid);
        out->transport = std::make_unique<Transport::SocketTransport>(
            stream[0], aux[0], Transport::TransportRole::Client);
        Transport::WireLogError("MG_Remote spawn: server pid=%d image=\"%s\" transport=spawn "
                     "(env scrubbed: %d entries removed)",
                     out->pid, image.c_str(), env.removed);
        return MOBILEGL_OK;
    }

    MobileGLResult ReapServer(SpawnedServer& server, std::uint32_t timeoutMs, int* outExitCode) {
        if (server.pid < 0) {
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }
        // Closing our end is what the child sees as EOF, and EOF is what it
        // exits on. Doing it here rather than making the caller remember is the
        // difference between "reap" and "reap, eventually, if you also did the
        // other thing".
        if (server.transport) {
            server.transport->Shutdown();
        }

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

    MobileGLResult SpawnServer(const std::string&, SpawnedServer*) {
        Transport::WireLogError("MG_Remote spawn: unsupported on this platform - P6 lands POSIX only "
                     "(CONTRACT-P6 §2.6); there is no fork() to build a second process from");
        return MOBILEGL_ERR_UNSUPPORTED;
    }
    MobileGLResult ReapServer(SpawnedServer&, std::uint32_t, int*) {
        return MOBILEGL_ERR_UNSUPPORTED;
    }
    int CountOwnChildren() { return -1; }

#endif

} // namespace MobileGL::MG_Remote::Server
