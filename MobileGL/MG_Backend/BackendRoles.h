// MobileGL - MobileGL/MG_Backend/BackendRoles.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_Backend/SharedImageTypes.h>
#include <MG_Util/Damage/Damage.h>

namespace MobileGL::MG_Backend {
    // What only the server asks of the backend it applies a session's records with. A backend that
    // can serve sessions implements it and returns it from BackendObject::AsServer; each default is
    // the answer of a backend with nothing to do for that hook.
    class ServerHooks {
    public:
        // ---- shared images ------------------------------------------------------------------
        //
        // Copies the default framebuffer of the surface current on this (the apply) thread into
        // `image`, rows TOP FIRST - GL's top row (y = height-1) lands in the image's first memory
        // row, which is what a wl_buffer's first row is - scaled to the image's extent if the two
        // differ. The image is ORDERED THROUGH ITS SYNC STATE (SharedImageRegistry.h): the copy
        // waits for the image's pending read fences before it writes, and its own completion
        // is published with SharedImages::PublishWrite as a sync_file - the call returns once
        // the copy is submitted, never waiting for it on the CPU (a backend with no fence export
        // waits and publishes -1). A reader binding the image waits for that fence on its GPU
        // when the write generation moved, and at its frame boundary publishes its frame's fence
        // with PublishRead / ReadTracker. The application's GL state is untouched. False = not
        // supported / failed (logged).
        //
        // `region` (GL window coordinates, origin bottom-left) is the part of the frame to copy;
        // Full copies all of it. The rest of the image is left as it is - the client sends a
        // region only for an image that already holds the frame outside it - and a backend that
        // cannot be sure the image's contents survived (a target it has never written) copies
        // all of it regardless.
        virtual Bool BlitDefaultFramebufferToSharedImage(const SharedImageView& image,
                                                         const MG_Util::Damage::Region& region) {
            (void)image, (void)region;
            return false;
        }

        // EGL_BUFFER_AGE_EXT of the draw surface of the context current on this (the apply)
        // thread: how many presents ago the buffer the next frame draws into held the frame then
        // presented; 0 = unknown (the application repaints everything). Asking says the client
        // relies on the answer, so from then on the backend keeps what it presents.
        //
        // `damageRegionFollows`: the client declares the region it draws (SetCurrentDamageRegion)
        // before drawing. A driver whose only buffer age is EGL_KHR_partial_update's keeps a
        // buffer's content only outside that region, so without one coming it is not asked.
        virtual Int32 QueryCurrentBufferAge(Bool damageRegionFollows) {
            (void)damageRegionFollows;
            return 0;
        }
        // eglSetDamageRegionKHR for that draw surface: the frame draws `region` (GL window
        // coordinates; Full = all of it) and nothing outside it. A backend without the notion
        // ignores it.
        virtual Bool SetCurrentDamageRegion(const MG_Util::Damage::Region& region) {
            (void)region;
            return true;
        }
        // IMPLICIT SYNC BY FLUSH: the session
        // on this (the apply) thread ends a frame at a glFlush. Every shared image it used since
        // its last boundary is published as written and read by one fence of everything it
        // submitted (SharedImages::ReadTracker::PublishFrameAsWrite), and from now on its first
        // use of an image in a frame waits for that image's pending reads (it may be about to
        // overwrite what another session still samples). A backend without shared images has
        // nothing to publish.
        virtual Bool PublishSharedImageAccesses() { return true; }
        // EGL_ANDROID_native_fence_sync's fence command, on the server: in `*fence`, a sync_file
        // (the caller's to close) that signals once everything the session submitted so far has
        // completed - and, for a session that writes images, what PublishSharedImageAccesses
        // does. -1 when there is none to give: the backend cannot export one, or the work has
        // already completed - in both cases it HAS completed when this returns.
        virtual Bool ExportNativeFence(int* fence) {
            *fence = -1;
            return PublishSharedImageAccesses();
        }

        // A served session's apply thread - the one thread that runs this backend from here on -
        // calls this first, in a process that may serve several sessions at once. A backend whose
        // native state is process-global by construction gives this object its own copy and binds
        // it to the calling thread. The default has nothing per process to separate.
        virtual void BindSessionStateToThisThread() {}
        // A served session's apply thread asks this each time it wakes, BEFORE it applies another
        // record: true when this session's own GPU work faulted or hung the GPU and the backend has
        // latched the session for it (MGPipeSessionLatch, family DeviceLost), so the next frame -
        // which would very likely stop the GPU again - is never applied. Cheap by contract: no
        // wait, at most a status read. The default knows of no fault.
        virtual Bool LatchIfGpuFaulted() { return false; }
        // A served session's ring moved to another of its client's contexts (bind_context). A
        // backend whose surface follows the native context it binds needs nothing; one that keeps
        // a single native surface binding re-binds the surface that context was last made current
        // with. `token` is the client's context token (the EGLContext MakeEGLCurrent was given).
        virtual void OnClientContextBound(Uint64 token) { (void)token; }

        // ---- the server's own display window going away and coming back -------------------
        //
        // `window` (a window surface's native window) is about to be destroyed - the display's
        // screen went off. Release ONLY what is bound to it: every window surface on it keeps its
        // client handle and draws into a placeholder from here on, and every context, object and
        // device stays. Called on the thread that runs this backend; nothing may reference the
        // window once it returns true. False: not supported (the caller ends the session instead).
        virtual Bool SuspendServerWindow(void* window) {
            (void)window;
            return false;
        }
        // The surfaces SuspendServerWindow parked are rebuilt on `window` (a new native window,
        // `width`x`height`), and their new extent is published as a creation publishes it. False:
        // they could not be (the caller ends the session).
        virtual Bool ResumeServerWindow(void* window, Uint32 width, Uint32 height) {
            (void)window, (void)width, (void)height;
            return false;
        }
        // `window` changed size to `width`x`height` (the display's report - the one size to publish).
        // Its surfaces publish that extent to the client now, without waiting for a present; a
        // swapchain that has to follow is rebuilt when no frame is half-recorded, else at the
        // present. False: nothing was published (no surface on it, or not supported).
        virtual Bool ServerWindowResized(void* window, Uint32 width, Uint32 height) {
            (void)window, (void)width, (void)height;
            return false;
        }

    protected:
        ~ServerHooks() = default;
    };

    // What only a client of a remote server asks of its backend. BackendObject_Remote implements it
    // and returns it from BackendObject::AsClient; each default is the answer of a backend with no
    // server behind it.
    class ClientHooks {
    public:
        // ---- shared images ------------------------------------------------------------------
        //
        // A new image, allocated by the server; `out->Fd` is the caller's to close.
        virtual Bool AllocateSharedImage(Uint32 width, Uint32 height, Uint32 fourcc, SharedImageExport* out) {
            (void)width, (void)height, (void)fourcc, (void)out;
            return false;
        }
        // Names the image `fd` was exported from (the caller keeps `fd`). False: not one of this
        // server's images, or the size/format disagree.
        virtual Bool ImportSharedImage(int fd, Uint32 width, Uint32 height, Uint32 fourcc, Uint64* outId) {
            (void)fd, (void)width, (void)height, (void)fourcc, (void)outId;
            return false;
        }
        // The same for a YUV fourcc, whose planes and hints `layout` gives. A YUV buffer the
        // server did not export is taken too (its labelled CPU-copy fallback).
        virtual Bool ImportSharedImagePlanes(int fd, Uint32 width, Uint32 height, Uint32 fourcc,
                                             const SharedImageImportLayout& layout, Uint64* outId) {
            (void)fd, (void)width, (void)height, (void)fourcc, (void)layout, (void)outId;
            return false;
        }
        // Drops this client's reference (an allocation's or an import's).
        virtual Bool ReleaseSharedImage(Uint64 id) {
            (void)id;
            return false;
        }
        // In the GL stream: the current context's default framebuffer, copied top row first into
        // the image (BlitDefaultFramebufferToSharedImage on the server) - `region` of it, Full for
        // all. Returns when it completed.
        virtual Bool PresentToSharedImage(Uint64 id, const MG_Util::Damage::Region& region) {
            (void)id, (void)region;
            return false;
        }
        // In the GL stream: EGL_BUFFER_AGE_EXT of the current draw surface (QueryCurrentBufferAge
        // on the server). False: the backend has no answer (the caller reports 0).
        virtual Bool QueryBufferAge(Bool damageRegionFollows, EGLint* age) {
            (void)damageRegionFollows, (void)age;
            return false;
        }
        // In the GL stream: eglSetDamageRegionKHR for the current draw surface
        // (SetCurrentDamageRegion on the server).
        virtual Bool SetDamageRegion(const MG_Util::Damage::Region& region) {
            (void)region;
            return false;
        }
        // In the GL stream: the texture whose lifetime id is `textureLifetimeId` takes the image as
        // its level-0 storage (glEGLImageTargetTexture2DOES), until its next storage definition.
        // The caller has already defined level 0 as Width x Height RGBA8 with no data.
        virtual Bool AttachSharedImageToTexture(Uint64 textureLifetimeId, Uint64 id) {
            (void)textureLifetimeId, (void)id;
            return false;
        }
        // In the GL stream, from glFlush/glFinish: PublishSharedImageAccesses on the server,
        // returning once the fence is published. Skipped (true) when nothing reached the stream
        // since the last one.
        virtual Bool FlushSharedImageAccesses() { return false; }
        // In the GL stream, from eglCreateSync(EGL_SYNC_NATIVE_FENCE_ANDROID) with no descriptor:
        // ExportNativeFence on the server. `*fence` is the sync_file it handed back (the
        // caller's to close), or -1 when it had none - the fenced work has then completed.
        virtual Bool CreateNativeFence(int* fence) {
            *fence = -1;
            return false;
        }
        // From eglQuerySurface(EGL_WIDTH/EGL_HEIGHT): a backend whose surface extents
        // change under the application (a remote server's window) applies what it has received.
        virtual void RefreshSurfaceExtent(EGLSurface surface) { (void)surface; }

    protected:
        ~ClientHooks() = default;
    };
} // namespace MobileGL::MG_Backend
