// MobileGL - MobileGL/MG_State/GLState/ProgramState/ShaderSourceKey.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_State/GLState/ProgramState/ShaderStage.h>

namespace MobileGL::MG_State::GLState {
    // The identity of "one glCompileShader's worth of input" - the tuple that decides
    // whether two compiles must produce byte-identical results. Shared by the two
    // per-context memos keyed on it, so that neither can drift from the other:
    //   * P0b's ShaderPreprocessCache, which memoizes the source-only half of a compile;
    //   * P1 stage 6's ShaderCompileAdoptionMap, which shares the job NODE itself.
    //
    // The 64-bit source hash is a LOOKUP ACCELERATOR ONLY. Every user of this key confirms
    // a candidate hit with a full byte comparison of the stored source before honoring it,
    // so a hash collision degrades to a miss and never to a wrong answer. That rule is not
    // negotiable - see the memo-hazard notes on ShaderPreprocessCache.
    //
    // envFingerprint is part of the identity because the pipeline's compute local-size
    // verdict is computed against CompileEnv's device limits: a memo must never be handed
    // back under an environment other than the one it was computed against.
    struct ShaderSourceKey {
        ShaderStage stage = ShaderStage::Unknown;
        Uint64 sourceHash = 0;
        SizeT sourceLength = 0;
        Uint64 envFingerprint = 0;

        Bool operator==(const ShaderSourceKey& other) const {
            return stage == other.stage && sourceHash == other.sourceHash &&
                   sourceLength == other.sourceLength && envFingerprint == other.envFingerprint;
        }
    };

    struct ShaderSourceKeyHasher {
        SizeT operator()(const ShaderSourceKey& key) const {
            // The source hash already spreads well; fold the three discriminators in so
            // that same-hash-different-stage/length/env keys land in different buckets.
            Uint64 mixed = key.sourceHash;
            mixed ^= static_cast<Uint64>(key.sourceLength) + 0x9e3779b97f4a7c15ull + (mixed << 6) + (mixed >> 2);
            mixed ^= static_cast<Uint64>(static_cast<Int>(key.stage)) * 0xff51afd7ed558ccdull;
            mixed ^= key.envFingerprint + 0x9e3779b97f4a7c15ull + (mixed << 6) + (mixed >> 2);
            return static_cast<SizeT>(mixed);
        }
    };
} // namespace MobileGL::MG_State::GLState
