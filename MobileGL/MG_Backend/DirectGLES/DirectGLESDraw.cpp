// MobileGL - MobileGL/MG_Backend/DirectGLES/DirectGLESDraw.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "DirectGLESInternal.h"

namespace MobileGL::MG_Backend::DirectGLES {
    const Uint8* ResolveIndirectCommandBytes(const void* indirect, SizeT requiredBytes, const char* label) {
        // P5c (hd, CONTRACT-P5C §3.5): under an active transport the command buffer resolves
        // from the handle the verb record carried, and its CPU bytes are the SERVER's staged
        // shadow (rule E) - the client's GL_DRAW_INDIRECT_BUFFER binding slot, the frontend
        // object's MappedData()/GetSize() (B3) and the client slot allocator (T2) are never
        // read. Coverage is asserted: commands read from bytes no record staged would be
        // zero-filled, which is a missing record, not a valid command stream.
        const MG_Pipe::MGPipeHandle handle = MG_Pipe::MGPipeApplier().VerbIndirectBuffer;
        if (!MG_Pipe::MGPipeHandleIsNull(handle)) {
            auto* resource = BufferImpl::FindBufferResourceForHandle(handle);
            const Uint8* base = resource ? BufferImpl::SplitHostBytes(*resource) : nullptr;
            // P8-C: a store nothing ever staged but a shader wrote (the GPU-culling shape)
            // takes its first copy from this server's own GL buffer. A store that HAS a copy
            // is not refreshed here: most indirect draws execute natively and read no field
            // on the CPU, so ExecuteIndexed/ArraysIndirectCommands refresh the fields only
            // when they consume them.
            if (base == nullptr && resource != nullptr) {
                base = BufferImpl::SplitHostBytesForCpuRead(*resource, handle, "indirect_command_bytes");
            }
            const SizeT commandOffset = reinterpret_cast<SizeT>(indirect);
            if (base == nullptr) {
                MGLOG_E_ONCE("%s skipped: the verb's indirect buffer {%u, %u} has no staged "
                             "shadow on this side and no GPU write to take one from",
                             label, handle.Slot, handle.Gen);
                return nullptr;
            }
            if (commandOffset + requiredBytes > BufferImpl::ResourceWidthForHandle(handle)) {
                MGLOG_E_ONCE("%s skipped: invalid GL_DRAW_INDIRECT_BUFFER binding or range", label);
                return nullptr;
            }
            BufferImpl::RequireStagedCoverage(*resource, base, commandOffset,
                                              commandOffset + requiredBytes, "indirect_command_bytes");
            return base + commandOffset;            }
        // No buffer was bound: the frontend already raised the GL error; keep the old
        // fall-through shape (a null indirect pointer is the same skip it was).
        if (!indirect) {
            MGLOG_E_ONCE("%s skipped: indirect pointer is null", label);
            return nullptr;
        }
        MGLOG_E_ONCE("%s skipped: the verb carried no indirect buffer handle", label);
        return nullptr;
    }

    // TODO: deletion for deleted objects

    namespace BufferImpl {
        // P5e (mv): THE ARM SELECTOR of this family - VertexInputReadsRecords() - moved to
        // Managers.h, unchanged. MultiDraw.cpp is a second translation unit on the same draw
        // path and has to select the same arm; a copy of the conjunction there would be two
        // selectors for one family. Its rationale travelled with it.

        // P5e (vi): "does this draw fetch any attribute out of the application's own memory",
        // answered FROM THE RECORD instead of from the frontend VAO. The emitter publishes
        // Res == kMGPipeNullHandle for a client-memory array (VertexInputEmit.h says so in as
        // many words: "A client-memory array is Res == kMGPipeNullHandle, and that is not a
        // hole"), and the window is truncated at the highest ENABLED attribute, so a null Res
        // inside [Start, Start+Count) is exactly the client-sourced case and nothing else.
        //
        // BOTH VIEWS ARE NEEDED, and taking only the buffer one would be wrong in the common
        // direction: the window covers [0, highest enabled + 1), so a VAO whose attribute 0 is
        // DISABLED and whose attribute 1 feeds from a VBO publishes entry[0].Res == null too.
        // A null Res is "client-sourced" only for an attribute the CONFIGURATION has enabled,
        // which is what the vertex-elements record answers. Reading one view and not the other
        // would send every such ordinary draw back to the frontend VAO and keep the row this
        // package is retiring alive for no reason.
        //
        // A prefix scan rather than a memo: Count is 1-3 on the measured workload, not 32, and
        // this replaces a cross-TU accessor call plus a SharedPtr dereference per DrawArrays.
        Bool AnyClientSideVertexArrayInRecord() {
            const auto& st = MG_Pipe::MGPipeApplier();
            if (st.VertexBufferCount == 0) return false;
            const MG_Pipe::MGPipeHandle elements = st.BoundVertexElements;
            if (MG_Pipe::MGPipeHandleIsNull(elements) || elements.Slot >= st.VertexElementsCsos.size()) {
                return false;
            }
            const auto& record = st.VertexElementsCsos[elements.Slot];
            if (!record.Live || record.Gen != elements.Gen) return false;

            const Uint32 begin = st.VertexBufferStart;
            const Uint32 end = begin + st.VertexBufferCount;
            for (Uint32 i = begin; i < end && i < MG_Pipe::kMGPipeMaxVertexAttribs; ++i) {
                if (!record.Attributes[i].Enabled) continue;
                if (MG_Pipe::MGPipeHandleIsNull(st.VertexBuffers[i].Res)) return true;
            }
            return false;
        }
    } // namespace BufferImpl

    namespace VertexArrayImpl {
        // The indirect executors' form: the command's own words ARE the draw's fetch, and a
        // command block a shader wrote this frame is exactly what these shapes exist for - so the
        // command buffer's shadow is reconciled before it is read, and `commandBytes` (the
        // caller's possibly stale copy) is only the fallback for a client-authored block.
        static Bool SyncClientSideVertexArraysForIndirectFetch(
            const Uint8* commandBytes, SizeT commandOffset, GLsizei stride, GLsizei index, Uint8 indexSize,
            const SharedPtr<MG_State::GLState::BufferObject>& commandBuffer) {
            return true;
        }

        // The indexed entry points' form. `indices` is a byte offset into the bound element buffer
        // when one is bound and the application's own array otherwise, which is the same
        // distinction every one of them already makes when it issues the draw.
        static Bool SyncClientSideVertexArraysForIndexedFetch(GLenum type, GLsizei count, const void* indices,
                                                              GLsizei instanceCount, GLint baseVertex,
                                                              GLuint baseInstance) {
            if (count <= 0 || instanceCount <= 0) return true;
            // MONOLITH ONLY - see SyncClientSideVertexArraysForFetch; this wrapper's own read of
            // the bound VAO is the frontend read that must not happen on an apply thread.
            return true;
        }

        static void SyncClientSideVertexArraysForDrawArrays(GLint first, GLsizei count) {
            if (!BufferImpl::AnyClientSideVertexArrayInRecord()) return;
            // P13: client-memory arrays are monolith-only - under a transport the client refuses
            // them (Fatal{UnmigratedVerb, "DrawArrays+CLIENT_ARRAYS"}) before any record goes out,
            // so a record naming one here means the bytes below belong to another process.
            if (MG_Config::Transport != MG_Config::TransportMode::Monolith) {
                MGLOG_F("MGPipe: Fatal{RoleViolation, \"ClientArrays\"} - a draw carrying a "
                        "client-memory vertex array reached the backend under a transport; the "
                        "client's refusal exists to make this unreachable");
                std::abort();
            }
            auto* twin = ResolveVaoTwin(MG_Pipe::MGPipeApplier().BoundVertexElements);
            if (twin == nullptr) return;
            const auto& currentVAO = MG_Pipe::gPipeInputs->GetBoundVertexArray();
            if (!currentVAO) return;
            twin->SyncClientSideAttributesForDrawArrays(currentVAO, first, count);
            return;
        }
    } // namespace VertexArrayImpl

    // ---- gl_ViewportIndex routing emulation, draw half ---------------------------------------
    // See the block comment in Managers.h for what this is and why. Here is the state half: one
    // replay pass per DISTINCT viewport state, each pushing that state onto the ES context's one
    // viewport / one scissor / one depth range and telling the fragment gate which indices it
    // serves.

    namespace ViewportRoutingImpl {
        // One replay pass: the state to push, and the set of gl_ViewportIndex values whose
        // fragments this pass is allowed to keep.
        struct RoutingPass {
            IntVec4 viewport{};
            IntVec4 scissorBox{};
            FloatVec2 depthRange{};
            Bool scissorTest = false;
            Uint32 indexMask = 0;
        };

        static constexpr Uint32 kAllViewportsMask =
            (RenderStateParameters::MAX_VIEWPORTS >= 32)
                ? 0xFFFFFFFFu
                : ((1u << RenderStateParameters::MAX_VIEWPORTS) - 1u);

        // The plan for the draw currently being issued. A file-scope buffer rather than a return
        // value because Begin/Apply/End are three calls around a draw the caller writes, and a
        // fixed array of 16 keeps it allocation-free on a path that is per draw. NOT re-entrant,
        // which is a property of the call sites and not an accident: every wrap in this file and
        // in MultiDraw.cpp is around the innermost native glDraw*, so no replay can begin inside
        // another - and a multi-draw tier that replayed its whole loop would be nesting.
        static Array<RoutingPass, RenderStateParameters::MAX_VIEWPORTS> g_passes{};
        static Uint g_passCount = 0;
        static PrgramImpl::BackendProgramObjectImpl* g_routedProgram = nullptr;

        // What index `i` actually rasterizes against, resolved exactly the way SyncRenderState
        // resolves index 0 - including both substitutions it makes, which are not cosmetic:
        //
        //   * a viewport of zero extent means "the application has never called glViewport", and
        //     GL's initial viewport is the whole surface, which the frontend cannot spell before
        //     a surface exists;
        //   * a scissor rectangle is read through the WRITTEN flag and not through its extent,
        //     because glScissor(0, 0, 0, 0) is a legal request meaning "reject every fragment"
        //     and is byte-identical to the never-written default that means the opposite.
        //
        // Resolving them here rather than deferring to SyncRenderState is what makes the grouping
        // below correct: two indices that differ only in a field that resolves to the same
        // rectangle really do rasterize identically and must share one pass.
        static RoutingPass ResolveIndexState(const RenderStateParameters& parameters, Uint index,
                                             Int surfaceWidth, Int surfaceHeight) {
            RoutingPass pass;
            const FloatVec4& viewport = parameters.Viewports[index];
            pass.viewport = IntVec4(static_cast<Int>(std::lround(viewport.x())),
                                    static_cast<Int>(std::lround(viewport.y())),
                                    static_cast<Int>(std::lround(viewport.z())),
                                    static_cast<Int>(std::lround(viewport.w())));
            if ((pass.viewport.z() <= 0 || pass.viewport.w() <= 0) && surfaceWidth > 0 && surfaceHeight > 0) {
                pass.viewport = IntVec4(0, 0, surfaceWidth, surfaceHeight);
            }
            pass.scissorBox = parameters.ScissorBoxes[index];
            if ((parameters.ScissorBoxWrittenMask & (1u << index)) == 0 && surfaceWidth > 0 &&
                surfaceHeight > 0) {
                pass.scissorBox = IntVec4(0, 0, surfaceWidth, surfaceHeight);
            }
            pass.depthRange = parameters.DepthRanges[index];
            pass.scissorTest = (parameters.ScissorTestEnabledMask & (1u << index)) != 0;
            return pass;
        }

        static Bool SameState(const RoutingPass& a, const RoutingPass& b) {
            return a.viewport == b.viewport && a.scissorBox == b.scissorBox &&
                   a.depthRange == b.depthRange && a.scissorTest == b.scissorTest;
        }
    } // namespace ViewportRoutingImpl

    Uint BeginViewportRoutingPasses() {
        using namespace ViewportRoutingImpl;
        g_passCount = 1;
        g_routedProgram = nullptr;

        // The whole emulation behind one static load, for every application that has never built
        // a program writing gl_ViewportIndex - which is all of them but the conformance suite.
        // Without it every draw in the process would pay GetCurrentBackendProgram's chain of
        // frontend lookups for an answer that cannot change.
        if (!g_anyProgramRoutesViewportIndex) {
            return 1;
        }

        auto* program = GetCurrentBackendProgram();
        if (program == nullptr || !program->RoutesViewportIndex()) {
            return 1;
        }
        g_routedProgram = program;
        // The gate reads zero until something writes it, and a zero mask discards every fragment.
        // So this is not an optimization that can be skipped in the one-pass case - it is what
        // keeps a routing program drawing at all.
        program->SetViewportPassMask(kAllViewportsMask);

        // Replaying multiplies every side effect the vertex and geometry stages have, and the
        // fragment gate can only undo the ones that happen in the FRAGMENT stage. Transform
        // feedback records per emitted primitive, so a replayed draw would write its vertices N
        // times; rasterizer discard means there are no fragments to gate at all, so replaying
        // would be pure cost with nothing to show for it. Both fall back to a single pass with an
        // open gate, i.e. to the pre-emulation behaviour, rather than to wrong data.
        if (MG_Pipe::gPipeInputs->IsTransformFeedbackActive() ||
            MG_Pipe::gPipeInputs->IsCapabilityEnabled(CapabilityInput::RasterizerDiscard)) {
            return 1;
        }

        const auto& parameters = MG_Pipe::gPipeInputs->GetRenderStateParameters();
        Int surfaceWidth = 0;
        Int surfaceHeight = 0;
        if (!QueryCurrentSurfaceSize(surfaceWidth, surfaceHeight)) {
            surfaceWidth = 0;
            surfaceHeight = 0;
        }

        Uint count = 0;
        for (Uint index = 0; index < RenderStateParameters::MAX_VIEWPORTS; ++index) {
            const RoutingPass resolved =
                ResolveIndexState(parameters, index, surfaceWidth, surfaceHeight);
            Uint existing = 0;
            for (; existing < count; ++existing) {
                if (SameState(g_passes[existing], resolved)) break;
            }
            if (existing == count) {
                g_passes[count] = resolved;
                ++count;
            }
            g_passes[existing].indexMask |= (1u << index);
        }

        // One group is the overwhelmingly common case - it is what glViewport, glScissor and
        // glDepthRange leave behind, because ARB_viewport_array defines all three as writing
        // EVERY index. The mask is already open and index 0's state is what SyncRenderState
        // pushed, so there is nothing to replay and nothing to restore.
        if (count <= 1) {
            g_passCount = 1;
            return 1;
        }
        g_passCount = count;
        return count;
    }

    void ApplyViewportRoutingPass(Uint pass) {
        using namespace ViewportRoutingImpl;
        if (pass >= g_passCount || g_routedProgram == nullptr) {
            return;
        }
        const RoutingPass& entry = g_passes[pass];
        g_GLESFuncs.glViewport(entry.viewport.x(), entry.viewport.y(), entry.viewport.z(),
                               entry.viewport.w());
        g_GLESFuncs.glScissor(entry.scissorBox.x(), entry.scissorBox.y(), entry.scissorBox.z(),
                              entry.scissorBox.w());
        // ES has one scissor-test enable where GL has sixteen, so the per-index bit becomes a
        // per-pass glEnable/glDisable. This is the half DirectVulkan cannot do at all (Vulkan has
        // no per-viewport scissor toggle either and has to widen a disabled index's rectangle to
        // the whole framebuffer instead); here the rectangle stays honest.
        entry.scissorTest ? g_GLESFuncs.glEnable(GL_SCISSOR_TEST) : g_GLESFuncs.glDisable(GL_SCISSOR_TEST);
        g_GLESFuncs.glDepthRangef(entry.depthRange.x(), entry.depthRange.y());
        g_routedProgram->SetViewportPassMask(entry.indexMask);
    }

    void EndViewportRoutingPasses(Uint passCount) {
        using namespace ViewportRoutingImpl;
        if (passCount <= 1) {
            // Nothing was pushed and the mask is already open; leaving the shadow alone here is
            // what keeps a non-routing draw at exactly its previous cost.
            g_routedProgram = nullptr;
            return;
        }
        if (g_routedProgram != nullptr) {
            // Any draw that reaches the driver without going through a replay - an internal blit,
            // or a path this emulation has not been taught about - must not inherit the last
            // pass's mask and paint nothing.
            g_routedProgram->SetViewportPassMask(kAllViewportsMask);
        }
        g_routedProgram = nullptr;
        g_passCount = 0;
        // The viewport, scissor, scissor-test enable and depth range now on the ES context belong
        // to the last replay pass, and the shadow SyncRenderState diffs against does not know it.
        // A full resync is the honest repair and costs one state push on the next draw, which
        // only a viewport-routing workload ever pays.
        RenderStateImpl::InvalidateSyncedRenderState();
    }

    static Bool SupportsNativeIndirectDraws() {
        return g_GLESCapabilities.SupportsDrawIndirect;
    }

    // P8-C: the command bytes an indirect executor is about to CONSUME on the CPU, made current.
    // Consumed means: every field on the CPU path (no native indirect), or on the native path the
    // uniforms fed from the command - gl_BaseVertex whenever the program reads it, and
    // gl_BaseInstance when the program reads it without the mg_IndirectParams storage view that
    // would give it the GPU's word. A native draw that feeds neither reads nothing and keeps its
    // pointer, so a compute-culled indirect buffer drawn every frame costs no readback.
    static const Uint8* CurrentIndirectCommandBytes(MG_Pipe::MGPipeHandle handle, const Uint8* commandBytes,
                                                    SizeT commandOffset, Bool useNative, Bool indexed) {
        if (MG_Pipe::MGPipeHandleIsNull(handle)) return commandBytes;
        if (useNative) {
            const auto program = GetCurrentBackendProgram();
            if (program == nullptr) return commandBytes;
            const Bool baseVertexFromCpu = indexed && program->ReadsBaseVertex();
            const Bool baseInstanceFromCpu = program->GetIndirectParamsBinding() < 0 && program->ReadsBaseInstance();
            if (!baseVertexFromCpu && !baseInstanceFromCpu) return commandBytes;
        }
        auto* resource = BufferImpl::FindBufferResourceForHandle(handle);
        if (resource == nullptr) return commandBytes;
        const Uint8* base = BufferImpl::SplitHostBytesForCpuRead(*resource, handle, "indirect_command_fields");
        return base != nullptr ? base + commandOffset : commandBytes;
    }

    // Runs an (indexed) indirect multi-draw. When a GL_DRAW_INDIRECT_BUFFER is bound the draws
    // execute natively on the GPU so commands written by compute shaders (e.g. Flywheel's
    // culling pipeline updating instanceCount) are honored; the CPU shadow is still consulted
    // for the per-command baseInstance, which is CPU-authored, to feed the mg_BaseInstance
    // shader emulation. Without GL_EXT_base_instance a non-zero baseInstance in the command is
    // technically undefined in ES; mobile drivers ignore the reserved word, instanced-array
    // fetches were never baseInstance-offset here anyway, and the CPU fallback cannot see
    // GPU-written command fields at all - so native is never worse. ANGLE-on-Vulkan instead
    // leaks the word into gl_InstanceID (it becomes vkCmdDrawIndexedIndirect's firstInstance);
    // the shader rewrite compensates by rebasing gl_InstanceID during these draws when
    // IndirectDrawInstanceIdIncludesBaseInstance is set (PromoteDrawParameterGlobalsToUniforms).
    // Only client-memory commands take the CPU per-command loop.
    static void ExecuteIndexedIndirectCommands(GLenum mode, GLenum type, SizeT indexSize, const Uint8* commandBytes,
                                               SizeT commandOffset,
                                               const SharedPtr<MG_State::GLState::BufferObject>& drawIndirectBuffer,
                                               GLsizei drawcount, GLsizei stride, const char* label) {
        (void)label;
        // An indirect command's firstIndex/count live in GPU memory, so the substitution has
        // to rewrite the whole element array buffer rather than this draw's range - which is
        // exactly what it does when no CPU-known count is handed to it. Held for the whole
        // command loop so every command in the batch reads the rewritten copy.
        //
        // firstIndex counts ELEMENTS, so it survives a widened copy untouched; what does not
        // survive is the type and the element size, which are re-taken from the substitution
        // below for both the native and the CPU-unrolled path.
        const ScopedRestartIndexSubstitution restart(type, /*count=*/0, /*indices=*/nullptr);
        if (!restart.DrawIsValid()) return;
        type = restart.IndexType();
        indexSize = MG_Util::GetGLTypeSize(type);
        // P5c (hd, CONTRACT-P5C §3.5): under an active transport the command buffer is the
        // handle the verb record carried (MGPipeApplier's verb stash) and the SSBO view's
        // resource resolves from it by handle - the frontend binding slot and the client slot
        // allocator are never read (T2).
        const MG_Pipe::MGPipeHandle verbBufferHandle =
            MG_Pipe::MGPipeApplier().VerbIndirectBuffer;
        const Bool useNative =
            !MG_Pipe::MGPipeHandleIsNull(verbBufferHandle) && SupportsNativeIndirectDraws();
        commandBytes = CurrentIndirectCommandBytes(verbBufferHandle, commandBytes, commandOffset, useNative,
                                                   /*indexed=*/true);
        if (useNative) {
            // gl_BaseInstance must observe GPU-written command fields; expose the indirect
            // buffer to the program's mg_IndirectParams SSBO view and address it per draw.
            const auto backendProgram = GetCurrentBackendProgram();
            const Int paramsBinding = backendProgram ? backendProgram->GetIndirectParamsBinding() : -1;
            if (paramsBinding >= 0) {
                auto* resource =
                    BufferImpl::EnsureBufferResourceForHandle(nullptr, verbBufferHandle);
                if (resource && resource->id != 0) {
                    BufferImpl::BindBufferBaseCached(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(paramsBinding),
                                                     resource->id);
                }
            }
            // gl_BaseVertex has no SSBO view of its own: the command's baseVertex word is read
            // from the CPU shadow, so a command whose baseVertex a compute shader wrote this
            // frame is not observable here (baseInstance is, through the view above). Feeding
            // the stale-but-usually-correct shadow beats leaving the uniform at the previous
            // draw's value, which is what a program reading gl_BaseVertex saw before.
            const Bool feedBaseVertex = CurrentProgramReadsBaseVertex();
            for (GLsizei i = 0; i < drawcount; ++i) {
                const SizeT cmdByteOffset = commandOffset + static_cast<SizeT>(i) * stride;
                SetCurrentDrawID(static_cast<Uint32>(i));
                if (paramsBinding >= 0 && backendProgram) {
                    // baseInstance is the 5th word of DrawElementsIndirectCommand.
                    backendProgram->SetBaseInstanceWordIndex(static_cast<Int32>((cmdByteOffset + 16) / 4));
                    if (feedBaseVertex) {
                        DrawElementsIndirectCommand cmd{};
                        std::memcpy(&cmd, commandBytes + static_cast<SizeT>(i) * stride, sizeof(cmd));
                        SetCurrentBaseVertex(cmd.baseVertex);
                    }
                } else {
                    DrawElementsIndirectCommand cmd{};
                    std::memcpy(&cmd, commandBytes + static_cast<SizeT>(i) * stride, sizeof(cmd));
                    SetCurrentBaseInstance(cmd.baseInstance);
                    SetCurrentBaseVertex(cmd.baseVertex);
                }
                ForEachViewportRoutingPass([&] {
                    if (!VertexArrayImpl::SyncClientSideVertexArraysForIndirectFetch(
                            commandBytes, commandOffset, stride, i, static_cast<Uint8>(indexSize), drawIndirectBuffer))
                        return;
                    g_GLESFuncs.glDrawElementsIndirect(mode, type, reinterpret_cast<const void*>(cmdByteOffset));
                });
            }
        } else {
            for (GLsizei i = 0; i < drawcount; ++i) {
                DrawElementsIndirectCommand cmd{};
                std::memcpy(&cmd, commandBytes + static_cast<SizeT>(i) * stride, sizeof(cmd));
                if (cmd.count == 0 || cmd.instanceCount == 0) {
                    continue;
                }
                SetCurrentDrawID(static_cast<Uint32>(i));
                SetCurrentBaseInstance(cmd.baseInstance);
                SetCurrentBaseVertex(cmd.baseVertex);
                const auto indexByteOffset = static_cast<SizeT>(cmd.firstIndex) * indexSize;
                ForEachViewportRoutingPass([&] {
                    if (!VertexArrayImpl::SyncClientSideVertexArraysForIndirectFetch(
                            commandBytes, commandOffset, stride, i, static_cast<Uint8>(indexSize), drawIndirectBuffer))
                        return;
                    g_GLESFuncs.glDrawElementsInstancedBaseVertex(
                        mode, static_cast<GLsizei>(cmd.count), type, reinterpret_cast<const GLvoid*>(indexByteOffset),
                        static_cast<GLsizei>(cmd.instanceCount), cmd.baseVertex);
                });
            }
        }
        SetCurrentDrawID(0);
        SetCurrentBaseInstance(0);
        SetCurrentBaseVertex(0);
    }

    static void ExecuteArraysIndirectCommands(GLenum mode, const Uint8* commandBytes, SizeT commandOffset,
                                              const SharedPtr<MG_State::GLState::BufferObject>& drawIndirectBuffer,
                                              GLsizei drawcount, GLsizei stride, const char* label) {
        (void)label;
        // DrawArraysIndirectCommand has no baseVertex word, so gl_BaseVertex is zero for every
        // command here. Written BEFORE the draws, not merely restored after them: the previous
        // draw is what leaves a stale value, and restoring afterwards would only protect the
        // NEXT draw while these commands ran with the stale one.
        SetCurrentBaseVertex(0);
        // P5c (hd, CONTRACT-P5C §3.5): see ExecuteIndexedIndirectCommands - the verb's handle,
        // never the frontend binding slot or the client allocator.
        const MG_Pipe::MGPipeHandle verbBufferHandle =
            MG_Pipe::MGPipeApplier().VerbIndirectBuffer;
        const Bool useNative =
            !MG_Pipe::MGPipeHandleIsNull(verbBufferHandle) && SupportsNativeIndirectDraws();
        commandBytes = CurrentIndirectCommandBytes(verbBufferHandle, commandBytes, commandOffset, useNative,
                                                   /*indexed=*/false);
        if (useNative) {
            const auto backendProgram = GetCurrentBackendProgram();
            const Int paramsBinding = backendProgram ? backendProgram->GetIndirectParamsBinding() : -1;
            if (paramsBinding >= 0) {
                auto* resource =
                    BufferImpl::EnsureBufferResourceForHandle(nullptr, verbBufferHandle);
                if (resource && resource->id != 0) {
                    BufferImpl::BindBufferBaseCached(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(paramsBinding),
                                                     resource->id);
                }
            }
            for (GLsizei i = 0; i < drawcount; ++i) {
                const SizeT cmdByteOffset = commandOffset + static_cast<SizeT>(i) * stride;
                SetCurrentDrawID(static_cast<Uint32>(i));
                if (paramsBinding >= 0 && backendProgram) {
                    // baseInstance is the 4th word of DrawArraysIndirectCommand.
                    backendProgram->SetBaseInstanceWordIndex(static_cast<Int32>((cmdByteOffset + 12) / 4));
                } else {
                    DrawArraysIndirectCommand cmd{};
                    std::memcpy(&cmd, commandBytes + static_cast<SizeT>(i) * stride, sizeof(cmd));
                    SetCurrentBaseInstance(cmd.baseInstance);
                }
                ForEachViewportRoutingPass([&] {
                    if (!VertexArrayImpl::SyncClientSideVertexArraysForIndirectFetch(
                            commandBytes, commandOffset, stride, i, 0, drawIndirectBuffer))
                        return;
                    g_GLESFuncs.glDrawArraysIndirect(mode, reinterpret_cast<const void*>(cmdByteOffset));
                });
            }
        } else {
            for (GLsizei i = 0; i < drawcount; ++i) {
                DrawArraysIndirectCommand cmd{};
                std::memcpy(&cmd, commandBytes + static_cast<SizeT>(i) * stride, sizeof(cmd));
                if (cmd.count == 0 || cmd.instanceCount == 0) {
                    continue;
                }
                SetCurrentDrawID(static_cast<Uint32>(i));
                SetCurrentBaseInstance(cmd.baseInstance);
                ForEachViewportRoutingPass([&] {
                    if (!VertexArrayImpl::SyncClientSideVertexArraysForIndirectFetch(
                            commandBytes, commandOffset, stride, i, 0, drawIndirectBuffer))
                        return;
                    g_GLESFuncs.glDrawArraysInstanced(mode, static_cast<GLint>(cmd.first),
                                                      static_cast<GLsizei>(cmd.count),
                                                      static_cast<GLsizei>(cmd.instanceCount));
                });
            }
        }
        SetCurrentDrawID(0);
        SetCurrentBaseInstance(0);
    }


    void Clear(GLbitfield mask) {
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG && MOBILEGL_ENABLE_SCOPE_MARKER
        DebugImpl::OpenGLScopeMarker marker(__func__);
#endif
        TextureImpl::SyncNeccessaryTextures();
        FramebufferImpl::SyncCurrentFBO();
        // A colour clear is exactly the operation that is allowed to write a widened
        // attachment's alpha - it is what puts the 1.0 there that every later draw is masked
        // away from. SyncCurrentFBO ran first, so g_alphaWidenedDrawBufferMask already describes
        // the framebuffer this clear will land on.
        RenderStateImpl::SyncRenderState(/*forColorClear=*/(mask & GL_COLOR_BUFFER_BIT) != 0);

        BindCurrentFBO(FramebufferTarget::Draw);

        // Debug-only diagnostics: is each offscreen clear complete and unscissored?
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG
        {
            static int diagCount = 0;
            GLint fboId = 0;
            g_GLESFuncs.glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fboId);
            if (fboId != 0 && diagCount++ < 900) {
                GLint color0 = 0, depthName = 0, box[4] = {0};
                GLboolean scissor = g_GLESFuncs.glIsEnabled(GL_SCISSOR_TEST);
                GLboolean cmask[4] = {0};
                GLfloat cc[4] = {0};
                g_GLESFuncs.glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                                                  GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &color0);
                g_GLESFuncs.glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                                                  GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &depthName);
                g_GLESFuncs.glGetIntegerv(GL_SCISSOR_BOX, box);
                g_GLESFuncs.glGetBooleanv(GL_COLOR_WRITEMASK, cmask);
                g_GLESFuncs.glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
                MGLOG_D("CLEAR fbo=%d color0=%d depth=%d mask=0x%x scissor=%d box=(%d,%d,%d,%d) cmask=%d%d%d%d cc=(%g,%g,%g,%g)",
                        fboId, color0, depthName, mask, (int)scissor, box[0], box[1], box[2], box[3], (int)cmask[0],
                        (int)cmask[1], (int)cmask[2], (int)cmask[3], cc[0], cc[1], cc[2], cc[3]);
            }
        }
#endif

        // GLES clamps the glClearColor state to [0,1] (desktop GL keeps it unclamped
        // for float color buffers). Minecraft's OIT clears its depth-bounds RGBA32F
        // target to -FLT_MAX as the MAX-blend identity, so an out-of-range clear
        // color must go through glClearBufferfv, which GLES does not clamp.
        GLbitfield remainingMask = mask;
        if ((mask & GL_COLOR_BUFFER_BIT) != 0) {
            const FloatVec4& cc = MG_Pipe::gPipeInputs->GetRenderStateParameters().ClearColor;
            const Bool outOfRange = cc.x() < 0.f || cc.x() > 1.f || cc.y() < 0.f || cc.y() > 1.f || cc.z() < 0.f ||
                                    cc.z() > 1.f || cc.w() < 0.f || cc.w() > 1.f;
            // A widened attachment's stored alpha has to end up 1.0, and glClear applies ONE
            // clear colour to every draw buffer - so a framebuffer that mixes a widened
            // attachment with a native one cannot be served by doctoring glClearColor. Take the
            // same per-draw-buffer glClearBufferfv route the out-of-range case already uses and
            // substitute the alpha only where it belongs. Scissor and the colour write mask apply
            // to glClearBufferfv exactly as they do to glClear, so a scissored clear stays
            // scissored and an application that masked alpha off still gets its way (the storage
            // then keeps the 1.0 an earlier clear left, which is the same answer).
            //
            // glClearBufferfv on an INTEGER colour buffer is GL_INVALID_OPERATION, so a
            // framebuffer with one of those as a draw buffer keeps plain glClear - which ES
            // leaves undefined for integer colour buffers anyway, and which an application that
            // wants a defined answer must replace with glClearBufferuiv/iv (those DO substitute
            // the widened alpha). The out-of-range trigger is left exactly as it was.
            const Uint32 widenedDrawBuffers = FramebufferImpl::g_alphaWidenedDrawBufferMask;
            const Bool widenedColorClear =
                widenedDrawBuffers != 0 && FramebufferImpl::g_integerColorDrawBufferMask == 0;
            GLint clearDrawFbo = 0;
            g_GLESFuncs.glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &clearDrawFbo);
            if ((outOfRange || widenedColorClear) && clearDrawFbo != 0) {
                GLint maxDrawBuffers = 0;
                GLint clearedCount = 0;
                GLint firstDb = -1;
                g_GLESFuncs.glGetIntegerv(GL_MAX_DRAW_BUFFERS, &maxDrawBuffers);
                for (GLint i = 0; i < maxDrawBuffers; ++i) {
                    GLint db = GL_NONE;
                    g_GLESFuncs.glGetIntegerv(GL_DRAW_BUFFER0 + static_cast<GLenum>(i), &db);
                    if (i == 0) firstDb = db;
                    if (db != GL_NONE) {
                        const Bool widened = i < 32 && (widenedDrawBuffers & (1u << i)) != 0;
                        const GLfloat value[4] = {cc.x(), cc.y(), cc.z(), widened ? 1.0f : cc.w()};
                        g_GLESFuncs.glClearBufferfv(GL_COLOR, i, value);
                        ++clearedCount;
                    }
                }
                remainingMask &= ~static_cast<GLbitfield>(GL_COLOR_BUFFER_BIT);
                (void)clearedCount;
                (void)firstDb;
                // Debug-only diagnostics: verify the unclamped clear actually landed.
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG
                {
                    static int diagCount = 0;
                    if (diagCount++ < 20) {
                        const GLenum clrErr = g_GLESFuncs.glGetError();
                        GLint prevRead = 0, prevPbo = 0;
                        g_GLESFuncs.glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
                        g_GLESFuncs.glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prevPbo);
                        g_GLESFuncs.glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
                        g_GLESFuncs.glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)clearDrawFbo);
                        GLint prevReadBuf = GL_COLOR_ATTACHMENT0;
                        g_GLESFuncs.glGetIntegerv(GL_READ_BUFFER, &prevReadBuf);
                        g_GLESFuncs.glReadBuffer(GL_COLOR_ATTACHMENT0);
                        GLfloat rb[4] = {0};
                        g_GLESFuncs.glReadPixels(100, 100, 1, 1, GL_RGBA, GL_FLOAT, rb);
                        const GLenum rbErr = g_GLESFuncs.glGetError();
                        int feDb0 = -1, feDb1 = -1;
                        Uint feIdx = 0, feVer = 0;
                        // P5e (fb): DEBUG-ONLY, and still a BARRIER_PULLED read. A diagnostic
                        // is not a reason to touch the binding slot on an apply thread, so
                        // under a transport the same four numbers come off the draw record -
                        // the external index has no record counterpart and stays 0, which reads
                        // as "the record did not say".
                        if (const auto* rec = BoundFramebufferRecord(
                                FramebufferTarget::Draw)) {
                            feVer = static_cast<Uint>(rec->ContentHash);
                            feDb0 = (int)rec->DrawBuffers[0];
                            feDb1 = (int)rec->DrawBuffers[1];
                        }
                        MGLOG_D("CLEARV fbo=%d clrErr=0x%x rbErr=0x%x cc.x=%g cleared=%d firstDb=0x%x prevReadBuf=0x%x "
                                "feFbo=%u feVer=%u feDb=[%d,%d] stored=(%g,%g,%g,%g)",
                                clearDrawFbo, clrErr, rbErr, cc.x(), clearedCount, firstDb, prevReadBuf, feIdx, feVer,
                                feDb0, feDb1, rb[0], rb[1], rb[2], rb[3]);
                        g_GLESFuncs.glReadBuffer(static_cast<GLenum>(prevReadBuf));
                        g_GLESFuncs.glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prevRead);
                        g_GLESFuncs.glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)prevPbo);
                    }
                }
#endif
            }
        }
        if (remainingMask != 0) {
            g_GLESFuncs.glClear(remainingMask);
        }
    }

    // ---------------------------------------------------------------------------
    // Arbitrary-index primitive restart
    //
    // Desktop GL restarts on whatever index glPrimitiveRestartIndex named; GLES core only
    // ever restarts on the all-ones value of the index type. When the two agree - which
    // includes every GL_PRIMITIVE_RESTART_FIXED_INDEX user - the render state push at
    // SyncRenderState is the whole implementation and nothing here does any work. When they
    // disagree the index DATA is rewritten into a scratch element array buffer.
    //
    // This used to throw instead. A throw here unwinds a C++ exception through the C GL ABI
    // and takes the process down - the same hazard GL_Texture.cpp and RenderState.cpp
    // already call out - so an application that merely asked for a legal desktop feature
    // died rather than got an error.
    // ---------------------------------------------------------------------------

    namespace {
        struct RestartScratchBuffer {
            Uint id = 0;
            SizeT capacity = 0;
        };

        RestartScratchBuffer g_restartIndices;
        Vector<Uint8> g_restartStaging;

        // Past this the rewrite would stage and re-upload hundreds of megabytes on EVERY
        // draw (the copy is not memoised, exactly as on the Vulkan side). Decline instead of
        // trying: a draw that renders nothing is recoverable, a stall of that size is not.
        constexpr SizeT kMaxRestartRewriteBytes = SizeT{1} << 26; // 64 MiB

        // The index type one step wider than this one, or 0 when there is none. Widening is how
        // an all-ones value that is a REAL vertex index keeps its meaning while the all-ones
        // value of the destination type serves as the restart sentinel: a source that cannot
        // spell 0xFFFF cannot collide with a 16-bit sentinel, and likewise 8 -> 16.
        GLenum WiderIndexType(GLenum indexType) {
            switch (indexType) {
            case GL_UNSIGNED_BYTE: return GL_UNSIGNED_SHORT;
            case GL_UNSIGNED_SHORT: return GL_UNSIGNED_INT;
            default: return 0;
            }
        }

        Uint32 ReadIndex(const Uint8* source, SizeT i, SizeT indexSize) {
            switch (indexSize) {
            case 1: return source[i];
            case 2: {
                Uint16 narrow = 0;
                std::memcpy(&narrow, source + i * 2, sizeof(narrow));
                return narrow;
            }
            default: {
                Uint32 wide = 0;
                std::memcpy(&wide, source + i * 4, sizeof(wide));
                return wide;
            }
            }
        }

        void WriteIndex(Uint8* destination, SizeT i, SizeT indexSize, Uint32 value) {
            switch (indexSize) {
            case 1: destination[i] = static_cast<Uint8>(value); break;
            case 2: {
                const Uint16 narrow = static_cast<Uint16>(value);
                std::memcpy(destination + i * 2, &narrow, sizeof(narrow));
                break;
            }
            default: std::memcpy(destination + i * 4, &value, sizeof(value)); break;
            }
        }

        // True when any index in the range already holds the type's all-ones value, i.e. when
        // that value is doing double duty as a real vertex index and so cannot also be the
        // restart sentinel. Only asked on the rare substitution path.
        Bool ContainsFixedRestartIndex(const Uint8* source, SizeT indexCount, SizeT indexSize,
                                       Uint32 fixedMax) {
            for (SizeT i = 0; i < indexCount; ++i) {
                if (ReadIndex(source, i, indexSize) == fixedMax) return true;
            }
            return false;
        }

        // Copies index data, replacing every occurrence of the application's restart index with
        // the all-ones value of the DESTINATION type - the only one GLES restarts on. The
        // destination may be wider than the source, which is what makes the copy lossless: a
        // source index equal to the source's all-ones value zero-extends to something the wider
        // sentinel can never equal, so it stays the vertex it was.
        //
        // Same width in and out is the degenerate case, used when the source contains no
        // all-ones index at all (nothing to protect) or when there is no wider type to move to.
        // In that last case only - a GL_UNSIGNED_INT stream that really does use index
        // 0xFFFFFFFF while asking to restart on a different one - a legal index has to be
        // nudged to 0xFFFFFFFE, because 32 bits cannot hold both meanings. The caller logs it;
        // it is the one input this feature cannot represent.
        void RewriteRestartIndices(const Uint8* source, SizeT indexCount, SizeT sourceIndexSize,
                                   SizeT destinationIndexSize, Uint32 applicationRestartIndex,
                                   Uint32 destinationFixedMax, Vector<Uint8>& output) {
            output.resize(indexCount * destinationIndexSize);
            for (SizeT i = 0; i < indexCount; ++i) {
                Uint32 value = ReadIndex(source, i, sourceIndexSize);
                if (value == applicationRestartIndex) {
                    value = destinationFixedMax;
                } else if (value == destinationFixedMax) {
                    // Only reachable when no widening was possible; see above.
                    value = destinationFixedMax - 1;
                }
                WriteIndex(output.data(), i, destinationIndexSize, value);
            }
        }

        // Whole-buffer respecify through the manager-wide staging target, so binding it
        // disturbs no VAO state. glBufferData orphans the previous store, so the upload
        // never waits on a draw still reading the old contents out of the same name.
        Bool UploadRestartScratch(SizeT bytes, const void* data) {
            if (g_restartIndices.id == 0) {
                GLuint id = 0;
                g_GLESFuncs.glGenBuffers(1, &id);
                if (id == 0) return false;
                g_restartIndices.id = id;
                g_restartIndices.capacity = 0;
            }
            BufferImpl::BindBufferId(BufferImpl::TempBufferTarget, g_restartIndices.id);
            SizeT capacity = g_restartIndices.capacity == 0 ? bytes : g_restartIndices.capacity;
            while (capacity < bytes) capacity *= 2;
            g_GLESFuncs.glBufferData(BufferImpl::TempBufferTarget, static_cast<GLsizeiptr>(capacity), nullptr,
                                     GL_STREAM_DRAW);
            g_restartIndices.capacity = capacity;
            if (data != nullptr && bytes != 0) {
                g_GLESFuncs.glBufferSubData(BufferImpl::TempBufferTarget, 0, static_cast<GLsizeiptr>(bytes), data);
                if (MG_Util::PipeStats::Enabled()) {
                    // Rewritten index list staged on the draw path: in a split build these
                    // bytes are the index-mirror-versus-ship decision of section 8.
                    MG_Util::PipeStats::AddBytes(MG_Util::PipeStats::ByteClass::StageIndexClient,
                                                 static_cast<Uint64>(bytes));
                }
            }
            return true;
        }
    } // namespace

    RestartSubstitutionKind ResolveRestartSubstitution(GLenum indexType) {
        if (!MG_Pipe::gPipeInputs->IsCapabilityEnabled(CapabilityInput::PrimitiveRestart) ||
            MG_Pipe::gPipeInputs->IsCapabilityEnabled(CapabilityInput::PrimitiveRestartFixedIndex)) {
            return RestartSubstitutionKind::None;
        }
        const Uint32 fixedMax = MG_Util::FixedRestartIndexForGLType(indexType);
        if (fixedMax == 0) return RestartSubstitutionKind::None;
        const Uint32 restartIndex = MG_Pipe::gPipeInputs->GetPrimitiveRestartIndex();
        if (restartIndex == fixedMax) return RestartSubstitutionKind::None;
        // Strictly greater, never truncated. GL 4.6 core 10.3.6 compares the fetched index
        // zero-extended against the full 32-bit state, so an index this type cannot hold matches
        // nothing. Truncating instead - glPrimitiveRestartIndex(0x100) over GL_UNSIGNED_BYTE data
        // becoming "restart on 0" - turns the most common index in any mesh into a restart.
        if (restartIndex > fixedMax) return RestartSubstitutionKind::SuppressRestart;
        return RestartSubstitutionKind::RewriteIndices;
    }

    void OnRestartSubstitutionContextDestroyed() {
        g_restartIndices = {};
        g_restartStaging.clear();
        g_restartStaging.shrink_to_fit();
    }

    ScopedSuppressedPrimitiveRestart::ScopedSuppressedPrimitiveRestart(RestartSubstitutionKind kind) {
        if (kind != RestartSubstitutionKind::SuppressRestart) return;
        // SyncRenderState turned the driver's fixed-index restart on because GL_PRIMITIVE_RESTART
        // is enabled; for this draw's index type it would restart on a value the application
        // never named. Toggled directly rather than through the render-state shadow, and put back
        // in the destructor, so the shadow stays true and the next draw pays nothing.
        g_GLESFuncs.glDisable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
        m_suppressed = true;
    }

    ScopedSuppressedPrimitiveRestart::~ScopedSuppressedPrimitiveRestart() {
        if (!m_suppressed) return;
        g_GLESFuncs.glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
    }

    ScopedRestartIndexSubstitution::ScopedRestartIndexSubstitution(GLenum indexType, GLsizei count,
                                                                   const void* indices)
        : m_kind(ResolveRestartSubstitution(indexType)), m_capOverride(m_kind), m_indices(indices),
          m_indexType(indexType) {
        if (m_kind != RestartSubstitutionKind::RewriteIndices) {
            return;
        }
        const SizeT sourceIndexSize = MG_Util::GetGLTypeSize(indexType);
        const Uint32 fixedMax = MG_Util::FixedRestartIndexForGLType(indexType);
        const Uint32 applicationRestartIndex = MG_Pipe::gPipeInputs->GetPrimitiveRestartIndex();

        const Uint8* source = nullptr;
        SizeT indexCount = 0;
        SizeT sourceByteOffset = 0;
        Bool hasElementBuffer = false;
        // -1: monolith, or this arm never resolved a backend id, and the tail asks
        // BoundElementArrayBufferId() exactly as it always did.
        Int serverElementBinding = -1;

        // P5c (hd, CONTRACT-P5C §3.5): with an active transport this constructor runs on the
        // apply thread, where the frontend BufferObject's legacy accessors are Fatal by name
        // (BufferObject.cpp's "buffer-legacy-arm" refusal). The element buffer therefore
        // resolves from the applier's set_index_buffer state (T2) and the bytes read from the
        // server's own staged shadow - "the backend's ScopedRestartIndexSubstitution reads the
        // index bytes it needs on its side" (CONTRACT-P5B d1), the same arm
        // MultiDrawElementsIndirectCount takes for its command buffer. A draw whose record
        // carried client indices (no index buffer bound) takes the client-pointer arm below,
        // `indices` already resolved through the server's segment resolver. Monolith takes
        // the original body, verbatim.
        const auto& applierState = MG_Pipe::MGPipeApplier();
        const MG_Pipe::MGPipeHandle elements = applierState.IndexBuffer.Res;
        serverElementBinding = 0;
        if (!MG_Pipe::MGPipeHandleIsNull(elements)) {
            auto* elementResource = BufferImpl::FindBufferResourceForHandle(elements);
            // The WHOLE buffer is rewritten, not just this draw's range, so that every
            // index keeps its position: an indirect draw's firstIndex lives in GPU memory
            // and cannot be adjusted from here. It is an ELEMENT index, so it survives
            // widening unchanged.
            const SizeT sizeBytes = BufferImpl::ResourceWidthForHandle(elements);
            if (sizeBytes < sourceIndexSize) {
                return; // Nothing to restart on; let the driver see the draw unchanged.
            }
            if (sizeBytes > kMaxRestartRewriteBytes) {
                MGLOG_E_ONCE("Draw skipped: GL_PRIMITIVE_RESTART with restart index %u needs the %zu-byte element "
                             "array buffer rewritten every draw, which is past the %zu-byte ceiling. Use "
                             "GL_PRIMITIVE_RESTART_FIXED_INDEX, or set glPrimitiveRestartIndex to the all-ones "
                             "value of the index type.",
                             applicationRestartIndex, sizeBytes, kMaxRestartRewriteBytes);
                m_valid = false;
                return;
            }
            // The staged shadow is the source of truth for CPU reads on this side: a
            // persistent map's blocks were consumed by the applier before this verb ran, and
            // (P8-C) a store a shader wrote on this side is read back from this server's own
            // GL buffer first - no writeback ever reaches the server's copy.
            const Uint8* hostBytes =
                elementResource
                    ? BufferImpl::SplitHostBytesForCpuRead(*elementResource, elements,
                                                           "primitive_restart_substitution")
                    : nullptr;
            if (hostBytes == nullptr) {
                MGLOG_E_ONCE("Draw skipped: GL_PRIMITIVE_RESTART with restart index %u needs a CPU-readable copy of "
                             "the bound element array buffer and none is available.",
                             applicationRestartIndex);
                m_valid = false;
                return;
            }
            // The M-3 rule for a whole-store reader (ResourceContentIsDeclared in ManagersBuffers.cpp): for a store
            // whose content the application SUPPLIED a coverage gap is a missing record and is
            // Fatal by name; for one it ORPHANED the gap is its own undefined content and
            // reading the shadow's zero-fill is exactly what the monolith arm's
            // MappedData() answers.
            const MG_Pipe::MGPipeResourceRecord* elementRecord = nullptr;
            if (elements.Slot < applierState.Resources.size()) {
                const auto& candidate = applierState.Resources[elements.Slot];
                if (candidate.Live && candidate.Gen == elements.Gen) elementRecord = &candidate;
            }
            if (elementRecord != nullptr && elementRecord->Desc.HasDefinedContent != 0) {
                BufferImpl::RequireStagedCoverage(*elementResource, hostBytes, 0, sizeBytes,
                                                  "primitive_restart_substitution");
            }
            hasElementBuffer = true;
            serverElementBinding = static_cast<Int>(elementResource->id);
            source = hostBytes;
            indexCount = sizeBytes / sourceIndexSize;
            sourceByteOffset = reinterpret_cast<SizeT>(indices);
        }
        if (!hasElementBuffer) {
            // No element array buffer: `indices` is a client pointer, so only the draw's own
            // range is readable and an indirect draw has nothing to read at all.
            if (count <= 0 || indices == nullptr || sourceIndexSize == 0) {
                MGLOG_E_ONCE("Draw skipped: GL_PRIMITIVE_RESTART with restart index %u needs either a bound element "
                             "array buffer or a client index array with a CPU-known count.",
                             applicationRestartIndex);
                m_valid = false;
                return;
            }
            if (static_cast<SizeT>(count) * sourceIndexSize > kMaxRestartRewriteBytes) {
                MGLOG_E_ONCE("Draw skipped: GL_PRIMITIVE_RESTART index rewrite of %zu bytes is past the %zu-byte "
                             "ceiling.",
                             static_cast<SizeT>(count) * sourceIndexSize, kMaxRestartRewriteBytes);
                m_valid = false;
                return;
            }
            source = static_cast<const Uint8*>(indices);
            indexCount = static_cast<SizeT>(count);
        }

        // Widen only when the source really does use the all-ones value as a vertex index -
        // otherwise the sentinel is free and the copy stays the caller's width, which keeps the
        // common substitution allocation-for-allocation identical to the narrow form.
        GLenum destinationType = indexType;
        SizeT destinationIndexSize = sourceIndexSize;
        if (ContainsFixedRestartIndex(source, indexCount, sourceIndexSize, fixedMax)) {
            const GLenum wider = WiderIndexType(indexType);
            // An element-array offset that is not a whole number of indices cannot be rescaled
            // into the widened copy, so such a draw keeps the narrow (lossy) form.
            const Bool offsetIsWholeIndices = sourceIndexSize != 0 && (sourceByteOffset % sourceIndexSize) == 0;
            if (wider != 0 && offsetIsWholeIndices &&
                indexCount * MG_Util::GetGLTypeSize(wider) <= kMaxRestartRewriteBytes) {
                destinationType = wider;
                destinationIndexSize = MG_Util::GetGLTypeSize(wider);
            } else {
                MGLOG_E_ONCE("GL_PRIMITIVE_RESTART with restart index %u over index data that also uses the "
                             "all-ones index %u: this index type cannot spell both, so every all-ones index is "
                             "drawn as %u instead. Use GL_PRIMITIVE_RESTART_FIXED_INDEX, or keep the all-ones "
                             "value out of the index data.",
                             applicationRestartIndex, fixedMax, fixedMax - 1);
            }
        }

        const Uint32 destinationFixedMax = MG_Util::FixedRestartIndexForGLType(destinationType);
        RewriteRestartIndices(source, indexCount, sourceIndexSize, destinationIndexSize, applicationRestartIndex,
                              destinationFixedMax, g_restartStaging);

        if (!UploadRestartScratch(g_restartStaging.size(), g_restartStaging.data())) {
            MGLOG_E_ONCE("Draw skipped: could not allocate the scratch element array buffer for GL_PRIMITIVE_RESTART "
                         "index substitution.");
            m_valid = false;
            return;
        }
        m_previousBinding = 0;
        if (serverElementBinding >= 0) m_previousBinding = static_cast<Uint>(serverElementBinding);
        BufferImpl::BindBufferId(GL_ELEMENT_ARRAY_BUFFER, g_restartIndices.id);
        m_substituted = true;
        m_indexType = destinationType;
        // The rewritten copy starts at byte 0 of the scratch buffer and holds one
        // destination-width element per source element, so an EBO-sourced draw keeps its ELEMENT
        // offset (rescaled to the new width) and a client-memory draw reads from the front.
        m_indices = hasElementBuffer
                        ? reinterpret_cast<const void*>((sourceByteOffset / sourceIndexSize) * destinationIndexSize)
                        : nullptr;
    }

    ScopedRestartIndexSubstitution::~ScopedRestartIndexSubstitution() {
        if (!m_substituted) return;
        BufferImpl::BindBufferId(GL_ELEMENT_ARRAY_BUFFER, m_previousBinding);
    }

    void DrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
        if (!VertexArrayImpl::SyncClientSideVertexArraysForIndexedFetch(type, count, indices, 1, 0, 0)) return;
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG && MOBILEGL_ENABLE_SCOPE_MARKER
        DebugImpl::OpenGLScopeMarker marker(__func__);
#endif
        DrawSyncFlags syncBit = DrawSyncBit::IndexBuffer;
        PrepareForDraw(syncBit);
        const ScopedRestartIndexSubstitution restart(type, count, indices);
        if (!restart.DrawIsValid()) return;
        ForEachViewportRoutingPass([&] {
            g_GLESFuncs.glDrawElements(mode, count, restart.IndexType(), restart.Indices());
        });
    }

    void DrawArrays(GLenum mode, GLint first, GLsizei count) {
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG && MOBILEGL_ENABLE_SCOPE_MARKER
        DebugImpl::OpenGLScopeMarker marker(__func__);
#endif
        DrawSyncFlags syncBit = DrawSyncBit::None;
        PrepareForDraw(syncBit);
        VertexArrayImpl::SyncClientSideVertexArraysForDrawArrays(first, count);
        ForEachViewportRoutingPass([&] {
            g_GLESFuncs.glDrawArrays(mode, first, count);
        });
    }

    void DrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const GLvoid* indices, GLint basevertex) {
        if (!VertexArrayImpl::SyncClientSideVertexArraysForIndexedFetch(type, count, indices, 1, basevertex, 0)) return;
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG && MOBILEGL_ENABLE_SCOPE_MARKER
        DebugImpl::OpenGLScopeMarker marker(__func__);
#endif
        DrawSyncFlags syncBit = DrawSyncBit::IndexBuffer;
        PrepareForDraw(syncBit);
        const ScopedRestartIndexSubstitution restart(type, count, indices);
        if (!restart.DrawIsValid()) return;
        SetCurrentBaseVertex(basevertex);
        ForEachViewportRoutingPass([&] {
            g_GLESFuncs.glDrawElementsBaseVertex(mode, count, restart.IndexType(), restart.Indices(), basevertex);
        });
        SetCurrentBaseVertex(0);
    }

    void MultiDrawArrays(GLenum mode, const GLint* first, const GLsizei* count, GLsizei drawcount) {
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG && MOBILEGL_ENABLE_SCOPE_MARKER
        DebugImpl::OpenGLScopeMarker marker(__func__);
#endif
        DrawSyncFlags syncBit = DrawSyncBit::None;
        PrepareForDraw(syncBit);

        // This loop IS the emulation - there is no batched tier for the non-indexed form -
        // so each sub-draw has to be given its own gl_DrawID here, exactly as the indexed
        // ladder and the indirect executors do. Without it every sub-draw of a
        // glMultiDrawArrays read draw index 0.
        const Bool feedDrawID = CurrentProgramReadsDrawID();
        for (GLsizei i = 0; i < drawcount; ++i) {
            // Client-side arrays are uploaded per sub-draw range, like the single DrawArrays path.
            VertexArrayImpl::SyncClientSideVertexArraysForDrawArrays(first[i], count[i]);
            if (feedDrawID) SetCurrentDrawID(static_cast<Uint32>(i));
            ForEachViewportRoutingPass([&] {
                g_GLESFuncs.glDrawArrays(mode, first[i], count[i]);
            });
        }
        if (feedDrawID) SetCurrentDrawID(0);
    }

    void MultiDrawElementsBaseVertex(GLenum mode, const GLsizei* count, GLenum type, const GLvoid* const* indices,
                                     GLsizei drawcount, const GLint* basevertex) {
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG && MOBILEGL_ENABLE_SCOPE_MARKER
        DebugImpl::OpenGLScopeMarker marker(__func__);
#endif
        MultiDrawImpl::DrawElementsBatch(mode, count, type, indices, drawcount, basevertex);
    }

    void MultiDrawElementsIndirect(GLenum mode, GLenum type, const void* indirect, GLsizei drawcount, GLsizei stride) {
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG && MOBILEGL_ENABLE_SCOPE_MARKER
        DebugImpl::OpenGLScopeMarker marker(__func__);
#endif
        if (drawcount <= 0) {
            return;
        }
        if (stride == 0) {
            stride = sizeof(DrawElementsIndirectCommand);
        }
        if (stride < static_cast<GLsizei>(sizeof(DrawElementsIndirectCommand))) {
            MGLOG_E_ONCE("MultiDrawElementsIndirect skipped: stride %d is smaller than command size %zu",
                    stride, sizeof(DrawElementsIndirectCommand));
            return;
        }

        DrawSyncFlags syncBit = DrawSyncBit::IndexBuffer | DrawSyncBit::IndirectBuffer | DrawSyncBit::Instancing;
        PrepareForDraw(syncBit);

        const SizeT indexSize = MG_Util::GetGLTypeSize(type);
        if (indexSize == 0) {
            MGLOG_E_ONCE("MultiDrawElementsIndirect skipped: unsupported index type 0x%x", type);
            return;
        }

        const auto* commandBytes = ResolveIndirectCommandBytes(
            indirect,
            static_cast<SizeT>(stride) * static_cast<SizeT>(drawcount - 1) + sizeof(DrawElementsIndirectCommand),
            "MultiDrawElementsIndirect");
        if (!commandBytes) {
            return;
        }

        const auto& drawIndirectBuffer =
            // P5c (hd, CONTRACT-P5C §3.5): with an active transport the frontend binding slot
            // is never read - the Execute* helpers resolve the buffer from the verb record's
            // handle (T2). Monolith reads the slot as it always did.
            SharedPtr<MG_State::GLState::BufferObject>(nullptr);
        ExecuteIndexedIndirectCommands(mode, type, indexSize, commandBytes, reinterpret_cast<SizeT>(indirect),
                                       drawIndirectBuffer, drawcount, stride, "MultiDrawElementsIndirect");
    }

    void MultiDrawElementsIndirectCount(GLenum mode, GLenum type, const void* indirect, GLintptr drawcount,
                                        GLsizei maxdrawcount, GLsizei stride) {
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG && MOBILEGL_ENABLE_SCOPE_MARKER
        DebugImpl::OpenGLScopeMarker marker(__func__);
#endif
        if (maxdrawcount <= 0) {
            return;
        }
        if (stride == 0) {
            stride = sizeof(DrawElementsIndirectCommand);
        }
        if (stride < static_cast<GLsizei>(sizeof(DrawElementsIndirectCommand))) {
            MGLOG_E_ONCE("MultiDrawElementsIndirectCount skipped: stride %d is smaller than command size %zu",
                    stride, sizeof(DrawElementsIndirectCommand));
            return;
        }

        DrawSyncFlags syncBit = DrawSyncBit::IndexBuffer | DrawSyncBit::IndirectBuffer | DrawSyncBit::Instancing;
        PrepareForDraw(syncBit);

        const SizeT indexSize = MG_Util::GetGLTypeSize(type);
        if (indexSize == 0) {
            MGLOG_E_ONCE("MultiDrawElementsIndirectCount skipped: unsupported index type 0x%x", type);
            return;
        }

        // P5c (hd, CONTRACT-P5C §3.5): with an active transport both buffers resolve from the
        // verb record's handles (MGPDrawIndirect::Buffer / ParameterBuffer) and the count
        // reads from the SERVER's staged shadow - the frontend binding slots, the frontend
        // accessors (B3) and the client allocator (T2) are never read. Monolith takes the
        // original body below, verbatim.
        const auto& applierState = MG_Pipe::MGPipeApplier();
        const SizeT commandOffset = reinterpret_cast<SizeT>(indirect);
        const SizeT commandBytes = commandOffset + static_cast<SizeT>(stride) * static_cast<SizeT>(maxdrawcount - 1) +
            sizeof(DrawElementsIndirectCommand);
        auto* drawResource = BufferImpl::FindBufferResourceForHandle(applierState.VerbIndirectBuffer);
        auto* paramResource =
            BufferImpl::FindBufferResourceForHandle(applierState.VerbIndirectParameterBuffer);
        // P8-C: the count is CONSUMED here, so the parameter buffer is made current first
        // (a store a shader wrote on this side is read back); the command buffer only gets a
        // first copy when it has none - its fields are made current by the executor, and
        // only when it consumes them (CurrentIndirectCommandBytes).
        const Uint8* const parameterBytes =
            paramResource ? BufferImpl::SplitHostBytesForCpuRead(*paramResource,
                                                                 applierState.VerbIndirectParameterBuffer,
                                                                 "indirect_count_parameter")
                          : nullptr;
        const Uint8* drawBytes = drawResource ? BufferImpl::SplitHostBytes(*drawResource) : nullptr;
        if (drawBytes == nullptr && drawResource != nullptr) {
            drawBytes = BufferImpl::SplitHostBytesForCpuRead(*drawResource, applierState.VerbIndirectBuffer,
                                                             "indirect_count_commands");
        }
        if (commandBytes > BufferImpl::ResourceWidthForHandle(applierState.VerbIndirectBuffer)) {
            MGLOG_E_ONCE("MultiDrawElementsIndirectCount skipped: invalid GL_DRAW_INDIRECT_BUFFER binding or range");
            return;
        }
        if (drawcount < 0 || static_cast<SizeT>(drawcount) + sizeof(Uint32) >
                                 BufferImpl::ResourceWidthForHandle(applierState.VerbIndirectParameterBuffer)) {
            MGLOG_E_ONCE("MultiDrawElementsIndirectCount skipped: invalid GL_PARAMETER_BUFFER binding or range");
            return;
        }
        // No staged shadow means no count to read, not a wrong one - the monolith body's
        // own rule for a buffer with no CPU shadow.
        if (parameterBytes == nullptr || drawBytes == nullptr) {
            MGLOG_E_ONCE("MultiDrawElementsIndirectCount skipped: CPU fallback cannot read the parameter or "
                    "draw-indirect buffer");
            return;
        }
        BufferImpl::RequireStagedCoverage(*drawResource, drawBytes, commandOffset, commandBytes,
                                          "multidraw_elements_indirect_count_commands");
        BufferImpl::RequireStagedCoverage(*paramResource, parameterBytes, static_cast<SizeT>(drawcount),
                                          static_cast<SizeT>(drawcount) + sizeof(Uint32),
                                          "multidraw_elements_indirect_count_parameter");
        Uint32 actualDrawCount = 0;
        std::memcpy(&actualDrawCount, parameterBytes + drawcount, sizeof(actualDrawCount));
        actualDrawCount = std::min<Uint32>(actualDrawCount, static_cast<Uint32>(maxdrawcount));
        ExecuteIndexedIndirectCommands(mode, type, indexSize, drawBytes + commandOffset, commandOffset,
                                       nullptr, static_cast<GLsizei>(actualDrawCount), stride,
                                       "MultiDrawElementsIndirectCount");
        return;
    }

    void MultiDrawArraysIndirect(GLenum mode, const void* indirect, GLsizei drawcount, GLsizei stride) {
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG && MOBILEGL_ENABLE_SCOPE_MARKER
        DebugImpl::OpenGLScopeMarker marker(__func__);
#endif
        if (drawcount <= 0) {
            return;
        }
        if (stride == 0) {
            stride = sizeof(DrawArraysIndirectCommand);
        }
        if (stride < static_cast<GLsizei>(sizeof(DrawArraysIndirectCommand))) {
            MGLOG_E_ONCE("MultiDrawArraysIndirect skipped: stride %d is smaller than command size %zu",
                    stride, sizeof(DrawArraysIndirectCommand));
            return;
        }

        DrawSyncFlags syncBit = DrawSyncBit::IndirectBuffer | DrawSyncBit::Instancing;
        PrepareForDraw(syncBit);

        const auto* commandBytes = ResolveIndirectCommandBytes(
            indirect,
            static_cast<SizeT>(stride) * static_cast<SizeT>(drawcount - 1) + sizeof(DrawArraysIndirectCommand),
            "MultiDrawArraysIndirect");
        if (!commandBytes) {
            return;
        }

        const auto& drawIndirectBuffer =
            // P5c (hd, CONTRACT-P5C §3.5): with an active transport the frontend binding slot
            // is never read - the Execute* helpers resolve the buffer from the verb record's
            // handle (T2). Monolith reads the slot as it always did.
            SharedPtr<MG_State::GLState::BufferObject>(nullptr);
        ExecuteArraysIndirectCommands(mode, commandBytes, reinterpret_cast<SizeT>(indirect), drawIndirectBuffer,
                                      drawcount, stride, "MultiDrawArraysIndirect");
    }

    // The non-indexed twin of MultiDrawElementsIndirectCount, and structurally identical to it:
    // ES has no GL_PARAMETER_BUFFER at all, so the draw count is read from the CPU shadow of the
    // bound one and the batch degenerates into an ordinary indirect multi-draw of that many
    // commands. Missing from the backend table until now, which made every
    // glMultiDrawArraysIndirectCount an INVALID_OPERATION ("backend does not support
    // indirect-parameter array draws") on DirectGLES while the extension was advertised.
    void MultiDrawArraysIndirectCount(GLenum mode, const void* indirect, GLintptr drawcount, GLsizei maxdrawcount,
                                      GLsizei stride) {
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG && MOBILEGL_ENABLE_SCOPE_MARKER
        DebugImpl::OpenGLScopeMarker marker(__func__);
#endif
        if (maxdrawcount <= 0) {
            return;
        }
        if (stride == 0) {
            stride = sizeof(DrawArraysIndirectCommand);
        }
        if (stride < static_cast<GLsizei>(sizeof(DrawArraysIndirectCommand))) {
            MGLOG_E_ONCE("MultiDrawArraysIndirectCount skipped: stride %d is smaller than command size %zu",
                    stride, sizeof(DrawArraysIndirectCommand));
            return;
        }

        DrawSyncFlags syncBit = DrawSyncBit::IndirectBuffer | DrawSyncBit::Instancing;
        PrepareForDraw(syncBit);

        // P5c (hd, CONTRACT-P5C §3.5): see the indexed twin - the verb record's handles and
        // the SERVER's staged shadow, never the frontend bindings or the client allocator.
        const auto& applierState = MG_Pipe::MGPipeApplier();
        const SizeT commandOffset = reinterpret_cast<SizeT>(indirect);
        const SizeT commandBytes = commandOffset + static_cast<SizeT>(stride) * static_cast<SizeT>(maxdrawcount - 1) +
            sizeof(DrawArraysIndirectCommand);
        auto* drawResource = BufferImpl::FindBufferResourceForHandle(applierState.VerbIndirectBuffer);
        auto* paramResource =
            BufferImpl::FindBufferResourceForHandle(applierState.VerbIndirectParameterBuffer);
        // P8-C: the count is CONSUMED here, so the parameter buffer is made current first
        // (a store a shader wrote on this side is read back); the command buffer only gets a
        // first copy when it has none - its fields are made current by the executor, and
        // only when it consumes them (CurrentIndirectCommandBytes).
        const Uint8* const parameterBytes =
            paramResource ? BufferImpl::SplitHostBytesForCpuRead(*paramResource,
                                                                 applierState.VerbIndirectParameterBuffer,
                                                                 "indirect_count_parameter")
                          : nullptr;
        const Uint8* drawBytes = drawResource ? BufferImpl::SplitHostBytes(*drawResource) : nullptr;
        if (drawBytes == nullptr && drawResource != nullptr) {
            drawBytes = BufferImpl::SplitHostBytesForCpuRead(*drawResource, applierState.VerbIndirectBuffer,
                                                             "indirect_count_commands");
        }
        if (commandBytes > BufferImpl::ResourceWidthForHandle(applierState.VerbIndirectBuffer)) {
            MGLOG_E_ONCE("MultiDrawArraysIndirectCount skipped: invalid GL_DRAW_INDIRECT_BUFFER binding or range");
            return;
        }
        if (drawcount < 0 || static_cast<SizeT>(drawcount) + sizeof(Uint32) >
                                 BufferImpl::ResourceWidthForHandle(applierState.VerbIndirectParameterBuffer)) {
            MGLOG_E_ONCE("MultiDrawArraysIndirectCount skipped: invalid GL_PARAMETER_BUFFER binding or range");
            return;
        }
        if (parameterBytes == nullptr || drawBytes == nullptr) {
            MGLOG_E_ONCE("MultiDrawArraysIndirectCount skipped: CPU fallback cannot read the parameter or "
                    "draw-indirect buffer");
            return;
        }
        BufferImpl::RequireStagedCoverage(*drawResource, drawBytes, commandOffset, commandBytes,
                                          "multidraw_arrays_indirect_count_commands");
        BufferImpl::RequireStagedCoverage(*paramResource, parameterBytes, static_cast<SizeT>(drawcount),
                                          static_cast<SizeT>(drawcount) + sizeof(Uint32),
                                          "multidraw_arrays_indirect_count_parameter");
        Uint32 actualDrawCount = 0;
        std::memcpy(&actualDrawCount, parameterBytes + drawcount, sizeof(actualDrawCount));
        actualDrawCount = std::min<Uint32>(actualDrawCount, static_cast<Uint32>(maxdrawcount));
        ExecuteArraysIndirectCommands(mode, drawBytes + commandOffset, commandOffset, nullptr,
                                      static_cast<GLsizei>(actualDrawCount), stride,
                                      "MultiDrawArraysIndirectCount");
        return;
    }

    void DrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                     const void* indices, GLint basevertex) {
        if (!VertexArrayImpl::SyncClientSideVertexArraysForIndexedFetch(type, count, indices, 1, basevertex, 0)) return;
        DrawSyncFlags syncBit = DrawSyncBit::IndexBuffer;
        PrepareForDraw(syncBit);
        const ScopedRestartIndexSubstitution restart(type, count, indices);
        if (!restart.DrawIsValid()) return;
        SetCurrentBaseVertex(basevertex);
        ForEachViewportRoutingPass([&] {
            g_GLESFuncs.glDrawRangeElementsBaseVertex(mode, start, end, count, restart.IndexType(), restart.Indices(),
                                                      basevertex);
        });
        SetCurrentBaseVertex(0);
    }

    void DrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices) {
        if (!VertexArrayImpl::SyncClientSideVertexArraysForIndexedFetch(type, count, indices, 1, 0, 0)) return;
        DrawSyncFlags syncBit = DrawSyncBit::IndexBuffer;
        PrepareForDraw(syncBit);
        const ScopedRestartIndexSubstitution restart(type, count, indices);
        if (!restart.DrawIsValid()) return;
        ForEachViewportRoutingPass([&] {
            g_GLESFuncs.glDrawRangeElements(mode, start, end, count, restart.IndexType(), restart.Indices());
        });
    }

    // True when the driver will apply baseInstance to the vertex fetch itself, in which case the
    // attribute-offset emulation must stay out of the way. SetCurrentBaseInstance is orthogonal
    // and runs either way - it feeds the shader's gl_BaseInstance, not the fetch.
    inline Bool UseNativeBaseInstance() {
        return g_GLESCapabilities.SupportsBaseInstance;
    }

    // The emulated shift has to be in place before PrepareForDraw, because that is what syncs the
    // VAO; a zero here is what un-shifts the arrays for the next ordinary draw.
    //
    // P3a (D-H2): this is the LEGACY arm's carrier. On the handle arm the draw's raw base
    // instance rides in MGPVertexBuffers::BaseInstance - a ContentHash input, so a base-instance
    // change that moves no buffer is still emitted rather than suppressed - and the server
    // decides whether to shift, because emulation ownership is the server's. The scopes below
    // are therefore compiled only where the pre-handle arm is (a pull build always).
    inline Uint32 EmulatedFetchBaseInstance(GLuint baseinstance) {
        return UseNativeBaseInstance() ? 0u : static_cast<Uint32>(baseinstance);
    }

#define MGL_SCOPED_FETCH_BASE_INSTANCE(name, value) ((void)(value))

    void DrawElementsInstancedBaseVertexBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                                     GLsizei instancecount, GLint basevertex, GLuint baseinstance) {
        if (!VertexArrayImpl::SyncClientSideVertexArraysForIndexedFetch(type, count, indices, instancecount, basevertex,
                                                                       baseinstance)) return;
        DrawSyncFlags syncBit = DrawSyncBit::IndexBuffer | DrawSyncBit::Instancing;
        MGL_SCOPED_FETCH_BASE_INSTANCE(fetchScope, EmulatedFetchBaseInstance(baseinstance));
        PrepareForDraw(syncBit);
        const ScopedRestartIndexSubstitution restart(type, count, indices);
        if (!restart.DrawIsValid()) return;
        SetCurrentBaseInstance(baseinstance);
        SetCurrentBaseVertex(basevertex);
        ForEachViewportRoutingPass([&] {
            if (UseNativeBaseInstance()) {
                g_GLESFuncs.glDrawElementsInstancedBaseVertexBaseInstanceEXT(mode, count, restart.IndexType(),
                                                                            restart.Indices(), instancecount,
                                                                            basevertex, baseinstance);
            } else {
                g_GLESFuncs.glDrawElementsInstancedBaseVertex(mode, count, restart.IndexType(), restart.Indices(),
                                                              instancecount, basevertex);
            }
        });
        SetCurrentBaseVertex(0);
        SetCurrentBaseInstance(0);
    }

    void DrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                         GLsizei instancecount, GLint basevertex) {
        if (!VertexArrayImpl::SyncClientSideVertexArraysForIndexedFetch(type, count, indices, instancecount, basevertex,
                                                                       0)) return;
        DrawSyncFlags syncBit = DrawSyncBit::IndexBuffer | DrawSyncBit::Instancing;
        PrepareForDraw(syncBit);
        const ScopedRestartIndexSubstitution restart(type, count, indices);
        if (!restart.DrawIsValid()) return;
        SetCurrentBaseVertex(basevertex);
        ForEachViewportRoutingPass([&] {
            g_GLESFuncs.glDrawElementsInstancedBaseVertex(mode, count, type, restart.Indices(), instancecount,
                                                          basevertex);
        });
        SetCurrentBaseVertex(0);
    }

    void DrawElementsInstancedBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                           GLsizei instancecount, GLuint baseinstance) {
        if (!VertexArrayImpl::SyncClientSideVertexArraysForIndexedFetch(type, count, indices, instancecount, 0,
                                                                       baseinstance)) return;
        DrawSyncFlags syncBit = DrawSyncBit::IndexBuffer | DrawSyncBit::Instancing;
        MGL_SCOPED_FETCH_BASE_INSTANCE(fetchScope, EmulatedFetchBaseInstance(baseinstance));
        PrepareForDraw(syncBit);
        const ScopedRestartIndexSubstitution restart(type, count, indices);
        if (!restart.DrawIsValid()) return;
        SetCurrentBaseInstance(baseinstance);
        ForEachViewportRoutingPass([&] {
            if (UseNativeBaseInstance()) {
                g_GLESFuncs.glDrawElementsInstancedBaseInstanceEXT(mode, count, restart.IndexType(), restart.Indices(),
                                                                  instancecount, baseinstance);
            } else {
                g_GLESFuncs.glDrawElementsInstanced(mode, count, restart.IndexType(), restart.Indices(), instancecount);
            }
        });
        SetCurrentBaseInstance(0);
    }

    void DrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instancecount) {
        if (!VertexArrayImpl::SyncClientSideVertexArraysForIndexedFetch(type, count, indices, instancecount, 0, 0))
            return;
        DrawSyncFlags syncBit = DrawSyncBit::IndexBuffer | DrawSyncBit::Instancing;
        PrepareForDraw(syncBit);
        const ScopedRestartIndexSubstitution restart(type, count, indices);
        if (!restart.DrawIsValid()) return;
        ForEachViewportRoutingPass([&] {
            g_GLESFuncs.glDrawElementsInstanced(mode, count, restart.IndexType(), restart.Indices(), instancecount);
        });
    }

    void DrawElementsIndirect(GLenum mode, GLenum type, const void* indirect) {
        DrawSyncFlags syncBit = DrawSyncBit::IndexBuffer | DrawSyncBit::IndirectBuffer | DrawSyncBit::Instancing;
        PrepareForDraw(syncBit);

        const SizeT indexSize = MG_Util::GetGLTypeSize(type);
        if (indexSize == 0) {
            MGLOG_E_ONCE("DrawElementsIndirect skipped: unsupported index type 0x%x", type);
            return;
        }

        const auto* commandBytes =
            ResolveIndirectCommandBytes(indirect, sizeof(DrawElementsIndirectCommand), "DrawElementsIndirect");
        if (!commandBytes) {
            return;
        }

        const auto& drawIndirectBuffer =
            // P5c (hd, CONTRACT-P5C §3.5): with an active transport the frontend binding slot
            // is never read - the Execute* helpers resolve the buffer from the verb record's
            // handle (T2). Monolith reads the slot as it always did.
            SharedPtr<MG_State::GLState::BufferObject>(nullptr);
        ExecuteIndexedIndirectCommands(mode, type, indexSize, commandBytes, reinterpret_cast<SizeT>(indirect),
                                       drawIndirectBuffer, 1, sizeof(DrawElementsIndirectCommand),
                                       "DrawElementsIndirect");
    }

    void DrawArraysInstancedBaseInstance(GLenum mode, GLint first, GLsizei count, GLsizei instancecount,
                                         GLuint baseinstance) {
        DrawSyncFlags syncBit = DrawSyncBit::Instancing;
        MGL_SCOPED_FETCH_BASE_INSTANCE(fetchScope, EmulatedFetchBaseInstance(baseinstance));
        PrepareForDraw(syncBit);
        SetCurrentBaseInstance(baseinstance);
        ForEachViewportRoutingPass([&] {
            if (UseNativeBaseInstance()) {
                g_GLESFuncs.glDrawArraysInstancedBaseInstanceEXT(mode, first, count, instancecount, baseinstance);
            } else {
                g_GLESFuncs.glDrawArraysInstanced(mode, first, count, instancecount);
            }
        });
        SetCurrentBaseInstance(0);
    }

    void DrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instancecount) {
        DrawSyncFlags syncBit = DrawSyncBit::Instancing;
        PrepareForDraw(syncBit);
        ForEachViewportRoutingPass([&] {
            g_GLESFuncs.glDrawArraysInstanced(mode, first, count, instancecount);
        });
    }

    void DrawArraysIndirect(GLenum mode, const void* indirect) {
        DrawSyncFlags syncBit = DrawSyncBit::IndirectBuffer | DrawSyncBit::Instancing;
        PrepareForDraw(syncBit);

        const auto* commandBytes =
            ResolveIndirectCommandBytes(indirect, sizeof(DrawArraysIndirectCommand), "DrawArraysIndirect");
        if (!commandBytes) {
            return;
        }

        const auto& drawIndirectBuffer =
            // P5c (hd, CONTRACT-P5C §3.5): with an active transport the frontend binding slot
            // is never read - the Execute* helpers resolve the buffer from the verb record's
            // handle (T2). Monolith reads the slot as it always did.
            SharedPtr<MG_State::GLState::BufferObject>(nullptr);
        ExecuteArraysIndirectCommands(mode, commandBytes, reinterpret_cast<SizeT>(indirect), drawIndirectBuffer, 1,
                                      sizeof(DrawArraysIndirectCommand), "DrawArraysIndirect");
    }

    // Empties the ES driver's error queue, BOUNDED. A driver that never answers GL_NO_ERROR - a
    // lost context is the usual way, and GL_CONTEXT_LOST is allowed to keep coming back - would
    // otherwise spin an unbounded drain forever inside whichever GL entry point happened to be
    // cleaning up, which is how a GPU reset reads as an unkillable process whose log simply
    // stops. A healthy context cannot queue anywhere near the cap, so reaching it IS the
    // diagnostic. Every drain in this backend goes through here so the bound cannot drift apart
    // between them.
    static constexpr Int kMaxDrainedGLErrors = 32;

    void DrainDriverErrors(const char* site) {
        Int drained = 0;
        while (drained < kMaxDrainedGLErrors && g_GLESFuncs.glGetError() != GL_NO_ERROR) {
            ++drained;
        }
        if (drained == kMaxDrainedGLErrors) {
            MGLOG_E_ONCE("%s: the ES driver still reported errors after %d drains - the context is most likely lost",
                         site, kMaxDrainedGLErrors);
        }
    }
} // namespace MobileGL::MG_Backend::DirectGLES
