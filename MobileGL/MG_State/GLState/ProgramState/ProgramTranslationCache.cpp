// MobileGL - MobileGL/MG_State/GLState/ProgramState/ProgramTranslationCache.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "ProgramTranslationCache.h"

namespace MobileGL::MG_State::GLState {
    namespace {
        // ---- L1 caps: 48 entries / 24 MiB ----
        //
        // Both numbers moved when the payload grew from "the SPIR-V modules" to "the whole
        // front end". An entry is now the stages' preprocessed source (the key), the SPIR-V,
        // the reflection snapshot and the global-UBO shadow - roughly twice what it was - so
        // the byte budget doubled and the entry count came down to keep the worst case in the
        // same place on a phone.
        //
        // The shape of the choice has not changed: this cache exists for REPETITION, not
        // coverage. A KHR-GL33.texture_swizzle smoke case builds 2592 programs out of fewer
        // than ten distinct ones, so a handful of entries serves it completely; an Iris
        // shaderpack load is ~300-600 MOSTLY DISTINCT programs that would never hit however
        // large the cache is, so a bigger cap there buys nothing and costs resident memory.
        // 48 is comfortably above the distinct-program count of every repetition workload
        // measured, and 24 MiB bounds the pathological case - a pack whose ~100 KB stages
        // really are re-linked - at roughly three times the existing 8 MiB
        // ShaderPreprocessCache budget, which is the other memo on this path.
        constexpr SizeT kMaxEntries = 48;
        constexpr SizeT kMaxBytes = 24u * 1024u * 1024u;

        SizeT StringsBytes(const Vector<String>& values) {
            SizeT bytes = 0;
            for (const String& value : values) bytes += value.size() + sizeof(String);
            return bytes;
        }

        SizeT ResourcesBytes(const Vector<ProgramObject::ResourceReflection>& records) {
            SizeT bytes = records.size() * sizeof(ProgramObject::ResourceReflection);
            for (const auto& record : records) bytes += record.name.size();
            return bytes;
        }
    } // namespace

    // Approximate on purpose: it feeds a budget, not an allocator. It counts the things that
    // actually scale with shader size - the SPIR-V, the reflection names, the UBO shadow -
    // and ignores per-entry fixed overhead.
    SizeT ProgramTranslationResultBytes(const ProgramTranslationResult& result) {
        SizeT bytes = 0;
        for (const auto& module : result.spirv.generatedSpirv) bytes += module.size() * sizeof(unsigned);
        bytes += result.spirv.uniformOffsets.size() * sizeof(Uint);
        bytes += result.spirv.globalUboScratch.size();
        bytes += ResourcesBytes(result.link.uniformReflection);
        bytes += ResourcesBytes(result.link.blockReflection);
        bytes += ResourcesBytes(result.link.pipeInputReflection);
        bytes += ResourcesBytes(result.link.pipeOutputReflection);
        bytes += StringsBytes(result.link.attribs);
        bytes += StringsBytes(result.link.xfbInterfaceNames);
        bytes += result.link.infoLog.size();
        return bytes;
    }

    MG_Util::ShaderTranspiler::BoundedTranslationCache<ProgramTranslationResult>&
    GetProgramTranslationCache() {
        // DELIBERATELY LEAKED - see the same note on the L2 cache in
        // MG_Util/ShaderTranspiler/TranslationCache.cpp. A function-local static OBJECT
        // registers its destructor at first use, and first use here is a ShaderCompilePool
        // worker; ShaderCompilePool's own atexit drain sentinel is registered strictly
        // earlier, and exit handlers run in reverse order - so the cache would be destroyed
        // while workers were still inserting into it. A function-local static POINTER is
        // trivially destructible and registers no exit handler at all.
        static auto* const kCache =
            new MG_Util::ShaderTranspiler::BoundedTranslationCache<ProgramTranslationResult>(
                "ShaderTranslationCache L1 (GLSL->front end)", kMaxEntries, kMaxBytes);
        return *kCache;
    }

    void ClearProgramTranslationCache() { GetProgramTranslationCache().Clear(); }

    void LogProgramTranslationCacheStats() { GetProgramTranslationCache().LogStats(); }
} // namespace MobileGL::MG_State::GLState
