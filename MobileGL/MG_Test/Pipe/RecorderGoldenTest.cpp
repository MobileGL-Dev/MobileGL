// MobileGL - MobileGL/MG_Test/Pipe/RecorderGoldenTest.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P13 W8: RECORDER GOLDENS - "the pushed content does not change by accident".
//
// G1 (the pull build's symbol set) is retired; what it guarded - that refactoring the client does
// not quietly change what the backend is told - is guarded here instead, at the seam that is left:
// the record stream. A fixed sequence of frontend GL calls is run with a recorder in front of every
// door a record leaves the client through:
//
//   * the two MGPipe interface tables (gMGPipeScreen / gMGPipeContext) and the route escapes, i.e.
//     every state / object record (PipeRoute.h routes nothing around them);
//   * the verb session (MG_Record::SetVerbSessionResolver), i.e. every verb record the emitters
//     in MG_Impl/Pipe/Verb plan - the same bytes a transport would carry.
//
// Each record is NORMALISED (the one thing in a payload that is not a function of the GL calls is
// an address: an MGPBlobRef / MGHostSpan naming caller memory - zeroed, the size kept; the bytes
// they name are hashed instead) and hashed with its companion bytes and tails. The golden is one
// line per record - index, call name, size, hash - in goldens/recorder/<fixture>.txt, small enough
// to review in a diff. The state records still reach the real applier after they are recorded, so
// the client's emitters see the same acceptance answers they always do; the verbs are recorded and
// answered "ok" (there is no backend in a unit process to apply them).
//
// REGENERATING IS DELIBERATE: MOBILEGL_RECORDER_GOLDEN_WRITE=1 rewrites the files and the case then
// fails, so a regeneration can never pass silently in CI. A changed golden is a statement that the
// pushed content changed on purpose; the commit that changes one says why.
//
// NEGATIVE CONTROLS: one field of one recorded record is perturbed (exactly what an emitter that
// wrote a field differently produces) and the comparison must go red naming that record; and the
// same fixture with one GL input changed must differ from the golden.

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "Includes.h"
#include "Init.h"
#include <Config.h>
#include <MG_Backend/BackendObjects.h>
#include <MG_Impl/GLImpl/Buffer/GL_Buffer.h>
#include <MG_Impl/GLImpl/Drawing/GL_Drawing.h>
#include <MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.h>
#include <MG_Impl/GLImpl/Program/GL_Program.h>
#include <MG_Impl/GLImpl/RenderState/GL_RenderState.h>
#include <MG_Impl/GLImpl/Texture/GL_Texture.h>
#include <MG_Impl/GLImpl/VertexArray/GL_VertexArray.h>
#include <MG_Impl/Pipe/Verb/VerbPort.h>
#include <MG_Pipe/MGPipe.h>
#include <MG_Pipe/PipeApply.h>
#include <MG_Pipe/PipeRoute.h>
#include <MG_State/GLState/Core.h>

#ifndef MGL_RECORDER_GOLDEN_DIR
#error "MGL_RECORDER_GOLDEN_DIR must name the golden directory (MG_Test/Pipe/CMakeLists.txt)"
#endif

using namespace MobileGL;
using namespace MobileGL::MG_Pipe;
namespace GL = MobileGL::MG_Impl::GLImpl;

namespace {
    // ---- the hash and the recorded stream -------------------------------------------------------

    struct Fnv {
        Uint64 Value = 1469598103934665603ull;
        void Add(const void* bytes, SizeT size) {
            const auto* p = static_cast<const Uint8*>(bytes);
            for (SizeT i = 0; i < size; ++i) {
                Value ^= p[i];
                Value *= 1099511628211ull;
            }
        }
        void AddU64(Uint64 v) { Add(&v, sizeof(v)); }
    };

    struct Record {
        String Op;
        Uint64 Bytes = 0;
        Uint64 Hash = 0;
    };

    std::vector<Record>* g_stream = nullptr;
    // Negative control: when set, the payload of the Nth record is perturbed before it is hashed.
    Int64 g_perturbIndex = -1;

    void Note(const char* op, Fnv& hash, Uint64 bytes) {
        if (g_stream == nullptr) return;
        if (static_cast<Int64>(g_stream->size()) == g_perturbIndex) hash.AddU64(0x5eedull);
        g_stream->push_back(Record{op, bytes, hash.Value});
    }

    // ---- normalisation: addresses out, sizes kept --------------------------------------------------

    void Scrub(MGPBlobRef& blob) {
        if (blob.Seg == kMGHostSpanSegNone) blob.Offset = 0;
    }
    void Scrub(MGHostSpan& span) {
        span.Ptr = nullptr;
        if (span.Seg == kMGHostSpanSegNone) span.Offset = 0;
    }

    template <typename T>
    void Normalize(T&) {}
    template <>
    void Normalize(MGPCaps& p) {
        Scrub(p.FormatCapabilities);
        Scrub(p.RendererInfo);
    }
    template <>
    void Normalize(MGPRenderStateDesc& p) { Scrub(p.Blob); }
    template <>
    void Normalize(MGPDynamicState& p) { Scrub(p.Blob); }
    template <>
    void Normalize(MGPVertexElements& p) { Scrub(p.Blob); }
    template <>
    void Normalize(MGPSamplerDesc& p) { Scrub(p.Parameters); }
    template <>
    void Normalize(MGPGlobalConstants& p) { Scrub(p.Blob); }
    template <>
    void Normalize(MGPResidualValueState& p) { Scrub(p.Blob); }
    template <>
    void Normalize(MGPSubData& p) { Scrub(p.Blob); }
    template <>
    void Normalize(MGPStorageBlockBinding& p) { Scrub(p.Name); }
    template <>
    void Normalize(MGPProgramDesc& p) {
        for (auto& stage : p.Spirv) Scrub(stage);
        Scrub(p.Reflection);
    }

    template <typename P>
    void HashPayload(Fnv& hash, const P* payload) {
        if (payload == nullptr) {
            hash.AddU64(0);
            return;
        }
        P copy;
        std::memcpy(&copy, payload, sizeof(P));
        Normalize(copy);
        hash.Add(&copy, sizeof(P));
    }

    // The element size of each table call's variable tail (the monolith adapters' own types,
    // PipeRoute.cpp's MGP_MONO_TAIL rows, plus the sub-data region list). A tail call missing here is
    // a red case, not a silently shorter hash.
    SizeT TailElementBytes(const char* op) {
        const String name = op;
        if (name == "SetVertexBuffers") return sizeof(MGPVertexBuffer);
        if (name == "SetSamplerViews") return sizeof(MGPBoundView);
        if (name == "BindSamplerStates") return sizeof(MGPipeHandle);
        if (name == "SetShaderImages") return sizeof(MGPImageView);
        if (name == "SetShaderBuffers") return sizeof(MGPBufferRange);
        if (name == "SetVertexAttribDefaults") return sizeof(MGPAttribValue);
        if (name == "ResourceSubData") return sizeof(MGPSubRegion);
        ADD_FAILURE() << "the recorder has no tail element size for " << op;
        return 0;
    }

    // The six argument shapes PipeTables.inc generates after the payload.
    Uint64 HashRest(Fnv&, const char*) { return 0; }
    Uint64 HashRest(Fnv&, const char*, MGPReplySlot*) { return 0; }
    Uint64 HashRest(Fnv& hash, const char*, const void* blob, Uint64 blobBytes) {
        hash.AddU64(blobBytes);
        if (blob != nullptr && blobBytes != 0) hash.Add(blob, static_cast<SizeT>(blobBytes));
        return blobBytes;
    }
    Uint64 HashRest(Fnv& hash, const char* op, const void* blob, Uint64 blobBytes, MGPReplySlot*) {
        return HashRest(hash, op, blob, blobBytes);
    }
    Uint64 HashRest(Fnv& hash, const char* op, const void* tail, Uint32 count) {
        hash.AddU64(count);
        const SizeT bytes = TailElementBytes(op) * count;
        if (tail != nullptr && bytes != 0) hash.Add(tail, bytes);
        return bytes;
    }
    Uint64 HashRest(Fnv& hash, const char* op, const void* blob, Uint64 blobBytes, const void* tail, Uint32 count,
                    MGPReplySlot*) {
        return HashRest(hash, op, blob, blobBytes) + HashRest(hash, op, tail, count);
    }

    // ---- the table wrappers, one per catalogue row -------------------------------------------------

    template <const char* Name, typename Fn>
    struct Wrap;
    template <const char* Name, typename P, typename... Rest>
    struct Wrap<Name, void (*)(const P*, Rest...)> {
        using Fn = void (*)(const P*, Rest...);
        static inline Fn Saved = nullptr;
        static void Call(const P* payload, Rest... rest) {
            Fnv hash;
            HashPayload(hash, payload);
            const Uint64 extra = HashRest(hash, Name, rest...);
            Note(Name, hash, sizeof(P) + extra);
            Saved(payload, rest...);
        }
        static void Install(Fn& slot) {
            if (slot == nullptr || slot == &Call) return;
            Saved = slot;
            slot = &Call;
        }
        static void Uninstall(Fn& slot) {
            if (slot == &Call) slot = Saved;
        }
    };

#define MGL_REC_TABLE_kScreen gMGPipeScreen
#define MGL_REC_TABLE_kCtxCso gMGPipeContext
#define MGL_REC_TABLE_kCtxState gMGPipeContext
#define MGL_REC_TABLE_kCtxObject gMGPipeContext
#define MGL_REC_TABLE_kCtxVerb gMGPipeContext
#define MGL_REC_TABLE_kCtxQuery gMGPipeContext

#define MGL_REC_NAME(Name, Payload, Class, Flags, Wait) constexpr char kRecName_##Name[] = #Name;
    MGP_CALL_LIST(MGL_REC_NAME)
#undef MGL_REC_NAME

    void InstallTableRecorders() {
#define MGL_REC_INSTALL(Name, Payload, Class, Flags, Wait)                                                   \
    Wrap<kRecName_##Name, decltype(MGL_REC_TABLE_##Class.Name)>::Install(MGL_REC_TABLE_##Class.Name);
        MGP_CALL_LIST(MGL_REC_INSTALL)
#undef MGL_REC_INSTALL
    }
    void UninstallTableRecorders() {
#define MGL_REC_UNINSTALL(Name, Payload, Class, Flags, Wait)                                                 \
    Wrap<kRecName_##Name, decltype(MGL_REC_TABLE_##Class.Name)>::Uninstall(MGL_REC_TABLE_##Class.Name);
        MGP_CALL_LIST(MGL_REC_UNINSTALL)
#undef MGL_REC_UNINSTALL
    }

    // ---- the escapes ---------------------------------------------------------------------------

    MGPipeRouteEscapes g_savedEscapes{};

    Bool RecResourceRespecify(const MGPResourceDesc* desc, const void* initialBytes, const MGPRespecifiedLevel* level) {
        Fnv hash;
        HashPayload(hash, desc);
        HashPayload(hash, level);
        hash.AddU64(initialBytes != nullptr ? 1u : 0u);
        Note("ResourceRespecify", hash, sizeof(*desc));
        return g_savedEscapes.ResourceRespecify(desc, initialBytes, level);
    }
    void RecResourceFlushRange(const MGPFlushRange* record, const void* bytes) {
        Fnv hash;
        HashPayload(hash, record);
        if (bytes != nullptr && record->Size != 0) hash.Add(bytes, static_cast<SizeT>(record->Size));
        Note("ResourceFlushRange", hash, sizeof(*record) + record->Size);
        g_savedEscapes.ResourceFlushRange(record, bytes);
    }
    void* RecMapPersistent(const MGPHandleOnly* handle, Uint64 size, const void* seedBytes) {
        Fnv hash;
        HashPayload(hash, handle);
        hash.AddU64(size);
        if (seedBytes != nullptr && size != 0) hash.Add(seedBytes, static_cast<SizeT>(size));
        Note("MapPersistent", hash, sizeof(*handle) + size);
        return g_savedEscapes.MapPersistent(handle, size, seedBytes);
    }
    void RecCreateShaderState(const MGPProgramDesc* desc, const MG_State::GLState::LinkArtifacts* link,
                              const MG_State::GLState::SpirvArtifacts* spirv, const Uint32* linkedStages,
                              Uint32 linkedStageCount) {
        Fnv hash;
        HashPayload(hash, desc);
        hash.AddU64(linkedStageCount);
        if (linkedStages != nullptr) hash.Add(linkedStages, sizeof(Uint32) * linkedStageCount);
        Note("CreateShaderState", hash, sizeof(*desc));
        g_savedEscapes.CreateShaderState(desc, link, spirv, linkedStages, linkedStageCount);
    }
    void RecSetProgramBindings(const MGPProgramBindings* hdr, const Int32* blockBindings,
                               const MGPProgramSamplerUnit* samplerUnits,
                               const MGPProgramStorageOverride* storageOverrides,
                               const char* const* storageOverrideNames) {
        Fnv hash;
        HashPayload(hash, hdr);
        if (blockBindings != nullptr) hash.Add(blockBindings, sizeof(Int32) * hdr->BlockBindingCount);
        if (samplerUnits != nullptr) hash.Add(samplerUnits, sizeof(MGPProgramSamplerUnit) * hdr->SamplerUnitCount);
        for (Uint32 i = 0; storageOverrides != nullptr && i < hdr->StorageOverrideCount; ++i) {
            MGPProgramStorageOverride entry = storageOverrides[i];
            Scrub(entry.Name);
            hash.Add(&entry, sizeof(entry));
            if (storageOverrideNames != nullptr && storageOverrideNames[i] != nullptr) {
                hash.Add(storageOverrideNames[i], std::strlen(storageOverrideNames[i]));
            }
        }
        Note("SetProgramBindings", hash, sizeof(*hdr));
        g_savedEscapes.SetProgramBindings(hdr, blockBindings, samplerUnits, storageOverrides, storageOverrideNames);
    }

    // ---- the verb session ------------------------------------------------------------------------

    const char* WireOpName(MGPWireOp op) {
        switch (op) {
#define MGL_REC_OP_NAME(Name, Payload, Class, Flags, Wait)                                                   \
    case MGPWireOp::Name:                                                                                     \
        return #Name;
            MGP_CALL_LIST(MGL_REC_OP_NAME)
#undef MGL_REC_OP_NAME
        default:
            return "<op>";
        }
    }

    class RecordingVerbSession final : public MG_Record::VerbSession {
    public:
        Uint64 EmitAndWait(MGPWireOp op, const void* payload, Uint64 payloadBytes, const void* varTail,
                           Uint64 varTailBytes, void* replyOut, Uint64 replyBytes, Int32* statusOut,
                           Uint64* replySizeOut) override {
            const MGPipeVerbTail tail{varTail, varTailBytes};
            return EmitAndWaitTails(op, payload, payloadBytes, &tail, varTail != nullptr ? 1u : 0u, replyOut,
                                    replyBytes, statusOut, replySizeOut);
        }
        Uint64 EmitAndWaitTails(MGPWireOp op, const void* payload, Uint64 payloadBytes, const MGPipeVerbTail* tails,
                                Uint32 tailCount, void* replyOut, Uint64 replyBytes, Int32* statusOut,
                                Uint64* replySizeOut) override {
            Fnv hash;
            HashVerbPayload(hash, op, payload, payloadBytes);
            Uint64 bytes = payloadBytes;
            for (Uint32 i = 0; i < tailCount; ++i) {
                hash.AddU64(tails[i].Size);
                if (tails[i].Bytes != nullptr && tails[i].Size != 0) hash.Add(tails[i].Bytes, static_cast<SizeT>(tails[i].Size));
                bytes += tails[i].Size;
            }
            Note(WireOpName(op), hash, bytes);
            // No backend lives here: every verb is answered ok, with zeroed reply bytes.
            if (replyOut != nullptr && replyBytes != 0) std::memset(replyOut, 0, static_cast<SizeT>(replyBytes));
            if (statusOut != nullptr) *statusOut = MGPipeReplySink::kStatusOk;
            if (replySizeOut != nullptr) *replySizeOut = replyBytes;
            return ++m_seq;
        }
        Uint32 MaxReplyBytes() const override { return 1u << 24; }
        void RequireReadPixelsReplyFits(Uint32, Uint32, Uint32, Uint32, Uint64) const override {}
        MGPBlobRef StageBytes(const void* bytes, Uint64 size) override {
            // A deterministic segment offset rather than an address, and the bytes go into the hash
            // of the record that names them through the next Note.
            Fnv hash;
            if (bytes != nullptr && size != 0) hash.Add(bytes, static_cast<SizeT>(size));
            Note("StageBytes", hash, size);
            const MGPBlobRef ref{m_staged, size, 1u, 0u};
            m_staged += (size + 7u) & ~Uint64{7};
            return ref;
        }

    private:
        static void HashVerbPayload(Fnv& hash, MGPWireOp op, const void* payload, Uint64 payloadBytes) {
            if (payload == nullptr || payloadBytes == 0) return;
            if (op == MGPWireOp::SetStorageBlockBinding && payloadBytes == sizeof(MGPStorageBlockBinding)) {
                HashPayload(hash, static_cast<const MGPStorageBlockBinding*>(payload));
                return;
            }
            hash.Add(payload, static_cast<SizeT>(payloadBytes));
        }
        Uint64 m_seq = 0;
        Uint64 m_staged = 0;
    };

    RecordingVerbSession* g_session = nullptr;
    MG_Record::VerbSession& ResolveRecordingSession(const char*) { return *g_session; }

    // ---- the scope a fixture runs in -------------------------------------------------------------

    class RecorderScope {
    public:
        explicit RecorderScope(std::vector<Record>& stream) {
            m_savedPush = MG_Config::Features.PipePush;
            MG_Config::Features.PipePush |= kMGPipeSubsystemsMigratedAtP5e;
            m_savedResourceOps = MGPipeGetResourceOps();
            static const MGPipeResourceOps kEmpty{};
            MGPipeSetResourceOps(&kEmpty);
            m_savedContext = Move(MG_State::pGLContext);
            MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();

            m_savedTable = MG_Backend::gBackendFunctionsTable;
            MG_Record::AssignVerbEmitters(MG_Backend::gBackendFunctionsTable);
            g_session = &m_session;
            MG_Record::SetVerbSessionResolver(&ResolveRecordingSession);
            m_savedPortRouting = MG_Record::SetMonolithVerbPortRoutingForTesting(false);

            g_savedEscapes = gMGPipeRouteEscapes;
            gMGPipeRouteEscapes.ResourceRespecify = &RecResourceRespecify;
            gMGPipeRouteEscapes.ResourceFlushRange = &RecResourceFlushRange;
            gMGPipeRouteEscapes.MapPersistent = &RecMapPersistent;
            gMGPipeRouteEscapes.CreateShaderState = &RecCreateShaderState;
            gMGPipeRouteEscapes.SetProgramBindings = &RecSetProgramBindings;
            InstallTableRecorders();
            g_stream = &stream;
        }
        ~RecorderScope() {
            g_stream = nullptr;
            UninstallTableRecorders();
            gMGPipeRouteEscapes = g_savedEscapes;
            MG_Backend::gBackendFunctionsTable = m_savedTable;
            // The context goes while the verb session is still the recorder's: its objects' deaths
            // emit, and nothing after this scope may land in another fixture's stream.
            MG_State::pGLContext.reset();
            MG_State::pGLContext = Move(m_savedContext);
            MGPipeSetResourceOps(m_savedResourceOps);
            MG_Config::Features.PipePush = m_savedPush;
            MG_Record::SetMonolithVerbPortRoutingForTesting(m_savedPortRouting);
            g_session = nullptr;
        }
        RecorderScope(const RecorderScope&) = delete;
        RecorderScope& operator=(const RecorderScope&) = delete;

    private:
        Uint64 m_savedPush = 0;
        Bool m_savedPortRouting = false;
        const MGPipeResourceOps* m_savedResourceOps = nullptr;
        SharedPtr<MG_State::GLState::GLContext> m_savedContext;
        MG_Backend::GlobalBackendFunctionsTable m_savedTable{};
        RecordingVerbSession m_session;
    };

    // ---- the fixtures: fixed frontend call sequences ---------------------------------------------

    const char* kVs = R"(#version 430 core
layout(location = 0) in vec4 a_position;
uniform vec4 u_offset;
void main() { gl_Position = a_position + u_offset; }
)";
    const char* kFs = R"(#version 430 core
uniform sampler2D u_texture;
out vec4 o_color;
void main() { o_color = texture(u_texture, vec2(0.5)); }
)";

    GLuint MakeShader(GLenum stage, const char* source) {
        const GLuint shader = GL::CreateShader(stage);
        GL::ShaderSource(shader, 1, &source, nullptr);
        GL::CompileShader(shader);
        return shader;
    }

    // Buffers, a vertex array, a sampled texture, a program with a uniform, and the draw verbs.
    // `vertexBytesDelta` changes one input for the negative control.
    void RunBufferVertexArrayDraw(GLsizeiptr vertexBytesDelta = 0) {
        const GLuint program = GL::CreateProgram();
        GL::AttachShader(program, MakeShader(GL_VERTEX_SHADER, kVs));
        GL::AttachShader(program, MakeShader(GL_FRAGMENT_SHADER, kFs));
        GL::LinkProgram(program);
        GLint linked = GL_FALSE;
        GL::GetProgramiv(program, GL_LINK_STATUS, &linked);
        ASSERT_EQ(linked, GL_TRUE) << "the fixture's program did not link";
        GL::UseProgram(program);
        GL::Uniform4f(GL::GetUniformLocation(program, "u_offset"), 0.25f, -0.5f, 0.0f, 0.0f);
        GL::Uniform1i(GL::GetUniformLocation(program, "u_texture"), 0);

        GLuint texture = 0;
        GL::GenTextures(1, &texture);
        GL::BindTexture(GL_TEXTURE_2D, texture);
        GL::TexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 2, 2);
        const Uint8 texels[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
        GL::TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, texels);
        GL::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        GL::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        GLuint vao = 0;
        GL::GenVertexArrays(1, &vao);
        GL::BindVertexArray(vao);
        const GLfloat vertices[12] = {-1, -1, 0, 1, 1, -1, 0, 1, 0, 1, 0, 1};
        GLuint vbo = 0;
        GL::GenBuffers(1, &vbo);
        GL::BindBuffer(GL_ARRAY_BUFFER, vbo);
        GL::BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizeof(vertices)) + vertexBytesDelta, nullptr,
                       GL_STATIC_DRAW);
        GL::BufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);
        GL::VertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), nullptr);
        GL::EnableVertexAttribArray(0);
        const GLushort indices[3] = {0, 1, 2};
        GLuint ebo = 0;
        GL::GenBuffers(1, &ebo);
        GL::BindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        GL::BufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

        GL::Enable(GL_BLEND);
        GL::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        GL::Viewport(0, 0, 64, 32);
        GL::DrawArrays(GL_TRIANGLES, 0, 3);
        GL::DrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, nullptr);
        GL::DrawArraysInstanced(GL_TRIANGLES, 0, 3, 4);
        GL::Disable(GL_BLEND);
        GL::DrawElementsInstancedBaseVertex(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, nullptr, 2, 0);
    }

    // A framebuffer over two textures: clears of both kinds, a blit, a mipmap generation.
    void RunTextureFramebufferClearBlit() {
        GLuint textures[2] = {0, 0};
        GL::GenTextures(2, textures);
        for (const GLuint texture : textures) {
            GL::BindTexture(GL_TEXTURE_2D, texture);
            GL::TexStorage2D(GL_TEXTURE_2D, 3, GL_RGBA8, 8, 8);
        }
        GLuint framebuffers[2] = {0, 0};
        GL::GenFramebuffers(2, framebuffers);
        for (Int i = 0; i < 2; ++i) {
            GL::BindFramebuffer(GL_FRAMEBUFFER, framebuffers[i]);
            GL::FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[i], 0);
        }
        GL::BindFramebuffer(GL_FRAMEBUFFER, framebuffers[0]);
        GL::ClearColor(0.25f, 0.5f, 0.75f, 1.0f);
        GL::Clear(GL_COLOR_BUFFER_BIT);
        const GLfloat color[4] = {1.0f, 0.0f, 0.5f, 1.0f};
        GL::ClearBufferfv(GL_COLOR, 0, color);
        GL::BindFramebuffer(GL_READ_FRAMEBUFFER, framebuffers[0]);
        GL::BindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffers[1]);
        GL::BlitFramebuffer(0, 0, 8, 8, 0, 0, 4, 4, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        GL::BindTexture(GL_TEXTURE_2D, textures[1]);
        GL::GenerateMipmap(GL_TEXTURE_2D);
    }

    // ---- the comparison ------------------------------------------------------------------------

    std::filesystem::path GoldenPath(const char* fixture) {
        return std::filesystem::path(MGL_RECORDER_GOLDEN_DIR) / (String(fixture) + ".txt");
    }

    String Render(const std::vector<Record>& stream) {
        std::ostringstream out;
        out << "# MobileGL recorder golden (MG_Test/Pipe/RecorderGoldenTest.cpp). One record per line:\n"
               "# index call bytes hash. Regenerate deliberately: MOBILEGL_RECORDER_GOLDEN_WRITE=1.\n";
        for (SizeT i = 0; i < stream.size(); ++i) {
            char hash[17];
            std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(stream[i].Hash));
            out << i << ' ' << stream[i].Op << ' ' << stream[i].Bytes << ' ' << hash << '\n';
        }
        return out.str();
    }

    std::vector<String> Lines(const String& text) {
        std::vector<String> lines;
        std::istringstream in(text);
        for (String line; std::getline(in, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            lines.push_back(line);
        }
        return lines;
    }

    // Empty when the stream matches the golden; otherwise the first difference, named.
    String Compare(const char* fixture, const std::vector<Record>& stream) {
        std::ifstream file(GoldenPath(fixture));
        if (!file.good()) return "no golden at " + GoldenPath(fixture).string();
        const String golden((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        const std::vector<String> want = Lines(golden);
        const std::vector<String> got = Lines(Render(stream));
        for (SizeT i = 0; i < std::max(want.size(), got.size()); ++i) {
            const String a = i < want.size() ? want[i] : "<end of golden>";
            const String b = i < got.size() ? got[i] : "<end of stream>";
            if (a != b) return "record " + std::to_string(i) + ": golden '" + a + "', recorded '" + b + "'";
        }
        return {};
    }

    Bool WriteRequested() {
        const char* knob = std::getenv("MOBILEGL_RECORDER_GOLDEN_WRITE");
        return knob != nullptr && std::strcmp(knob, "1") == 0;
    }

    void CheckAgainstGolden(const char* fixture, const std::vector<Record>& stream) {
        ASSERT_FALSE(stream.empty()) << fixture << ": the recorder saw no record at all";
        if (WriteRequested()) {
            std::error_code ec;
            std::filesystem::create_directories(GoldenPath(fixture).parent_path(), ec);
            std::ofstream out(GoldenPath(fixture), std::ios::binary);
            out << Render(stream);
            ASSERT_TRUE(out.good()) << fixture << ": could not write " << GoldenPath(fixture).string();
            FAIL() << fixture << ": golden REWRITTEN at " << GoldenPath(fixture).string()
                   << " (MOBILEGL_RECORDER_GOLDEN_WRITE=1); review the diff and rerun without the knob";
        }
        const String difference = Compare(fixture, stream);
        EXPECT_TRUE(difference.empty())
            << fixture << ": the pushed record stream changed - " << difference
            << ". If the change is deliberate, regenerate with MOBILEGL_RECORDER_GOLDEN_WRITE=1 and say why "
               "in the commit";
    }

    // HANDLES AND CSO NUMBERS ARE A FUNCTION OF WHAT THE PROCESS DID BEFORE, so a golden is only
    // comparable for the FIRST capture in a process. ctest runs every case in a process of its own
    // (gtest_discover_tests), which is the shape the goldens are recorded and checked in; a case
    // that compares a later capture against a golden says so instead of failing for that reason.
    Uint32 g_captures = 0;

    std::vector<Record> Capture(void (*fixture)()) {
        ++g_captures;
        std::vector<Record> stream;
        {
            RecorderScope scope(stream);
            fixture();
        }
        return stream;
    }

    Bool FirstCaptureInProcess() { return g_captures == 0; }

    // Index of the first record whose op is `op` in a golden, or -1.
    Int64 FirstGoldenRecordOf(const char* fixture, const char* op) {
        std::ifstream file(GoldenPath(fixture));
        const String golden((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        const std::vector<String> lines = Lines(golden);
        for (SizeT i = 0; i < lines.size(); ++i) {
            std::istringstream fields(lines[i]);
            String index, name;
            fields >> index >> name;
            if (name == op) return static_cast<Int64>(i);
        }
        return -1;
    }
} // namespace

TEST(RecorderGolden, BufferVertexArrayDraw) {
    if (!FirstCaptureInProcess()) GTEST_SKIP() << "compare the first capture of a process only (ctest runs each case alone)";
    const std::vector<Record> stream = Capture([] { RunBufferVertexArrayDraw(); });
    CheckAgainstGolden("BufferVertexArrayDraw", stream);
}

TEST(RecorderGolden, TextureFramebufferClearBlit) {
    if (!FirstCaptureInProcess()) GTEST_SKIP() << "compare the first capture of a process only (ctest runs each case alone)";
    const std::vector<Record> stream = Capture(&RunTextureFramebufferClearBlit);
    CheckAgainstGolden("TextureFramebufferClearBlit", stream);
}

// The SHAPE of the stream (which records, in which order) is a function of the calls alone; only
// handle values and content-addressed CSO numbers depend on what the process did before, which is
// why the goldens compare a process's first capture. Two captures in one process: the same calls.
TEST(RecorderGolden, TheSameCallsRecordTheSameStreamTwice) {
    const std::vector<Record> first = Capture(&RunTextureFramebufferClearBlit);
    const std::vector<Record> second = Capture(&RunTextureFramebufferClearBlit);
    ASSERT_EQ(first.size(), second.size());
    for (SizeT i = 0; i < first.size(); ++i) {
        EXPECT_EQ(first[i].Op, second[i].Op) << "record " << i;
    }
}

// NEGATIVE CONTROL 1: one field of one record perturbed - what an emitter that wrote one field
// differently produces - must turn the comparison red AT that record, and nowhere before it.
TEST(RecorderGolden, APerturbedRecordFieldIsCaughtAtThatRecord) {
    if (WriteRequested()) GTEST_SKIP() << "goldens are being rewritten";
    if (!FirstCaptureInProcess()) GTEST_SKIP() << "compare the first capture of a process only (ctest runs each case alone)";
    // The first draw: a verb record, i.e. the session half of the recorder rather than the tables.
    const Int64 victim = FirstGoldenRecordOf("BufferVertexArrayDraw", "DrawVbo");
    ASSERT_GE(victim, 0) << "the golden has no DrawVbo record to perturb";
    g_perturbIndex = victim;
    const std::vector<Record> perturbed = Capture([] { RunBufferVertexArrayDraw(); });
    g_perturbIndex = -1;
    const String difference = Compare("BufferVertexArrayDraw", perturbed);
    EXPECT_EQ(difference.rfind("record " + std::to_string(victim) + ":", 0), 0u)
        << "a one-field change in record " << victim << " was not reported there: '" << difference << "'";
}

// NEGATIVE CONTROL 2: the same fixture with ONE GL input changed (the vertex buffer is defined four
// bytes longer) must differ from the golden - first at a resource record, which is where that input
// enters the stream.
TEST(RecorderGolden, AChangedGlInputChangesTheStream) {
    if (WriteRequested()) GTEST_SKIP() << "goldens are being rewritten";
    if (!FirstCaptureInProcess()) GTEST_SKIP() << "compare the first capture of a process only (ctest runs each case alone)";
    const std::vector<Record> stream = Capture([] { RunBufferVertexArrayDraw(4); });
    const String difference = Compare("BufferVertexArrayDraw", stream);
    ASSERT_FALSE(difference.empty())
        << "a buffer defined with a different size recorded the golden stream - the recorder is not "
           "seeing the resource family";
    EXPECT_NE(difference.find(" Resource"), String::npos)
        << "the first difference is not a resource record: " << difference;
}

int main(int argc, char** argv) {
    MobileGL::Initialize();
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
