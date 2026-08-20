// MobileGL - MobileGL/MG_Util/ShaderTranspiler/TranslationCache.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "TranslationCache.h"

#include <Config.h>

namespace MobileGL::MG_Util::ShaderTranspiler {
    namespace {
        // Tags keep two different key builders from ever producing the same blob,
        // even if their inputs happened to serialize identically.
        constexpr Uint32 kSpirvKeyTag = 0x4d474c31u;  // "MGL1"
        constexpr Uint32 kEsslKeyTag = 0x4d474c32u;   // "MGL2"

        // Bumped whenever the SHAPE of a key changes (a field added, a field's
        // meaning changed). It is in every blob, so a stale in-memory entry from a
        // previous shape cannot be honoured - and a future disk tier gets the same
        // protection for free.
        constexpr Uint32 kKeyLayoutVersion = 1u;

        // The repo's existing cache epoch (MG_Config::CacheVersion, the seed
        // ProgramFactory::ComputeHash uses). Strictly redundant for an in-memory
        // cache - one process cannot hold two of them - but it is the knob a disk
        // tier would have to turn, and putting it in now means the blob format does
        // not have to change when that tier arrives.
        void AppendCommonKeyPrefix(TranslationKeyBuilder& builder, const Uint32 tag) {
            builder.Value(tag);
            builder.Value(kKeyLayoutVersion);
            builder.Value(MG_Config::CacheVersion);
        }

        // ---- L1 caps -------------------------------------------------------
        // 64 entries / 12 MiB.
        //
        // The win this cache exists for is REPETITION, not coverage: a CTS smoke
        // case compiles a handful of distinct sources 2592 times, and a handful of
        // entries serves it completely. The opposite workload - an Iris shaderpack
        // load - is ~300-600 MOSTLY DISTINCT programs, which would never hit no
        // matter how large the cache is, so a large cap there buys nothing and
        // costs resident memory on a phone. 64 entries is comfortably above the
        // distinct-source count of every repetition workload measured, and the
        // 12 MiB ceiling bounds the pathological case (a pack whose ~100 KB stages
        // ARE re-linked) at the same order as the existing 8 MiB
        // ShaderPreprocessCache budget.
        constexpr SizeT kSpirvCacheMaxEntries = 64;
        constexpr SizeT kSpirvCacheMaxBytes = 12u * 1024u * 1024u;

        // ---- L2 caps -------------------------------------------------------
        // 128 entries / 12 MiB. Same reasoning, twice the entry count: L2 is keyed
        // per STAGE rather than per program, so the same program population needs
        // roughly twice the slots. The byte budget stays put - an L2 entry (SPIR-V
        // in, ESSL text out) is smaller than an L1 one (all stages' source in, all
        // stages' SPIR-V out).
        constexpr SizeT kEsslCacheMaxEntries = 128;
        constexpr SizeT kEsslCacheMaxBytes = 12u * 1024u * 1024u;
    } // namespace

    Bool ShaderTranslationCacheEnabled() {
        // Read live rather than latched into a function-local static. MG_Config::Features
        // is a plain global of scalars written once by MG_ConfigLoader::Init() - a load
        // costs nothing, no worker ever touches the environment through it, and the unit
        // tests (which flip the field directly, as AsyncCompileTest and QueryTest already
        // do) need the switch to actually take effect when they flip it.
        return MG_Config::Features.ShaderTranslationCache != MG_Config::QuirkOverride::ForceOff;
    }

    void TranslationKeyBuilder::Bytes(const void* data, const SizeT length) {
        if (length == 0) return;
        m_blob.append(static_cast<const char*>(data), length);
    }

    void TranslationKeyBuilder::Text(const StringView text) {
        Value(static_cast<Uint64>(text.size()));
        Bytes(text.data(), text.size());
    }

    void TranslationKeyBuilder::Words(const Vector<Uint32>& words) {
        Value(static_cast<Uint64>(words.size()));
        Bytes(words.data(), words.size() * sizeof(Uint32));
    }

    void TranslationKeyBuilder::NameSet(const std::set<String>& names) {
        Value(static_cast<Uint64>(names.size()));
        for (const String& name : names) Text(name);
    }

    TranslationCacheKey MakeTranslationCacheKey(String blob) {
        TranslationCacheKey key;
        key.hash = static_cast<Uint64>(XXH64(blob.data(), blob.size(), 0));
        key.blob = MakeShared<const String>(Move(blob));
        return key;
    }

    TranslationCacheKey BuildSpirvTranslationKey(const SpirvTranslationKeyInputs& inputs) {
        TranslationKeyBuilder builder;
        AppendCommonKeyPrefix(builder, kSpirvKeyTag);
        builder.Value(inputs.envFingerprint);
        builder.Value(inputs.shaderCompileFlags);
        builder.Value(static_cast<Uint8>(inputs.enableSpirvValidation));
        builder.Value(static_cast<Uint64>(inputs.stages.size()));
        for (const auto& stage : inputs.stages) {
            builder.Value(static_cast<Uint32>(stage.type));
            builder.Text(stage.preprocessedSource);
        }
        static const UnorderedMap<String, Uint> kEmpty;
        builder.NameMap(inputs.explicitVertexInLocations ? *inputs.explicitVertexInLocations : kEmpty);
        builder.NameMap(inputs.explicitFragmentOutLocations ? *inputs.explicitFragmentOutLocations : kEmpty);
        builder.NameMap(inputs.explicitFragmentOutIndices ? *inputs.explicitFragmentOutIndices : kEmpty);
        builder.NameMap(inputs.explicitOpaqueUniformBindings ? *inputs.explicitOpaqueUniformBindings : kEmpty);
        return MakeTranslationCacheKey(builder);
    }

    SizeT SpirvTranslationResultBytes(const SpirvTranslationResult& result) {
        SizeT bytes = 0;
        for (const auto& module : result.modules) bytes += module.size() * sizeof(Uint32);
        return bytes;
    }

    TranslationCacheKey BuildEsslTranslationKey(const EsslTranslationKeyInputs& inputs) {
        TranslationKeyBuilder builder;
        AppendCommonKeyPrefix(builder, kEsslKeyTag);
        builder.Value(static_cast<Uint32>(inputs.shaderType));
        builder.Value(static_cast<Uint8>(inputs.supportsViewportArray));
        builder.Value(static_cast<Uint8>(inputs.supportsNoperspectiveInterpolation));
        builder.Value(inputs.maxColorTextureSamples);
        builder.Value(inputs.maxIntegerSamples);
        builder.Value(inputs.maxDepthTextureSamples);
        builder.Value(inputs.advertisedMaxSamples);
        builder.Value(static_cast<Uint32>(inputs.esslVersion));
        builder.Value(static_cast<Uint8>(inputs.enableSpirvValidation));
        static const std::set<String> kEmptySet;
        builder.NameSet(inputs.xfbCaptureBlockNames ? *inputs.xfbCaptureBlockNames : kEmptySet);
        static const UnorderedMap<String, Uint> kEmptyFormats;
        builder.NameMap(inputs.glFormatByUniformName ? *inputs.glFormatByUniformName : kEmptyFormats);
        static const UnorderedMap<String, Int> kEmptyBindings;
        builder.NameMap(inputs.storageBlockBindingOverrides ? *inputs.storageBlockBindingOverrides
                                                            : kEmptyBindings);
        static const Vector<Uint32> kEmptyWords;
        builder.Words(inputs.spirv ? *inputs.spirv : kEmptyWords);
        return MakeTranslationCacheKey(builder);
    }

    SizeT EsslTranslationResultBytes(const EsslTranslationResult& result) {
        SizeT bytes = result.essl.size();
        for (const String& name : result.flattenedXfbBlockNames) bytes += name.size();
        return bytes;
    }

    BoundedTranslationCache<SpirvTranslationResult>& GetSpirvTranslationCache() {
        // Function-local static: the caches must not be constructed before
        // MG_Config is loaded, and they must survive every context teardown (the
        // key carries the CompileEnv fingerprint, so surviving is safe).
        static BoundedTranslationCache<SpirvTranslationResult> kCache(
            "ShaderTranslationCache L1 (GLSL->SPIR-V)", kSpirvCacheMaxEntries, kSpirvCacheMaxBytes);
        return kCache;
    }

    BoundedTranslationCache<EsslTranslationResult>& GetEsslTranslationCache() {
        static BoundedTranslationCache<EsslTranslationResult> kCache(
            "ShaderTranslationCache L2 (SPIR-V->ESSL)", kEsslCacheMaxEntries, kEsslCacheMaxBytes);
        return kCache;
    }

    void ClearShaderTranslationCaches() {
        GetSpirvTranslationCache().Clear();
        GetEsslTranslationCache().Clear();
    }

    void LogShaderTranslationCacheStats() {
        GetSpirvTranslationCache().LogStats();
        GetEsslTranslationCache().LogStats();
    }
} // namespace MobileGL::MG_Util::ShaderTranspiler
