// MobileGL - MobileGL/MG_Remote/Server/SurfaceControlFrame.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "SurfaceControlFrame.h"

namespace MobileGL::MG_Remote::Server {

    const char* SurfaceControlOpName(SurfaceControlOp op) {
        switch (op) {
        case SurfaceControlOp::None: return "None";
        case SurfaceControlOp::InitializeDisplay: return "InitializeDisplay";
        case SurfaceControlOp::CreateWindowSurface: return "CreateWindowSurface";
        case SurfaceControlOp::CreatePbufferSurface: return "CreatePbufferSurface";
        case SurfaceControlOp::ResizeWindowSurface: return "ResizeWindowSurface";
        case SurfaceControlOp::ReleaseSurface: return "ReleaseSurface";
        case SurfaceControlOp::MakeCurrent: return "MakeCurrent";
        case SurfaceControlOp::ReleaseCurrent: return "ReleaseCurrent";
        case SurfaceControlOp::SetSwapInterval: return "SetSwapInterval";
        case SurfaceControlOp::ReleaseResources: return "ReleaseResources";
        case SurfaceControlOp::SetWindowHandle: return "SetWindowHandle";
        case SurfaceControlOp::InitCapabilitiesInprocOnly: return "InitCapabilitiesInprocOnly";
        case SurfaceControlOp::SwapBuffersInprocOnly: return "SwapBuffersInprocOnly";
        case SurfaceControlOp::InitWindowSurfaceInprocOnly: return "InitWindowSurfaceInprocOnly";
        case SurfaceControlOp::ProbeForTesting: return "ProbeForTesting";
        }
        return "<unknown SurfaceControlOp>";
    }

    bool SurfaceControlOpHasWireKind(SurfaceControlOp op) {
        switch (op) {
        case SurfaceControlOp::InitializeDisplay:
        case SurfaceControlOp::CreateWindowSurface:
        case SurfaceControlOp::CreatePbufferSurface:
        case SurfaceControlOp::ResizeWindowSurface:
        case SurfaceControlOp::ReleaseSurface:
        case SurfaceControlOp::MakeCurrent:
        case SurfaceControlOp::ReleaseCurrent:
        case SurfaceControlOp::SetSwapInterval:
        case SurfaceControlOp::ReleaseResources:
        case SurfaceControlOp::SetWindowHandle:
            return true;
        default:
            return false;
        }
    }

} // namespace MobileGL::MG_Remote::Server
