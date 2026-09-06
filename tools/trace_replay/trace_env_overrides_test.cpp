// Host-side check for the retrace lane's `K=V;K=V` environment passthrough.
//
// The passthrough is the one hand-rolled parse between a CI flag and setenv(), and the
// only lane that runs it end to end is an on-device retrace - too slow and too indirect
// to notice a splitting bug, and it would report the bug as "the knob had no effect".
// This program is built and RUN at build time by every desktop configure that builds
// mobilegl_trace_replay (tools/trace_replay/CMakeLists.txt), so the CI job that only
// builds the runner still exercises it.
//
// Deliberately assert-free: the retrace lane configures Release, NDEBUG is defined, and
// <cassert> would compile every check away into a green run that checked nothing.

#include "trace_env_overrides.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

int gFailures = 0;

void ExpectSplit(const std::string& input, const std::vector<std::string>& expected) {
    const std::vector<std::string> actual = mobilegl_trace::SplitSemicolonList(input);
    if (actual == expected) {
        return;
    }
    ++gFailures;
    std::cerr << "SplitSemicolonList(\"" << input << "\") gave " << actual.size()
              << " entries, expected " << expected.size() << ":";
    for (const std::string& entry : actual) {
        std::cerr << " [" << entry << "]";
    }
    std::cerr << "\n";
}

const char* ActionName(mobilegl_trace::EnvOverrideAction action) {
    switch (action) {
        case mobilegl_trace::EnvOverrideAction::Ignore:
            return "Ignore";
        case mobilegl_trace::EnvOverrideAction::Set:
            return "Set";
        case mobilegl_trace::EnvOverrideAction::Unset:
            return "Unset";
    }
    return "?";
}

void ExpectParse(const std::string& entry,
                 mobilegl_trace::EnvOverrideAction expectedAction,
                 const std::string& expectedKey,
                 const std::string& expectedValue) {
    std::string key = "<untouched>";
    std::string value = "<untouched>";
    const mobilegl_trace::EnvOverrideAction action =
            mobilegl_trace::ParseEnvOverride(entry, &key, &value);
    if (action == expectedAction && key == expectedKey && value == expectedValue) {
        return;
    }
    ++gFailures;
    std::cerr << "ParseEnvOverride(\"" << entry << "\") gave " << ActionName(action) << " key=["
              << key << "] value=[" << value << "], expected " << ActionName(expectedAction)
              << " key=[" << expectedKey << "] value=[" << expectedValue << "]\n";
}

} // namespace

int main() {
    using mobilegl_trace::EnvOverrideAction;

    // Splitting.
    ExpectSplit("", {});
    ExpectSplit("MOBILEGL_PIPE_PUSH=1", {"MOBILEGL_PIPE_PUSH=1"});
    ExpectSplit("MOBILEGL_PIPE_PUSH=1;MOBILEGL_PIPE_VERIFY=1",
                {"MOBILEGL_PIPE_PUSH=1", "MOBILEGL_PIPE_VERIFY=1"});
    // A trailing ';' is what a caller that joins a list gets for free, and an empty entry
    // must not be turned into an unsetenv("") - the whole passthrough would then depend on
    // how carefully the shell script trimmed its own string.
    ExpectSplit("A=1;", {"A=1"});
    ExpectSplit(";;A=1;;B=2;;", {"A=1", "B=2"});
    ExpectSplit(";", {});
    // The values the plan's knobs actually carry: a path, a size, a comma list.
    ExpectSplit("MOBILEGL_PIPE_TEXEL_RETAIN_MB=64;MOBILEGL_LOG_FILE_PATH=/sdcard/MG/a.log",
                {"MOBILEGL_PIPE_TEXEL_RETAIN_MB=64", "MOBILEGL_LOG_FILE_PATH=/sdcard/MG/a.log"});

    // Classification.
    ExpectParse("MOBILEGL_PIPE_PUSH=1", EnvOverrideAction::Set, "MOBILEGL_PIPE_PUSH", "1");
    // `K=` is an empty value, NOT an unset: a knob tested with getenv() != nullptr sees
    // those two as opposite answers.
    ExpectParse("MOBILEGL_PIPE_PUSH=", EnvOverrideAction::Set, "MOBILEGL_PIPE_PUSH", "");
    // No '=' means unset - the only way to clear a default the per-knob marshalling set.
    ExpectParse("MOBILEGL_PIPE_PUSH", EnvOverrideAction::Unset, "MOBILEGL_PIPE_PUSH", "");
    // Only the FIRST '=' separates, so a value may contain '='. Anything else would
    // truncate a base64 or a query-string-shaped value without a word of warning.
    ExpectParse("MOBILEGL_A=b=c", EnvOverrideAction::Set, "MOBILEGL_A", "b=c");
    ExpectParse("MOBILEGL_A==", EnvOverrideAction::Set, "MOBILEGL_A", "=");
    // An empty key must never reach setenv/unsetenv, which would be EINVAL at best.
    ExpectParse("", EnvOverrideAction::Ignore, "", "");
    ExpectParse("=1", EnvOverrideAction::Ignore, "", "");
    ExpectParse("=", EnvOverrideAction::Ignore, "", "");

    if (gFailures != 0) {
        std::cerr << "trace env passthrough: " << gFailures << " check(s) failed\n";
        return 1;
    }
    std::cout << "trace env passthrough: all checks passed\n";
    return 0;
}
