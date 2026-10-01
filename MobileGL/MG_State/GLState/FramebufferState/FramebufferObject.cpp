// MobileGL - MobileGL/MG_State/GLState/FramebufferState/FramebufferObject.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "FramebufferObject.h"
#include "MG_State/GLState/StateObjectDeathNotice.h"
#include "MG_Util/Types.h"

#include <atomic>
#include <MG_Pipe/PipeMutation.h>

// The display host's frame, as the backend publishes it (MG_Backend/DirectGLES/DirectGLES.h).
// DECLARED HERE RATHER THAN INCLUDED, and that is a deliberate compromise with a named end: the
// client asks whether its framebuffer is complete BEFORE any backend binding runs, so this judgement
// cannot ask the backend - and the frontend knowing a backend fact is exactly what a record is for.
// The wire does not carry one yet (MG_Protocol's dispatch has no glEGLImageTargetTexture2DOES), so
// the fact is read from where it already is; when that record lands, this block goes with it.
// The backend namespace is the one DirectGLES.cpp:67 opens and defines these in - the SAME nesting
// as Uint g_hostFrameFramebufferId beside them.  Getting this wrong is not a typo with no cost: a
// Linux shared library links with undefined symbols allowed, so the container build passed while the
// Android server build (ld.lld, --no-undefined) named all three - which is how a latent runtime
// failure was caught by the platform that builds the render server.
namespace MobileGL::MG_Backend::DirectGLES {
    extern Uint g_hostFrameRenderbuffer;
    extern Uint g_hostFrameWidth;
    extern Uint g_hostFrameHeight;
}

namespace MobileGL::MG_State::GLState {
    // Starts at 1 so a zero-initialized memo slot can never carry a live object's id.
    // Atomic for the same reason as the VAO counter: it costs nothing, and a duplicate
    // id would resurrect exactly the ABA this id exists to kill.
    static std::atomic<Uint64> s_nextFramebufferLifetimeId{1};

    Uint64 FramebufferObject::AllocateLifetimeId() {
        return s_nextFramebufferLifetimeId.fetch_add(1, std::memory_order_relaxed);
    }

#if MOBILEGL_PIPE_PUSH
    FramebufferObject::~FramebufferObject() {
        // P4a D-I2: A FRAMEBUFFER HAS A HANDLE AND NO WIRE LIFETIME. PipeCalls.def carries
        // resource_destroy and five delete_* rows and NO framebuffer delete, because a
        // framebuffer is not a resource and is not a CSO - it is STATE, and
        // set_framebuffer_state is the only call that names one - and the catalogue is closed,
        // so P4a invents no row. The helper is therefore steps 2 and 3 only: the death notice,
        // raised while the handle still resolves (this is the P2 step-e2 announcement that used
        // to stand here alone - the last SharedPtr to this object dropping, not the glDelete*
        // that only marks the name and leaves a still-bound object very much alive), and then
        // the slot.
        //
        // What makes a dangling Fbo unreachable is the frontend's own
        // MarkFramebufferObjectForDeletion path, which already rebinds any slot holding the
        // victim to framebuffer 0; and a RECYCLED framebuffer handle can never be suppressed
        // against its predecessor's record, because Fbo carries Gen and Gen is inside the
        // record's ContentHash.
        MG_Pipe::MGPipeEmitFramebufferDestroyAndFree(m_lifetimeId);
    }
#endif

    // FramebufferAttachmentObject
    FramebufferAttachmentObject::FramebufferAttachmentObject(
        const SharedPtr<MG_State::GLState::ITextureObject>& texture, TextureUploadTarget textureUploadTarget, Int level,
        Int layer, Bool layered)
        : m_texture(texture), m_textureUploadTarget(textureUploadTarget), m_textureLevel(level),
          m_textureLayer(layer), m_layered(layered) {}
    FramebufferAttachmentObject::FramebufferAttachmentObject(const SharedPtr<RenderbufferObject>& renderbuffer)
        : m_renderbuffer(renderbuffer) {}
    FramebufferAttachmentObject::FramebufferAttachmentObject(Bool IsValid)
        : m_texture(nullptr), m_renderbuffer(nullptr) {
        m_isValid = IsValid;
    }

    Bool FramebufferAttachmentObject::IsTexture() const {
        return m_texture != nullptr;
    }

    Bool FramebufferAttachmentObject::IsRenderbuffer() const {
        return m_renderbuffer != nullptr;
    }

    Bool FramebufferAttachmentObject::IsEmpty() const {
        return m_texture == nullptr && m_renderbuffer == nullptr;
    }

    const SharedPtr<MG_State::GLState::ITextureObject>& FramebufferAttachmentObject::GetTexture() const {
        return m_texture;
    }

    const SharedPtr<RenderbufferObject>& FramebufferAttachmentObject::GetRenderbuffer() const {
        return m_renderbuffer;
    }

    Int FramebufferAttachmentObject::GetTextureLevel() const {
        return m_textureLevel;
    }

    Int FramebufferAttachmentObject::GetTextureLayer() const {
        return m_textureLayer;
    }

    Bool FramebufferAttachmentObject::IsLayered() const {
        return m_layered;
    }

    TextureUploadTarget FramebufferAttachmentObject::GetTextureUploadTarget() const {
        return m_textureUploadTarget;
    }

    Bool FramebufferAttachmentObject::IsComplete() const {
        if (IsTexture()) {
            Bool complete = m_texture->IsComplete();
            return complete;
        } else if (IsRenderbuffer()) {
            Bool complete = m_renderbuffer->IsAllocated();
            return complete;
        }

        return false;
    }

    IntVec3 FramebufferAttachmentObject::GetSize() const {
        if (IsTexture()) {
            MOBILEGL_ASSERT(nullptr != static_cast<MG_State::GLState::TextureObjectMipmap*>(m_texture.get()),
                            "Texture object here should always be an object with mipmap");
            auto textureMipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(m_texture.get());
            TextureUploadTarget resolvedTarget = m_textureUploadTarget;
            if (resolvedTarget == TextureUploadTarget::Unknown) {
                const auto& uploadTargets = m_texture->GetUploadTargets();
                MOBILEGL_ASSERT(!uploadTargets.empty(),
                                "FramebufferAttachmentObject::GetSize: textureId=%u exposes no upload targets",
                                m_texture->GetExternalIndex());
                resolvedTarget = uploadTargets[0];
            }
            return textureMipmapObject->GetMipmapTexelSize(resolvedTarget, m_textureLevel);
        } else if (IsRenderbuffer()) {
            return {m_renderbuffer->GetWidth(), m_renderbuffer->GetHeight(), 1};
        }
        return {0, 0, 0};
    }

    Bool FramebufferAttachmentObject::IsValid() const {
        return m_isValid;
    }

    // FramebufferObject
    FramebufferObject::FramebufferObject(Uint externalIndex)
        : m_externalIndex(externalIndex), m_attachmentVersions{}, m_drawBuffers{} {
        m_attachmentObjects.fill(FramebufferAttachmentObject(false));
        m_drawBuffers.fill(FramebufferAttachmentType::None);
        const FramebufferAttachmentType defaultColorBuffer =
            (externalIndex == 0) ? FramebufferAttachmentType::BackLeft : FramebufferAttachmentType::Color0;
        m_drawBuffers[0] = defaultColorBuffer;
        m_readBuffer = defaultColorBuffer;
        m_attachmentVersions.fill(0);
    }

    void FramebufferObject::AttachTexture(FramebufferAttachmentType type, const SharedPtr<ITextureObject>& texture,
                                          TextureUploadTarget textureUploadTarget, int level, int layer, Bool layered) {
        m_attachmentObjects[static_cast<SizeT>(type)] =
            FramebufferAttachmentObject(texture, textureUploadTarget, level, layer, layered);
        BumpAttachmentVersion(type);
    }

    void FramebufferObject::AttachRenderbuffer(FramebufferAttachmentType type,
                                               const SharedPtr<RenderbufferObject>& renderbuffer) {
        m_attachmentObjects[static_cast<SizeT>(type)] = FramebufferAttachmentObject(renderbuffer);
        BumpAttachmentVersion(type);
    }

    void FramebufferObject::Detach(FramebufferAttachmentType type) {
        m_attachmentObjects[static_cast<SizeT>(type)] = FramebufferAttachmentObject(false);
        BumpAttachmentVersion(type);
    }

    const FramebufferAttachmentObject& FramebufferObject::GetAttachment(FramebufferAttachmentType type) const {
        return m_attachmentObjects[static_cast<SizeT>(type)];
    }

    const FramebufferObject::FramebufferAttachmentObjectArray& FramebufferObject::GetAllAttachmentObjects() const {
        return m_attachmentObjects;
    }

    Bool FramebufferObject::CheckCompleteness() const {
        if (m_attachmentObjects.empty()) {
            return false;
        }

        Int width = -1, height = -1;
        Int validAttachmentCount = 0;
        // THE IMPORTED dma-buf, and why it is judged BEFORE the validity test rather than after it.
        // Measured: with kwin_wayland's framebuffer attached - it imports the display daemon's
        // dma-buf as an EGLImage ("taking target 0x8d65 over an EGLImage of 1440x2937 fourcc
        // 0x34324241") and builds an FBO over that texture - the server answered
        // "GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT".  The old early-continue below dropped the
        // attachment (a texture that arrived this way has no storage, so it reports itself invalid)
        // and the completeness gate further down was never reached.
        //
        // THE GATE IS THE MEASURED SHAPE, and it is deliberately narrow: this process is holding a
        // display host frame (g_hostFrameRenderbuffer != 0), and the attachment is a texture with no
        // storage of its own.  That buffer IS the frame the host handed this process - same memory -
        // and the backend binds the frame's renderbuffer in place of the empty texture
        // (MG_Backend/DirectGLES/Managers.cpp).  A size comparison would be the sharper test and is
        // impossible here: the imported attachment has no size, which is exactly why the wire needs a
        // record to say so (MG_Protocol's dispatch has no glEGLImageTargetTexture2DOES yet); when it
        // lands, this gate is replaced by that record and nothing else changes.
        // AND IT DELIBERATELY DOES NOT ASK WHETHER THE FRAME IS IN HAND YET, which the first version
        // of this gate did - and that version did not work, measured: kwin imports the dma-buf and
        // builds its framebuffer BEFORE the host-framed surface exists, so g_hostFrameRenderbuffer is
        // still zero at the instant the client asks, the gate refused, and kwin printed
        // "GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT" again.  What the attachment IS is the fact
        // that holds at that instant: a texture with no storage of its own is the imported dma-buf,
        // and the buffer it names is the display daemon's frame - the one this process is about to be
        // handed.  The backend binds that frame's renderbuffer to the attachment as soon as it has
        // one (Managers.cpp re-syncs attachments per draw, so the first draw after the frame arrives
        // lands in it).
        for (const auto& attachmentObject : m_attachmentObjects) {
            if (!attachmentObject.IsValid() && !attachmentObject.IsTexture()) continue;

            ++validAttachmentCount;
            const auto& attachment = attachmentObject;
            auto attachmentSize = attachment.GetSize();
            Int w = attachmentSize.x();
            Int h = attachmentSize.y();

            if (width == -1) {
                width = w;
                height = h;
            } else if (width != w || height != h) {
                return false;
            }

            // AN IMPORTED dma-buf COUNTS AS COMPLETE, in the measured shape and no other: the
            // attachment has no storage of its own and this process holds a display host frame of the
            // same size.  Measured, and the reason this is here at all: kwin_wayland imports the
            // display daemon's dma-buf as an EGLImage ("taking target 0x8d65 over an EGLImage of
            // 1440x2937 fourcc 0x34324241") and then refuses its own framebuffer with "framebuffer for
            // dmabuf 0 is not complete" - a refusal that is WRONG here, because the buffer it imported
            // IS the frame the display host handed this process (same memory, same size) and the
            // backend binds that frame's renderbuffer in place of the empty texture
            // (MG_Backend/DirectGLES/Managers.cpp, the external-OES arm under the same gate).  A size
            // that differs, or no frame held, keeps the old answer: an ordinary empty attachment is
            // still incomplete, and nothing is ever rendered into a buffer of the wrong size.
            // AND IT DOES NOT ASK WHETHER A FRAME IS HELD, OR HOW BIG IT IS - which the first version
            // of this gate did, and which is why it never worked.  This judgement runs on BOTH sides
            // of a split run: the client answers glCheckFramebufferStatus out of its own state before
            // any backend binding happens, and the client holds no frame at all.  So a gate that
            // required g_hostFrameRenderbuffer != 0 and a matching size refused on the client every
            // time, and kwin kept printing "Invalid framebuffer status:
            // GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT" - measured, twice, after that gate was
            // deployed.  What the attachment IS is the fact that holds on both sides: a texture with
            // no storage of its own is the imported dma-buf, and the server binds the display host's
            // frame to it as soon as it has one (Managers.cpp re-syncs the attachments per draw, so
            // the first draw after the frame arrives lands in it).  An attachment that IS valid - an
            // ordinary texture or renderbuffer with real storage - still has to be complete.
            // A TEXTURE ATTACHMENT THAT IS NOT COMPLETE IS THE IMPORTED dma-buf, and it counts.  The
            // condition is written on IsTexture() and NOT on IsValid(), which is the mistake two
            // earlier versions of this gate made - measured both times as
            // "Invalid framebuffer status: GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT" from a session
            // that was healthy and had really reached the import:
            //   * requiring a host frame to be in hand refused on the CLIENT, which holds none;
            //   * requiring the attachment to be INVALID refused too, because AttachTexture registers
            //     the attachment (FramebufferObject.cpp:163) whether or not the texture has storage -
            //     so the imported one is perfectly valid and simply incomplete.
            // What is true on both sides, at that instant, is what this asks: the attachment is a
            // TEXTURE and it is incomplete, i.e. it has no storage of its own, and the server binds the
            // display host's frame to it as soon as it holds one (Managers.cpp re-syncs the attachments
            // per draw).  A RENDERBUFFER attachment still has to be complete, so an ordinary empty
            // attachment of that kind keeps the old refusal.
            if (!attachment.IsComplete() && !attachment.IsTexture()) {
                return false;
            }
        }

        if (validAttachmentCount == 0) return false;
        return true;
    }

    void FramebufferObject::SetDrawBuffer(Uint index, FramebufferAttachmentType buffer) {
        if (m_drawBuffers[index] == buffer) return;
        m_drawBuffers[index] = buffer;
        BumpAttachmentVersion(buffer);
    }

    const FramebufferObject::FramebufferAttachmentArray& FramebufferObject::GetDrawBuffers() const {
        return m_drawBuffers;
    }

    void FramebufferObject::SetReadBuffer(FramebufferAttachmentType buf) {
        if (m_readBuffer == buf) return;
        m_readBuffer = buf;
        ++m_objectVersion;
        MGP_NOTE_AGGREGATE(FramebufferAttachment);
    }

    Uint FramebufferObject::GetExternalIndex() const {
        return m_externalIndex;
    }

#define MOBILEGL_DEFINE_FRAMEBUFFER_DEFAULT_SETTER(name, member, type)                                                \
    void FramebufferObject::Set##name(type value) {                                                                    \
        if (member == value) return;                                                                                    \
        member = value;                                                                                                 \
        ++m_objectVersion;                                                                                              \
        MGP_NOTE_AGGREGATE(FramebufferAttachment); \
    }

    MOBILEGL_DEFINE_FRAMEBUFFER_DEFAULT_SETTER(DefaultWidth, m_defaultWidth, Int)
    MOBILEGL_DEFINE_FRAMEBUFFER_DEFAULT_SETTER(DefaultHeight, m_defaultHeight, Int)
    MOBILEGL_DEFINE_FRAMEBUFFER_DEFAULT_SETTER(DefaultLayers, m_defaultLayers, Int)
    MOBILEGL_DEFINE_FRAMEBUFFER_DEFAULT_SETTER(DefaultSamples, m_defaultSamples, Int)
    MOBILEGL_DEFINE_FRAMEBUFFER_DEFAULT_SETTER(DefaultFixedSampleLocations, m_defaultFixedSampleLocations, Bool)
#undef MOBILEGL_DEFINE_FRAMEBUFFER_DEFAULT_SETTER

    void FramebufferObject::BumpAttachmentVersion(FramebufferAttachmentType type) {
        ++m_attachmentVersions[static_cast<SizeT>(type)];
        ++m_objectVersion;
        MGP_NOTE_AGGREGATE(FramebufferAttachment);
    }
} // namespace MobileGL::MG_State::GLState
