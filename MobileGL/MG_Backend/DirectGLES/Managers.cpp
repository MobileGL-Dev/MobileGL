// MobileGL - MobileGL/MG_Backend/DirectGLES/Managers.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "ManagersInternal.h"

namespace MobileGL::MG_Backend::DirectGLES {
    Uint g_backendContextGeneration = 1;
    constexpr const char* BASE_INSTANCE_LOWERED_NAME = "mg_BaseInstanceLowered";
    constexpr const char* ZERO_BASED_INSTANCE_ID_NAME = "mg_ZeroBasedInstanceID";

    // See the block comment on ForEachViewportRoutingPass in Managers.h. Auto is ON, including on
    // a driver that advertises GL_OES_viewport_array: that extension gives the shader a name, not
    // the driver fifteen more rectangles to rasterize against, and nothing in MobileGL has ever
    // programmed the indexed state it would need.
    Bool ViewportArrayEmulationEnabled() {
        return MG_Config::Features.EsprytViewportArrayEmulation != MG_Config::QuirkOverride::ForceOff;
    }

    Bool g_anyProgramRoutesViewportIndex = false;

    GLenum ResolveBackendMinFilter(const SamplerParameters& samplerParams, Bool avoidMipmapMinFilter) {
        GLenum filter = MG_Util::ConvertSamplerFilterModeToGLEnum(samplerParams.minFilter,
                                                                  samplerParams.mipmapMode);
        if (!avoidMipmapMinFilter) {
            return filter;
        }
        switch (filter) {
        case GL_NEAREST_MIPMAP_NEAREST:
        case GL_NEAREST_MIPMAP_LINEAR:
            return GL_NEAREST;
        case GL_LINEAR_MIPMAP_NEAREST:
        case GL_LINEAR_MIPMAP_LINEAR:
            return GL_LINEAR;
        default:
            return filter;
        }
    }

    String ReplaceIdentifier(String source, const String& from, const String& to) {
        SizeT pos = 0;
        while ((pos = source.find(from, pos)) != String::npos) {
            const Bool leftIsIdent = pos > 0 &&
                (std::isalnum(static_cast<unsigned char>(source[pos - 1])) || source[pos - 1] == '_');
            const SizeT end = pos + from.size();
            const Bool rightIsIdent = end < source.size() &&
                (std::isalnum(static_cast<unsigned char>(source[end])) || source[end] == '_');
            if (!leftIsIdent && !rightIsIdent) {
                source.replace(pos, from.size(), to);
                pos += to.size();
            } else {
                pos = end;
            }
        }
        return source;
    }

    String InjectUniformAfterVersion(String source, const String& declaration) {
        const SizeT versionPos = source.find("#version");
        if (versionPos == String::npos) {
            return declaration + "\n" + source;
        }

        const SizeT lineEnd = source.find('\n', versionPos);
        if (lineEnd == String::npos) {
            return source + "\n" + declaration + "\n";
        }
        source.insert(lineEnd + 1, declaration + "\n");
        return source;
    }

        namespace {
        Bool g_processTeardown = false;
        std::once_flag g_teardownSentinelOnce;

#if MOBILEGL_BUILD_DISAGGREGATED
        // P12: raised only while DropEveryTwinForEndedServerSession empties the twin tables, on the
        // apply thread that is the only reader of the twins (Managers.h).
        Bool g_serverSessionTwinTeardown = false;
#endif
    } // namespace

#if MOBILEGL_BUILD_DISAGGREGATED
    Bool InProcessTeardown() { return g_processTeardown || g_serverSessionTwinTeardown; }
#else
    Bool InProcessTeardown() { return g_processTeardown; }
#endif
    Bool InProcessExit() { return g_processTeardown; }
    void EnsureProcessTeardownSentinel() {
        std::call_once(g_teardownSentinelOnce,
                       [] { std::atexit(+[] { g_processTeardown = true; }); });
    }

    namespace {
        // P2 step e2's dispatcher: the frontend told us an object died, so drop its twin NOW.
        // One entry point for all six kinds, because the answer is the same for all six - which
        // is why the notice carries the kind rather than there being six ops tables. The release
        // is by the handle the notice carries and reaches every holder of the kind (SlotTables.h,
        // the holder list); the client frees the slot afterwards.
        //
        // A notice that arrives after exit() has begun is dropped: past that point the twin's
        // destructor must not call into the driver (see InProcessTeardown()), and the process
        // is about to hand every GPU object back anyway. That twin is a deliberate leak.
        void OnFrontendStateObjectDestroyed(MG_Pipe::MGPipeKind kind, Uint64 lifetimeId,
                                            MG_Pipe::MGPipeHandle handle) {
            if (InProcessTeardown()) return;
#if MOBILEGL_BUILD_DISAGGREGATED
            // P5c (ct), CONTRACT-P5C.md §5.2: with an active transport the death crosses AS A
            // RECORD - object_death, the framebuffer family's first wire delete opcode. The GL
            // thread resolves the dying object's handle in its OWN allocator inside
            // EmitObjectDeathRecord: no handle means the server never saw the object and
            // NOTHING crosses (which replaces the mailbox's unconditional delivery), and a
            // handle means the record's EmitAndWait orders the death against in-flight verbs
            // that name it - the one property the blocking control-channel hop provided and
            // the only one P5c keeps. The sink (ServerVerbSink::OnObjectDeath) releases the
            // kind's twin table by handle on the apply thread, so neither a lifetime-id probe
            // nor the client's allocator is touched from there any more.
            if (MG_Config::Transport != MG_Config::TransportMode::Monolith &&
                !MG_Remote::Server::ServerLoop::OnApplyThread()) {
                // P5e (id), CONTRACT-P5E §4.4: THE RECORD IS THE ONLY DELIVERY. A server role
                // with no client session (a ServerLoop fixture) has no twin built from a record,
                // so it has nothing to release here; its twins die with the backend at Stop.
                (void)MG_Remote::Client::EmitObjectDeathRecord(kind, lifetimeId);
                return;
            }
            // Falling through HERE under a transport means the death was raised ON the apply
            // thread - the server backend destroying a frontend object it created itself.
#endif
            // P13: the twin is released BY HANDLE, the one the notice carries - the same
            // per-kind release the wire's object_death runs on the server
            // (ReleaseTwinsForWireObjectDeath below), so no arm resolves a lifetime id in the
            // allocator any more. The slot itself goes back in the caller (PipeFill.cpp's
            // NotifyAndFree), after this returns. A null handle is an object that never had a
            // slot of this kind, hence no twin.
            if (!MG_Pipe::MGPipeHandleIsNull(handle)) (void)ReleaseTwinsForWireObjectDeath(handle, kind);
        }

        const MG_State::GLState::StateObjectDeathOps g_glesStateObjectDeathOps = {
            .OnDestroyed = OnFrontendStateObjectDestroyed,
        };
    } // namespace

    // P5e (id), CONTRACT-P5E §4.2 / §4.3: THE TWIN DEATH MOMENT, AND THE ABA ANSWER.
    //
    // The moment: the apply of `object_death` / the kind's own delete opcode / `resource_destroy`
    // - in RING ORDER, i.e. after every verb that named the dying generation and before any
    // record that names the slot's next owner - or the server backend's own destruction at Stop
    // (InProcessTeardown). `applier_reset` touches neither twins nor object records
    // (PipeApply.h's MGPipeApplierReset), so a make-current kills nothing.
    //
    // The ABA answer, which is what makes "no wait is added at any delete" (§2.7) safe: Gen
    // moves ONLY on reuse (SlotAllocator.cpp), the client frees a slot only AFTER its death
    // record went out (PipeFill.cpp's NotifyAndFree), and the ring is in order. So at the
    // server every record naming {s, g} is applied before object_death{s, g}, which is applied
    // before create{s, g+1}. THE ORDERING IS THE ARGUMENT AND THE GEN COMPARE IS THE PROOF:
    //   * GetOrCreate({s, g+1}) on a live {s, g} entry RE-MINTS - it resets the twin, so the
    //     successor never inherits the dead object's driver ids (SlotTables.h);
    //   * FindByHandle({s, g}) after the recycle answers NULL, so a stale handle resolves to
    //     nothing rather than to its successor's twin;
    //   * a backward {s, g-1} is REFUSED rather than adopted, so a late record cannot destroy
    //     the incumbent.
    // Which means even a SKIPPED death (an object that never crossed emits none,
    // WireTables.cpp) is answered by the forward-Gen reset alone. Pinned by
    // DirectGLESSlotTable.AGenerationBehindTheLiveTwinIsRefusedRatherThanAdopted (all three
    // clauses, on the table itself) and end to end by the CtWireScenario recycle cases.
    Bool ReleaseTwinsForWireObjectDeath(MG_Pipe::MGPipeHandle handle, MG_Pipe::MGPipeKind kind) {
        // The handle-keyed twin of the notice switch above, and deliberately NOT that switch:
        // the record carried the handle, so no lifetime id is probed and the client's
        // allocator - a client-only surface under a transport (CONTRACT-P5C.md §3.1) - is
        // never touched from the apply thread. Each arm names its kind's registry GLOBAL for
        // the same spelling reason as the notice arms: ReleaseByHandle /
        // ReleaseTwinByHandle is static and is answered by every table of the kind that
        // exists, this global's own and any by-value copy a fixture or a context reset holds.
        switch (kind) {
        case MG_Pipe::MGPipeKind::Texture:
            return TextureImpl::g_backendTextureObjects.ReleaseByHandle(handle);
        case MG_Pipe::MGPipeKind::Framebuffer:
            return FramebufferImpl::g_backendFramebufferObjects.ReleaseByHandle(handle);
        case MG_Pipe::MGPipeKind::Renderbuffer:
            return RenderbufferImpl::g_backendRenderbufferObjects.ReleaseByHandle(handle);
        case MG_Pipe::MGPipeKind::SamplerCso:
            return SamplerImpl::g_backendSamplerObjects.ReleaseByHandle(handle);
        case MG_Pipe::MGPipeKind::ShaderCso:
            return PrgramImpl::g_backendProgramObjects.ReleaseByHandle(handle);
        case MG_Pipe::MGPipeKind::SamplerViewCso:
            // The notice arm's REDUNDANT SECOND PATH, keyed by the view's handle the same
            // way (CONTRACT-P5C.md §5.2): the client's delete_sampler_view may already have
            // released the twin, in which case the generation check inside fails and this
            // answers false. A false here is idempotency, never a leak - the view twin owns
            // no driver id of its own.
            return SamplerViewImpl::BackendSamplerViewTable::ReleaseTwinByHandle(handle);
        case MG_Pipe::MGPipeKind::VertexElementsCso:
            return VertexArrayImpl::g_backendVertexArrayObjects.ReleaseByHandle(handle);
        default:
            // Buffer's death crosses as resource_destroy (P3a), exactly as the notice
            // switch's default arm rules; every other kind has no twin table here.
            return false;
        }
    }

#if MOBILEGL_BUILD_DISAGGREGATED
    // P12: see Managers.h. Every table is replaced by an empty one, holders first (views before
    // the textures they view, framebuffers before their attachments, VAOs before the buffers they
    // fetch from), so the last reference to each twin goes here and its destructor - answered
    // InProcessTeardown() - touches neither the driver nor another TU's statics.
    //
    // P14 S6: "EVERY TABLE" IS NOW "EVERY BUCKET OF THIS SESSION". The tables are keyed by
    // {session, share group}, so the same rebuild of the eight registries would take a NEIGHBOUR
    // session's twins with it - and with S2's in-process supervisor serving several sessions at
    // once that is not a hypothetical: this runs from ~BackendObject_DirectGLES, on the apply
    // thread of the session that is ending. So the drop is scoped to the calling thread's
    // session when it has one, and is the process-wide drop (key {0, 0} and nothing else) when it
    // does not - which is the shape every unit case and the fork-worker world still has.
    void DropEveryTwinForEndedServerSession() {
        if (g_processTeardown) return;
        g_serverSessionTwinTeardown = true;
        TwinKey key;
        if (CurrentTwinKey(&key) && key.SessionKey != 0) {
            Uint32 buckets = 0;
            buckets += SamplerViewImpl::g_backendSamplerViews.DropBucketsOfSession(key.SessionKey);
            buckets += FramebufferImpl::g_backendFramebufferObjects.DropBucketsOfSession(key.SessionKey);
            buckets += VertexArrayImpl::g_backendVertexArrayObjects.DropBucketsOfSession(key.SessionKey);
            buckets += PrgramImpl::g_backendProgramObjects.DropBucketsOfSession(key.SessionKey);
            buckets += SamplerImpl::g_backendSamplerObjects.DropBucketsOfSession(key.SessionKey);
            buckets += TextureImpl::g_backendTextureObjects.DropBucketsOfSession(key.SessionKey);
            buckets += RenderbufferImpl::g_backendRenderbufferObjects.DropBucketsOfSession(key.SessionKey);
            buckets += BufferImpl::g_backendBufferResources.DropBucketsOfSession(key.SessionKey);
            MGLOG_I("DirectGLES: the ended server session dropped %u twin group(s) across eight kinds "
                    "(no driver call); the other sessions in this process keep theirs",
                    buckets);
        } else {
            SamplerViewImpl::g_backendSamplerViews.ResetBuckets();
            FramebufferImpl::g_backendFramebufferObjects.ResetBuckets();
            VertexArrayImpl::g_backendVertexArrayObjects.ResetBuckets();
            PrgramImpl::g_backendProgramObjects.ResetBuckets();
            SamplerImpl::g_backendSamplerObjects.ResetBuckets();
            TextureImpl::g_backendTextureObjects.ResetBuckets();
            RenderbufferImpl::g_backendRenderbufferObjects.ResetBuckets();
            BufferImpl::g_backendBufferResources.ResetBuckets();
        }
        // P12 review fix (major): AND THE UNIT SHADOWS THAT POINT AT THEM. A texture or sampler twin
        // scrubs itself out of g_boundTexturesCache / g_boundSamplersCache in its destructor - but
        // not under InProcessTeardown(), which answered true for every twin just dropped. Left
        // behind, those raw pointers name freed twins; the next session's twin allocated at the same
        // address then reads "already bound" and skips its glBindTexture / glBindSampler on a
        // context where nothing is bound, so its uploads land on texture 0 and it samples black.
        // The new context's active unit is GL_TEXTURE0, so the active-unit shadow goes back to 0 too.
        TextureImpl::g_boundTexturesCache = {};
        TextureImpl::g_activeTextureUnit = 0;
        SamplerImpl::g_boundSamplersCache = {};
        g_serverSessionTwinTeardown = false;
    }
#endif

    // The one sentence that decides the arm, written once so that a test can drive every
    // combination of the two knobs and so that bring-up and first-use cannot disagree.
    EsprytSlotArmVerdict ClassifyEsprytSlotArm(Bool subsystemBitSet, Bool legacyMemosEnabled) {
        // The legacy arm is not compiled, so the handle arm is the only arm and neither knob
        // can produce an armless configuration.
        (void)subsystemBitSet;
        (void)legacyMemosEnabled;
        return EsprytSlotArmVerdict::Handles;
    }

    EsprytSlotArmVerdict CurrentEsprytSlotArmVerdict() {
        return ClassifyEsprytSlotArm(
            (MG_Config::Features.PipePush & MG_Pipe::kMGPipeSubsystemEsprytSlots) != 0,
            /*legacyMemosEnabled=*/true);
    }

    void DiagnoseEsprytSlotArm() {
        if (CurrentEsprytSlotArmVerdict() != EsprytSlotArmVerdict::NoArm) return;
        // Loud, named, and NOT a stop - see the comment on this function in SlotTables.h for
        // why a stop raised from inside EGL bring-up is swallowed into a skipped lane.
        MGLOG_E("MGPipe: PipeLegacyMemosDisabled - MOBILEGL_PIPE_PUSH leaves "
                "kMGPipeSubsystemEsprytSlots (bit 5) clear and MOBILEGL_PIPE_LEGACY_MEMOS=0 "
                "makes the legacy twin registry unreachable, so this context has no twin table "
                "arm at all; the first twin lookup will stop the process");
    }

    Bool ResolveEsprytSlotTablesArm() {
        // Resolved once and latched by the inline EsprytSlotTablesEnabled() in SlotTables.h:
        // the two arms of StateBackendObjectRegistry keep their twins in different containers,
        // so an answer that changed mid-run would strand every twin already built (and, for
        // the driver ids those twins own, leak them).
        //
        // This runs at the FIRST TWIN LOOKUP, not at backend bring-up. That is deliberate and
        // it is the fix for a lane that went green by skipping: bring-up runs inside
        // eglMakeCurrent, which the integration harness pre-flights in a forked child, and a
        // child that aborts is reported as "no usable GPU" and skips every scenario. Here the
        // stop lands in the caller of the twin lookup - a scenario body, a sync path, a test -
        // where ctest reports it as a failure. A process that never looks a twin up never needs
        // an arm and is never stopped by this.
        const EsprytSlotArmVerdict verdict = CurrentEsprytSlotArmVerdict();
        if (verdict == EsprytSlotArmVerdict::NoArm) {
            // The operator asked for the handle arm to be OFF and the legacy arm to be
            // unreachable at the same time, which leaves no arm at all. This is a Fatal{},
            // and a Fatal{} in this codebase STOPS (MG_Impl/Pipe/PipeFill.cpp's BadKnob and
            // its verify trap are both MGLOG_F + abort). Returning here instead would run
            // the very arm the operator disabled and hand back a green result measured on
            // it - which is exactly the lever HandleRecycleScenario's arms are selected
            // with, so a mis-set A/B would be scored silently against the wrong arm
            // (ARCHITECTURE.md 9.6).
            MGLOG_F("MGPipe: Fatal{PipeLegacyMemosDisabled, \"MOBILEGL_PIPE_PUSH leaves "
                    "kMGPipeSubsystemEsprytSlots (bit 5) clear and MOBILEGL_PIPE_LEGACY_MEMOS=0 "
                    "makes the legacy twin registry unreachable, so there is no twin table arm "
                    "to run\"}");
            std::abort();
        }
        if (verdict == EsprytSlotArmVerdict::Legacy) {
            return false;
        }
        if ((MG_Config::Features.PipePush & MG_Pipe::kMGPipeSubsystemEsprytSlots) == 0) {
            // The bit is clear but this build has no legacy twin registry to fall back to, so
            // the bit decides nothing. Recorded so a log reader sees the mismatch.
            MGLOG_D("MGPipe: kMGPipeSubsystemEsprytSlots is clear but this build has no "
                    "legacy twin registry; running the handle arm anyway");
        }
        // The notice is only consumable on the handle arm (the legacy registry keys on the
        // frontend ADDRESS, which is gone by the time a destructor speaks), so it is installed
        // exactly where it can be answered. Once per process, cold.
        MG_State::GLState::SetStateObjectDeathOps(&g_glesStateObjectDeathOps);
        return true;
    }

    Bool VertexStageStorageBlockUsable(Int maxVertexShaderStorageBlocks) {
        // One block is all the indirect-params view needs, so this is a >= 1 test and not a
        // budget calculation. Negative is treated as unusable rather than clamped: a driver
        // that leaves the out-param untouched is telling us nothing, and guessing "yes" here
        // is what produces an unlinkable program.
        return maxVertexShaderStorageBlocks >= 1;
    }

    static Bool CanUseVertexStageStorageBlock() {
        return VertexStageStorageBlockUsable(g_GLESCapabilities.MaxVertexShaderStorageBlocks);
    }

    String EmulateBaseInstanceInVertexShader(String source, GLenum shaderType) {
        if (shaderType != GL_VERTEX_SHADER || source.find("gl_BaseInstance") == String::npos) {
            return source;
        }
        String replaced = ReplaceIdentifier(source, "gl_BaseInstance", BASE_INSTANCE_UNIFORM_NAME);
        if (replaced == source) {
            // Only a substring hit (e.g. gl_BaseInstanceARB inside a SPIRV-Cross #ifdef
            // fallback); nothing was rewritten, so nothing must be declared either.
            return source;
        }
        return InjectUniformAfterVersion(std::move(replaced),
                                         String("uniform highp int ") + BASE_INSTANCE_UNIFORM_NAME + ";");
    }

    // The LowerDrawParametersPass demotes gl_DrawID / gl_BaseInstance / gl_BaseVertex to plain
    // Private globals (mg_DrawID / mg_BaseInstanceLowered / mg_BaseVertex); SPIRV-Cross then
    // emits them as ordinary global declarations. mg_DrawID / mg_BaseVertex become uniforms fed
    // per (sub-)draw. gl_BaseInstance is special: for indirect draws its value lives in the
    // (possibly GPU-written) indirect command buffer, so its declaration expands into a
    // std430 SSBO view of that buffer indexed by a CPU-computed word index, with the plain
    // mg_BaseInstance uniform as the fallback for non-indirect draws.
    //
    // The word index is stored ONE-BASED, so that zero - the value every GLSL uniform starts
    // at - is the "not an indirect draw" sentinel. Nothing seeds this uniform before a
    // program's first draw, and the non-indirect draw entry points never write it at all, so a
    // zero-based index with a negative sentinel would leave every such draw reading
    // mg_indirectWords[0] out of a storage buffer no one bound. That is not a silent zero on a
    // real driver: it returned garbage on Adreno, and a garbage gl_BaseInstance pushed the CTS
    // shader_draw_parameters geometry clean off screen.
    String PromoteDrawParameterGlobalsToUniforms(String source, GLenum shaderType) {
        if (shaderType != GL_VERTEX_SHADER) {
            return source;
        }
        for (const char* name : {DRAW_ID_UNIFORM_NAME, BASE_VERTEX_UNIFORM_NAME}) {
            for (const char* declPrefix : {"highp int ", "mediump int ", "lowp int ", "int ", "highp uint ",
                                           "mediump uint ", "uint "}) {
                const String declaration = String(declPrefix) + name + ";";
                const SizeT pos = source.find(declaration);
                if (pos == String::npos) {
                    continue;
                }
                // Only promote a standalone global declaration, not a uniform we already emitted.
                const Bool alreadyUniform = pos >= 8 && source.compare(pos - 8, 8, "uniform ") == 0;
                if (!alreadyUniform) {
                    const Bool hasPrecision = std::strncmp(declPrefix, "int ", 4) != 0 &&
                                              std::strncmp(declPrefix, "uint ", 5) != 0;
                    const String qualifier = hasPrecision ? "uniform " : "uniform highp ";
                    source.replace(pos, declaration.size(), qualifier + declaration);
                }
                break;
            }
        }
        for (const char* declPrefix : {"highp int ", "mediump int ", "lowp int ", "int "}) {
            const String declaration = String(declPrefix) + BASE_INSTANCE_LOWERED_NAME + ";";
            SizeT pos = source.find(declaration);
            if (pos == String::npos) {
                continue;
            }
            // On drivers where native indirect draws leak the command's baseInstance into
            // gl_InstanceID (ANGLE-on-Vulkan; IndirectDrawInstanceIdIncludesBaseInstance),
            // rebase gl_InstanceID back to zero during those draws so shaders computing
            // gl_BaseInstance + gl_InstanceID don't add the base twice. Scoped to shaders
            // using gl_BaseInstance: only they take the native indirect SSBO machinery.
            const Bool rebaseInstanceId = g_GLESCapabilities.IndirectDrawInstanceIdIncludesBaseInstance &&
                                          source.find("gl_InstanceID") != String::npos;
            if (rebaseInstanceId) {
                source = ReplaceIdentifier(source, "gl_InstanceID", ZERO_BASED_INSTANCE_ID_NAME);
                pos = source.find(declaration); // the declaration contains no gl_InstanceID
            }
            const Int paramsBinding = g_GLESCapabilities.MaxShaderStorageBufferBindings > 0
                                          ? g_GLESCapabilities.MaxShaderStorageBufferBindings - 1
                                          : 0;
            // The whole indirect half of this machinery is a storage block read from the VERTEX
            // stage, and a storage block in the vertex stage is optional in both APIs: the
            // minimum for GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS is 0 (GL 4.6 table 23.64, ES 3.2
            // table 21.44) and ARM's GLES driver takes that allowance - a Mali-G925 reports 0.
            // Emitting the block anyway does not make it work; it makes the program UNLINKABLE
            // ("The number of vertex shader storage blocks (1) is greater than the maximum
            // number allowed (0)"), and because the frontend's LINK_STATUS is glslang's and not
            // the driver's, the application never learns: every draw with that program silently
            // renders nothing. Dropping just the indirect half costs strictly less.
            const Bool canReadIndirectParamsFromVertexStage = CanUseVertexStageStorageBlock();
            String machinery;
            if (source.find(String("uniform highp int ") + BASE_INSTANCE_UNIFORM_NAME + ";") == String::npos) {
                machinery += String("uniform highp int ") + BASE_INSTANCE_UNIFORM_NAME + ";\n";
            }
            if (!canReadIndirectParamsFromVertexStage) {
                // Degraded, but contained and loud. gl_BaseInstance collapses to the plain
                // mg_BaseInstance uniform, which the non-indirect draw entry points do set
                // correctly - so ordinary instanced draws are unaffected. What is lost is the
                // per-command baseInstance of an INDIRECT draw, which lives in the (possibly
                // GPU-written) command buffer and can only be read through this block: those
                // draws now see the last uniform value rather than their own command's. No
                // alternative path is attempted, deliberately - there is nowhere else in the
                // vertex stage to read a GPU-written buffer from.
                //
                // MGLOG_E_ONCE, not _D: this silently changes rendering for exactly the
                // workloads (Create/Flywheel indirect instancing) whose bug reports are
                // impossible to read without it, and once per process is bounded.
                MGLOG_E_ONCE("gl_BaseInstance: this driver reports GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS = %d, so the "
                             "%s storage block an indirect draw's baseInstance must be read through cannot be "
                             "declared in the vertex stage. Dropping indirect baseInstance support: non-indirect "
                             "draws are correct, indirect draws will see a stale per-command baseInstance.",
                             g_GLESCapabilities.MaxVertexShaderStorageBlocks, INDIRECT_PARAMS_BLOCK_NAME);
                if (rebaseInstanceId) {
                    // Without the block there is no per-command baseInstance to subtract, and
                    // the uniform is the same value the define below resolves to, so rebasing
                    // by it would cancel the base out of gl_InstanceID twice.
                    machinery += String("#define ") + ZERO_BASED_INSTANCE_ID_NAME + " gl_InstanceID\n";
                }
                machinery += String("#define ") + BASE_INSTANCE_LOWERED_NAME + " (" + BASE_INSTANCE_UNIFORM_NAME + ")";
                source.replace(pos, declaration.size(), machinery);
                break;
            }
            machinery += String("uniform highp int ") + BASE_INSTANCE_WORD_INDEX_UNIFORM_NAME + ";\n";
            machinery += String("layout(std430, binding = ") + std::to_string(paramsBinding) +
                         ") readonly buffer " + INDIRECT_PARAMS_BLOCK_NAME +
                         " { highp uint mg_indirectWords[]; };\n";
            if (rebaseInstanceId) {
                machinery += String("#define ") + ZERO_BASED_INSTANCE_ID_NAME + " (gl_InstanceID - ((" +
                             BASE_INSTANCE_WORD_INDEX_UNIFORM_NAME + " > 0) ? int(mg_indirectWords[uint(" +
                             BASE_INSTANCE_WORD_INDEX_UNIFORM_NAME + " - 1)]) : 0))\n";
            }
            machinery += String("#define ") + BASE_INSTANCE_LOWERED_NAME + " ((" +
                         BASE_INSTANCE_WORD_INDEX_UNIFORM_NAME + " > 0) ? int(mg_indirectWords[uint(" +
                         BASE_INSTANCE_WORD_INDEX_UNIFORM_NAME + " - 1)]) : " + BASE_INSTANCE_UNIFORM_NAME + ")";
            source.replace(pos, declaration.size(), machinery);
            break;
        }
        return source;
    }

    // ---- gl_ViewportIndex routing emulation, ESSL half ---------------------------------------
    //
    // LowerViewportIndexPass has already turned the BuiltIn ViewportIndex OUTPUT into a plain
    // Private global, so SPIRV-Cross printed `int mg_ViewportIndex;` at file scope and the stage
    // still stores the index the application asked for - it just goes nowhere. The two passes
    // below give it somewhere to go WITHOUT naming a builtin the language does not have: the
    // producing stage's global becomes an ordinary flat varying, and the fragment stage gets a
    // gate that discards every fragment whose primitive routed to a viewport the current replay
    // pass is not drawing. DirectGLES.cpp's ForEachViewportRoutingPass is the other half - it
    // re-issues the draw once per distinct viewport state with the real
    // glViewport/glScissor/glDepthRangef pushed for it and this uniform set to the set of
    // indices that state serves.
    //
    // FLAT is semantics, not performance: GL takes a primitive's viewport index from its
    // PROVOKING VERTEX, which is exactly what flat interpolation delivers, so a primitive whose
    // vertices carry different indices routes the way the spec says with no extra machinery.
    //
    // NO layout(location = N) on either side, deliberately. The two stages are transpiled
    // independently and neither can see the other's location assignment: the producing stage
    // knows its own outputs, the fragment stage only the subset it consumes, and a number derived
    // from either can disagree with the other. Leaving both unqualified hands the assignment to
    // the driver's linker, which then matches them BY NAME - the ordinary GLSL rule, and the only
    // one that needs no cross-stage channel. The cost is one varying slot, which a program
    // already at GL_MAX_VARYING_VECTORS cannot spare.
    constexpr const char* VIEWPORT_INDEX_VARYING_NAME = "mg_ViewportIndex";
    constexpr const char* VIEWPORT_GATED_ENTRY_POINT_NAME = "mg_ViewportGatedMain";
    constexpr const char* ESSL_ENTRY_POINT_SIGNATURE = "void main()";
    static_assert(RenderStateParameters::MAX_VIEWPORTS == 16,
                  "the fragment gate below spells the index clamp as `& 15` and the pass mask as a "
                  "16-bit int; both follow MAX_VIEWPORTS and have to be respelled with it");

    // Producing stage (vertex / tessellation evaluation / geometry - the three GL lets write the
    // builtin). Returns whether the demoted global was found and promoted, which is also the
    // answer to "does this program route viewports at all".
    Bool PromoteViewportIndexGlobalToVarying(String& source) {
        // The same shape PromoteDrawParameterGlobalsToUniforms matches, and for the same reason:
        // SPIRV-Cross prints the demoted global with or without a precision qualifier depending
        // on what the module carried. Only a declaration that starts its own line may be
        // rewritten - `mg_ViewportIndex = gl_InvocationID;` in the body contains the name too and
        // has to be left exactly as it is.
        const String declared = String(VIEWPORT_INDEX_VARYING_NAME) + ";";
        for (const char* declPrefix : {"highp int ", "mediump int ", "lowp int ", "int "}) {
            const String declaration = String(declPrefix) + declared;
            const SizeT pos = source.find(declaration);
            if (pos == String::npos) {
                continue;
            }
            // Column 0 of its own line is what separates the declaration from the tail of any
            // other declaration or expression that ends in the same name.
            if (pos != 0 && source[pos - 1] != '\n') {
                continue;
            }
            source.replace(pos, declaration.size(),
                           String("flat out highp int ") + VIEWPORT_INDEX_VARYING_NAME + ";");
            return true;
        }
        return false;
    }

    // Fragment stage. Returns false when the stage has no entry point to gate onto, which the
    // caller reports: the program still links and still renders, it just renders every index
    // with the first replay pass's state - i.e. it degrades to the pre-emulation behaviour
    // rather than to a black screen.
    Bool InjectViewportIndexPassGate(String& source) {
        // Built beside the input and swapped in only on success, so a stage this pass declines
        // reaches the driver exactly as it arrived rather than half-rewritten.
        // A fragment stage that READS gl_ViewportIndex has no ESSL spelling for it either -
        // LowerViewportIndexPass deliberately demotes only OUTPUTS, because a demoted INPUT would
        // answer from an undefined Private global. Now that the routing varying exists and
        // carries the real per-primitive value, that read has somewhere honest to go.
        String gated = ReplaceIdentifier(source, "gl_ViewportIndex", VIEWPORT_INDEX_VARYING_NAME);

        const SizeT entryPos = gated.find(ESSL_ENTRY_POINT_SIGNATURE);
        if (entryPos == String::npos) {
            return false;
        }

        // Declarations go immediately before the entry point rather than after #version: that
        // position is already past every #extension directive (which must precede any other
        // token) and past everything the body can name, so it can invalidate neither.
        //
        // Renaming the entry point rather than splicing a prologue into its body keeps the
        // application's code byte-identical, including an early `return`.
        String preamble = String("flat in highp int ") + VIEWPORT_INDEX_VARYING_NAME + ";\n";
        preamble += String("uniform highp int ") + VIEWPORT_PASS_MASK_UNIFORM_NAME + ";\n";
        preamble += String("void ") + VIEWPORT_GATED_ENTRY_POINT_NAME + "()";
        gated.replace(entryPos, std::strlen(ESSL_ENTRY_POINT_SIGNATURE), preamble);

        // `& 15` clamps the shift operand into range for MAX_VIEWPORTS = 16. GL leaves an index
        // outside [0, MAX_VIEWPORTS) undefined, but an ESSL shift by >= 32 is undefined in a way
        // that can take the whole draw with it, so the emulation picks a defined answer instead.
        //
        // The mask, not an equality test against a pass number: viewport indices whose whole
        // state tuple is identical share ONE replay pass (see BeginViewportRoutingPasses), and
        // the overwhelmingly common case - every index still holding what glViewport broadcast -
        // is then a single pass with every bit set, i.e. a gate that discards nothing and a draw
        // that is issued exactly once.
        //
        // PERFORMANCE NOTE: a fragment shader containing `discard` cannot take the early-Z fast
        // path on a tiler, so a routed draw pays late-Z on top of its N replay passes. Accepted
        // deliberately: this runs only for a program that writes gl_ViewportIndex, and that is
        // why the gate is injected per program rather than into every fragment shader.
        gated += "\n";
        gated += String(ESSL_ENTRY_POINT_SIGNATURE) + "\n";
        gated += "{\n";
        gated += String("    if (((") + VIEWPORT_PASS_MASK_UNIFORM_NAME + " >> (" +
                 VIEWPORT_INDEX_VARYING_NAME + " & 15)) & 1) == 0)\n";
        gated += "    {\n";
        gated += "        discard;\n";
        gated += "    }\n";
        gated += "    else\n";
        gated += "    {\n";
        gated += String("        ") + VIEWPORT_GATED_ENTRY_POINT_NAME + "();\n";
        gated += "    }\n";
        gated += "}\n";
        source = std::move(gated);
        return true;
    }

    namespace BufferImpl {
        // The two P3a families get the three-way treatment ClassifyEsprytSlotArm established for
        // bit 5, rather than reading their bit and stopping there: "the bit is clear AND the
        // legacy arm has been taken away" is a real configuration an operator can ask for, and a
        // family that only reads its own bit answers it by running the very arm that was
        // disabled, in silence. The two families differ in whether that third verdict is
        // REACHABLE, which is why the classifier takes it as a parameter rather than assuming it.
        //
        // A STOP, exactly like ResolveEsprytSlotTablesArm's NoArm, and M-4 is the promotion.
        //
        // It was a warning while the premise for the warning held: bits 7 and 8 were clear in
        // every `.Handles` itest lane, which pinned MOBILEGL_PIPE_PUSH=0x7f beside
        // MOBILEGL_PIPE_LEGACY_MEMOS=0, so aborting would have turned that lane red for a
        // mis-pinned control rather than for a defect. That premise is gone: gates' M1 re-pinned
        // MGL_ITEST_HANDLES_ARM_KNOBS to "MOBILEGL_PIPE_LEGACY_MEMOS=0" + "MOBILEGL_PIPE_PUSH=0x1ff"
        // (MG_IntegrationTest/CMakeLists.txt), and the four CSO lanes went to 0x1ff /
        // 0x800000000000_01ff, which is contract-review item 11 closed for all three lane
        // families. The only lanes that set LEGACY_MEMOS=0 now carry an explicit vertex-input
        // arm, so an armless verdict can no longer be a lane's own configuration - it can only
        // be an operator who asked for both arms to be gone.
        //
        // Warning-and-continue is the wrong answer to that, for ResolveEsprytSlotTablesArm's
        // reason verbatim: continuing runs the very arm the operator disabled and hands back a
        // result measured on it, which is exactly the lever HandleRecycleScenario's arms are
        // selected with, so a mis-set A/B would be scored silently against the wrong arm
        // (ARCHITECTURE.md 9.6). Resolved lazily at the first use, not at bring-up, so the stop
        // lands in a scenario body where ctest reports it rather than inside eglMakeCurrent
        // where the harness reads an abort as "no usable GPU" and skips.
        //
        // This closes espryt D14 / closure row 7 and, with it, row 6's "LEGACY_MEMOS=0 +
        // PUSH=0x7f must name a verdict": that combination now names NoArm and stops.
        enum class PipeSubsystemArmVerdict { Handles, Legacy, NoArm };

        // The one voice for an armless verdict, so the two families cannot disagree about what
        // it means or about whether it stops. `what` names the family and the bit.
        [[noreturn]] void StopOnArmlessPipeSubsystem(const char* what) {
            MGLOG_F("MGPipe: Fatal{PipeLegacyMemosDisabled, \"%s, so there is no arm to run\"}", what);
            std::abort();
        }

        PipeSubsystemArmVerdict ClassifyPipeSubsystemArm(Bool subsystemBitSet, Bool legacyMemosEnabled,
                                                         Bool legacyArmSurvivesLegacyMemos) {
            if (subsystemBitSet) return PipeSubsystemArmVerdict::Handles;
            if (legacyArmSurvivesLegacyMemos || legacyMemosEnabled) return PipeSubsystemArmVerdict::Legacy;
            return PipeSubsystemArmVerdict::NoArm;
        }
    } // namespace BufferImpl

    // ---- P4a (D-K3): the four family arm resolvers, beside BufferImpl's two ----
    //
    // They reuse BufferImpl's ClassifyPipeSubsystemArm / StopOnArmlessPipeSubsystem unchanged:
    // one voice for "the operator left no arm at all", so the six families cannot disagree
    // about what an armless verdict means or about whether it stops.
    //
    // Every one of the four answers legacyArmSurvivesLegacyMemos = FALSE, because every one of
    // the four pre-handle arms is compiled under MOBILEGL_PIPE_LEGACY_MEMOS (Managers.h names
    // them per family). So all four can reach NoArm and all four stop with the named
    // Fatal{PipeLegacyMemosDisabled, ...} rather than running the very arm the operator asked
    // to have taken away and handing back a result measured on it.

    namespace {
        // Shared by the dependent families, so the refusal reads the same way every time and a
        // future family cannot invent a different wording. Returns true when the dependency is
        // missing, having said so once.
        //
        // P3b/P4b wave 2-D package D3: THE ROWS ARE READ FROM MG_Pipe/SubsystemDeps.def AND ARE
        // NO LONGER RESTATED HERE. This function used to take a single dependency BIT written out
        // at each call site - the server's own copy of the rule, one of the six statements R-5
        // found and two of which had drifted - and a family with two required bits needed two
        // guarded calls with two hand-written sentences. It now takes the FAMILY and asks
        // MGPipeSubsystemDependenciesAreSet, so the set of required bits, the subset that is
        // actually missing and the reason all come from the table; `what` is only the family's
        // own name. The client's gate reads the same rows
        // (MG_Impl/Pipe/PipeFill.cpp P4aFamilyDependencyBits), which is what makes
        // MG_Test/Backend/DirectGLES/SubsystemDepsTest.cpp's comparison of the two a comparison
        // of two readers rather than of two copies.
        Bool PipeSubsystemDependencyMissing(Uint64 mask, Uint64 family, const char* what) {
            if (MG_Pipe::MGPipeSubsystemDependenciesAreSet(family, mask)) return false;
            const Uint64 required = MG_Pipe::MGPipeSubsystemRequires(family);
            MGLOG_E("MGPipe: %s is set but MOBILEGL_PIPE_PUSH=0x%llx does not carry every bit "
                    "MG_Pipe/SubsystemDeps.def says it requires (requires 0x%llx, missing 0x%llx): "
                    "%s - REFUSING the dependent bit and running the legacy arm. Set every bit of "
                    "the row, or clear the family's own",
                    what, static_cast<unsigned long long>(mask),
                    static_cast<unsigned long long>(required),
                    static_cast<unsigned long long>(required & ~mask),
                    MG_Pipe::MGPipeSubsystemDependencyWhy(family));
            return true;
        }

        // THE SAFETY ARGUMENT P4a MADE AND NEVER RAN (P3b/P4b espryt D1 slice 5).
        //
        // "Running the legacy arm" is a real answer in a monolith and a POSTPONED CRASH under a
        // transport. All four pre-handle arms reach frontend state through accessors that are
        // BARRIER_PULLED rows (FieldOwnership.def) - GetFramebufferBindingSlot is the loudest of
        // them - and under an active transport that state belongs to a client that is no longer
        // parked behind this apply. The accessor's honest answer is
        // Fatal{UnmigratedPipeInput, "<field>@<verb>"}, raised on the first verb that reaches it.
        //
        // So the verdict "Legacy" under a transport is not safe, it is merely quiet. The audit
        // that called g_fboTextureSyncList "separation-safe because the record arm returns first"
        // was right only while the record arm is ARMED, and nothing checked that.
        //
        // A FRAME-TIME FATAL IS THE WORST SPELLING OF "MOBILEGL_PIPE_PUSH LEFT BIT 9 CLEAR": it
        // arrives after a session is up, inside a scenario body, naming an accessor and a verb
        // rather than the mask that chose them. Stop at resolve time instead - once, before the
        // first verb - where both causes are still in scope and can be said in one sentence.
        //
        // THROUGH StopOnArmlessPipeSubsystem AND NOT A SECOND FATAL, deliberately: it is this
        // file's one voice for "the configuration left no arm to run", the census already knows
        // its word, and a transport with no record arm IS that condition - the legacy arm is
        // present but unreachable, which is the same "no arm" with a different cause. The cause
        // is what `detail` carries.
        [[noreturn]] void StopOnTransportWithoutRecordArm(const char* bit, const char* detail) {
            char what[512];
            std::snprintf(what, sizeof(what),
                          "MOBILEGL_TRANSPORT is not monolith and %s is clear (or refused), so the "
                          "record arm is off and the legacy arm is unreachable: %s",
                          bit, detail);
            BufferImpl::StopOnArmlessPipeSubsystem(what);
        }
    } // namespace

    Bool ResolveFramebufferSubsystemArm() {
        const Uint64 mask = MG_Config::Features.PipePush;
        const Bool bitSet = (mask & MG_Pipe::kMGPipeSubsystemFramebuffer) != 0;
        Bool refused = false;
        if (bitSet) {
            // The framebuffer family's row, from MG_Pipe/SubsystemDeps.def. The mirror pair (bit
            // 10 set, bit 9 clear) is FINE - the legacy FBO sync reaches the texture twin through
            // SyncTextureObjectToBackend, which dispatches to the handle arm by itself - and that
            // is stated with the other mirror pairs in MGPipe.h, beside the table it is the
            // complement of, rather than here.
            refused = PipeSubsystemDependencyMissing(mask, MG_Pipe::kMGPipeSubsystemFramebuffer,
                                                     "kMGPipeSubsystemFramebuffer (bit 9)");
            // D-C3: the wire array is Color[8] and this is the driver's RAW ES cap, which is
            // NOT clamped to 8 on the GLES path (ValidateColorAttachmentInRange rejects at or
            // above it, and BackendObject_DirectGLES publishes it verbatim). Every campaign
            // device reports 8 and ES 3.2's minimum is 8 - but a driver reporting more would
            // make the record silently truncate, and truncating silently is the bug class this
            // phase is closing, while widening the payload is a wire change nobody has evidence
            // for. So: refuse the bit, name the cap, run the legacy arm.
            if (!refused &&
                static_cast<Uint32>(std::max<Int>(g_GLESCapabilities.MaxColorAttachments, 0)) >
                    MG_Pipe::kMGPipeMaxColorAttachments) {
                MGLOG_E("MGPipe: this driver reports GL_MAX_COLOR_ATTACHMENTS = %d, above "
                        "MGPFramebufferState::Color[%u]'s wire width - REFUSING "
                        "kMGPipeSubsystemFramebuffer (bit 9) rather than truncating the record, "
                        "and running the legacy framebuffer arm",
                        g_GLESCapabilities.MaxColorAttachments, MG_Pipe::kMGPipeMaxColorAttachments);
                refused = true;
            }
        }
        const BufferImpl::PipeSubsystemArmVerdict verdict = BufferImpl::ClassifyPipeSubsystemArm(
            bitSet && !refused, /*legacyMemosEnabled=*/true,
            /*legacyArmSurvivesLegacyMemos=*/false);
        if (verdict == BufferImpl::PipeSubsystemArmVerdict::NoArm) {
            BufferImpl::StopOnArmlessPipeSubsystem(
                "MOBILEGL_PIPE_PUSH leaves kMGPipeSubsystemFramebuffer (bit 9) clear (or refuses "
                "it) and MOBILEGL_PIPE_LEGACY_MEMOS=0 disables the pre-handle g_fboSynced* arm");
        }
        if (verdict != BufferImpl::PipeSubsystemArmVerdict::Handles) {
            StopOnTransportWithoutRecordArm(
                "kMGPipeSubsystemFramebuffer (bit 9)",
                "the pre-handle g_fboSynced* / g_fboTextureSyncList arm reads the framebuffer "
                "binding slots, and under a transport GetFramebufferBindingSlot is a BARRIER_PULLED "
                "row belonging to a client that is not parked behind this apply - so the first verb "
                "to reach it would raise Fatal{UnmigratedPipeInput, "
                "\"GetFramebufferBindingSlot@<verb>\"} mid-frame instead");
        }
        const Bool enabled = verdict == BufferImpl::PipeSubsystemArmVerdict::Handles;
        MGLOG_D("MGPipe: Espryt framebuffer family runs the %s arm", enabled ? "handle" : "legacy");
        return enabled;
    }

    Bool ResolveTextureResourceSubsystemArm() {
        const Uint64 mask = MG_Config::Features.PipePush;
        const Bool bitSet = (mask & MG_Pipe::kMGPipeSubsystemTextureResources) != 0;
        Bool refused = false;
        if (bitSet) {
            // The texture family's row, from MG_Pipe/SubsystemDeps.def. It is the ONLY row with
            // two required bits, and it used to be TWO guarded calls here with the pair of
            // sentences written out by hand - bit 7 for a buffer texture's BufferForTexBuffer,
            // and bit 11, D-K2's FOURTH ROW (ID-14/ID-15), for MGPTextureParams::BuiltinSampler.
            // One call now asks the table for both, and the refusal names whichever of the two is
            // actually missing instead of always naming the first. Package F pins this arm at
            // 0x5ff and CMakeLists.txt's refusal lane at 0x7ff; both are rows of the table now.
            //
            // The mirror pairs live in MGPipe.h with the rest. The one worth remembering at this
            // call site is that bits 10 and 11 are SYMMETRIC - each row names the other - so the
            // only two masks that reach the texture handle arm are "both set" and "neither set".
            refused = PipeSubsystemDependencyMissing(mask, MG_Pipe::kMGPipeSubsystemTextureResources,
                                                     "kMGPipeSubsystemTextureResources (bit 10)");
        }
        const BufferImpl::PipeSubsystemArmVerdict verdict = BufferImpl::ClassifyPipeSubsystemArm(
            bitSet && !refused, /*legacyMemosEnabled=*/true,
            /*legacyArmSurvivesLegacyMemos=*/false);
        if (verdict == BufferImpl::PipeSubsystemArmVerdict::NoArm) {
            BufferImpl::StopOnArmlessPipeSubsystem(
                "MOBILEGL_PIPE_PUSH leaves kMGPipeSubsystemTextureResources (bit 10) clear (or "
                "refuses it) and MOBILEGL_PIPE_LEGACY_MEMOS=0 disables the pre-handle texture "
                "cheap-gate trio");
        }
        if (verdict != BufferImpl::PipeSubsystemArmVerdict::Handles) {
            StopOnTransportWithoutRecordArm(
                "kMGPipeSubsystemTextureResources (bit 10)",
                "the pre-handle texture cheap-gate trio resolves through the frontend texture "
                "objects and their binding slots, which under a transport are client memory (rule "
                "E) rather than anything this side owns");
        }
        const Bool enabled = verdict == BufferImpl::PipeSubsystemArmVerdict::Handles;
        MGLOG_D("MGPipe: Espryt texture-resource family runs the %s arm", enabled ? "handle" : "legacy");
        return enabled;
    }

    Bool ResolveSamplerSubsystemArm() {
        const Uint64 mask = MG_Config::Features.PipePush;
        const Bool bitSet = (mask & MG_Pipe::kMGPipeSubsystemSamplers) != 0;
        Bool refused = false;
        if (bitSet) {
            // The sampler family's row, from MG_Pipe/SubsystemDeps.def, and this is the pair G12
            // drives at 0x9ff. Its mirror (bit 10 set, bit 11 clear) is NOT fine either - that is
            // D-K2's fourth row, refused by ResolveTextureResourceSubsystemArm above - so the
            // dependency is SYMMETRIC and the table carries both halves.
            refused = PipeSubsystemDependencyMissing(mask, MG_Pipe::kMGPipeSubsystemSamplers,
                                                     "kMGPipeSubsystemSamplers (bit 11)");
        }
        const BufferImpl::PipeSubsystemArmVerdict verdict = BufferImpl::ClassifyPipeSubsystemArm(
            bitSet && !refused, /*legacyMemosEnabled=*/true,
            /*legacyArmSurvivesLegacyMemos=*/false);
        if (verdict == BufferImpl::PipeSubsystemArmVerdict::NoArm) {
            BufferImpl::StopOnArmlessPipeSubsystem(
                "MOBILEGL_PIPE_PUSH leaves kMGPipeSubsystemSamplers (bit 11) clear (or refuses "
                "it) and MOBILEGL_PIPE_LEGACY_MEMOS=0 disables UnitSamplerLookupMemo's WeakPtr "
                "arm and SamplerPassMemo's raw-pointer rows");
        }
        if (verdict != BufferImpl::PipeSubsystemArmVerdict::Handles) {
            StopOnTransportWithoutRecordArm(
                "kMGPipeSubsystemSamplers (bit 11)",
                "UnitSamplerLookupMemo's WeakPtr arm and SamplerPassMemo's raw-pointer rows are "
                "keyed on FRONTEND objects, and under a transport there is no frontend object on "
                "this side to key on - a WeakPtr that can never lock is not a slower arm, it is a "
                "unit that samples through whatever it last held");
        }
        const Bool enabled = verdict == BufferImpl::PipeSubsystemArmVerdict::Handles;
        MGLOG_D("MGPipe: Espryt sampler family runs the %s arm", enabled ? "handle" : "legacy");
        return enabled;
    }

    Bool ResolveProgramSubsystemArm() {
        // Bit 12 depends on NOTHING, and that is said out loud rather than left as an absence:
        // a ShaderCso handle names no texture and no buffer, the archive rides beside the
        // record as a companion pointer, and the eight extra inputs the server still
        // specialises on (D-H5) are read from state this backend already holds.
        const Bool bitSet = (MG_Config::Features.PipePush & MG_Pipe::kMGPipeSubsystemPrograms) != 0;
        const BufferImpl::PipeSubsystemArmVerdict verdict = BufferImpl::ClassifyPipeSubsystemArm(
            bitSet, /*legacyMemosEnabled=*/true, /*legacyArmSurvivesLegacyMemos=*/false);
        if (verdict == BufferImpl::PipeSubsystemArmVerdict::NoArm) {
            BufferImpl::StopOnArmlessPipeSubsystem(
                "MOBILEGL_PIPE_PUSH leaves kMGPipeSubsystemPrograms (bit 12) clear and "
                "MOBILEGL_PIPE_LEGACY_MEMOS=0 disables g_programTwinLookupMemo");
        }
        if (verdict != BufferImpl::PipeSubsystemArmVerdict::Handles) {
            StopOnTransportWithoutRecordArm(
                "kMGPipeSubsystemPrograms (bit 12)",
                "g_programTwinLookupMemo is keyed on the frontend ProgramObject and the legacy arm "
                "reads the current program through it; under a transport the program arrives as a "
                "ShaderCso handle with its archive beside the record and there is no frontend "
                "program here at all");
        }
        const Bool enabled = verdict == BufferImpl::PipeSubsystemArmVerdict::Handles;
        MGLOG_D("MGPipe: Espryt program family runs the %s arm", enabled ? "handle" : "legacy");
        return enabled;
    }

    void ResolveRecordArmFamilies() {
        // The masks are ConfigLoader's to refuse (any mask clearing bits 0-13 stops at startup),
        // so the one question left is the driver's: MGPFramebufferState::Color[8] is the wire's
        // width and this is the RAW ES cap, which nothing clamps. The framebuffer family used to
        // refuse its bit and fall back to the legacy arm here (D-C3); with one arm, a driver
        // reporting more stops by name instead of having the record truncate its attachments.
        const Uint32 cap = static_cast<Uint32>(std::max<Int>(g_GLESCapabilities.MaxColorAttachments, 0));
        if (cap > MG_Pipe::kMGPipeMaxColorAttachments) {
            MGLOG_E("MGPipe: this driver reports GL_MAX_COLOR_ATTACHMENTS = %u, above "
                    "MGPFramebufferState::Color[%u]'s wire width", cap, MG_Pipe::kMGPipeMaxColorAttachments);
            BufferImpl::StopOnArmlessPipeSubsystem(
                "kMGPipeSubsystemFramebuffer (bit 9) cannot carry this driver's colour attachments, and "
                "the record arm is the only arm since P13 W6");
        }
    }

    const MG_Pipe::MGPipeResourceRecord* PipeTextureRecordForHandle(MG_Pipe::MGPipeHandle res) {
        return PipeRecordAt(MG_Pipe::MGPipeApplier().TextureResources, res);
    }

    MobileGL::TextureTarget PipeTextureTargetForHandle(MG_Pipe::MGPipeHandle res) {
        const auto* record = PipeTextureRecordForHandle(res);
        if (record == nullptr) return MobileGL::TextureTarget::Unknown;
        // THE INVERSE OF MG_Pipe::MGPipeResourceTargetForTextureTarget, COMPUTED FROM IT rather
        // than restated as a second table. MGPipeTypes.h already static_asserts that the
        // forward map covers the frontend enum and that it separates the one pair anybody has
        // ever folded together (2D vs rectangle), so a linear search over a dozen enumerators
        // is total, cannot drift, and runs at most once per image unit per sweep. A second
        // switch here is exactly how the two spellings end up disagreeing about a target that
        // was added to only one of them.
        for (SizeT i = 0; i < static_cast<SizeT>(MobileGL::TextureTarget::TextureTargetCount); ++i) {
            const auto target = static_cast<MobileGL::TextureTarget>(i);
            if (MG_Pipe::MGPipeResourceTargetForTextureTarget(target) ==
                static_cast<Uint32>(record->Desc.Target)) {
                return target;
            }
        }
        return MobileGL::TextureTarget::Unknown;
    }

    Int PipeTextureBackendSampleCountForHandle(MG_Pipe::MGPipeHandle res) {
        const auto* record = PipeTextureRecordForHandle(res);
        if (record == nullptr) return 0;
        const MobileGL::TextureTarget target = PipeTextureTargetForHandle(res);
        if (!TextureImpl::IsMultisampleTextureTarget(target)) return 0;
        // The same three inputs the storage allocation clamps with (SyncToBackend's multisample
        // arm): the record's format and requested count, and the format's ES transfer format,
        // which only decides the integer-vs-colour fallback when the probe has no answer.
        const auto internalFormat = static_cast<TextureInternalFormat>(record->Desc.InternalFormat);
        GLenum glInternalFormat = 0, glFormat = 0, glType = 0;
        TextureImpl::GenerateTextureFormatInfo(internalFormat, &glInternalFormat, &glFormat, &glType, target);
        return ClampSamplesToBackendSupport(GetFormatCapabilityTargetIndex(target), internalFormat, glFormat,
                                            static_cast<Int>(record->Desc.Samples));
    }

    const MG_Pipe::MGPipeSamplerCsoRecord* PipeSamplerCsoRecordForHandle(MG_Pipe::MGPipeHandle cso) {
        return PipeRecordAt(MG_Pipe::MGPipeApplier().SamplerCsos, cso);
    }

    const MG_Pipe::MGPipeSamplerViewRecord* PipeSamplerViewRecordForHandle(MG_Pipe::MGPipeHandle view) {
        return PipeRecordAt(MG_Pipe::MGPipeApplier().SamplerViewCsos, view);
    }

    // N-7: D KEYS ON THE UPLOAD HALF; WIRE (AccumulatePendingUpload, PipeApply.cpp) KEYS ON
    // THE WHOLE PACKED FIELD. That asymmetry is resolved HERE, in D's favour and with the
    // reason, rather than by widening the two comparisons: D's question is per (uploadTarget,
    // level) because that is the granularity its upload loop iterates - it is handed a
    // TextureUploadTarget and a level and has no second key to offer - while wire's is per
    // record, where the whole field arrived. They agree because a texture's resource-target
    // byte is constant for the object's life; if that ever stopped holding, wire would store
    // two entries where D sees one and D's consume would strand the second in the set for ever
    // (a full re-upload of that level on every sync, silently).
    //
    // So the low byte is CROSS-CHECKED instead of ignored. The comparand is the resource's own
    // descriptor Target, which B fills from exactly the expression the sub-data's low byte
    // comes from (TextureEmit.h's MGPipeBuildTextureResourceDesc and EmitResourceCreate against
    // EmitOneLevel), so the two are carried and equal by construction and any disagreement is a
    // seam defect, not a shape to learn. This is also MGPResourceDesc::Target's first reader in
    // this package (esprytobj-v2.md §2(6) recorded it as having none).
    void NotePipeSubDataTargetLowByte(const MG_Pipe::MGPipeResourceRecord& record, Uint16 packedTarget, Uint16 level) {
        const Uint8 carried = MG_Pipe::MGPipeSubDataResourceTargetOf(packedTarget);
        if (carried == record.Desc.Target) return;
        MGLOG_E_ONCE("MGPipe: a pending upload for level %u carries resource target %u while its "
                     "resource's descriptor says %u - D matches on the upload half only, so this "
                     "entry was matched anyway; the two sides disagree about what the resource is",
                     static_cast<Uint>(level), static_cast<Uint>(carried),
                     static_cast<Uint>(record.Desc.Target));
    }

    const MG_Pipe::MGPipeResourceRecord::PendingUpload* FindPipeTextureUpload(
        const MG_Pipe::MGPipeResourceRecord& record, Uint16 uploadTarget, Uint16 level) {
        // Linear, and deliberately so: the set holds one entry per DIRTY (uploadTarget, level)
        // of one texture, which is at most six faces x the level count and in practice one or
        // two entries. A map would cost an allocation per texture per frame to save a walk of
        // three.
        //
        // THE STORED KEY IS THE PACKED MGPSubData::Target (ID-12): low byte
        // MGPipeResourceTarget, HIGH byte TextureUploadTarget. The applier stores the whole
        // field verbatim (PipeApply.cpp's `entry.UploadTarget = record.Target`) because
        // MGPRespecifiedLevel::UploadTarget has to pair with it byte for byte, so the decode is
        // the reader's - here and at the consume, the only two places D compares it. `uploadTarget`
        // is static_cast<Uint16>(MobileGL::TextureUploadTarget), i.e. already the half.
        // Without the decode every texture upload would be dropped silently: Texture1D is 0 and
        // Texture2D is 1, which collide with the resource-target byte of Buffer and Tex1D.
        for (const auto& pending : record.PendingUploads) {
            if (MG_Pipe::MGPipeSubDataUploadTargetOf(pending.UploadTarget) == static_cast<Uint8>(uploadTarget) &&
                pending.Level == level) {
                NotePipeSubDataTargetLowByte(record, pending.UploadTarget, level);
                return &pending;
            }
        }
        return nullptr;
    }

    void ConsumePipeTextureUpload(MG_Pipe::MGPipeHandle res, Uint16 uploadTarget, Uint16 level) {
        // Re-resolved rather than taken as a reference from the caller: between the read and
        // the consume the caller has issued driver work, and the applier's Vector may have been
        // grown by a re-entrant call in between. Cheap - the resolve is a bounds check and two
        // compares - and it is the difference between a dangling reference and a no-op.
        auto& applier = MG_Pipe::MGPipeApplier();
        if (MG_Pipe::MGPipeHandleIsNull(res) || res.Slot >= applier.TextureResources.size()) return;
        auto& record = applier.TextureResources[res.Slot];
        if (!record.Live || record.Gen != res.Gen) return;
        for (SizeT index = 0; index < record.PendingUploads.size(); ++index) {
            const auto& pending = record.PendingUploads[index];
            // The same HIGH-byte decode FindPipeTextureUpload makes, and it has to be the same
            // one: a consume that missed here after a find that hit would leave the entry in the
            // set forever and re-upload the level on every sync.
            if (MG_Pipe::MGPipeSubDataUploadTargetOf(pending.UploadTarget) != static_cast<Uint8>(uploadTarget) ||
                pending.Level != level) {
                continue;
            }
            NotePipeSubDataTargetLowByte(record, pending.UploadTarget, level);
            // Swap-and-pop: the set is unordered by construction (it is a per-(target, level)
            // accumulation, not a queue), and an ordered erase would be quadratic over a full
            // cube-map drain.
            record.PendingUploads[index] = std::move(record.PendingUploads.back());
            record.PendingUploads.pop_back();
            return;
        }
    }

    Bool RearmPipeTextureLevelUpload(MG_Pipe::MGPipeHandle res, Uint16 packedTarget, Uint16 level,
                                     const MG_Pipe::MGPBox& wholeLevel) {
        // THE SERVER RE-DIRTYING THE CLIENT'S MODEL IS THE ONE DIRECTION D-D5's INVERSION DOES
        // NOT COVER, and this is the server-side answer to it (esprytobj review M-1).
        //
        // D-D5 moves the rect model and the emission cursor to the CLIENT, which clears its own
        // flags at emission - so on the handle arm IsStorageDirty is already false for every
        // level the applier accepted, and the upload loop reads the applier's pending set
        // instead. But three sites in this backend still write into the frontend dirty model,
        // and the pending set cannot see them:
        //
        //   Managers.cpp  RequireImageBindableStorage  - re-dirties every defined level so the
        //                 widened image carrier is replayed rather than allocated empty (LIVE,
        //                 and it is this helper's only caller);
        //   DirectGLES.cpp:7406  GenerateThreeChannelFloatMipmapOnCpu - same shape, milder
        //                 outcome (package E's file; E's verification round calls this helper
        //                 there, or states why not);
        //   DirectGLES.cpp:6735  the CLEAR half (MarkStorageDirty(..., false)) after a
        //                 glGetTexImage shadow refresh - it clears rather than dirties, so
        //                 nothing is owed and there is nothing to re-arm.
        //
        // The alternative the review offered - OR the frontend flag back into the per-level
        // question - was NOT taken: it would put two authorities back on the handle arm, and
        // "the server never clears the client's flag" (D-D5) would then have to grow an
        // exception for the levels the OR consumed. Re-arming the set keeps ONE authority; the
        // consume path is unchanged and clears exactly what it uploaded.
        //
        // The entry is WHOLE-LEVEL and carries no regions (RegionCount 0 = "the union box is the
        // whole story", D-D3), because a replay owes every texel of the level, and it MERGES
        // with an entry the client already emitted rather than duplicating it - a whole-level
        // box subsumes any scatter behind it. Returns false, loudly, when the set is at its
        // bound: a dropped replay is the bug this exists to stop, so it must not be silent.
        auto& applier = MG_Pipe::MGPipeApplier();
        if (MG_Pipe::MGPipeHandleIsNull(res) || res.Slot >= applier.TextureResources.size()) return false;
        auto& record = applier.TextureResources[res.Slot];
        if (!record.Live || record.Gen != res.Gen) return false;
        const Uint8 uploadHalf = MG_Pipe::MGPipeSubDataUploadTargetOf(packedTarget);
        for (auto& pending : record.PendingUploads) {
            if (MG_Pipe::MGPipeSubDataUploadTargetOf(pending.UploadTarget) != uploadHalf ||
                pending.Level != level) {
                continue;
            }
            pending.UnionBox = wholeLevel;
            pending.Regions.clear();
            return true;
        }
        if (record.PendingUploads.size() >= MG_Pipe::kMGPipeMaxPendingUploads) {
            MGLOG_E_ONCE("MGPipe: texture handle {%u, %u} already holds the applier's %u pending "
                         "uploads, so the server's own re-dirty of (uploadTarget=%u, level=%u) "
                         "cannot be armed - that level will not be replayed",
                         res.Slot, res.Gen, MG_Pipe::kMGPipeMaxPendingUploads, uploadHalf, level);
            return false;
        }
        MG_Pipe::MGPipeResourceRecord::PendingUpload entry;
        entry.UploadTarget = packedTarget;
        entry.Level = level;
        entry.UnionBox = wholeLevel;
        record.PendingUploads.push_back(std::move(entry));
        return true;
    }

    namespace TextureImpl {
#undef MGB_RECT_SRC_PTR
    } // namespace TextureImpl
} // namespace MobileGL::MG_Backend::DirectGLES
