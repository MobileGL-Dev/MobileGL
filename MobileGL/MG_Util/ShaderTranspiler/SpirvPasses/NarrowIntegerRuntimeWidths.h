// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/NarrowIntegerRuntimeWidths.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include "source/opt/instruction_list.h"
#include "source/opt/ir_context.h"

#include <Includes.h>

#include <functional>

// The RuntimeWidths half of ExtendNarrowIntegerVertexInputsPass. The data width of each location
// is draw state, so the seed reads it from two uint globals - bit L of the first set for 8-bit
// data at location L, of the second for 16-bit data - and widens the fetched value by the input's
// own rule from that many low bits: s = 24 or 16 (0 where neither bit is set) and
//   int input:  value = (value << s) >>> s  (zero-extension)
//   uint input: value = (value << s) >> s   (sign-extension)
// Only shifts, AND, multiply and add, which ESSL 3.00 can spell; no bitfield instruction.
namespace MobileGL::MG_Util::ShaderTranspiler::NarrowIntegerRuntimeWidths {
    struct Ids {
        Uint32 uintTypeId = 0;
        Uint32 zeroId = 0;
        Uint32 oneId = 0;
        Uint32 byteShiftId = 0;  // 24
        Uint32 shortShiftId = 0; // 16
        Uint32 privatePointerTypeId = 0;
        Uint32 widths8Id = 0;
        Uint32 widths16Id = 0;
        Uint32 widths8ValueId = 0;
        Uint32 widths16ValueId = 0;
    };

    // The shift-amount type of one input: its uint vector, or uint for a scalar input.
    struct ShiftType {
        Uint32 typeId = 0;
        Uint32 components = 1;
    };

    // Types and constants first: they have to precede every variable the pass adds or moves.
    Bool MaterializeTypes(spvtools::opt::IRContext* context, Ids& ids);
    ShiftType MaterializeShiftType(spvtools::opt::IRContext* context, const Ids& ids, Uint32 elementTypeId);

    // The two named Private globals, initialized to 0 so an unset width reads as "as fetched".
    Bool DeclareGlobals(spvtools::opt::IRContext* context, Ids& ids, const std::function<Uint32()>& nextId);

    // Instructions go in before `insertPoint`, which then points past them.
    struct Emitter {
        spvtools::opt::IRContext* context = nullptr;
        spvtools::opt::InstructionList::iterator* insertPoint = nullptr;
        std::function<Uint32()> nextId;

        Uint32 Emit(spv::Op opcode, Uint32 typeId, const Vector<Uint32>& operands) const;
    };

    // Loads both widths once, at the top of the entry point.
    void LoadWidths(const Emitter& emitter, Ids& ids);

    // `valueId` (of `elementTypeId`, the input's type) widened per the bits at `locationId`.
    Uint32 Extend(const Emitter& emitter, const Ids& ids, Uint32 elementTypeId, const ShiftType& shiftType,
                  Uint32 locationId, Bool signExtend, Uint32 valueId);
} // namespace MobileGL::MG_Util::ShaderTranspiler::NarrowIntegerRuntimeWidths
