// MobileGL - MobileGL/MG_Backend/DirectGLES/Utils.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_State/GLState/Core.h>

namespace MobileGL::MG_Backend::DirectGLES {
    namespace DebugImpl {
        class ErrorLopper {
        public:
            static void Loop(const std::function<void(GLenum)>&);
            static void Clear();
            ErrorLopper();
            ~ErrorLopper();
        };

        class OpenGLScopeMarker {
        public:
            explicit OpenGLScopeMarker(const String& scopeName);
            ~OpenGLScopeMarker();
        };
    } // namespace DebugImpl

    namespace BufferImpl {} // namespace BufferImpl

    namespace VertexArrayImpl {
        GLenum GetBindingQuery(GLenum target, bool isTexture);
    } // namespace VertexArrayImpl

    namespace TextureImpl {
        void GenerateTextureFormatInfo(TextureInternalFormat internalFormat, GLenum* outInternalFormat,
                                       GLenum* outFormat, GLenum* outType,
                                       TextureTarget target = TextureTarget::Unknown);
        void GenerateRenderbufferFormatInfo(TextureInternalFormat internalFormat, GLenum* outInternalFormat,
                                            GLenum* outFormat, GLenum* outType);
        Bool ShouldUseCaveatTextureFormat(TextureInternalFormat internalFormat, TextureTarget target);
        Bool ShouldUseCaveatRenderbufferFormat(TextureInternalFormat internalFormat);
    } // namespace TextureImpl

    namespace FramebufferImpl {} // namespace FramebufferImpl

    namespace PrgramImpl {
        String ProcessOutColorLocations(const String& glslCode);
        String ForceSupporterOutput(const String& glslCode);
        String ClampRGBA8SnormFallbackOutputs(String glslCode, GLenum shaderType, Uint32 outputMask);
        String ForceFlatIntegerVaryings(const String& glslCode, GLenum shaderType);
        String RemoveLayoutBinding(const String& glslCode);
    } // namespace PrgramImpl

    namespace Utils {
        void CheckGLESError();
        GLenum GetBindingQuery(GLenum target, bool isTexture);
    } // namespace Utils
} // namespace MobileGL::MG_Backend::DirectGLES
