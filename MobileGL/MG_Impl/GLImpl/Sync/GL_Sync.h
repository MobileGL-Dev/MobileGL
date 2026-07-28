// MobileGL - MobileGL/MG_Impl/GLImpl/Sync/GL_Sync.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

namespace MobileGL::MG_Impl::GLImpl {
    GLsync FenceSync(GLenum condition, GLbitfield flags);
    GLboolean IsSync(GLsync sync);
    GLenum ClientWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout);
    void WaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout);
    void DeleteSync(GLsync sync);
    void GetSynciv(GLsync sync, GLenum pname, GLsizei bufSize, GLsizei* length, GLint* values);
    // Destroys every still-registered sync object exactly as DeleteSync would.
    // GL requires syncs to die with their context; called only from full library
    // teardown (DestroyImpl), where no context survives on any thread, so the
    // process-global registry can be drained wholesale. Must run while the
    // backend function table is still populated: each backend handle has to be
    // released by the backend that created it, never by a later re-initialized
    // one.
    void DestroyAllSyncObjects();
} // namespace MobileGL::MG_Impl::GLImpl
