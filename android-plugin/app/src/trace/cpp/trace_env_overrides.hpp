#pragma once

// The generic environment passthrough of the retrace lane, split out of
// trace_replay_jni.cpp and trace_replay_core.cpp so a host-side test can pin it.
//
// One intent extra (`--es mobilegl_env "K=V;K=V"`) carries every MOBILEGL_* knob that has
// no dedicated flag, which is what PLAN-B.md §11 P0 needs when it adds MOBILEGL_PIPE_*.
// That makes this hand-rolled two-level parse the single point where the whole batch can
// be silently misread, and the only lane that exercises it end to end runs on a device -
// hence tools/trace_replay/trace_env_overrides_test.cpp, which every desktop configure
// that builds the replay runner runs at build time.

#include <cstddef>
#include <string>
#include <vector>

namespace mobilegl_trace {

// Splits `A;B;C` into its entries, dropping empty ones. Also used for the texture and
// FBO dump lists, whose entries carry their own ',' and ':' separators. A value that
// itself contains ';' therefore cannot be expressed - that is the format's limit, not a
// bug to work around here.
inline std::vector<std::string> SplitSemicolonList(const std::string& value) {
    std::vector<std::string> values;
    std::size_t begin = 0;
    while (begin < value.size()) {
        const std::size_t end = value.find(';', begin);
        const std::string entry = value.substr(begin, end - begin);
        if (!entry.empty()) {
            values.push_back(entry);
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return values;
}

enum class EnvOverrideAction {
    // Nothing to do: the entry is empty, or names an empty key.
    Ignore,
    // setenv(key, value, 1). `K=` is a Set of the empty string, deliberately distinct
    // from Unset: a knob read with getenv() != nullptr treats them differently.
    Set,
    // unsetenv(key). An entry with no '=' means this, and it is the only way for a
    // caller to clear a variable the per-knob marshalling above it already set.
    Unset,
};

// Classifies one `KEY=VALUE` / `KEY` entry. The first '=' separates; later ones belong to
// the value, so `KEY=a=b` sets KEY to `a=b`.
inline EnvOverrideAction ParseEnvOverride(const std::string& entry,
                                          std::string* key,
                                          std::string* value) {
    key->clear();
    value->clear();
    const std::size_t separator = entry.find('=');
    if (separator == std::string::npos) {
        if (entry.empty()) {
            return EnvOverrideAction::Ignore;
        }
        *key = entry;
        return EnvOverrideAction::Unset;
    }
    if (separator == 0) {
        return EnvOverrideAction::Ignore;
    }
    *key = entry.substr(0, separator);
    *value = entry.substr(separator + 1);
    return EnvOverrideAction::Set;
}

} // namespace mobilegl_trace
