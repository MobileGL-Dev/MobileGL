// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/DecoratePositionInvariantPass.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "DecoratePositionInvariantPass.h"

#include "spirv.hpp"
#include "source/opt/instruction.h"
#include "source/opt/ir_context.h"
#include "source/util/make_unique.h"

#include <algorithm>
#include <vector>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            namespace {
                using spvtools::opt::Instruction;
                using spvtools::opt::IRContext;
                using spvtools::opt::Operand;

                // Identifies one member of a decorated struct (gl_PerVertex's Position slot).
                struct MemberKey {
                    uint32_t id = 0;
                    uint32_t member = 0;
                    bool operator==(const MemberKey& other) const {
                        return id == other.id && member == other.member;
                    }
                };

                // OpDecorate      <target-id> <decoration> [literals...]
                // OpMemberDecorate <struct-id> <member> <decoration> [literals...]
                constexpr uint32_t kDecorateTargetOperand = 0;
                constexpr uint32_t kDecorateDecorationOperand = 1;
                constexpr uint32_t kDecorateBuiltInOperand = 2;
                constexpr uint32_t kMemberDecorateStructOperand = 0;
                constexpr uint32_t kMemberDecorateMemberOperand = 1;
                constexpr uint32_t kMemberDecorateDecorationOperand = 2;
                constexpr uint32_t kMemberDecorateBuiltInOperand = 3;
            } // namespace

            spvtools::opt::Pass::Status DecoratePositionInvariantPass::Process() {
                auto* irContext = context();

                // Collect first: AddAnnotationInst mutates the list being walked.
                std::vector<uint32_t> invariantIds;
                std::vector<MemberKey> invariantMembers;
                std::vector<uint32_t> positionIds;
                std::vector<MemberKey> positionMembers;

                for (const Instruction& annotation : irContext->annotations()) {
                    if (annotation.opcode() == spv::Op::OpDecorate) {
                        if (annotation.NumInOperands() <= kDecorateDecorationOperand) {
                            continue;
                        }
                        const auto decoration = static_cast<spv::Decoration>(
                            annotation.GetSingleWordInOperand(kDecorateDecorationOperand));
                        const uint32_t target = annotation.GetSingleWordInOperand(kDecorateTargetOperand);
                        if (decoration == spv::Decoration::Invariant) {
                            invariantIds.push_back(target);
                        } else if (decoration == spv::Decoration::BuiltIn &&
                                   annotation.NumInOperands() > kDecorateBuiltInOperand &&
                                   static_cast<spv::BuiltIn>(annotation.GetSingleWordInOperand(
                                       kDecorateBuiltInOperand)) == spv::BuiltIn::Position) {
                            positionIds.push_back(target);
                        }
                    } else if (annotation.opcode() == spv::Op::OpMemberDecorate) {
                        if (annotation.NumInOperands() <= kMemberDecorateDecorationOperand) {
                            continue;
                        }
                        const auto decoration = static_cast<spv::Decoration>(
                            annotation.GetSingleWordInOperand(kMemberDecorateDecorationOperand));
                        const MemberKey key{
                            annotation.GetSingleWordInOperand(kMemberDecorateStructOperand),
                            annotation.GetSingleWordInOperand(kMemberDecorateMemberOperand)};
                        if (decoration == spv::Decoration::Invariant) {
                            invariantMembers.push_back(key);
                        } else if (decoration == spv::Decoration::BuiltIn &&
                                   annotation.NumInOperands() > kMemberDecorateBuiltInOperand &&
                                   static_cast<spv::BuiltIn>(annotation.GetSingleWordInOperand(
                                       kMemberDecorateBuiltInOperand)) == spv::BuiltIn::Position) {
                            positionMembers.push_back(key);
                        }
                    }
                }

                Bool changed = false;

                for (const uint32_t target : positionIds) {
                    if (std::find(invariantIds.begin(), invariantIds.end(), target) != invariantIds.end()) {
                        continue;
                    }
                    irContext->AddAnnotationInst(spvtools::MakeUnique<Instruction>(
                        irContext, spv::Op::OpDecorate, 0, 0,
                        std::initializer_list<Operand>{
                            {SPV_OPERAND_TYPE_ID, {target}},
                            {SPV_OPERAND_TYPE_DECORATION,
                             {static_cast<uint32_t>(spv::Decoration::Invariant)}}}));
                    // Guard against a second Position decoration on the same target.
                    invariantIds.push_back(target);
                    changed = true;
                }

                for (const MemberKey& key : positionMembers) {
                    if (std::find(invariantMembers.begin(), invariantMembers.end(), key) !=
                        invariantMembers.end()) {
                        continue;
                    }
                    irContext->AddAnnotationInst(spvtools::MakeUnique<Instruction>(
                        irContext, spv::Op::OpMemberDecorate, 0, 0,
                        std::initializer_list<Operand>{
                            {SPV_OPERAND_TYPE_ID, {key.id}},
                            {SPV_OPERAND_TYPE_LITERAL_INTEGER, {key.member}},
                            {SPV_OPERAND_TYPE_DECORATION,
                             {static_cast<uint32_t>(spv::Decoration::Invariant)}}}));
                    invariantMembers.push_back(key);
                    changed = true;
                }

                if (!changed) {
                    return Status::SuccessWithoutChange;
                }

                irContext->InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);
                return Status::SuccessWithChange;
            }

            spvtools::Optimizer::PassToken
            DecoratePositionInvariantPass::CreateDecoratePositionInvariantPass() {
                return spvtools::Optimizer::PassToken(MakeUnique<DecoratePositionInvariantPass>());
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
