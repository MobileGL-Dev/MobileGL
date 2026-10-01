// MobileGL - MobileGL/MG_Remote/Client/DmaBufImport.cpp
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "DmaBufImport.h"

#include "ClientSession.h"
#include "../Protocol/mg_protocol_base.h"
#include "../Transport/ITransport.h"

namespace MobileGL::MG_Remote::Client {

    Bool ShareImportedDmaBuf(Uint32 target, Uint32 texName, Uint32 width, Uint32 height, Uint32 fourCC,
                             Uint32 planeIndex, Uint32 planeCount, Int planeFd, Int stride, Int offset,
                             Uint64 modifier, Bool hasModifier) {
        if (planeFd < 0 || planeCount == 0 || planeCount > kDmaBufImportMaxPlanes) return false;
        // The session's control-plane transport is where the descriptor channel lives; no session or
        // no channel (a TCP session whose aux came up without SCM_RIGHTS) is a named refusal here.
        ClientSession* session = ClientSession::Active();
        if (session == nullptr) return false;
        Transport::ITransport* transport = session->Control_Plane();
        if (transport == nullptr) return false;

        DmaBufImportRequest request{};
        request.Magic = kDmaBufImportRequestMagic;
        request.Version = kDmaBufImportVersion;
        request.Target = target;
        request.TexName = texName;
        request.Width = width;
        request.Height = height;
        request.FourCC = fourCC;
        request.PlaneCount = planeCount;
        request.Stride[planeIndex % kDmaBufImportMaxPlanes] = stride;
        request.Offset[planeIndex % kDmaBufImportMaxPlanes] = offset;
        request.Modifier[planeIndex % kDmaBufImportMaxPlanes] = modifier;
        request.HasModifier[planeIndex % kDmaBufImportMaxPlanes] = hasModifier ? 1u : 0u;

        MobileGLByteSpan sideband{};
        sideband.data = &request;
        sideband.size = sizeof(request);
        return transport->ShareFd(planeFd, sideband) == MOBILEGL_OK;
    }
} // namespace MobileGL::MG_Remote::Client
