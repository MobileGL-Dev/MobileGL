// MobileGL - MobileGL/MG_Backend/DirectGLES/Managers.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "Managers.h"
#include "Utils.h"
#include "DirectGLES.h"
#include <Config.h>
#include <MG_Util/ShaderTranspiler/ShaderCompiler.h>

#include <MG_Util/BackendLoaders/OpenGL/Loader.h>
#include <MG_Util/Converters/GLToStr/GLEnumConverter.h>
#include <MG_Util/Converters/MGToGL/DataTypeConverter.h>
#include <MG_Util/Converters/MGToGL/BufferEnumConverter.h>
#include <MG_Util/Converters/GLToMG/TextureEnumConverter.h>
#include <MG_Util/Converters/MGToGL/ProgramEnumConverter.h>
#include <MG_Util/Converters/MGToGL/TextureEnumConverter.h>
#include <MG_Util/Converters/MGToStr/TextureEnumConverter.h>
#include <MG_Util/Converters/MGToStr/FramebufferEnumConverter.h>
#include <MG_State/GLState/TextureState/TextureObjectBuffer.h>
#include <MG_Util/Converters/GLToMG/FramebufferEnumConverter.h>
#include <MG_Util/Converters/MGToGL/FramebufferEnumConverter.h>
#include <MG_State/GLState/FramebufferState/FramebufferObject.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <regex>

namespace MobileGL::MG_Backend::DirectGLES {
    constexpr Bool PREFER_MAP_BUFFER_RANGE_FOR_BUFFER_SYNC = false;
    constexpr const char* BASE_INSTANCE_UNIFORM_NAME = "mg_BaseInstance";
    constexpr const char* DRAW_ID_UNIFORM_NAME = "mg_DrawID";
    constexpr const char* BASE_VERTEX_UNIFORM_NAME = "mg_BaseVertex";
    constexpr const char* BASE_INSTANCE_LOWERED_NAME = "mg_BaseInstanceLowered";
    constexpr const char* BASE_INSTANCE_WORD_INDEX_UNIFORM_NAME = "mg_BaseInstanceWordIndex";
    constexpr const char* INDIRECT_PARAMS_BLOCK_NAME = "mg_IndirectParams";
    constexpr const char* ZERO_BASED_INSTANCE_ID_NAME = "mg_ZeroBasedInstanceID";

    static Bool IsAngleLlvmpipeRenderer() {
        return g_GLESCapabilities.IsAngleLlvmpipeRenderer;
    }

    static Bool ShouldAvoidSamplerMipmapMinFilterOnAngleLlvmpipe() {
        // IsAngleLlvmpipeRenderer combined with the
        // MOBILEGL_AVOID_SAMPLER_MIPMAP_MIN_FILTER feature toggle,
        // both resolved in FillInGLESCapabilities.
        return g_GLESCapabilities.AvoidSamplerMipmapMinFilter;
    }

    static GLenum ResolveBackendMinFilter(const SamplerParameters& samplerParams,
                                          Bool avoidMipmapMinFilter) {
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
            String machinery;
            if (source.find(String("uniform highp int ") + BASE_INSTANCE_UNIFORM_NAME + ";") == String::npos) {
                machinery += String("uniform highp int ") + BASE_INSTANCE_UNIFORM_NAME + ";\n";
            }
            machinery += String("uniform highp int ") + BASE_INSTANCE_WORD_INDEX_UNIFORM_NAME + ";\n";
            machinery += String("layout(std430, binding = ") + std::to_string(paramsBinding) +
                         ") readonly buffer " + INDIRECT_PARAMS_BLOCK_NAME +
                         " { highp uint mg_indirectWords[]; };\n";
            if (rebaseInstanceId) {
                machinery += String("#define ") + ZERO_BASED_INSTANCE_ID_NAME + " (gl_InstanceID - ((" +
                             BASE_INSTANCE_WORD_INDEX_UNIFORM_NAME + " >= 0) ? int(mg_indirectWords[uint(" +
                             BASE_INSTANCE_WORD_INDEX_UNIFORM_NAME + ")]) : 0))\n";
            }
            machinery += String("#define ") + BASE_INSTANCE_LOWERED_NAME + " ((" +
                         BASE_INSTANCE_WORD_INDEX_UNIFORM_NAME + " >= 0) ? int(mg_indirectWords[uint(" +
                         BASE_INSTANCE_WORD_INDEX_UNIFORM_NAME + ")]) : " + BASE_INSTANCE_UNIFORM_NAME + ")";
            source.replace(pos, declaration.size(), machinery);
            break;
        }
        return source;
    }

    // The transpile pipeline invents image binding numbers: when the GL source declares
    // an image uniform without layout(binding), glslang auto-assigns one (desktop GL
    // allows that and lets the app pick the unit with glUniform1i, which ES forbids on
    // image uniforms). The unit the app actually addresses lives in frontend state: the
    // layout(binding) reflected at link time, or whatever glUniform1i stored afterwards.
    // Rewrite every image uniform declaration to that unit so imageLoad/Store hits the
    // unit the app bound with glBindImageTexture.
    String RebindImageUniformsToFrontendUnits(
        String source, const SharedPtr<MG_State::GLState::ProgramObject>& stateProgramObject) {
        if (!stateProgramObject || source.find("image") == String::npos) {
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
                Int location = stateProgramObject->GetUniformLocation(name);
                if (location < 0) {
                    location = stateProgramObject->GetUniformLocation(name + "[0]");
                }
                if (location >= 0) {
                    const Int unit = stateProgramObject->GetUniformSamplerOrImageUnitIndex(location);
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

    namespace BufferImpl {
        namespace {
            using MG_State::GLState::BackendBufferResource;
            using MG_State::GLState::BufferBackendOps;
            using MG_State::GLState::BufferObject;

            // GL_ARRAY_BUFFER redundant-bind cache (id 0 = unknown/none).
            Uint g_boundArrayBufferId = 0;
            Bool g_boundArrayBufferKnown = false;

            // Driver-level GL_PIXEL_PACK/UNPACK_BUFFER binding shadows (see
            // Managers.h). Resting state between operations is 0; scopes in the
            // readback/upload paths bind what they need through the cache and
            // return to 0, so a stale user PBO can never capture a later
            // readback that meant to target client memory.
            Uint g_boundPixelPackBufferId = 0;
            Bool g_boundPixelPackBufferKnown = false;
            Uint g_boundPixelUnpackBufferId = 0;
            Bool g_boundPixelUnpackBufferKnown = false;

            // Bumped whenever the backend ES context is destroyed; resources with
            // an older generation hold ids from a dead context.
            Uint g_bufferContextGeneration = 1;

            // Defined next to the indexed-binding shadow below; forward-declared so
            // every glDeleteBuffers site in this namespace can scrub stale shadow
            // entries (GL resets a deleted buffer's bindings - indexed and pixel
            // pack/unpack alike - to 0, and a recycled name matching a stale shadow
            // entry would otherwise false-skip the rebind).
            void ScrubBufferBindingShadowsForId(Uint id);

            // Resources whose owning BufferObject died; ids deleted at the next
            // sync point with a current ES context.
            Vector<SharedPtr<BackendBufferResource>> g_deferredBufferReleases;
            std::mutex g_deferredBufferReleasesMutex;
            // Cheap emptiness probe so the per-draw drain can skip the mutex and
            // context check when nothing was enqueued (the overwhelmingly common
            // case). Written only under the mutex; read lock-free.
            std::atomic<Bool> g_hasDeferredBufferReleases{false};

            // --- Buffer-storage pool (Mesa-style BO recycle) -------------------------
            // Recycle idle GL buffer ids of an EXACT byte size instead of glDeleteBuffers
            // (which triggers the kgsl_sharedmem_free -> mmu_unmap -> smmu/power/bandwidth
            // cascade that dominated per-frame driver cost). An id retired during frame N
            // is handed back only once the GPU has completed frame N (fence watermark, see
            // DirectGLES::CompletedFrameSerial), then reseeded in place with glBufferSubData
            // (no glBufferData realloc). All GL access is on the ES-context-owning thread;
            // the mutex only guards against off-thread deferred-release enrollment races.
            struct PooledBuffer {
                Uint id = 0;
                SizeT size = 0;
                Uint contextGeneration = 0;
                Uint64 retireSerial = 0;
            };
            UnorderedMap<SizeT, Vector<PooledBuffer>> g_bufferPool;
            SizeT g_pooledBytes = 0;
            std::mutex g_poolMutex;
            constexpr SizeT kMaxPoolableBufferBytes = 8u * 1024u * 1024u; // bigger buffers: delete now
            constexpr SizeT kMaxPoolBytes = 64u * 1024u * 1024u;          // total pool budget
            constexpr SizeT kMaxEntriesPerBucket = 32;

            Bool IsPoolable(const GLESBufferResource& r) {
                // Require working fences: recycling is gated on the frame-completion
                // watermark, which only advances if Present can insert/poll fences.
                return g_GLESFuncs.glFenceSync != nullptr && g_GLESFuncs.glGetSynciv != nullptr &&
                       r.id != 0 && !r.persistentMapped && r.contextGeneration == g_bufferContextGeneration &&
                       r.storageInitialized && r.storageSize > 0 && r.storageSize <= kMaxPoolableBufferBytes;
            }

            // Retire a buffer id into the pool (owning thread; caller verified IsPoolable).
            // Zeroes r.id to keep the single-owner invariant {live | deferred | pool}.
            void EnrollIntoPool(GLESBufferResource& r) {
                if (g_boundArrayBufferKnown && g_boundArrayBufferId == r.id) {
                    InvalidateArrayBufferBindingCache();
                }
                // Pooling keeps the id alive (and thus any driver binding of it);
                // drop to unknown rather than claiming the post-delete 0 state.
                if ((g_boundPixelPackBufferKnown && g_boundPixelPackBufferId == r.id) ||
                    (g_boundPixelUnpackBufferKnown && g_boundPixelUnpackBufferId == r.id)) {
                    InvalidatePixelBufferBindingCaches();
                }
                const std::lock_guard<std::mutex> lock(g_poolMutex);
                auto& bucket = g_bufferPool[r.storageSize];
                if (bucket.size() >= kMaxEntriesPerBucket || g_pooledBytes + r.storageSize > kMaxPoolBytes) {
                    ScrubBufferBindingShadowsForId(r.id);
                    g_GLESFuncs.glDeleteBuffers(1, &r.id); // over budget: don't pool
                    r.id = 0;
                    return;
                }
                // +1: Present increments the serial at frame END, so during the frame
                // now being built CurrentFrameSerial() reads (frame-1). A buffer used
                // this frame is only GPU-done once THIS frame's fence (serial+1) signals.
                bucket.push_back(
                    {r.id, r.storageSize, r.contextGeneration, DirectGLES::CurrentFrameSerial() + 1});
                g_pooledBytes += r.storageSize;
                r.id = 0;
            }

            // Hand back an idle pooled id of EXACTLY `size` whose GPU work is complete,
            // else 0. Owning thread only. Drops stale-generation entries encountered.
            Uint AcquireFromPool(SizeT size) {
                const Uint64 completed = DirectGLES::CompletedFrameSerial();
                const std::lock_guard<std::mutex> lock(g_poolMutex);
                auto it = g_bufferPool.find(size);
                if (it == g_bufferPool.end()) return 0;
                auto& bucket = it->second;
                for (SizeT i = bucket.size(); i-- > 0;) { // newest-first: hottest + most-likely-idle
                    PooledBuffer& e = bucket[i];
                    if (e.contextGeneration != g_bufferContextGeneration) {
                        g_pooledBytes -= e.size; // dead-context id: drop, no GL
                        bucket[i] = bucket.back();
                        bucket.pop_back();
                        continue;
                    }
                    if (e.retireSerial <= completed) {
                        const Uint id = e.id;
                        g_pooledBytes -= e.size;
                        bucket[i] = bucket.back();
                        bucket.pop_back();
                        return id;
                    }
                }
                return 0;
            }

            // --- Global-UBO ring (see Managers.h) ------------------------------------
            constexpr SizeT kUboRingInitialBytes = 4u * 1024u * 1024u;
            constexpr SizeT kUboRingMaxBytes = 64u * 1024u * 1024u;

            struct UboRingState {
                Uint id = 0;
                Uint8* mappedPtr = nullptr;
                SizeT size = 0;
                // Monotonic linear cursors: `head` counts every byte ever allocated
                // (incl. wrap padding); everything below `tail` is GPU-complete. Ring
                // offset of a linear position is pos % size, so in-flight bytes are
                // head - tail and must stay <= size.
                Uint64 head = 0;
                Uint64 tail = 0;
                Uint32 generation = 0; // bumped on every (re)create/grow; 0 = never valid
                Uint contextGeneration = 0;
                SizeT alignment = 256;
                // A hard storage-creation failure under this context; stop retrying
                // per draw (cleared when the context generation moves on).
                Bool creationFailed = false;
            };
            UboRingState g_uboRing;

            // Grown-away ring stores: deletable only once the GPU finished the last
            // frame that could reference them (same watermark as the buffer pool).
            struct RetiredUboRing {
                Uint id = 0;
                Uint contextGeneration = 0;
                Uint64 retireSerial = 0;
            };
            Vector<RetiredUboRing> g_retiredUboRings;

            // Present()-time high-water marks: every byte below headAtPresent was
            // written during frames <= frameSerial, so once frameSerial completes,
            // tail may advance to headAtPresent. FIFO by construction.
            struct UboRingFrameMark {
                Uint64 frameSerial = 0;
                Uint64 headAtPresent = 0;
            };
            Vector<UboRingFrameMark> g_uboRingFrameMarks;

            // The ES context the ring's id/map belonged to is gone (or was never
            // seen): drop every handle without GL calls and re-arm creation. The
            // generation counter must survive the reset — frame serials also survive
            // context recreation, so a restarted counter could revalidate a stale
            // per-program slot cache against the new ring.
            void ResetUboRingForNewContext() {
                const Uint32 keptGeneration = g_uboRing.generation;
                g_uboRing = {};
                g_uboRing.generation = keptGeneration;
                g_uboRing.contextGeneration = g_bufferContextGeneration;
                g_retiredUboRings.clear();
                g_uboRingFrameMarks.clear();
            }

            GLESBufferResource* ResourceOf(BufferObject& bufferObject) {
                return static_cast<GLESBufferResource*>(bufferObject.GetBackendResource().get());
            }

            Bool CanTouchGLNow() {
                return DirectGLES::IsBackendContextCurrentOnThisThread();
            }

            // (Re)specify backend storage from the shadow copy: glBufferData.
            // The orphaning point - the ES driver performs the actual rename.
            // TODO(buffer-pool Phase 2): orphan-on-respecify is NOT yet implemented.
            // When the current id is BUSY (lastUseFrameSerial > CompletedFrameSerial())
            // && !persistentMapped && !noOrphan, express the orphan as an id-swap
            // (retire the busy id into the pool, bind a fresh/pooled id) instead of the
            // in-place glBufferData below, to avoid the driver's own rename/stall. Not
            // pursued yet: glBufferData/glBufferSubData currently sit below profiler
            // noise, so respecify is not a hot path in the profiled scenes.
            void RespecifyStorageNow(GLESBufferResource& resource, BufferObject& bufferObject) {
#ifdef TRACY_ENABLE
                ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
                const SizeT size = bufferObject.GetSize();
                const GLenum usage = MG_Util::ConvertBufferUsageToGLEnum(bufferObject.GetUsage());
                BindBufferId(TempBufferTarget, resource.id);
                // An orphaning respecify (glBufferData with NULL, content never
                // written since) stays a pure NULL reallocation: the driver renames
                // the store without a stall and nothing is transferred. Uploading
                // the stale shadow here turned Minecraft-style orphaning into a
                // full-size synchronized upload.
                const void* initialData =
                    (size > 0 && bufferObject.HasDefinedContent()) ? bufferObject.MappedData() : nullptr;
                g_GLESFuncs.glBufferData(TempBufferTarget, (GLsizeiptr)size, initialData, usage);
                resource.storageSize = size;
                resource.storageInitialized = true;
                resource.pendingRespecify = false;
                resource.pendingRanges.clear();
                resource.syncedChangeSerial = bufferObject.GetChangeSerial();
            }

            Bool StorageMatches(const GLESBufferResource& resource, const BufferObject& bufferObject) {
                return resource.storageInitialized && !resource.pendingRespecify &&
                       resource.storageSize == bufferObject.GetSize();
            }



            void UploadRangeNow(GLESBufferResource& resource, BufferObject& bufferObject, SizeT start, SizeT end) {
#ifdef TRACY_ENABLE
                ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
                if (start >= end) return;
                BindBufferId(TempBufferTarget, resource.id);
                g_GLESFuncs.glBufferSubData(TempBufferTarget, (GLintptr)start, (GLsizeiptr)(end - start),
                                            bufferObject.MappedData() + start);
            }

            // EXT_buffer_storage bit values (same numeric values as the desktop ARB
            // tokens); defined locally so this compiles regardless of which GLES headers
            // expose the EXT tokens.
            constexpr GLbitfield kMapPersistentBit = 0x0040;
            constexpr GLbitfield kMapCoherentBit = 0x0080;
            constexpr GLbitfield kDynamicStorageBit = 0x0100;

            // Zero-copy persistent map: back the buffer with real immutable,
            // persistently+coherently mapped GL storage (EXT_buffer_storage) and hand the
            // app that mapped pointer (adopted by the frontend PipeResource). Returns
            // nullptr when the extension is unavailable or the context is not current, in
            // which case the frontend keeps its CPU-shadow model. Idempotent.
            void* Ops_AcquirePersistentMap(BufferObject& bufferObject) {
                if (!CanTouchGLNow() || !g_GLESFuncs.glBufferStorageEXT || !g_GLESFuncs.glMapBufferRange ||
                    !g_GLESFuncs.glGenBuffers) {
                    return nullptr;
                }
                const SizeT size = bufferObject.GetSize();
                if (size == 0) return nullptr;

                auto* resource = static_cast<GLESBufferResource*>(bufferObject.GetBackendResource().get());
                if (!resource) {
                    auto created = MakeShared<GLESBufferResource>();
                    resource = created.get();
                    bufferObject.SetBackendResource(std::move(created));
                }
                resource->contextGeneration = g_bufferContextGeneration;

                if (resource->persistentMapped && resource->persistentPtr && resource->storageSize == size) {
                    return resource->persistentPtr; // idempotent
                }

                // Need a fresh id: glBufferStorage fails on a buffer that already has
                // immutable storage, and any prior mutable store is replaced anyway.
                if (resource->id != 0) {
                    ScrubBufferBindingShadowsForId(resource->id);
                    g_GLESFuncs.glDeleteBuffers(1, &resource->id);
                    resource->id = 0;
                }
                g_GLESFuncs.glGenBuffers(1, &resource->id);
                if (resource->id == 0) return nullptr;

                // Seed from the shadow (MappedData() is still the shadow: the frontend
                // adopts and drops it only after this returns).
                BindBufferId(TempBufferTarget, resource->id);
                const void* initial = bufferObject.MappedData();
                g_GLESFuncs.glBufferStorageEXT(TempBufferTarget, static_cast<GLsizeiptr>(size), initial,
                                               GL_MAP_WRITE_BIT | kMapPersistentBit | kMapCoherentBit |
                                                   kDynamicStorageBit);
                void* ptr = g_GLESFuncs.glMapBufferRange(TempBufferTarget, 0, static_cast<GLsizeiptr>(size),
                                                         GL_MAP_WRITE_BIT | kMapPersistentBit | kMapCoherentBit);
                if (!ptr) {
                    MGLOG_E("Ops_AcquirePersistentMap: glMapBufferRange(persistent) failed for buffer %u",
                            resource->id);
                    resource->persistentMapped = false;
                    resource->persistentPtr = nullptr;
                    return nullptr;
                }
                resource->persistentPtr = ptr;
                resource->persistentMapped = true;
                resource->storageSize = size;
                resource->storageInitialized = true;
                resource->pendingRespecify = false;
                {
                    const std::lock_guard<std::mutex> lock(resource->pendingMutex);
                    resource->pendingRanges.clear();
                }
                resource->syncedChangeSerial = bufferObject.GetChangeSerial();
                return ptr;
            }

            void Ops_Respecify(BufferObject& bufferObject) {
                auto* resource = ResourceOf(bufferObject);
                if (!resource) return; // lazy: EnsureBufferResource full-uploads on creation
                if (resource->persistentMapped) return; // immutable persistent storage is never respecified
                if (!CanTouchGLNow() || resource->id == 0 ||
                    resource->contextGeneration != g_bufferContextGeneration) {
                    resource->pendingRespecify = true;
                    resource->pendingRanges.clear();
                    return;
                }
                if (bufferObject.GetSize() == 0) {
                    resource->storageInitialized = false;
                    resource->storageSize = 0;
                    resource->pendingRespecify = false;
                    resource->pendingRanges.clear();
                    return;
                }
                RespecifyStorageNow(*resource, bufferObject);
            }

            void Ops_SubData(BufferObject& bufferObject, SizeT offset, SizeT size) {
                auto* resource = ResourceOf(bufferObject);
                if (!resource) return;
                if (resource->pendingRespecify) return; // full re-upload pending anyway
                if (!CanTouchGLNow() || resource->id == 0 ||
                    resource->contextGeneration != g_bufferContextGeneration ||
                    !StorageMatches(*resource, bufferObject)) {
                    resource->pendingRanges.Add({offset, offset + size});
                    return;
                }
                UploadRangeNow(*resource, bufferObject, offset, offset + size);
                resource->syncedChangeSerial = bufferObject.GetChangeSerial();
            }

            void Ops_FlushMappedRange(BufferObject& bufferObject, Range1D range,
                                      Flags<BufferMappingAccessBit> appAccess) {
                auto* resource = ResourceOf(bufferObject);
                if (!resource) return;
                if (resource->pendingRespecify) return;
                if (!CanTouchGLNow() || resource->id == 0 ||
                    resource->contextGeneration != g_bufferContextGeneration ||
                    !StorageMatches(*resource, bufferObject)) {
                    resource->pendingRanges.Add(range);
                    return;
                }

                // Honour the app's real mapping flags per call: only reach for a
                // mapped upload when the app allowed invalidation/unsynchronized
                // access, otherwise a plain glBufferSubData carries the exact
                // synchronization semantics.
                const Bool invalidate = (appAccess & BufferMappingAccessBit::InvalidateRange) ||
                                        (appAccess & BufferMappingAccessBit::InvalidateBuffer);
                const Bool unsynchronized = static_cast<Bool>(appAccess & BufferMappingAccessBit::Unsynchronized);
                if (PREFER_MAP_BUFFER_RANGE_FOR_BUFFER_SYNC && (invalidate || unsynchronized)) {
#ifdef TRACY_ENABLE
                    ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
                    BindBufferId(TempBufferTarget, resource->id);
                    void* mappedData = g_GLESFuncs.glMapBufferRange(
                        TempBufferTarget, (GLintptr)range.start, (GLsizeiptr)(range.end - range.start),
                        GL_MAP_WRITE_BIT | (invalidate ? GL_MAP_INVALIDATE_RANGE_BIT : 0) |
                            (unsynchronized ? GL_MAP_UNSYNCHRONIZED_BIT : 0));
                    if (mappedData) {
                        Memcpy(mappedData, bufferObject.MappedData() + range.start,
                               range.end - range.start);
                        g_GLESFuncs.glUnmapBuffer(TempBufferTarget);
                        resource->syncedChangeSerial = bufferObject.GetChangeSerial();
                        return;
                    }
                    MGLOG_E("Failed to map buffer with ID: %u for flush, falling back to glBufferSubData",
                            resource->id);
                }
                UploadRangeNow(*resource, bufferObject, range.start, range.end);
                resource->syncedChangeSerial = bufferObject.GetChangeSerial();
            }

            // A shader wrote this buffer through a storage/atomic-counter binding, so the ES
            // driver's copy is ahead of the frontend shadow. Pull the whole thing back so
            // MapBuffer/GetBufferSubData/CopyBufferSubData see the real results.
            void Ops_ReadbackFromGpu(BufferObject& bufferObject) {
                auto* resource = ResourceOf(bufferObject);
                if (!resource || resource->id == 0 || !resource->storageInitialized) return;
                if (resource->persistentMapped) return; // shadow already IS the GPU storage
                if (!CanTouchGLNow() || resource->contextGeneration != g_bufferContextGeneration) return;
                if (!g_GLESFuncs.glMapBufferRange || !g_GLESFuncs.glUnmapBuffer) return;
                const SizeT size = std::min<SizeT>(bufferObject.GetSize(), resource->storageSize);
                if (size == 0) return;

                BindBufferId(TempBufferTarget, resource->id);
                void* mapped = g_GLESFuncs.glMapBufferRange(TempBufferTarget, 0, static_cast<GLsizeiptr>(size),
                                                            GL_MAP_READ_BIT);
                if (mapped == nullptr) {
                    MGLOG_E("Ops_ReadbackFromGpu: glMapBufferRange(read) failed for buffer %u", resource->id);
                    return;
                }
                bufferObject.WritebackFromBackend({mapped, size}, 0);
                g_GLESFuncs.glUnmapBuffer(TempBufferTarget);
                // The shadow now matches the backend byte for byte; without this the next
                // draw would see a newer change serial and re-upload the readback over it.
                resource->syncedChangeSerial = bufferObject.GetChangeSerial();
            }

            void Ops_OnDestroy(SharedPtr<BackendBufferResource>&& resource) {
                if (!resource) return;
                auto* glesResource = static_cast<GLESBufferResource*>(resource.get());
                if (glesResource->contextGeneration != g_bufferContextGeneration) {
                    glesResource->id = 0; // id belonged to a destroyed context
                    return;
                }
                if (CanTouchGLNow()) {
                    if (IsPoolable(*glesResource)) {
                        EnrollIntoPool(*glesResource); // recycle instead of glDeleteBuffers
                        return;
                    }
                    if (glesResource->id != 0) {
                        if (g_boundArrayBufferKnown && g_boundArrayBufferId == glesResource->id) {
                            InvalidateArrayBufferBindingCache();
                        }
                        ScrubBufferBindingShadowsForId(glesResource->id);
                        g_GLESFuncs.glDeleteBuffers(1, &glesResource->id);
                        glesResource->id = 0;
                    }
                    return;
                }
                const std::lock_guard<std::mutex> lock(g_deferredBufferReleasesMutex);
                g_deferredBufferReleases.push_back(std::move(resource));
                g_hasDeferredBufferReleases.store(true, std::memory_order_release);
            }

            const BufferBackendOps g_glesBufferBackendOps = {
                .Respecify = Ops_Respecify,
                .SubData = Ops_SubData,
                .FlushMappedRange = Ops_FlushMappedRange,
                .OnDestroy = Ops_OnDestroy,
                .AcquirePersistentMap = Ops_AcquirePersistentMap,
                .ReadbackFromGpu = Ops_ReadbackFromGpu,
            };
        } // namespace

        void RegisterBufferBackendOps() {
            MG_State::GLState::SetBufferBackendOps(&g_glesBufferBackendOps);
        }

        void UnregisterBufferBackendOps() {
            if (MG_State::GLState::GetBufferBackendOps() == &g_glesBufferBackendOps) {
                MG_State::GLState::SetBufferBackendOps(nullptr);
            }
            InvalidateArrayBufferBindingCache();
            // Pooled ids belong to the dying context too; drop them without glDeleteBuffers.
            ClearBufferPool();
            const std::lock_guard<std::mutex> lock(g_deferredBufferReleasesMutex);
            // The ES context owning these ids is going away; just drop the handles.
            g_deferredBufferReleases.clear();
            g_hasDeferredBufferReleases.store(false, std::memory_order_release);
        }

        void OnBackendContextDestroyed() {
            UnregisterBufferBackendOps();
            ++g_bufferContextGeneration;
            InvalidateArrayBufferBindingCache();
            InvalidateIndexedBufferBindingCache();
            InvalidatePixelBufferBindingCaches();
            // The global-UBO ring's id and persistent map died with the context;
            // drop the handles (no GL) and let the next draw recreate the ring.
            ResetUboRingForNewContext();
        }

        void ProcessDeferredBufferReleases() {
            // Runs on every draw; skip the context check, mutex and vector churn
            // outright when nothing was enqueued since the last drain.
            if (!g_hasDeferredBufferReleases.load(std::memory_order_acquire)) return;
            if (!CanTouchGLNow()) return;
            Vector<SharedPtr<BackendBufferResource>> releases;
            {
                const std::lock_guard<std::mutex> lock(g_deferredBufferReleasesMutex);
                releases.swap(g_deferredBufferReleases);
                g_hasDeferredBufferReleases.store(false, std::memory_order_release);
            }
            for (auto& resource : releases) {
                auto* glesResource = static_cast<GLESBufferResource*>(resource.get());
                if (glesResource->contextGeneration != g_bufferContextGeneration) {
                    glesResource->id = 0;
                    continue;
                }
                if (IsPoolable(*glesResource)) {
                    EnrollIntoPool(*glesResource); // recycle instead of glDeleteBuffers
                    continue;
                }
                if (glesResource->id != 0) {
                    if (g_boundArrayBufferKnown && g_boundArrayBufferId == glesResource->id) {
                        InvalidateArrayBufferBindingCache();
                    }
                    ScrubBufferBindingShadowsForId(glesResource->id);
                    g_GLESFuncs.glDeleteBuffers(1, &glesResource->id);
                    glesResource->id = 0;
                }
            }
        }

        GLESBufferResource* GetBufferResource(MG_State::GLState::BufferObject* bufferObject) {
            if (!bufferObject) return nullptr;
            return static_cast<GLESBufferResource*>(bufferObject->GetBackendResource().get());
        }

        Bool IsBufferDrawClean(const MG_State::GLState::BufferObject* frontend, const GLESBufferResource* resource) {
            // Identity first: a respecify path can hand the frontend a NEW resource; the
            // memoed pointer is then stale (and only kept alive by the caller's shadow).
            if (!resource || resource != frontend->GetBackendResource().get()) return false;
            if (resource->contextGeneration != g_bufferContextGeneration) return false;
            if (resource->id == 0) return false;
            // Zero-copy coherent persistent store: EnsureBufferResource's own early-out —
            // the app writes straight into the mapped GPU storage, nothing to sync.
            if (resource->persistentMapped) return resource->persistentPtr != nullptr;
            // A live non-zero-copy map may owe a per-draw SyncPersistentMappedRange push
            // (persistent maps mutate the shadow without bumping the change serial).
            if (frontend->IsMapped()) return false;
            if (resource->pendingRespecify || !resource->storageInitialized) return false;
            // Same unlocked emptiness probe EnsureBufferResource's replay branch uses.
            if (!resource->pendingRanges.empty()) return false;
            if (resource->storageSize != frontend->GetSize()) return false;
            return resource->syncedChangeSerial.load(std::memory_order_acquire) == frontend->GetChangeSerial();
        }

        GLESBufferResource* EnsureBufferResource(const SharedPtr<MG_State::GLState::BufferObject>& bufferObject) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (!bufferObject) return nullptr;

            auto* resource = static_cast<GLESBufferResource*>(bufferObject->GetBackendResource().get());
            if (!resource) {
                auto newResource = MakeShared<GLESBufferResource>();
                newResource->pendingRespecify = true;
                resource = newResource.get();
                bufferObject->SetBackendResource(std::move(newResource));
            }

            if (resource->contextGeneration != g_bufferContextGeneration) {
                // The id (if any) belonged to a destroyed ES context.
                resource->id = 0;
                resource->storageInitialized = false;
                resource->storageSize = 0;
                resource->pendingRespecify = true;
                resource->pendingRanges.clear();
                resource->contextGeneration = g_bufferContextGeneration;
                // The persistent map (and its pointer) died with the old context; the
                // frontend re-acquires a fresh one on its next map.
                resource->persistentMapped = false;
                resource->persistentPtr = nullptr;
            }

            // Zero-copy coherent persistent buffer: the app writes straight into the
            // persistently mapped immutable store, so there is nothing to (re)upload at
            // draw time. This is where the per-draw whole-buffer glBufferSubData used to run.
            if (resource->persistentMapped && resource->persistentPtr && resource->id != 0) {
                return resource;
            }

            if (resource->id == 0) {
                // Try to recycle an idle same-size buffer from the pool (GPU-complete,
                // exact byte size) and reseed it in place with glBufferSubData, instead
                // of glGenBuffers + fresh-storage glBufferData (the kgsl alloc path).
                const SizeT poolSize = bufferObject->GetSize();
                const Uint reused =
                    (poolSize > 0 && !resource->persistentMapped) ? AcquireFromPool(poolSize) : 0;
                if (reused != 0) {
                    resource->id = reused;
                    resource->storageSize = poolSize;
                    resource->storageInitialized = true;
                    resource->pendingRespecify = false;
                    BindBufferId(TempBufferTarget, reused);
                    g_GLESFuncs.glBufferSubData(TempBufferTarget, 0, (GLsizeiptr)poolSize,
                                                bufferObject->MappedData());
                    {
                        const std::lock_guard<std::mutex> lock(resource->pendingMutex);
                        resource->pendingRanges.clear();
                    }
                    resource->syncedChangeSerial = bufferObject->GetChangeSerial();
                } else {
                    g_GLESFuncs.glGenBuffers(1, &resource->id);
                    if (resource->id == 0) {
                        MGLOG_E("Failed to generate buffer object.");
                        MGLOG_E("ES glGetError(): %s",
                                MG_Util::ConvertGLEnumToString(g_GLESFuncs.glGetError()).c_str());
                        return resource;
                    }
                    resource->storageInitialized = false;
                    resource->pendingRespecify = true;
                }
            }

            // Push persistently-mapped writes first; lands either as an immediate
            // SubData (fresh storage) or as part of the full re-upload below.
            bufferObject->SyncPersistentMappedRange();

            if (bufferObject->GetSize() == 0) {
                return resource;
            }

            if (resource->pendingRespecify || !resource->storageInitialized ||
                resource->storageSize != bufferObject->GetSize()) {
                RespecifyStorageNow(*resource, *bufferObject);
            } else if (!resource->pendingRanges.empty()) {
                for (const auto& range : resource->pendingRanges) {
                    const SizeT end = std::min(range.end, bufferObject->GetSize());
                    UploadRangeNow(*resource, *bufferObject, std::min(range.start, end), end);
                }
                resource->pendingRanges.clear();
                resource->syncedChangeSerial = bufferObject->GetChangeSerial();
            } else if (resource->syncedChangeSerial != bufferObject->GetChangeSerial()) {
                // Ops could not track some writes (e.g. the ops table was
                // unregistered between contexts); re-upload everything.
                RespecifyStorageNow(*resource, *bufferObject);
            }
            return resource;
        }

        void BindBufferId(GLenum target, Uint id) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (target == GL_ARRAY_BUFFER) {
                if (g_boundArrayBufferKnown && g_boundArrayBufferId == id) {
                    return;
                }
                g_boundArrayBufferId = id;
                g_boundArrayBufferKnown = true;
            }
            g_GLESFuncs.glBindBuffer(target, id);
        }

        void InvalidateArrayBufferBindingCache() {
            g_boundArrayBufferId = 0;
            g_boundArrayBufferKnown = false;
        }

        void BindPixelPackBufferId(Uint id) {
            if (g_boundPixelPackBufferKnown && g_boundPixelPackBufferId == id) {
                return;
            }
            g_GLESFuncs.glBindBuffer(GL_PIXEL_PACK_BUFFER, id);
            g_boundPixelPackBufferId = id;
            g_boundPixelPackBufferKnown = true;
        }

        void BindPixelUnpackBufferId(Uint id) {
            if (g_boundPixelUnpackBufferKnown && g_boundPixelUnpackBufferId == id) {
                return;
            }
            g_GLESFuncs.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, id);
            g_boundPixelUnpackBufferId = id;
            g_boundPixelUnpackBufferKnown = true;
        }

        void InvalidatePixelBufferBindingCaches() {
            g_boundPixelPackBufferId = 0;
            g_boundPixelPackBufferKnown = false;
            g_boundPixelUnpackBufferId = 0;
            g_boundPixelUnpackBufferKnown = false;
        }

        void NoteBufferIdDeleted(Uint id) {
            if (id == 0) {
                return;
            }
            if (g_boundArrayBufferKnown && g_boundArrayBufferId == id) {
                InvalidateArrayBufferBindingCache();
            }
            ScrubBufferBindingShadowsForId(id);
        }

        namespace {
            // Shadow of the GL indexed buffer bindings so redundant glBindBufferBase/Range
            // (same index + id + range) are skipped. isBase distinguishes a whole-buffer
            // base bind from a sub-range bind. Fresh/reset context: every point is base(0)
            // == unbound, which matches the GL default.
            struct IndexedBufferBinding {
                Uint id = 0;
                GLintptr offset = 0;
                GLsizeiptr size = 0;
                Bool isBase = true;
            };
            constexpr SizeT kMaxIndexedBufferBindings = 64;
            IndexedBufferBinding g_indexedUBOBindings[kMaxIndexedBufferBindings];
            IndexedBufferBinding g_indexedSSBOBindings[kMaxIndexedBufferBindings];
            IndexedBufferBinding* IndexedBindingShadow(GLenum glTarget, Uint index) {
                if (index >= kMaxIndexedBufferBindings) return nullptr; // out of range: never cache
                if (glTarget == GL_UNIFORM_BUFFER) return &g_indexedUBOBindings[index];
                if (glTarget == GL_SHADER_STORAGE_BUFFER) return &g_indexedSSBOBindings[index];
                return nullptr;
            }

            // glDeleteBuffers resets the deleted buffer's bindings (indexed and
            // pixel pack/unpack ones included) to 0 in the current context; mirror
            // that in the shadows, or a later buffer recycling the same name with a
            // matching shadow entry would false-skip its rebind. Default
            // IndexedBufferBinding{} == base(0) == the post-delete GL state.
            void ScrubBufferBindingShadowsForId(Uint id) {
                if (id == 0) return;
                for (auto& binding : g_indexedUBOBindings) {
                    if (binding.id == id) binding = {};
                }
                for (auto& binding : g_indexedSSBOBindings) {
                    if (binding.id == id) binding = {};
                }
                if (g_boundPixelPackBufferKnown && g_boundPixelPackBufferId == id) {
                    g_boundPixelPackBufferId = 0;
                }
                if (g_boundPixelUnpackBufferKnown && g_boundPixelUnpackBufferId == id) {
                    g_boundPixelUnpackBufferId = 0;
                }
            }
        } // namespace

        void BindBufferBaseCached(GLenum glTarget, Uint index, Uint id) {
            auto* s = IndexedBindingShadow(glTarget, index);
            if (s && s->isBase && s->id == id) return;
            g_GLESFuncs.glBindBufferBase(glTarget, index, id);
            if (s) *s = {id, 0, 0, true};
        }

        void BindBufferRangeCached(GLenum glTarget, Uint index, Uint id, GLintptr offset, GLsizeiptr size) {
            auto* s = IndexedBindingShadow(glTarget, index);
            if (s && !s->isBase && s->id == id && s->offset == offset && s->size == size) return;
            g_GLESFuncs.glBindBufferRange(glTarget, index, id, offset, size);
            if (s) *s = {id, offset, size, false};
        }

        void InvalidateIndexedBufferBindingCache() {
            for (auto& b : g_indexedUBOBindings) b = {};
            for (auto& b : g_indexedSSBOBindings) b = {};
        }

        void TrimBufferPool() {
            const std::lock_guard<std::mutex> lock(g_poolMutex);
            if (g_pooledBytes <= kMaxPoolBytes) return;
            // Over budget: evict oldest-retireSerial entries with real glDeleteBuffers.
            while (g_pooledBytes > kMaxPoolBytes) {
                SizeT oldestKey = 0, oldestIdx = 0;
                Uint64 oldestSerial = ~Uint64{0};
                Bool found = false;
                for (auto& kv : g_bufferPool) {
                    for (SizeT i = 0; i < kv.second.size(); ++i) {
                        if (kv.second[i].retireSerial < oldestSerial) {
                            oldestSerial = kv.second[i].retireSerial;
                            oldestKey = kv.first;
                            oldestIdx = i;
                            found = true;
                        }
                    }
                }
                if (!found) break;
                auto& bucket = g_bufferPool[oldestKey];
                PooledBuffer& e = bucket[oldestIdx];
                if (e.contextGeneration == g_bufferContextGeneration && e.id != 0) {
                    ScrubBufferBindingShadowsForId(e.id);
                    g_GLESFuncs.glDeleteBuffers(1, &e.id);
                }
                g_pooledBytes -= e.size;
                bucket[oldestIdx] = bucket.back();
                bucket.pop_back();
            }
        }

        void ClearBufferPool() {
            const std::lock_guard<std::mutex> lock(g_poolMutex);
            // Ids belong to the dying context; drop without glDeleteBuffers (mirrors
            // the g_deferredBufferReleases.clear() discipline).
            g_bufferPool.clear();
            g_pooledBytes = 0;
        }

        // --- Global-UBO ring (see Managers.h) ------------------------------------
        namespace {
            // (Re)create the ring store with room for at least minBytes. Any live
            // store is retired (deleted once the GPU finished the last frame that
            // could reference its slots), never deleted in place. Returns false and
            // leaves the current store untouched when minBytes cannot fit under the
            // size cap; a GL failure loses the store and latches creationFailed so
            // draws stop retrying under this context.
            Bool CreateUboRingStorage(SizeT minBytes) {
                SizeT newSize = kUboRingInitialBytes;
                while (newSize < minBytes) newSize *= 2;
                if (newSize > kUboRingMaxBytes) return false;

                if (g_uboRing.id != 0) {
                    g_retiredUboRings.push_back(
                        {g_uboRing.id, g_uboRing.contextGeneration, DirectGLES::CurrentFrameSerial() + 1});
                }
                const Uint32 nextGeneration = g_uboRing.generation + 1;
                g_uboRing.id = 0;
                g_uboRing.mappedPtr = nullptr;

                Uint id = 0;
                g_GLESFuncs.glGenBuffers(1, &id);
                if (id != 0) {
                    BindBufferId(TempBufferTarget, id);
                    g_GLESFuncs.glBufferStorageEXT(TempBufferTarget, static_cast<GLsizeiptr>(newSize), nullptr,
                                                   GL_MAP_WRITE_BIT | kMapPersistentBit | kMapCoherentBit);
                    void* ptr = g_GLESFuncs.glMapBufferRange(TempBufferTarget, 0, static_cast<GLsizeiptr>(newSize),
                                                             GL_MAP_WRITE_BIT | kMapPersistentBit | kMapCoherentBit);
                    if (!ptr) {
                        // The dying id is what the array-buffer cache has recorded as
                        // bound; a later buffer recycling the name would false-skip.
                        InvalidateArrayBufferBindingCache();
                        g_GLESFuncs.glDeleteBuffers(1, &id);
                        id = 0;
                    } else {
                        g_uboRing.mappedPtr = static_cast<Uint8*>(ptr);
                    }
                }
                if (id == 0) {
                    MGLOG_E("Global-UBO ring: persistent storage creation failed (%zu bytes); "
                            "falling back to glBufferSubData uploads.",
                            newSize);
                    g_uboRing.creationFailed = true;
                    return false;
                }

                const GLint capsAlignment = g_GLESCapabilities.UniformBufferOffsetAlignment;
                g_uboRing.id = id;
                g_uboRing.size = newSize;
                g_uboRing.head = 0;
                g_uboRing.tail = 0;
                g_uboRing.generation = nextGeneration;
                g_uboRing.alignment = capsAlignment > 0 ? static_cast<SizeT>(capsAlignment) : 256;
                g_uboRingFrameMarks.clear();
                MGLOG_D("Global-UBO ring: %zu MiB persistent store ready (id %u, gen %u, align %zu).",
                        newSize / (1024u * 1024u), id, nextGeneration, g_uboRing.alignment);
                return true;
            }
        } // namespace

        Bool UboRingAvailable() {
            if (MG_Config::Features.DisableUboRing) return false;
            // Reclamation rides the Present fence watermark; without working fences
            // slots would never be provably GPU-idle (same rule as IsPoolable).
            if (!g_GLESFuncs.glBufferStorageEXT || !g_GLESFuncs.glMapBufferRange || !g_GLESFuncs.glGenBuffers ||
                !g_GLESFuncs.glFenceSync || !g_GLESFuncs.glGetSynciv) {
                return false;
            }
            if (!CanTouchGLNow()) return false;
            if (g_uboRing.contextGeneration != g_bufferContextGeneration) {
                ResetUboRingForNewContext();
            }
            return !g_uboRing.creationFailed;
        }

        Bool UboRingAllocate(SizeT size, SizeT& outOffset) {
            if (size == 0 || !UboRingAvailable()) return false;
            // Division-based rounding: the spec doesn't promise a power-of-two
            // alignment. Slot offsets stay multiples of the alignment because every
            // slot size is, and wrap padding restarts at ring offset 0.
            const SizeT alignedSize =
                (size + g_uboRing.alignment - 1) / g_uboRing.alignment * g_uboRing.alignment;
            if (g_uboRing.id == 0 && !CreateUboRingStorage(alignedSize)) {
                return false;
            }

            // Advance tail past every frame the GPU provably finished.
            const Uint64 completed = DirectGLES::CompletedFrameSerial();
            SizeT retiredMarks = 0;
            for (const auto& mark : g_uboRingFrameMarks) {
                if (mark.frameSerial > completed) break;
                if (mark.headAtPresent > g_uboRing.tail) g_uboRing.tail = mark.headAtPresent;
                ++retiredMarks;
            }
            if (retiredMarks > 0) {
                g_uboRingFrameMarks.erase(g_uboRingFrameMarks.begin(),
                                          g_uboRingFrameMarks.begin() + static_cast<std::ptrdiff_t>(retiredMarks));
            }

            // A slot may not straddle the ring end; pad the cursor to the boundary.
            SizeT offset = static_cast<SizeT>(g_uboRing.head % g_uboRing.size);
            if (offset + alignedSize > g_uboRing.size) {
                g_uboRing.head += g_uboRing.size - offset;
                offset = 0;
            }

            if (g_uboRing.head + alignedSize - g_uboRing.tail > g_uboRing.size) {
                // In-flight span would overrun live slots: grow instead of overwrite.
                if (CreateUboRingStorage(std::max(g_uboRing.size * 2, alignedSize))) {
                    offset = 0;
                } else if (g_uboRing.creationFailed) {
                    return false; // store lost; callers fall back to glBufferSubData
                } else {
                    // At the size cap (>kUboRingMaxBytes of uniforms in flight). First
                    // try to free room by waiting for the OLDEST in-flight frames to
                    // retire - a bounded wait that ends as soon as enough tail space
                    // exists, instead of draining the entire queue.
                    constexpr Uint64 kFrameWaitNs = 50ull * 1000 * 1000; // 50ms per frame
                    while (!g_uboRingFrameMarks.empty() &&
                           g_uboRing.head + alignedSize - g_uboRing.tail > g_uboRing.size) {
                        const auto& oldest = g_uboRingFrameMarks.front();
                        if (!DirectGLES::WaitForFrameSerialCompleted(oldest.frameSerial, kFrameWaitNs)) {
                            break;
                        }
                        if (oldest.headAtPresent > g_uboRing.tail) g_uboRing.tail = oldest.headAtPresent;
                        g_uboRingFrameMarks.erase(g_uboRingFrameMarks.begin());
                    }
                    if (g_uboRing.head + alignedSize - g_uboRing.tail <= g_uboRing.size) {
                        offset = static_cast<SizeT>(g_uboRing.head % g_uboRing.size);
                        if (offset + alignedSize > g_uboRing.size) {
                            g_uboRing.head += g_uboRing.size - offset;
                            offset = 0;
                        }
                        g_uboRing.head += alignedSize;
                        outOffset = offset;
                        return true;
                    }
                    // No usable fence covers the oldest frames: drain once rather than
                    // corrupt live slots.
                    if (g_GLESFuncs.glFinish) g_GLESFuncs.glFinish();
                    g_uboRing.tail = g_uboRing.head;
                    g_uboRingFrameMarks.clear();
                    // Same-frame slots written before the drain may now be recycled by
                    // the very next allocations; a generation bump keeps later draws
                    // from rebinding those cached offsets.
                    ++g_uboRing.generation;
                    offset = static_cast<SizeT>(g_uboRing.head % g_uboRing.size);
                    if (offset + alignedSize > g_uboRing.size) {
                        g_uboRing.head += g_uboRing.size - offset;
                        offset = 0;
                    }
                }
            }

            g_uboRing.head += alignedSize;
            outOffset = offset;
            return true;
        }

        void* UboRingMappedPtr() { return g_uboRing.mappedPtr; }
        Uint UboRingBufferId() { return g_uboRing.id; }
        Uint32 UboRingGeneration() { return g_uboRing.generation; }

        void UboRingOnPresent() {
            if (!CanTouchGLNow()) return;

            // Delete grown-away stores the GPU is provably done with.
            const Uint64 completed = DirectGLES::CompletedFrameSerial();
            for (SizeT i = g_retiredUboRings.size(); i-- > 0;) {
                RetiredUboRing& entry = g_retiredUboRings[i];
                const Bool staleContext = entry.contextGeneration != g_bufferContextGeneration;
                if (!staleContext && entry.retireSerial > completed) continue;
                if (!staleContext && entry.id != 0) {
                    ScrubBufferBindingShadowsForId(entry.id);
                    g_GLESFuncs.glDeleteBuffers(1, &entry.id);
                }
                g_retiredUboRings[i] = g_retiredUboRings.back();
                g_retiredUboRings.pop_back();
            }

            if (g_uboRing.id == 0 || g_uboRing.contextGeneration != g_bufferContextGeneration) return;
            // Retire completed marks here too — UboRingAllocate is the main consumer,
            // but frames with no global-UBO draws would otherwise let the list grow
            // one entry per Present, unboundedly.
            SizeT retiredMarks = 0;
            for (const auto& mark : g_uboRingFrameMarks) {
                if (mark.frameSerial > completed) break;
                if (mark.headAtPresent > g_uboRing.tail) g_uboRing.tail = mark.headAtPresent;
                ++retiredMarks;
            }
            if (retiredMarks > 0) {
                g_uboRingFrameMarks.erase(g_uboRingFrameMarks.begin(),
                                          g_uboRingFrameMarks.begin() + static_cast<std::ptrdiff_t>(retiredMarks));
            }
            // Record this frame's high-water mark (Present just fenced the serial now
            // reported by CurrentFrameSerial()). A fence-less Present repeats the
            // serial; fold into the existing mark.
            const Uint64 serial = DirectGLES::CurrentFrameSerial();
            if (!g_uboRingFrameMarks.empty() && g_uboRingFrameMarks.back().frameSerial == serial) {
                g_uboRingFrameMarks.back().headAtPresent = g_uboRing.head;
            } else {
                g_uboRingFrameMarks.push_back({serial, g_uboRing.head});
            }
        }
    } // namespace BufferImpl

    namespace VertexArrayImpl {
        namespace {
            SizeT GetDataTypeSize(DataType type) {
                switch (type) {
                case DataType::Int8:
                case DataType::Uint8:
                    return 1;
                case DataType::Int16:
                case DataType::Uint16:
                case DataType::Float16:
                    return 2;
                case DataType::Int32:
                case DataType::Uint32:
                case DataType::Float32:
                case DataType::Fixed32:
                    return 4;
                case DataType::Float64:
                    return 8;
                default:
                    return 0;
                }
            }

            // Tightly-packed byte size of one vertex element: 4 for the 2_10_10_10 types and GL_BGRA
            // (one 32-bit word / 4 bytes), componentSize * size otherwise. 0 for unknown types.
            SizeT GetAttributeByteSize(DataType type, int size, Bool isBgra) {
                if (type == DataType::Int2101010Rev || type == DataType::Uint2101010Rev || isBgra) {
                    return 4;
                }
                const SizeT componentSize = GetDataTypeSize(type);
                return componentSize == 0 ? 0 : componentSize * static_cast<SizeT>(size);
            }
        } // namespace

        BackendVertexArrayObject::BackendVertexArrayObject() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            m_clientAttributeBufferIds.fill(0);
            g_GLESFuncs.glGenVertexArrays(1, &m_backendVAOId);
            if (m_backendVAOId == 0) {
                MGLOG_E("Failed to generate vertex array object.");
                MGLOG_E("ES glGetError(): %s", MG_Util::ConvertGLEnumToString(g_GLESFuncs.glGetError()).c_str());
            } else {
                MGLOG_D("Generated vertex array object with ID: %u.", m_backendVAOId);
            }
        }

        BackendVertexArrayObject::~BackendVertexArrayObject() {
            if (m_backendVAOId != 0) {
                NoteVAOIdDeleted(m_backendVAOId);
                g_GLESFuncs.glDeleteVertexArrays(1, &m_backendVAOId);
                m_backendVAOId = 0;
            }
            for (auto& bufferId : m_clientAttributeBufferIds) {
                if (bufferId != 0) {
                    BufferImpl::NoteBufferIdDeleted(bufferId);
                    g_GLESFuncs.glDeleteBuffers(1, &bufferId);
                    bufferId = 0;
                }
            }
        }

        namespace {
            Uint g_boundBackendVAOId = 0;
            Bool g_boundBackendVAOKnown = false;
        } // namespace

        void BindBackendVAOId(Uint id) {
            if (g_boundBackendVAOKnown && g_boundBackendVAOId == id) {
                return;
            }
            g_GLESFuncs.glBindVertexArray(id);
            g_boundBackendVAOId = id;
            g_boundBackendVAOKnown = true;
        }

        void InvalidateVAOBindingCache() {
            g_boundBackendVAOKnown = false;
        }

        void NoteVAOIdDeleted(Uint id) {
            if (g_boundBackendVAOKnown && g_boundBackendVAOId == id) {
                g_boundBackendVAOId = 0; // glDeleteVertexArrays reverts a bound VAO to 0
            }
        }

        void BackendVertexArrayObject::Bind() const {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            BindBackendVAOId(m_backendVAOId);
        }

        inline Bool BindAttributeBuffer(const MG_State::GLState::VertexAttribute& attrib) {
            const auto& bufferObject = attrib.Buffer;
            if (!bufferObject) {
                MGLOG_W("Attribute has no bound buffer, skipping.");
                return false;
            }

            auto* backendResource = BufferImpl::EnsureBufferResource(bufferObject);
            if (!backendResource || backendResource->id == 0) {
                MGLOG_E("No backend buffer found for attribute's buffer, cannot bind attribute.");
                return false;
            }

            BufferImpl::BindBufferId(GL_ARRAY_BUFFER, backendResource->id);
            return true;
        }

        void BackendVertexArrayObject::SyncToBackend(
            const SharedPtr<MG_State::GLState::VertexArrayObject>& stateVAOObject) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (!stateVAOObject) {
                MGLOG_E("State VAO object is null, cannot sync to backend.");
                return;
            }

            MGLOG_D("Syncing VAO with backend ID %u to backend for state ID %u", m_backendVAOId,
                    stateVAOObject->GetExternalIndex());

            // One compare instead of MAX_VERTEX_ATTRIBS x 3 per draw: the config version
            // aggregates every per-attribute version bump (see the member comment), and the
            // index-buffer slot version covers the only other thing this function reads. When
            // both are clean there is nothing to emit, and the VAO is not even bound here -
            // PrepareForDraw's BindCurrentVAO establishes the draw binding regardless.
            const Uint32 currentConfigVersion = stateVAOObject->GetConfigVersion();
            const Uint16 currentIndexBufferVersion = stateVAOObject->GetIndexBufferBindingSlot().GetVersion();
            const Bool attributesDirty = !m_hasSyncedConfigVersion || m_syncedConfigVersion != currentConfigVersion;
            const Bool indexBufferDirty = currentIndexBufferVersion != m_syncedIndexBufferVersion;
            if (!attributesDirty && !indexBufferDirty) {
                return;
            }

            Bind();

            const auto& allAttributeVersions = stateVAOObject->GetAllAttributeVersions();
            const auto& allAttributes = stateVAOObject->GetAllAttributes();
            for (Uint attribIndex = 0; attribIndex < allAttributes.size() && attributesDirty; ++attribIndex) {
                const auto& attrib = allAttributes[attribIndex];
                Bool needsSyncSwitch = allAttributeVersions[attribIndex].SwitchVersion !=
                                       m_syncedAttributeVersions[attribIndex].SwitchVersion;
                if (needsSyncSwitch) {
                    if (attrib.Enabled) {
                        g_GLESFuncs.glEnableVertexAttribArray(attribIndex);
                    } else {
                        g_GLESFuncs.glDisableVertexAttribArray(attribIndex);
                    }
                }

                Bool needsSyncFormat = allAttributeVersions[attribIndex].FormatVersion !=
                                       m_syncedAttributeVersions[attribIndex].FormatVersion;
                Bool needsSyncBuffer = allAttributeVersions[attribIndex].BufferVersion !=
                                       m_syncedAttributeVersions[attribIndex].BufferVersion;
                if (!needsSyncFormat && !needsSyncBuffer) continue;

                // Defence in depth. The frontend already declines glVertexAttribLFormat on this
                // backend (SupportsFloat64VertexAttributes is false - ES has no GL_DOUBLE vertex
                // format and ESSL has no fp64 type), so IsLong should never arrive here; if it ever
                // did, passing GL_DOUBLE to glVertexAttribPointer would only raise GL_INVALID_ENUM on
                // the real driver. Disabling rather than merely skipping matters: becoming long bumps
                // FormatVersion, not SwitchVersion, so the enable/disable block above will not run
                // again and an already-enabled array would stay enabled with no pointer and no
                // ARRAY_BUFFER binding - which ES 3.1+ makes an INVALID_OPERATION at draw.
                if (attrib.IsLong) {
                    MGLOG_E("DirectGLES: vertex attribute %u is a 64-bit (GL_DOUBLE) array, which this "
                            "backend cannot feed - disabling the array",
                            attribIndex);
                    g_GLESFuncs.glDisableVertexAttribArray(attribIndex);
                    continue;
                }

                if (!BindAttributeBuffer(attrib)) {
                    continue;
                }

                if (!attrib.IsInteger) {
                    // GL_BGRA is passed to the driver as the size argument (the driver reorders BGRA).
                    const GLint glSize = attrib.IsBgra ? static_cast<GLint>(GL_BGRA) : attrib.Size;
                    g_GLESFuncs.glVertexAttribPointer(
                        attribIndex, glSize, MG_Util::ConvertDataTypeToGLEnum(attrib.Type),
                        attrib.Normalized ? GL_TRUE : GL_FALSE, attrib.Stride, (const void*)attrib.Offset);
                } else {
                    g_GLESFuncs.glVertexAttribIPointer(attribIndex, attrib.Size,
                                                       MG_Util::ConvertDataTypeToGLEnum(attrib.Type), attrib.Stride,
                                                       (const void*)attrib.Offset);
                }

                if (needsSyncFormat) {
                    g_GLESFuncs.glVertexAttribDivisor(attribIndex, attrib.Divisor);
                }
            }

            if (indexBufferDirty) {
                const auto& indexBufferBinding = stateVAOObject->GetIndexBufferBindingSlot().GetBoundObject();
                Bool indexBufferSynced = false;
                if (indexBufferBinding) {
                    auto* backendResource = BufferImpl::EnsureBufferResource(indexBufferBinding);
                    if (backendResource && backendResource->id != 0) {
                        BufferImpl::BindBufferId(GL_ELEMENT_ARRAY_BUFFER, backendResource->id);
                        indexBufferSynced = true;
                    } else {
                        MGLOG_W("No backend buffer found for index buffer binding, cannot bind index buffer.");
                    }
                } else {
                    g_GLESFuncs.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
                    indexBufferSynced = true;
                }

                if (indexBufferSynced) {
                    m_syncedIndexBufferVersion = currentIndexBufferVersion;
                }
            }

            if (attributesDirty) {
                m_syncedAttributeVersions = allAttributeVersions;
                m_syncedConfigVersion = currentConfigVersion;
                m_hasSyncedConfigVersion = true;
            }
        }

        void BackendVertexArrayObject::SyncClientSideAttributesForDrawArrays(
            const SharedPtr<MG_State::GLState::VertexArrayObject>& stateVAOObject, GLint first, GLsizei count) {
            if (!stateVAOObject || count <= 0 || first < 0) {
                return;
            }

            Bind();

            const auto& allAttributes = stateVAOObject->GetAllAttributes();
            for (Uint attribIndex = 0; attribIndex < allAttributes.size(); ++attribIndex) {
                const auto& attrib = allAttributes[attribIndex];
                if (!attrib.Enabled || attrib.Buffer) {
                    continue;
                }

                // Same reason as SyncToBackend: there is no ES vertex format for a 64-bit array, and
                // this path only ever reaches glVertexAttribPointer/IPointer.
                if (attrib.IsLong) {
                    g_GLESFuncs.glDisableVertexAttribArray(attribIndex);
                    continue;
                }

                const auto* clientData = reinterpret_cast<const Uint8*>(attrib.Offset);
                const SizeT elementSize = GetAttributeByteSize(attrib.Type, attrib.Size, attrib.IsBgra);
                if (!clientData || elementSize == 0 || attrib.Size <= 0) {
                    continue;
                }

                const SizeT stride = attrib.Stride > 0 ? static_cast<SizeT>(attrib.Stride) : elementSize;
                const SizeT uploadSize = static_cast<SizeT>(first + count - 1) * stride + elementSize;

                auto& bufferId = m_clientAttributeBufferIds[attribIndex];
                if (bufferId == 0) {
                    g_GLESFuncs.glGenBuffers(1, &bufferId);
                    if (bufferId == 0) {
                        MGLOG_E("Failed to create client-side vertex attribute upload buffer.");
                        continue;
                    }
                }

                BufferImpl::BindBufferId(GL_ARRAY_BUFFER, bufferId);
                g_GLESFuncs.glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(uploadSize), clientData,
                                         GL_STREAM_DRAW);

                if (!attrib.IsInteger) {
                    const GLint glSize = attrib.IsBgra ? static_cast<GLint>(GL_BGRA) : attrib.Size;
                    g_GLESFuncs.glVertexAttribPointer(
                        attribIndex, glSize, MG_Util::ConvertDataTypeToGLEnum(attrib.Type),
                        attrib.Normalized ? GL_TRUE : GL_FALSE, static_cast<GLsizei>(stride), nullptr);
                } else {
                    g_GLESFuncs.glVertexAttribIPointer(attribIndex, attrib.Size,
                                                       MG_Util::ConvertDataTypeToGLEnum(attrib.Type),
                                                       static_cast<GLsizei>(stride), nullptr);
                }
            }

        }

        StateBackendObjectRegistry<MG_State::GLState::VertexArrayObject, BackendVertexArrayObject>
            g_backendVertexArrayObjects;
    } // namespace VertexArrayImpl

    namespace TextureImpl {
        BackendTextureObject::BackendTextureObject() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            g_GLESFuncs.glGenTextures(1, &m_backendTextureId);
            m_contextGeneration = g_textureContextGeneration;
            if (m_backendTextureId == 0) {
                MGLOG_E("Failed to generate texture object.");
                MGLOG_E("ES glGetError(): %s", MG_Util::ConvertGLEnumToString(g_GLESFuncs.glGetError()).c_str());
            } else {
                MGLOG_D("Generated texture object with ID: %u.", m_backendTextureId);
            }
        }

        BackendTextureObject::~BackendTextureObject() {
            if (m_backendTextureId == 0) {
                return;
            }
            // Scrub every driver-state shadow that could false-skip when the name
            // or this heap address is recycled - regardless of whether the id can
            // still be deleted.
            ScratchFBOImpl::NoteTextureIdDeleted(m_backendTextureId);
            for (auto& unitCache : g_boundTexturesCache) {
                for (auto& boundTexture : unitCache) {
                    if (boundTexture == this) {
                        boundTexture = nullptr;
                    }
                }
            }
            if (m_contextGeneration == g_textureContextGeneration && g_GLESFuncs.glDeleteTextures) {
                g_GLESFuncs.glDeleteTextures(1, &m_backendTextureId);
            }
            m_backendTextureId = 0;
        }

        void BackendTextureObject::Bind(GLenum target, Uint unit) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (g_activeTextureUnit != unit) {
                ActivateTextureUnit(unit);
            }

            auto targetN = static_cast<SizeT>(MG_Util::ConvertGLEnumToTextureTarget(target));
            if (this == g_boundTexturesCache[unit][targetN]) return;

            g_GLESFuncs.glBindTexture(target, m_backendTextureId);
            g_boundTexturesCache[unit][targetN] = this;
        }

        Uint BackendTextureObject::GetBackendTextureId() const {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            return m_backendTextureId;
        }

        void BackendTextureObject::RequireImageBindableStorage() {
            if (m_imageBindableStorageRequired) {
                return;
            }
            m_imageBindableStorageRequired = true;
            m_isInitialized = false;
        }

        void BackendTextureObject::RecreateBackendTexture() {
            if (m_backendTextureId != 0) {
                ScratchFBOImpl::NoteTextureIdDeleted(m_backendTextureId);
                if (m_contextGeneration == g_textureContextGeneration) {
                    g_GLESFuncs.glDeleteTextures(1, &m_backendTextureId);
                }
                for (auto& unitCache : g_boundTexturesCache) {
                    for (auto& boundTexture : unitCache) {
                        if (boundTexture == this) {
                            boundTexture = nullptr;
                        }
                    }
                }
            }

            g_GLESFuncs.glGenTextures(1, &m_backendTextureId);
            m_contextGeneration = g_textureContextGeneration;
            if (m_backendTextureId == 0) {
                MGLOG_E("Failed to regenerate texture object.");
                MGLOG_E("ES glGetError(): %s", MG_Util::ConvertGLEnumToString(g_GLESFuncs.glGetError()).c_str());
            } else {
                MGLOG_D("Regenerated texture object with ID: %u.", m_backendTextureId);
            }
            m_isInitialized = false;
            m_backendStorageImmutable = false;
            m_prevTextureInfo = {};
        }

        // Sets the backend GL unpack state to MobileGL's upload default for the scope,
        // then restores it. The previous state is read from a shadow instead of via
        // glGetIntegerv - that query forces a driver pipeline sync and, because texture
        // uploads run it per dirty texture per frame, it dominated the DirectGLES draw
        // path. The backend unpack state is set ONLY by MobileGL's own save/restore
        // helpers (this class and, historically, the R32F copy path), all of
        // which restore to the resting default, so the shadow stays accurate; a one-time
        // forced sync pins the backend to that known default up front. Apply() is
        // compare-and-set, so the (now redundant) glPixelStorei calls also usually no-op.
        class ScopedDefaultUnpackState {
        public:
            ScopedDefaultUnpackState() {
                EnsureShadowSynced();
                m_prevAlignment = s_alignment;
                m_prevRowLength = s_rowLength;
                m_prevSkipRows = s_skipRows;
                m_prevSkipPixels = s_skipPixels;
                m_prevImageHeight = s_imageHeight;
                m_prevSkipImages = s_skipImages;
                // Shadow mip data is tightly packed (ProcessTexturePixelsDataUnpack emits
                // width * bpp rows with no padding), so uploads must use UNPACK_ALIGNMENT = 1.
                // Alignment 4 made the driver read e.g. 7-byte R8 rows at an 8-byte stride,
                // shifting every row of a non-multiple-of-4 upload by one pixel.
                Apply(1, 0, 0, 0, 0, 0);
            }

            ~ScopedDefaultUnpackState() {
                Apply(m_prevAlignment, m_prevRowLength, m_prevSkipRows, m_prevSkipPixels, m_prevImageHeight,
                      m_prevSkipImages);
            }

        private:
            static void EnsureShadowSynced() {
                if (s_synced) {
                    return;
                }
                s_synced = true;
                g_GLESFuncs.glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                g_GLESFuncs.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                g_GLESFuncs.glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
                g_GLESFuncs.glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
                g_GLESFuncs.glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
                g_GLESFuncs.glPixelStorei(GL_UNPACK_SKIP_IMAGES, 0);
                s_alignment = 4;
                s_rowLength = 0;
                s_skipRows = 0;
                s_skipPixels = 0;
                s_imageHeight = 0;
                s_skipImages = 0;
            }

            static void Apply(GLint alignment, GLint rowLength, GLint skipRows, GLint skipPixels, GLint imageHeight,
                              GLint skipImages) {
                if (alignment != s_alignment) { g_GLESFuncs.glPixelStorei(GL_UNPACK_ALIGNMENT, alignment); s_alignment = alignment; }
                if (rowLength != s_rowLength) { g_GLESFuncs.glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLength); s_rowLength = rowLength; }
                if (skipRows != s_skipRows) { g_GLESFuncs.glPixelStorei(GL_UNPACK_SKIP_ROWS, skipRows); s_skipRows = skipRows; }
                if (skipPixels != s_skipPixels) { g_GLESFuncs.glPixelStorei(GL_UNPACK_SKIP_PIXELS, skipPixels); s_skipPixels = skipPixels; }
                if (imageHeight != s_imageHeight) { g_GLESFuncs.glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, imageHeight); s_imageHeight = imageHeight; }
                if (skipImages != s_skipImages) { g_GLESFuncs.glPixelStorei(GL_UNPACK_SKIP_IMAGES, skipImages); s_skipImages = skipImages; }
            }

            GLint m_prevAlignment = 4;
            GLint m_prevRowLength = 0;
            GLint m_prevSkipRows = 0;
            GLint m_prevSkipPixels = 0;
            GLint m_prevImageHeight = 0;
            GLint m_prevSkipImages = 0;

            // Shadow of the backend GL unpack state (GL defaults). See class comment.
            static inline Bool s_synced = false;
            static inline GLint s_alignment = 4;
            static inline GLint s_rowLength = 0;
            static inline GLint s_skipRows = 0;
            static inline GLint s_skipPixels = 0;
            static inline GLint s_imageHeight = 0;
            static inline GLint s_skipImages = 0;
        };

        static Uint GetNormFallbackComponentCount(TextureInternalFormat format) {
            switch (format) {
            case TextureInternalFormat::R8Snorm:
            case TextureInternalFormat::R16:
            case TextureInternalFormat::R16Snorm:
                return 1;
            case TextureInternalFormat::RG8Snorm:
            case TextureInternalFormat::RG16:
            case TextureInternalFormat::RG16Snorm:
                return 2;
            case TextureInternalFormat::RGB8Snorm:
            case TextureInternalFormat::RGB16:
            case TextureInternalFormat::RGB10:  // stored as RGB16 (UNorm16 shadow)
            case TextureInternalFormat::RGB12:  // stored as RGB16 (UNorm16 shadow)
            case TextureInternalFormat::RGB16Snorm:
                return 3;
            case TextureInternalFormat::RGBA8Snorm:
            case TextureInternalFormat::RGBA16:
            case TextureInternalFormat::RGBA12: // stored as RGBA16 (UNorm16 shadow)
            case TextureInternalFormat::RGBA16Snorm:
                return 4;
            default:
                return 0;
            }
        }

        static Bool IsSnormFallbackFormat(TextureInternalFormat format) {
            switch (format) {
            case TextureInternalFormat::R8Snorm:
            case TextureInternalFormat::RG8Snorm:
            case TextureInternalFormat::RGB8Snorm:
            case TextureInternalFormat::RGBA8Snorm:
            case TextureInternalFormat::R16Snorm:
            case TextureInternalFormat::RG16Snorm:
            case TextureInternalFormat::RGB16Snorm:
            case TextureInternalFormat::RGBA16Snorm:
                return true;
            default:
                return false;
            }
        }

        static Bool IsNorm8FallbackFormat(TextureInternalFormat format) {
            switch (format) {
            case TextureInternalFormat::R8Snorm:
            case TextureInternalFormat::RG8Snorm:
            case TextureInternalFormat::RGB8Snorm:
            case TextureInternalFormat::RGBA8Snorm:
                return true;
            default:
                return false;
            }
        }

        static const void* PrepareNormFloatFallbackUpload(TextureInternalFormat format,
                                                          const IntVec3& texelSize,
                                                          const void* data,
                                                          SizeT byteSize,
                                                          GLenum uploadType,
                                                          Vector<Float>& convertedData) {
            const Uint componentCount = GetNormFallbackComponentCount(format);
            if (componentCount == 0 || uploadType != GL_FLOAT || data == nullptr || byteSize == 0) {
                return data;
            }

            const SizeT texelCount = static_cast<SizeT>(std::max(texelSize.x(), 0)) *
                                     static_cast<SizeT>(std::max(texelSize.y(), 0)) *
                                     static_cast<SizeT>(std::max(texelSize.z(), 0));
            const SizeT componentTotal = texelCount * static_cast<SizeT>(componentCount);
            const SizeT sourceComponentSize = IsNorm8FallbackFormat(format) ? sizeof(Int8) : sizeof(Uint16);
            const SizeT sourceComponentTotal = byteSize / sourceComponentSize;
            if (componentTotal == 0 || sourceComponentTotal == 0) {
                return nullptr;
            }

            convertedData.assign(componentTotal, 0.0f);
            const SizeT copyComponentTotal = std::min(componentTotal, sourceComponentTotal);
            if (IsNorm8FallbackFormat(format)) {
                const Int8* src = static_cast<const Int8*>(data);
                constexpr Float invMaxSnorm8 = 1.0f / 127.0f;
                for (SizeT i = 0; i < copyComponentTotal; ++i) {
                    convertedData[i] = std::max(static_cast<Float>(src[i]) * invMaxSnorm8, -1.0f);
                }
            } else if (IsSnormFallbackFormat(format)) {
                const Int16* src = static_cast<const Int16*>(data);
                constexpr Float invMaxSnorm16 = 1.0f / 32767.0f;
                for (SizeT i = 0; i < copyComponentTotal; ++i) {
                    convertedData[i] = std::max(static_cast<Float>(src[i]) * invMaxSnorm16, -1.0f);
                }
            } else {
                const Uint16* src = static_cast<const Uint16*>(data);
                constexpr Float invMaxUnorm16 = 1.0f / 65535.0f;
                for (SizeT i = 0; i < copyComponentTotal; ++i) {
                    convertedData[i] = static_cast<Float>(src[i]) * invMaxUnorm16;
                }
            }
            return convertedData.data();
        }

        // RGB565/RGB5_A1 shadow data is stored as 8-bit unorm; uploading it as GL_UNSIGNED_BYTE
        // leaves the 8-bit -> 5/6-bit requantization to the driver, whose rounding direction is
        // implementation-defined: Adreno rounds to nearest (lossless round trip) but Mali floors,
        // drifting mid-range texels one 5-bit step down and failing the KHR-GL3x
        // pixelstoragemodes.teximage3d rgb565/rgb5a1 1/32-eps checks. Repack the shadow rows into
        // the packed 16-bit client type with round-to-nearest instead - that recovers the original
        // 5/6-bit values exactly (the shadow expansion round(v * 255 / max) is injective), so the
        // driver stores them verbatim with no requantization left to its discretion. 4-bit formats
        // (RGBA4) are exempt: their 8-bit expansion (v * 17) is exact under either rounding.
        // Always retargets *inOutType for these formats (even for null data) so every upload of a
        // level uses the same client type.
        static const void* PreparePackedNormUpload(TextureInternalFormat format, const IntVec3& texelSize,
                                                   const void* data, SizeT byteSize, GLenum* inOutType,
                                                   Vector<Uint8>& packedData) {
            if (format != TextureInternalFormat::RGB5 && format != TextureInternalFormat::RGB5A1) {
                return data;
            }
            const Bool hasAlpha = format == TextureInternalFormat::RGB5A1;
            const GLenum packedType = hasAlpha ? GL_UNSIGNED_SHORT_5_5_5_1 : GL_UNSIGNED_SHORT_5_6_5;
            // Idempotent across a region's level loop: glType is shared, so later levels arrive with
            // the already-retargeted packed type and must still be converted.
            if (*inOutType != GL_UNSIGNED_BYTE && *inOutType != packedType) {
                return data;
            }
            *inOutType = packedType;
            if (data == nullptr || byteSize == 0) {
                return data;
            }
            const SizeT srcPixelBytes = hasAlpha ? 4 : 3;
            const SizeT texelCount = std::min(static_cast<SizeT>(std::max(texelSize.x(), 0)) *
                                                  static_cast<SizeT>(std::max(texelSize.y(), 0)) *
                                                  static_cast<SizeT>(std::max(texelSize.z(), 1)),
                                              byteSize / srcPixelBytes);
            packedData.resize(texelCount * sizeof(Uint16));
            const Uint8* src = static_cast<const Uint8*>(data);
            auto* dst = reinterpret_cast<Uint16*>(packedData.data());
            for (SizeT i = 0; i < texelCount; ++i, src += srcPixelBytes) {
                const Uint32 r = (static_cast<Uint32>(src[0]) * 31u + 127u) / 255u;
                const Uint32 b = (static_cast<Uint32>(src[2]) * 31u + 127u) / 255u;
                if (hasAlpha) {
                    const Uint32 g = (static_cast<Uint32>(src[1]) * 31u + 127u) / 255u;
                    dst[i] = static_cast<Uint16>((r << 11) | (g << 6) | (b << 1) | (src[3] >= 128 ? 1u : 0u));
                } else {
                    const Uint32 g = (static_cast<Uint32>(src[1]) * 63u + 127u) / 255u;
                    dst[i] = static_cast<Uint16>((r << 11) | (g << 5) | b);
                }
            }
            return packedData.data();
        }

        void BackendTextureObject::SyncMipmapsToBackend(
            const SharedPtr<MG_State::GLState::ITextureObject>& stateTextureObject) {
            if (!stateTextureObject) {
                MGLOG_E("State texture object is null, cannot sync to backend.");
                return;
            }

#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif

            // First-level clean gate (see the member comment): three version compares and no
            // virtual shape walk. Every mutation the slower probe below would catch bumps one of
            // the keys - shape via the context's sampling-resolution generation (coarse: any
            // texture's shape churn re-opens every gate, which only costs a fall-through to the
            // probe), CPU pixels via the content version, samples/fixed-locations via the params
            // version - and backend-side storage resets clear m_isInitialized. Restricted to
            // Mipmap storage like the probe fast path: a buffer texture's backing store can move
            // without any of these keys noticing.
            if (m_isInitialized && m_syncedShapeContextId != 0 && MG_State::pGLContext &&
                m_syncedShapeContextId == MG_State::pGLContext->GetTextureContextId() &&
                m_syncedShapeGeneration == MG_State::pGLContext->GetSamplingResolutionGeneration() &&
                m_syncedContentVersion == stateTextureObject->GetContentVersion() &&
                m_syncedShapeParamsVersion == stateTextureObject->GetTextureParamsVersion() &&
                stateTextureObject->GetStorageType() == TextureStorageType::Mipmap) {
                return;
            }

            MGLOG_D("Syncing texture mipmaps with backend ID %u to backend for state ID %u", m_backendTextureId,
                    stateTextureObject->GetExternalIndex());

            GLenum target = ConvertTextureTargetToBackendGLEnum(stateTextureObject->GetTarget());
            auto targetInternal = stateTextureObject->GetTarget();
            MGLOG_D("    Texture target for syncing is %s",
                    MG_Util::ConvertTextureTargetToString(targetInternal).c_str());
            if (!IsSupportedTextureTarget(targetInternal)) {
                MGLOG_E("    Texture target %s is not supported, skipping.",
                        MG_Util::ConvertTextureTargetToString(targetInternal).c_str());
                return;
            }

            // The texture needs to be regenerated completely with glTexImage* calls if:
            // 1. Not initialized
            // 2. InternalFormat changed
            // 3. Size changed
            // 4. Mipmap levels changed

            if (!stateTextureObject->IsComplete()) {
                MGLOG_D("Texture object with ID: %u is not complete, skipping sync.",
                        stateTextureObject->GetExternalIndex());
                return;
            }

            // Fast path: a fully-synced mipmap texture is the common per-draw case.
            // SyncNeccessaryTextures re-syncs every bound texture each draw, and the
            // scratch Bind below targets the temp unit - which sequential distinct
            // textures thrash, forcing a real glBindTexture per texture per draw. When
            // nothing needs uploading, skip the bind + upload machinery entirely;
            // BindCurrentTextures() re-establishes the real sampling bindings regardless.
            // The content-version stamp short-circuits before any shape probing: it
            // bumps on every CPU-side pixel mutation, so an unchanged stamp plus an
            // unchanged shape means no level can be dirty. Shape stays a separate
            // compare because a NULL-data glTexImage changes it without touching the
            // content version.
            if (m_isInitialized && stateTextureObject->GetStorageType() == TextureStorageType::Mipmap &&
                m_syncedContentVersion != 0 &&
                m_syncedContentVersion == stateTextureObject->GetContentVersion()) {
                auto* mipmapObject =
                    static_cast<MG_State::GLState::TextureObjectMipmap*>(stateTextureObject.get());
                const auto probeBaseSize = stateTextureObject->GetBaseSize();
                StateTextureBasicInfo probe = {stateTextureObject->GetFormat(),
                                               static_cast<SizeT>(probeBaseSize.x()),
                                               static_cast<SizeT>(probeBaseSize.y()),
                                               static_cast<SizeT>(probeBaseSize.z()),
                                               static_cast<SizeT>(mipmapObject->GetMipmapLevelCount()),
                                               0,
                                               stateTextureObject->GetSamples(),
                                               stateTextureObject->HasFixedSampleLocations()};
                if (probe == m_prevTextureInfo) {
                    MGLOG_D("Texture ID %u already fully synced, skipping scratch bind + upload.",
                            m_backendTextureId);
                    // The probe just proved "fully synced" from the real state, so the cheap
                    // gate may be (re)stamped here: the coarse generation only ever goes stale
                    // from OTHER textures' churn, and this draw re-validated this one.
                    if (MG_State::pGLContext) {
                        m_syncedShapeContextId = MG_State::pGLContext->GetTextureContextId();
                        m_syncedShapeGeneration = MG_State::pGLContext->GetSamplingResolutionGeneration();
                        m_syncedShapeParamsVersion = stateTextureObject->GetTextureParamsVersion();
                    }
                    return;
                }
            }

            Bind(target);
            DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                MGLOG_D("%s(%s:%d) ES error: %s", func, file, line, MG_Util::ConvertGLEnumToString(err).c_str());
            });
            const auto baseSize = stateTextureObject->GetBaseSize();
            StateTextureBasicInfo currentTextureInfo = {stateTextureObject->GetFormat(),
                                                        static_cast<SizeT>(baseSize.x()),
                                                        static_cast<SizeT>(baseSize.y()),
                                                        static_cast<SizeT>(baseSize.z()),
                                                        0,
                                                        0,
                                                        stateTextureObject->GetSamples(),
                                                        stateTextureObject->HasFixedSampleLocations()};
            switch (stateTextureObject->GetStorageType()) {
            case TextureStorageType::Mipmap: {
                auto* textureMipmapObject =
                    static_cast<MG_State::GLState::TextureObjectMipmap*>(stateTextureObject.get());
                const auto mipmapCount = textureMipmapObject->GetMipmapLevelCount();
                currentTextureInfo.mipmapLevels = mipmapCount;

                Bool needsRegeneration = !m_isInitialized || (currentTextureInfo != m_prevTextureInfo);
                if (needsRegeneration && m_backendStorageImmutable) {
                    RecreateBackendTexture();
                    Bind(target);
                }

                const Bool canAppendMipmaps =
                    m_isInitialized &&
                    !m_imageBindableStorageRequired &&
                    !stateTextureObject->IsImmutable() &&
                    currentTextureInfo.internalFormat == m_prevTextureInfo.internalFormat &&
                    currentTextureInfo.width == m_prevTextureInfo.width &&
                    currentTextureInfo.height == m_prevTextureInfo.height &&
                    currentTextureInfo.depth == m_prevTextureInfo.depth &&
                    currentTextureInfo.bufferExternalIndex == m_prevTextureInfo.bufferExternalIndex &&
                    currentTextureInfo.samples == m_prevTextureInfo.samples &&
                    currentTextureInfo.fixedSampleLocations == m_prevTextureInfo.fixedSampleLocations &&
                    currentTextureInfo.mipmapLevels > m_prevTextureInfo.mipmapLevels &&
                    !TextureImpl::IsMultisampleTextureTarget(targetInternal);

                MGLOG_D("%s: Got texture info: %dx%dx%d, mips %d, format %s", __func__, baseSize.x(), baseSize.y(),
                        baseSize.z(), mipmapCount,
                        MG_Util::ConvertTextureInternalFormatToString(textureMipmapObject->GetFormat()).c_str());

                if (canAppendMipmaps) {
                    MGLOG_D("Texture mip count increased for backend ID %u, appending levels %zu..%zu",
                            m_backendTextureId, m_prevTextureInfo.mipmapLevels, mipmapCount - 1);

                    GLenum glInternalFormat, glType, glFormat;
                    TextureImpl::GenerateTextureFormatInfo(textureMipmapObject->GetFormat(), &glInternalFormat,
                                                           &glFormat, &glType, targetInternal);

                    const auto& uploadTargets = textureMipmapObject->GetUploadTargets();
                    ScopedDefaultUnpackState unpackState;
                    for (auto& uploadTarget : uploadTargets) {
                        for (SizeT level = m_prevTextureInfo.mipmapLevels; level < mipmapCount; ++level) {
                            auto levelTexelSize = textureMipmapObject->GetMipmapTexelSize(uploadTarget, level);
                            auto levelByteSize = textureMipmapObject->GetMipmapByteSize(uploadTarget, level);
                            bool levelDirty = textureMipmapObject->IsStorageDirty(uploadTarget, level);
                            auto glUploadTarget = ConvertTextureUploadTargetToBackendGLEnum(uploadTarget);
                            auto* pData = (levelDirty && levelByteSize != 0)
                                              ? textureMipmapObject->MapMipmapData(uploadTarget, level)
                                              : nullptr;
                            Vector<Float> convertedUploadData;
                            const void* uploadData = PrepareNormFloatFallbackUpload(
                                textureMipmapObject->GetFormat(), levelTexelSize, pData, levelByteSize, glType,
                                convertedUploadData);
                            Vector<Uint8> packedUploadData;
                            uploadData = PreparePackedNormUpload(textureMipmapObject->GetFormat(), levelTexelSize,
                                                                 uploadData, levelByteSize, &glType, packedUploadData);

                            DebugImpl::ErrorLopper::Clear();
                            BufferImpl::BindPixelUnpackBufferId(0); // no-op once the resting 0 state is pinned
                            const IntVec3 uploadSize =
                                GetBackendUploadSize(stateTextureObject->GetTarget(), levelTexelSize);
                            switch (MapToBackendTextureTarget(stateTextureObject->GetTarget())) {
                            case TextureTarget::Texture2D:
                            case TextureTarget::TextureCubeMap:
                                g_GLESFuncs.glTexImage2D(
                                    glUploadTarget, static_cast<GLint>(level), (GLint)glInternalFormat,
                                    static_cast<GLsizei>(uploadSize.x()), static_cast<GLsizei>(uploadSize.y()),
                                    0, glFormat, glType, uploadData);
                                break;
                            case TextureTarget::Texture3D:
                            case TextureTarget::Texture2DArray:
                            // ES 3.2 has GL_TEXTURE_CUBE_MAP_ARRAY natively and it stores exactly
                            // like a 2D array whose depth is 6 * the cube count.
                            case TextureTarget::TextureCubeMapArray:
                                g_GLESFuncs.glTexImage3D(
                                    glUploadTarget, static_cast<GLint>(level), (GLint)glInternalFormat,
                                    static_cast<GLsizei>(uploadSize.x()), static_cast<GLsizei>(uploadSize.y()),
                                    static_cast<GLsizei>(uploadSize.z()), 0, glFormat, glType, uploadData);
                                break;
                            default:
                                MGLOG_E("Unhandled texture target %s",
                                        MG_Util::ConvertTextureTargetToString(stateTextureObject->GetTarget()).c_str());
                                break;
                            }
                            DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__, func = __func__,
                                                          glUploadTarget, glInternalFormat, glFormat, glType,
                                                          pData](GLenum err) {
                                MGLOG_D("%s(%s:%d) ES error: %s. glTexImage*: target=%s, internalformat=%s, format=%s, "
                                        "type=%s, pixels=%p",
                                        func, file, line, MG_Util::ConvertGLEnumToString(err).c_str(),
                                        MG_Util::ConvertGLEnumToString(glUploadTarget).c_str(),
                                        MG_Util::ConvertGLEnumToString(glInternalFormat).c_str(),
                                        MG_Util::ConvertGLEnumToString(glFormat).c_str(),
                                        MG_Util::ConvertGLEnumToString(glType).c_str(), pData);
                            });
                            textureMipmapObject->MarkStorageDirty(uploadTarget, level, false);
                        }
                    }
                    needsRegeneration = false;
                }

                if (needsRegeneration) {
                    MGLOG_D("Texture state changed significantly or not initialized, regenerating texture with ID: %u",
                            m_backendTextureId);

                    // Regenerate all mipmap levels
                    GLenum glInternalFormat, glType, glFormat;
                    TextureImpl::GenerateTextureFormatInfo(textureMipmapObject->GetFormat(), &glInternalFormat,
                                                           &glFormat, &glType, targetInternal);

                    const auto& uploadTargets = textureMipmapObject->GetUploadTargets();
                    if (TextureImpl::IsMultisampleTextureTarget(targetInternal)) {
                        DebugImpl::ErrorLopper::Clear();
                        BufferImpl::BindPixelUnpackBufferId(0); // no-op once the resting 0 state is pinned
                        switch (targetInternal) {
                        case TextureTarget::Texture2DMultisample:
                            g_GLESFuncs.glTexStorage2DMultisample(
                                target, static_cast<GLsizei>(stateTextureObject->GetSamples()), glInternalFormat,
                                static_cast<GLsizei>(baseSize.x()), static_cast<GLsizei>(baseSize.y()),
                                stateTextureObject->HasFixedSampleLocations() ? GL_TRUE : GL_FALSE);
                            break;
                        case TextureTarget::Texture2DMultisampleArray:
                            g_GLESFuncs.glTexStorage3DMultisample(
                                target, static_cast<GLsizei>(stateTextureObject->GetSamples()), glInternalFormat,
                                static_cast<GLsizei>(baseSize.x()), static_cast<GLsizei>(baseSize.y()),
                                static_cast<GLsizei>(baseSize.z()),
                                stateTextureObject->HasFixedSampleLocations() ? GL_TRUE : GL_FALSE);
                            break;
                        default:
                            MOBILEGL_ASSERT(false, "Unexpected multisample target: %d", static_cast<Int>(targetInternal));
                            break;
                        }
                        m_backendStorageImmutable = true;
                        for (const auto& uploadTarget : uploadTargets) {
                            for (SizeT level = 0; level < mipmapCount; ++level) {
                                textureMipmapObject->MarkStorageDirty(uploadTarget, level, false);
                            }
                        }
                    } else if (stateTextureObject->IsImmutable() || m_imageBindableStorageRequired) {
                        DebugImpl::ErrorLopper::Clear();
                        BufferImpl::BindPixelUnpackBufferId(0); // no-op once the resting 0 state is pinned
                        const IntVec3 storageSize = GetBackendUploadSize(targetInternal, baseSize);
                        switch (MapToBackendTextureTarget(targetInternal)) {
                        case TextureTarget::Texture2D:
                        case TextureTarget::TextureCubeMap:
                            g_GLESFuncs.glTexStorage2D(target, static_cast<GLsizei>(mipmapCount), glInternalFormat,
                                                       static_cast<GLsizei>(storageSize.x()),
                                                       static_cast<GLsizei>(storageSize.y()));
                            break;
                        case TextureTarget::Texture3D:
                        case TextureTarget::Texture2DArray:
                        case TextureTarget::TextureCubeMapArray:
                            g_GLESFuncs.glTexStorage3D(target, static_cast<GLsizei>(mipmapCount), glInternalFormat,
                                                       static_cast<GLsizei>(storageSize.x()),
                                                       static_cast<GLsizei>(storageSize.y()),
                                                       static_cast<GLsizei>(storageSize.z()));
                            break;
                        default:
                            MGLOG_E("Unhandled immutable texture target %s",
                                    MG_Util::ConvertTextureTargetToString(targetInternal).c_str());
                            break;
                        }
                        DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__, func = __func__, target,
                                                      glInternalFormat](GLenum err) {
                            MGLOG_D("%s(%s:%d) ES error: %s. glTexStorage*: target=%s, internalformat=%s", func,
                                    file, line, MG_Util::ConvertGLEnumToString(err).c_str(),
                                    MG_Util::ConvertGLEnumToString(target).c_str(),
                                    MG_Util::ConvertGLEnumToString(glInternalFormat).c_str());
                        });
                        m_backendStorageImmutable = true;

                        ScopedDefaultUnpackState unpackState;
                        for (auto& uploadTarget : uploadTargets) {
                            for (SizeT level = 0; level < mipmapCount; ++level) {
                                auto levelByteSize = textureMipmapObject->GetMipmapByteSize(uploadTarget, level);
                                const bool levelDirty = textureMipmapObject->IsStorageDirty(uploadTarget, level);
                                if (levelDirty && levelByteSize != 0) {
                                    auto levelTexelSize =
                                        textureMipmapObject->GetMipmapTexelSize(uploadTarget, level);
                                    auto glUploadTarget = ConvertTextureUploadTargetToBackendGLEnum(uploadTarget);
                                    auto* pData = textureMipmapObject->MapMipmapData(uploadTarget, level);
                                    Vector<Float> convertedUploadData;
                                    const void* uploadData = PrepareNormFloatFallbackUpload(
                                        textureMipmapObject->GetFormat(), levelTexelSize, pData, levelByteSize, glType,
                                        convertedUploadData);
                                    Vector<Uint8> packedUploadData;
                                    uploadData =
                                        PreparePackedNormUpload(textureMipmapObject->GetFormat(), levelTexelSize,
                                                                uploadData, levelByteSize, &glType, packedUploadData);

                                    DebugImpl::ErrorLopper::Clear();
                                    BufferImpl::BindPixelUnpackBufferId(0); // no-op once the resting 0 state is pinned
                                    const IntVec3 uploadSize =
                                        GetBackendUploadSize(targetInternal, levelTexelSize);
                                    switch (MapToBackendTextureTarget(targetInternal)) {
                                    case TextureTarget::Texture2D:
                                    case TextureTarget::TextureCubeMap:
                                        g_GLESFuncs.glTexSubImage2D(
                                            glUploadTarget, static_cast<GLint>(level), 0, 0,
                                            static_cast<GLsizei>(uploadSize.x()),
                                            static_cast<GLsizei>(uploadSize.y()), glFormat, glType, uploadData);
                                        break;
                                    case TextureTarget::Texture3D:
                                    case TextureTarget::Texture2DArray:
                                    case TextureTarget::TextureCubeMapArray:
                                        g_GLESFuncs.glTexSubImage3D(
                                            glUploadTarget, static_cast<GLint>(level), 0, 0, 0,
                                            static_cast<GLsizei>(uploadSize.x()),
                                            static_cast<GLsizei>(uploadSize.y()),
                                            static_cast<GLsizei>(uploadSize.z()), glFormat, glType, uploadData);
                                        break;
                                    default:
                                        break;
                                    }
                                    DebugImpl::ErrorLopper::Loop(
                                        [file = __FILE__, line = __LINE__, func = __func__, glUploadTarget,
                                         glFormat, glType, pData](GLenum err) {
                                            MGLOG_D("%s(%s:%d) ES error: %s. glTexSubImage*: target=%s, format=%s, "
                                                    "type=%s, pixels=%p",
                                                    func, file, line, MG_Util::ConvertGLEnumToString(err).c_str(),
                                                    MG_Util::ConvertGLEnumToString(glUploadTarget).c_str(),
                                                    MG_Util::ConvertGLEnumToString(glFormat).c_str(),
                                                    MG_Util::ConvertGLEnumToString(glType).c_str(), pData);
                                        });
                                }
                                textureMipmapObject->MarkStorageDirty(uploadTarget, level, false);
                            }
                        }
                    } else {
                        m_backendStorageImmutable = false;
                        ScopedDefaultUnpackState unpackState;
                        for (auto& uploadTarget : uploadTargets) {
                            for (SizeT level = 0; level < mipmapCount; ++level) {
                                auto levelTexelSize = textureMipmapObject->GetMipmapTexelSize(uploadTarget, level);
                                auto levelByteSize = textureMipmapObject->GetMipmapByteSize(uploadTarget, level);
                                bool levelDirty = textureMipmapObject->IsStorageDirty(uploadTarget, level);
                                auto glUploadTarget = ConvertTextureUploadTargetToBackendGLEnum(uploadTarget);
                                auto* pData = (levelDirty && levelByteSize != 0)
                                                  ? textureMipmapObject->MapMipmapData(uploadTarget, level)
                                                  : nullptr;
                                Vector<Float> convertedUploadData;
                                const void* uploadData = PrepareNormFloatFallbackUpload(
                                    textureMipmapObject->GetFormat(), levelTexelSize, pData, levelByteSize, glType,
                                    convertedUploadData);
                                Vector<Uint8> packedUploadData;
                                uploadData =
                                    PreparePackedNormUpload(textureMipmapObject->GetFormat(), levelTexelSize,
                                                            uploadData, levelByteSize, &glType, packedUploadData);
                                MGLOG_D("%s: target: %s: syncing mip %d: %dx%dx%d, byteSize = %d, pData = %p, "
                                        "levelDirty = %s",
                                        __func__, MG_Util::ConvertTextureUploadTargetToString(uploadTarget).c_str(),
                                        level, levelTexelSize.x(), levelTexelSize.y(), levelTexelSize.z(),
                                        levelByteSize, pData, levelDirty ? "true" : "false");

                                DebugImpl::ErrorLopper::Clear();
                                BufferImpl::BindPixelUnpackBufferId(0); // no-op once the resting 0 state is pinned
                                auto textureTarget = stateTextureObject->GetTarget();
                                const IntVec3 uploadSize = GetBackendUploadSize(textureTarget, levelTexelSize);
                                switch (MapToBackendTextureTarget(textureTarget)) {
                                case TextureTarget::Texture2D:
                                case TextureTarget::TextureCubeMap: {
                                    g_GLESFuncs.glTexImage2D(
                                        glUploadTarget, static_cast<GLint>(level), (GLint)glInternalFormat,
                                        static_cast<GLsizei>(uploadSize.x()),
                                        static_cast<GLsizei>(uploadSize.y()), 0, glFormat, glType, uploadData);
                                    break;
                                }
                                case TextureTarget::Texture3D:
                                case TextureTarget::Texture2DArray:
                                case TextureTarget::TextureCubeMapArray: {
                                    g_GLESFuncs.glTexImage3D(
                                        glUploadTarget, static_cast<GLint>(level), (GLint)glInternalFormat,
                                        static_cast<GLsizei>(uploadSize.x()),
                                        static_cast<GLsizei>(uploadSize.y()),
                                        static_cast<GLsizei>(uploadSize.z()), 0, glFormat, glType, uploadData);
                                    break;
                                }
                                default: {
                                    MGLOG_E("Unhandled texture target %s",
                                            MG_Util::ConvertTextureTargetToString(textureTarget).c_str());
                                }
                                }
                                DebugImpl::ErrorLopper::Loop(
                                    [file = __FILE__, line = __LINE__, func = __func__, glUploadTarget,
                                     glInternalFormat, glFormat, glType, pData](GLenum err) {
                                        MGLOG_D("%s(%s:%d) ES error: %s. glTexImage*: target=%s, internalformat=%s, "
                                                "format=%s, type=%s, pixels=%p",
                                                func, file, line, MG_Util::ConvertGLEnumToString(err).c_str(),
                                                MG_Util::ConvertGLEnumToString(glUploadTarget).c_str(),
                                                MG_Util::ConvertGLEnumToString(glInternalFormat).c_str(),
                                                MG_Util::ConvertGLEnumToString(glFormat).c_str(),
                                                MG_Util::ConvertGLEnumToString(glType).c_str(), pData);
                                    });
                                MGLOG_D("Regenerated mipmap level %d for texture with ID: %u", level,
                                        m_backendTextureId);
                                textureMipmapObject->MarkStorageDirty(uploadTarget, level, false);
                            }
                        }
                    }

                    m_isInitialized = true;
                }

                { // Update all dirty mipmap levels
                    if (TextureImpl::IsMultisampleTextureTarget(targetInternal)) {
                        const auto& uploadTargets = textureMipmapObject->GetUploadTargets();
                        for (const auto& uploadTarget : uploadTargets) {
                            for (SizeT level = 0; level < mipmapCount; ++level) {
                                if (textureMipmapObject->IsStorageDirty(uploadTarget, level)) {
                                    textureMipmapObject->MarkStorageDirty(uploadTarget, level, false);
                                }
                            }
                        }
                        break;
                    }

                    const auto mipmapCount = textureMipmapObject->GetMipmapLevelCount();
                    GLenum glInternalFormat, glType, glFormat;
                    TextureImpl::GenerateTextureFormatInfo(textureMipmapObject->GetFormat(), &glInternalFormat,
                                                           &glFormat, &glType, targetInternal);
                    const auto& uploadTargets = textureMipmapObject->GetUploadTargets();
                    ScopedDefaultUnpackState unpackState;
                    for (auto& uploadTarget : uploadTargets) {
                        for (SizeT level = 0; level < mipmapCount; ++level) {
                            if (!textureMipmapObject->IsStorageDirty(uploadTarget, level)) {
                                continue;
                            }

                            auto byteSize = textureMipmapObject->GetMipmapByteSize(uploadTarget, level);
                            if (byteSize == 0) {
                                MGLOG_W("Mipmap level %d has no data, skipping update.", level);
                                continue;
                            }

                            if (level > 0)
                                MGLOG_D("%s: Updating dirty mip %d for texture ID %u, size: %dx%d, "
                                        "byteSize: %d",
                                        __func__, level, m_backendTextureId,
                                        textureMipmapObject->GetMipmapTexelSize(uploadTarget, level).x(),
                                        textureMipmapObject->GetMipmapTexelSize(uploadTarget, level).y(), byteSize);

                            auto glUploadTarget = ConvertTextureUploadTargetToBackendGLEnum(uploadTarget);
                            BufferImpl::BindPixelUnpackBufferId(0); // no-op once the resting 0 state is pinned
                            DebugImpl::ErrorLopper::Loop(
                                [file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                                    MGLOG_D("%s(%s:%d) ES error: %s", func, file, line,
                                            MG_Util::ConvertGLEnumToString(err).c_str());
                                });
                            auto texelSize = textureMipmapObject->GetMipmapTexelSize(uploadTarget, level);
                            const void* mipData = textureMipmapObject->MapMipmapData(uploadTarget, level);
                            Vector<Float> convertedUploadData;
                            const void* uploadData = PrepareNormFloatFallbackUpload(
                                textureMipmapObject->GetFormat(), texelSize, mipData, byteSize, glType,
                                convertedUploadData);
                            Vector<Uint8> packedUploadData;
                            uploadData = PreparePackedNormUpload(textureMipmapObject->GetFormat(), texelSize,
                                                                 uploadData, byteSize, &glType, packedUploadData);
                            const IntVec3 uploadSize =
                                GetBackendUploadSize(stateTextureObject->GetTarget(), texelSize);
                            // Sub-rect upload: when only a region of the level changed (a
                            // 16x16 sprite in a 1024x512 atlas, the per-frame lightmap) and
                            // the shadow bytes go to the driver unconverted, upload just that
                            // region with UNPACK_ROW_LENGTH striding into the level shadow.
                            // Conversion fallbacks rewrite the whole level into a fresh
                            // buffer, so they stay on the full-level path, as do targets
                            // whose backend upload size differs from the shadow's texel size.
                            const auto dirtyRegion = textureMipmapObject->GetStorageDirtyRegion(uploadTarget, level);
                            const SizeT texelCount = static_cast<SizeT>(texelSize.x()) *
                                                     static_cast<SizeT>(texelSize.y()) *
                                                     static_cast<SizeT>(std::max(texelSize.z(), 1));
                            const Bool subRectEligible =
                                uploadData == mipData && !dirtyRegion.Empty() &&
                                !dirtyRegion.CoversWholeLevel(texelSize) && texelCount > 0 &&
                                byteSize % texelCount == 0 && uploadSize.x() == texelSize.x() &&
                                uploadSize.y() == texelSize.y() &&
                                std::max(uploadSize.z(), 1) == std::max(texelSize.z(), 1);
                            const SizeT bpp = subRectEligible ? byteSize / texelCount : 0;
                            const IntVec3 regionSize = {dirtyRegion.hi.x() - dirtyRegion.lo.x(),
                                                        dirtyRegion.hi.y() - dirtyRegion.lo.y(),
                                                        dirtyRegion.hi.z() - dirtyRegion.lo.z()};
                            const SizeT levelRowBytes = static_cast<SizeT>(texelSize.x()) * bpp;
                            const SizeT levelSliceBytes = static_cast<SizeT>(texelSize.y()) * levelRowBytes;
                            const Uint8* regionPtr =
                                static_cast<const Uint8*>(uploadData) +
                                static_cast<SizeT>(dirtyRegion.lo.z()) * levelSliceBytes +
                                static_cast<SizeT>(dirtyRegion.lo.y()) * levelRowBytes +
                                static_cast<SizeT>(dirtyRegion.lo.x()) * bpp;
                            switch (MapToBackendTextureTarget(stateTextureObject->GetTarget())) {
                            case TextureTarget::Texture2D:
                            case TextureTarget::TextureCubeMap:
                                if (subRectEligible) {
                                    g_GLESFuncs.glPixelStorei(GL_UNPACK_ROW_LENGTH, texelSize.x());
                                    g_GLESFuncs.glTexSubImage2D(
                                        glUploadTarget, static_cast<GLint>(level), dirtyRegion.lo.x(),
                                        dirtyRegion.lo.y(), static_cast<GLsizei>(regionSize.x()),
                                        static_cast<GLsizei>(regionSize.y()), glFormat, glType, regionPtr);
                                    // The surrounding ScopedDefaultUnpackState shadow says 0.
                                    g_GLESFuncs.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                                } else {
                                    g_GLESFuncs.glTexSubImage2D(glUploadTarget, static_cast<GLint>(level), 0, 0,
                                                                static_cast<GLsizei>(uploadSize.x()),
                                                                static_cast<GLsizei>(uploadSize.y()), glFormat,
                                                                glType, uploadData);
                                }
                                break;
                            case TextureTarget::Texture3D:
                            case TextureTarget::Texture2DArray:
                            // ES 3.2 has GL_TEXTURE_CUBE_MAP_ARRAY natively and it stores exactly
                            // like a 2D array whose depth is 6 * the cube count.
                            case TextureTarget::TextureCubeMapArray:
                                if (subRectEligible) {
                                    g_GLESFuncs.glPixelStorei(GL_UNPACK_ROW_LENGTH, texelSize.x());
                                    g_GLESFuncs.glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, texelSize.y());
                                    g_GLESFuncs.glTexSubImage3D(
                                        glUploadTarget, static_cast<GLint>(level), dirtyRegion.lo.x(),
                                        dirtyRegion.lo.y(), dirtyRegion.lo.z(),
                                        static_cast<GLsizei>(regionSize.x()),
                                        static_cast<GLsizei>(regionSize.y()),
                                        static_cast<GLsizei>(regionSize.z()), glFormat, glType, regionPtr);
                                    g_GLESFuncs.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                                    g_GLESFuncs.glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
                                } else {
                                    g_GLESFuncs.glTexSubImage3D(glUploadTarget, static_cast<GLint>(level), 0, 0, 0,
                                                                static_cast<GLsizei>(uploadSize.x()),
                                                                static_cast<GLsizei>(uploadSize.y()),
                                                                static_cast<GLsizei>(uploadSize.z()), glFormat,
                                                                glType, uploadData);
                                }
                                break;
                            default:
                                MGLOG_E("Unhandled texture target %s",
                                        MG_Util::ConvertTextureTargetToString(stateTextureObject->GetTarget()).c_str());
                                break;
                            }
                            textureMipmapObject->MarkStorageDirty(uploadTarget, level, false);
                        }
                    }
                }
                break;
            }
            case TextureStorageType::Buffer: {
                auto* textureBufferObject =
                    static_cast<MG_State::GLState::TextureObjectBuffer*>(stateTextureObject.get());
                auto& slot = textureBufferObject->GetBufferBindingSlot();
                auto& buffer = slot.GetBoundObject();
                if (!buffer) {
                    MGLOG_D("Texture buffer object with ID: %u has no bound buffer, skipping sync.",
                            stateTextureObject->GetExternalIndex());
                    return;
                }
                auto bufferIndex = buffer->GetExternalIndex();
                currentTextureInfo.bufferExternalIndex = bufferIndex;

                Bool needsRegeneration = !m_isInitialized || (currentTextureInfo != m_prevTextureInfo);

                // Need to sync texture buffer if not synced yet
                auto* backendBufferResource = BufferImpl::EnsureBufferResource(buffer);
                if (!backendBufferResource || backendBufferResource->id == 0) {
                    MGLOG_E("Failed to sync backing buffer for texture buffer with ID: %u",
                            stateTextureObject->GetExternalIndex());
                    return;
                }

                // Bind buffer to texture
                auto backendId = backendBufferResource->id;

                GLenum glInternalFormat, glType, glFormat;
                TextureImpl::GenerateTextureFormatInfo(textureBufferObject->GetFormat(), &glInternalFormat, &glFormat,
                                                       &glType, TextureTarget::TextureBuffer);

                if (needsRegeneration) {
                    MGLOG_D("Texture state changed significantly or not initialized, regenerating texture buffer with "
                            "ID: %u, buffer ID: %u, buffer size: %zu, format: %s",
                            m_backendTextureId, backendId, buffer->GetSize(),
                            MG_Util::ConvertGLEnumToString(glInternalFormat).c_str());
                    // A texture that names a window of the buffer needs the range form; the
                    // whole-buffer forms report offset 0 and the buffer's current size, which
                    // glTexBuffer expresses more directly (and works where the range entry point
                    // is absent).
                    const SizeT rangeOffset = textureBufferObject->GetBufferRangeOffset();
                    const SizeT rangeSize = textureBufferObject->GetBufferRangeSizeInBytes();
                    if (rangeOffset == 0 && rangeSize == buffer->GetSize()) {
                        g_GLESFuncs.glTexBuffer(GL_TEXTURE_BUFFER, glInternalFormat, backendId);
                    } else if (g_GLESFuncs.glTexBufferRange != nullptr) {
                        g_GLESFuncs.glTexBufferRange(GL_TEXTURE_BUFFER, glInternalFormat, backendId,
                                                     static_cast<GLintptr>(rangeOffset),
                                                     static_cast<GLsizeiptr>(rangeSize));
                    } else {
                        MGLOG_E("Texture buffer %u names a sub-range but the driver has no "
                                "glTexBufferRange; binding the whole buffer instead",
                                stateTextureObject->GetExternalIndex());
                        g_GLESFuncs.glTexBuffer(GL_TEXTURE_BUFFER, glInternalFormat, backendId);
                    }
                    DebugImpl::ErrorLopper::Loop(
                        [file = __FILE__, line = __LINE__, func = __func__, glInternalFormat, backendId](GLenum err) {
                            MGLOG_D("%s(%s:%d) glTexBuffer(format=%s, buffer=%u) ES error: %s",
                                    func, file, line, MG_Util::ConvertGLEnumToString(glInternalFormat).c_str(),
                                    backendId, MG_Util::ConvertGLEnumToString(err).c_str());
                        });
                }
                break;
            }
            default:
                THROW_UNIMPL_EXCEPTION;
            }

            DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                MGLOG_D("%s(%s:%d) ES error: %s", func, file, line, MG_Util::ConvertGLEnumToString(err).c_str());
            });

            m_prevTextureInfo = currentTextureInfo;
            // Everything dirty at entry is uploaded (or provably has no bytes to
            // upload); stamp the version so per-draw re-syncs short-circuit until
            // the next CPU-side mutation.
            m_syncedContentVersion = stateTextureObject->GetContentVersion();
            // Same instant, so the cheap gate's keys describe exactly this synced state.
            // Only Mipmap storage may arm it - the gate refuses other storage types anyway,
            // but a stale trio must not linger on an object that later switches type.
            if (MG_State::pGLContext && stateTextureObject->GetStorageType() == TextureStorageType::Mipmap) {
                m_syncedShapeContextId = MG_State::pGLContext->GetTextureContextId();
                m_syncedShapeGeneration = MG_State::pGLContext->GetSamplingResolutionGeneration();
                m_syncedShapeParamsVersion = stateTextureObject->GetTextureParamsVersion();
            } else {
                m_syncedShapeContextId = 0;
            }
        }

        void BackendTextureObject::SyncBuiltinSamplerToBackend(
            const SharedPtr<MG_State::GLState::ITextureObject>& stateTextureObject) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif

            if (!stateTextureObject) {
                MGLOG_E("State texture object is null, cannot sync to backend.");
                return;
            }

            auto* samplerObject = stateTextureObject->GetSamplerObject().get();
            Uint currentSamplerVersion = samplerObject->GetVersion();
            if (m_syncedSamplerVersion == currentSamplerVersion) {
                MGLOG_D("Sampler parameters have not changed for texture ID: %u, skipping sync.", m_backendTextureId);
                return;
            }

            m_syncedSamplerVersion = currentSamplerVersion;

            MGLOG_D("Syncing texture built-in sampler with backend ID %u to backend for state ID %u",
                    m_backendTextureId, stateTextureObject->GetExternalIndex());

            GLenum target = ConvertTextureTargetToBackendGLEnum(stateTextureObject->GetTarget());
            auto targetInternal = stateTextureObject->GetTarget();
            MGLOG_D("    Texture target for syncing is %s",
                    MG_Util::ConvertTextureTargetToString(targetInternal).c_str());
            if (!IsSupportedTextureTarget(targetInternal)) {
                MGLOG_E("    Texture target %s is not supported, skipping.",
                        MG_Util::ConvertTextureTargetToString(targetInternal).c_str());
                return;
            }

            const auto& samplerParams = samplerObject->GetAllSamplerParameters();
            if (TextureImpl::IsMultisampleTextureTarget(targetInternal)) {
                m_cacheSamplerParameters = samplerParams;
                return;
            }

            Bind(target);
            DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                MGLOG_D("%s(%s:%d) ES error: %s", func, file, line, MG_Util::ConvertGLEnumToString(err).c_str());
            });

            // Update built-in sampler parameters
            MGLOG_D("Updating sampler parameters for texture with ID: %u", m_backendTextureId);

#define SYNC_TEX_SAMPLER_PARAM_IF_CHANGED(internalName, glName, type)                                                  \
    if (m_cacheSamplerParameters.internalName != samplerParams.internalName) {                                         \
        g_GLESFuncs.glTexParameteri(target, glName,                                                                    \
                                    MG_Util::ConvertSampler##type##ToGLEnum(samplerParams.internalName));              \
        m_cacheSamplerParameters.internalName = samplerParams.internalName;                                            \
        DebugImpl::ErrorLopper::Loop(                                                                                  \
            [file = __FILE__, line = __LINE__, func = __func__,                                                        \
             t = MG_Util::ConvertSampler##type##ToGLEnum(samplerParams.internalName)](GLenum err) {                    \
                MGLOG_D("%s(%s:%d) ES error %s, GL_TEXTURE_MIN_FILTER = %s", func, file, line,                         \
                        MG_Util::ConvertGLEnumToString(err).c_str(), MG_Util::ConvertGLEnumToString(t).c_str());       \
            });                                                                                                        \
    }

            if (m_cacheSamplerParameters.minFilter != samplerParams.minFilter ||
                m_cacheSamplerParameters.mipmapMode != samplerParams.mipmapMode) {
                g_GLESFuncs.glTexParameteri(target, GL_TEXTURE_MIN_FILTER,
                                            (GLint)ResolveBackendMinFilter(samplerParams, IsAngleLlvmpipeRenderer()));
                m_cacheSamplerParameters.minFilter = samplerParams.minFilter;
                m_cacheSamplerParameters.mipmapMode = samplerParams.mipmapMode;
            }
            if (m_cacheSamplerParameters.magFilter != samplerParams.magFilter) {
                g_GLESFuncs.glTexParameteri(
                    target, GL_TEXTURE_MAG_FILTER,
                    (GLint)MG_Util::ConvertSamplerFilterModeToGLEnum(samplerParams.magFilter, SamplerMipmapMode::None));
                m_cacheSamplerParameters.magFilter = samplerParams.magFilter;
            }
            DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                MGLOG_D("%s(%s:%d) ES error %s", func, file, line, MG_Util::ConvertGLEnumToString(err).c_str());
            });

            SYNC_TEX_SAMPLER_PARAM_IF_CHANGED(wrapS, GL_TEXTURE_WRAP_S, WrapMode)
            SYNC_TEX_SAMPLER_PARAM_IF_CHANGED(wrapT, GL_TEXTURE_WRAP_T, WrapMode)
            if (SupportsWrapR(targetInternal)) {
                SYNC_TEX_SAMPLER_PARAM_IF_CHANGED(wrapR, GL_TEXTURE_WRAP_R, WrapMode)
            } else {
                m_cacheSamplerParameters.wrapR = samplerParams.wrapR;
            }
            SYNC_TEX_SAMPLER_PARAM_IF_CHANGED(compareFunc, GL_TEXTURE_COMPARE_FUNC, CompareFunc)
            SYNC_TEX_SAMPLER_PARAM_IF_CHANGED(compareMode, GL_TEXTURE_COMPARE_MODE, CompareMode)
            if (m_cacheSamplerParameters.minLod != samplerParams.minLod) {
                g_GLESFuncs.glTexParameterf(target, GL_TEXTURE_MIN_LOD, samplerParams.minLod);
                m_cacheSamplerParameters.minLod = samplerParams.minLod;
            }
            if (m_cacheSamplerParameters.maxLod != samplerParams.maxLod) {
                g_GLESFuncs.glTexParameterf(target, GL_TEXTURE_MAX_LOD, samplerParams.maxLod);
                m_cacheSamplerParameters.maxLod = samplerParams.maxLod;
            }
            if (m_cacheSamplerParameters.maxAnisotropy != samplerParams.maxAnisotropy) {
                if (g_GLESCapabilities.SupportsTextureFilterAnisotropy) {
                    g_GLESFuncs.glTexParameterf(target, GL_TEXTURE_MAX_ANISOTROPY_EXT,
                                                samplerParams.maxAnisotropy);
                }
                // Unsupported GLES backends intentionally treat anisotropy as a
                // frontend-only no-op; remember the observed value so the cache
                // remains coherent without issuing an illegal enum every sync.
                m_cacheSamplerParameters.maxAnisotropy = samplerParams.maxAnisotropy;
            }
            DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                MGLOG_D("%s(%s:%d) ES error %s", func, file, line, MG_Util::ConvertGLEnumToString(err).c_str());
            });
#undef SYNC_TEX_SAMPLER_PARAM_IF_CHANGED
        }

        void BackendTextureObject::SyncTextureParamsToBackend(
            const SharedPtr<MG_State::GLState::ITextureObject>& stateTextureObject) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif

            if (!stateTextureObject) {
                MGLOG_E("State texture object is null, cannot sync to backend.");
                return;
            }

            Uint16 currentTextureParamsVersion = stateTextureObject->GetTextureParamsVersion();
            if (m_syncedTextureParamsVersion == currentTextureParamsVersion) {
                MGLOG_D("Texture parameters have not changed for texture ID: %u, skipping sync.", m_backendTextureId);
                return;
            }
            m_syncedTextureParamsVersion = currentTextureParamsVersion;

            MGLOG_D("Syncing texture params with backend ID %u to backend for state ID %u", m_backendTextureId,
                    stateTextureObject->GetExternalIndex());

            GLenum target = ConvertTextureTargetToBackendGLEnum(stateTextureObject->GetTarget());
            auto targetInternal = stateTextureObject->GetTarget();
            MGLOG_D("    Texture target for syncing is %s",
                    MG_Util::ConvertTextureTargetToString(targetInternal).c_str());
            if (!IsSupportedTextureTarget(targetInternal)) {
                MGLOG_E("    Texture target %s is not supported, skipping.",
                        MG_Util::ConvertTextureTargetToString(targetInternal).c_str());
                return;
            }

            // Multisample targets reject the *sampler* parameters (LOD range, border color) but
            // GL_TEXTURE_SWIZZLE_* is texture state, not sampler state, and ES accepts it on them.
            // Bailing out entirely used to drop every swizzle write on the floor, which is what the
            // frontend already assumes is legal (see GL_Texture.cpp's MS-invalid pname list, which
            // deliberately omits the swizzle enums). Note the caches for the skipped parameters are
            // still refreshed so they never look stale, but m_cacheSwizzleParams must NOT be, or the
            // change detection below would swallow the very writes we came here to emit.
            const Bool isMultisampleTarget = TextureImpl::IsMultisampleTextureTarget(targetInternal);
            if (isMultisampleTarget) {
                m_cacheLodRange = stateTextureObject->GetLevelRange();
                m_cacheBorderColor = stateTextureObject->GetBorderColor();
            }

            Bind(target);
            DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                MGLOG_D("%s(%s:%d) ES error: %s", func, file, line, MG_Util::ConvertGLEnumToString(err).c_str());
            });

            // Update texture parameters
            MGLOG_D("Updating texture parameters for texture with ID: %u", m_backendTextureId);

            const auto& levelRange = stateTextureObject->GetLevelRange();

            if (!isMultisampleTarget && m_cacheLodRange.x() != levelRange.x()) {
                g_GLESFuncs.glTexParameteri(target, GL_TEXTURE_BASE_LEVEL, static_cast<GLint>(levelRange.x()));
                m_cacheLodRange.x() = levelRange.x();
            }
            DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                MGLOG_D("%s(%s:%d) ES error %s", func, file, line, MG_Util::ConvertGLEnumToString(err).c_str());
            });
            if (!isMultisampleTarget && m_cacheLodRange.y() != levelRange.y()) {
                g_GLESFuncs.glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(levelRange.y()));
                m_cacheLodRange.y() = levelRange.y();
            }
            DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                MGLOG_D("%s(%s:%d) ES error %s", func, file, line, MG_Util::ConvertGLEnumToString(err).c_str());
            });

            // A three-channel format widened to four for a multisample target (see
            // NormalizePixelFormat) gains an alpha channel the frontend format does not have, and
            // whatever the draw that filled it wrote there is not what GL would report: a format
            // without alpha reads back as 1.0. Answer the ALPHA swizzle source with ONE so the
            // promotion stays invisible, composed with the swizzle the application asked for.
            Vec4<TextureSwizzleParam> swizzleParams = stateTextureObject->GetAllSwizzleParams();
            if (TextureImpl::BackendTextureFormatAddsAlpha(stateTextureObject->GetFormat(), targetInternal)) {
                for (SizeT channel = 0; channel < 4; ++channel) {
                    if (swizzleParams[channel] == TextureSwizzleParam::Alpha) {
                        swizzleParams[channel] = TextureSwizzleParam::One;
                    }
                }
            }
            if (swizzleParams != m_cacheSwizzleParams) {
#define SYNC_TEX_SWIZZLE_PARAM_IF_CHANGED(func, glEnum)                                                                \
    if (m_cacheSwizzleParams.func != swizzleParams.func) {                                                             \
        g_GLESFuncs.glTexParameteri(target, glEnum, MG_Util::ConvertTextureSwizzleParamToGLEnum(swizzleParams.func));  \
        m_cacheSwizzleParams.func = swizzleParams.func;                                                                \
    }
                SYNC_TEX_SWIZZLE_PARAM_IF_CHANGED(r(), GL_TEXTURE_SWIZZLE_R);
                SYNC_TEX_SWIZZLE_PARAM_IF_CHANGED(g(), GL_TEXTURE_SWIZZLE_G);
                SYNC_TEX_SWIZZLE_PARAM_IF_CHANGED(b(), GL_TEXTURE_SWIZZLE_B);
                SYNC_TEX_SWIZZLE_PARAM_IF_CHANGED(a(), GL_TEXTURE_SWIZZLE_A);
#undef SYNC_TEX_SWIZZLE_PARAM_IF_CHANGED
                m_cacheSwizzleParams = swizzleParams;
                DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                    MGLOG_D("%s(%s:%d) ES error %s", func, file, line, MG_Util::ConvertGLEnumToString(err).c_str());
                });
            }

            // GL_TEXTURE_BORDER_COLOR needs ES 3.2 or EXT/OES_texture_border_clamp; on a driver
            // without it every such call is INVALID_ENUM, so the parameter is simply not synced.
            if (!isMultisampleTarget && g_GLESCapabilities.SupportsTextureBorderClamp &&
                m_cacheBorderColor != stateTextureObject->GetBorderColor()) {
                const auto& borderColor = stateTextureObject->GetBorderColor();
                GLfloat borderColorArray[4] = {borderColor.x(), borderColor.y(), borderColor.z(), borderColor.w()};
                g_GLESFuncs.glTexParameterfv(target, GL_TEXTURE_BORDER_COLOR, borderColorArray);
                m_cacheBorderColor = borderColor;
                DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                    MGLOG_D("%s(%s:%d) ES error %s", func, file, line, MG_Util::ConvertGLEnumToString(err).c_str());
                });
            }
        }

        void ActivateTextureUnit(Uint unit) {
            if (unit == g_activeTextureUnit) {
                return;
            }
            g_GLESFuncs.glActiveTexture(GL_TEXTURE0 + unit);
            g_activeTextureUnit = unit;
        }

        void UnbindTexture(Uint unit, GLenum target) { // Activates `unit` when an unbind is issued
            auto targetN = static_cast<SizeT>(MG_Util::ConvertGLEnumToTextureTarget(target));
            if (g_boundTexturesCache[unit][targetN] == nullptr) return;

            ActivateTextureUnit(unit);
            g_GLESFuncs.glBindTexture(target, 0);
            g_boundTexturesCache[unit][targetN] = nullptr;
        }

        Uint g_activeTextureUnit = 0;
        Uint g_textureContextGeneration = 1;
        Array<Array<BackendTextureObject*, (SizeT)TextureTarget::TextureTargetCount>,
              MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS>
            g_boundTexturesCache;
        StateBackendObjectRegistry<MG_State::GLState::ITextureObject, BackendTextureObject> g_backendTextureObjects;
    } // namespace TextureImpl

    namespace FramebufferImpl {
        BackendFramebufferObject::BackendFramebufferObject() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            // Identity until a non-identity draw-buffer array forces a relocation. A framebuffer
            // that is never draw-bound never runs the recompute, so the table has to start out
            // matching what the attachment loop will physically do.
            for (Uint i = 0; i < MAX_COLOR_ATTACHMENT_SLOTS; ++i) {
                m_backendColorSlots[i] = GL_COLOR_ATTACHMENT0 + i;
            }
            g_GLESFuncs.glGenFramebuffers(1, &m_backendFBOId);
            if (m_backendFBOId == 0) {
                MGLOG_E("Failed to generate framebuffer object.");
                MGLOG_E("ES glGetError(): %s", MG_Util::ConvertGLEnumToString(g_GLESFuncs.glGetError()).c_str());
            } else {
                MGLOG_D("Generated framebuffer object with ID: %u.", m_backendFBOId);
            }
        }

        void BackendFramebufferObject::Bind(FramebufferTarget target) const {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (target == FramebufferTarget::Read)
                BindFramebufferId(GL_READ_FRAMEBUFFER, m_backendFBOId);
            else
                BindFramebufferId(GL_DRAW_FRAMEBUFFER, m_backendFBOId);
        }

        namespace {
            // Driver-level framebuffer-binding shadow (see Managers.h). Indexed by
            // FramebufferTarget {Draw, Read}.
            Array<Uint, SizeT(FramebufferTarget::FramebufferTargetCount)> g_driverFBOBindings = {0, 0};
            Array<Bool, SizeT(FramebufferTarget::FramebufferTargetCount)> g_driverFBOBindingKnown = {false, false};
        } // namespace

        void BindFramebufferId(GLenum fbTarget, Uint id) {
            const Bool bindsDraw = fbTarget == GL_DRAW_FRAMEBUFFER || fbTarget == GL_FRAMEBUFFER;
            const Bool bindsRead = fbTarget == GL_READ_FRAMEBUFFER || fbTarget == GL_FRAMEBUFFER;
            const SizeT drawIdx = SizeT(FramebufferTarget::Draw);
            const SizeT readIdx = SizeT(FramebufferTarget::Read);
            const Bool drawMatches =
                !bindsDraw || (g_driverFBOBindingKnown[drawIdx] && g_driverFBOBindings[drawIdx] == id);
            const Bool readMatches =
                !bindsRead || (g_driverFBOBindingKnown[readIdx] && g_driverFBOBindings[readIdx] == id);
            if (drawMatches && readMatches) {
                return;
            }
            g_GLESFuncs.glBindFramebuffer(fbTarget, id);
            if (bindsDraw) {
                g_driverFBOBindings[drawIdx] = id;
                g_driverFBOBindingKnown[drawIdx] = true;
            }
            if (bindsRead) {
                g_driverFBOBindings[readIdx] = id;
                g_driverFBOBindingKnown[readIdx] = true;
            }
        }

        Uint CurrentFramebufferBinding(FramebufferTarget target) {
            const SizeT idx = SizeT(target);
            if (!g_driverFBOBindingKnown[idx]) {
                // Cold path: pin the shadow from the driver once (init probes and
                // pre-shadow code bind raw but restore what they found).
                GLint binding = 0;
                g_GLESFuncs.glGetIntegerv(
                    target == FramebufferTarget::Read ? GL_READ_FRAMEBUFFER_BINDING : GL_DRAW_FRAMEBUFFER_BINDING,
                    &binding);
                g_driverFBOBindings[idx] = static_cast<Uint>(binding);
                g_driverFBOBindingKnown[idx] = true;
            }
            return g_driverFBOBindings[idx];
        }

        void InvalidateFramebufferBindingCache() {
            g_driverFBOBindings = {0, 0};
            g_driverFBOBindingKnown = {false, false};
            // The per-target "already synced" memo describes work pushed into the ES context
            // that is being left or replaced. Both callers (MakeCurrent, DestroyEGLContext)
            // mean the context may have been reset under us, so claim nothing is synced:
            // a null object never matches a real binding, so SyncCurrentFBO re-pushes.
            g_fboSyncedSlotVersions = {0};
            g_fboSyncedObjectVersions = {0};
            g_fboSyncedObjects = {};
        }

        void BackendFramebufferObject::InvalidateSyncedState() {
            std::fill(std::begin(m_frontendDrawBuffers), std::end(m_frontendDrawBuffers),
                      FramebufferAttachmentType::Unknown);
            std::fill(std::begin(m_backendDrawBuffers), std::end(m_backendDrawBuffers), GL_NONE);
            // NOTE: this does NOT empty the backend ES framebuffer - m_backendFBOId keeps every
            // attachment it had, possibly under a non-identity permutation. Declaring the table
            // identity here is safe only because every attachment version below is invalidated too,
            // so the next sync re-attaches all non-empty attachments at their identity points AND
            // (see SyncToBackend's attachment loop) detaches any colour point whose frontend owner
            // is empty. Without that detach a stale image would survive under a point the table now
            // claims for a different, empty attachment.
            for (Uint i = 0; i < MAX_COLOR_ATTACHMENT_SLOTS; ++i) {
                m_backendColorSlots[i] = GL_COLOR_ATTACHMENT0 + i;
            }
            m_frontendReadBuffer = FramebufferAttachmentType::Unknown;
            m_backendReadBuffer = GL_NONE;
            std::fill(m_syncedFrontendAttachmentVersions.begin(), m_syncedFrontendAttachmentVersions.end(),
                      static_cast<Uint16>(~0u));
        }

        static Bool SyncAttachmentObject(GLenum glFBOTarget,
                                         const MG_State::GLState::FramebufferAttachmentObject& attachmentObject,
                                         GLenum glBackendAttachment) {
            if (attachmentObject.IsTexture()) {
                const auto& textureObject = attachmentObject.GetTexture();
                SharedPtr<TextureImpl::BackendTextureObject> backendTextureObject;
                if (auto* backendTextureSlot = TextureImpl::g_backendTextureObjects.Find(textureObject.get())) {
                    backendTextureObject = *backendTextureSlot;
                } else {
                    auto& newTextureSlot = TextureImpl::g_backendTextureObjects.GetOrCreate(textureObject);
                    if (!newTextureSlot) {
                        newTextureSlot = MakeShared<TextureImpl::BackendTextureObject>();
                    }
                    backendTextureObject = newTextureSlot;
                }
                if (!backendTextureObject) {
                    MGLOG_E("%s: No backend texture found for FBO attachment, cannot bind texture.", __func__);
                    return false;
                }
                backendTextureObject->SyncMipmapsToBackend(textureObject);
                if (attachmentObject.IsLayered()) {
                    g_GLESFuncs.glFramebufferTexture(glFBOTarget, glBackendAttachment,
                                                     backendTextureObject->GetBackendTextureId(),
                                                     static_cast<GLint>(attachmentObject.GetTextureLevel()));
                } else if (const auto uploadTarget = attachmentObject.GetTextureUploadTarget();
                           uploadTarget == TextureUploadTarget::Texture3D ||
                           uploadTarget == TextureUploadTarget::Texture2DArray ||
                           uploadTarget == TextureUploadTarget::Texture1DArray ||
                           uploadTarget == TextureUploadTarget::CubeMapArray ||
                           uploadTarget == TextureUploadTarget::Texture2DMultisampleArray) {
                    // Single slice/layer of a 3D or array texture: ES has no
                    // glFramebufferTexture3D, layers attach via glFramebufferTextureLayer.
                    g_GLESFuncs.glFramebufferTextureLayer(glFBOTarget, glBackendAttachment,
                                                          backendTextureObject->GetBackendTextureId(),
                                                          static_cast<GLint>(attachmentObject.GetTextureLevel()),
                                                          static_cast<GLint>(attachmentObject.GetTextureLayer()));
                } else {
                    auto glTextureTarget = TextureImpl::ConvertTextureUploadTargetToBackendGLEnum(
                        attachmentObject.GetTextureUploadTarget());
                    if (glTextureTarget == GL_UNKNOWN_MGL) {
                        glTextureTarget = TextureImpl::ConvertTextureTargetToBackendGLEnum(textureObject->GetTarget());
                    }
                    // glBindTexture rejects cube-face enums (INVALID_ENUM with no
                    // bind, while Bind() would still record the cube-map cache slot
                    // as bound): bind via the owning cube target; the attach below
                    // keeps the face target.
                    const Bool isCubeFace = glTextureTarget >= GL_TEXTURE_CUBE_MAP_POSITIVE_X &&
                                            glTextureTarget <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z;
                    backendTextureObject->Bind(isCubeFace ? GL_TEXTURE_CUBE_MAP : glTextureTarget);
                    g_GLESFuncs.glFramebufferTexture2D(glFBOTarget, glBackendAttachment, glTextureTarget,
                                                       backendTextureObject->GetBackendTextureId(),
                                                       static_cast<GLint>(attachmentObject.GetTextureLevel()));
                }
            } else if (attachmentObject.IsRenderbuffer()) {
                const auto& renderbufferObject = attachmentObject.GetRenderbuffer();
                SharedPtr<RenderbufferImpl::BackendRenderbufferObject> backendRenderbufferObject;
                if (auto* backendRenderbufferSlot =
                        RenderbufferImpl::g_backendRenderbufferObjects.Find(renderbufferObject.get())) {
                    backendRenderbufferObject = *backendRenderbufferSlot;
                } else {
                    auto& newRenderbufferSlot =
                        RenderbufferImpl::g_backendRenderbufferObjects.GetOrCreate(renderbufferObject);
                    if (!newRenderbufferSlot) {
                        newRenderbufferSlot = MakeShared<RenderbufferImpl::BackendRenderbufferObject>();
                    }
                    backendRenderbufferObject = newRenderbufferSlot;
                }

                backendRenderbufferObject->SyncToBackend(renderbufferObject);
                backendRenderbufferObject->Bind();
                g_GLESFuncs.glFramebufferRenderbuffer(glFBOTarget, glBackendAttachment, GL_RENDERBUFFER,
                                                      backendRenderbufferObject->GetBackendRenderbufferId());
            }
            return true;
        }

        static Bool IsSnormFormat(TextureInternalFormat format) {
            switch (format) {
            case TextureInternalFormat::R8Snorm:
            case TextureInternalFormat::RG8Snorm:
            case TextureInternalFormat::RGB8Snorm:
            case TextureInternalFormat::RGBA8Snorm:
            case TextureInternalFormat::R16Snorm:
            case TextureInternalFormat::RG16Snorm:
            case TextureInternalFormat::RGB16Snorm:
            case TextureInternalFormat::RGBA16Snorm:
                return true;
            default:
                return false;
            }
        }

        static Bool IsUnormFormat(TextureInternalFormat format) {
            switch (format) {
            case TextureInternalFormat::R16:
            case TextureInternalFormat::RG16:
            case TextureInternalFormat::RGB16:
            case TextureInternalFormat::RGBA16:
                return true;
            default:
                return false;
            }
        }

        static Bool IsSnormFallbackAttachment(
            const MG_State::GLState::FramebufferAttachmentObject& attachmentObject) {
            if (attachmentObject.IsTexture()) {
                const auto& textureObject = attachmentObject.GetTexture();
                return textureObject && IsSnormFormat(textureObject->GetFormat()) &&
                       TextureImpl::ShouldUseCaveatTextureFormat(textureObject->GetFormat(), textureObject->GetTarget());
            }
            if (attachmentObject.IsRenderbuffer()) {
                const auto& renderbufferObject = attachmentObject.GetRenderbuffer();
                return renderbufferObject &&
                       IsSnormFormat(renderbufferObject->GetInternalFormat()) &&
                       TextureImpl::ShouldUseCaveatRenderbufferFormat(renderbufferObject->GetInternalFormat());
            }
            return false;
        }

        static Bool IsUnormFallbackAttachment(
            const MG_State::GLState::FramebufferAttachmentObject& attachmentObject) {
            if (attachmentObject.IsTexture()) {
                const auto& textureObject = attachmentObject.GetTexture();
                return textureObject && IsUnormFormat(textureObject->GetFormat()) &&
                       TextureImpl::ShouldUseCaveatTextureFormat(textureObject->GetFormat(), textureObject->GetTarget());
            }
            if (attachmentObject.IsRenderbuffer()) {
                const auto& renderbufferObject = attachmentObject.GetRenderbuffer();
                return renderbufferObject &&
                       IsUnormFormat(renderbufferObject->GetInternalFormat()) &&
                       TextureImpl::ShouldUseCaveatRenderbufferFormat(renderbufferObject->GetInternalFormat());
            }
            return false;
        }

        Bool IsFixedPointFallbackReadAttachment() {
            const auto& readFBO =
                MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Read).GetBoundObject();
            if (!readFBO) {
                return false;
            }
            const auto readBuffer = readFBO->GetReadBuffer();
            if (readBuffer < FramebufferAttachmentType::Color0 || readBuffer > FramebufferAttachmentType::Color31) {
                return false;
            }
            // Any signed-normalized attachment, not just the ones currently substituted:
            // ES has no GL_CLAMP_READ_COLOR at all, so even a natively stored SNORM buffer
            // hands back the negative half that desktop GL clamps away.
            const auto& attachmentObject = readFBO->GetAttachment(readBuffer);
            if (attachmentObject.IsTexture()) {
                const auto& textureObject = attachmentObject.GetTexture();
                return textureObject && IsSnormFormat(textureObject->GetFormat());
            }
            if (attachmentObject.IsRenderbuffer()) {
                const auto& renderbufferObject = attachmentObject.GetRenderbuffer();
                return renderbufferObject && IsSnormFormat(renderbufferObject->GetInternalFormat());
            }
            return false;
        }

        void BackendFramebufferObject::SyncReadBufferToBackend(
            const SharedPtr<MG_State::GLState::FramebufferObject>& stateFBOObject) {
            if (!stateFBOObject) {
                return;
            }
            auto frontendReadBuf = stateFBOObject->GetReadBuffer();
            if (frontendReadBuf == m_frontendReadBuffer) {
                return;
            }
            m_frontendReadBuffer = frontendReadBuf;

            GLenum glBackendReadBuffer = GetBackendAttachmentType(frontendReadBuf);
            if (m_backendReadBuffer != glBackendReadBuffer) {
                m_backendReadBuffer = glBackendReadBuffer;
                // glReadBuffer targets whatever FBO is bound to GL_READ_FRAMEBUFFER. When this is
                // reached from SyncCurrentFBO's "same FBO as draw" skip path the backend FBO was
                // only bound as DRAW, so bind it as READ first to route the read buffer correctly.
                Bind(FramebufferTarget::Read);
                g_GLESFuncs.glReadBuffer(glBackendReadBuffer);
            }
        }

        Bool BackendFramebufferObject::RecomputeBackendColorSlots(
            const FramebufferObject::FramebufferAttachmentArray& stateDrawBuffers) {
            // Only the first GL_MAX_COLOR_ATTACHMENTS points exist in the backend. The frontend's own
            // limit (ValidateColorAttachmentInRange, which reads the clamped
            // GetDynamicParameters().MaxColorAttachments) is never larger than this raw ES cap, so an
            // index the frontend accepted is always < slotCount. Indices at or above it can never own
            // an image and stay on their identity point - never touched, never a GL error.
            const Uint slotCount =
                std::min<Uint>(MAX_COLOR_ATTACHMENT_SLOTS,
                               static_cast<Uint>(std::max<Int>(g_GLESCapabilities.MaxColorAttachments, 1)));

            GLenum newSlots[MAX_COLOR_ATTACHMENT_SLOTS];
            for (Uint i = 0; i < MAX_COLOR_ATTACHMENT_SLOTS; ++i) {
                newSlots[i] = GL_COLOR_ATTACHMENT0 + i;
            }
            Bool assigned[MAX_COLOR_ATTACHMENT_SLOTS] = {false};
            Bool slotTaken[MAX_COLOR_ATTACHMENT_SLOTS] = {false};

            // 1. ES pins draw-buffer slot s to GL_COLOR_ATTACHMENTs, so an attachment named by draw
            //    buffer slot s has no choice: its image must sit at backend point s. This has to
            //    agree with the compaction the caller just pushed through glDrawBuffers.
            for (Uint s = 0; s < FramebufferObject::MAX_DRAW_BUFFERS && s < slotCount; ++s) {
                const auto frontendBuf = stateDrawBuffers[s];
                if (frontendBuf < FramebufferAttachmentType::Color0 ||
                    frontendBuf > FramebufferAttachmentType::Color31) {
                    continue; // GL_NONE, or a default-framebuffer FRONT/BACK token: never relocated.
                }
                const Uint a =
                    static_cast<Uint>(frontendBuf) - static_cast<Uint>(FramebufferAttachmentType::Color0);
                // Neither guard may ever fire: a duplicate draw buffer is already INVALID_OPERATION
                // and an out-of-range one is rejected by ValidateColorAttachmentInRange. If one did
                // fire the table would disagree with the glDrawBuffers the caller already issued,
                // which is the exact non-injectivity this table exists to remove.
                MOBILEGL_ASSERT(a < slotCount && !assigned[a],
                                "Draw buffer %u names colour attachment %u which is out of range or duplicated.", s,
                                a);
                if (a >= slotCount || assigned[a]) {
                    continue;
                }
                newSlots[a] = GL_COLOR_ATTACHMENT0 + s;
                assigned[a] = true;
                slotTaken[s] = true;
            }

            // 2. Everything else keeps its identity point when that point survived step 1. This is
            //    what makes the ordinary drawBuffers[s] == COLOR_ATTACHMENTs case a strict no-op:
            //    the table stays identity, nothing moves, no attachment is re-issued.
            for (Uint a = 0; a < slotCount; ++a) {
                if (assigned[a] || slotTaken[a]) {
                    continue;
                }
                newSlots[a] = GL_COLOR_ATTACHMENT0 + a;
                assigned[a] = true;
                slotTaken[a] = true;
            }

            // 3. What is left are attachments whose identity point step 1 took away. Park them on the
            //    lowest free point. They are not draw buffers, so nothing is rendered through them;
            //    they only have to stay addressable for glReadBuffer and blits, and the map has to
            //    stay injective so reading one of them cannot land on another's image.
            for (Uint a = 0; a < slotCount; ++a) {
                if (assigned[a]) {
                    continue;
                }
                for (Uint s = 0; s < slotCount; ++s) {
                    if (!slotTaken[s]) {
                        newSlots[a] = GL_COLOR_ATTACHMENT0 + s;
                        assigned[a] = true;
                        slotTaken[s] = true;
                        break;
                    }
                }
            }

            Bool moved = false;
            for (Uint a = 0; a < MAX_COLOR_ATTACHMENT_SLOTS; ++a) {
                if (m_backendColorSlots[a] == newSlots[a]) {
                    continue;
                }
                m_backendColorSlots[a] = newSlots[a];
                moved = true;
                // This attachment's image now belongs at a different backend point. Its frontend
                // version has not changed, so the attachment loop would skip it; force it.
                m_syncedFrontendAttachmentVersions[static_cast<SizeT>(FramebufferAttachmentType::Color0) + a] =
                    static_cast<Uint16>(~0u);
            }
            return moved;
        }

        void BackendFramebufferObject::SyncToBackend(
            const SharedPtr<MG_State::GLState::FramebufferObject>& stateFBOObject, FramebufferTarget asTarget) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (!stateFBOObject) {
                MGLOG_E("State FBO object is null, cannot sync to backend.");
                return;
            }
            MGLOG_D("Syncing FBO with backend ID %u to backend for state ID %u, as %s FBO", m_backendFBOId,
                    stateFBOObject->GetExternalIndex(), (asTarget == FramebufferTarget::Draw ? "DRAW" : "READ"));
            GLenum glFBOTarget = MG_Util::ConvertFramebufferTargetToGLEnum(asTarget);
            Bind(asTarget);

            // -------------------- Connect attachments (set buffers) -----------------------
            // 1. Remap draw buffers
            auto& stateDrawBuffers = stateFBOObject->GetDrawBuffers();
            Bool drawBufferClean = false;
            if (memcmp(m_frontendDrawBuffers, stateDrawBuffers.data(),
                       FramebufferObject::MAX_DRAW_BUFFERS * sizeof(FramebufferAttachmentType)) == 0) {
                drawBufferClean = true;
            }

            // glDrawBuffers writes the state of the FBO bound to GL_DRAW_FRAMEBUFFER.
            // When this object is only bound as the READ target the call would land on
            // whatever framebuffer is draw-bound AND falsely stamp this object's memo,
            // so the later draw-target sync skips as "clean" while the real state is
            // stale (Minecraft 26.x OIT: the scratch clear-FBO kept draw buffers NONE
            // from its blit-destination configuration, silently dropping every
            // offscreen color clear).
            if (!drawBufferClean && asTarget == FramebufferTarget::Draw) {
                memcpy(m_frontendDrawBuffers, stateDrawBuffers.data(),
                       FramebufferObject::MAX_DRAW_BUFFERS * sizeof(FramebufferAttachmentType));
                std::fill(m_backendDrawBuffers, m_backendDrawBuffers + FramebufferObject::MAX_DRAW_BUFFERS, GL_NONE);
                int nEffectiveBuffers = 0;
                for (GLint i = 0; i < FramebufferObject::MAX_DRAW_BUFFERS; ++i) {
                    auto& frontendBuf = stateDrawBuffers[i];
                    if (frontendBuf == FramebufferAttachmentType::None) {
                        m_backendDrawBuffers[i] = GL_NONE;
                        continue;
                    }

                    // Create compacted mapping
                    if (frontendBuf == FramebufferAttachmentType::FrontLeft ||
                        frontendBuf == FramebufferAttachmentType::FrontRight ||
                        frontendBuf == FramebufferAttachmentType::BackLeft ||
                        frontendBuf == FramebufferAttachmentType::BackRight) {
                        MGLOG_D("%s: frontend buf token found for default fbo, shouldn't remap", __func__);
                        m_backendDrawBuffers[i] = MG_Util::ConvertFramebufferAttachmentTypeToGLEnum(frontendBuf);
                    } else {
                        m_backendDrawBuffers[i] = GL_COLOR_ATTACHMENT0 + i;
                    }
                    nEffectiveBuffers = i + 1;
                }
                g_GLESFuncs.glDrawBuffers(nEffectiveBuffers, m_backendDrawBuffers);
                // The line above pinned backend point s to draw-buffer slot s, so the images have to
                // be moved under those points. Rebuild the whole colour map and, when anything moved,
                // also drop the read-buffer memo: SyncReadBufferToBackend keys it on the frontend
                // enum alone, which does not change when the point under it does.
                if (RecomputeBackendColorSlots(stateDrawBuffers)) {
                    m_frontendReadBuffer = FramebufferAttachmentType::Unknown;
                }
                MGLOG_D("DBAPPLY beFbo=%u target=%d n=%d db0=0x%x feDb0=%d", m_backendFBOId, (int)asTarget,
                        nEffectiveBuffers, m_backendDrawBuffers[0], (int)stateDrawBuffers[0]);
            }

            if (asTarget == FramebufferTarget::Draw) {
                Uint32 snormClampOutputMask = 0;
                Uint32 unormClampOutputMask = 0;
                for (Uint i = 0; i < FramebufferObject::MAX_DRAW_BUFFERS && i < 32; ++i) {
                    const auto frontendBuf = stateDrawBuffers[i];
                    if (frontendBuf < FramebufferAttachmentType::Color0 ||
                        frontendBuf > FramebufferAttachmentType::Color31) {
                        continue;
                    }
                    const auto& attachmentObject = stateFBOObject->GetAttachment(frontendBuf);
                    if (IsSnormFallbackAttachment(attachmentObject)) {
                        snormClampOutputMask |= (1u << i);
                    } else if (IsUnormFallbackAttachment(attachmentObject)) {
                        unormClampOutputMask |= (1u << i);
                    }
                }
                PrgramImpl::g_snormFallbackClampOutputMask = snormClampOutputMask;
                PrgramImpl::g_unormFallbackClampOutputMask = unormClampOutputMask;
            }

            // 2. Remap read buffer. glReadBuffer writes the READ-bound FBO's state, so
            // only apply (and stamp the memo) when this object is bound as READ.
            if (asTarget == FramebufferTarget::Read) {
                SyncReadBufferToBackend(stateFBOObject);
            }

            // -------------------- Attach texture to backend FBO -----------------------
            const auto& attachments = stateFBOObject->GetAllAttachmentObjects();
            const auto& attachmentVersions = stateFBOObject->GetAllFramebufferAttachmentVersions();
            for (SizeT i = 0; i < attachments.size(); ++i) {
                const auto& attachmentObject = attachments[i];
                auto frontendType = static_cast<FramebufferAttachmentType>(i);
                GLenum glBackendAttachment = GL_NONE;
                if (frontendType >= FramebufferAttachmentType::Color0 &&
                    frontendType <= FramebufferAttachmentType::Color31)
                    glBackendAttachment = GetBackendAttachmentType(frontendType);
                else
                    glBackendAttachment = MG_Util::ConvertFramebufferAttachmentTypeToGLEnum(frontendType);

                // relevant FRONTEND!!! version should be checked and updated
                if (m_syncedFrontendAttachmentVersions[i] != attachmentVersions[i]) {
                    // SyncAttachmentObject only ever attaches: for an empty frontend attachment it
                    // returns true and issues nothing, so the point keeps whatever was there. That is
                    // what makes m_backendColorSlots a permutation of the PHYSICAL layout rather than
                    // a claim about one - a point handed to an attachment with no image would
                    // otherwise still hold the previous owner's image and glReadBuffer would return
                    // it. Bounded by GL_MAX_COLOR_ATTACHMENTS because GL_COLOR_ATTACHMENTn above the
                    // driver's limit is INVALID_ENUM, and restricted to colour points because
                    // FRONT_LEFT/BACK_LEFT and co. are not ES attachment points at all.
                    const Bool isColorPoint =
                        frontendType >= FramebufferAttachmentType::Color0 &&
                        frontendType <= FramebufferAttachmentType::Color31 &&
                        (static_cast<Int>(frontendType) - static_cast<Int>(FramebufferAttachmentType::Color0)) <
                            g_GLESCapabilities.MaxColorAttachments;
                    if (isColorPoint && attachmentObject.IsEmpty() && glBackendAttachment != GL_NONE) {
                        g_GLESFuncs.glFramebufferRenderbuffer(glFBOTarget, glBackendAttachment, GL_RENDERBUFFER, 0);
                    }
                    if (SyncAttachmentObject(glFBOTarget, attachmentObject, glBackendAttachment)) {
                        m_syncedFrontendAttachmentVersions[i] = attachmentVersions[i];
                    }
                }
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG
                else {
                    MGLOG_D("%s: Skipped SyncAttachmentObject(target=%s, frontendObj=(%dx%dx%d, %s), backendAtt=%s), "
                            "version = %u",
                            __func__, MG_Util::ConvertGLEnumToString(glFBOTarget).c_str(),
                            attachmentObject.GetSize().x(), attachmentObject.GetSize().y(),
                            attachmentObject.GetSize().z(),
                            MG_Util::ConvertFramebufferAttachmentTypeToString(frontendType).c_str(),
                            MG_Util::ConvertGLEnumToString(glBackendAttachment).c_str(),
                            m_syncedFrontendAttachmentVersions[i]);
                    if (!attachmentObject.IsTexture() && !attachmentObject.IsRenderbuffer()) {
                        continue;
                    }
                    GLint objectType = GL_NONE;
                    g_GLESFuncs.glGetFramebufferAttachmentParameteriv(
                        glFBOTarget, glBackendAttachment, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &objectType);
                    MOBILEGL_ASSERT((objectType == GL_NONE) ||
                                        (attachmentObject.IsTexture() && objectType == GL_TEXTURE) ||
                                        (attachmentObject.IsRenderbuffer() && objectType == GL_RENDERBUFFER),
                                    "Attachment type not match!");
                    GLint objectName = 0;
                    g_GLESFuncs.glGetFramebufferAttachmentParameteriv(
                        glFBOTarget, glBackendAttachment, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &objectName);
                    // Verify that the backend object's name and parameters match the frontend attachment state
                    if (attachmentObject.IsTexture()) {
                        const auto& textureObject = attachmentObject.GetTexture();
                        auto* backendTextureSlot = TextureImpl::g_backendTextureObjects.Find(textureObject.get());
                        MOBILEGL_ASSERT(backendTextureSlot != nullptr && *backendTextureSlot != nullptr,
                                        "No backend texture found while framebuffer reports texture attachment.");
                        GLuint backendTexId = (*backendTextureSlot)->GetBackendTextureId();
                        MOBILEGL_ASSERT(static_cast<GLint>(backendTexId) == objectName,
                                        "Attachment texture name mismatch between GLES (%d) and backend texture object "
                                        "(%d), frontend texture object ID=%d.",
                                        objectName, backendTexId, textureObject->GetExternalIndex());

                        GLint texLevel = 0;
                        g_GLESFuncs.glGetFramebufferAttachmentParameteriv(
                            glFBOTarget, glBackendAttachment, GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL, &texLevel);
                        MOBILEGL_ASSERT(texLevel == static_cast<GLint>(attachmentObject.GetTextureLevel()),
                                        "Attachment texture level mismatch between GLES and state object.");
                    } else if (attachmentObject.IsRenderbuffer()) {
                        const auto& renderbufferObject = attachmentObject.GetRenderbuffer();
                        auto* backendRboSlot =
                            RenderbufferImpl::g_backendRenderbufferObjects.Find(renderbufferObject.get());
                        MOBILEGL_ASSERT(
                            backendRboSlot != nullptr && *backendRboSlot != nullptr,
                            "No backend renderbuffer found while framebuffer reports renderbuffer attachment.");
                        GLuint backendRboId = (*backendRboSlot)->GetBackendRenderbufferId();
                        MOBILEGL_ASSERT(static_cast<GLint>(backendRboId) == objectName,
                                        "Attachment renderbuffer name mismatch between GLES and state object.");
                    }
                }
#endif
            }
        }

        GLenum BackendFramebufferObject::GetBackendAttachmentType(FramebufferAttachmentType frontendAtt) const {
            // Only colour attachments are ever relocated; depth/stencil, the default framebuffer's
            // FRONT/BACK names and None map straight through.
            if (frontendAtt < FramebufferAttachmentType::Color0 || frontendAtt > FramebufferAttachmentType::Color31) {
                return MG_Util::ConvertFramebufferAttachmentTypeToGLEnum(frontendAtt);
            }
            // The table is a permutation of the backend colour points, so this is the one point that
            // owns this attachment. Searching the draw-buffer array instead returned the identity
            // point for every attachment that was not a draw buffer - which is exactly the point a
            // relocated draw buffer had just taken over, so COLOR_ATTACHMENT0 read back the image of
            // whatever attachment was last made the draw buffer.
            const Uint index = static_cast<Uint>(frontendAtt) - static_cast<Uint>(FramebufferAttachmentType::Color0);
            return m_backendColorSlots[index];
        }

        StateBackendObjectRegistry<MG_State::GLState::FramebufferObject, BackendFramebufferObject>
            g_backendFramebufferObjects;
        Array<Uint16, SizeT(FramebufferTarget::FramebufferTargetCount)> g_fboSyncedSlotVersions = {0};
        // Tracks the bound FBO's object version (bumped on any attachment/drawbuffer change)
        // per target: re-attaching textures or changing draw buffers on an already-bound FBO
        // must re-sync it even when the binding-slot version has not moved.
        Array<Uint16, SizeT(FramebufferTarget::FramebufferTargetCount)> g_fboSyncedObjectVersions = {0};
        Array<MG_State::GLState::FramebufferObject*, SizeT(FramebufferTarget::FramebufferTargetCount)>
            g_fboSyncedObjects = {};
    } // namespace FramebufferImpl

    namespace ScratchFBOImpl {
        namespace {
            ScratchFramebuffer g_tempFramebuffer;
            ScratchFramebuffer g_blitReadFramebuffer;
            ScratchFramebuffer g_blitDrawFramebuffer;
            Uint g_completeTinyFBOId = 0;
            Uint g_completeTinyRBOId = 0;

            // Detach every point the shadow no longer vouches for. Used when the
            // shadow is unknown (context reset, texture id deleted while attached).
            void ScrubAllAttachments(ScratchFramebuffer& fb, GLenum fbTarget) {
                g_GLESFuncs.glFramebufferTexture2D(fbTarget, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
                g_GLESFuncs.glFramebufferTexture2D(fbTarget, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
                fb.colorTex = 0;
                fb.colorTarget = 0;
                fb.colorLevel = 0;
                fb.colorLayer = -1;
                fb.depthTex = 0;
                fb.depthTarget = 0;
                fb.depthLevel = 0;
                fb.depthHasStencil = false;
                fb.attachmentsKnown = true;
            }

            void PrepareForUse(ScratchFramebuffer& fb, GLenum fbTarget) {
                if (!fb.attachmentsKnown) {
                    ScrubAllAttachments(fb, fbTarget);
                }
            }

            // The post-attach glGetError probe below must not misread an error some
            // earlier operation left queued; drain before attaching (rare path -
            // only runs when the attachment actually changes).
            void DrainPendingGLErrors() {
                while (g_GLESFuncs.glGetError() != GL_NO_ERROR) {
                }
            }

            // Record the color point as detached when the shadow said something was
            // there; the actual detach call is the caller's (it may be replaced by
            // the new attach directly when the point is being overwritten).
            void RecordNoColor(ScratchFramebuffer& fb) {
                fb.colorTex = 0;
                fb.colorTarget = 0;
                fb.colorLevel = 0;
                fb.colorLayer = -1;
            }

            void RecordNoDepth(ScratchFramebuffer& fb) {
                fb.depthTex = 0;
                fb.depthTarget = 0;
                fb.depthLevel = 0;
                fb.depthHasStencil = false;
            }
        } // namespace

        ScratchFramebuffer& TempFramebuffer() {
            return g_tempFramebuffer;
        }
        ScratchFramebuffer& BlitReadFramebuffer() {
            return g_blitReadFramebuffer;
        }
        ScratchFramebuffer& BlitDrawFramebuffer() {
            return g_blitDrawFramebuffer;
        }

        Uint EnsureId(ScratchFramebuffer& fb) {
            if (fb.id == 0) {
                g_GLESFuncs.glGenFramebuffers(1, &fb.id);
                // A fresh FBO has nothing attached and COLOR_ATTACHMENT0 read/draw
                // buffers (the ES defaults for a non-default framebuffer).
                fb.attachmentsKnown = true;
                RecordNoColor(fb);
                RecordNoDepth(fb);
                fb.readBuffer = GL_COLOR_ATTACHMENT0;
                fb.drawBuffer = GL_COLOR_ATTACHMENT0;
            }
            return fb.id;
        }

        void EnsureColorAttachment2D(ScratchFramebuffer& fb, GLenum fbTarget, Uint tex, GLenum texTarget,
                                     GLint level) {
            PrepareForUse(fb, fbTarget);
            if (fb.depthTex != 0) {
                g_GLESFuncs.glFramebufferTexture2D(fbTarget, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
                RecordNoDepth(fb);
            }
            if (fb.colorTex == tex && fb.colorTarget == texTarget && fb.colorLevel == level && fb.colorLayer < 0) {
                return;
            }
            if (fb.colorTex != 0) {
                // Detach first: if the new attach fails, the point must read as
                // missing (incomplete FBO), not silently keep the old texture.
                g_GLESFuncs.glFramebufferTexture2D(fbTarget, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
            }
            DrainPendingGLErrors();
            g_GLESFuncs.glFramebufferTexture2D(fbTarget, GL_COLOR_ATTACHMENT0, texTarget, tex, level);
            if (g_GLESFuncs.glGetError() != GL_NO_ERROR) {
                RecordNoColor(fb);
                return;
            }
            fb.colorTex = tex;
            fb.colorTarget = texTarget;
            fb.colorLevel = level;
            fb.colorLayer = -1;
        }

        void EnsureColorAttachmentLayer(ScratchFramebuffer& fb, GLenum fbTarget, Uint tex, GLint level, GLint layer) {
            PrepareForUse(fb, fbTarget);
            if (fb.depthTex != 0) {
                g_GLESFuncs.glFramebufferTexture2D(fbTarget, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
                RecordNoDepth(fb);
            }
            if (fb.colorTex == tex && fb.colorTarget == 0 && fb.colorLevel == level && fb.colorLayer == layer) {
                return;
            }
            if (fb.colorTex != 0) {
                g_GLESFuncs.glFramebufferTexture2D(fbTarget, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
            }
            DrainPendingGLErrors();
            g_GLESFuncs.glFramebufferTextureLayer(fbTarget, GL_COLOR_ATTACHMENT0, tex, level, layer);
            if (g_GLESFuncs.glGetError() != GL_NO_ERROR) {
                RecordNoColor(fb);
                return;
            }
            fb.colorTex = tex;
            fb.colorTarget = 0;
            fb.colorLevel = level;
            fb.colorLayer = layer;
        }

        void EnsureDepthAttachment2D(ScratchFramebuffer& fb, GLenum fbTarget, Uint tex, GLenum texTarget, GLint level,
                                     Bool withStencil) {
            PrepareForUse(fb, fbTarget);
            if (fb.colorTex != 0) {
                g_GLESFuncs.glFramebufferTexture2D(fbTarget, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
                RecordNoColor(fb);
            }
            if (fb.depthTex == tex && fb.depthTarget == texTarget && fb.depthLevel == level &&
                fb.depthHasStencil == withStencil) {
                return;
            }
            if (fb.depthTex != 0) {
                // One call clears both depth and stencil points regardless of how
                // the previous attachment was made.
                g_GLESFuncs.glFramebufferTexture2D(fbTarget, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
            }
            DrainPendingGLErrors();
            g_GLESFuncs.glFramebufferTexture2D(fbTarget,
                                               withStencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT,
                                               texTarget, tex, level);
            if (g_GLESFuncs.glGetError() != GL_NO_ERROR) {
                RecordNoDepth(fb);
                return;
            }
            fb.depthTex = tex;
            fb.depthTarget = texTarget;
            fb.depthLevel = level;
            fb.depthHasStencil = withStencil;
        }

        void EnsureNoColorAttachment(ScratchFramebuffer& fb, GLenum fbTarget) {
            PrepareForUse(fb, fbTarget);
            if (fb.colorTex != 0) {
                g_GLESFuncs.glFramebufferTexture2D(fbTarget, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
                RecordNoColor(fb);
            }
        }

        void EnsureNoDepthAttachment(ScratchFramebuffer& fb, GLenum fbTarget) {
            PrepareForUse(fb, fbTarget);
            if (fb.depthTex != 0) {
                g_GLESFuncs.glFramebufferTexture2D(fbTarget, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
                RecordNoDepth(fb);
            }
        }

        void EnsureReadBuffer(ScratchFramebuffer& fb, GLenum readBuffer) {
            if (fb.readBuffer == readBuffer) {
                return;
            }
            g_GLESFuncs.glReadBuffer(readBuffer);
            fb.readBuffer = readBuffer;
        }

        void EnsureDrawBuffer(ScratchFramebuffer& fb, GLenum drawBuffer) {
            if (fb.drawBuffer == drawBuffer) {
                return;
            }
            g_GLESFuncs.glDrawBuffers(1, &drawBuffer);
            fb.drawBuffer = drawBuffer;
        }

        Uint EnsureCompleteTinyFramebufferId() {
            if (g_completeTinyFBOId != 0) {
                return g_completeTinyFBOId;
            }
            // One-time creation: the renderbuffer binding is context state with no
            // shadow, so save/restore it by query here (cold path only).
            GLint prevRenderbuffer = 0;
            g_GLESFuncs.glGetIntegerv(GL_RENDERBUFFER_BINDING, &prevRenderbuffer);
            g_GLESFuncs.glGenFramebuffers(1, &g_completeTinyFBOId);
            g_GLESFuncs.glGenRenderbuffers(1, &g_completeTinyRBOId);
            FramebufferImpl::BindFramebufferId(GL_FRAMEBUFFER, g_completeTinyFBOId);
            g_GLESFuncs.glBindRenderbuffer(GL_RENDERBUFFER, g_completeTinyRBOId);
            g_GLESFuncs.glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 1, 1);
            g_GLESFuncs.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                                                  g_completeTinyRBOId);
            const GLenum drawBuffer = GL_COLOR_ATTACHMENT0;
            g_GLESFuncs.glDrawBuffers(1, &drawBuffer);
            g_GLESFuncs.glReadBuffer(GL_COLOR_ATTACHMENT0);
            MOBILEGL_ASSERT(g_GLESFuncs.glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
                            "Scratch 1x1 framebuffer is incomplete.");
            g_GLESFuncs.glBindRenderbuffer(GL_RENDERBUFFER, static_cast<Uint>(prevRenderbuffer));
            return g_completeTinyFBOId;
        }

        void NoteTextureIdDeleted(Uint textureId) {
            if (textureId == 0) {
                return;
            }
            for (ScratchFramebuffer* fb : {&g_tempFramebuffer, &g_blitReadFramebuffer, &g_blitDrawFramebuffer}) {
                if (fb->colorTex == textureId || fb->depthTex == textureId) {
                    fb->attachmentsKnown = false;
                }
            }
        }

        void OnBackendContextDestroyed() {
            g_tempFramebuffer = {};
            g_blitReadFramebuffer = {};
            g_blitDrawFramebuffer = {};
            g_completeTinyFBOId = 0;
            g_completeTinyRBOId = 0;
        }
    } // namespace ScratchFBOImpl

    namespace PixelStoreImpl {
        namespace {
            PackState g_packState;
            Bool g_packStateKnown = false;

            void PinPackState(const PackState& value) {
                g_GLESFuncs.glPixelStorei(GL_PACK_ALIGNMENT, value.Alignment);
                g_GLESFuncs.glPixelStorei(GL_PACK_ROW_LENGTH, value.RowLength);
                g_GLESFuncs.glPixelStorei(GL_PACK_SKIP_ROWS, value.SkipRows);
                g_GLESFuncs.glPixelStorei(GL_PACK_SKIP_PIXELS, value.SkipPixels);
                g_packState = value;
                g_packStateKnown = true;
            }
        } // namespace

        void ApplyPackState(const PackState& desired) {
            if (!g_packStateKnown) {
                PinPackState(desired);
                return;
            }
            if (desired.Alignment != g_packState.Alignment) {
                g_GLESFuncs.glPixelStorei(GL_PACK_ALIGNMENT, desired.Alignment);
                g_packState.Alignment = desired.Alignment;
            }
            if (desired.RowLength != g_packState.RowLength) {
                g_GLESFuncs.glPixelStorei(GL_PACK_ROW_LENGTH, desired.RowLength);
                g_packState.RowLength = desired.RowLength;
            }
            if (desired.SkipRows != g_packState.SkipRows) {
                g_GLESFuncs.glPixelStorei(GL_PACK_SKIP_ROWS, desired.SkipRows);
                g_packState.SkipRows = desired.SkipRows;
            }
            if (desired.SkipPixels != g_packState.SkipPixels) {
                g_GLESFuncs.glPixelStorei(GL_PACK_SKIP_PIXELS, desired.SkipPixels);
                g_packState.SkipPixels = desired.SkipPixels;
            }
        }

        PackState CurrentPackState() {
            if (!g_packStateKnown) {
                // Fresh/unknown context: pin to the GL defaults (what a new context
                // starts with; writing them makes the shadow authoritative either way).
                PinPackState(PackState{});
            }
            return g_packState;
        }

        void InvalidatePackStateCache() {
            g_packStateKnown = false;
        }
    } // namespace PixelStoreImpl

    namespace PrgramImpl {
        Uint32 g_snormFallbackClampOutputMask = 0;
        Uint g_fragColorBroadcastCount = 1;
        Uint32 g_unormFallbackClampOutputMask = 0;
        Uint g_lastUsedBackendProgramId = 0;
        StateBackendObjectRegistry<MG_State::GLState::ProgramObject, BackendProgramObjectImpl> g_backendProgramObjects;

        BackendProgramObjectImpl::BackendProgramObjectImpl() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            m_backendProgramId = g_GLESFuncs.glCreateProgram();
            if (m_backendProgramId == 0) {
                MGLOG_E("Failed to create program object in backend.");
                MGLOG_E("ES glGetError(): %s", MG_Util::ConvertGLEnumToString(g_GLESFuncs.glGetError()).c_str());

            } else {
                MGLOG_D("Created backend program object with ID: %u", m_backendProgramId);
            }
        }

        BackendProgramObjectImpl::~BackendProgramObjectImpl() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
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

        void BackendProgramObjectImpl::SyncToBackend(
            const SharedPtr<MG_State::GLState::ProgramObject>& stateProgramObject) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (!stateProgramObject) {
                MGLOG_E("State program object is null, skipping backend sync.");
                return;
            }

            if (!stateProgramObject->GetLinkStatus()) {
                MGLOG_E("Program object is not linked, skipping backend sync. State program ID: %u",
                        stateProgramObject->GetExternalIndex());
                return;
            }

            MGLOG_D("Syncing program to backend. State program ID: %u, Backend ID: %u",
                    stateProgramObject->GetExternalIndex(), m_backendProgramId);
            m_backendProgramUsable = true;
            m_snormFallbackClampOutputMask = g_snormFallbackClampOutputMask;
            m_unormFallbackClampOutputMask = g_unormFallbackClampOutputMask;
            m_fragColorBroadcastCount = g_fragColorBroadcastCount;

            // Detach all existing shaders
            GLint attachedCount = 0;
            g_GLESFuncs.glGetProgramiv(m_backendProgramId, GL_ATTACHED_SHADERS, &attachedCount);
            MGLOG_D("Currently attached shaders count: %d", attachedCount);

            if (attachedCount > 0) {
                Vector<GLuint> attachedShaders(attachedCount);
                GLsizei actualCount;
                g_GLESFuncs.glGetAttachedShaders(m_backendProgramId, attachedCount, &actualCount,
                                                 attachedShaders.data());
                MGLOG_D("Detaching %d existing shaders from program %u", actualCount, m_backendProgramId);

                for (GLsizei i = 0; i < actualCount; ++i) {
                    MGLOG_D("Detaching shader ID: %u from program %u", attachedShaders[i], m_backendProgramId);
                    g_GLESFuncs.glDetachShader(m_backendProgramId, attachedShaders[i]);
                }
            }

            // Attach current shaders
            auto& attachedShaders = stateProgramObject->GetAttachedShaders();
            MGLOG_D("Attaching %zu shaders to program %u", attachedShaders.size(), m_backendProgramId);
            for (auto& shader : attachedShaders) {
                const auto& src = shader->GetShaderSource();
                const auto& stage =
                    MG_Util::ConvertGLEnumToString(MG_Util::ConvertShaderStageToGLEnum(shader->GetShaderStage()));
                MGLOG_D("Original src @ %s: \n", stage.c_str());
                MGLOG_D("%s:", src.empty() ? "" : src.c_str());
            }
            auto& shaderSpirvs = stateProgramObject->GetGeneratedSpirv();

            for (int index = 0; index < attachedShaders.size(); ++index) {
                auto& shader = attachedShaders[index];
                GLenum glShaderType = MG_Util::ConvertShaderStageToGLEnum(shader->GetShaderStage());
                GLuint backendShaderId = g_GLESFuncs.glCreateShader(glShaderType);

                if (backendShaderId == 0) {
                    MGLOG_E("Failed to create backend shader for attachment.");
                    continue;
                }
                String source;
                auto& spirvCode = shaderSpirvs[index];

                // ESSL cannot express gl_DrawID/gl_BaseInstance/gl_BaseVertex; demote them to
                // plain globals (mg_*) before handing the module to SPIRV-Cross.
                Vector<unsigned int> loweredSpirv;
                const Vector<unsigned int>* effectiveSpirv = &spirvCode;
                if (glShaderType == GL_VERTEX_SHADER &&
                    MG_Util::ShaderTranspiler::ShaderCompiler::LowerDrawParametersForEssl(spirvCode, loweredSpirv) &&
                    !loweredSpirv.empty()) {
                    effectiveSpirv = &loweredSpirv;
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
                        *effectiveSpirv, uboPrecisionSpirv) &&
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
                        *effectiveSpirv, noperspectiveSpirv) &&
                    !noperspectiveSpirv.empty()) {
                    effectiveSpirv = &noperspectiveSpirv;
                }

                // ES has no rectangle sampler, and SPIRV-Cross refuses the whole module rather
                // than approximating one. The shared pass turns the type into the 2D one and
                // divides the coordinate of every normalized-coordinate lookup by the texture
                // size, which is the whole of the difference between the two.
                Vector<unsigned int> rectLoweredSpirv;
                if (MG_Util::ShaderTranspiler::ShaderCompiler::LowerRectImages(*effectiveSpirv, rectLoweredSpirv) &&
                    !rectLoweredSpirv.empty()) {
                    effectiveSpirv = &rectLoweredSpirv;
                }

                MG_Util::ShaderTranspiler::SpvcSession spvcSession(*effectiveSpirv,
                    MG_Util::ShaderTranspiler::SessionUsageBit::Transpile);

                spvc_compiler_options options;
                spvcSession.CreateOptions(&options);

                spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_GLSL_VERSION,
                                               ResolveBackendEsslVersion());
                spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_ES, SPVC_TRUE);
                spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_VULKAN_SEMANTICS, SPVC_FALSE);

                spvcSession.SetOptions(options);

                const char* result = nullptr;
                spvcSession.Compile(&result);

                if (!result) {
                    MG_Util::ShaderTranspiler::ResultInfo r;
                    r.log += "Failed to compile the shader to GLSL: \n";
                    r.log += spvcSession.GetLastErrorString();
                    r.errc = -5;
                    MGLOG_E("%s", r.log.c_str());
                    m_backendProgramUsable = false;
                    continue;
                }

                source = result;

                source = RebindImageUniformsToFrontendUnits(std::move(source), stateProgramObject);
                source = RemoveLayoutBinding(source);
                source = ProcessOutColorLocations(source);
                source = ForceFlatIntegerVaryings(source, glShaderType);
                source = BroadcastLegacyFragColor(std::move(source), glShaderType, m_fragColorBroadcastCount);
                source = EmulateTextureLodBias(source);
                source = EmulateBaseInstanceInVertexShader(std::move(source), glShaderType);
                source = PromoteDrawParameterGlobalsToUniforms(std::move(source), glShaderType);
                source = ForceSupporterOutput(source);
                source = ClampNormFallbackOutputs(std::move(source), glShaderType,
                                                  m_snormFallbackClampOutputMask,
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

                const char* sourceCStr = source.c_str();
                MGLOG_D("Setting shader source for backend shader ID: %u\nsrc:\n%s", backendShaderId, sourceCStr);
                g_GLESFuncs.glShaderSource(backendShaderId, 1, &sourceCStr, nullptr);
                g_GLESFuncs.glCompileShader(backendShaderId);

                GLint compileStatus;
                g_GLESFuncs.glGetShaderiv(backendShaderId, GL_COMPILE_STATUS, &compileStatus);
                if (compileStatus == GL_FALSE) {
                    GLint logLength;
                    g_GLESFuncs.glGetShaderiv(backendShaderId, GL_INFO_LOG_LENGTH, &logLength);
                    Vector<GLchar> log(logLength);
                    g_GLESFuncs.glGetShaderInfoLog(backendShaderId, logLength, nullptr, log.data());
                    MGLOG_E("Shader compilation failed for backend ID %u: %s", backendShaderId, log.data());
                    m_backendProgramUsable = false;
                    continue;
                }

                MGLOG_D("Attaching shader ID: %u to program %u", backendShaderId, m_backendProgramId);
                g_GLESFuncs.glAttachShader(m_backendProgramId, backendShaderId);

                MGLOG_D("Processed shader source length: %zu", source.length());
            }

            // Transform feedback capture runs on the real driver (see XfbImpl in
            // DirectGLES.cpp), so the capture set has to be declared on the backend
            // program before it links. SPIRV-Cross keeps user output names verbatim in
            // the transpiled ESSL (`out vec4 result_0;` stays `result_0`), so the
            // frontend's requested names carry over unchanged.
            if (stateProgramObject->GetTransformFeedbackVaryingCount() > 0 &&
                g_GLESFuncs.glTransformFeedbackVaryings != nullptr) {
                const auto& xfbVaryings = stateProgramObject->GetTransformFeedbackVaryings();
                Vector<const GLchar*> xfbNames;
                xfbNames.reserve(xfbVaryings.size());
                for (const auto& xfbVarying : xfbVaryings) {
                    xfbNames.push_back(xfbVarying.name.c_str());
                }
                MGLOG_D("Declaring %zu transform feedback varyings on program %u", xfbNames.size(),
                        m_backendProgramId);
                g_GLESFuncs.glTransformFeedbackVaryings(m_backendProgramId, static_cast<GLsizei>(xfbNames.size()),
                                                        xfbNames.data(),
                                                        stateProgramObject->GetTransformFeedbackBufferMode());
            }

            // Link program
            MGLOG_D("Linking program %u", m_backendProgramId);
            g_GLESFuncs.glLinkProgram(m_backendProgramId);

            GLint linkStatus;
            g_GLESFuncs.glGetProgramiv(m_backendProgramId, GL_LINK_STATUS, &linkStatus);
            m_backendProgramUsable = m_backendProgramUsable && linkStatus == GL_TRUE;
            if (linkStatus != GL_TRUE) {
                GLint logLength;
                g_GLESFuncs.glGetProgramiv(m_backendProgramId, GL_INFO_LOG_LENGTH, &logLength);
                Vector<GLchar> log(logLength);
                g_GLESFuncs.glGetProgramInfoLog(m_backendProgramId, logLength, nullptr, log.data());
                MGLOG_E("Program %u linking failed for %u: %s", stateProgramObject->GetExternalIndex(),
                        m_backendProgramId, log.data());
            } else {
                MGLOG_D("Program linked successfully. ID: %u", m_backendProgramId);
            }
            m_baseInstanceUniformLocation = g_GLESFuncs.glGetUniformLocation(m_backendProgramId,
                                                                             BASE_INSTANCE_UNIFORM_NAME);
            m_drawIdUniformLocation = g_GLESFuncs.glGetUniformLocation(m_backendProgramId, DRAW_ID_UNIFORM_NAME);
            m_baseInstanceWordIndexUniformLocation =
                g_GLESFuncs.glGetUniformLocation(m_backendProgramId, BASE_INSTANCE_WORD_INDEX_UNIFORM_NAME);
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
            if (stateProgramObject->GetUBOSize() > 0) {
                g_GLESFuncs.glGenBuffers(1, &m_backendGlobalUBOId);
                g_GLESFuncs.glBindBuffer(GL_UNIFORM_BUFFER, m_backendGlobalUBOId);
                g_GLESFuncs.glBufferData(GL_UNIFORM_BUFFER, stateProgramObject->GetUBOSize(), nullptr, GL_STREAM_DRAW);
                g_GLESFuncs.glBindBuffer(GL_UNIFORM_BUFFER, 0);
            } else {
                m_backendGlobalUBOId = 0;
            }

            CacheResourceLocations(stateProgramObject);
            m_syncedLinkVersion = stateProgramObject->GetLinkVersion();

            m_isInitialized = true;
            MGLOG_D("Program sync completed. backend ID %u", m_backendProgramId);
        }

        // Resolves every name-based resource lookup once per link so the per-draw path
        // (BindCurrentProgramWithResources) never issues glGetUniformBlockIndex /
        // glGetUniformLocation string queries; block-to-binding-point assignments are
        // program state and only need to be established here.
        void BackendProgramObjectImpl::CacheResourceLocations(
            const SharedPtr<MG_State::GLState::ProgramObject>& stateProgramObject) {
            m_globalUboBackendBlockIndex = -1;
            m_globalUboBackendBlockSize = 0;
            m_lastUploadedGlobalUboVersion = ~0u;
            m_globalUboRingAllocation = {};
            if (stateProgramObject->GetUBOSize() > 0) {
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
                    MGLOG_W("Program %u has frontend global UBO storage, but backend has no %s block.",
                            stateProgramObject->GetExternalIndex(), MG_Util::ShaderTranspiler::GLOBAL_UBO_NAME);
                }
            }

            const Int uboCount = stateProgramObject->GetActiveUniformBlocksCount();
            m_uniformBlockBackendIndices.assign(static_cast<SizeT>(std::max(uboCount, 0)), -1);
            Uint lastUBOBinding = 0; // binding 0 is reserved for the global UBO
            for (Int i = 0; i < uboCount; ++i) {
                ++lastUBOBinding;
                const auto& name = stateProgramObject->GetUniformBlockName(static_cast<Uint>(i));
                const GLuint backendBlkIdx = g_GLESFuncs.glGetUniformBlockIndex(m_backendProgramId, name.c_str());
                if (backendBlkIdx == GL_INVALID_INDEX) {
                    // Either eliminated as unused, or an SSBO block (frontend reflection
                    // lists those among uniform blocks); SSBO bindings are baked into the ESSL.
                    continue;
                }
                m_uniformBlockBackendIndices[static_cast<SizeT>(i)] = static_cast<Int>(backendBlkIdx);
                g_GLESFuncs.glUniformBlockBinding(m_backendProgramId, backendBlkIdx, lastUBOBinding);
                MGLOG_D("CACHE prog=%u beProg=%u blk[%d]='%s' beIdx=%u -> bePoint=%u",
                        stateProgramObject->GetExternalIndex(), m_backendProgramId, i, name.c_str(), backendBlkIdx,
                        lastUBOBinding);
            }

            m_samplerUniformBindings.clear();
            const Uint maxUniformLoc = stateProgramObject->GetMaxUniformLocation();
            for (Uint loc = 0; loc <= maxUniformLoc; ++loc) {
                const auto& name = stateProgramObject->GetUniformName(loc);
                if (name.empty()) continue;
                const GLenum uniformType = stateProgramObject->GetUniformType(loc);
                if (IsImageUniformType(uniformType)) {
                    // ES image units come exclusively from the layout(binding=N) qualifier
                    // (preserved in the transpiled ESSL); glUniform1i on an image uniform
                    // is an INVALID_OPERATION.
                    continue;
                }
                const Int backendLoc = g_GLESFuncs.glGetUniformLocation(m_backendProgramId, name.c_str());
                if (backendLoc < 0) continue;
                SamplerUniformBinding binding;
                binding.frontendLocation = loc;
                binding.backendLocation = backendLoc;
                binding.uniformType = uniformType;
                binding.lastAssignedUnit = -1;
                // Present only for the samplers EmulateTextureLodBias actually rewrote; the
                // pass names it after the sampler, which SPIRV-Cross preserves verbatim.
                binding.lodBiasLocation =
                    g_GLESFuncs.glGetUniformLocation(m_backendProgramId, (String(LOD_BIAS_UNIFORM_PREFIX) + name).c_str());
                binding.lastAssignedLodBias = 0.0f;
                m_samplerUniformBindings.push_back(binding);
            }
        }

        void BackendProgramObjectImpl::Use() const {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            // glUseProgram on a program that did not link is an INVALID_OPERATION and
            // leaves the *previous* program current, so the draw would silently render
            // with an unrelated shader (KHR-GL3x.texture_size_promotion read another
            // test case's alpha that way once a sampler2DRect stage failed to
            // transpile). Bind nothing instead: the draw is then a visible no-op.
            const Uint programToBind = m_backendProgramUsable ? m_backendProgramId : 0;
            if (g_lastUsedBackendProgramId == programToBind) {
                return;
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
            if (m_baseInstanceWordIndexUniformLocation >= 0) {
                g_GLESFuncs.glUniform1i(m_baseInstanceWordIndexUniformLocation, -1);
            }
        }

        void BackendProgramObjectImpl::SetBaseInstanceWordIndex(Int32 wordIndex) const {
            if (m_baseInstanceWordIndexUniformLocation >= 0) {
                g_GLESFuncs.glUniform1i(m_baseInstanceWordIndexUniformLocation, wordIndex);
            }
        }

        void BackendProgramObjectImpl::SetDrawID(Uint32 drawId) const {
            if (m_drawIdUniformLocation < 0) {
                return;
            }
            g_GLESFuncs.glUniform1i(m_drawIdUniformLocation, static_cast<GLint>(drawId));
        }
    } // namespace PrgramImpl

    namespace SamplerImpl {
        BackendSamplerObject::BackendSamplerObject() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            g_GLESFuncs.glGenSamplers(1, &m_backendSamplerId);
            if (m_backendSamplerId == 0) {
                MGLOG_E("Failed to generate sampler object.");
                MGLOG_E("ES glGetError(): %s", MG_Util::ConvertGLEnumToString(g_GLESFuncs.glGetError()).c_str());
            } else {
                MGLOG_D("Generated sampler object with ID: %u.", m_backendSamplerId);
            }
        }

        void BackendSamplerObject::SyncToBackend(
            const SharedPtr<MG_State::GLState::SamplerObject>& stateSamplerObject) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (!stateSamplerObject) {
                MGLOG_E("State sampler object is null, cannot sync to backend.");
                return;
            }

            Uint currentSamplerVersion = stateSamplerObject->GetVersion();
            if (m_isInitialized && m_syncedSamplerVersion == currentSamplerVersion) {
                MGLOG_D("Sampler parameters have not changed for sampler ID: %u, skipping sync.",
                        stateSamplerObject->GetExternalIndex());
                return;
            }

            m_syncedSamplerVersion = currentSamplerVersion;

            MGLOG_D("Syncing sampler with backend ID %u to backend for state ID %u", m_backendSamplerId,
                    stateSamplerObject->GetExternalIndex());

            const auto& samplerParams = stateSamplerObject->GetAllSamplerParameters();

#define SYNC_SAMPLER_PARAM_IF_CHANGED(internalName, glName, type)                                                      \
    if (m_cacheSamplerParameters.internalName != samplerParams.internalName) {                                         \
        g_GLESFuncs.glSamplerParameteri(m_backendSamplerId, glName,                                                    \
                                        (GLint)MG_Util::ConvertSampler##type##ToGLEnum(samplerParams.internalName));   \
        m_cacheSamplerParameters.internalName = samplerParams.internalName;                                            \
    }

            if (m_cacheSamplerParameters.minFilter != samplerParams.minFilter ||
                m_cacheSamplerParameters.mipmapMode != samplerParams.mipmapMode) {
                g_GLESFuncs.glSamplerParameteri(m_backendSamplerId, GL_TEXTURE_MIN_FILTER,
                                                (GLint)ResolveBackendMinFilter(
                                                    samplerParams,
                                                    ShouldAvoidSamplerMipmapMinFilterOnAngleLlvmpipe()));
                m_cacheSamplerParameters.minFilter = samplerParams.minFilter;
                m_cacheSamplerParameters.mipmapMode = samplerParams.mipmapMode;
            }
            if (m_cacheSamplerParameters.magFilter != samplerParams.magFilter) {
                g_GLESFuncs.glSamplerParameteri(
                    m_backendSamplerId, GL_TEXTURE_MAG_FILTER,
                    (GLint)MG_Util::ConvertSamplerFilterModeToGLEnum(samplerParams.magFilter, SamplerMipmapMode::None));
                m_cacheSamplerParameters.magFilter = samplerParams.magFilter;
            }

            SYNC_SAMPLER_PARAM_IF_CHANGED(wrapS, GL_TEXTURE_WRAP_S, WrapMode)
            SYNC_SAMPLER_PARAM_IF_CHANGED(wrapT, GL_TEXTURE_WRAP_T, WrapMode)
            SYNC_SAMPLER_PARAM_IF_CHANGED(wrapR, GL_TEXTURE_WRAP_R, WrapMode)
            SYNC_SAMPLER_PARAM_IF_CHANGED(compareFunc, GL_TEXTURE_COMPARE_FUNC, CompareFunc)
            SYNC_SAMPLER_PARAM_IF_CHANGED(compareMode, GL_TEXTURE_COMPARE_MODE, CompareMode)
            if (m_cacheSamplerParameters.minLod != samplerParams.minLod) {
                g_GLESFuncs.glSamplerParameterf(m_backendSamplerId, GL_TEXTURE_MIN_LOD, samplerParams.minLod);
                m_cacheSamplerParameters.minLod = samplerParams.minLod;
            }
            if (m_cacheSamplerParameters.maxLod != samplerParams.maxLod) {
                g_GLESFuncs.glSamplerParameterf(m_backendSamplerId, GL_TEXTURE_MAX_LOD, samplerParams.maxLod);
                m_cacheSamplerParameters.maxLod = samplerParams.maxLod;
            }
            if (m_cacheSamplerParameters.maxAnisotropy != samplerParams.maxAnisotropy) {
                if (g_GLESCapabilities.SupportsTextureFilterAnisotropy) {
                    g_GLESFuncs.glSamplerParameterf(m_backendSamplerId, GL_TEXTURE_MAX_ANISOTROPY_EXT,
                                                    samplerParams.maxAnisotropy);
                }
                m_cacheSamplerParameters.maxAnisotropy = samplerParams.maxAnisotropy;
            }
            if (m_cacheSamplerParameters.borderColor != samplerParams.borderColor) {
                // Same gate as the texture-side border colour above.
                if (g_GLESCapabilities.SupportsTextureBorderClamp && g_GLESFuncs.glSamplerParameterfv) {
                    const GLfloat borderColorArray[4] = {
                        samplerParams.borderColor.x(), samplerParams.borderColor.y(),
                        samplerParams.borderColor.z(), samplerParams.borderColor.w()};
                    g_GLESFuncs.glSamplerParameterfv(m_backendSamplerId, GL_TEXTURE_BORDER_COLOR, borderColorArray);
                }
                m_cacheSamplerParameters.borderColor = samplerParams.borderColor;
            }
#undef SYNC_SAMPLER_PARAM_IF_CHANGED
            m_isInitialized = true;
        }

        void BackendSamplerObject::Bind(Uint unit) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (g_boundSamplersCache[unit] == this) return;

            g_GLESFuncs.glBindSampler(static_cast<GLenum>(unit), m_backendSamplerId);
            g_boundSamplersCache[unit] = this;
        }

        Uint BackendSamplerObject::GetBackendSamplerId() const {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            return m_backendSamplerId;
        }

        void UnbindSampler(Uint unit) {
            if (g_boundSamplersCache[unit] == nullptr) return;

            g_GLESFuncs.glBindSampler(static_cast<GLenum>(unit), 0);
            g_boundSamplersCache[unit] = nullptr;
        }

        Array<BackendSamplerObject*, MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS> g_boundSamplersCache;
        StateBackendObjectRegistry<MG_State::GLState::SamplerObject, BackendSamplerObject> g_backendSamplerObjects;
    } // namespace SamplerImpl

    namespace RenderbufferImpl {
        BackendRenderbufferObject::BackendRenderbufferObject() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            g_GLESFuncs.glGenRenderbuffers(1, &m_backendRBOId);
            if (m_backendRBOId == 0) {
                MGLOG_E("Failed to generate renderbuffer object.");
                MGLOG_E("ES glGetError(): %s", MG_Util::ConvertGLEnumToString(g_GLESFuncs.glGetError()).c_str());
            }
        }

        void BackendRenderbufferObject::Bind() const {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            g_GLESFuncs.glBindRenderbuffer(GL_RENDERBUFFER, m_backendRBOId);
        }

        void BackendRenderbufferObject::SyncToBackend(
            const SharedPtr<MG_State::GLState::RenderbufferObject>& stateRBOObject) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (!stateRBOObject) {
                MGLOG_E("State RBO object is null, cannot sync to backend.");
                return;
            }

            MGLOG_D("Syncing RBO with backend ID %u to backend for state ID %u", m_backendRBOId,
                    stateRBOObject->GetExternalIndex());

            if (m_isInitialized && m_cacheInternalFormat == stateRBOObject->GetInternalFormat() &&
                m_cacheWidth == stateRBOObject->GetWidth() && m_cacheHeight == stateRBOObject->GetHeight() &&
                m_cacheSamples == stateRBOObject->GetSamples()) {
                MGLOG_D("RBO %u already initialized with matching parameters, skipping re-allocation.",
                        stateRBOObject->GetExternalIndex());
                return;
            }

            Bind();

            // Allocate storage
            TextureInternalFormat internalFormat = stateRBOObject->GetInternalFormat();
            Int width = static_cast<Int>(stateRBOObject->GetWidth());
            Int height = static_cast<Int>(stateRBOObject->GetHeight());
            Int samples = static_cast<Int>(stateRBOObject->GetSamples());
            GLenum glInternalFormat, glType, glFormat;
            TextureImpl::GenerateRenderbufferFormatInfo(internalFormat, &glInternalFormat, &glFormat, &glType);

            if (samples > 0) {
                g_GLESFuncs.glRenderbufferStorageMultisample(
                    GL_RENDERBUFFER, static_cast<GLsizei>(samples), glInternalFormat, static_cast<GLsizei>(width),
                    static_cast<GLsizei>(height));
            } else {
                g_GLESFuncs.glRenderbufferStorage(GL_RENDERBUFFER, glInternalFormat, static_cast<GLsizei>(width),
                                                  static_cast<GLsizei>(height));
            }

            m_cacheInternalFormat = internalFormat;
            m_cacheWidth = width;
            m_cacheHeight = height;
            m_cacheSamples = samples;

            m_isInitialized = true;
            MGLOG_D("RBO %u sync completed. backend ID %u", stateRBOObject->GetExternalIndex(), m_backendRBOId);
        }

        StateBackendObjectRegistry<MG_State::GLState::RenderbufferObject, BackendRenderbufferObject>
            g_backendRenderbufferObjects;
    } // namespace RenderbufferImpl
} // namespace MobileGL::MG_Backend::DirectGLES
