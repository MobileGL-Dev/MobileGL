// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/NarrowIntegerRuntimeWidths.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "NarrowIntegerRuntimeWidths.h"

#include "spirv.hpp"
#include "source/opt/constants.h"
#include "source/opt/instruction.h"
#include "source/opt/module.h"
#include "source/opt/type_manager.h"
#include "source/util/make_unique.h"

#include <MG_Util/ShaderTranspiler/NarrowIntegerVertexInputs.h>

namespace MobileGL::MG_Util::ShaderTranspiler::NarrowIntegerRuntimeWidths {
    namespace {
        using spvtools::opt::Instruction;
        using spvtools::opt::IRContext;
        using spvtools::opt::Operand;

        Bool AddNamedGlobal(IRContext* context, const Ids& ids, Uint32 id, const char* name) {
            if (id == 0) return false;
            context->AddGlobalValue(spvtools::MakeUnique<Instruction>(
                context, spv::Op::OpVariable, ids.privatePointerTypeId, id,
                std::initializer_list<Operand>{
                    {SPV_OPERAND_TYPE_STORAGE_CLASS, {static_cast<Uint32>(spv::StorageClass::Private)}},
                    {SPV_OPERAND_TYPE_ID, {ids.zeroId}}}));
            context->AddDebug2Inst(spvtools::MakeUnique<Instruction>(
                context, spv::Op::OpName, 0, 0,
                std::initializer_list<Operand>{{SPV_OPERAND_TYPE_ID, {id}},
                                               {SPV_OPERAND_TYPE_LITERAL_STRING, spvtools::utils::MakeVector(name)}}));
            return true;
        }
    } // namespace

    Bool MaterializeTypes(IRContext* context, Ids& ids) {
        auto* typeMgr = context->get_type_mgr();
        auto* constantMgr = context->get_constant_mgr();
        ids.uintTypeId = typeMgr->GetUIntTypeId();
        ids.zeroId = constantMgr->GetUIntConstId(0);
        ids.oneId = constantMgr->GetUIntConstId(1);
        ids.byteShiftId = constantMgr->GetUIntConstId(24);
        ids.shortShiftId = constantMgr->GetUIntConstId(16);
        ids.privatePointerTypeId = typeMgr->FindPointerToType(ids.uintTypeId, spv::StorageClass::Private);
        return ids.uintTypeId != 0 && ids.zeroId != 0 && ids.oneId != 0 && ids.byteShiftId != 0 &&
               ids.shortShiftId != 0 && ids.privatePointerTypeId != 0;
    }

    ShiftType MaterializeShiftType(IRContext* context, const Ids& ids, Uint32 elementTypeId) {
        auto* typeMgr = context->get_type_mgr();
        const auto* type = typeMgr->GetType(elementTypeId);
        if (type == nullptr) return {};
        if (const auto* vector = type->AsVector()) {
            const Uint32 components = vector->element_count();
            return {typeMgr->GetUIntVectorTypeId(components), components};
        }
        return {ids.uintTypeId, 1};
    }

    Bool DeclareGlobals(IRContext* context, Ids& ids, const std::function<Uint32()>& nextId) {
        ids.widths8Id = nextId();
        ids.widths16Id = nextId();
        return AddNamedGlobal(context, ids, ids.widths8Id, kNarrowIntegerWidths8GlobalName) &&
               AddNamedGlobal(context, ids, ids.widths16Id, kNarrowIntegerWidths16GlobalName);
    }

    Uint32 Emitter::Emit(spv::Op opcode, Uint32 typeId, const Vector<Uint32>& operands) const {
        const Uint32 resultId = typeId != 0 ? nextId() : 0;
        Instruction::OperandList operandList;
        for (const Uint32 id : operands)
            operandList.push_back({SPV_OPERAND_TYPE_ID, {id}});
        *insertPoint = insertPoint->InsertBefore(
            spvtools::MakeUnique<Instruction>(context, opcode, typeId, resultId, operandList));
        ++*insertPoint;
        return resultId;
    }

    void LoadWidths(const Emitter& emitter, Ids& ids) {
        ids.widths8ValueId = emitter.Emit(spv::Op::OpLoad, ids.uintTypeId, {ids.widths8Id});
        ids.widths16ValueId = emitter.Emit(spv::Op::OpLoad, ids.uintTypeId, {ids.widths16Id});
    }

    Uint32 Extend(const Emitter& emitter, const Ids& ids, Uint32 elementTypeId, const ShiftType& shiftType,
                  Uint32 locationId, Bool signExtend, Uint32 valueId) {
        // s = ((w8 >> L) & 1) * 24 + ((w16 >> L) & 1) * 16; one location has one data type, so at
        // most one of the two bits is set and s is 0, 16 or 24.
        const auto widthShift = [&](Uint32 widthsValueId, Uint32 shiftId) {
            const Uint32 shifted =
                emitter.Emit(spv::Op::OpShiftRightLogical, ids.uintTypeId, {widthsValueId, locationId});
            const Uint32 bit = emitter.Emit(spv::Op::OpBitwiseAnd, ids.uintTypeId, {shifted, ids.oneId});
            return emitter.Emit(spv::Op::OpIMul, ids.uintTypeId, {bit, shiftId});
        };
        const Uint32 byteShift = widthShift(ids.widths8ValueId, ids.byteShiftId);
        const Uint32 shortShift = widthShift(ids.widths16ValueId, ids.shortShiftId);
        Uint32 shift = emitter.Emit(spv::Op::OpIAdd, ids.uintTypeId, {byteShift, shortShift});
        if (shiftType.components > 1)
            shift = emitter.Emit(spv::Op::OpCompositeConstruct, shiftType.typeId,
                                 Vector<Uint32>(shiftType.components, shift));
        const Uint32 high = emitter.Emit(spv::Op::OpShiftLeftLogical, elementTypeId, {valueId, shift});
        return emitter.Emit(signExtend ? spv::Op::OpShiftRightArithmetic : spv::Op::OpShiftRightLogical, elementTypeId,
                            {high, shift});
    }
} // namespace MobileGL::MG_Util::ShaderTranspiler::NarrowIntegerRuntimeWidths
