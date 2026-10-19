// MobileGL - MobileGL/MG_IntegrationTest/Harness/IntegerAttributeTypePeek.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "IntegerAttributeTypePeek.h"

#include <cstdio>
#include <cstring>
#include <vector>

#if !defined(__ANDROID__)
#include <Config.h>
#include <MG_Backend/DirectGLES/DirectGLES.h>
#if MOBILEGL_BUILD_DISAGGREGATED
#include <MG_Remote/Client/ClientSession.h>
#include <MG_Remote/Server/ServerLoop.h>
#endif
#define MGITEST_INTEGER_ATTRIBUTE_TYPE_PEEK_LIVE 1
#endif

namespace MGITest {

    bool IntegerAttributeTypeFitsInput(unsigned inputType, bool arrayInteger, unsigned arrayType) {
        switch (inputType) {
        case 0x1404: // GL_INT
        case 0x8B53: // GL_INT_VEC2
        case 0x8B54: // GL_INT_VEC3
        case 0x8B55: // GL_INT_VEC4
            return arrayInteger && (arrayType == 0x1400 || arrayType == 0x1402 || arrayType == 0x1404);
        case 0x1405: // GL_UNSIGNED_INT
        case 0x8DC6: // GL_UNSIGNED_INT_VEC2
        case 0x8DC7: // GL_UNSIGNED_INT_VEC3
        case 0x8DC8: // GL_UNSIGNED_INT_VEC4
            return arrayInteger && (arrayType == 0x1401 || arrayType == 0x1403 || arrayType == 0x1405);
        default:
            return !arrayInteger;
        }
    }

#if defined(MGITEST_INTEGER_ATTRIBUTE_TYPE_PEEK_LIVE)
    namespace {
        namespace MGB = MobileGL::MG_Backend::DirectGLES;

        // Written and read only on the thread that issues Espryt's driver calls: the test thread
        // in monolith, the apply thread under the in-process transport (every access there goes
        // through a probe on that thread).
        IntegerAttributeTypeTally g_tally;
        bool g_watching = false;

        void NoteViolation(GLint program, const char* name, GLint location, GLenum inputType, GLint arrayInteger,
                           GLint arrayType) {
            ++g_tally.violations;
            if (!g_tally.firstViolation.empty()) return;
            char text[256];
            std::snprintf(text, sizeof(text),
                          "draw %u: driver program %d input '%s' (type 0x%04x) at location %d reads an array "
                          "declared %s with type 0x%04x",
                          g_tally.draws, static_cast<int>(program), name, static_cast<unsigned>(inputType),
                          static_cast<int>(location), arrayInteger ? "integer" : "float",
                          static_cast<unsigned>(arrayType));
            g_tally.firstViolation = text;
        }

        // Every enabled array an active input of the current driver program reads.
        void InspectDraw() {
            ++g_tally.draws;
            auto& gl = MGB::g_GLESFuncs;
            GLint program = 0;
            gl.glGetIntegerv(GL_CURRENT_PROGRAM, &program);
            if (program == 0) return;
            GLint count = 0;
            GLint maxLength = 0;
            gl.glGetProgramiv(static_cast<GLuint>(program), GL_ACTIVE_ATTRIBUTES, &count);
            gl.glGetProgramiv(static_cast<GLuint>(program), GL_ACTIVE_ATTRIBUTE_MAX_LENGTH, &maxLength);
            std::vector<char> name(static_cast<std::size_t>(maxLength > 0 ? maxLength : 0) + 1, '\0');
            bool violated = false;
            for (GLint index = 0; index < count && !violated; ++index) {
                GLint size = 0;
                GLenum type = 0;
                gl.glGetActiveAttrib(static_cast<GLuint>(program), static_cast<GLuint>(index),
                                     static_cast<GLsizei>(name.size()), nullptr, &size, &type, name.data());
                if (std::strncmp(name.data(), "gl_", 3) == 0) continue;
                const GLint location = gl.glGetAttribLocation(static_cast<GLuint>(program), name.data());
                if (location < 0) continue;
                GLint enabled = 0;
                GLint arrayInteger = 0;
                GLint arrayType = 0;
                gl.glGetVertexAttribiv(static_cast<GLuint>(location), GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled);
                if (!enabled) continue;
                gl.glGetVertexAttribiv(static_cast<GLuint>(location), GL_VERTEX_ATTRIB_ARRAY_INTEGER, &arrayInteger);
                gl.glGetVertexAttribiv(static_cast<GLuint>(location), GL_VERTEX_ATTRIB_ARRAY_TYPE, &arrayType);
                if (IntegerAttributeTypeFitsInput(type, arrayInteger != 0, static_cast<unsigned>(arrayType))) continue;
                NoteViolation(program, name.data(), location, type, arrayInteger, arrayType);
                violated = true;
            }
        }

        // One hook per driver entry point: the original is kept beside it and called after the
        // inspection, with the arguments untouched.
#define MGITEST_HOOK(name, before)                                                                                     \
    struct Hook_##name {                                                                                               \
        static inline decltype(MGB::g_GLESFuncs.name) original = nullptr;                                              \
        template <typename... Args>                                                                                    \
        static void Call(Args... args) {                                                                               \
            before();                                                                                                  \
            original(args...);                                                                                         \
        }                                                                                                              \
        static void Install() {                                                                                        \
            if (original != nullptr || MGB::g_GLESFuncs.name == nullptr) return;                                       \
            original = MGB::g_GLESFuncs.name;                                                                          \
            MGB::g_GLESFuncs.name = &Call;                                                                             \
        }                                                                                                              \
        static void Remove() {                                                                                         \
            if (original == nullptr) return;                                                                           \
            MGB::g_GLESFuncs.name = original;                                                                          \
            original = nullptr;                                                                                        \
        }                                                                                                              \
    };

        void CountLink() {
            ++g_tally.links;
        }

        MGITEST_HOOK(glDrawArrays, InspectDraw)
        MGITEST_HOOK(glDrawElements, InspectDraw)
        MGITEST_HOOK(glDrawRangeElements, InspectDraw)
        MGITEST_HOOK(glDrawArraysInstanced, InspectDraw)
        MGITEST_HOOK(glDrawElementsInstanced, InspectDraw)
        MGITEST_HOOK(glDrawArraysIndirect, InspectDraw)
        MGITEST_HOOK(glDrawElementsIndirect, InspectDraw)
        MGITEST_HOOK(glDrawElementsBaseVertex, InspectDraw)
        MGITEST_HOOK(glDrawRangeElementsBaseVertex, InspectDraw)
        MGITEST_HOOK(glDrawElementsInstancedBaseVertex, InspectDraw)
        MGITEST_HOOK(glMultiDrawArraysIndirectEXT, InspectDraw)
        MGITEST_HOOK(glMultiDrawElementsIndirectEXT, InspectDraw)
        MGITEST_HOOK(glMultiDrawElementsBaseVertexEXT, InspectDraw)
        MGITEST_HOOK(glDrawArraysInstancedBaseInstanceEXT, InspectDraw)
        MGITEST_HOOK(glDrawElementsInstancedBaseInstanceEXT, InspectDraw)
        MGITEST_HOOK(glDrawElementsInstancedBaseVertexBaseInstanceEXT, InspectDraw)
        MGITEST_HOOK(glLinkProgram, CountLink)
#undef MGITEST_HOOK

#define MGITEST_EACH_HOOK(action)                                                                                      \
    Hook_glDrawArrays::action();                                                                                       \
    Hook_glDrawElements::action();                                                                                     \
    Hook_glDrawRangeElements::action();                                                                                \
    Hook_glDrawArraysInstanced::action();                                                                              \
    Hook_glDrawElementsInstanced::action();                                                                            \
    Hook_glDrawArraysIndirect::action();                                                                               \
    Hook_glDrawElementsIndirect::action();                                                                             \
    Hook_glDrawElementsBaseVertex::action();                                                                           \
    Hook_glDrawRangeElementsBaseVertex::action();                                                                      \
    Hook_glDrawElementsInstancedBaseVertex::action();                                                                  \
    Hook_glMultiDrawArraysIndirectEXT::action();                                                                       \
    Hook_glMultiDrawElementsIndirectEXT::action();                                                                     \
    Hook_glMultiDrawElementsBaseVertexEXT::action();                                                                   \
    Hook_glDrawArraysInstancedBaseInstanceEXT::action();                                                               \
    Hook_glDrawElementsInstancedBaseInstanceEXT::action();                                                             \
    Hook_glDrawElementsInstancedBaseVertexBaseInstanceEXT::action();                                                   \
    Hook_glLinkProgram::action();

        void StartOnDriverThread() {
            g_tally = {};
            if (!g_watching) {
                MGITEST_EACH_HOOK(Install)
            }
            g_watching = true;
        }

        void StopOnDriverThread() {
            if (g_watching) {
                MGITEST_EACH_HOOK(Remove)
            }
            g_watching = false;
        }
#undef MGITEST_EACH_HOOK

        // "Is Espryt the backend running": on any other backend no ES entry point was resolved.
        bool EsprytIsRunning() {
            return MGB::g_GLESFuncs.glDrawArrays != nullptr || Hook_glDrawArrays::original != nullptr;
        }

#if MOBILEGL_BUILD_DISAGGREGATED
        bool InProcess() {
            return MobileGL::MG_Config::Transport == MobileGL::MG_Config::TransportMode::InProcess;
        }

        // Runs `body` on the apply thread once every record published so far has been applied.
        template <typename Body>
        bool OnApplyThread(Body body) {
            auto* session = MobileGL::MG_Remote::Client::ClientSession::Active();
            if (session == nullptr) return false;
            if (session->WaitForApplied(session->LastPublishedSeq(), 30000) !=
                MobileGL::MG_Remote::Transport::SessionWait::Reached)
                return false;
            const auto result = MobileGL::MG_Remote::Server::ServerLoopInstance().RunProbeOnApplyThreadForTesting(
                +[](void* opaque) -> MobileGLResult {
                    (*static_cast<Body*>(opaque))();
                    return MOBILEGL_OK;
                },
                &body);
            return result == MOBILEGL_OK;
        }
#endif
    } // namespace

    bool StartIntegerAttributeTypeWatch(std::string* whyNot) {
        std::string reason;
        bool started = false;
        if (MobileGL::MG_Config::Transport == MobileGL::MG_Config::TransportMode::Monolith) {
            if (EsprytIsRunning()) {
                StartOnDriverThread();
                started = true;
            } else {
                reason = "the backend running is not DirectGLES";
            }
        }
#if MOBILEGL_BUILD_DISAGGREGATED
        else if (InProcess()) {
            bool espryt = false;
            const bool reached = OnApplyThread([&]() {
                espryt = EsprytIsRunning();
                if (espryt) StartOnDriverThread();
            });
            started = reached && espryt;
            if (!started)
                reason = reached ? "the backend running is not DirectGLES" : "the apply thread did not answer";
        }
#endif
        else {
            reason = "the server runs in another process";
        }
        if (!started && whyNot != nullptr) *whyNot = reason;
        return started;
    }

    bool ReadIntegerAttributeTypeWatch(IntegerAttributeTypeTally* out) {
        if (out == nullptr) return false;
#if MOBILEGL_BUILD_DISAGGREGATED
        if (InProcess()) {
            bool watching = false;
            if (!OnApplyThread([&]() {
                    watching = g_watching;
                    *out = g_tally;
                }))
                return false;
            return watching;
        }
#endif
        if (MobileGL::MG_Config::Transport != MobileGL::MG_Config::TransportMode::Monolith || !g_watching) return false;
        *out = g_tally;
        return true;
    }

    void StopIntegerAttributeTypeWatch() {
#if MOBILEGL_BUILD_DISAGGREGATED
        if (InProcess()) {
            OnApplyThread([]() { StopOnDriverThread(); });
            return;
        }
#endif
        if (MobileGL::MG_Config::Transport == MobileGL::MG_Config::TransportMode::Monolith) StopOnDriverThread();
    }
#else
    bool StartIntegerAttributeTypeWatch(std::string* whyNot) {
        if (whyNot != nullptr) *whyNot = "Espryt's driver table is not reachable in this build";
        return false;
    }
    bool ReadIntegerAttributeTypeWatch(IntegerAttributeTypeTally*) {
        return false;
    }
    void StopIntegerAttributeTypeWatch() {}
#endif

} // namespace MGITest
