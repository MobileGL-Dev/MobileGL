// MobileGL - MobileGL/MG_Remote/Client/ServerProbe.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "ServerProbe.h"

#include <Init.h>
#include <MG_Remote/Transport/SocketTransport.h>
#include <MG_Util/Debug/Log.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>

namespace MobileGL::MG_Remote::Client {

    namespace {
        constexpr Uint32 kDefaultProbeTimeoutMs = 250;
        constexpr Uint32 kMaxProbeTimeoutMs = 60000;

        std::atomic<Bool> g_available{false};
        std::atomic<Bool> g_declineLogged{false};

        std::string Lowered(std::string text) {
            std::transform(text.begin(), text.end(), text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        Uint32 ProbeTimeoutMs() {
            String text;
            if (!MG_ConfigLoader::LookupSetting("MOBILEGL_IPC_PROBE_TIMEOUT_MS", text)) return kDefaultProbeTimeoutMs;
            char* end = nullptr;
            errno = 0;
            const unsigned long value = std::strtoul(text.c_str(), &end, 10);
            if (end == text.c_str() || *end != '\0' || errno == ERANGE || value > kMaxProbeTimeoutMs) {
                return kDefaultProbeTimeoutMs;
            }
            return static_cast<Uint32>(value);
        }
    } // namespace

    Bool ConfiguredProbeTarget(ServerProbeTarget* out) {
        String transport;
        String control;
        if (!MG_ConfigLoader::LookupSetting("MOBILEGL_TRANSPORT", transport) || Lowered(transport) != "spawn") {
            return false;
        }
        if (!MG_ConfigLoader::LookupSetting("MOBILEGL_IPC_CONTROL", control)) return false; // "fork": launches its own
        std::string endpoint;
        if (control.compare(0, 5, "unix:") == 0) {
            endpoint = control.substr(5);
        } else if (control.compare(0, 6, "tcp://") == 0) {
            endpoint = control;
        } else {
            return false; // fork, fd:<control>,<aux>: nothing to dial
        }
        if (endpoint.empty()) return false;
        if (out != nullptr) {
            out->endpoint = endpoint;
            out->timeoutMs = ProbeTimeoutMs();
        }
        return true;
    }

    ServerProbeResult ProbeConfiguredServer() {
        ServerProbeTarget target;
        if (!ConfiguredProbeTarget(&target) || target.timeoutMs == 0) return ServerProbeResult::NotApplicable;
        return Transport::SocketTransport::Probe(target.endpoint, target.timeoutMs) ? ServerProbeResult::Reachable
                                                                                    : ServerProbeResult::Unreachable;
    }

    Bool ConfiguredServerAvailable() {
        if (g_available.load(std::memory_order_acquire)) return true;
        const auto started = std::chrono::steady_clock::now();
        const ServerProbeResult result = ProbeConfiguredServer();
        if (result != ServerProbeResult::Unreachable) {
            g_available.store(true, std::memory_order_release);
            return true;
        }
        if (!g_declineLogged.exchange(true)) {
            ServerProbeTarget target;
            (void)ConfiguredProbeTarget(&target);
            const auto tookUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now() - started).count();
            MGLOG_I("MG_Remote client: no MobileGL server answers at \"%s\" (probed in %lld us); declining, so "
                    "the system's next GL implementation serves this process",
                    target.endpoint.c_str(), static_cast<long long>(tookUs));
        }
        return false;
    }

    void ResetServerProbeCache() {
        g_available.store(false, std::memory_order_release);
        g_declineLogged.store(false, std::memory_order_release);
    }

} // namespace MobileGL::MG_Remote::Client
