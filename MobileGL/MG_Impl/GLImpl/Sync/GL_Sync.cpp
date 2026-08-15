// MobileGL - MobileGL/MG_Impl/GLImpl/Sync/GL_Sync.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "GL_Sync.h"
#include <MG_Backend/BackendObjects.h>

namespace MobileGL::MG_Impl::GLImpl {
    namespace {
        // Frontend sync object: wraps an optional backend fence handle. A null
        // backend handle (backend has no fence support, or could not create a
        // fence at call time) keeps the legacy always-signaled behavior.
        //
        // SharedPtr-owned, not raw: DeleteSync can remove the registry entry while
        // another thread is inside ClientWaitSync/GetSynciv. Those callers hold a
        // SharedPtr copy, so the object stays alive until the last reader leaves.
        // `mutex` then serializes backend-handle reads against the one-time
        // backend-handle release performed by DeleteSync / DestroyAllSyncObjects.
        struct SyncObject {
            std::mutex mutex;
            MG_Backend::BackendSyncHandle backendHandle = nullptr;
            GLenum condition = GL_SYNC_GPU_COMMANDS_COMPLETE;
            GLbitfield flags = 0;

            void ReleaseBackendHandle() {
                const std::lock_guard<std::mutex> lock(mutex);
                if (backendHandle == nullptr) {
                    return;
                }
                const auto backendDeleteSync = MG_Backend::gBackendFunctionsTable.GL.DeleteSync;
                if (backendDeleteSync) {
                    backendDeleteSync(backendHandle);
                }
                backendHandle = nullptr;
            }
        };

        // Sync calls may arrive from any thread (launchers migrate the context
        // across JVM threads), so the live-object registry is mutex-guarded.
        // Entries left at process shutdown are simply dropped; their backend
        // handles die with the backend.
        std::mutex g_syncObjectsMutex;
        UnorderedMap<GLsync, SharedPtr<SyncObject>> g_liveSyncObjects;

        SharedPtr<SyncObject> FindSyncObject(GLsync sync) {
            const std::lock_guard<std::mutex> lock(g_syncObjectsMutex);
            const auto it = g_liveSyncObjects.find(sync);
            return it != g_liveSyncObjects.end() ? it->second : nullptr;
        }
    } // namespace

    GLsync FenceSync(GLenum condition, GLbitfield flags) {
        auto syncObject = MakeShared<SyncObject>();
        syncObject->condition = condition;
        syncObject->flags = flags;
        if (const auto backendFenceSync = MG_Backend::gBackendFunctionsTable.GL.FenceSync) {
            syncObject->backendHandle = backendFenceSync();
        }
        const GLsync handle = reinterpret_cast<GLsync>(syncObject.get());
        const std::lock_guard<std::mutex> lock(g_syncObjectsMutex);
        g_liveSyncObjects[handle] = syncObject;
        return handle;
    }

    GLboolean IsSync(GLsync sync) {
        return FindSyncObject(sync) != nullptr ? GL_TRUE : GL_FALSE;
    }

    GLenum ClientWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout) {
        const SharedPtr<SyncObject> syncObject = FindSyncObject(sync);
        if (!syncObject) {
            return GL_WAIT_FAILED;
        }
        const auto backendClientWaitSync = MG_Backend::gBackendFunctionsTable.GL.ClientWaitSync;
        // Hold the per-object lock across the backend call: a concurrent
        // DeleteSync may already have removed this object from the registry, but
        // it cannot free the backend handle (or the wrapper) until this reader
        // finishes. ClientWaitSync can block for `timeout`; that blocks only this
        // sync object, never the registry or unrelated syncs.
        const std::lock_guard<std::mutex> lock(syncObject->mutex);
        if (!backendClientWaitSync || syncObject->backendHandle == nullptr) {
            return GL_ALREADY_SIGNALED; // legacy always-signaled fallback
        }
        return backendClientWaitSync(syncObject->backendHandle, flags, timeout);
    }

    void WaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout) {
        const SharedPtr<SyncObject> syncObject = FindSyncObject(sync);
        if (!syncObject) {
            return;
        }
        const auto backendWaitSync = MG_Backend::gBackendFunctionsTable.GL.WaitSync;
        const std::lock_guard<std::mutex> lock(syncObject->mutex);
        if (backendWaitSync && syncObject->backendHandle != nullptr) {
            backendWaitSync(syncObject->backendHandle, flags, timeout);
        }
    }

    void DeleteSync(GLsync sync) {
        if (sync == nullptr) {
            return; // glDeleteSync(0) is silently ignored
        }
        SharedPtr<SyncObject> syncObject;
        {
            const std::lock_guard<std::mutex> lock(g_syncObjectsMutex);
            const auto it = g_liveSyncObjects.find(sync);
            if (it == g_liveSyncObjects.end()) {
                return;
            }
            syncObject = it->second;
            g_liveSyncObjects.erase(it);
        }
        // Release the backend handle under the object lock. The local SharedPtr
        // (and any reader's SharedPtr) keeps the wrapper itself alive until every
        // in-flight backend call has returned.
        syncObject->ReleaseBackendHandle();
    }

    void GetSynciv(GLsync sync, GLenum pname, GLsizei bufSize, GLsizei* length, GLint* values) {
        const SharedPtr<SyncObject> syncObject = FindSyncObject(sync);
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
            const std::lock_guard<std::mutex> lock(syncObject->mutex);
            const Bool signaled = !backendGetSyncStatus || syncObject->backendHandle == nullptr ||
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
        // a DeleteSync racing this sweep finds an empty registry and returns.
        // Readers racing this sweep keep their SharedPtr copy alive, and each
        // object's own lock makes the backend-handle release wait for them.
        UnorderedMap<GLsync, SharedPtr<SyncObject>> orphans;
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
        for (const auto& [_, syncObject] : orphans) {
            if (syncObject) {
                syncObject->ReleaseBackendHandle();
            }
        }
        MGLOG_D("DestroyAllSyncObjects: reclaimed %zu sync object(s) the app left undeleted", orphans.size());
    }
} // namespace MobileGL::MG_Impl::GLImpl
