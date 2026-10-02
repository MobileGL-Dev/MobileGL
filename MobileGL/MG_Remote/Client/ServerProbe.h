// MobileGL - MobileGL/MG_Remote/Client/ServerProbe.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// IS THE SERVER THIS PROCESS WOULD DIAL THERE AT ALL.
//
// Installed as the system's EGL/GLX vendor, this library is asked first by every GL process on the
// machine, and the session bring-up behind its first display waits up to 20 s for a server that
// may simply not be running. The loaders that ask (EGL and GLX dispatchers, the GBM loader) all
// move on to the next implementation when one declines - so the honest, cheap answer when nobody
// is listening is to decline before that wait starts, and this is the question that decides it.
//
// Only a CONNECT dial can be probed: MOBILEGL_TRANSPORT=spawn with MOBILEGL_IPC_CONTROL naming
// `unix:<path>`, `unix:@<abstract>` or `tcp://host:port`. Everything else - monolith, inproc, a
// fork control that launches its own server, an fd: pair somebody already connected - has no
// remote to be absent and answers NotApplicable. Both settings are read the way the configuration
// loader reads them (environment over the system configuration files), because the probe runs
// before MobileGL initializes.
//
// MOBILEGL_IPC_PROBE_TIMEOUT_MS bounds one probe (default 250, 1..60000); 0 turns probing off.

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
        std::string endpoint;     // SocketTransport form: a path, "@name", or "tcp://host:port"
        Uint32 timeoutMs = 0;     // 0: probing disabled
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
