// MobileGL - MobileGL/MG_Util/X11/DisplayGuard.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "DisplayGuard.h"

#include <MG_Util/Debug/Log.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#if defined(__linux__)
#include <dirent.h>
#include <unistd.h>
#endif

namespace MobileGL::MG_Util::X11 {
    namespace {
        constexpr const char* kSocketDirectory = "/tmp/.X11-unix/X";
        // __SO_ACCEPTCON in /proc/net/unix's Flags column: the socket listens.
        constexpr unsigned long kAcceptConnectionsFlag = 0x00010000ul;

        Bool AllDigits(const char* begin, const char* end) {
            if (begin == end) return false;
            for (const char* p = begin; p != end; ++p) {
                if (*p < '0' || *p > '9') return false;
            }
            return true;
        }

        Int32 ToNumber(const char* begin, const char* end) {
            if (!AllDigits(begin, end) || end - begin > 6) return -1;
            Int32 value = 0;
            for (const char* p = begin; p != end; ++p) value = value * 10 + (*p - '0');
            return value;
        }

        String ReadWhole(const char* path) {
            std::ifstream in(path, std::ios::binary);
            if (!in) return {};
            std::stringstream all;
            all << in.rdbuf();
            return all.str();
        }

        Bool Forbidden(const char* value) {
            if (value == nullptr) return false;
            return std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0 || std::strcmp(value, "off") == 0 ||
                   std::strcmp(value, "no") == 0;
        }
    } // namespace

    Int32 ParseDisplayNumber(const char* name) {
        if (name == nullptr || name[0] == '\0') return -1;
        const SizeT length = std::strlen(name);
        // A socket path (launchd-style names, and how some tools spell a local display).
        const SizeT dirLength = std::strlen(kSocketDirectory);
        const char* path = name[0] == '@' ? name + 1 : name;
        if (std::strncmp(path, kSocketDirectory, dirLength) == 0) {
            const char* digits = path + dirLength;
            const char* end = digits;
            while (*end >= '0' && *end <= '9') ++end;
            return ToNumber(digits, end);
        }
        // [protocol/][host]:display[.screen]: the LAST colon (an IPv6 host has colons of its own).
        const char* colon = std::strrchr(name, ':');
        if (colon == nullptr) return -1;
        const char* digits = colon + 1;
        const char* end = digits;
        while (*end >= '0' && *end <= '9') ++end;
        if (*end != '\0' && *end != '.') return -1;
        (void)length;
        return ToNumber(digits, end);
    }

    Bool IsXServerProgram(const String& basename) {
        static const char* const kServers[] = {"Xwayland", "Xorg", "Xorg.bin", "X", "Xvfb", "Xephyr",
                                               "Xnest", "Xvnc", "Xwin", "XWin", "Xquartz", "Xfake"};
        for (const char* server : kServers) {
            if (basename == server) return true;
        }
        return false;
    }

    Vector<Uint64> ListeningDisplaySocketInodes(const String& procNetUnix, Int32 number) {
        Vector<Uint64> inodes;
        if (number < 0) return inodes;
        const String wanted = String(kSocketDirectory) + std::to_string(number);
        std::istringstream lines(procNetUnix);
        String line;
        Bool header = true;
        while (std::getline(lines, line)) {
            if (header) { // "Num RefCount Protocol Flags Type St Inode Path"
                header = false;
                if (line.find("Inode") != String::npos) continue;
            }
            std::istringstream fields(line);
            String num, refCount, protocol, flags, type, state, inode, path;
            if (!(fields >> num >> refCount >> protocol >> flags >> type >> state >> inode)) continue;
            if (!(fields >> path)) continue; // unbound
            const unsigned long flagBits = std::strtoul(flags.c_str(), nullptr, 16);
            if ((flagBits & kAcceptConnectionsFlag) == 0) continue;
            const String bare = !path.empty() && path[0] == '@' ? path.substr(1) : path;
            if (bare != wanted) continue;
            inodes.push_back(std::strtoull(inode.c_str(), nullptr, 10));
        }
        return inodes;
    }

    Bool ServesDisplay(const ProcessFacts& facts, Int32 number) {
        if (number < 0) return false;
        for (const Uint64 listening : ListeningDisplaySocketInodes(facts.ProcNetUnix, number)) {
            for (const Uint64 own : facts.OwnSocketInodes) {
                if (own == listening) return true;
            }
        }
        return false;
    }

    Bool MayDial(const char* displayName, const ProcessFacts& facts, const char* overrideValue) {
        if (Forbidden(overrideValue)) return false;
        if (displayName == nullptr || displayName[0] == '\0') return false;
        // An X server never dials a display: the one it would reach first is likely its own, and
        // nothing it draws needs another one.
        if (IsXServerProgram(facts.ExeBasename)) return false;
        const Int32 number = ParseDisplayNumber(displayName);
        return number < 0 || !ServesDisplay(facts, number);
    }

    ProcessFacts ReadProcessFacts() {
        ProcessFacts facts;
#if defined(__linux__)
        char exe[4096] = {};
        const ssize_t length = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (length > 0) {
            exe[length] = '\0';
            const char* slash = std::strrchr(exe, '/');
            facts.ExeBasename = slash != nullptr ? slash + 1 : exe;
        }
        facts.ProcNetUnix = ReadWhole("/proc/self/net/unix");
        if (DIR* fds = ::opendir("/proc/self/fd")) {
            while (const dirent* entry = ::readdir(fds)) {
                if (entry->d_name[0] == '.') continue;
                const String link = String("/proc/self/fd/") + entry->d_name;
                char target[128] = {};
                const ssize_t n = ::readlink(link.c_str(), target, sizeof(target) - 1);
                if (n <= 0) continue;
                target[n] = '\0';
                // "socket:[12345]"
                if (std::strncmp(target, "socket:[", 8) != 0) continue;
                facts.OwnSocketInodes.push_back(std::strtoull(target + 8, nullptr, 10));
            }
            ::closedir(fds);
        }
#endif
        return facts;
    }

    Bool MayDialX11Display(const char* displayName) {
        const char* overrideValue = std::getenv("MOBILEGL_X11_DIAL");
        if (displayName == nullptr || displayName[0] == '\0') return false;
        const Bool allowed = MayDial(displayName, ReadProcessFacts(), overrideValue);
        if (!allowed) {
            static std::atomic<Bool> logged{false};
            if (!logged.exchange(true)) {
                MGLOG_I("X11: not opening a connection to display '%s' - this process serves it (an X server), or "
                        "MOBILEGL_X11_DIAL forbids it",
                        displayName);
            }
        }
        return allowed;
    }
} // namespace MobileGL::MG_Util::X11
