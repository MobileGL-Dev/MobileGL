// MobileGL - MobileGL/MG_Impl/GLImpl/Debug/GL_Debug.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "GL_Debug.h"

#include <cstring>

#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/ErrorState/Error.h>
#include <MG_Impl/GLImpl/Query/GL_Query.h>
#include <MG_Util/Converters/GLToStr/GLEnumConverter.h>

namespace MobileGL::MG_Impl::GLImpl {
    namespace {
        // Must agree with what GL_Getter answers for GL_MAX_DEBUG_GROUP_STACK_DEPTH and
        // GL_MAX_DEBUG_MESSAGE_LENGTH / GL_MAX_LABEL_LENGTH; an application that sizes a buffer
        // off the query and then trips a different limit here would have no way to explain it.
        constexpr SizeT kMaxDebugGroupStackDepth = 64;
        constexpr GLsizei kMaxDebugMessageLength = 1024;
        constexpr GLsizei kMaxLabelLength = 256;

        // The debug state KHR_debug makes per-context. Held here rather than on GLContext because
        // nothing else in MobileGL reads it, and it is keyed on the context id so a
        // destroyed-and-recreated context starts with an empty stack and no labels - which the
        // unit tests, which recreate the context between cases, depend on.
        struct DebugState {
            Uint64 contextId = 0;
            // The messages pushed with glPushDebugGroup, innermost last. The base group GL creates
            // the context with is implicit and is what makes the reported depth start at 1.
            Vector<String> groupStack;
            // Keyed by (identifier, name); see MakeObjectLabelKey.
            UnorderedMap<Uint64, String> objectLabels;
        };

        DebugState& State() {
            static DebugState state;
            const Uint64 contextId = MG_State::pGLContext ? MG_State::pGLContext->GetTextureContextId() : 0;
            if (state.contextId != contextId) {
                state.contextId = contextId;
                state.groupStack.clear();
                state.objectLabels.clear();
            }
            return state;
        }

        Uint64 MakeObjectLabelKey(GLenum identifier, GLuint name) {
            return (static_cast<Uint64>(identifier) << 32) | static_cast<Uint64>(name);
        }

        void RecordDebugError(ErrorCode code, const char* caller, const String& message) {
            MG_State::pGLContext->RecordError(code, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, message));
        }

        // GL 4.6 core 20.2: only an APPLICATION or THIRD_PARTY source may be injected; the rest
        // are reserved for the implementation itself.
        Bool ValidateInjectedSource(GLenum source, const char* caller) {
            if (source == GL_DEBUG_SOURCE_APPLICATION || source == GL_DEBUG_SOURCE_THIRD_PARTY) {
                return true;
            }
            RecordDebugError(ErrorCode::InvalidEnum, caller,
                             std::format("source {} is not GL_DEBUG_SOURCE_APPLICATION or "
                                         "GL_DEBUG_SOURCE_THIRD_PARTY.",
                                         MG_Util::ConvertGLEnumToString(source)));
            return false;
        }

        // A negative length means the string is NUL-terminated (GL 4.6 core 20.2), which is how
        // every one of these entry points spells "just use the whole thing".
        Bool ValidateDebugStringLength(GLsizei length, const GLchar* text, GLsizei limit, const char* caller,
                                       const char* what) {
            const GLsizei effective =
                length < 0 ? static_cast<GLsizei>(text != nullptr ? std::strlen(text) : 0) : length;
            if (effective < limit) {
                return true;
            }
            RecordDebugError(ErrorCode::InvalidValue, caller,
                             std::format("{} length {} is not less than the {} limit of {}.", what, effective, what,
                                         limit));
            return false;
        }

        String MakeDebugString(GLsizei length, const GLchar* text) {
            if (text == nullptr) return {};
            return length < 0 ? String(text) : String(text, static_cast<SizeT>(length));
        }

        // Whether `name` currently names an object of `identifier`'s type. GL 4.6 core 20.5 makes
        // labelling something that does not exist INVALID_VALUE, and every type KHR_debug lists
        // has a frontend name check - so this is answered exactly rather than waved through.
        // GL_DISPLAY_LIST is deliberately absent: it exists only in the compatibility profile,
        // which MobileGL does not expose, so it falls to the INVALID_ENUM path below.
        Bool ValidateLabelledObject(GLenum identifier, GLuint name, Bool& outIdentifierKnown) {
            outIdentifierKnown = true;
            auto* context = MG_State::pGLContext.get();
            switch (identifier) {
            case GL_BUFFER:
                return context->ValidateBufferName(name);
            case GL_SHADER:
                return context->ValidateShaderName(name);
            case GL_PROGRAM:
                return context->ValidateProgramName(name);
            case GL_VERTEX_ARRAY:
                return context->ValidateVertexArrayName(name);
            case GL_QUERY:
                return IsQuery(name) == GL_TRUE;
            case GL_PROGRAM_PIPELINE:
                return context->ValidateProgramPipelineName(name);
            case GL_TRANSFORM_FEEDBACK:
                return context->ValidateTransformFeedbackName(name);
            case GL_SAMPLER:
                return context->ValidateSamplerName(name);
            case GL_TEXTURE:
                return context->ValidateTextureName(name);
            case GL_RENDERBUFFER:
                return context->ValidateRenderbufferName(name);
            case GL_FRAMEBUFFER:
                // Name 0 is the default framebuffer, which is a real, labellable object.
                return name == 0 || context->ValidateFramebufferName(name);
            default:
                outIdentifierKnown = false;
                return false;
            }
        }
    } // namespace

    GLint GetDebugGroupStackDepth() {
        // GL 4.6 core 20.6: the context is created with one group already on the stack, so the
        // reported depth is one more than the number of pushes the application has made.
        return static_cast<GLint>(State().groupStack.size()) + 1;
    }

    void PushDebugGroup(GLenum source, GLuint id, GLsizei length, const GLchar* message) {
        static_cast<void>(id);
        if (!ValidateInjectedSource(source, __func__)) return;
        if (!ValidateDebugStringLength(length, message, kMaxDebugMessageLength, __func__, "message")) return;

        auto& state = State();
        if (state.groupStack.size() + 1 >= kMaxDebugGroupStackDepth) {
            // Not INVALID_*: KHR_debug gives the group stack its own error code.
            RecordDebugError(ErrorCode::StackOverflow, __func__,
                             std::format("the debug group stack is already {} deep, which is its maximum.",
                                         kMaxDebugGroupStackDepth));
            return;
        }
        state.groupStack.push_back(MakeDebugString(length, message));
        MGLOG_D("glPushDebugGroup(%s) -> depth %d", state.groupStack.back().c_str(), GetDebugGroupStackDepth());
    }

    void PopDebugGroup() {
        auto& state = State();
        if (state.groupStack.empty()) {
            // The base group the context was created with may not be popped (GL 4.6 core 20.6).
            RecordDebugError(ErrorCode::StackUnderflow, __func__,
                             "the debug group stack holds only the group the context was created with.");
            return;
        }
        MGLOG_D("glPopDebugGroup(%s)", state.groupStack.back().c_str());
        state.groupStack.pop_back();
    }

    void DebugMessageInsert(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                            const GLchar* buf) {
        static_cast<void>(id);
        if (!ValidateInjectedSource(source, __func__)) return;
        switch (type) {
        case GL_DEBUG_TYPE_ERROR:
        case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR:
        case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR:
        case GL_DEBUG_TYPE_PORTABILITY:
        case GL_DEBUG_TYPE_PERFORMANCE:
        case GL_DEBUG_TYPE_MARKER:
        case GL_DEBUG_TYPE_PUSH_GROUP:
        case GL_DEBUG_TYPE_POP_GROUP:
        case GL_DEBUG_TYPE_OTHER:
            break;
        default:
            RecordDebugError(ErrorCode::InvalidEnum, __func__,
                             std::format("type {} is not a debug message type.",
                                         MG_Util::ConvertGLEnumToString(type)));
            return;
        }
        switch (severity) {
        case GL_DEBUG_SEVERITY_HIGH:
        case GL_DEBUG_SEVERITY_MEDIUM:
        case GL_DEBUG_SEVERITY_LOW:
        case GL_DEBUG_SEVERITY_NOTIFICATION:
            break;
        default:
            RecordDebugError(ErrorCode::InvalidEnum, __func__,
                             std::format("severity {} is not a debug message severity.",
                                         MG_Util::ConvertGLEnumToString(severity)));
            return;
        }
        if (!ValidateDebugStringLength(length, buf, kMaxDebugMessageLength, __func__, "message")) return;

        // No callback is ever invoked and the message log is empty by construction
        // (GL_MAX_DEBUG_LOGGED_MESSAGES is 1 and glGetDebugMessageLog returns nothing), so the
        // application-visible effect is exactly the error checking above. The text still reaches
        // MobileGL's own log, where it is worth having next to the calls it annotates - at debug
        // level, so an application that inserts a message per draw costs nothing in a release build.
        MGLOG_D("glDebugMessageInsert: %s", MakeDebugString(length, buf).c_str());
    }

    void ObjectLabel(GLenum identifier, GLuint name, GLsizei length, const GLchar* label) {
        Bool identifierKnown = false;
        const Bool objectExists = ValidateLabelledObject(identifier, name, identifierKnown);
        if (!identifierKnown) {
            RecordDebugError(ErrorCode::InvalidEnum, __func__,
                             std::format("identifier {} is not a labellable object type.",
                                         MG_Util::ConvertGLEnumToString(identifier)));
            return;
        }
        if (!objectExists) {
            RecordDebugError(ErrorCode::InvalidValue, __func__,
                             std::format("{} {} is not the name of an existing object.",
                                         MG_Util::ConvertGLEnumToString(identifier), name));
            return;
        }
        if (!ValidateDebugStringLength(length, label, kMaxLabelLength, __func__, "label")) return;

        auto& labels = State().objectLabels;
        const Uint64 key = MakeObjectLabelKey(identifier, name);
        if (label == nullptr) {
            // GL 4.6 core 20.5: a NULL label removes any label the object had.
            labels.erase(key);
            return;
        }
        labels[key] = MakeDebugString(length, label);
    }

    void GetObjectLabel(GLenum identifier, GLuint name, GLsizei bufSize, GLsizei* length, GLchar* label) {
        if (bufSize < 0) {
            RecordDebugError(ErrorCode::InvalidValue, __func__, "bufSize must not be negative.");
            return;
        }
        Bool identifierKnown = false;
        const Bool objectExists = ValidateLabelledObject(identifier, name, identifierKnown);
        if (!identifierKnown) {
            RecordDebugError(ErrorCode::InvalidEnum, __func__,
                             std::format("identifier {} is not a labellable object type.",
                                         MG_Util::ConvertGLEnumToString(identifier)));
            return;
        }
        if (!objectExists) {
            RecordDebugError(ErrorCode::InvalidValue, __func__,
                             std::format("{} {} is not the name of an existing object.",
                                         MG_Util::ConvertGLEnumToString(identifier), name));
            return;
        }

        const auto& labels = State().objectLabels;
        const auto it = labels.find(MakeObjectLabelKey(identifier, name));
        const String& text = it != labels.end() ? it->second : String{};
        // GL 4.6 core 20.5: the returned length excludes the NUL, and an unlabelled object hands
        // back an empty string with length 0 rather than an error.
        SizeT copied = 0;
        if (label != nullptr && bufSize > 0) {
            copied = std::min(text.size(), static_cast<SizeT>(bufSize) - 1);
            std::memcpy(label, text.data(), copied);
            label[copied] = '\0';
        }
        if (length != nullptr) {
            *length = static_cast<GLsizei>(copied);
        }
    }
} // namespace MobileGL::MG_Impl::GLImpl
