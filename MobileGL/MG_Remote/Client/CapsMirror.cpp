// MobileGL - MobileGL/MG_Remote/Client/CapsMirror.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P5 c0 stubs for package c1. Every body is MGLOG_F + std::abort and never a silent no-op: a
// caps accessor that answers a default is how a split lane runs on the wrong device's limits.

#include "CapsMirror.h"

#include <MG_Util/Debug/Log.h>

#include <cstdlib>

namespace MobileGL::MG_Remote::Client {

#define MGP5_C0_STUB(what)                                                                                             \
    do {                                                                                                               \
        MGLOG_F("MGPipe: Fatal{UnimplementedCapsMirror, \"%s\"} - P5 package c1 has not landed "                       \
                "this yet; c0 shipped the signature only",                                                             \
                what);                                                                                                 \
        std::abort();                                                                                                  \
    } while (0)

    void CapsMirror::Adopt(const MG_Pipe::MGPCaps&, const MG_Backend::FormatCapabilityCache&,
                           const RendererInfo&, const String&, BackendType) {
        MGP5_C0_STUB("CapsMirror::Adopt");
    }

    // Not stubs: the two the placeholder contract above promises are readable before the first
    // snapshot. Everything else aborts, so nothing can accidentally answer from a zeroed mirror.
    Bool CapsMirror::Valid() const { return m_generation != 0; }
    Uint64 CapsMirror::Generation() const { return m_generation; }

    const RendererInfo& CapsMirror::Renderer() const { MGP5_C0_STUB("CapsMirror::Renderer"); }
    const MG_Backend::DynamicBackendParameters& CapsMirror::Dynamic() const {
        MGP5_C0_STUB("CapsMirror::Dynamic");
    }
    const MG_Backend::FormatCapabilityCache& CapsMirror::Formats() const {
        MGP5_C0_STUB("CapsMirror::Formats");
    }
    const String& CapsMirror::ApiVersion() const { MGP5_C0_STUB("CapsMirror::ApiVersion"); }
    BackendType CapsMirror::Backend() const { MGP5_C0_STUB("CapsMirror::Backend"); }
    Uint64 CapsMirror::CallMask() const { MGP5_C0_STUB("CapsMirror::CallMask"); }
    Bool CapsMirror::HasCap(MG_Pipe::MGPCapBit) const { MGP5_C0_STUB("CapsMirror::HasCap"); }
    Bool CapsMirror::ServerConsumes(Uint64) const { MGP5_C0_STUB("CapsMirror::ServerConsumes"); }
    Bool CapsMirror::PrefersCpuXfbPrimitiveAccounting() const {
        MGP5_C0_STUB("CapsMirror::PrefersCpuXfbPrimitiveAccounting");
    }

    CapsMirror& CapsMirrorInstance() {
        // ID-8: leak at exit. No frontend destructor may reach pipe or backend state from an
        // exit handler, and that rule applies once per role-local singleton, not once overall.
        static CapsMirror& instance = *new CapsMirror{};
        return instance;
    }

#undef MGP5_C0_STUB

} // namespace MobileGL::MG_Remote::Client
