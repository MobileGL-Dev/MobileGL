// MobileGL - MobileGL/MG_IntegrationTest/Harness/IntegerAttributeTypePeek.h
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// What Espryt hands the ES driver for each integer vertex input, checked at every draw the driver
// receives. ES defines an integer attribute's value only when the array's type has the input's
// signedness (BYTE/SHORT/INT for int, the unsigned three for uint) and a float input only from a
// float array; anything else is undefined, and a driver that sign-extends GL_UNSIGNED_BYTE data
// into an ivec4 input is within its rights. Values alone cannot show this on a driver that happens
// to zero-extend, so the watch reads the driver's own vertex array state at each draw.
//
// A SEPARATE TRANSLATION UNIT for PipeApplyPeek.h's reason: it includes Espryt's own headers,
// which may not meet a scenario's GL headers. Every entry point returns false where it cannot
// look - Android, a backend other than DirectGLES, and a server in another process - and a caller
// that gets false has learned nothing.

#pragma once

#include <string>

namespace MGITest {

    struct IntegerAttributeTypeTally {
        // Draws the driver received while the watch was on, and how many of them had at least one
        // enabled array whose type the program's input cannot read with a defined value.
        unsigned draws = 0;
        unsigned violations = 0;
        // glLinkProgram calls the driver received, for the cases that bound relinks.
        unsigned links = 0;
        // The first violation, spelled out for the failure message.
        std::string firstViolation;
    };

    // Starts a fresh tally. False where the watch cannot run; `whyNot` then names the reason.
    bool StartIntegerAttributeTypeWatch(std::string* whyNot);
    // The tally so far, once every draw issued before the call has reached the driver.
    bool ReadIntegerAttributeTypeWatch(IntegerAttributeTypeTally* out);
    // Puts the driver's entry points back. Harmless when no watch is running.
    void StopIntegerAttributeTypeWatch();

    // The rule the watch applies to one enabled array: whether an input of GL type `inputType`
    // reads an array declared (`arrayInteger`, `arrayType`) with a value ES defines.
    bool IntegerAttributeTypeFitsInput(unsigned inputType, bool arrayInteger, unsigned arrayType);

} // namespace MGITest
