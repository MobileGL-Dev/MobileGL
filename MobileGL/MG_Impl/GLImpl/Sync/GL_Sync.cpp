// MobileGL - MobileGL/MG_Impl/GLImpl/Sync/GL_Sync.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "GL_Sync.h"
#include <MG_Backend/BackendObjects.h>
#include <MG_State/GLState/Core.h>

namespace MobileGL::MG_Impl::GLImpl {
    namespace {
        // Frontend sync object: wraps an optional backend fence handle. A null
        // backend handle (backend has no fence support, or could not create a
        // fence at call time) keeps the legacy always-signaled behavior.
        struct SyncObject {
            MG_Backend::BackendSyncHandle backendHandle = nullptr;
            GLenum condition = GL_SYNC_GPU_COMMANDS_COMPLETE;
            GLbitfield flags = 0;
        };

        // Sync calls may arrive from any thread (launchers migrate the context
        // across JVM threads), so the live-object registry is mutex-guarded.
        // Entries left at process shutdown are simply dropped; their backend
        // handles die with the backend.
        std::mutex g_syncObjectsMutex;
        UnorderedMap<GLsync, SyncObject*> g_liveSyncObjects;

        SyncObject* FindSyncObject(GLsync sync) {
            const std::lock_guard<std::mutex> lock(g_syncObjectsMutex);
            const auto it = g_liveSyncObjects.find(sync);
            return it != g_liveSyncObjects.end() ? it->second : nullptr;
        }
    } // namespace

    GLsync FenceSync(GLenum condition, GLbitfield flags) {
        // GL 4.6 core 4.1.2: GL_SYNC_GPU_COMMANDS_COMPLETE is the only condition and the only
        // legal flags value is zero; both violations return 0 rather than a handle. A caller that
        // then hands the 0 back to glDeleteSync hits the glDeleteSync(0) no-op below.
        if (condition != GL_SYNC_GPU_COMMANDS_COMPLETE) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "condition must be GL_SYNC_GPU_COMMANDS_COMPLETE."));
            return nullptr;
        }
        if (flags != 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "flags must be zero."));
            return nullptr;
        }
        auto* syncObject = new SyncObject;
        syncObject->condition = condition;
        syncObject->flags = flags;
        if (const auto backendFenceSync = MG_Backend::gBackendFunctionsTable.GL.FenceSync) {
            syncObject->backendHandle = backendFenceSync();
        }
        const GLsync handle = reinterpret_cast<GLsync>(syncObject);
        const std::lock_guard<std::mutex> lock(g_syncObjectsMutex);
        g_liveSyncObjects[handle] = syncObject;
        return handle;
    }

    GLboolean IsSync(GLsync sync) {
        return FindSyncObject(sync) != nullptr ? GL_TRUE : GL_FALSE;
    }

    GLenum ClientWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout) {
        const auto* syncObject = FindSyncObject(sync);
        if (!syncObject) {
            return GL_WAIT_FAILED;
        }
        const auto backendClientWaitSync = MG_Backend::gBackendFunctionsTable.GL.ClientWaitSync;
        if (!backendClientWaitSync || !syncObject->backendHandle) {
            return GL_ALREADY_SIGNALED; // legacy always-signaled fallback
        }
        return backendClientWaitSync(syncObject->backendHandle, flags, timeout);
    }

    void WaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout) {
        // GL 4.6 core 4.1.2: the server-side wait takes no flags and no finite timeout - both
        // arguments exist only to be forward-compatible, and anything else is INVALID_VALUE.
        // Neither backend ever honored a nonzero timeout (DirectGLES hard-codes
        // 0/GL_TIMEOUT_IGNORED, DirectVulkan's queue ordering makes the wait implicit), so
        // rejecting the call loses no wait that used to happen.
        if (flags != 0 || timeout != GL_TIMEOUT_IGNORED) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "flags must be zero and timeout must be GL_TIMEOUT_IGNORED."));
            return;
        }
        const auto* syncObject = FindSyncObject(sync);
        if (!syncObject) {
            return;
        }
        const auto backendWaitSync = MG_Backend::gBackendFunctionsTable.GL.WaitSync;
        if (backendWaitSync && syncObject->backendHandle) {
            backendWaitSync(syncObject->backendHandle, flags, timeout);
        }
    }

    void DeleteSync(GLsync sync) {
        if (sync == nullptr) {
            return; // glDeleteSync(0) is silently ignored
        }
        SyncObject* syncObject = nullptr;
        {
            const std::lock_guard<std::mutex> lock(g_syncObjectsMutex);
            const auto it = g_liveSyncObjects.find(sync);
            if (it == g_liveSyncObjects.end()) {
                return;
            }
            syncObject = it->second;
            g_liveSyncObjects.erase(it);
        }
        const auto backendDeleteSync = MG_Backend::gBackendFunctionsTable.GL.DeleteSync;
        if (backendDeleteSync && syncObject->backendHandle) {
            backendDeleteSync(syncObject->backendHandle);
        }
        delete syncObject;
    }

    void GetSynciv(GLsync sync, GLenum pname, GLsizei bufSize, GLsizei* length, GLint* values) {
        const auto* syncObject = FindSyncObject(sync);
        if (!syncObject) {
            if (length) {
                *length = 0;
            }
            return;
        }

        GLint value = 0;
        switch (pname) {
        case GL_OBJECT_TYPE:
            value = GL_SYNC_FENCE;
            break;
        case GL_SYNC_STATUS: {
            const auto backendGetSyncStatus = MG_Backend::gBackendFunctionsTable.GL.GetSyncStatus;
            const Bool signaled = !backendGetSyncStatus || !syncObject->backendHandle ||
                                  backendGetSyncStatus(syncObject->backendHandle);
            value = signaled ? GL_SIGNALED : GL_UNSIGNALED;
            break;
        }
        case GL_SYNC_CONDITION:
            value = static_cast<GLint>(syncObject->condition);
            break;
        case GL_SYNC_FLAGS:
            value = static_cast<GLint>(syncObject->flags);
            break;
        default:
            break;
        }

        if (length) {
            *length = bufSize > 0 && values ? 1 : 0;
        }
        if (bufSize > 0 && values) {
            values[0] = value;
        }
    }

    void DestroyAllSyncObjects() {
        // Detach the registry under the lock, release outside it. Entries the app
        // already deleted were erased by DeleteSync, so nothing here double-frees;
        // a DeleteSync racing this sweep finds an empty registry and returns. A
        // thread still blocked inside ClientWaitSync/GetSynciv during teardown
        // holds a raw SyncObject* these deletes invalidate - the same undefined
        // race an app-driven DeleteSync already has.
        UnorderedMap<GLsync, SyncObject*> orphans;
        {
            const std::lock_guard<std::mutex> lock(g_syncObjectsMutex);
            orphans.swap(g_liveSyncObjects);
        }
        if (orphans.empty()) {
            return;
        }
        // Both backends' DeleteSync only free the heap wrapper once their GL
        // context/renderer is gone (generation/current-thread guards), so this is
        // safe after the backend has released its EGL resources - but not after
        // the function table itself is cleared.
        const auto backendDeleteSync = MG_Backend::gBackendFunctionsTable.GL.DeleteSync;
        for (const auto& [_, syncObject] : orphans) {
            if (backendDeleteSync && syncObject->backendHandle) {
                backendDeleteSync(syncObject->backendHandle);
            }
            delete syncObject;
        }
        MGLOG_D("DestroyAllSyncObjects: reclaimed %zu sync object(s) the app left undeleted", orphans.size());
    }
} // namespace MobileGL::MG_Impl::GLImpl
