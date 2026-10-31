// MobileGL - tools/trace_replay/trace_replay_lease_test.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "trace_replay_lease.hpp"

#include <iostream>
#include <thread>
#include <vector>

int main() {
    using mobilegl_trace::TraceReplayLease;
    std::atomic<int> rejected{0};
    {
        TraceReplayLease original;
        if (!original) {
            std::cerr << "initial replay could not claim process state\n";
            return 1;
        }
        std::vector<std::thread> contenders;
        for (int i = 0; i < 16; ++i) {
            contenders.emplace_back([&] {
                TraceReplayLease duplicate;
                if (!duplicate) {
                    ++rejected;
                }
            });
        }
        for (auto& thread : contenders) {
            thread.join();
        }
        if (rejected != 16) {
            std::cerr << "a duplicate replay entered the live process state\n";
            return 1;
        }
        // A rejected caller's destructor must not release the real owner's claim.
        TraceReplayLease stillDuplicate;
        if (stillDuplicate) {
            std::cerr << "rejected caller released the original replay's claim\n";
            return 1;
        }
    }
    TraceReplayLease next;
    if (!next) {
        std::cerr << "completed replay did not release process state\n";
        return 1;
    }
    std::cout << "TraceReplayLease: concurrent invocations rejected; owner cleanup retained\n";
    return 0;
}
