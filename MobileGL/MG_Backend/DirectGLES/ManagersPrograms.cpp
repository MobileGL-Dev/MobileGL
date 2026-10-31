// MobileGL - MobileGL/MG_Backend/DirectGLES/ManagersPrograms.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "ManagersInternal.h"

namespace MobileGL::MG_Backend::DirectGLES {
    // ES has no atomic-counter buffers: glslang lowers every atomic_uint onto a synthesized
    // storage block, so one GL counter BUFFER costs one of the driver's shader-storage binding
    // points. Those slots are taken from the TOP of the range downwards - below the one
    // mg_IndirectParams already reserves - so an application binding its own SSBOs from 0 upwards
    // never meets them, and the slot for GL binding N is `this - N` in every stage of the
    // program without any shared state. Negative when the driver has no room left at all.
    static Int AtomicCounterEsslBindingTop() {
        return g_GLESCapabilities.MaxShaderStorageBufferBindings - 2;
    }

    static Bool ShouldAvoidExplicitLodBiasOnAngleLlvmpipe() {
        // IsAngleLlvmpipeRenderer combined with the MOBILEGL_ESPRYT_AVOID_EXPLICIT_LOD_BIAS
        // feature toggle, both resolved in FillInGLESCapabilities.
        return g_GLESCapabilities.AvoidExplicitLodBias;
    }

    // GL 4.6 core table 23.53 requires GL_MAX_SAMPLES >= 4, so this is the floor MobileGL
    // advertises whatever the ES driver reports. It steers ClampMultisampleFetchesForEssl
    // AND is part of the L2 translation-memo key, so the transpile and the key must read
    // the same constant - hence one definition rather than two locals.
    // Recomputed here rather than calling GL_Getter's GetAdvertisedMaxSamples(): this is
    // backend code and must not reach into the GL frontend. 4 is that translation unit's
    // kFrontendMaxSamples, which is the source of truth - keep the two in step.
    constexpr Int kFrontendMaxSamples = 4;

    // Whether a usampler2DMS fetch's sample index is bounded at RUNTIME by the bound texture's
    // real sample count rather than at compile time by the integer ceiling: armed when that
    // ceiling is squeezed below what MobileGL advertises AND the depth ceiling - which the
    // stencil aspect of a multisample depth/stencil texture is allocated under, and which a
    // usampler reads too - exceeds it, so no one static bound is right for both. Read on both
    // sides of the translation-memo boundary (the SPIR-V pass defers, the ESSL pass takes
    // over); it is a function of three values the memo key already carries, so the two halves
    // cannot disagree about one program.
    static Bool ClampsUnsignedMultisampleFetchesAtRuntime() {
        const Int advertisedMaxSamples = std::max(g_GLESCapabilities.MaxSamples, kFrontendMaxSamples);
        return g_GLESCapabilities.MaxIntegerSamples < advertisedMaxSamples &&
               g_GLESCapabilities.MaxDepthTextureSamples > g_GLESCapabilities.MaxIntegerSamples;
    }

    static Uint ResolveBackendEsslVersion() {
        const auto& version = g_GLESCapabilities.GLESVersion;
        if (version.Major > 3 || (version.Major == 3 && version.Minor >= 2)) {
            return 320;
        }
        if (version.Major == 3 && version.Minor >= 1) {
            return 310;
        }
        return 300;
    }

    // The transpile pipeline invents image binding numbers: when the GL source declares
    // an image uniform without layout(binding), glslang auto-assigns one (desktop GL
    // allows that and lets the app pick the unit with glUniform1i, which ES forbids on
    // image uniforms). The unit the app actually addresses lives in frontend state: the
    // layout(binding) reflected at link time, or whatever glUniform1i stored afterwards.
    // Rewrite every image uniform declaration to that unit so imageLoad/Store hits the
    // unit the app bound with glBindImageTexture.
    String RebindImageUniformsToFrontendUnits(String source, const PrgramImpl::ProgramBuildSource& src) {
        // P5e (pg): the SOURCE, not the frontend object. The two reads below - the uniform's
        // location and the unit glUniform1i put there - are exactly the pair the record's
        // set_program_bindings tail carries, so on the handle arm this bakes the unit the client
        // observed rather than one it would have had to dereference a live ProgramObject for.
        // The null test the SharedPtr form carried moved to the caller, which returned early on
        // a null program long before it reached here.
        if (source.find("image") == String::npos) {
            return source;
        }
        static const std::regex imageDeclRegex(
            R"((layout\s*\(([^)]*)\)\s*)?uniform\s+(?:(?:readonly|writeonly|coherent|volatile|restrict|highp|mediump|lowp)\s+)*[iu]?image[A-Za-z0-9]+\s+([A-Za-z_][A-Za-z0-9_]*)\s*(\[[^\]]*\])?\s*;)");
        static const std::regex bindingValueRegex(R"(binding\s*=\s*\d+)");

        String result;
        result.reserve(source.size());
        SizeT lineStart = 0;
        while (lineStart <= source.size()) {
            const SizeT lineEnd = source.find('\n', lineStart);
            const Bool lastLine = lineEnd == String::npos;
            String line = source.substr(lineStart, lastLine ? String::npos : lineEnd - lineStart);

            std::smatch match;
            if (std::regex_search(line, match, imageDeclRegex)) {
                const String name = match[3].str();
                Int location = src.GetUniformLocation(name);
                if (location < 0) {
                    location = src.GetUniformLocation(name + "[0]");
                }
                if (location >= 0) {
                    const Int unit = src.GetUniformSamplerOrImageUnitIndex(location);
                    if (unit >= 0) {
                        const String bindingText = "binding = " + std::to_string(unit);
                        if (std::regex_search(line, bindingValueRegex)) {
                            line = std::regex_replace(line, bindingValueRegex, bindingText);
                        } else if (match[1].matched) {
                            const SizeT layoutOpen = line.find('(', match.position(1));
                            line.insert(layoutOpen + 1, bindingText + ", ");
                        } else {
                            line.insert(match.position(0), "layout(" + bindingText + ") ");
                        }
                    }
                }
            }

            result += line;
            if (lastLine) {
                break;
            }
            result += '\n';
            lineStart = lineEnd + 1;
        }
        return result;
    }

    const MG_Pipe::MGPipeShaderCsoRecord* PipeShaderCsoRecordForHandle(MG_Pipe::MGPipeHandle cso) {
        // The composite band is a CLIENT-side indexing detail (contract D13) and the server
        // must not learn about it: one reader, one answer, and the band's own table is indexed
        // by (slot - base) exactly as the allocator's is.
        if (MG_Pipe::MGPipeIsCompositeShaderSlot(cso.Slot)) {
            const auto& band = MG_Pipe::MGPipeApplier().CompositeShaderCsos;
            const Uint32 index = cso.Slot - MG_Pipe::kMGPipeShaderCsoCompositeSlotBase;
            if (index >= band.size()) return nullptr;
            const auto& record = band[index];
            if (!record.Live || record.Gen != cso.Gen) return nullptr;
            return &record;
        }
        return PipeRecordAt(MG_Pipe::MGPipeApplier().ShaderCsos, cso);
    }

    namespace PrgramImpl {
        Uint32 g_snormFallbackClampOutputMask = 0;
        Uint g_fragColorBroadcastCount = 1;
        Uint32 g_unormFallbackClampOutputMask = 0;
        Uint g_lastUsedBackendProgramId = 0;
        // Every error-queue drain in the program build path is bounded by this: a lost
        // context never answers GL_NO_ERROR, and the build runs on the thread that would
        // then spin forever.
        constexpr Int kMaxDrainedProgramErrors = 32;
        TwinRegistry<MG_State::GLState::ProgramObject, BackendProgramObjectImpl, MG_Pipe::MGPipeKind::ShaderCso>
            g_backendProgramObjects;

        // ---- P5e (pg): the source, both ways round ---------------------------------------

        Int ProgramArchiveSource::GetUniformLocation(const String& name) const {
            // ProgramObject::GetUniformLocation over the archive. Reproduced rather than called
            // because the frontend's form is a non-static member over Artifacts() and this arm
            // has no ProgramObject to call it on; the ARRAY RULES are the whole body and they
            // are the part a paraphrase would get wrong, so they are transcribed exactly.
            const auto it = Link->uniformLocations.find(name);
            if (it != Link->uniformLocations.end()) return static_cast<Int>(it->second);
            if (name.empty()) return -1;
            // Reflection stores GL-style names: an array uniform is keyed "arr[0]" at its base
            // location, so a bare "arr" resolves to that entry.
            if (name.back() != ']') {
                const auto suffixed = Link->uniformLocations.find(name + "[0]");
                if (suffixed != Link->uniformLocations.end()) return static_cast<Int>(suffixed->second);
                return -1;
            }
            if (name.length() < 4) return -1;
            // An ARRAY OF ARRAYS is keyed by its full "[0]"-terminated spelling ("a[2][1][0]"),
            // so a query that already ends in a subscript may still be the NAME of an array
            // rather than an element of one. That reading is tried FIRST; only then is the
            // trailing subscript read as an element index.
            {
                const auto arrayOfArrays = Link->uniformLocations.find(name + "[0]");
                if (arrayOfArrays != Link->uniformLocations.end()) {
                    return static_cast<Int>(arrayOfArrays->second);
                }
            }
            const SizeT bracket = name.rfind('[');
            if (bracket == String::npos || bracket + 1 >= name.length() - 1) return -1;
            Uint element = 0;
            for (SizeT i = bracket + 1; i < name.length() - 1; ++i) {
                if (name[i] < '0' || name[i] > '9') return -1;
                element = element * 10 + static_cast<Uint>(name[i] - '0');
                if (element > 0x0FFFFFFFu) return -1;
            }
            auto base = Link->uniformLocations.find(name.substr(0, bracket) + "[0]");
            if (base == Link->uniformLocations.end()) {
                base = Link->uniformLocations.find(name.substr(0, bracket));
                if (base == Link->uniformLocations.end()) return -1;
            }
            const Int baseLocation = static_cast<Int>(base->second);
            if (!IsValidUniformLocation(baseLocation)) return -1;
            const Int index = Link->uniformIndexInTProgram[baseLocation];
            if (!UniformAt(index).type.isArray) return -1;
            if (static_cast<GLint>(element) >=
                MG_State::GLState::ProgramObject::GetUniformArraySizeByTIndex(*Link, index)) {
                return -1;
            }
            const Int location = baseLocation + static_cast<Int>(element);
            if (!UniformLocationsAliasSameUniform(baseLocation, location)) return -1;
            return location;
        }

        const String& ProgramArchiveSource::GetUniformBlockName(Uint index) const {
            static const String kEmpty;
            if (index >= Link->glBlockIndexToTProgram.size()) return kEmpty;
            const Int tBlockIndex = Link->glBlockIndexToTProgram[index];
            if (tBlockIndex < 0 || static_cast<SizeT>(tBlockIndex) >= Link->blockReflection.size()) {
                return kEmpty;
            }
            return Link->blockReflection[tBlockIndex].name;
        }

        Uint ProgramArchiveSource::GetUniformBlockBinding(Uint index) const {
            // THE OVERLAY WINS WHEN THERE IS ONE, and the archive's link-time value stands when
            // there is not. glUniformBlockBinding moves this AFTER the link that produced the
            // archive, so a twin answering from the archive alone would bind the program's
            // blocks to the points it was LINKED with rather than the ones it is BOUND with.
            if (OverlayGoverns && BlockBindingOverlay != nullptr) {
                if (index < BlockBindingOverlay->size()) {
                    return static_cast<Uint>((*BlockBindingOverlay)[index]);
                }
                // A block past the tail the record declared. The tail is dense over
                // GetActiveUniformBlocksCount(), so this is a record and an archive that
                // disagree about how many blocks the program has - which can only happen if a
                // create and a bindings record crossed. 0 is GL's own default and is the safe
                // answer; the mismatch is the emitter's to fix, not this read's to hide.
                return 0;
            }
            if (index >= Link->uniformBlockBinding.size()) return 0;
            return static_cast<Uint>(Link->uniformBlockBinding[index]);
        }

        Int ProgramArchiveSource::GetUniformSamplerOrImageUnitIndex(Uint location) const {
            if (OverlayGoverns) {
                // THE TAIL IS THE WHOLE SET, so a location it does not name has no unit - NOT
                // whatever the archive's link-time snapshot said. The client emits every
                // assigned unit it can see, including the ones the link seeded from
                // layout(binding=N), so "absent" really does mean -1.
                //
                // Sparse and ASCENDING BY LOCATION on the wire (CONTRACT-P5E §1), which is what
                // lets this be a binary search: the build walks every location from 0 to
                // maxUniformLocation, and a linear scan per location would be quadratic on a
                // program with a thousand locations and a handful of samplers.
                if (SamplerUnitOverlay == nullptr) return -1;
                const auto it = std::lower_bound(
                    SamplerUnitOverlay->begin(), SamplerUnitOverlay->end(), location,
                    [](const MG_Pipe::MGPProgramSamplerUnit& entry, Uint value) { return entry.Location < value; });
                if (it == SamplerUnitOverlay->end() || it->Location != location) return -1;
                return static_cast<Int>(it->Unit);
            }
            if (location >= Link->uniformSamplerOrImageUnitIndex.size()) return -1;
            return Link->uniformSamplerOrImageUnitIndex[location];
        }

        ProgramArchiveSource ProgramArchiveSource::FromRecord(MG_Pipe::MGPipeHandle cso,
                                                              const MG_Pipe::MGPipeShaderCsoRecord& record) {
            // THE HANDLE ARM. Every field below is server-owned: the archive the create adopted,
            // the descriptor the create stored, and the three tails set_program_bindings
            // carried. Nothing here touches a ProgramObject, which is the property the strict
            // lane measures.
            ProgramArchiveSource source;
            source.Link = &record.Archive->Link;
            source.Spirv = &record.Archive->Spirv;
            source.Identity = cso.Slot;
            source.IdentityIsHandleSlot = true;
            // LinkStatus IS A FIELD NOW, and that is the whole point of c0e adding it: before
            // P5e "linked" was only implied by the create existing, which is a different
            // statement - create_shader_state is re-issued at every link that moves the link
            // version, and a FAILED relink of a bound program moves it too (ID-88).
            source.Linked = record.Desc.LinkStatus != 0;
            source.SpirvUsable = record.Desc.SpirvStatus != 0;
            source.SpirvValidationEnabled = record.Desc.EnableSpirvValidation != 0;
            source.PointSizeWasDemoted = record.Desc.PointSizeDemoted != 0;
            source.GlobalUboSize = record.Desc.GlobalUboSize;
            source.LinkedStages.reserve(record.Archive->LinkedStages.size());
            for (const Uint32 stage : record.Archive->LinkedStages) {
                source.LinkedStages.push_back(static_cast<ShaderStage>(stage));
            }
            // "HAS A BINDINGS RECORD EVER BEEN APPLIED TO THIS RECORD" - not "is the tail
            // non-empty". A program with no uniform blocks, no sampler and no override emits a
            // record with three empty tails, and that record is the statement that the archive's
            // link-time values are no longer the answer.
            source.OverlayGoverns = record.BindingsSerial != 0;
            source.BlockBindingOverlay = &record.BlockBindings;
            source.SamplerUnitOverlay = &record.SamplerUnits;
            if (source.OverlayGoverns) {
                source.StorageOverrides.reserve(record.StorageOverrides.size());
                for (const auto& entry : record.StorageOverrides) {
                    source.StorageOverrides[entry.Name] = entry.Binding;
                }
            } else {
                source.StorageOverrides = record.Archive->Link.shaderStorageBlockBinding;
            }
            // THE RECORD'S NUMBER EITHER WAY, and deliberately not a recomputation when the
            // overlay is absent. This is the value the twin STAMPS and the draw path's clean
            // clause COMPARES (CONTRACT-P5E §5.5 names record.Signature as the input), so the
            // two have to be the same field or every draw of a program with an override
            // rebuilds. A record with no bindings applied carries 0 while the archive may hold
            // a link-time override map - the next set_program_bindings then produces a non-zero
            // signature and forces exactly one rebuild, which is correct and, since the emitter
            // sends the bindings in the same breath as the create, not a state a draw reaches.
            source.StorageOverrideSignature = record.Signature;
            return source;
        }

        Uint ProgramBlockBindingFromRecord(const MG_Pipe::MGPipeShaderCsoRecord& record, Int blockIndex) {
            if (blockIndex < 0) return 0;
            const SizeT index = static_cast<SizeT>(blockIndex);
            if (record.BindingsSerial != 0) {
                // The tail is the whole set; a block past it is a record and an archive that
                // disagree about the block count, and GL's own default of 0 is the safe answer.
                return index < record.BlockBindings.size() ? static_cast<Uint>(record.BlockBindings[index]) : 0;
            }
            if (!record.Archive || index >= record.Archive->Link.uniformBlockBinding.size()) return 0;
            return static_cast<Uint>(record.Archive->Link.uniformBlockBinding[index]);
        }

        Int ProgramSamplerUnitFromRecord(const MG_Pipe::MGPipeShaderCsoRecord& record, Uint location) {
            if (record.BindingsSerial != 0) {
                const auto it = std::lower_bound(
                    record.SamplerUnits.begin(), record.SamplerUnits.end(), location,
                    [](const MG_Pipe::MGPProgramSamplerUnit& entry, Uint value) { return entry.Location < value; });
                if (it == record.SamplerUnits.end() || it->Location != location) return -1;
                return static_cast<Int>(it->Unit);
            }
            if (!record.Archive || location >= record.Archive->Link.uniformSamplerOrImageUnitIndex.size()) {
                return -1;
            }
            return record.Archive->Link.uniformSamplerOrImageUnitIndex[location];
        }

        // P5e (id), CONTRACT-P5E §4.1. See Managers.h for the contract - in particular why the
        // composite band had to land in the same package as this function.
        BackendProgramObjectImpl* ResolveProgramTwin(MG_Pipe::MGPipeHandle cso) {
            if (MG_Pipe::MGPipeHandleIsNull(cso)) return nullptr;
            // THE RECORD FIRST, and through the BAND-AWARE reader: a composite pipeline's CSO
            // lives in the applier's CompositeShaderCsos table, and asking the ordinary one for
            // it would answer null for a program that is perfectly live.
            if (PipeShaderCsoRecordForHandle(cso) == nullptr) {
                MGLOG_E_ONCE("MGPipe: shader CSO {%u, %u} has no applier record on the handle arm, so "
                             "no driver program can be built for it and the draw keeps what is bound",
                             cso.Slot, cso.Gen);
                return nullptr;
            }
            auto* slot = AdoptTwinByHandle(g_backendProgramObjects, cso, "shader CSO");
            if (slot == nullptr) return nullptr;
            if (!*slot) *slot = MakeShared<BackendProgramObjectImpl>();
            return slot->get();
        }

        BackendProgramObjectImpl::BackendProgramObjectImpl() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            m_backendProgramId = g_GLESFuncs.glCreateProgram();
            if (m_backendProgramId == 0) {
                MGLOG_E_ONCE("Failed to create program object in backend.");
                MGLOG_E_ONCE("ES glGetError(): %s", MG_Util::ConvertGLEnumToString(g_GLESFuncs.glGetError()).c_str());

            } else {
                MGLOG_D("Created backend program object with ID: %u", m_backendProgramId);
            }
        }

        BackendProgramObjectImpl::~BackendProgramObjectImpl() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (InProcessTeardown()) {
                return; // see InProcessTeardown(): the driver may be unloaded already
            }
            if (m_backendProgramId != 0) {
                MGLOG_D("Deleting backend program object with ID: %u", m_backendProgramId);
                g_GLESFuncs.glDeleteProgram(m_backendProgramId);
                // The driver may recycle this GL name for a future program; a stale
                // guard entry would then wrongly skip the glUseProgram for it.
                if (g_lastUsedBackendProgramId == m_backendProgramId) {
                    g_lastUsedBackendProgramId = 0;
                }
            }
        }

        Bool ApplyShaderStorageBlockBinding(Uint backendProgramId, const String& blockName, Uint binding) {
            if (backendProgramId == 0 || blockName.empty()) return false;
            if (!g_GLESFuncs.glGetProgramResourceIndex || !g_GLESFuncs.glShaderStorageBlockBinding) return false;
            GLuint driverIndex =
                g_GLESFuncs.glGetProgramResourceIndex(backendProgramId, GL_SHADER_STORAGE_BLOCK, blockName.c_str());
            if (driverIndex == GL_INVALID_INDEX) {
                // An arrayed block is enumerated per element by GL but declared once; the
                // generated ESSL carries the bare block name.
                const auto bracket = blockName.rfind('[');
                if (bracket == String::npos || blockName.back() != ']') return false;
                driverIndex = g_GLESFuncs.glGetProgramResourceIndex(backendProgramId, GL_SHADER_STORAGE_BLOCK,
                                                                    blockName.substr(0, bracket).c_str());
                if (driverIndex == GL_INVALID_INDEX) return false;
            }
            g_GLESFuncs.glShaderStorageBlockBinding(backendProgramId, driverIndex, binding);
            return true;
        }

        void ReseedShaderStorageBlockBindings(Uint backendProgramId, const ProgramBuildSource& src) {
            const auto& overrides = src.GetShaderStorageBlockBindingOverrides();
            if (overrides.empty()) return; // the overwhelming majority of programs
            for (const auto& [blockName, binding] : overrides) {
                if (binding < 0) continue;
                ApplyShaderStorageBlockBinding(backendProgramId, blockName, static_cast<Uint>(binding));
            }
        }

        // P5e (pg). ON A PUSH BUILD THE SIGNATURE IS THE SOURCE'S, TAKEN VERBATIM, and it is not
        // recomputed here for a reason worth stating: on the handle arm the number the DRAW PATH
        // compares against is MGPipeShaderCsoRecord::Signature - the CLIENT's commutative hash,
        // computed in MG_Impl/Pipe/ProgramEmit.h. If the server recomputed its own from the
        // record's override tail, the two formulas would have to stay bit-identical for ever or
        // every draw of every program with an override would rebuild. Taking the client's number
        // verbatim removes that coupling: there is one authority, and the twin's stamp and the
        // clean clause read the same field. The monolith arm's source carries the answer of the
        // function this replaces, computed from the frontend map, so its behavior is unchanged.
        Uint64 ComputeShaderStorageBlockBindingSignature(const ProgramBuildSource& src) {
            return src.StorageOverrideSignature;
        }

        namespace {
            // The GL internal format bound to an image unit right now. GL_NONE for a unit
            // outside the frontend's array, which cannot be addressed at all.
            Uint BoundImageUnitFormat(Int unit) {
                if (unit < 0 || unit >= MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS) return 0;
                // P5e (fb, §5.4): the format-less image bake asks what format the APPLICATION
                // named at this unit, and under a transport the record is the only statement of
                // it - MGPImageView::InternalFormat is that same GLenum, copied by the client
                // out of the unit's own binding. A unit the applier has never been told about
                // answers 0, which is what an unbound unit answers on the frontend arm too.
                const auto& st = MG_Pipe::MGPipeApplier();
                if (static_cast<SizeT>(unit) >= st.BoundShaderImages.size()) return 0;
                return static_cast<Uint>(st.BoundShaderImages[static_cast<SizeT>(unit)].InternalFormat);
            }

            // Combines one (unit, format) pair into a running digest. Commutative, so the order
            // the uniforms are walked in cannot change the answer, and mixed rather than summed
            // so a unit and a format cannot trade places between two pairs and cancel out.
            Uint64 MixImageUnitFormat(Uint64 signature, Int unit, Uint format) {
                Uint64 entry = static_cast<Uint64>(static_cast<Uint32>(unit)) + 0x9e3779b97f4a7c15ull;
                entry ^= static_cast<Uint64>(format) + 0xbf58476d1ce4e5b9ull + (entry << 6) + (entry >> 2);
                return signature + entry;
            }

            // Pipeline position of a shader stage. Names the PRODUCER of an inter-stage
            // interface block: a block one stage consumes was written by the stage before it.
            // ShaderStage is declared in pipeline order, so the enum value IS the position;
            // compute has no inter-stage interface at all and is reported as -1.
            Int InterStagePipelineIndex(ShaderStage stage) {
                switch (stage) {
                case ShaderStage::Vertex:
                case ShaderStage::TessControl:
                case ShaderStage::TessEval:
                case ShaderStage::Geometry:
                case ShaderStage::Fragment:
                    return static_cast<Int>(stage);
                default:
                    return -1;
                }
            }

            // Whether a stage can declare interface blocks in BOTH directions at once, i.e.
            // whether one block name can name two different blocks inside it. A vertex INPUT
            // and a fragment OUTPUT cannot be blocks and compute has neither, so only these
            // three can. This is what keeps the module probe off every program without
            // tessellation or geometry - which is every program Minecraft and its shader packs
            // build.
            Bool CanDeclareBlocksInBothDirections(ShaderStage stage) {
                return stage == ShaderStage::TessControl || stage == ShaderStage::TessEval ||
                       stage == ShaderStage::Geometry;
            }

            // Reflection names an array uniform after its first element ("g_image[0]") at every
            // location it spans; SPIR-V names the variable once, without the subscript. This is
            // the name both sides agree on.
            String ImageUniformBaseName(const String& reflectionName) {
                if (reflectionName.size() >= 3 && reflectionName.compare(reflectionName.size() - 3, 3, "[0]") == 0) {
                    return reflectionName.substr(0, reflectionName.size() - 3);
                }
                return reflectionName;
            }

            // GL_MAX_<stage>_IMAGE_UNIFORMS as the ES driver reports it, which is also exactly what
            // MobileGL advertises for it (GL_Getter answers from the same DynamicBackendParameters).
            // -1 for a stage the ES side has no such limit for, which is the "cannot say" answer
            // the diagnostic that reads it prints rather than a made-up number. [[maybe_unused]]
            // because its only caller is an MGLOG_E argument, and MGLOG_E compiles to nothing in a
            // build whose MOBILEGL_LOG_ACTIVE_LEVEL is above ERROR.
            [[maybe_unused]] Int AdvertisedStageImageUniformLimit(ShaderStage stage) {
                switch (stage) {
                case ShaderStage::Vertex:
                    return g_GLESCapabilities.MaxVertexImageUniforms;
                case ShaderStage::TessControl:
                    return g_GLESCapabilities.MaxTessControlImageUniforms;
                case ShaderStage::TessEval:
                    return g_GLESCapabilities.MaxTessEvaluationImageUniforms;
                case ShaderStage::Geometry:
                    return g_GLESCapabilities.MaxGeometryImageUniforms;
                case ShaderStage::Fragment:
                    return g_GLESCapabilities.MaxFragmentImageUniforms;
                case ShaderStage::Compute:
                    return g_GLESCapabilities.MaxComputeImageUniforms;
                default:
                    return -1;
                }
            }

            // Whether a glslang layout format is one GLSL ES has in core; the rest reach ES only
            // through GL_NV_image_formats. Asked of DECLARED formats, which this backend passes
            // through untouched - the emitted ESSL still has to be legal for the driver.
            Bool IsCoreEsslLayoutFormat(glslang::TLayoutFormat format) {
                switch (format) {
                case glslang::ElfRgba32f:
                case glslang::ElfRgba16f:
                case glslang::ElfR32f:
                case glslang::ElfRgba8:
                case glslang::ElfRgba8Snorm:
                case glslang::ElfRgba32i:
                case glslang::ElfRgba16i:
                case glslang::ElfRgba8i:
                case glslang::ElfR32i:
                case glslang::ElfRgba32ui:
                case glslang::ElfRgba16ui:
                case glslang::ElfRgba8ui:
                case glslang::ElfR32ui:
                    return true;
                default:
                    return false;
                }
            }

            // The GL internal format a glslang layout format names, for the eighteen non-core
            // formats WidenImageFormatsForEssl carries losslessly plus nothing else: the only
            // question asked of it is "does this DECLARED format widen", and answering 0 for
            // everything else is the same "no" a non-widenable format gets. Kept as its own
            // switch rather than routed through the frontend's enum converters because a
            // TLayoutFormat is a glslang value and the reflection snapshot stores it raw.
            //
            // IT MUST LIST EXACTLY WHAT WideningOfSpirvImageFormat DOES. This table is what arms
            // the pass (ImageFormatWillBeWidened -> declaresWidenableImageFormat), so a format the
            // pass would carry but this switch answers 0 for never gets the chance: the module
            // reaches SPIRV-Cross with its original qualifier, the throw takes the stage, and the
            // only visible symptom is the "no GLSL ES spelling" diagnostic for a format that has
            // one. That is exactly what r11f_g11f_b10f did until it was added here.
            Uint GLInternalFormatOfLayoutFormat(glslang::TLayoutFormat format) {
                switch (format) {
                case glslang::ElfRg32f:
                    return 0x8230; // GL_RG32F
                case glslang::ElfRg16f:
                    return 0x822F; // GL_RG16F
                case glslang::ElfR16f:
                    return 0x822D; // GL_R16F
                case glslang::ElfRg8:
                    return 0x822B; // GL_RG8
                case glslang::ElfR8:
                    return 0x8229; // GL_R8
                case glslang::ElfRg8Snorm:
                    return 0x8F95; // GL_RG8_SNORM
                case glslang::ElfR8Snorm:
                    return 0x8F94; // GL_R8_SNORM
                case glslang::ElfRg32i:
                    return 0x823B; // GL_RG32I
                case glslang::ElfRg16i:
                    return 0x8239; // GL_RG16I
                case glslang::ElfR16i:
                    return 0x8233; // GL_R16I
                case glslang::ElfRg8i:
                    return 0x8237; // GL_RG8I
                case glslang::ElfR8i:
                    return 0x8231; // GL_R8I
                case glslang::ElfRg32ui:
                    return 0x823C; // GL_RG32UI
                case glslang::ElfRg16ui:
                    return 0x823A; // GL_RG16UI
                case glslang::ElfR16ui:
                    return 0x8234; // GL_R16UI
                case glslang::ElfRg8ui:
                    return 0x8238; // GL_RG8UI
                case glslang::ElfR8ui:
                    return 0x8232; // GL_R8UI
                // Not a channel widening but a lossless re-encoding into rgba16f - the one entry
                // here whose carrier has a different per-channel layout. See
                // WidenImageFormatsPass.h.
                case glslang::ElfR11fG11fB10f:
                    return 0x8C3A; // GL_R11F_G11F_B10F
                // 10/10/10/2 unsigned INTEGER channels in an rgba16ui: same component type, same
                // channel count, every value representable. Only the transfer is re-encoded.
                case glslang::ElfRgb10a2ui:
                    return 0x906F; // GL_RGB10_A2UI
                // The seven NORMALIZED formats, carried in an rgba16ui as their own channel CODES.
                // These are the entries whose carrier changes the shader-visible type as well as
                // the qualifier (image2D becomes uimage2D), so every access through them is
                // wrapped in the GL 4.6 2.3.5 conversion - see WidenImageFormatsPass.h.
                case glslang::ElfRgba16:
                    return 0x805B; // GL_RGBA16
                case glslang::ElfRg16:
                    return 0x822C; // GL_RG16
                case glslang::ElfR16:
                    return 0x822A; // GL_R16
                case glslang::ElfRgb10A2:
                    return 0x8059; // GL_RGB10_A2
                case glslang::ElfRgba16Snorm:
                    return 0x8F9B; // GL_RGBA16_SNORM
                case glslang::ElfRg16Snorm:
                    return 0x8F99; // GL_RG16_SNORM
                case glslang::ElfR16Snorm:
                    return 0x8F98; // GL_R16_SNORM
                default:
                    return 0;
                }
            }

            // Whether the ESSL chain will re-declare an image of this format in a core carrier
            // and mask its accesses (WidenImageFormatsForEssl). The same rule
            // TextureImpl::GetImageBindableStorageWidening applies to the storage and the bind -
            // the three layers move together or the shader addresses a texel size the storage
            // does not have.
            //
            // Without GL_NV_image_formats there is no legal spelling for any non-core format, so
            // everything carriable widens. WITH the extension only the formats SPIRV-Cross
            // refuses to print do: it throws for its is_desktop_only_format set instead of
            // emitting a token, and the throw loses the stage however willing the driver was.
            Bool ImageFormatWillBeWidened(Uint glInternalFormat) {
                if (glInternalFormat == 0) return false;
                if (MG_Util::ShaderTranspiler::ShaderCompiler::WidenedCoreEsslImageFormat(glInternalFormat) == 0) {
                    return false;
                }
                return !g_GLESCapabilities.SupportsExtendedImageFormats ||
                       !MG_Util::ShaderTranspiler::ShaderCompiler::SpirvCrossCanPrintEsslImageFormat(glInternalFormat);
            }
        } // namespace

        // What the format bake needs from the frontend, collected in one walk of the uniform
        // reflection: which image uniforms declared NO format (the only ones a bake may touch -
        // a declared format is authoritative and stays), what the units they address currently
        // hold, and whether any format in play - declared or baked - is outside the ES core set.
        ImageFormatBakeInputs CollectImageFormatBakeInputs(const ProgramBuildSource& src) {
            ImageFormatBakeInputs inputs;
            // A format GLSL ES cannot spell on a driver with no GL_NV_image_formats to spell it
            // with. There is no legal ESSL for such a shader at all, so the stage will not
            // compile and the program is lost - a failure that used to leave nothing behind but
            // a draw that rendered nothing. Recorded and reported ONCE per program build rather
            // than per uniform: an image array reaches this decision once per element.
            String unspellableUniform;
            String unspellableFormat;
            Uint unspellableCount = 0;
            const auto recordUnspellableFormat = [&](const String& uniformName, String formatSpelling) {
                if (unspellableCount == 0) {
                    unspellableUniform = uniformName;
                    unspellableFormat = Move(formatSpelling);
                }
                ++unspellableCount;
            };

            const Uint maxUniformLoc = src.GetMaxUniformLocation();
            for (Uint loc = 0; loc <= maxUniformLoc; ++loc) {
                const auto& name = src.GetUniformName(loc);
                if (name.empty()) continue;
                if (!IsImageUniformType(src.GetUniformType(loc))) continue;
                const auto& type = src.GetUniformTypeFacts(loc);
                if (type.hasFormat) {
                    // Declared, and therefore never overridden by the BAKE - but a non-core
                    // spelling still has to become legal ESSL somehow.
                    const auto declaredFormat = static_cast<glslang::TLayoutFormat>(type.layoutFormat);
                    if (!IsCoreEsslLayoutFormat(declaredFormat)) {
                        // Eighteen of the twenty-six non-core formats are re-declared in a core
                        // format that carries them losslessly, with every access masked back to
                        // the channels GL says they have (WidenImageFormatsForEssl, and the
                        // matching storage/bind widening in TextureImpl). Those need neither the
                        // extension nor the diagnostic: there IS a legal spelling for them now.
                        if (ImageFormatWillBeWidened(GLInternalFormatOfLayoutFormat(declaredFormat))) {
                            inputs.declaresWidenableImageFormat = true;
                        } else {
                            inputs.needsExtendedImageFormats = true;
                            if (!g_GLESCapabilities.SupportsExtendedImageFormats) {
                                // From the OWNED TypeFacts, not from a live TType: the reflection
                                // snapshot already carries the declared layout format, and there
                                // is no glslang object to ask on a translation-cache L1 hit.
                                recordUnspellableFormat(name,
                                                        glslang::TQualifier::getLayoutFormatString(declaredFormat));
                            }
                        }
                    }
                    continue;
                }
                const Int unit = src.GetUniformSamplerOrImageUnitIndex(loc);
                if (unit < 0 || unit >= MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS) continue;
                const Uint boundFormat = BoundImageUnitFormat(unit);

                // Every format-less uniform contributes to the rebuild key, including one whose
                // unit holds nothing yet: an image bound for the first time AFTER the link has
                // to move the key, or the program built against "nothing bound" would never be
                // rebuilt against the real format.
                inputs.units.push_back(unit);
                inputs.signature = MixImageUnitFormat(inputs.signature, unit, boundFormat);

                if (boundFormat == 0) continue;
                if (!MG_Util::ShaderTranspiler::ShaderCompiler::GLInternalFormatIsCoreEsslImageFormat(boundFormat)) {
                    // The same three-way split the DECLARED branch above makes, and it has to be
                    // the same one: a format-less image is baked with the bound format, so from
                    // WidenImageFormatsForEssl's point of view the two routes hand it identical
                    // modules and must arm it identically.
                    if (ImageFormatWillBeWidened(boundFormat)) {
                        // The bake writes this format INTO the module, so the widening that runs
                        // straight after has to be armed for it even though nothing DECLARED it -
                        // and armed WHETHER OR NOT the driver has GL_NV_image_formats. SPIRV-Cross
                        // throws for its is_desktop_only_format set the moment it targets ESSL,
                        // however willing the driver was, so the extension decides HOW MUCH gets
                        // widened (widenOnlyUnprintableImageFormats) and never WHETHER. Arming
                        // this only on the no-extension path left the shader half of the widening
                        // switched off while TextureImpl's storage/bind half - which keys on
                        // SpirvCrossCanPrintEsslImageFormat, not on the driver bit - still ran:
                        // the stage threw, the program linked without it, and every dispatch
                        // silently did nothing. That is the whole of
                        // KHR-GL43.stencil_texturing.functional's compute half, whose uni_image is
                        // a format-less uimage2D bound to an R8UI texture.
                        inputs.declaresWidenableImageFormat = true;
                    } else if (!g_GLESCapabilities.SupportsExtendedImageFormats) {
                        // Outside the GLSL ES core set, with no GL_NV_image_formats to spell it
                        // and no core format that carries it exactly: there is no legal ESSL for
                        // this stage at all. Leaving the image format-LESS is NOT a softer
                        // failure: all three test devices reject a format-less image declaration
                        // outright ("all images have to define layout format"), readonly and
                        // writeonly alike, so it trades one hard compile error for another. The
                        // unit stays in the rebuild key either way, so a rebind to a spellable
                        // format still rebuilds and works.
                        MGLOG_D("Image uniform '%s' has no declared format and its unit %d holds 0x%x, which "
                                "GLSL ES core cannot spell, this driver has no GL_NV_image_formats for, and "
                                "no core format carries exactly.",
                                name.c_str(), unit, boundFormat);
                        recordUnspellableFormat(
                            name, MG_Util::ShaderTranspiler::ShaderCompiler::EsslImageFormatSpelling(boundFormat));
                        continue;
                    } else {
                        inputs.needsExtendedImageFormats = true;
                    }
                }
                const String baseName = ImageUniformBaseName(name);
                const auto existing = inputs.glFormatByUniformName.find(baseName);
                if (existing == inputs.glFormatByUniformName.end()) {
                    inputs.glFormatByUniformName.emplace(baseName, boundFormat);
                } else if (existing->second != boundFormat) {
                    // An ARRAY whose elements were pointed at units holding different formats.
                    // One declaration carries one qualifier, so there is no spelling for it, and
                    // the uniform is left format-less rather than given a format that is wrong
                    // for all but one element. Marked in place with GL_NONE and swept below -
                    // never by erasing here, because the entry is reached again by the array's
                    // remaining elements and a flat hash map must not be mutated structurally
                    // while an iterator into it is live.
                    existing->second = 0;
                }
            }
            for (const auto& entry : inputs.glFormatByUniformName) {
                if (entry.second == 0) inputs.conflictedNames.push_back(entry.first);
            }
            for (const auto& conflicted : inputs.conflictedNames) {
                inputs.glFormatByUniformName.erase(conflicted);
            }
            // Split off the ones SPIRV-Cross will not print. They cannot go through the module -
            // it throws for them when targeting ESSL, and the stage is lost - so they are spelled
            // into the emitted text instead. Collected first, erased after, because a flat hash
            // map must not be restructured while it is being walked.
            Vector<String> textCompleted;
            for (const auto& entry : inputs.glFormatByUniformName) {
                if (MG_Util::ShaderTranspiler::ShaderCompiler::SpirvCrossCanPrintEsslImageFormat(entry.second)) {
                    continue;
                }
                // A format the widening carries stays on the module route even though SPIRV-Cross
                // would not print it: by the time SPIRV-Cross sees the declaration it names the
                // core carrier, which it does print. Writing the narrow spelling into the text
                // instead would put back exactly the token the driver rejects.
                if (ImageFormatWillBeWidened(entry.second)) {
                    continue;
                }
                String spelling = MG_Util::ShaderTranspiler::ShaderCompiler::EsslImageFormatSpelling(entry.second);
                if (spelling.empty()) continue; // no image-format spelling at all; nothing to write
                inputs.esslFormatQualifierByUniformName.emplace(entry.first, Move(spelling));
                textCompleted.push_back(entry.first);
            }
            for (const auto& name : textCompleted) {
                inputs.glFormatByUniformName.erase(name);
            }
            // Unlatched MGLOG_E, like the transpile- and link-failure diagnostics in SyncToBackend:
            // one line per failing program build, and naming the uniform and the format is the
            // whole diagnostic value. Left as a log rather than a link failure on purpose - the
            // frontend has already reported LINK_STATUS = true and GL cannot retract it, and the
            // program stays queryable exactly as the "linked but not drawable" exit leaves it.
            if (unspellableCount != 0) {
                MGLOG_E("Image format '%s' on uniform '%s' has no GLSL ES spelling and this driver does not expose "
                        "GL_NV_image_formats%s; the stage using it cannot compile and the program will draw "
                        "nothing.",
                        unspellableFormat.empty() ? "(none)" : unspellableFormat.c_str(), unspellableUniform.c_str(),
                        unspellableCount > 1 ? " (and it is not the only image uniform affected)" : "");
            }
            return inputs;
        }

        // Every image ARRAY whose elements the application did NOT leave on units consecutive from
        // element zero - the only shape ESSL can spell, since an image unit there comes solely
        // from the one layout(binding=N) an array declaration carries. Desktop GL assigns them
        // per element with glUniform1i, which ES makes an INVALID_OPERATION on an image uniform,
        // so there is nothing to fix at the API end and the emitted text has to carry it
        // (RemapImageArrayElementUnits). Empty for every program that does not do this, which is
        // very nearly all of them - one walk of the reflection and no allocation in that case.
        Vector<ImageArrayUnitPlan> CollectNonConsecutiveImageArrayPlans(const ProgramBuildSource& src) {
            Vector<ImageArrayUnitPlan> plans;
            const Uint maxUniformLoc = src.GetMaxUniformLocation();
            for (Uint loc = 0; loc <= maxUniformLoc; ++loc) {
                const auto& name = src.GetUniformName(loc);
                if (name.empty()) continue;
                if (!IsImageUniformType(src.GetUniformType(loc))) continue;
                // Reflection repeats the array's "g_image[0]" spelling at EVERY location the array
                // spans, so only the location that name resolves back to is the array itself.
                if (src.GetUniformLocation(name) != static_cast<Int>(loc)) continue;
                const String baseName = ImageUniformBaseName(name);
                if (baseName == name) continue; // a scalar image: one binding says it all

                ImageArrayUnitPlan plan;
                plan.name = baseName;
                for (Uint element = loc;
                     element <= maxUniformLoc &&
                     src.UniformLocationsAliasSameUniform(static_cast<Int>(loc), static_cast<Int>(element));
                     ++element) {
                    plan.units.push_back(src.GetUniformSamplerOrImageUnitIndex(element));
                }
                if (plan.units.size() < 2) continue;

                Bool consecutive = true;
                for (SizeT element = 0; element < plan.units.size(); ++element) {
                    if (plan.units[element] != plan.units[0] + static_cast<Int>(element)) {
                        consecutive = false;
                        break;
                    }
                }
                // What ESSL does unaided is already right; leaving these out is what keeps the
                // emitted text of every ordinary image shader byte-identical to before.
                if (consecutive) continue;
                plans.push_back(Move(plan));
            }
            return plans;
        }

        Uint64 BackendProgramObjectImpl::ComputeImageUnitFormatSignature() const {
            if (m_formatlessImageUnits.empty()) return 0; // all but a handful of programs
            Uint64 signature = 0;
            for (const Int unit : m_formatlessImageUnits) {
                signature = MixImageUnitFormat(signature, unit, BoundImageUnitFormat(unit));
            }
            return signature;
        }

        Bool BackendProgramObjectImpl::ImageUnitFormatsStillMatch() const {
            if (m_formatlessImageUnits.empty()) return m_imageUnitFormatSignature == 0;
            return ComputeImageUnitFormatSignature() == m_imageUnitFormatSignature;
        }

        // ===== THE MEMOIZED SEGMENT (shader translation memo, level 2) =====
        //
        // One stage's sanitized SPIR-V turned into the ESSL SPIRV-Cross emits, through the
        // DirectGLES-specific pass chain. Extracted out of SyncToBackend's loop so that the
        // boundary the L2 memo keys on is a function signature rather than a comment: every
        // input this reads is either an argument below or a process-global capability bit,
        // and EVERY ONE OF THEM IS IN EsslTranslationKeyInputs. If you add a read here, add
        // it to BuildEsslTranslationKey too - an under-specified key here is a silently
        // miscompiled shader.
        //
        // Reads (audited): the arguments; g_GLESCapabilities.{SupportsViewportArray,
        // MaxSamples, MaxColorTextureSamples, MaxIntegerSamples, MaxDepthTextureSamples,
        // SupportsNoperspectiveInterpolation, SupportsExtendedImageFormats, GLESVersion}
        // (the last via ResolveBackendEsslVersion); and m_backendProgramId, for a log line only.
        //
        // Deliberately NOT in here, and therefore NOT in the key: the text-level passes that
        // follow in SyncToBackend. They are cheap string work and they read a long tail of
        // live per-program state (RebindImageUniformsToFrontendUnits walks the ProgramObject
        // reflection, the norm-clamp masks and the fragColor broadcast count are live
        // globals, the buffer-texture tier retargets an #extension line) whose inclusion
        // would make the key both enormous and fragile for no measurable saving.
        //
        // Returns false when SPIRV-Cross refused the module; `outError` then holds its
        // message and nothing is memoized.
        Bool BackendProgramObjectImpl::TranspileSpirvToEssl(
            const Vector<unsigned int>& spirvCode, const GLenum glShaderType,
            const std::set<String>& xfbCaptureBlockNames, const ImageFormatBakeInputs& imageFormatBake,
            const UnorderedMap<String, Int>& storageBlockBindingOverrides,
            const std::map<String, String>& inputBlockRenames, const std::map<String, String>& outputBlockRenames,
            const Bool stripInputBlockLocations, const Bool stripOutputBlockLocations,
            const Int atomicCounterEsslBindingTop, const Bool narrowIntegerSeedsArmed, const Bool enableSpirvValidation,
            String& outSource, std::set<String>& outFlattenedXfbBlockNames, Vector<Int>& outAtomicCounterGlBindings,
            String& outError) const {
            // ESSL cannot express gl_DrawID/gl_BaseInstance/gl_BaseVertex; demote them to
            // plain globals (mg_*) before handing the module to SPIRV-Cross.
            Vector<unsigned int> loweredSpirv;
            const Vector<unsigned int>* effectiveSpirv = &spirvCode;
            if (glShaderType == GL_VERTEX_SHADER &&
                MG_Util::ShaderTranspiler::ShaderCompiler::LowerDrawParametersForEssl(spirvCode, loweredSpirv,
                                                                                      enableSpirvValidation) &&
                !loweredSpirv.empty()) {
                effectiveSpirv = &loweredSpirv;
            }

            // A geometry stage that writes memory, on a driver that drops the writes it makes
            // after EmitVertex(): every emit is deferred behind them (DeferGeometryEmitsPass). FIRST
            // in the chain, while every output is still an Output variable - the viewport-index
            // lowering below demotes one, and must see the replay's stores to it as well.
            // Self-gated on the module (an emit and a memory write), so a geometry stage that
            // writes nothing comes back untouched. Keyed by deferGeometryEmitsArmed.
            Vector<unsigned int> deferredEmitSpirv;
            if (glShaderType == GL_GEOMETRY_SHADER && g_GLESCapabilities.DropsGeometryWritesAfterEmit) {
                bool deferred = false;
                if (MG_Util::ShaderTranspiler::ShaderCompiler::DeferGeometryEmitsForEssl(
                        *effectiveSpirv, deferredEmitSpirv, deferred, enableSpirvValidation) &&
                    deferred && !deferredEmitSpirv.empty()) {
                    effectiveSpirv = &deferredEmitSpirv;
                    MGLOG_I_ONCE("DirectGLES is deferring a geometry stage's EmitVertex() calls behind its "
                                 "memory writes, because this driver drops storage writes made after an emit.");
                }
            }

            // GL_KHR_shader_subgroup where the ES driver has none (SubgroupPolicy.h): the compute
            // stage's subgroup operations are lowered onto a 32-lane virtual subgroup in
            // workgroup-shared memory, so the ESSL needs nothing from the driver but barrier()
            // and shared memory. Where the driver has subgroups, the operations stay and
            // SPIRV-Cross spells them as GL_KHR_shader_subgroup_* built-ins. iterationRP's
            // scratch race is patched first unless MOBILEGL_ITRP_FIX is ForceOff, as
            // on DirectVulkan. On a driver whose shared-memory loads go stale across barrier()
            // the scratch is read through atomic loads. Keyed by subgroupEmulationArmed /
            // iterationRPBarrierFixArmed / subgroupScratchAtomicReads.
            Vector<unsigned int> barrierFixedSpirv;
            Vector<unsigned int> emulatedSubgroupSpirv;
            if (glShaderType == GL_COMPUTE_SHADER &&
                ResolveEsprytSubgroupImplementation(g_GLESCapabilities) == SubgroupImplementation::Emulated) {
                if (ShouldApplyIterationRPFixes() &&
                    MG_Util::ShaderTranspiler::ShaderCompiler::FixIterationRPBarrierForVulkan(
                        *effectiveSpirv, barrierFixedSpirv, enableSpirvValidation) &&
                    !barrierFixedSpirv.empty()) {
                    effectiveSpirv = &barrierFixedSpirv;
                }
                if (MG_Util::ShaderTranspiler::ShaderCompiler::EmulateSubgroupsForVulkan(
                        *effectiveSpirv, emulatedSubgroupSpirv,
                        static_cast<Uint32>(g_GLESCapabilities.MaxComputeSharedMemorySize), enableSpirvValidation,
                        g_GLESCapabilities.SharedMemoryLoadsGoStale) &&
                    !emulatedSubgroupSpirv.empty()) {
                    effectiveSpirv = &emulatedSubgroupSpirv;
                } else {
                    MGLOG_E("DirectGLES: subgroup emulation declined a compute stage; it keeps subgroup "
                            "operations this ES driver cannot compile");
                }
            }

            // ESSL cannot express gl_ViewportIndex either, but unlike the draw parameters
            // there IS an extension that provides it - so this runs only when the driver does
            // NOT advertise GL_OES_viewport_array. A driver that does keeps the builtin and
            // gets the `#extension` request added to the decompiled source below instead.
            // Demoting the builtin costs the multi-viewport routing (every invocation lands in
            // viewport 0), which is the degradation ViewportArrayScenario already documents
            // for this backend; NOT demoting it costs the whole program, because the stage
            // fails to compile and every draw made with it silently renders nothing.
            // Gated on the module actually declaring the output, so no other stage pays an
            // optimizer round trip for it.
            // One parse of the module answers every armed pass gate below. The per-gate
            // Declares* probes each cost a BuildModule per stage, and on a driver where both
            // gates are armed (Mali: no GL_OES_viewport_array AND integer multisample
            // squeezed to 1) the doubled parse made compile-heavy workloads ~10% slower.
            // Probing the pre-lowering module is sound for both gates: demoting
            // gl_ViewportIndex neither adds nor removes multisampled image types.
            // Recomputed here rather than calling GL_Getter's GetAdvertisedMaxSamples():
            // this is backend code and must not reach into the GL frontend. 4 is that
            // translation unit's kFrontendMaxSamples, which is the source of truth -
            // keep the two in step.
            const Int advertisedMaxSamples = std::max(g_GLESCapabilities.MaxSamples, kFrontendMaxSamples);
            // Armed by the EMULATION as well as by the missing extension, and the emulation is on
            // by default (MOBILEGL_ESPRYT_FORCE_VIEWPORT_ARRAY_EMULATION). Having the extension is not a
            // reason to keep the builtin: it only ever gave the SHADER a compilable name, while
            // the driver's INDEXED viewport state was never programmed by anything in MobileGL
            // (SyncRenderState pushes index 0 and stops), so an extension-capable driver
            // rasterized every index as index 0 exactly like a driver without it. Lowering here
            // is what lets the ESSL passes downstream turn the builtin into the flat varying the
            // replay gates on.
            //
            // Restricted to the three stages GL lets WRITE the builtin (4.1 core gives it to the
            // geometry stage, ARB_shader_viewport_layer_array adds vertex and tessellation
            // evaluation). A fragment stage's gl_ViewportIndex is an INPUT, which the pass
            // declines anyway, and a compute stage has none - so arming those two only ever
            // bought them the shared probe's BuildModule for nothing.
            const Bool stageCanWriteViewportIndex = glShaderType == GL_VERTEX_SHADER ||
                                                    glShaderType == GL_TESS_EVALUATION_SHADER ||
                                                    glShaderType == GL_GEOMETRY_SHADER;
            const Bool viewportLoweringArmed =
                stageCanWriteViewportIndex &&
                (ViewportArrayEmulationEnabled() || !g_GLESCapabilities.SupportsViewportArray);
            const Bool sampleClampArmed = g_GLESCapabilities.MaxColorTextureSamples < advertisedMaxSamples ||
                                          g_GLESCapabilities.MaxIntegerSamples < advertisedMaxSamples ||
                                          g_GLESCapabilities.MaxDepthTextureSamples < advertisedMaxSamples;
            // The image-format widening is armed on EVERY driver, so its probe has to ride the
            // shared parse rather than add one: it is asked of every stage of every program, and
            // a BuildModule per stage per gate is exactly what cost compile-heavy CTS cases ~10%
            // before this struct existed. What differs per driver is only HOW MUCH it widens -
            // everything carriable where there is no GL_NV_image_formats to spell the narrow
            // format, and only the formats SPIRV-Cross refuses to print where there is.
            const Bool widenOnlyUnprintableImageFormats = g_GLESCapabilities.SupportsExtendedImageFormats;
            MG_Util::ShaderTranspiler::ShaderCompiler::SpirvGateFeatures spirvGates;
            if (viewportLoweringArmed || sampleClampArmed) {
                spirvGates = MG_Util::ShaderTranspiler::ShaderCompiler::ProbeSpirvGateFeatures(*effectiveSpirv);
            }

            Vector<unsigned int> loweredViewportSpirv;
            if (viewportLoweringArmed && spirvGates.WritesViewportIndexOutput &&
                MG_Util::ShaderTranspiler::ShaderCompiler::LowerViewportIndexForEssl(
                    *effectiveSpirv, loweredViewportSpirv, enableSpirvValidation) &&
                !loweredViewportSpirv.empty()) {
                effectiveSpirv = &loweredViewportSpirv;
                MGLOG_D("Program %u stage %s writes gl_ViewportIndex, which ESSL has no core "
                        "spelling for. The builtin was demoted to a plain global; %s.",
                        m_backendProgramId, MG_Util::ConvertGLEnumToString(glShaderType).c_str(),
                        ViewportArrayEmulationEnabled() ? "the ESSL passes below promote it to a routing varying"
                                                        : "every invocation renders into viewport 0");
            }

            // GL 4.6 core table 23.53 requires GL_MAX_SAMPLES >= 4, so every multisample
            // ceiling MobileGL advertises is floored to 4 no matter what the ES driver
            // reports - but the realized allocation cannot be, and
            // ClampSamplesToBackendSupport quietly gives an integer or depth multisample
            // texture the ONE sample Adreno and Mali actually support for it. A shader
            // written against the advertised ceiling then fetches a sample that storage does
            // not have and reads garbage; KHR-GL33/40/41.texture_swizzle.functional_* and
            // KHR-GLxx.texture_size_promotion.functional bake exactly that literal in. Clamp
            // the Sample operand to the backend-real per-category maximum so the fetch lands
            // inside the allocation. Gated on some category actually being squeezed AND the
            // module actually declaring a multisampled image, so no other stage pays an
            // optimizer round trip for it. DirectVulkan is deliberately not given this: it
            // allocates the sample count it was asked for, so its modules are already right.
            Vector<unsigned int> clampedSampleSpirv;
            if (sampleClampArmed && spirvGates.DeclaresMultisampledImage &&
                MG_Util::ShaderTranspiler::ShaderCompiler::ClampMultisampleFetchesForEssl(
                    *effectiveSpirv, clampedSampleSpirv, g_GLESCapabilities.MaxColorTextureSamples,
                    g_GLESCapabilities.MaxIntegerSamples, g_GLESCapabilities.MaxDepthTextureSamples,
                    advertisedMaxSamples, enableSpirvValidation, ClampsUnsignedMultisampleFetchesAtRuntime()) &&
                !clampedSampleSpirv.empty()) {
                effectiveSpirv = &clampedSampleSpirv;
            }

            // GLSL ES has no ARRAY vertex inputs, and SPIRV-Cross refuses the whole module
            // rather than emulating them, so this has to happen before it sees the binary.
            Vector<unsigned int> splitArrayInputSpirv;
            if (glShaderType == GL_VERTEX_SHADER &&
                MG_Util::ShaderTranspiler::ShaderCompiler::SplitArrayVertexInputsForEssl(
                    *effectiveSpirv, splitArrayInputSpirv, enableSpirvValidation) &&
                !splitArrayInputSpirv.empty() && splitArrayInputSpirv != *effectiveSpirv) {
                // Only when the pass ACTUALLY split something: the optimizer hands back a
                // re-serialized copy either way, and that round trip is not free - it cost the
                // minecraft-1.21.1-neoforge-create-indirect-in-world-align1024 retrace 0.15 SSIM.
                effectiveSpirv = &splitArrayInputSpirv;
            }

            // An armed vertex stage's narrow-integer seeds (NarrowIntegerSeeds.h), after the split
            // and adopted only when the pass rewrote the module. Keyed by narrowIntegerSeedsArmed.
            Vector<unsigned int> narrowSeededSpirv;
            effectiveSpirv =
                SeedNarrowIntegerVertexInputsForEssl(narrowIntegerSeedsArmed && glShaderType == GL_VERTEX_SHADER,
                                                     *effectiveSpirv, narrowSeededSpirv, enableSpirvValidation);

            // Adopt the rewritten module only when THIS stage actually had one of the blocks, for
            // the array-input split's reason above. The report is per stage, not cumulative: a
            // fragment shader consuming a block the vertex stage reported must still take its own
            // rewrite or the two stages stop matching.
            Vector<unsigned int> flattenedXfbSpirv;
            if (!xfbCaptureBlockNames.empty()) {
                // Reported into a local first, and published only if the module is really
                // adopted. The caller unions the published set unconditionally (so that a
                // cache HIT contributes its names too), so publishing a name for a rewrite
                // that was declined would rename a capture the emitted ESSL never renamed.
                std::set<String> flattenedNames;
                if (MG_Util::ShaderTranspiler::ShaderCompiler::FlattenXfbInterfaceBlocksForEssl(
                        *effectiveSpirv, xfbCaptureBlockNames, flattenedNames, flattenedXfbSpirv,
                        enableSpirvValidation) &&
                    !flattenedXfbSpirv.empty() && !flattenedNames.empty()) {
                    effectiveSpirv = &flattenedXfbSpirv;
                    outFlattenedXfbBlockNames = Move(flattenedNames);
                }
            }

            // The producer-keyed interface-block rename, planned program-wide by the caller and
            // applied to this stage: the blocks it CONSUMES are spelled after the previous stage
            // present in the program and the ones it PRODUCES after itself, so a tessellation
            // evaluation stage's two TCSOutputBlocks stop being one name and every other stage
            // still agrees with it. See UniquifyIoBlockNamesPass for why Mali needs it.
            //
            // INSIDE THE MEMOIZED SEGMENT, and at exactly the position it was written in - after
            // the XFB flatten, before the UBO precision strip. Both halves of that matter:
            //   * INSIDE, because it rewrites the MODULE and the emitted ESSL carries the result.
            //     Left outside, a second program sharing this stage's key would be served ESSL
            //     with the blocks un-renamed and the repair would silently stop working - the
            //     same trap SetAtomicCounterBlockBindings sets one screen down.
            //   * AT THIS POSITION, because moving a SPIR-V pass in a chain is a behavioral
            //     change, and this one arrived device-verified on Mali. Hoisting it above the
            //     cache probe instead would have needed no key material at all (the rename would
            //     already be in the module bytes the key hashes) and was rejected for that
            //     reason: it reorders the chain, and it would re-serialize the module on every
            //     build including the ones the memo is there to make free.
            // Its two rename maps are therefore KEY MATERIAL - see the caller.
            Vector<unsigned int> uniquifiedIoBlockSpirv;
            if (!inputBlockRenames.empty() || !outputBlockRenames.empty()) {
                std::set<String> stageRenamedIoBlockNames;
                if (MG_Util::ShaderTranspiler::ShaderCompiler::UniquifyIoBlockNamesForEssl(
                        *effectiveSpirv, inputBlockRenames, outputBlockRenames, stageRenamedIoBlockNames,
                        uniquifiedIoBlockSpirv, enableSpirvValidation) &&
                    !uniquifiedIoBlockSpirv.empty() && !stageRenamedIoBlockNames.empty()) {
                    effectiveSpirv = &uniquifiedIoBlockSpirv;
                    MGLOG_D("Program %u stage %s: %zu inter-stage interface block(s) renamed per "
                            "producing stage, because some stage of this program declares the same "
                            "block name in both directions and the ES driver may alias the two.",
                            m_backendProgramId, MG_Util::ConvertGLEnumToString(glShaderType).c_str(),
                            stageRenamedIoBlockNames.size());
                }
            }

            // ESSL stage-matches uniform blocks by member precision, but SPIRV-Cross prints
            // a RelaxedPrecision member as explicit "mediump" in the vertex stage and as
            // UNQUALIFIED (mediump-by-default) in the fragment stage; after
            // ForceSupporterOutput swaps the fragment header to highp, that member reads
            // back as highp and the ES driver refuses to link ("definitions of uniform
            // block ... do not match"). Strip the hint from block structs so both stages
            // declare the member highp; nothing else about emission changes.
            Vector<unsigned int> uboPrecisionSpirv;
            if (MG_Util::ShaderTranspiler::ShaderCompiler::StripUboMemberRelaxedPrecisionForEssl(
                    *effectiveSpirv, uboPrecisionSpirv, enableSpirvValidation) &&
                !uboPrecisionSpirv.empty()) {
                effectiveSpirv = &uboPrecisionSpirv;
            }

            // noperspective is core desktop GLSL and reaches here as the SPIR-V NoPerspective
            // decoration. SPIRV-Cross renders it as ESSL `noperspective` + `#extension
            // GL_NV_shader_noperspective_interpolation : require`; a driver without that extension
            // rejects the require. So on such devices emulate screen-linear interpolation instead
            // (pre-multiply outputs by gl_Position.w, recover inputs via gl_FragCoord.w) and drop
            // the decoration - exact, extension-free. Devices that have the extension keep the
            // decoration and let the hardware do it natively.
            Vector<unsigned int> noperspectiveSpirv;
            if (!g_GLESCapabilities.SupportsNoperspectiveInterpolation &&
                MG_Util::ShaderTranspiler::ShaderCompiler::EmulateNoPerspectiveForEssl(
                    *effectiveSpirv, noperspectiveSpirv, enableSpirvValidation) &&
                !noperspectiveSpirv.empty()) {
                effectiveSpirv = &noperspectiveSpirv;
            }

            // ES has no rectangle sampler, and SPIRV-Cross refuses the whole module rather
            // than approximating one. The shared pass turns the type into the 2D one and
            // divides the coordinate of every normalized-coordinate lookup by the texture
            // size, which is the whole of the difference between the two.
            Vector<unsigned int> rectLoweredSpirv;
            if (MG_Util::ShaderTranspiler::ShaderCompiler::LowerRectImages(*effectiveSpirv, rectLoweredSpirv,
                                                                           enableSpirvValidation) &&
                !rectLoweredSpirv.empty()) {
                effectiveSpirv = &rectLoweredSpirv;
            }

            // ES has no 1D texture at all, so a 1D ARRAY is stored as a 2D array with height
            // 1 (MapToBackendTextureTarget / GetBackendUploadSize). SPIRV-Cross emulates 1D
            // as 2D for images without ever asking whether the type is arrayed, so a
            // 1D-array image comes out as ivec2(ivec2(u, layer), 0) - three components in a
            // two-component constructor, which every driver rejects, taking the whole
            // program with it. The pass does the conversion properly - type to 2D array,
            // coordinate to (u, 0, layer) - before SPIRV-Cross can apply its own.
            Vector<unsigned int> arrayImageSpirv;
            if (MG_Util::ShaderTranspiler::ShaderCompiler::Lower1DArrayImagesForEssl(*effectiveSpirv, arrayImageSpirv,
                                                                                     enableSpirvValidation) &&
                !arrayImageSpirv.empty()) {
                effectiveSpirv = &arrayImageSpirv;
            }

            // The SAMPLER half of the same 1D story, and a defect one layer deeper than the one
            // above. SPIRV-Cross DOES widen a 1D sampler's coordinate for ES - it just prints the
            // OFFSET and the two GRADIENT operands with the arity the desktop shader spelled, so
            // textureLodOffset(sampler1DArray, vec2, float, int) is emitted against a
            // sampler2DArray and the driver answers "no matching overloaded function found",
            // losing the stage and silently no-oping every dispatch that used it. Widening the
            // operands alone would be an INVALID module (the validator derives the required arity
            // from the image's own Dim), so the pass moves the type to 2D and widens coordinate,
            // offset and gradients together.
            //
            // NO KEY MATERIAL, by the same test LegalizeResourceArrayIndexingForEssl passes:
            // it takes the module and nothing else, no capability bit arms it, and it self-gates
            // on the module's own content (BinaryHasOffsetOrGrad1DSampledImage). The module is
            // already the largest thing in the L2 key, so it is covered completely.
            Vector<unsigned int> sampled1DSpirv;
            if (MG_Util::ShaderTranspiler::ShaderCompiler::Lower1DSampledImagesForEssl(*effectiveSpirv, sampled1DSpirv,
                                                                                       enableSpirvValidation) &&
                !sampled1DSpirv.empty()) {
                effectiveSpirv = &sampled1DSpirv;
            }

            // GLSL ES has no format-less image: `writeonly uniform uimage2D` is legal desktop
            // GLSL 4.2 and an Adreno ES compile error ("all images have to define layout
            // format"), which loses the whole program. Give each such image the format the
            // application bound to its unit - the one GL's format-class rules make correct -
            // so SPIRV-Cross prints a qualifier. AFTER the 1D-array lowering above, which
            // also rewrites image types, so this one is looking at the final shapes.
            //
            // Gated on the module actually declaring one: the map is empty for every program
            // whose images all declare formats, and the cheap probe keeps a program that has
            // an unbound format-less image from paying an optimizer round trip per stage.
            Vector<unsigned int> imageFormatSpirv;
            if (!imageFormatBake.glFormatByUniformName.empty() &&
                MG_Util::ShaderTranspiler::ShaderCompiler::DeclaresFormatlessStorageImage(*effectiveSpirv) &&
                MG_Util::ShaderTranspiler::ShaderCompiler::BakeImageFormatsForEssl(
                    *effectiveSpirv, imageFormatBake.glFormatByUniformName, imageFormatSpirv, enableSpirvValidation) &&
                !imageFormatSpirv.empty()) {
                effectiveSpirv = &imageFormatSpirv;
            }

            // GL has forty image formats and GLSL ES core has thirteen; the other twenty-seven
            // reach ES only through GL_NV_image_formats, which no tested driver advertises. A
            // shader declaring one of them has NO legal ESSL spelling at all - SPIRV-Cross throws
            // for some of them and the driver rejects the token for the rest ("'rg32f' : not a
            // legal layout qualifier id"), and dropping the qualifier is refused too ("all images
            // have to define layout format") - so the stage is lost and every draw with the
            // program silently renders nothing. Seventeen of them widen EXACTLY into a core format
            // of the same per-channel width, and this rewrites those declarations to the carrier
            // and masks every access back to the channels GL says the format has. The other nine
            // have no exact carrier and keep the honest diagnostic
            // CollectImageFormatBakeInputs emits.
            //
            // A driver that HAS GL_NV_image_formats still needs part of this. SPIRV-Cross throws
            // for its is_desktop_only_format set when it targets ESSL rather than printing a
            // token, and the throw loses the stage however willing the driver was - llvmpipe
            // advertises the extension and `layout(r8ui) uimage2D` lost its whole program there
            // until the widening ran for it too. So the driver bit decides HOW MUCH is widened,
            // never WHETHER.
            //
            // AFTER the bake above, deliberately: a format-less image whose unit holds a non-core
            // format is baked with that format and widened here, so both routes end in the same
            // place and there is no second widening rule for baked declarations.
            //
            // KEY MATERIAL: g_GLESCapabilities.SupportsExtendedImageFormats, which selects the
            // mode - see EsslTranslationKeyInputs::supportsExtendedImageFormats. The pass takes
            // no other input: what it rewrites is a pure function of the module's own declared
            // formats and that mode, and the module is already the largest thing in the L2 key.
            // The ARMING flag is deliberately NOT key material: it only decides whether the pass
            // runs, and the module below is adopted only when the pass actually changed the bytes
            // - so a program-wide flag that over-arms a stage costs an optimizer round trip and
            // changes no output.
            //
            // DirectVulkan is deliberately not given this: it takes the declared format natively
            // and resolves the descriptor's view format from the same bind state.
            Vector<unsigned int> widenedImageFormatSpirv;
            if (imageFormatBake.declaresWidenableImageFormat &&
                MG_Util::ShaderTranspiler::ShaderCompiler::WidenImageFormatsForEssl(
                    *effectiveSpirv, widenedImageFormatSpirv, widenOnlyUnprintableImageFormats,
                    enableSpirvValidation) &&
                !widenedImageFormatSpirv.empty() && widenedImageFormatSpirv != *effectiveSpirv) {
                effectiveSpirv = &widenedImageFormatSpirv;
            }

            // GLSL ES demands a constant integral expression to index a fragment output
            // array; SPIR-V does not, so a shader that writes coeff[i] from a loop
            // reaches SPIRV-Cross intact and comes out as ESSL a strict driver rejects
            // outright ("array indexes for fragment outputs must be constant integral
            // expressions"), linking no program and silently no-oping every draw that
            // uses it. llvmpipe accepts it, ANGLE does not - which is the whole of the
            // improved-transparency-minecraft-26.3 failure. Fold or lower the index here,
            // on the ESSL path only: the same module is legal for DirectVulkan.
            Vector<unsigned int> outputIndexSpirv;
            if (glShaderType == GL_FRAGMENT_SHADER &&
                MG_Util::ShaderTranspiler::ShaderCompiler::LegalizeFragmentOutputIndexingForEssl(
                    *effectiveSpirv, outputIndexSpirv, enableSpirvValidation) &&
                !outputIndexSpirv.empty()) {
                effectiveSpirv = &outputIndexSpirv;
            }

            // Same rule, two more resources, every stage: desktop GL lets an array of
            // storage blocks and an array of image uniforms be indexed with any
            // dynamically-uniform expression, GLSL ES keeps the ES 3.1
            // constant-expression rule for both, and the drivers enforce it - Qualcomm
            // ("indexing into an SSBO array using a non-constant expression is not
            // permitted"), llvmpipe ("image arrays indexed with non-constant expressions are
            // forbidden in GLSL ES") - losing the stage, the program, and every draw or
            // dispatch that used it, while the frontend keeps reporting the link glslang
            // performed. Fold or lower the index here, on the ESSL path only: the same
            // module is legal for DirectVulkan, which binds the array as one descriptor
            // array.
            //
            // The image half is also what makes RemapImageArrayElementUnits below possible
            // at all: that pass rewrites `g_image[k]` into a per-element declaration, and it
            // can only do that once every k the emitted ESSL spells is a literal.
            //
            // NO KEY MATERIAL, and that is a conclusion rather than an omission: this takes the
            // module and nothing else - no capability bit arms it, no per-program plan steers
            // it - and it self-gates on the module's own content
            // (BinaryHasDynamicResourceArrayIndexing). The module is already the largest
            // thing in the L2 key, so it is fully covered. Contrast LowerViewportIndexForEssl,
            // whose signature is equally module-only but which SupportsViewportArray ARMS -
            // that bit is in the key precisely because of it.
            Vector<unsigned int> blockArrayIndexSpirv;
            if (MG_Util::ShaderTranspiler::ShaderCompiler::LegalizeResourceArrayIndexingForEssl(
                    *effectiveSpirv, blockArrayIndexSpirv, enableSpirvValidation) &&
                !blockArrayIndexSpirv.empty()) {
                effectiveSpirv = &blockArrayIndexSpirv;
            }

            // glslang kept the application's layout(offset = N) on the atomic counters it
            // lowered onto gl_AtomicCounterBlock_<N>, and no std140/std430 layout can put
            // member 0 anywhere but offset 0 - so SPIRV-Cross throws ("cannot be expressed as
            // neither std430 nor std140") and the stage never reaches the driver. Collapse the
            // block into one uint array at offset 0 and re-index each counter to the element
            // that used to be at its byte offset; the buffer then stays bound whole, which it
            // has to (GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT is 64 on Adreno 830 and 32 or
            // more everywhere else, so an 8-byte bind offset is not expressible on any of them).
            // BEFORE SetAtomicCounterBlockBindings
            // below, which only moves the block's BINDING and needs the block intact.
            //
            // NO KEY MATERIAL either, for the same reason - and note where the application's
            // layout(offset = N) values live: glslang already baked them into the module as
            // member Offset decorations, so they are in the key as MODULE BYTES. There is no
            // separate offset input to carry.
            Vector<unsigned int> atomicCounterSpirv;
            if (MG_Util::ShaderTranspiler::ShaderCompiler::FlattenAtomicCounterBlockOffsetsForEssl(
                    *effectiveSpirv, atomicCounterSpirv, enableSpirvValidation) &&
                !atomicCounterSpirv.empty()) {
                effectiveSpirv = &atomicCounterSpirv;
            }

            // The second half of the inter-stage interface-block repair, and the one that
            // actually closes the 420pack group: this driver drops the payload of a block that
            // carries an explicit layout(location=) whenever a tessellation or geometry stage
            // is in the pipeline, so the qualifier comes off and ES matches the block by name
            // and member sequence instead. The names those two sides agree on are the ones the
            // rename above just fixed, which is why this runs AFTER it and not before.
            //
            // The caller arms the two directions; both are false unless the driver POST
            // measured the defect AND this program has a stage that can hit it. Adopted only
            // when this stage really had a located block, for the reason the array-input split
            // documents: the optimizer hands back a re-serialized copy either way.
            //
            // LAST IN THE CHAIN, and that position is load-bearing. Vulkan SPIR-V REQUIRES a
            // Location on every user-defined Input/Output variable
            // ([VUID-StandaloneSpirv-Location-04915]), so the module this produces is
            // deliberately no longer valid Vulkan SPIR-V - it is an ESSL-emission intermediate
            // that goes straight into SPIRV-Cross and reaches no driver as SPIR-V. Running it
            // here means no later pass validates what it produced; the pass itself skips
            // validation for the same reason (see StripIoBlockLocationsForEssl). Anywhere
            // earlier and every remaining pass would latch a validation failure on a module
            // that is doing exactly what it was asked to.
            Vector<unsigned int> strippedIoBlockLocationSpirv;
            if (stripInputBlockLocations || stripOutputBlockLocations) {
                Bool strippedAny = false;
                if (MG_Util::ShaderTranspiler::ShaderCompiler::StripIoBlockLocationsForEssl(
                        *effectiveSpirv, stripInputBlockLocations, stripOutputBlockLocations, strippedAny,
                        strippedIoBlockLocationSpirv, enableSpirvValidation) &&
                    !strippedIoBlockLocationSpirv.empty() && strippedAny) {
                    effectiveSpirv = &strippedIoBlockLocationSpirv;
                    // THE ARMING SIGNAL, and it is INFO on purpose: the per-stage line below is
                    // MGLOG_D, which is compiled out of every build CI and the device runs, so
                    // nothing outside a debug build could tell an armed repair from a silently
                    // un-armed one. Latched, so it costs one line per process rather than one
                    // per stage of every program. The integration lane that pins the emulation
                    // on asserts on exactly this line - see UnlocatedIoBlockScenario.
                    MGLOG_I_ONCE("DirectGLES is emitting inter-stage interface blocks WITHOUT their "
                                 "layout(location) qualifier, because this driver loses a located "
                                 "block's payload across a tessellation or geometry boundary.");
                    MGLOG_D("Program %u stage %s: interface-block location qualifiers dropped "
                            "(%s), because this driver loses a located block's payload across a "
                            "tessellation or geometry boundary.",
                            m_backendProgramId, MG_Util::ConvertGLEnumToString(glShaderType).c_str(),
                            stripInputBlockLocations
                                ? (stripOutputBlockLocations ? "consumed and produced" : "consumed")
                                : "produced");
                }
            }

            MG_Util::ShaderTranspiler::SpvcSession spvcSession(*effectiveSpirv,
                                                               MG_Util::ShaderTranspiler::SessionUsageBit::Transpile);

            spvc_compiler_options options;
            spvcSession.CreateOptions(&options);

            spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_GLSL_VERSION, ResolveBackendEsslVersion());
            spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_ES, SPVC_TRUE);
            spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_VULKAN_SEMANTICS, SPVC_FALSE);

            spvcSession.SetOptions(options);

            // ES fixes a storage block's binding at link from its layout(binding=) qualifier
            // and has no glShaderStorageBlockBinding to move it afterwards, so a rebinding
            // can only be honored by printing it INTO the qualifier. Rewriting the Binding
            // decoration before SPIRV-Cross emits is what does that; RemoveLayoutBinding
            // then deliberately preserves the qualifier for `buffer` declarations.
            if (!storageBlockBindingOverrides.empty()) { // empty for almost every program
                spvcSession.SetShaderStorageBlockBinding(storageBlockBindingOverrides);
            }

            // Atomic counters, same mechanism for the same reason. glslang already turned
            // every atomic_uint into a member of gl_AtomicCounterBlock_<N> and let the IO
            // mapper pick that block's binding, which has no relation to the GL binding point
            // N the application bound its counter buffer to - and can alias an SSBO the
            // application binds itself. Move each block to its reserved slot and record N, so
            // the draw path knows which GL_ATOMIC_COUNTER_BUFFER points to re-issue as
            // storage-buffer bindings.
            //
            // BOTH HALVES ARE MEMO STATE. `atomicCounterEsslBindingTop` decides the binding
            // this prints into the ESSL, so it is in the L2 key; `outAtomicCounterGlBindings`
            // is an OUTPUT this stage produces and the draw path consumes, so it is in the L2
            // payload. A hit that replayed only the text would leave the bindings empty and
            // every counter buffer unbound - the same class of silent loss the flattened XFB
            // block names would have been.
            spvcSession.SetAtomicCounterBlockBindings(atomicCounterEsslBindingTop, outAtomicCounterGlBindings);

            // `layout(index = 0)` is the GL default spelled out loud, and GLSL ES has no such
            // qualifier in core - a stage that prints it is refused with "index layout
            // qualifier requires EXT_blend_func_extended" and the whole program then draws
            // nothing. Drop the decoration when it carries the default; a REAL dual-source
            // index (1) is left alone, because that one genuinely needs the extension and the
            // driver has to see it. Fragment stage only: no other stage can carry it.
            if (glShaderType == GL_FRAGMENT_SHADER) {
                spvcSession.DropDefaultFragmentOutputColorIndex();
            }

            // `readonly writeonly` together says the buffer variable can only be asked its
            // .length(), which the frontend has already enforced - so the pair is inert, and
            // printing it is not. llvmpipe's ES compiler refuses a block spelled that way and the
            // stage never reaches the program.
            spvcSession.RelaxReadWriteExclusiveStorageBuffers();

            // gl_ShadingRateEXT / gl_PrimitiveShadingRateEXT: SPIRV-Cross spells them only for
            // Vulkan GLSL, so they go through under a sentinel name and are restored below. Only
            // a program whose stages use them gets here with anything to do, and only a driver
            // with GL_EXT_fragment_shading_rate gets such a program past the frontend.
            const Bool restoreShadingRateBuiltins = spvcSession.PrepareShadingRateBuiltinsForEssl();

            const char* result = nullptr;
            spvcSession.Compile(&result);

            if (!result) {
                // The caller owns the diagnostic: it is the one that knows the
                // frontend program id, and a failed transpile must NOT be memoized -
                // the message names the stage and is worth re-emitting every time.
                const char* lastError = spvcSession.GetLastErrorString();
                outError = lastError ? lastError : "";
                return false;
            }

            outSource = result;
            if (restoreShadingRateBuiltins) {
                MG_Util::ShaderTranspiler::SpvcSession::FinishShadingRateBuiltinsForEssl(outSource);
            }
            return true;
        }

        // GL 4.6 core 11.2.2 lets a program have a tessellation EVALUATION shader and no CONTROL
        // shader: the input patch is passed through unmodified and the levels come from the
        // PATCH_DEFAULT_OUTER_LEVEL / PATCH_DEFAULT_INNER_LEVEL state. OpenGL ES 3.2 has no such
        // state and no such allowance - it rejects the program at link, and with an EMPTY info
        // log, which was verified on an Adreno 830 with no MobileGL in the process (TES-only:
        // link=0, log empty; the same shaders plus any TCS: link=1, with or without the SSBO and
        // atomic counter the failing conformance case also declares). The frontend's own glslang
        // link succeeds, so GL_LINK_STATUS reads TRUE, program 0 is bound in its place, and every
        // draw silently renders nothing - a black framebuffer, an atomic counter still at 0 and
        // an untouched SSBO, with no error anywhere.
        //
        // So the missing stage is synthesized and attached here, alongside the program's own.
        // DirectVulkan already does exactly this for the same structural reason
        // (ProgramFactory::BuildPassthroughTessControlSource), so this completes the pair rather
        // than inventing an approach.
        //
        // Nothing that works today can be harmed by it: it fires ONLY for a program that has an
        // evaluation stage and no control stage, and every such program fails to link on ES right
        // now. The worst case is that the synthesized stage fails to compile or link, which leaves
        // the program exactly as dead as it already was - but with a driver log that says why,
        // where today there is an empty one.
        void BackendProgramObjectImpl::AttachPassthroughTessControlStage(
            const ProgramBuildSource& src, const Int tessEvalShaderIndex,
            const Vector<Vector<unsigned int>>& shaderSpirvs, const String& vertexStageEssl,
            const String& tessEvalStageEssl) {
            // PATCH_VERTICES is dynamic state, and it decides the synthesized stage's output
            // patch size - so a program built for one value is stale for another. Recorded here
            // and compared on the draw path (SyncCurrentProgram), the same shape as the
            // storage-block and image-format signatures next to it.
            const Uint patchVertices = MG_Pipe::gPipeInputs->IsLive() ? MG_Pipe::gPipeInputs->GetPatchVertices() : 3u;
            m_passthroughTessControlPatchVertices = static_cast<Int>(patchVertices);
            // PATCH_DEFAULT_{OUTER,INNER}_LEVEL are the same kind of dynamic state and are baked
            // into the same stage (ES has no such state and no entry point to forward them to), so
            // they are recorded and compared alongside the patch size - the two move together, as
            // BuildPassthroughTessControlEssl's contract says.
            m_passthroughTessControlOuterLevel = MG_Pipe::gPipeInputs->IsLive()
                                                     ? MG_Pipe::gPipeInputs->GetPatchDefaultOuterLevel()
                                                     : FloatVec4(1.0f, 1.0f, 1.0f, 1.0f);
            m_passthroughTessControlInnerLevel = MG_Pipe::gPipeInputs->IsLive()
                                                     ? MG_Pipe::gPipeInputs->GetPatchDefaultInnerLevel()
                                                     : FloatVec2(1.0f, 1.0f);

            if (tessEvalShaderIndex < 0 || static_cast<SizeT>(tessEvalShaderIndex) >= shaderSpirvs.size()) {
                MGLOG_E("Program %u has a tessellation evaluation stage with no control stage, but no "
                        "SPIR-V for it; the pass-through control stage GL describes cannot be checked, so "
                        "the program is left to fail its ES link.",
                        src.GetExternalIndex());
                m_backendProgramUsable = false;
                return;
            }

            // The one shape the pass-through cannot stand in for. It forwards gl_Position and
            // nothing else, so an evaluation stage that reads a user-defined varying or a
            // per-patch input - both of which carry a Location, where every built-in it needs
            // does not - would start reading undefined values the moment a control stage sat
            // between it and the vertex stage. Declining keeps that from being silent; it is the
            // identical rule DirectVulkan applies in ReflectPassthroughTessControlNeed.
            if (MG_Util::ShaderTranspiler::ShaderCompiler::ModuleReadsLocatedInput(
                    shaderSpirvs[static_cast<SizeT>(tessEvalShaderIndex)])) {
                MGLOG_E("Program %u has a tessellation evaluation stage with no control stage AND reads a "
                        "user-defined input through it; a synthesized pass-through control stage cannot "
                        "forward that, so the program is declined rather than fed an undefined varying.",
                        src.GetExternalIndex());
                m_backendProgramUsable = false;
                return;
            }

            // Mirrored from the neighbors rather than fixed: whether SPIRV-Cross redeclares
            // gl_PerVertex, and with which members, depends on what the application's shaders
            // touched, and a synthesized stage that redeclares a DIFFERENT shape than the stage
            // it feeds is an ES link error against a program with no other problem. gl_in copies
            // the vertex stage's OUT block (that is what arrives) and gl_out the evaluation
            // stage's IN block (that is what is expected). A neighbor that redeclared nothing
            // yields an empty list, which leaves the driver's own built-in declaration in place -
            // which is exactly what matching it requires.
            const String inMembers = ExtractPerVertexBlockMembers(vertexStageEssl, /*input=*/false).value_or(String());
            const String outMembers =
                ExtractPerVertexBlockMembers(tessEvalStageEssl, /*input=*/true).value_or(String());

            String source =
                BuildPassthroughTessControlEssl(ResolveBackendEsslVersion(), patchVertices, inMembers, outMembers,
                                                m_passthroughTessControlOuterLevel, m_passthroughTessControlInnerLevel);
            // The mirrored member lists can carry gl_PointSize - the neighbor stage declared it,
            // so matching it is the whole point - and a redeclaration is exactly as illegal as a
            // reference in ESSL without the extension. Same directive, same never-speculative
            // rule as the per-stage loop; a driver with neither spelling gets nothing added and
            // fails below with its own message, which is the honest outcome for a shape it
            // cannot express.
            const char* passthroughPointSizeExtension =
                source.find("gl_PointSize") != String::npos
                    ? PointSizeExtensionName(g_GLESCapabilities.TessellationPointSizeSupport, /*tessellation=*/true)
                    : nullptr;
            source = RequestPointSizeExtension(Move(source), passthroughPointSizeExtension);
            // Same for gl_ClipDistance/gl_CullDistance mirrored out of a vertex stage that
            // writes them: the neighbor could only compile with the extension, so it exists.
            source = RequestClipCullDistanceExtension(Move(source));

            const GLuint backendShaderId = g_GLESFuncs.glCreateShader(GL_TESS_CONTROL_SHADER);
            if (backendShaderId == 0) {
                MGLOG_E("Failed to create the synthesized pass-through tessellation control shader for "
                        "program %u.",
                        src.GetExternalIndex());
                m_backendProgramUsable = false;
                return;
            }

            const char* sourceCStr = source.c_str();
            MGLOG_D("Synthesized pass-through tessellation control stage for program %u (patch vertices "
                    "%u):\n%s",
                    src.GetExternalIndex(), patchVertices, sourceCStr);
            g_GLESFuncs.glShaderSource(backendShaderId, 1, &sourceCStr, nullptr);
            g_GLESFuncs.glCompileShader(backendShaderId);

            // GL_FALSE, not GL_TRUE, for the reason the per-stage loop states: an unwritten
            // out-param must read as "compile failed" and never as a silent success.
            GLint compileStatus = GL_FALSE;
            g_GLESFuncs.glGetShaderiv(backendShaderId, GL_COMPILE_STATUS, &compileStatus);
            if (compileStatus == GL_FALSE) {
                GLint logLength = 0;
                g_GLESFuncs.glGetShaderiv(backendShaderId, GL_INFO_LOG_LENGTH, &logLength);
                if (logLength < 0) logLength = 0;
                Vector<GLchar> log(static_cast<SizeT>(logLength) + 1, '\0');
                g_GLESFuncs.glGetShaderInfoLog(backendShaderId, logLength, nullptr, log.data());
                log.back() = '\0';
                MGLOG_E("The synthesized pass-through tessellation control stage failed to compile for "
                        "program %u. Driver log: %s\nSource:\n%s",
                        src.GetExternalIndex(), log.data(), sourceCStr);
                m_backendProgramUsable = false;
                g_GLESFuncs.glDeleteShader(backendShaderId);
                return;
            }

            g_GLESFuncs.glAttachShader(m_backendProgramId, backendShaderId);
            // Same ownership handover as every other stage: glDeleteShader only FLAGS, so this is
            // what makes the program own it and what keeps a relink from leaking it.
            g_GLESFuncs.glDeleteShader(backendShaderId);
        }

        // ---- THE PROGRAM BUILD, AND THE ONE SOURCE IT READS (P5e pg) ----------------------
        //
        // ONE BODY, TWO HEADS, AND THE PULL BUILD'S HEAD IS THE ORIGINAL ONE. In the pull build
        // `ProgramBuildSource` IS `MG_State::GLState::ProgramObject`, this function keeps its
        // name, its signature and its null guard, and `src` is a reference to the object it was
        // already dereferencing on every line - so the pull build compiles the same reads
        // through the same accessors and G1's byte identity survives a rename and nothing else.
        //
        // In a PUSH build the body is a private worker that the two public overloads feed:
        // SyncToBackend(program) - the monolith-glue half, ruling 1 - wraps the frontend's own
        // archive, and SyncToBackendByHandle(cso) wraps the RECORD's. That is the whole of this
        // family's rule F: after this, nothing in a program build dereferences a frontend
        // object, and the forty accessor calls below read memory the server owns.
        void BackendProgramObjectImpl::SyncToBackendFromSource(const ProgramBuildSource& src) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            // Before either early return below, so Use() can name the GL program of a no-op draw.
            m_frontendProgramId = src.GetExternalIndex();

            // GetSpirvStatus() as well as GetLinkStatus(): a program whose phase-B job was
            // canceled (teardown) or whose optimizer run failed is fully linked and fully
            // queryable, but has no SPIR-V to build a driver program out of. GL cannot retract
            // a LINK_STATUS it already reported true, so "linked but not drawable" is the
            // answer, and this is where the ES backend expresses it.
            if (!src.GetLinkStatus() || !src.GetSpirvStatus()) {
                MGLOG_E_ONCE("Program object is not linked or has no generated SPIR-V, skipping backend sync. State "
                             "program ID: %u",
                             src.GetExternalIndex());
                return;
            }
            // After the return above, which keeps the previous driver program and the widths it holds.
            BeginNarrowIntegerSeedBuild(m_narrowIntegerSeeds);

            MGLOG_D("Syncing program to backend. State program ID: %u, Backend ID: %u", src.GetExternalIndex(),
                    m_backendProgramId);
            // Every link-derived cache below (incl. m_samplerUniformBindings and its
            // lastAssignedUnit/lastAssignedLodBias program-state mirrors) is rebuilt;
            // the sampler-pass memo keyed on them must not survive.
            m_samplerPassMemo.valid = false;
            m_backendProgramUsable = true;
            m_snormFallbackClampOutputMask = g_snormFallbackClampOutputMask;
            m_unormFallbackClampOutputMask = g_unormFallbackClampOutputMask;
            m_fragColorBroadcastCount = g_fragColorBroadcastCount;
            // The generated ESSL bakes these in (see the SetShaderStorageBlockBinding call in the
            // transpile loop below), so the set they were generated against is part of what makes
            // this build current - the draw path compares the signature and rebuilds on a change.
            const auto& storageBlockBindingOverrides = src.GetShaderStorageBlockBindingOverrides();
            m_shaderStorageBlockBindingSignature = ComputeShaderStorageBlockBindingSignature(src);
            // Rebuilt by the transpile loop below, one entry per atomic-counter block; the top is
            // snapshotted so every stage and the draw path resolve one slot per GL binding.
            m_atomicCounterGlBindings.clear();
            m_atomicCounterEsslBindingTop = AtomicCounterEsslBindingTop();
            // Re-established by AttachPassthroughTessControlStage below when this program needs
            // one; cleared first so a program that stops needing one (a relink that now attaches
            // a real control stage) does not keep comparing against a stale patch size. The
            // default levels are re-established from the same call and gated on the same -1.
            m_passthroughTessControlPatchVertices = -1;
            m_passthroughTessControlOuterLevel = FloatVec4(1.0f, 1.0f, 1.0f, 1.0f);
            m_passthroughTessControlInnerLevel = FloatVec2(1.0f, 1.0f);
            // The same shape again for image FORMATS: what a format-less image declaration
            // compiles to depends on live glBindImageTexture state, so the pairs it was built
            // against are recorded here and compared per draw (ImageUnitFormatsStillMatch).
            // Taken BEFORE the transpile loop so both the bake and the key see one snapshot.
            const ImageFormatBakeInputs imageFormatBake = CollectImageFormatBakeInputs(src);
            m_formatlessImageUnits = imageFormatBake.units;
            m_imageUnitFormatSignature = imageFormatBake.signature;
            for (const auto& conflicted : imageFormatBake.conflictedNames) {
                MGLOG_D("Image uniform '%s' of program %u declares no format and its elements address units with "
                        "different bound formats; left format-less.",
                        conflicted.c_str(), src.GetExternalIndex());
            }
            // ...and once more for image ARRAYS whose per-element units are not consecutive, which
            // ESSL has no way to express in one declaration. Program-wide, like the bake, and read
            // from the same snapshot of the reflection; the per-stage rewrite happens below.
            const Vector<ImageArrayUnitPlan> nonConsecutiveImageArrays = CollectNonConsecutiveImageArrayPlans(src);

            // Detach all existing shaders
            GLint attachedCount = 0;
            g_GLESFuncs.glGetProgramiv(m_backendProgramId, GL_ATTACHED_SHADERS, &attachedCount);
            MGLOG_D("Currently attached shaders count: %d", attachedCount);

            if (attachedCount > 0) {
                Vector<GLuint> attachedShaders(attachedCount);
                // Every GL out-param in this function is pre-initialized and every count is
                // re-clamped after the query. A driver that returns without writing the
                // out-param (no current context, a lost context, a stubbed entry point) would
                // otherwise leak an uninitialized stack value straight into a container size
                // or a loop bound - which is exactly how this path used to throw
                // length_error out of a Vector fill-ctor.
                GLsizei actualCount = 0;
                g_GLESFuncs.glGetAttachedShaders(m_backendProgramId, attachedCount, &actualCount,
                                                 attachedShaders.data());
                actualCount = std::clamp<GLsizei>(actualCount, 0, static_cast<GLsizei>(attachedShaders.size()));
                MGLOG_D("Detaching %d existing shaders from program %u", actualCount, m_backendProgramId);

                for (GLsizei i = 0; i < actualCount; ++i) {
                    MGLOG_D("Detaching shader ID: %u from program %u", attachedShaders[i], m_backendProgramId);
                    g_GLESFuncs.glDetachShader(m_backendProgramId, attachedShaders[i]);
                }
            }

            // Attach current shaders.
            //
            // The EXECUTABLE's stage list, not GetAttachedShaders(): this loop indexes
            // shaderSpirvs by the same running index, and the generated SPIR-V is a link
            // artifact while the attach list is live. GL 4.6 core 7.3 makes glAttachShader take
            // effect only at the next link, so a program that is attached to after it linked has
            // MORE entries in the attach list than there are modules - and pairing the two read
            // straight off the end of shaderSpirvs (a std::vector copy from garbage, which is
            // how this crashed). GetLinkedShaderStages() is the list the modules were generated
            // from, one entry per module, in module order.
            const Vector<ShaderStage> linkedStages = src.GetLinkedShaderStages();
            auto& shaderSpirvs = src.GetGeneratedSpirv();
            // Both come from the same Link(), so they agree by construction. If they ever did
            // not there would be no index this function could safely use for EITHER array, so
            // this refuses the build instead of picking one and hoping.
            if (linkedStages.size() != shaderSpirvs.size()) {
                MGLOG_E_ONCE("Program %u: %zu linked stage(s) but %zu generated SPIR-V module(s); refusing to "
                             "build a backend program from mismatched link artifacts.",
                             src.GetExternalIndex(), linkedStages.size(), shaderSpirvs.size());
                m_backendProgramUsable = false;
                return;
            }
            if (src.PointSizeDemoted()) {
                // THE ARMING SIGNAL, INFO on purpose and latched: the integration lane that
                // pins MOBILEGL_POINT_SIZE_DEMOTION=1 asserts on exactly this line, because
                // every rendering assertion stays green on a healthy driver whether the
                // demotion ran or was silently disarmed. See PointSizeDemotionScenario.
                MGLOG_I_ONCE("DirectGLES is building programs whose tessellation/geometry gl_PointSize was "
                             "demoted to an ordinary varying, because this driver cannot host the built-in "
                             "in those stages.");
            }
            MGLOG_D("Attaching %zu shaders to program %u", linkedStages.size(), m_backendProgramId);
            for (const auto& ref : src.GetLinkedShaderSnapshot()) {
                if (!ref.shader) continue;
                const auto& stage =
                    MG_Util::ConvertGLEnumToString(MG_Util::ConvertShaderStageToGLEnum(ref.shader->GetShaderStage()));
                // The source THIS link consumed, which a later glShaderSource does not disturb.
                const String& src = ref.source ? *ref.source : ref.shader->GetShaderSource();
                MGLOG_D("Original src @ %s: \n", stage.c_str());
                MGLOG_D("%s:", src.empty() ? "" : src.c_str());
            }
            const Bool enableSpirvValidation = src.GetSpirvValidationEnabled();

            // Blocks a transform-feedback capture request names a member of ("StageData" of
            // "StageData.attrib[0]"). The Adreno ES driver accepts such a request, links, and
            // then captures nothing at all for it, so those blocks - and ONLY those - get
            // flattened into per-member variables below, in EVERY stage, so a producer and its
            // consumer keep matching. gl_PerVertex members ("gl_Position") carry no block
            // prefix and so never enter this set.
            std::set<String> xfbCaptureBlockNames;
            for (const auto& xfbVarying : src.GetTransformFeedbackVaryings()) {
                const SizeT dot = xfbVarying.name.find('.');
                if (dot != String::npos && dot > 0) {
                    xfbCaptureBlockNames.insert(xfbVarying.name.substr(0, dot));
                }
            }
            std::set<String> flattenedXfbBlockNames;
            // Stages whose ESSL had a read+write image declaration doubled into a coherent
            // read/write pair, and by how many. Empty for every program but a handful; consulted
            // ONLY when the link then fails, because the doubling spends the driver's per-stage
            // GL_MAX_*_IMAGE_UNIFORMS budget that MobileGL keeps advertising unadjusted (halving
            // the advertised value would fail basic-api and NotSupported-out cases that never pay
            // the doubling, so the limit must stay honest and the connection has to be made here
            // instead). See the budget note on SplitReadWriteImageUniforms.
            struct SplitImageUniformStage {
                ShaderStage stage;
                Uint splitCount;
            };
            Vector<SplitImageUniformStage> splitImageUniformStages;

            // Desktop GLSL keeps SEPARATE name namespaces for input and output interface
            // blocks, so ONE stage may legally declare `in FOO {...}` and `out FOO {...}` at
            // the same time - which the tessellation evaluation stage of both interface-block
            // tests in KHR-GL42/43.shading_language_420pack does ("in TCSOutputBlock ... out
            // TCSOutputBlock"). SPIRV-Cross keeps the same split (block_input_names vs
            // block_output_names) and re-emits BOTH under the name FOO, so the generated ESSL
            // declares two different blocks called FOO in one shader. Adreno's ES compiler
            // keeps them apart; Mali's does not - the stage compiles, the program links, and
            // the output block's payload never reaches the next stage. All 22 of that group's
            // Mali failures are exactly the two tests that write this shape, and every one of
            // them passes on Adreno and on DirectVulkan.
            //
            // The repair is a rename keyed on the PRODUCING stage, planned here and applied
            // per stage below so a producer and its consumer keep naming the same block.
            // Gated twice over, because a re-serialized module is not free (it cost the
            // create-indirect retrace 0.15 SSIM the first time the array-input split missed
            // its gate; the case is now
            // minecraft-1.21.1-neoforge-create-indirect-in-world-align1024): only a tessellation
            // or geometry stage can declare blocks in both directions at all, and even then the
            // probe has to FIND a collision before any
            // stage is rewritten.
            std::set<String> collidingIoBlockNames;
            std::set<String> declaredIoBlockNames;
            Vector<Int> stagePipelineIndices(linkedStages.size(), -1);
            Bool anyStageCanDeclareBlocksInBothDirections = false;
            for (SizeT index = 0; index < linkedStages.size(); ++index) {
                const ShaderStage stage = linkedStages[index];
                stagePipelineIndices[index] = InterStagePipelineIndex(stage);
                if (CanDeclareBlocksInBothDirections(stage)) anyStageCanDeclareBlocksInBothDirections = true;
            }
            // A SECOND, INDEPENDENT interface-block repair riding the same gate, because it
            // needs the same question answered: "does this program have a stage where an
            // inter-stage block can go wrong?". CanDeclareBlocksInBothDirections is true for
            // exactly the tessellation and geometry stages, which is also exactly the set of
            // stages whose presence makes this driver drop a LOCATED block's payload (a
            // vertex-to-fragment located block is fine on the same driver, measured). The two
            // repairs are otherwise unrelated: the rename fixes a name collision inside ONE
            // stage, this drops a qualifier from EVERY block of the program - so it does not
            // wait for the collision probe to find anything.
            const Bool ioBlockLocationStripArmed =
                !g_GLESCapabilities.SupportsLocatedInterStageIoBlocks && anyStageCanDeclareBlocksInBothDirections;
            if (anyStageCanDeclareBlocksInBothDirections) {
                for (SizeT index = 0; index < shaderSpirvs.size(); ++index) {
                    MG_Util::ShaderTranspiler::ShaderCompiler::ProbeIoBlockNamesForEssl(
                        shaderSpirvs[index], collidingIoBlockNames, declaredIoBlockNames);
                }
                // A block a capture request names is resolved BY NAME at
                // glTransformFeedbackVaryings time - and flattened away entirely by the pass
                // below - so renaming one would ask the driver for a block the request does
                // not spell.
                for (const auto& xfbCaptureBlockName : xfbCaptureBlockNames) {
                    collidingIoBlockNames.erase(xfbCaptureBlockName);
                }
            }
            // The one spelling every stage of THIS program agrees on for `blockName` as written
            // by pipeline stage `producerPipelineIndex`. "__" is reserved in GLSL, so a name
            // already ending in '_' does not get another one, and the digit-suffix loop steps
            // off any name the program already spells.
            const auto uniqueIoBlockName = [&declaredIoBlockNames](const String& blockName, Int producerPipelineIndex) {
                const char* separator = (!blockName.empty() && blockName.back() == '_') ? "" : "_";
                String candidate = blockName + separator + "mgio" + std::to_string(producerPipelineIndex);
                while (declaredIoBlockNames.find(candidate) != declaredIoBlockNames.end()) {
                    candidate += "0";
                }
                return candidate;
            };

            // Desktop GL makes the tessellation CONTROL stage optional; OpenGL ES 3.2 rejects a
            // program that has an evaluation stage without one, with an empty info log. When that
            // is this program's shape, one is synthesized below - and it has to be spelled to
            // MATCH the two stages it sits between, so their emitted ESSL is kept here as it is
            // produced. Empty for every program that has a control stage of its own, which is
            // all but a handful.
            Bool hasTessEvalStage = false;
            Bool hasTessControlStage = false;
            Int tessEvalShaderIndex = -1;
            String vertexStageEssl;
            String tessEvalStageEssl;
            //
            // Asked of the executable for the same reason the loop below indexes it: a
            // tessellation evaluation shader merely ATTACHED to a linked vertex+fragment program
            // is not part of what this program runs, and synthesizing a control stage for it
            // would build a tessellating driver program for an executable that does not
            // tessellate (and would take tessEvalShaderIndex past the end of shaderSpirvs).
            for (SizeT index = 0; index < linkedStages.size(); ++index) {
                const ShaderStage stage = linkedStages[index];
                if (stage == ShaderStage::TessControl) hasTessControlStage = true;
                if (stage == ShaderStage::TessEval) {
                    hasTessEvalStage = true;
                    tessEvalShaderIndex = static_cast<Int>(index);
                }
            }
            const Bool needsPassthroughTessControl = hasTessEvalStage && !hasTessControlStage;

            // The stage order the loop below walks, with every FRAGMENT stage moved to the end.
            // The viewport-routing gate is the reason: whether a fragment stage needs one is a
            // question about the OTHER stages ("does any of them still write gl_ViewportIndex?"),
            // and the honest, free answer to it is the promotion the producing stage's own text
            // pass just performed. Answering it any other way costs a BuildModule per
            // pre-rasterization stage of every program - the parse the shared SpirvGateFeatures
            // probe exists to avoid. Nothing else in the loop is order-sensitive: the two
            // passthrough-tessellation sources it captures are a vertex and an evaluation stage,
            // and the three sets it accumulates are unions.
            //
            // The pre-rasterization stages are walked in PIPELINE order (the stage enum's), so a
            // consumer's input-block redeclaration below can mirror the output block its producer
            // has already emitted.
            Vector<SizeT> stageOrder;
            stageOrder.reserve(linkedStages.size());
            for (SizeT index = 0; index < linkedStages.size(); ++index) {
                if (linkedStages[index] != ShaderStage::Fragment) stageOrder.push_back(index);
            }
            std::stable_sort(stageOrder.begin(), stageOrder.end(), [&linkedStages](SizeT a, SizeT b) {
                return static_cast<Int>(linkedStages[a]) < static_cast<Int>(linkedStages[b]);
            });
            for (SizeT index = 0; index < linkedStages.size(); ++index) {
                if (linkedStages[index] == ShaderStage::Fragment) stageOrder.push_back(index);
            }
            // The output gl_PerVertex members the last pre-rasterization stage emitted, for the
            // next stage's input redeclaration (RedeclareClipCullDistanceInputsInPerVertexBlock).
            // A synthesized pass-through control stage copies its input block through, so the
            // vertex stage's members are what an evaluation stage behind one sees.
            String producerOutPerVertexMembers;
            // Set by whichever pre-rasterization stage's demoted mg_ViewportIndex global the text
            // pass turned into a varying; read by the fragment stage to decide whether to inject
            // the gate that consumes it.
            Bool programRoutesViewportIndex = false;
            // No fragment stage, no gate - and without a gate the promotion below would only add
            // an output nothing can read. That is not merely useless: in a separable program
            // pipeline the fragment stage lives in a DIFFERENT program, which never saw this
            // build and cannot be given a gate, so promoting there would hang an unmatched
            // varying off a program to buy nothing. Both cases keep the pre-emulation behavior,
            // which is what a program with no fragment stage had anyway.
            const Bool programHasFragmentStage =
                std::find(linkedStages.begin(), linkedStages.end(), ShaderStage::Fragment) != linkedStages.end();
            const Bool viewportEmulationForThisProgram = ViewportArrayEmulationEnabled() && programHasFragmentStage;

            for (const SizeT index : stageOrder) {
                GLenum glShaderType = MG_Util::ConvertShaderStageToGLEnum(linkedStages[index]);
                GLuint backendShaderId = g_GLESFuncs.glCreateShader(glShaderType);

                if (backendShaderId == 0) {
                    MGLOG_E_ONCE("Failed to create backend shader for attachment.");
                    continue;
                }
                String source;
                auto& spirvCode = shaderSpirvs[index];

                // A samplerBuffer is core in the OpenGL 3.1+ context MobileGL advertises but needs
                // ES 3.2 or EXT/OES_texture_buffer on the host. Without it SPIRV-Cross emits
                // `#extension GL_EXT_texture_buffer : require` and the driver rejects both that
                // and the isamplerBuffer keyword - the program never links and every draw using it
                // becomes a silent no-op. Say so here, naming the stage. Deliberately unlatched:
                // this is bounded by program count, and which stage failed is the whole point.
                // Gated on the capability so the module walk never runs on a healthy driver.
                if (!AreBufferTexturesSupported() &&
                    MG_Util::ShaderTranspiler::ShaderCompiler::ModuleDeclaresBufferTextureSampler(spirvCode)) {
                    MGLOG_E("Program %u stage %s samples a buffer texture, which this ES driver "
                            "cannot provide (%s). The shader will not compile and the program will "
                            "not link; every draw using it is a no-op.",
                            m_backendProgramId, MG_Util::ConvertGLEnumToString(glShaderType).c_str(),
                            GetBufferTextureTierName());
                    m_backendProgramUsable = false;
                    g_GLESFuncs.glDeleteShader(backendShaderId);
                    continue;
                }

                // ---- L2 of the shader translation memo -------------------------------
                // The whole DirectGLES SPIR-V pass chain plus SPIRV-Cross for this stage,
                // memoized on the module bytes and on every capability bit and per-program
                // input that steers them. See TranslationCache.h for the key inventory and
                // for why the text-level passes below stay outside the boundary.
                MG_Util::ShaderTranspiler::EsslTranslationKeyInputs esslKeyInputs;
                esslKeyInputs.spirv = &spirvCode;
                esslKeyInputs.shaderType = glShaderType;
                // The EFFECTIVE arming, computed the same way TranspileSpirvToEssl computes it.
                // Duplicated rather than shared because the two live on opposite sides of the
                // memo boundary - and a key that disagrees with the pass it is keying is the one
                // failure mode of this cache that renders wrong pixels instead of being slow.
                esslKeyInputs.viewportIndexLoweringArmed =
                    (glShaderType == GL_VERTEX_SHADER || glShaderType == GL_TESS_EVALUATION_SHADER ||
                     glShaderType == GL_GEOMETRY_SHADER) &&
                    (ViewportArrayEmulationEnabled() || !g_GLESCapabilities.SupportsViewportArray);
                esslKeyInputs.deferGeometryEmitsArmed =
                    glShaderType == GL_GEOMETRY_SHADER && g_GLESCapabilities.DropsGeometryWritesAfterEmit;
                esslKeyInputs.subgroupEmulationArmed =
                    glShaderType == GL_COMPUTE_SHADER &&
                    ResolveEsprytSubgroupImplementation(g_GLESCapabilities) == SubgroupImplementation::Emulated;
                esslKeyInputs.subgroupEmulationScratchBytes =
                    esslKeyInputs.subgroupEmulationArmed ? g_GLESCapabilities.MaxComputeSharedMemorySize : 0;
                esslKeyInputs.iterationRPBarrierFixArmed =
                    esslKeyInputs.subgroupEmulationArmed && ShouldApplyIterationRPFixes();
                esslKeyInputs.subgroupScratchAtomicReads =
                    esslKeyInputs.subgroupEmulationArmed && g_GLESCapabilities.SharedMemoryLoadsGoStale;
                esslKeyInputs.supportsNoperspectiveInterpolation =
                    g_GLESCapabilities.SupportsNoperspectiveInterpolation;
                esslKeyInputs.supportsExtendedImageFormats = g_GLESCapabilities.SupportsExtendedImageFormats;
                esslKeyInputs.maxColorTextureSamples = g_GLESCapabilities.MaxColorTextureSamples;
                esslKeyInputs.maxIntegerSamples = g_GLESCapabilities.MaxIntegerSamples;
                esslKeyInputs.maxDepthTextureSamples = g_GLESCapabilities.MaxDepthTextureSamples;
                esslKeyInputs.advertisedMaxSamples = std::max(g_GLESCapabilities.MaxSamples, kFrontendMaxSamples);
                esslKeyInputs.xfbCaptureBlockNames = &xfbCaptureBlockNames;
                esslKeyInputs.glFormatByUniformName = &imageFormatBake.glFormatByUniformName;
                esslKeyInputs.storageBlockBindingOverrides = &storageBlockBindingOverrides;
                esslKeyInputs.esslVersion = ResolveBackendEsslVersion();
                esslKeyInputs.atomicCounterEsslBindingTop = m_atomicCounterEsslBindingTop;

                // THIS STAGE's share of the program-wide interface-block rename plan built above
                // the loop. Resolved here, outside the memoized segment, because it is planning
                // and not translation - exactly like the image-format bake map - and because that
                // makes the two maps a plain function argument the L2 key can carry.
                //
                // KEYING ON THE RESOLVED MAPS rather than on what they were derived from
                // (collidingIoBlockNames, declaredIoBlockNames, stagePipelineIndices, this
                // stage's index) is deliberate: the maps ARE the pass's arguments, so they are
                // exactly as fine as the pass's behavior and no finer. Two programs whose
                // collision plans differ but whose maps for THIS stage come out identical really
                // do produce the same ESSL and should share the entry.
                //
                // A block whose other end is NOT in this program is deliberately left out of the
                // plan: in a separate-shader-objects pipeline the interface it matches across
                // lives in another program that never saw this plan, and renaming one side of
                // THAT would break a program pipeline to repair a driver quirk. That is what the
                // producer/consumer presence tests below are for - in a monolithic program both
                // are trivially satisfied for every interface the collision can touch.
                std::map<String, String> inputBlockRenames;
                std::map<String, String> outputBlockRenames;
                if (!collidingIoBlockNames.empty() && stagePipelineIndices[index] >= 0) {
                    const Int myPipelineIndex = stagePipelineIndices[index];
                    Int producerPipelineIndex = -1;
                    Bool hasConsumerStage = false;
                    for (const Int otherPipelineIndex : stagePipelineIndices) {
                        if (otherPipelineIndex < 0) continue;
                        if (otherPipelineIndex < myPipelineIndex && otherPipelineIndex > producerPipelineIndex) {
                            producerPipelineIndex = otherPipelineIndex;
                        }
                        if (otherPipelineIndex > myPipelineIndex) hasConsumerStage = true;
                    }
                    for (const auto& collidingBlockName : collidingIoBlockNames) {
                        if (producerPipelineIndex >= 0) {
                            inputBlockRenames[collidingBlockName] =
                                uniqueIoBlockName(collidingBlockName, producerPipelineIndex);
                        }
                        if (hasConsumerStage) {
                            outputBlockRenames[collidingBlockName] =
                                uniqueIoBlockName(collidingBlockName, myPipelineIndex);
                        }
                    }
                }
                esslKeyInputs.inputBlockRenames = &inputBlockRenames;
                esslKeyInputs.outputBlockRenames = &outputBlockRenames;

                // ...and THIS STAGE's share of the interface-block LOCATION strip, planned the
                // same way and for the same reason. The gate has three parts, all of which have
                // to hold before a single block loses its qualifier:
                //   * the driver POST measured the defect (never a renderer-string quirk list);
                //   * this program has a stage that can hit it - a located block between a
                //     vertex and a fragment stage works on the affected driver, so a program
                //     with neither tessellation nor geometry keeps its ESSL byte for byte;
                //   * for THIS stage and THIS direction, this program HAS a stage on that side
                //     of it. That is the same test the rename plan above makes, and the same
                //     approximation: it asks "is some stage of this program earlier/later than
                //     me", not "is the exact partner of every one of my blocks here". The two
                //     coincide for every program MobileGL builds, because a separable pipeline
                //     is flattened into one composite carrying every stage that has a shader
                //     (GLContext::GetProgramForDraw) and a program bound with glUseProgram has
                //     no partner program at all - so a stage set with a gap in it does not
                //     arise. Should one ever arise, this must become the nearest-stage
                //     resolution the rename plan computes, or the two ends of the gap would
                //     disagree about the qualifier.
                // The direction tests deliberately mirror that plan rather than inventing a
                // second rule for the same question.
                Bool stripInputBlockLocations = false;
                Bool stripOutputBlockLocations = false;
                if (ioBlockLocationStripArmed && stagePipelineIndices[index] >= 0) {
                    const Int myPipelineIndex = stagePipelineIndices[index];
                    for (const Int otherPipelineIndex : stagePipelineIndices) {
                        if (otherPipelineIndex < 0) continue;
                        if (otherPipelineIndex < myPipelineIndex) stripInputBlockLocations = true;
                        if (otherPipelineIndex > myPipelineIndex) stripOutputBlockLocations = true;
                    }
                }
                esslKeyInputs.stripInputBlockLocations = stripInputBlockLocations;
                esslKeyInputs.stripOutputBlockLocations = stripOutputBlockLocations;
                esslKeyInputs.enableSpirvValidation = enableSpirvValidation;
                esslKeyInputs.narrowIntegerSeedsArmed = glShaderType == GL_VERTEX_SHADER && m_narrowIntegerSeeds.built;

                auto& esslCache = MG_Util::ShaderTranspiler::GetEsslTranslationCache();
                MG_Util::ShaderTranspiler::TranslationCacheKey esslCacheKey;
                if (MG_Util::ShaderTranspiler::ShaderTranslationCacheEnabled()) {
                    esslCacheKey = MG_Util::ShaderTranspiler::BuildEsslTranslationKey(esslKeyInputs);
                }

                std::set<String> stageFlattenedXfbBlockNames;
                // Per stage, and NOT m_atomicCounterGlBindings directly: on a miss the
                // transpile appends to this, on a hit the payload supplies it, and only then
                // is it folded into the program-wide vector. Pointing the transpile straight
                // at the member would have made the miss path and the hit path disagree about
                // who owns the append.
                Vector<Int> stageAtomicCounterGlBindings;
                const MG_Util::ShaderTranspiler::EsslTranslationResultPtr esslHit =
                    esslCacheKey.Valid() ? esslCache.Find(esslCacheKey) : nullptr;
                if (esslHit) {
                    source = esslHit->essl;
                    stageFlattenedXfbBlockNames = esslHit->flattenedXfbBlockNames;
                    stageAtomicCounterGlBindings = esslHit->atomicCounterGlBindings;
                } else {
                    String transpileError;
                    if (!TranspileSpirvToEssl(spirvCode, glShaderType, xfbCaptureBlockNames, imageFormatBake,
                                              storageBlockBindingOverrides, inputBlockRenames, outputBlockRenames,
                                              stripInputBlockLocations, stripOutputBlockLocations,
                                              m_atomicCounterEsslBindingTop, esslKeyInputs.narrowIntegerSeedsArmed,
                                              enableSpirvValidation, source, stageFlattenedXfbBlockNames,
                                              stageAtomicCounterGlBindings, transpileError)) {
                        // MGLOG_E, unlatched, like the compile- and link-failure diagnostics
                        // below: one line per failing stage is bounded by program count and
                        // naming the stage is the entire diagnostic value. A stage that never
                        // reaches the driver leaves the program short of that stage, so the link
                        // fails with an EMPTY driver info log - the least debuggable failure
                        // MobileGL can produce, and what hid the whole
                        // KHR-GL43.vertex_attrib_binding family behind "the draw captured zeros".
                        MGLOG_E("Shader transpilation to ESSL failed. State program ID: %u, stage: %s, "
                                "SPIRV-Cross error: %s",
                                src.GetExternalIndex(), MG_Util::ConvertGLEnumToString(glShaderType).c_str(),
                                transpileError.c_str());
                        m_backendProgramUsable = false;
                        continue;
                    }
                    if (esslCacheKey.Valid()) {
                        auto payload = MakeShared<MG_Util::ShaderTranspiler::EsslTranslationResult>();
                        payload->essl = source;
                        payload->flattenedXfbBlockNames = stageFlattenedXfbBlockNames;
                        payload->atomicCounterGlBindings = stageAtomicCounterGlBindings;
                        const SizeT payloadBytes = MG_Util::ShaderTranspiler::EsslTranslationResultBytes(*payload);
                        esslCache.Insert(esslCacheKey,
                                         MG_Util::ShaderTranspiler::EsslTranslationResultPtr(Move(payload)),
                                         payloadBytes);
                    }
                }
                // Per stage, never cumulative: a fragment shader consuming the same block
                // reports a name the vertex stage already reported, and its own rewrite must
                // still be taken or the two stages stop matching. Done here rather than inside
                // the transpile so a cache HIT contributes its names too.
                flattenedXfbBlockNames.insert(stageFlattenedXfbBlockNames.begin(), stageFlattenedXfbBlockNames.end());
                // Same rule for the atomic-counter bindings this stage declared, and for the
                // same reason: the loop below de-duplicates across stages, so a hit that
                // contributed nothing would silently drop a counter buffer the draw path has
                // to bind.
                m_atomicCounterGlBindings.insert(m_atomicCounterGlBindings.end(), stageAtomicCounterGlBindings.begin(),
                                                 stageAtomicCounterGlBindings.end());

                // SPIRV-Cross sizes written clip/cull distances with a standalone
                // `out float gl_ClipDistance[N];`, which an ES compiler may refuse as a reserved
                // name and so lose the whole program. Redeclared as gl_PerVertex members instead.
                // BEFORE the gl_PointSize extension probe below (the block can name it) and
                // before vertexStageEssl is captured, so a synthesized pass-through control
                // stage mirrors the block this produces.
                source = RedeclareClipCullDistancesInPerVertexBlock(std::move(source), glShaderType);
                // ...and the consumer half: a stage reading gl_in[i].gl_ClipDistance/CullDistance
                // redeclares its input block with the sizes the producer wrote.
                source = RedeclareClipCullDistanceInputsInPerVertexBlock(std::move(source), glShaderType,
                                                                         producerOutPerVertexMembers);
                if (glShaderType != GL_FRAGMENT_SHADER) {
                    producerOutPerVertexMembers =
                        ExtractPerVertexBlockMembers(source, /*input=*/false).value_or(String());
                }

                // Position in the chain is arbitrary: this is the only header-level rewrite, it
                // edits #extension directives and never the body, and the replacement is the
                // same length and stays an #extension line - so it commutes with every pass
                // below, including ForceSupporterOutput's scan for the last directive. First,
                // because a header concern reads better before the body ones.
                source = RetargetTextureBufferExtension(std::move(source), g_GLESCapabilities.TextureBufferSupport);
                // The other header-level rewrite, and next to that one for the same reason. The
                // formats it covers are both the ones the bake above put into the module and the
                // ones the application declared itself - either can be outside the thirteen GLSL
                // ES has in core, and neither reaches the driver without this directive.
                source =
                    RequestExtendedImageFormats(std::move(source), imageFormatBake.needsExtendedImageFormats &&
                                                                       g_GLESCapabilities.SupportsExtendedImageFormats);
                // The third header-level rewrite, for the builtin SPIRV-Cross prints bare:
                // gl_ViewportIndex is in no version of ESSL core, so without this directive the
                // stage does not compile and the whole program - not just its viewport routing -
                // is lost. The token probe keeps the line off every other program and the
                // capability gate keeps it off drivers that would hard-error on an unadvertised
                // name; a driver without the extension took the LowerViewportIndexPass fallback
                // above and its source no longer names the builtin at all, so the two are mutually
                // exclusive by construction. Read `source` BEFORE it is moved from.
                // The routing emulation is the third way this can be reached and the only one
                // that needs no directive: it renames the fragment stage's read onto the varying
                // the producing stage now writes, a few passes below.
                const Bool needsViewportArrayExtension =
                    g_GLESCapabilities.SupportsViewportArray &&
                    !(ViewportArrayEmulationEnabled() && programRoutesViewportIndex) &&
                    source.find("gl_ViewportIndex") != String::npos;
                source = RequestViewportArrayExtension(std::move(source), needsViewportArrayExtension);

                // The fourth header-level rewrite, and the same shape as the third: ESSL has no
                // gl_PointSize in a tessellation or geometry stage at ANY version - 320 makes the
                // stages core and still leaves the built-in behind EXT/OES_..._point_size - while
                // SPIRV-Cross prints it bare. Without the directive the stage fails to compile
                // with "`gl_PointSize' undeclared", which takes the whole program to program 0:
                // the draw renders nothing AND glBeginTransformFeedback is rejected, so a capture
                // of anything at all off that program silently comes back empty. The token probe
                // keeps the line off every other program and PointSizeExtensionName returns
                // nullptr - i.e. nothing is emitted - on a driver advertising neither spelling.
                if (source.find("gl_PointSize") != String::npos) {
                    const Bool tessellationStage =
                        glShaderType == GL_TESS_CONTROL_SHADER || glShaderType == GL_TESS_EVALUATION_SHADER;
                    if (tessellationStage || glShaderType == GL_GEOMETRY_SHADER) {
                        const auto tier = tessellationStage ? g_GLESCapabilities.TessellationPointSizeSupport
                                                            : g_GLESCapabilities.GeometryPointSizeSupport;
                        const char* pointSizeExtension = PointSizeExtensionName(tier, tessellationStage);
                        if (pointSizeExtension == nullptr) {
                            // Latched, and an ERROR rather than a warning: what follows is a
                            // driver compile failure whose text names a built-in the application
                            // never mis-spelled, and the reason is a missing driver capability
                            // rather than anything in the shader. Saying so here is the whole
                            // difference between a legible skip and an unexplained black draw.
                            MGLOG_E_ONCE("This driver advertises neither the EXT nor the OES %s_point_size "
                                         "extension, so its ESSL has no gl_PointSize in a %s stage; program %u "
                                         "will fail to compile. Point size from a non-vertex stage is not "
                                         "available on this device.",
                                         tessellationStage ? "tessellation" : "geometry",
                                         tessellationStage ? "tessellation" : "geometry", src.GetExternalIndex());
                        }
                        source = RequestPointSizeExtension(std::move(source), pointSizeExtension);
                    }
                }

                source = RebindImageUniformsToFrontendUnits(std::move(source), src);
                // The completion half of the format bake, for the formats SPIRV-Cross throws on
                // rather than prints (r8ui and the rest of its desktop-only set). Empty for every
                // program whose format-less images bound a format the module could carry, which
                // is the normal case - those were baked into the SPIR-V above and this pass finds
                // their declarations already qualified. AFTER the rebind, so the layout qualifier
                // it edits is the one that already exists; BEFORE the split and the binding
                // strip, so both halves of a split image inherit the format.
                source = BakeImageFormatQualifiers(std::move(source), imageFormatBake.esslFormatQualifierByUniformName);
                // An image ARRAY whose elements do not sit on consecutive units cannot be spelled
                // by the single layout(binding=N) the rebind above stamped: ESSL gives element k
                // the unit N+k and there is no glUniform1i to correct it with. Split the array
                // into one scalar declaration per element, each with its own binding. AFTER the
                // rebind and the format bake, both of which look the array up by its GL uniform
                // name and need the binding already there; BEFORE the read+write split, so an
                // element that is both read and written is split with its own binding on it.
                if (!nonConsecutiveImageArrays.empty()) {
                    Vector<String> declinedImageArrays;
                    source = RemapImageArrayElementUnits(source, nonConsecutiveImageArrays, &declinedImageArrays);
                    for (const auto& declined : declinedImageArrays) {
                        // MGLOG_E, unlatched, like the transpile- and compile-failure diagnostics
                        // around it: this is the "linked, drew, produced wrong numbers, said
                        // nothing" shape that cost earlier waves whole days, and one line per
                        // declined array is bounded by program count. There is no honest GL answer
                        // to give instead - the frontend has already reported LINK_STATUS = true.
                        MGLOG_E("Image array %s. Its elements address image units GLSL ES cannot be made to reach "
                                "from one declaration, so this stage will read and write the WRONG units. State "
                                "program ID: %u, stage: %s.",
                                declined.c_str(), src.GetExternalIndex(),
                                MG_Util::ConvertGLEnumToString(glShaderType).c_str());
                    }
                }
                // Wedged between those two on purpose:
                //  * AFTER RebindImageUniformsToFrontendUnits, so the binding it copies onto
                //    both halves of a split image is already the frontend texture unit (and so
                //    that pass never has to reason about the alias it introduces);
                //  * BEFORE RemoveLayoutBinding, whose keepBindingRegex recognizes an image
                //    declaration and preserves its binding - an image unit cannot be set from
                //    the API in ES, so the qualifier is the only binding mechanism there is,
                //    and both halves of the pair have to still be carrying theirs when it runs.
                // Takes no stage: the qualifier it adds is a decision about THIS text's accesses
                // and the rename that keeps two stages from declaring one image uniform
                // differently is keyed on that same decision, so two stages that agree still
                // share one uniform (see the location-budget note on the pass).
                Uint splitImageUniformCount = 0;
                source = SplitReadWriteImageUniforms(source, &splitImageUniformCount);
                if (splitImageUniformCount != 0) {
                    splitImageUniformStages.push_back({linkedStages[index], splitImageUniformCount});
                }
                source = RemoveLayoutBinding(source);
                source = ProcessOutColorLocations(source);
                source = ForceFlatIntegerVaryings(source, glShaderType);
                source = BroadcastLegacyFragColor(std::move(source), glShaderType, m_fragColorBroadcastCount);
                source = LowerZeroGradCubeArrayShadowLookups(source, glShaderType);
                source = EmulateTextureLodBias(source, ShouldAvoidExplicitLodBiasOnAngleLlvmpipe());
                if (ClampsUnsignedMultisampleFetchesAtRuntime()) {
                    source = ClampUnsignedMultisampleFetches(source);
                }
                source = EmulateBaseInstanceInVertexShader(std::move(source), glShaderType);
                source = PromoteDrawParameterGlobalsToUniforms(std::move(source), glShaderType);
                // The two halves of the gl_ViewportIndex routing emulation, next to the draw-
                // parameter promotion because they are the same shape: a builtin ESSL cannot
                // spell, demoted to a plain global by a SPIR-V pass, given a real interface here.
                // BEFORE ForceSupporterOutput, so the `precision highp` statements it hoists to
                // the top land above the declarations these inject; AFTER
                // ForceFlatIntegerVaryings, which matches only declarations carrying a
                // layout(...) qualifier and so cannot touch either of them.
                if (viewportEmulationForThisProgram) {
                    if (glShaderType == GL_FRAGMENT_SHADER) {
                        if (programRoutesViewportIndex && !InjectViewportIndexPassGate(source)) {
                            // MGLOG_E, unlatched, like the transpile- and compile-failure
                            // diagnostics around it: the program still links and still draws, so
                            // nothing else in the process will ever say that its viewport routing
                            // silently collapsed back to one rectangle.
                            MGLOG_E("Program %u routes gl_ViewportIndex but its fragment stage has no "
                                    "entry point to gate, so the routing cannot be emulated: every "
                                    "index will rasterize against viewport 0. State program ID: %u.",
                                    m_backendProgramId, src.GetExternalIndex());
                        }
                    } else if (PromoteViewportIndexGlobalToVarying(source)) {
                        programRoutesViewportIndex = true;
                    }
                }
                source = ForceSupporterOutput(source);
                source = ClampNormFallbackOutputs(std::move(source), glShaderType, m_snormFallbackClampOutputMask,
                                                  m_unormFallbackClampOutputMask);

                // Patch for Photon compiler precision issue
                String findStr = "1000000.0";
                String replaceStr = "65500.0";
                auto pos = source.find(findStr);
                while (pos != String::npos) {
                    MGLOG_D("Applying patch #2 to Photon...");
                    source.replace(pos, findStr.length(), replaceStr);
                    pos = source.find(findStr, pos);
                }

                NoteNarrowIntegerSeedSource(m_narrowIntegerSeeds, source, glShaderType);
                const char* sourceCStr = source.c_str();
                MGLOG_D("Setting shader source for backend shader ID: %u\nsrc:\n%s", backendShaderId, sourceCStr);
                g_GLESFuncs.glShaderSource(backendShaderId, 1, &sourceCStr, nullptr);
                g_GLESFuncs.glCompileShader(backendShaderId);

                // GL_FALSE, not GL_TRUE: an unwritten out-param must read as "compile failed", never
                // as a silent success that attaches an uncompiled shader.
                GLint compileStatus = GL_FALSE;
                g_GLESFuncs.glGetShaderiv(backendShaderId, GL_COMPILE_STATUS, &compileStatus);
                if (compileStatus == GL_FALSE) {
                    GLint logLength = 0;
                    g_GLESFuncs.glGetShaderiv(backendShaderId, GL_INFO_LOG_LENGTH, &logLength);
                    if (logLength < 0) logLength = 0;
                    // +1 and zero-filled: GL_INFO_LOG_LENGTH already counts the terminator,
                    // but a driver that reports 0 (or fails the query) must still leave
                    // log.data() a readable empty C string for the %s below.
                    Vector<GLchar> log(static_cast<SizeT>(logLength) + 1, '\0');
                    g_GLESFuncs.glGetShaderInfoLog(backendShaderId, logLength, nullptr, log.data());
                    log.back() = '\0';
                    // MGLOG_E, unlatched. This was parked at MGLOG_I while the level ordering
                    // compiled E and W out of every INFO build: the Android retrace artifact
                    // carried 294 INFO lines and zero ERROR lines while two generated shaders
                    // were being rejected outright, and the lane could not say why it was
                    // rendering an empty translucent layer. A shader the driver refuses is
                    // never noise.
                    //
                    // A BOUNDED EXCERPT of the source goes with it. The driver log names a line
                    // and a column in text that exists nowhere but here, so without any source at
                    // all the only way to read "`gl_PointSize' undeclared" is to rebuild the whole
                    // library at DEBUG - but the full dump cannot go at E either. This is not
                    // "one line per refused shader": SyncToBackend's rebuild gate keys on
                    // per-draw state (the enabled-draw-buffer count among it), so a program used
                    // across passes with different draw-buffer counts re-transpiles, re-compiles
                    // and re-fails on every alternation, i.e. per frame. At E - live at the
                    // production INFO level - each of those records would push the whole
                    // post-SPIRV-Cross ESSL through the global log mutex with a forced flush onto
                    // /sdcard/MG/latest.log, the file users are asked to share. The excerpt keeps
                    // the record O(1); the full text is still there at D, printed against this
                    // same backend shader id by the "Setting shader source" line above, so
                    // nothing needs to be dumped twice.
                    constexpr SizeT kMaxLoggedSourceBytes = 2048;
                    String truncatedSource;
                    const char* sourceForLog = source.c_str();
                    if (source.size() > kMaxLoggedSourceBytes) {
                        // Back up to a line boundary when there is one inside the window, so the
                        // excerpt ends on a whole statement rather than mid-token. Built only on
                        // this branch: a stage that fits keeps its own buffer and is not copied.
                        SizeT cut = kMaxLoggedSourceBytes;
                        if (const SizeT lastNewline = source.rfind('\n', cut);
                            lastNewline != String::npos && lastNewline > 0) {
                            cut = lastNewline + 1;
                        }
                        truncatedSource = source.substr(0, cut);
                        truncatedSource += "... [" + std::to_string(source.size() - cut) +
                                           " more bytes; the whole stage is printed at the DEBUG level]\n";
                        sourceForLog = truncatedSource.c_str();
                    }
                    MGLOG_E("Shader compilation failed. State program ID: %u, stage: %s, backend shader ID: "
                            "%u, driver log: %s\nSource:\n%s",
                            src.GetExternalIndex(), MG_Util::ConvertGLEnumToString(glShaderType).c_str(),
                            backendShaderId, log.data(), sourceForLog);
                    m_backendProgramUsable = false;
                    // Nothing will ever attach this one, so nothing else can free it.
                    g_GLESFuncs.glDeleteShader(backendShaderId);
                    continue;
                }

                MGLOG_D("Attaching shader ID: %u to program %u", backendShaderId, m_backendProgramId);
                g_GLESFuncs.glAttachShader(m_backendProgramId, backendShaderId);
                // Hand the shader's lifetime to the program, immediately and unconditionally.
                //
                // glDeleteShader only FLAGS a shader; the driver frees it when it is attached to
                // nothing. Flagging it here is what makes the program own it, so deleting the
                // program (or the detach loop above, on a relink) is what actually frees it.
                // Without this call every program build leaked its shader objects for the process
                // lifetime, and a relink leaked them twice - the detach loop above dropped the
                // program's reference to shaders nothing had flagged, so they became unreachable
                // AND undeletable. The GL swizzle conformance test builds 1,296 programs per case,
                // so a handful of cases left tens of thousands of live driver shaders behind and
                // the driver started mis-serving them (KHR-GL33/GL40.texture_swizzle.smoke_*).
                // Same class of defect as the missing framebuffer/renderbuffer/sampler destructors
                // fixed in Wave 1, and the last of that family: this is the one backend GL object
                // MobileGL creates without an owning wrapper to destroy it.
                g_GLESFuncs.glDeleteShader(backendShaderId);

                // Kept AFTER every text-level pass, so what the synthesized control stage mirrors
                // is the text the driver actually sees, not an intermediate form.
                if (needsPassthroughTessControl) {
                    if (glShaderType == GL_VERTEX_SHADER) {
                        vertexStageEssl = source;
                    } else if (glShaderType == GL_TESS_EVALUATION_SHADER) {
                        tessEvalStageEssl = source;
                    }
                }

                MGLOG_D("Processed shader source length: %zu", source.length());
            }

            if (needsPassthroughTessControl) {
                AttachPassthroughTessControlStage(src, tessEvalShaderIndex, shaderSpirvs, vertexStageEssl,
                                                  tessEvalStageEssl);
            }

            // A counter buffer declared by several stages was recorded once per stage; the draw
            // path binds per GL binding point, so collapse the duplicates here rather than
            // re-issuing the same glBindBufferBase two or three times every draw.
            if (!m_atomicCounterGlBindings.empty()) {
                std::sort(m_atomicCounterGlBindings.begin(), m_atomicCounterGlBindings.end());
                m_atomicCounterGlBindings.erase(
                    std::unique(m_atomicCounterGlBindings.begin(), m_atomicCounterGlBindings.end()),
                    m_atomicCounterGlBindings.end());
            }

            // Transform feedback capture runs on the real driver (see XfbImpl in
            // DirectGLES.cpp), so the capture set has to be declared on the backend
            // program before it links. SPIRV-Cross keeps user output names verbatim in
            // the transpiled ESSL (`out vec4 result_0;` stays `result_0`), so the
            // frontend's requested names carry over unchanged.
            SizeT declaredXfbVaryingCount = 0;
            if (src.GetTransformFeedbackVaryingCount() > 0 && g_GLESFuncs.glTransformFeedbackVaryings != nullptr) {
                const auto& xfbVaryings = src.GetTransformFeedbackVaryings();
                Vector<const GLchar*> xfbNames;
                xfbNames.reserve(xfbVaryings.size());
                // A block this build flattened no longer HAS the member the application asked
                // for; it has the variable that replaced it. Everything else - including a
                // member of a block that was left alone - keeps the application's spelling.
                // Storage first, pointers after: xfbNames holds pointers into these strings.
                //
                // Same rule for a demoted gl_PointSize: the capture stage's ESSL no longer
                // spells the built-in at all - the value lives in the carrier the demotion
                // named - so the driver-side request has to follow it there. Only when the
                // capture stage IS a demoted one (geometry, else evaluation): a program whose
                // capture stage is the vertex shader keeps the built-in and its spelling,
                // whatever happened to a control stage behind it.
                Bool captureStageDemoted = false;
                if (src.PointSizeDemoted()) {
                    for (const ShaderStage linkedStage : linkedStages) {
                        if (linkedStage == ShaderStage::TessEval || linkedStage == ShaderStage::Geometry) {
                            captureStageDemoted = true;
                            break;
                        }
                    }
                }
                Vector<String> rewrittenXfbNames(xfbVaryings.size());
                for (SizeT nameIndex = 0; nameIndex < xfbVaryings.size(); ++nameIndex) {
                    String flatName;
                    if (!flattenedXfbBlockNames.empty() &&
                        MG_Util::ShaderTranspiler::ShaderCompiler::RewriteXfbCaptureNameForFlattenedBlock(
                            xfbVaryings[nameIndex].name, flattenedXfbBlockNames, flatName)) {
                        rewrittenXfbNames[nameIndex] = std::move(flatName);
                    } else if (captureStageDemoted && xfbVaryings[nameIndex].name == "gl_PointSize") {
                        rewrittenXfbNames[nameIndex] =
                            MG_Util::ShaderTranspiler::ShaderCompiler::POINT_SIZE_CAPTURE_CARRIER_NAME;
                    } else {
                        rewrittenXfbNames[nameIndex] = xfbVaryings[nameIndex].name;
                    }
                }
                for (const auto& xfbName : rewrittenXfbNames) {
                    xfbNames.push_back(xfbName.c_str());
                }
                MGLOG_D("Declaring %zu transform feedback varyings on program %u", xfbNames.size(), m_backendProgramId);
                // Bounded: a lost context never answers GL_NO_ERROR, and this runs on the
                // thread that would then spin forever.
                for (Int i = 0; i < kMaxDrainedProgramErrors && g_GLESFuncs.glGetError() != GL_NO_ERROR; ++i) {}
                g_GLESFuncs.glTransformFeedbackVaryings(m_backendProgramId, static_cast<GLsizei>(xfbNames.size()),
                                                        xfbNames.data(), src.GetTransformFeedbackBufferMode());
                // Unchecked before. A rejected capture set leaves the program linking happily
                // with NO capture set at all, and then every draw of every span records
                // nothing while the application reads its buffer's pre-draw bytes and
                // GL_NO_ERROR - the signature four conformance families were stuck on.
                if (const GLenum xfbError = g_GLESFuncs.glGetError(); xfbError != GL_NO_ERROR) {
                    String declared;
                    for (const auto& xfbName : rewrittenXfbNames) {
                        if (!declared.empty()) declared += ", ";
                        declared += xfbName;
                    }
                    MGLOG_E("The ES driver REJECTED the transform feedback capture set for backend program %u with "
                            "%s (mode %s): [%s]. Every capture made with GL program %u will record nothing.",
                            m_backendProgramId, MG_Util::ConvertGLEnumToString(xfbError).c_str(),
                            MG_Util::ConvertGLEnumToString(src.GetTransformFeedbackBufferMode()).c_str(),
                            declared.c_str(), src.GetExternalIndex());
                }
                declaredXfbVaryingCount = xfbNames.size();
            }

            // Link program
            MGLOG_D("Linking program %u", m_backendProgramId);
            g_GLESFuncs.glLinkProgram(m_backendProgramId);

            GLint linkStatus = GL_FALSE;
            g_GLESFuncs.glGetProgramiv(m_backendProgramId, GL_LINK_STATUS, &linkStatus);
            m_backendProgramUsable = m_backendProgramUsable && linkStatus == GL_TRUE;
            if (linkStatus != GL_TRUE) {
                GLint logLength = 0;
                g_GLESFuncs.glGetProgramiv(m_backendProgramId, GL_INFO_LOG_LENGTH, &logLength);
                if (logLength < 0) logLength = 0;
                Vector<GLchar> log(static_cast<SizeT>(logLength) + 1, '\0');
                g_GLESFuncs.glGetProgramInfoLog(m_backendProgramId, logLength, nullptr, log.data());
                log.back() = '\0';
                // MGLOG_I for the same reason as the compile failure above: a program that
                // links nothing no-ops every draw that uses it, and that has to be readable
                // in an INFO-level artifact.
                MGLOG_E("Program linking failed. State program ID: %u, backend program ID: %u, driver log: %s",
                        src.GetExternalIndex(), m_backendProgramId, log.data());
                // The one link failure MobileGL can name a cause for that the driver's log never
                // will: ESSL has no legal single declaration for a read+write image outside
                // r32f/r32i/r32ui, so those are split into a coherent pair and the stage ends up
                // declaring more image uniforms than the application did - against a
                // GL_MAX_*_IMAGE_UNIFORMS that is still the driver's raw number, because lowering
                // it would fail basic-api and NotSupported-out every case that only ever uses
                // readonly/writeonly images. A shader declaring more than half a stage's budget in
                // read+write images therefore links here and nowhere else, and without this line
                // the next reader has only a generic driver message to go on.
                for (const SplitImageUniformStage& split : splitImageUniformStages) {
                    MGLOG_E("...and %u read+write image uniform(s) in stage %s were split into coherent "
                            "read/write pairs, so that stage declares %u image uniform(s) more than the "
                            "program did; GL_MAX_*_IMAGE_UNIFORMS for it is %d. If the driver log names "
                            "image uniforms, that is the cause.",
                            split.splitCount,
                            MG_Util::ConvertGLEnumToString(MG_Util::ConvertShaderStageToGLEnum(split.stage)).c_str(),
                            split.splitCount, AdvertisedStageImageUniformLimit(split.stage));
                }
            } else {
                MGLOG_D("Program linked successfully. ID: %u", m_backendProgramId);
                // A link that SUCCEEDS can still have dropped the capture set: ESSL rejects a
                // requested name the transpiled shader does not actually declare by simply not
                // capturing it, and a program whose last vertex-processing stage was rewritten
                // by a SPIR-V pass (viewport-index lowering, gl_PerVertex handling, the
                // synthesized pass-through tessellation control stage) can end up spelling its
                // outputs differently from the frontend's request. Asking the driver what it
                // ACTUALLY linked is the only way to tell that apart from a driver that just
                // captures nothing - which is the whole ambiguity the empty-capture failures
                // across geometry_shader / tessellation_shader / gpu_shader5 / DSA sat on.
                if (declaredXfbVaryingCount > 0) {
                    GLint linkedXfbVaryings = 0;
                    GLint linkedXfbBufferMode = 0;
                    g_GLESFuncs.glGetProgramiv(m_backendProgramId, GL_TRANSFORM_FEEDBACK_VARYINGS, &linkedXfbVaryings);
                    g_GLESFuncs.glGetProgramiv(m_backendProgramId, GL_TRANSFORM_FEEDBACK_BUFFER_MODE,
                                               &linkedXfbBufferMode);
                    for (Int i = 0; i < kMaxDrainedProgramErrors && g_GLESFuncs.glGetError() != GL_NO_ERROR; ++i) {}
                    const GLenum requestedMode = src.GetTransformFeedbackBufferMode();
                    if (static_cast<SizeT>(std::max(linkedXfbVaryings, 0)) != declaredXfbVaryingCount ||
                        static_cast<GLenum>(linkedXfbBufferMode) != requestedMode) {
                        MGLOG_E("Backend program %u (GL program %u) linked with a capture set the driver does not "
                                "agree with: asked for %zu varying(s) in mode %s, the driver reports %d varying(s) "
                                "in mode %s. Captures made with it will be empty or wrongly laid out.",
                                m_backendProgramId, src.GetExternalIndex(), declaredXfbVaryingCount,
                                MG_Util::ConvertGLEnumToString(requestedMode).c_str(), linkedXfbVaryings,
                                MG_Util::ConvertGLEnumToString(static_cast<GLenum>(linkedXfbBufferMode)).c_str());
                    } else {
                        MGLOG_D("Backend program %u capture set confirmed by the driver: %d varying(s), mode %s",
                                m_backendProgramId, linkedXfbVaryings,
                                MG_Util::ConvertGLEnumToString(static_cast<GLenum>(linkedXfbBufferMode)).c_str());
                    }
                }
            }
            // The driver program was relinked IN PLACE, so its GL name no longer identifies
            // the executable behind it - and that name is exactly what Use()'s
            // g_lastUsedBackendProgramId early-out treats as identifying it. Without this,
            // a rebuild of the program that is already bound issues no glUseProgram at all
            // and the driver keeps running whatever the last one installed.
            //
            // GL 4.6 core 7.3 does promise that a successful re-link of a program in use
            // installs the new executable - but only "for all shader stages where the program
            // is active", and a stage the previous link did not produce is not active for
            // anything. So a relink that ADDS a stage is precisely the case the promise does
            // not cover. Verified with no MobileGL in the process (bare EGL + GLES 3.2,
            // llvmpipe 26.1.4): vertex+fragment linked, used and drawn renders; a geometry
            // shader attached and relinked reports LINK_STATUS true with an empty info log,
            // and the next draw renders NOTHING and raises no error - while the same draw
            // after a fresh glUseProgram of the same name renders again.
            //
            // A flag rather than zeroing the guard: 0 is also the id Use() binds for a build
            // that did NOT come out usable, and a zeroed guard would make it skip that
            // glUseProgram(0) and leave the failed program's previous executable running -
            // the silent wrong-shader draw Use() exists to prevent.
            m_rebindAfterRelink = true;
            m_baseInstanceUniformLocation =
                g_GLESFuncs.glGetUniformLocation(m_backendProgramId, BASE_INSTANCE_UNIFORM_NAME);
            m_drawIdUniformLocation = g_GLESFuncs.glGetUniformLocation(m_backendProgramId, DRAW_ID_UNIFORM_NAME);
            m_baseVertexUniformLocation =
                g_GLESFuncs.glGetUniformLocation(m_backendProgramId, BASE_VERTEX_UNIFORM_NAME);
            m_baseInstanceWordIndexUniformLocation =
                g_GLESFuncs.glGetUniformLocation(m_backendProgramId, BASE_INSTANCE_WORD_INDEX_UNIFORM_NAME);
            // Asked of the DRIVER rather than remembered from the injection, deliberately: the
            // gate is only real if the uniform survived compilation and linking, and this is the
            // one question whose answer covers both. A gate the driver optimized away would
            // otherwise leave the draw path replaying passes whose mask reaches nothing, which
            // renders every index's primitives in every pass.
            m_viewportPassMaskUniformLocation =
                g_GLESFuncs.glGetUniformLocation(m_backendProgramId, VIEWPORT_PASS_MASK_UNIFORM_NAME);
            if (m_viewportPassMaskUniformLocation >= 0) {
                // Sticky, and never cleared on a relink: it only ever short-circuits a per-draw
                // check, so being late to go false costs a pointer compare and being late to go
                // true would cost correctness.
                g_anyProgramRoutesViewportIndex = true;
            }
            // The mg_IndirectParams block binding is baked into the ESSL (ES cannot rebind
            // SSBO blocks after compile); record it so draws bind the indirect buffer there.
            m_indirectParamsBinding = -1;
            if (m_baseInstanceWordIndexUniformLocation >= 0 && g_GLESFuncs.glGetProgramResourceIndex) {
                const GLuint blockIndex = g_GLESFuncs.glGetProgramResourceIndex(
                    m_backendProgramId, GL_SHADER_STORAGE_BLOCK, INDIRECT_PARAMS_BLOCK_NAME);
                if (blockIndex != GL_INVALID_INDEX && g_GLESCapabilities.MaxShaderStorageBufferBindings > 0) {
                    m_indirectParamsBinding = g_GLESCapabilities.MaxShaderStorageBufferBindings - 1;
                }
            }

            // Create global UBO
            if (src.GetUBOSize() > 0) {
                g_GLESFuncs.glGenBuffers(1, &m_backendGlobalUBOId);
                g_GLESFuncs.glBindBuffer(GL_UNIFORM_BUFFER, m_backendGlobalUBOId);
                g_GLESFuncs.glBufferData(GL_UNIFORM_BUFFER, src.GetUBOSize(), nullptr, GL_STREAM_DRAW);
                g_GLESFuncs.glBindBuffer(GL_UNIFORM_BUFFER, 0);
            } else {
                m_backendGlobalUBOId = 0;
            }

            CacheResourceLocations(src);
            // NOT the mechanism that makes a rebinding work - the transpiled qualifier above is.
            // glShaderStorageBlockBinding is a GL 4.3 entry point that no real ES driver exposes,
            // so this replay is a no-op almost everywhere; it stays because it is still correct
            // (and cheaper than a rebuild) on a driver that does expose it, e.g. a desktop GL
            // driver used as the ES backend. AFTER the link either way, because it needs the
            // driver's linked interface.
            ReseedShaderStorageBlockBindings(m_backendProgramId, src);

            m_isInitialized = true;
            MGLOG_D("Program sync completed. backend ID %u", m_backendProgramId);
        }

        // P5e (pg), CONTRACT-P5E §5.5: THE BUILD THAT NAMES NO CLIENT MEMORY. Everything it
        // reads is the record: the archive create_shader_state carried, the three binding tails
        // set_program_bindings carried, and the descriptor. Nothing here resolves a frontend
        // identity, probes the client allocator or holds a SharedPtr to a frontend object - the
        // three absences rule F is made of.
        void BackendProgramObjectImpl::SyncToBackendByHandle(MG_Pipe::MGPipeHandle cso) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            const auto* record = PipeShaderCsoRecordForHandle(cso);
            if (record == nullptr) {
                MGLOG_E_ONCE("MGPipe: shader CSO {%u, %u} has no applier record, so no driver "
                             "program can be built for it",
                             cso.Slot, cso.Gen);
                return;
            }
            // THE ARCHIVE IS THE RECORD'S OR THE BUILD DOES NOT HAPPEN. A record with no archive
            // is one the transport arm never filled - a monolith create that reached a handle
            // arm - and building from an empty reflection would produce a program with no
            // uniforms and no modules, i.e. a black screen with no marker anywhere. The
            // create's own trip wire already refuses a record that declares no blobs AND
            // carries no artifacts (PipeApply.cpp); this is the same statement one level later,
            // where the twin can name the handle.
            if (!record->Archive) {
                MGLOG_E_ONCE("MGPipe: shader CSO {%u, %u} carries no server-owned archive; its "
                             "create_shader_state was emitted by the monolith arm and this is "
                             "the handle arm, so there is nothing to build from",
                             cso.Slot, cso.Gen);
                m_backendProgramUsable = false;
                return;
            }
            // A capture program with integer inputs is armed at its first build, so arming never
            // relinks a program in the middle of a capture (NarrowIntegerSeeds.h).
            PrepareNarrowIntegerSeedBuild(m_narrowIntegerSeeds, *record);
            SyncToBackendFromSource(ProgramArchiveSource::FromRecord(cso, *record));
            // The two server-owned keys the draw path's clean condition reads instead of
            // GetLinkVersion()/GetImageUnitVersion(). BindingsSerial is the second because a
            // glUniform1i on an IMAGE uniform is baked into the generated ESSL rather than
            // re-issued per draw - the record that carries it must therefore force a rebuild,
            // exactly as m_imageUnitVersion used to.
            m_syncedShaderCsoSerial = record->Serial;
            m_syncedBindingsSerial = record->BindingsSerial;
        }

        namespace {
            // The GL name of the array element that lives at `location`, given the reflection
            // name reported for it. Reflection reports one name per UNIFORM ("goku[0]") but
            // one location per ELEMENT, so a caller walking locations sees the same name
            // repeatedly; this turns it back into "goku[k]". Anything that is not an array
            // (or whose base location cannot be resolved) comes back unchanged, so the only
            // behavior that moves is the array case.
            String SubscriptUniformNameForElement(const ProgramBuildSource& program, const String& name,
                                                  Uint location) {
                if (name.size() < 3 || name.compare(name.size() - 3, 3, "[0]") != 0) return name;
                const Int base = program.GetUniformLocation(name);
                if (base < 0 || static_cast<Uint>(base) > location) return name;
                const Uint element = location - static_cast<Uint>(base);
                if (element == 0) return name;
                return name.substr(0, name.size() - 3) + "[" + std::to_string(element) + "]";
            }
        } // namespace

        // Resolves every name-based resource lookup once per link so the per-draw path
        // (BindCurrentProgramWithResources) never issues glGetUniformBlockIndex /
        // glGetUniformLocation string queries; block-to-binding-point assignments are
        // program state and only need to be established here.
        void BackendProgramObjectImpl::CacheResourceLocations(const ProgramBuildSource& src) {
            m_globalUboBackendBlockIndex = -1;
            m_globalUboBackendBlockSize = 0;
            m_lastUploadedGlobalUboVersion = ~0u;
            m_globalUboRingAllocation = {};
            if (src.GetUBOSize() > 0) {
                const Uint blockIndex =
                    g_GLESFuncs.glGetUniformBlockIndex(m_backendProgramId, MG_Util::ShaderTranspiler::GLOBAL_UBO_NAME);
                if (blockIndex != GL_INVALID_INDEX) {
                    m_globalUboBackendBlockIndex = static_cast<Int>(blockIndex);
                    g_GLESFuncs.glUniformBlockBinding(m_backendProgramId, blockIndex, 0);
                    // Ring bindings are ranges and must span the block as the backend
                    // compiled it (its std140 padding may exceed the frontend's
                    // SPIR-V-reflected size).
                    if (g_GLESFuncs.glGetActiveUniformBlockiv) {
                        GLint blockDataSize = 0;
                        g_GLESFuncs.glGetActiveUniformBlockiv(m_backendProgramId, blockIndex,
                                                              GL_UNIFORM_BLOCK_DATA_SIZE, &blockDataSize);
                        m_globalUboBackendBlockSize = static_cast<Int>(blockDataSize);
                    }
                } else {
                    MGLOG_W_ONCE("Program %u has frontend global UBO storage, but backend has no %s block.",
                                 src.GetExternalIndex(), MG_Util::ShaderTranspiler::GLOBAL_UBO_NAME);
                }
            }

            const Int uboCount = src.GetActiveUniformBlocksCount();
            m_uniformBlockBackendIndices.assign(static_cast<SizeT>(std::max(uboCount, 0)), -1);
            Uint lastUBOBinding = 0; // binding 0 is reserved for the global UBO
            for (Int i = 0; i < uboCount; ++i) {
                ++lastUBOBinding;
                const auto& name = src.GetUniformBlockName(static_cast<Uint>(i));
                const GLuint backendBlkIdx = g_GLESFuncs.glGetUniformBlockIndex(m_backendProgramId, name.c_str());
                if (backendBlkIdx == GL_INVALID_INDEX) {
                    // Either eliminated as unused, or an SSBO block (frontend reflection
                    // lists those among uniform blocks); SSBO bindings are baked into the ESSL.
                    continue;
                }
                m_uniformBlockBackendIndices[static_cast<SizeT>(i)] = static_cast<Int>(backendBlkIdx);
                g_GLESFuncs.glUniformBlockBinding(m_backendProgramId, backendBlkIdx, lastUBOBinding);
                MGLOG_D("CACHE prog=%u beProg=%u blk[%d]='%s' beIdx=%u -> bePoint=%u", src.GetExternalIndex(),
                        m_backendProgramId, i, name.c_str(), backendBlkIdx, lastUBOBinding);
            }

            m_samplerUniformBindings.clear();
            const Uint maxUniformLoc = src.GetMaxUniformLocation();
            for (Uint loc = 0; loc <= maxUniformLoc; ++loc) {
                const auto& name = src.GetUniformName(loc);
                if (name.empty()) continue;
                const GLenum uniformType = src.GetUniformType(loc);
                if (IsImageUniformType(uniformType)) {
                    // ES image units come exclusively from the layout(binding=N) qualifier
                    // (preserved in the transpiled ESSL); glUniform1i on an image uniform
                    // is an INVALID_OPERATION.
                    continue;
                }
                // Reflection names an array uniform after its FIRST element ("goku[0]") at
                // every location the array spans, so asking the driver for that one name
                // once per location hands back the same backend location N times. The
                // per-draw pass then issues N glUniform1i calls against it and only the
                // last element's unit survives - "layout(binding = 1) uniform sampler2D
                // goku[7]" ended up with goku[0] on unit 7 and goku[1..6] still on 0.
                // Address each element by its own name instead; the frontend already
                // reserves one location per element, so the element index is the distance
                // from the array's base location.
                const String elementName = SubscriptUniformNameForElement(src, name, loc);
                const Int backendLoc = g_GLESFuncs.glGetUniformLocation(m_backendProgramId, elementName.c_str());
                if (backendLoc < 0) continue;
                SamplerUniformBinding binding;
                binding.frontendLocation = loc;
                binding.backendLocation = backendLoc;
                binding.uniformType = uniformType;
                binding.lastAssignedUnit = -1;
                // Present only for the samplers EmulateTextureLodBias actually rewrote; the
                // pass names it after the sampler, which SPIRV-Cross preserves verbatim.
                binding.lodBiasLocation = g_GLESFuncs.glGetUniformLocation(
                    m_backendProgramId, (String(LOD_BIAS_UNIFORM_PREFIX) + elementName).c_str());
                binding.lastAssignedLodBias = 0.0f;
                // Present only for the unsigned multisample samplers ClampUnsignedMultisampleFetches
                // rewrote, named the same way.
                if (uniformType == GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE ||
                    uniformType == GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE_ARRAY) {
                    binding.msSampleMaxLocation = g_GLESFuncs.glGetUniformLocation(
                        m_backendProgramId, (String(MS_SAMPLE_MAX_UNIFORM_PREFIX) + elementName).c_str());
                }
                binding.lastAssignedMsSampleMax = 0;
                m_samplerUniformBindings.push_back(binding);
            }
            FinishNarrowIntegerSeedBuild(m_narrowIntegerSeeds, m_backendProgramId, m_backendProgramUsable);
        }

        void BackendProgramObjectImpl::Use() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            // glUseProgram on a program that did not link is an INVALID_OPERATION and
            // leaves the *previous* program current, so the draw would silently render
            // with an unrelated shader (KHR-GL3x.texture_size_promotion read another
            // test case's alpha that way once a sampler2DRect stage failed to
            // transpile). Bind nothing instead: the draw is then a visible no-op.
            const Uint programToBind = m_backendProgramUsable ? m_backendProgramId : 0;
            // ...unless SyncToBackend relinked this program since the last bind, in which case
            // the id matching proves nothing about the executable behind it.
            if (g_lastUsedBackendProgramId == programToBind && !m_rebindAfterRelink) {
                return;
            }
            m_rebindAfterRelink = false;
            if (!m_backendProgramUsable) {
                // Every draw made with this program renders nothing and raises no GL error, so
                // without this line the only symptom is a framebuffer that kept its clear
                // color. Latched: the early return above only dedupes CONSECUTIVE binds, so an
                // app alternating a healthy and a broken program would otherwise log every
                // single draw. Parked at MGLOG_I until the level ordering was fixed.
                MGLOG_E_ONCE("Backend program for GL program %u is unusable (a shader failed to transpile, "
                             "compile or link); binding program 0 - draws with it will render nothing",
                             m_frontendProgramId);
            }
            MGLOG_D("Using program %u", programToBind);
            g_GLESFuncs.glUseProgram(programToBind);
            g_lastUsedBackendProgramId = programToBind;
        }

        void BackendProgramObjectImpl::SetBaseInstance(Uint32 baseInstance) const {
            if (m_baseInstanceUniformLocation >= 0) {
                g_GLESFuncs.glUniform1i(m_baseInstanceUniformLocation, static_cast<GLint>(baseInstance));
            }
            // A direct value disables the indirect-command-buffer read.
            SetBaseInstanceWordIndex(-1);
        }

        // The uniform is written one-based so that its GLSL initial value, zero, already reads
        // as "no indirect command" - see PromoteDrawParameterGlobalsToUniforms.
        void BackendProgramObjectImpl::SetBaseInstanceWordIndex(Int32 wordIndex) const {
            if (m_baseInstanceWordIndexUniformLocation >= 0) {
                g_GLESFuncs.glUniform1i(m_baseInstanceWordIndexUniformLocation, wordIndex < 0 ? 0 : wordIndex + 1);
            }
        }

        void BackendProgramObjectImpl::SetBaseVertex(Int32 baseVertex) const {
            if (m_baseVertexUniformLocation >= 0) {
                g_GLESFuncs.glUniform1i(m_baseVertexUniformLocation, baseVertex);
            }
        }

        void BackendProgramObjectImpl::SetDrawID(Uint32 drawId) const {
            if (m_drawIdUniformLocation < 0) {
                return;
            }
            g_GLESFuncs.glUniform1i(m_drawIdUniformLocation, static_cast<GLint>(drawId));
        }

        void BackendProgramObjectImpl::SetViewportPassMask(Uint32 indexMask) const {
            if (m_viewportPassMaskUniformLocation < 0) {
                return;
            }
            g_GLESFuncs.glUniform1i(m_viewportPassMaskUniformLocation, static_cast<GLint>(indexMask));
        }
    } // namespace PrgramImpl
} // namespace MobileGL::MG_Backend::DirectGLES
