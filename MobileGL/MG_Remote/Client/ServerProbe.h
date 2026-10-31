// MobileGL - MobileGL/MG_Remote/Client/ServerProbe.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// Whether the server this process would dial is listening. As the system's EGL/GLX vendor this
// library is asked first by every GL process, and session bring-up would wait up to 20 s for an
// absent server; the EGL/GLX dispatchers and the GBM loader move on when a vendor declines, so
// the library declines early instead. Only a connect dial is probed (MOBILEGL_TRANSPORT=spawn
// with MOBILEGL_IPC_CONTROL = unix:<path>, unix:@<name> or tcp://host:port); anything else
// answers NotApplicable. Settings are read as the config loader reads them (environment over
// system files) because the probe runs before MobileGL initializes.
// MOBILEGL_IPC_PROBE_TIMEOUT_MS bounds one probe (default 250, range 1..60000); 0 disables it.

#pragma once

#include <Includes.h>

#include <string>

namespace MobileGL::MG_Remote::Client {

    enum class ServerProbeResult {
        NotApplicable, // nothing to dial: the configuration serves the process another way
        Reachable,     // something accepted a connection at the configured endpoint
        Unreachable,   // the configured endpoint refused, does not exist, or did not answer in time
    };

    struct ServerProbeTarget {
        std::string endpoint; // SocketTransport form: a path, "@name", or "tcp://host:port"
        Uint32 timeoutMs = 0; // 0: probing disabled
    };

    // What the configuration says to probe; false when it names nothing probeable (NotApplicable).
    Bool ConfiguredProbeTarget(ServerProbeTarget* out);

    // One probe of the configured server, uncached.
    ServerProbeResult ProbeConfiguredServer();

    // The cached form the entry points use: once a probe found the server (or there is nothing to
    // probe) the answer stays true for the life of the process; a negative answer is re-probed on
    // the next call, which is one refused connect.
    Bool ConfiguredServerAvailable();

    // Forgets a positive answer (tests; and the library's teardown, so a re-initialization asks again).
    void ResetServerProbeCache();

} // namespace MobileGL::MG_Remote::Client
