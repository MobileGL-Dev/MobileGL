// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/EliminateFloatEqualsZeroPass.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "EliminateFloatEqualsZeroPass.h"

#include "spirv.hpp"
#include "source/opt/constants.h"
#include "source/opt/def_use_manager.h"
#include "source/opt/instruction.h"
#include "source/opt/ir_builder.h"
#include "source/opt/ir_context.h"
#include "source/opt/module.h"
#include "source/opt/type_manager.h"
#include <cmath>
#include <vector>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            spvtools::opt::Pass::Status EliminateFloatEqualsZeroPass::Process() {
                using namespace spvtools;
                using namespace spvtools::opt;
                bool modified = false;

                analysis::ConstantManager* const_mgr = context()->get_constant_mgr();
                analysis::DefUseManager* def_use_mgr = context()->get_def_use_mgr();
                analysis::TypeManager* type_mgr = context()->get_type_mgr();

                // 2. Import `GLSL.std.450` extension ID (for abs() func)
                uint32_t glsl_std_450_id = context()->get_feature_mgr()->GetExtInstImportId_GLSLstd450();
                if (glsl_std_450_id == 0) {
                    return Status::SuccessWithoutChange;
                }

                // 3. iterate all function -> basic block -> insn
                for (auto& func : *get_module()) {
                    for (auto& bb : func) {
                        for (auto itInst = bb.begin(); itInst != bb.end();) {
                            auto& inst = *itInst;

                            bool shouldSkip = true;

                            // Check if opcode is `OpFOrdEqual`, `OpFUnordEqual`,
                            // `OpFOrdNotEqual` or `OpFUnordNotEqual`,
                            // simply skip if irrelevant
                            switch (inst.opcode()) {
                            case spv::Op::OpFOrdEqual:
                            case spv::Op::OpFUnordEqual:
                            case spv::Op::OpFOrdNotEqual:
                            case spv::Op::OpFUnordNotEqual:
                                shouldSkip = false;
                                break;
                            default:
                                break;
                            }

                            if (shouldSkip) {
                                ++itInst;
                                continue;
                            }

                            // check if operand is "float 0.0"
                            // OpFOrdEqual ResultType ResultID Operand1 Operand2
                            uint32_t op1_id = inst.GetSingleWordInOperand(0);
                            uint32_t op2_id = inst.GetSingleWordInOperand(1);

                            uint32_t var_id = 0;

                            // The constant's WIDTH decides which accessor may read it, and asking
                            // the wrong one does not fail - it answers.
                            //
                            // GetFloat() bit-casts words()[0], which only means anything at 32
                            // bits. On a 64-bit constant words()[0] is the LOW half of the
                            // mantissa, and that half is zero for every round double a shader
                            // actually spells: 1.0lf, 2.0lf, 0.5lf, 100.0lf. Each of those
                            // therefore looked like 0.0 here, and `d != 1.0lf` was rewritten into
                            // `abs(d) >= epsilon` - which is TRUE for d == 1.0. That is the whole
                            // of KHR-GL43.compute_shader.fp64-case2: twelve uniforms compared
                            // against vector and matrix constructors were untouched (a composite
                            // is not a FloatConstant) and the one scalar comparison in the shader
                            // came out inverted. GetFloat() asserts the width, but every shipping
                            // build compiles with NDEBUG, so the assert never ran.
                            //
                            // Widths other than 32 and 64 are declined rather than guessed at:
                            // GetDoubleValue() reads words()[1], which a 16-bit constant does not
                            // have.
                            auto is_float_zero = [&](uint32_t id) -> bool {
                                const analysis::Constant* c = const_mgr->FindDeclaredConstant(id);
                                if (c == nullptr) return false;
                                const analysis::FloatConstant* floatConstant = c->AsFloatConstant();
                                if (floatConstant == nullptr) return false;
                                const analysis::Float* floatType =
                                    floatConstant->type() != nullptr ? floatConstant->type()->AsFloat() : nullptr;
                                if (floatType == nullptr) return false;
                                switch (floatType->width()) {
                                case 32: return std::fabs(floatConstant->GetFloatValue()) <= K_EPSILON;
                                case 64: return std::fabs(floatConstant->GetDoubleValue()) <= K_EPSILON;
                                default: return false;
                                }
                            };

                            if (is_float_zero(op2_id)) {
                                var_id = op1_id; // x == 0.0
                            } else if (is_float_zero(op1_id)) {
                                var_id = op2_id; // 0.0 == x
                            } else {
                                ++itInst;
                                continue;
                            }

                            // --- Found it, continue to patch it ---
                            MGLOG_D("Found 1 occurrence of `FloatEqualsZero`, patching");

                            // 1. Get var type (Float) and result type (Bool)
                            uint32_t float_type_id = def_use_mgr->GetDef(var_id)->type_id();
                            uint32_t bool_type_id = inst.type_id();

                            // 2. Create constant ID for `Epsilon`
                            const analysis::Constant* eps_const = const_mgr->GetConstant(
                                type_mgr->GetType(float_type_id), {*(reinterpret_cast<const uint32_t*>(&K_EPSILON))});
                            uint32_t eps_id = const_mgr->GetDefiningInstruction(eps_const)->result_id();

                            // 3. Build Abs(x) inst
                            // OpExtInst %float_type %glsl_import Abs %x
                            InstructionBuilder builder(
                                context(), &inst, IRContext::kAnalysisDefUse | IRContext::kAnalysisInstrToBlockMapping);

                            std::vector<Operand> abs_operands;
                            abs_operands.push_back({SPV_OPERAND_TYPE_ID, {glsl_std_450_id}});
                            abs_operands.push_back({SPV_OPERAND_TYPE_LITERAL_INTEGER, {4}}); // 4 is FAbs
                            abs_operands.push_back({SPV_OPERAND_TYPE_ID, {var_id}});

                            // In GLSL.std.450, `FAbs`'s OpCode == 4
                            // Ref: https://registry.khronos.org/SPIR-V/specs/1.0/GLSL.std.450.html
                            Instruction* abs_inst = builder.AddInstruction(MakeUnique<Instruction>(
                                context(), spv::Op::OpExtInst, float_type_id, context()->TakeNextId(), abs_operands));

                            // 4. build Abs(x) < Epsilon
                            // OpFOrdLessThan %bool_type %abs_val %eps
                            std::vector<Operand> less_operands;
                            less_operands.push_back({SPV_OPERAND_TYPE_ID, {abs_inst->result_id()}});
                            less_operands.push_back({SPV_OPERAND_TYPE_ID, {eps_id}});

                            spv::Op replacementOp = spv::Op::OpNop;
                            switch (inst.opcode()) {
                            case spv::Op::OpFOrdEqual:
                                replacementOp = spv::Op::OpFOrdLessThan;
                                break;
                            case spv::Op::OpFUnordEqual:
                                replacementOp = spv::Op::OpFUnordLessThan;
                                break;
                            case spv::Op::OpFOrdNotEqual:
                                replacementOp = spv::Op::OpFOrdGreaterThanEqual;
                                break;
                            case spv::Op::OpFUnordNotEqual:
                                replacementOp = spv::Op::OpFUnordGreaterThanEqual;
                                break;
                            default:
                                MOBILEGL_ASSERT(false, "Unexpected float compare opcode: %d",
                                                static_cast<int>(inst.opcode()));
                                break;
                            }
                            Instruction* less_than_inst = builder.AddInstruction(MakeUnique<Instruction>(
                                context(), replacementOp, bool_type_id, context()->TakeNextId(), less_operands));

                            // 5. Replaces all uses of old insn with new one
                            context()->ReplaceAllUsesWith(inst.result_id(), less_than_inst->result_id());

                            // 6. Kill old instruction (will be cleaned up by DCE later)
                            auto nextInstIt = context()->KillInst(&inst);
                            if (nextInstIt) {
                                itInst = nextInstIt;
                            } else {
                                ++itInst;
                            }

                            modified = true;
                        }
                    }
                }
                return modified ? Status::SuccessWithChange : Status::SuccessWithoutChange;
            }

            spvtools::Optimizer::PassToken EliminateFloatEqualsZeroPass::CreateEliminateFloatEqualsZeroPass() {
                return spvtools::Optimizer::PassToken(MakeUnique<EliminateFloatEqualsZeroPass>());
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
