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

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

namespace MGITest::PipeStatsWindow {

    // The lane's private log path, or empty when the lane configured none.
    inline std::string LibraryLogPath() {
        const char* path = std::getenv("MOBILEGL_LOG_FILE_PATH");
        return (path != nullptr && *path != '\0') ? std::string(path) : std::string();
    }

    inline std::string ReadWholeFile(const std::string& path) {
        if (path.empty()) return {};
        std::ifstream file(path, std::ios::binary);
        if (!file.good()) return {};
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

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

    inline Window LastFromLaneLog() { return Last(ReadWholeFile(LibraryLogPath())); }

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

} // namespace MGITest::PipeStatsWindow
