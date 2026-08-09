// MobileGL - MobileGL/MG_Impl/GLImpl/Program/ProgramInterface.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

namespace MobileGL::MG_State::GLState {
    class ProgramObject;
}

// The GL program interface (ARB_program_interface_query / GL 4.3 §7.3.1) as a frontend
// resource model.
//
// WHY IT IS HERE AND NOT IN A BACKEND. glGetProgramResource* describes the program the
// APPLICATION wrote, in the application's namespace. Neither backend program is in that
// namespace: DirectGLES compiles SPIRV-Cross-generated ESSL where default-block uniforms
// live inside the synthesized MGL_GLOBAL_UBO (so a GL_UNIFORM location query against it is
// structurally -1) and stage in/out names are rewritten; DirectVulkan has no GL-level
// reflection at all and can only re-derive a partial, diverging copy. The one authoritative
// source is the frontend glslang reflection a link already produced, which is the same
// place glGetActiveUniform answers from. This layer generalizes that rule to every
// interface, so the six entry points never consult gBackendFunctionsTable.
//
// NAMING RULES LIVE HERE, NOT IN ProgramObject. The interface query spells resources
// differently from glGetActiveUniform / glGetActiveAttrib (an array is "name[0]", a lookup
// accepts both "name" and "name[0]", a subscript must be a strict decimal). Those two
// getters are what GL30-33 exercises and they must not move, so every normalization is
// applied on the way in and out of THIS file.
namespace MobileGL::MG_Impl::GLImpl::ProgramInterface {
    using ProgramObject = MG_State::GLState::ProgramObject;

    // <programInterface> is one of the GL 4.6 Table 7.1 interfaces.
    Bool IsInterfaceEnum(GLenum programInterface);
    // Interfaces whose resources have names (everything except GL_ATOMIC_COUNTER_BUFFER).
    Bool IsNamedInterface(GLenum programInterface);
    // <prop> is a property token GetProgramResourceiv knows at all (else GL_INVALID_ENUM).
    Bool IsResourceProp(GLenum prop);
    // <prop> applies to <programInterface> (else GL_INVALID_OPERATION).
    Bool InterfaceSupportsProp(GLenum programInterface, GLenum prop);
    // Interfaces GetProgramResourceLocation accepts (else GL_INVALID_ENUM).
    Bool InterfaceHasLocations(GLenum programInterface);

    // GL_ACTIVE_RESOURCES / GL_MAX_NAME_LENGTH / GL_MAX_NUM_ACTIVE_VARIABLES. All three
    // report zero for an interface this implementation cannot enumerate and for a program
    // that has not linked successfully - which is what the spec requires of a program with
    // no active resources.
    Int GetActiveResourceCount(ProgramObject& program, GLenum programInterface);
    Int GetMaxNameLength(ProgramObject& program, GLenum programInterface);
    Int GetMaxNumActiveVariables(ProgramObject& program, GLenum programInterface);

    // GL_INVALID_INDEX when <name> names no active resource of the interface.
    GLuint GetResourceIndex(ProgramObject& program, GLenum programInterface, const char* name);
    // False when <index> is out of range for the interface (the caller raises INVALID_VALUE).
    Bool GetResourceName(ProgramObject& program, GLenum programInterface, GLuint index, String& outName);
    // Appends the value(s) of <prop> for the resource; GL_ACTIVE_VARIABLES appends several.
    // False when <index> is out of range.
    Bool GetResourceProp(ProgramObject& program, GLenum programInterface, GLuint index, GLenum prop,
                         Vector<GLint>& outValues);
    GLint GetResourceLocation(ProgramObject& program, GLenum programInterface, const char* name);
    GLint GetResourceLocationIndex(ProgramObject& program, GLenum programInterface, const char* name);
} // namespace MobileGL::MG_Impl::GLImpl::ProgramInterface
