// MobileGL - MobileGL/MG_Backend/DirectGLES/MultiDraw.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <Config.h>
#include "DirectGLES.h"

// Emulation of the desktop glMultiDrawElements / glMultiDrawElementsBaseVertex entry
// points on OpenGL ES, which has neither in core.
//
// Every strategy below is an emulation; they differ only in which driver capability
// they lean on and in how many driver entries a batch of N sub-draws costs. The design
// follows MobileGlues (MobileGL-Dev/MobileGlues, gl/multidraw.cpp) tier for tier, plus
// the native GL_EXT_multi_draw_arrays interaction that MobileGL already had:
//
//   Ext           one glMultiDrawElementsBaseVertexEXT           1 driver entry
//   MultiIndirect one glMultiDrawElementsIndirectEXT             1 driver entry + 1 upload
//   Indirect      N x glDrawElementsIndirect                     N + 1 upload
//   BaseVertex    N x glDrawElementsBaseVertex                   N
//   DrawElements  N x glDrawElements over CPU-rebased indices    N + 1 upload
//   Compute       1 x glDrawElements over a GPU-flattened,       1 dispatch + 1 entry
//                 rebased index stream
//
// Which one runs is resolved once per ES context from the driver's capabilities,
// capped by MOBILEGL_ESPRYT_MULTIDRAW_MODE, and can additionally be demoted per batch
// when the batch's own shape rules a tier out (see ResolveTierForBatch in the .cpp).
namespace MobileGL::MG_Backend::DirectGLES::MultiDrawImpl {
    // The tier this ES context resolved to, computed on first use and stable after.
    MG_Config::GLESMultiDrawMode ResolvedTier();
    // "multiindirect", "compute", ... - stable identifiers, also used by the POST row.
    const char* TierName(MG_Config::GLESMultiDrawMode tier);
    // One line naming the resolved tier, the tiers the driver can support, and the env
    // clamp if one applied. For DriverPost and the startup log.
    String DescribeTierResolution();

    // The resolution itself, as a pure function of a capability set: the backend feeds
    // it the live ES context's capabilities, DriverPost feeds it the ones it probed
    // standalone, and both therefore report the same tier. `explanation`, when non-null,
    // receives the "requested -> resolved (driver supports: ...)" line.
    MG_Config::GLESMultiDrawMode ResolveTier(const MG_External::GLESCapabilities& caps,
                                             const MG_External::GLESFunctionsTable& funcs,
                                             MG_Config::GLESMultiDrawMode requested, String* explanation);
    // Whether one tier is runnable on the given capability set, for per-row POST output.
    Bool IsTierSupported(const MG_External::GLESCapabilities& caps, const MG_External::GLESFunctionsTable& funcs,
                         MG_Config::GLESMultiDrawMode tier);

    // Runs `drawcount` indexed sub-draws as one glMultiDrawElements(BaseVertex) call
    // would. `basevertex` is null for the plain glMultiDrawElements entry point (every
    // base vertex is 0). Owns the whole draw, preparation included: callers must not
    // have run PrepareForDraw, because the compute tier has to dispatch before the
    // draw state is established.
    void DrawElementsBatch(GLenum mode, const GLsizei* count, GLenum type, const GLvoid* const* indices,
                           GLsizei drawcount, const GLint* basevertex);

    // The ES context is gone: every scratch buffer and the compute program belonged to
    // it, so drop the names without deleting them (the dead context reclaims them).
    void OnBackendContextDestroyed();
} // namespace MobileGL::MG_Backend::DirectGLES::MultiDrawImpl
