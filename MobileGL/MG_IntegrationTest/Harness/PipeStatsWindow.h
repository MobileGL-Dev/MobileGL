// MobileGL - MobileGL/MG_IntegrationTest/Harness/PipeStatsWindow.h
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Reading ONE PipeStats summary window out of the library's own log, for the scenarios whose
// claim is about a counter rather than about pixels.
//
// WHY THROUGH A LOG FILE AT ALL. MG_Util::PipeStats is internal to the library and this module
// cannot link against it (ScenarioFixture.h has the long version: on Android this binary links
// the SHIPPING libMobileGL.so, built -fvisibility=hidden). The library's `MGPipe stats:` line is
// the only channel, so a lane that wants to read a counter sets MOBILEGL_PIPE_STATS=1,
// MOBILEGL_PIPE_STATS_PERIOD=1 - one line per eglSwapBuffers - and a MOBILEGL_LOG_FILE_PATH of
// its OWN.
//
// THE LOG PATH HAS TO BE PRIVATE TO ONE CTEST ENTRY, and that is not a style rule: the library
// opens it fopen(path, "w"), so every process launched in a lane TRUNCATES it. Two entries of one
// lane reading the same path race under `ctest -j`, and the shape of the failure is an empty read
// that looks exactly like "the counter was never emitted". So a case that reads a window gets a
// ctest entry whose TEST_FILTER selects that case alone, with a log path nothing else writes -
// the rule PipeVerifyArmingScenario and CsoContentAddressingScenario already follow.
//
// THE WINDOW IS "SINCE THE PREVIOUS LINE" (PipeStats::FormatWindowLine), so the caller closes the
// setup window with a swap, runs the workload, swaps again, and reads the LAST line - which then
// covers the workload and nothing else.

#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

#if MOBILEGL_BUILD_DISAGGREGATED
extern "C" void MGPipeSyncPeerLog();
#endif

namespace MGITest::PipeStatsWindow {

    // P6: MOBILEGL_LOG_FILE_PATH IS A BASE NAME, NOT A FILE. The library writes one log per
    // ROLE - `<stem>.client<ext>` and `<stem>.server<ext>` - because under inproc both roles are
    // threads of one process and a single file made every per-side assertion a search.
    //
    // THE RULE IS COPIED HERE AND IT HAS TO BE, which is worth stating because the obvious fix is
    // to call the library's own MG_Util::Debug::RoleLogPath and delete this. That does not link:
    // this module links the SHIPPING libMobileGL.so, built -fvisibility=hidden, and the role-path
    // helpers are `t` (local) in it - only MG_Test, which links the static archive, may call them.
    //
    // SO THE COPY IS FLAVOUR-AWARE INSTEAD. That is the part the first copy got wrong: it derived
    // `.client` in BOTH flavours, while the pull build has one role and writes
    // MOBILEGL_LOG_FILE_PATH unchanged. In the verify lane it therefore opened a file that never
    // existed and every reader reported "the library never logged" - a product failure that had
    // not happened. Keep this `#if` and Log.cpp's RoleLogPath in step.
    inline std::string RoleLogPath(const char* roleSuffix) {
        const char* base = std::getenv("MOBILEGL_LOG_FILE_PATH");
        if (base == nullptr || *base == '\0') return {};
#if MOBILEGL_BUILD_DISAGGREGATED
        std::string path(base);
        const std::string::size_type slash = path.find_last_of("/\\");
        const std::string::size_type dot = path.find_last_of('.');
        if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
            return path + "." + roleSuffix;
        }
        return path.substr(0, dot) + "." + roleSuffix + path.substr(dot);
#else
        (void)roleSuffix;
        return std::string(base);
#endif
    }

    // The lane's private CLIENT log path, or empty when the lane configured none. In the pull
    // build this IS the whole log.
    inline std::string LibraryLogPath() { return RoleLogPath("client"); }

    // The server role's half, or EMPTY when this build has no separate server half. Empty rather
    // than "the same path again": every caller below concatenates the two, and a pull build that
    // named one file twice would show each line twice - which reads as a doubled counter or a
    // repeated diagnostic rather than as a path mistake.
    inline std::string ServerLibraryLogPath() {
#if MOBILEGL_BUILD_DISAGGREGATED
        return RoleLogPath("server");
#else
        return {};
#endif
    }

    inline std::string ReadWholeFile(const std::string& path) {
        if (path.empty()) return {};
        std::ifstream file(path, std::ios::binary);
        if (!file.good()) return {};
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    inline std::string ReadFileSince(const std::string& path, std::uintmax_t offset) {
        if (path.empty()) return {};
        std::ifstream file(path, std::ios::binary);
        if (!file.good()) return {};
        file.seekg(static_cast<std::streamoff>(offset));
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    // A "the log is this long right now" snapshot, ONE OFFSET PER ROLE.
    //
    // The scenarios that assert on arming take a mark before the workload and read what was
    // appended after it, so that only bytes this case caused can satisfy - or refute - the claim.
    // The roles are separate files, so this cannot be one scalar: a single offset applied to the
    // concatenation would slide by however much the OTHER role happened to write, and the read
    // would start mid-line in the wrong file.
    struct LogMark {
        std::uintmax_t client = 0;
        std::uintmax_t server = 0;
    };

    inline std::uintmax_t FileSizeOrZero(const std::string& path) {
        if (path.empty()) return 0;
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.good()) return 0;
        const std::streamoff size = file.tellg();
        return size < 0 ? 0 : static_cast<std::uintmax_t>(size);
    }

    inline LogMark MarkLaneLog() {
#if MOBILEGL_BUILD_DISAGGREGATED
        MGPipeSyncPeerLog();
#endif
        return LogMark{FileSizeOrZero(LibraryLogPath()), FileSizeOrZero(ServerLibraryLogPath())};
    }

    // BOTH ROLES, and for the arming diagnostics the SERVER's is the one that matters. Those lines
    // are emitted by the BACKEND - "demoted to an ordinary varying", the reroute and emulation
    // banners - and under inproc the backend runs on the apply thread, which is the server role.
    // Reading only the client's half finds nothing and reports "the emulation is not armed",
    // which is the most expensive possible way to be wrong: it accuses the product of a defect
    // that the reader itself invented.
    inline std::string ReadLaneLogSince(const LogMark& mark) {
#if MOBILEGL_BUILD_DISAGGREGATED
        MGPipeSyncPeerLog();
#endif
        return ReadFileSince(LibraryLogPath(), mark.client)
               + ReadFileSince(ServerLibraryLogPath(), mark.server);
    }

    inline std::string ReadLaneLog() { return ReadLaneLogSince(LogMark{}); }

    // The last summary line in the log, verbatim. `found` is false when the library never emitted
    // one, which is a different failure from "the counter read zero" and has to be reported as
    // one: it means the stats channel never reached the process, not that the workload did
    // nothing.
    struct Window {
        bool found = false;
        std::string line;
    };

    inline Window Last(const std::string& log) {
        Window window;
        const std::string marker = "MGPipe stats:";
        const std::size_t at = log.rfind(marker);
        if (at == std::string::npos) return window;
        const std::size_t end = log.find('\n', at);
        window.line = log.substr(at, end == std::string::npos ? std::string::npos : end - at);
        window.found = true;
        return window;
    }

    // BOTH ROLES, and the server's is where the numbers are. The `draws` / `vbs` counters
    // increment in the backend's PrepareForDraw, which runs on the APPLY THREAD - the server
    // role - so under inproc split the summary line lands in the server's log. Reading only the
    // client's would find the "counters ON" banner (client-side) but no window, which reads as
    // "the stats channel never reached the process". Concatenated, Last() takes whichever role
    // emitted the final summary.
    inline Window LastFromLaneLog() {
        return Last(ReadWholeFile(LibraryLogPath()) + ReadWholeFile(ServerLibraryLogPath()));
    }

    // One counter out of that line, by its short name ("mpr", "draws", "csom"), or -1 when the
    // line does not carry it. The search includes the SEPARATOR before the name and the `=` after
    // it, so "draws" cannot match "draws/f=" and "mpr" cannot match a longer name ending in it -
    // a substring match here would read a neighbouring counter's value and report it as this
    // one's, which is the one way a counter assertion can be wrong without ever failing.
    inline long long CounterOrAbsent(const Window& window, const char* shortName) {
        if (!window.found) return -1;
        // A counter is preceded either by a space (` mpr=`, ` draws=`) or by its bracket's
        // opening (`cso[csom=`, `bytes/f[stage-buffer=`); nothing in the line is preceded by
        // anything else.
        for (const char* prefix : {" ", "["}) {
            const std::string key = std::string(prefix) + shortName + "=";
            const std::size_t at = window.line.find(key);
            if (at == std::string::npos) continue;
            return std::strtoll(window.line.c_str() + at + key.size(), nullptr, 10);
        }
        return -1;
    }

    // The same lookup for a counter that is printed as a FIXED-POINT PER-FRAME FIGURE rather
    // than as an integer, which is every member of the bytes/f[...] bracket: FormatWindowLine
    // divides each byte class by the window's frame count and prints two decimals whenever the
    // window contains a Present. `pmap` is one of those, so CounterOrAbsent's strtoll reads
    // "0.37" as 0 and an assertion that a push HAPPENED silently becomes an assertion that it
    // pushed at least one whole byte per frame - the one way this counter can be wrong without
    // ever failing. Returns -1.0 when the line does not carry the name; every real value of a
    // byte class is >= 0, so the sentinel cannot collide with one.
    inline double CounterAsDoubleOrAbsent(const Window& window, const char* shortName) {
        if (!window.found) return -1.0;
        for (const char* prefix : {" ", "["}) {
            const std::string key = std::string(prefix) + shortName + "=";
            const std::size_t at = window.line.find(key);
            if (at == std::string::npos) continue;
            return std::strtod(window.line.c_str() + at + key.size(), nullptr);
        }
        return -1.0;
    }

} // namespace MGITest::PipeStatsWindow
