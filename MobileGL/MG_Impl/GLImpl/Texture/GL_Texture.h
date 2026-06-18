// MobileGL - MobileGL/MG_Impl/GLImpl/Texture/GL_Texture.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

namespace MobileGL::MG_Impl::GLImpl {
    /* @INSERTION_POINT:FUNCTION_DECLARATION@ */
    void BindImageTexture(GLuint unit, GLuint texture, GLint level, GLboolean layered, GLint layer, GLenum access,
                          GLenum format);
    void GenerateMipmap(GLenum target);
    void GetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid* pixels);
    void CreateTextures(GLenum target, GLsizei n, GLuint* textures);
    void TextureStorage1D(GLuint texture, GLsizei levels, GLenum internalformat, GLsizei width);
    void TextureStorage2D(GLuint texture, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height);
    void TextureStorage3D(GLuint texture, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height,
                          GLsizei depth);
    void TextureStorage2DMultisample(GLuint texture, GLsizei samples, GLenum internalformat, GLsizei width,
                                     GLsizei height, GLboolean fixedsamplelocations);
    void TextureStorage3DMultisample(GLuint texture, GLsizei samples, GLenum internalformat, GLsizei width,
                                     GLsizei height, GLsizei depth, GLboolean fixedsamplelocations);
    void TextureSubImage1D(GLuint texture, GLint level, GLint xoffset, GLsizei width, GLenum format, GLenum type,
                           const void* pixels);
    void TextureSubImage2D(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,
                           GLenum format, GLenum type, const void* pixels);
    void TextureSubImage3D(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width,
                           GLsizei height, GLsizei depth, GLenum format, GLenum type, const void* pixels);
    void TextureParameterf(GLuint texture, GLenum pname, GLfloat param);
    void TextureParameterfv(GLuint texture, GLenum pname, const GLfloat* params);
    void TextureParameteri(GLuint texture, GLenum pname, GLint param);
    void TextureParameterIiv(GLuint texture, GLenum pname, const GLint* params);
    void TextureParameterIuiv(GLuint texture, GLenum pname, const GLuint* params);
    void TextureParameteriv(GLuint texture, GLenum pname, const GLint* params);
    void GenerateTextureMipmap(GLuint texture);
    void BindTextureUnit(GLuint unit, GLuint texture);
    void GetTextureImage(GLuint texture, GLint level, GLenum format, GLenum type, GLsizei bufSize, void* pixels);
    void GetTextureSubImage(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width,
                            GLsizei height, GLsizei depth, GLenum format, GLenum type, GLsizei bufSize, void* pixels);
    void GetTextureParameterfv(GLuint texture, GLenum pname, GLfloat* params);
    void GetTextureParameterIiv(GLuint texture, GLenum pname, GLint* params);
    void GetTextureParameterIuiv(GLuint texture, GLenum pname, GLuint* params);
    void GetTextureParameteriv(GLuint texture, GLenum pname, GLint* params);
    void GetTextureLevelParameterfv(GLuint texture, GLint level, GLenum pname, GLfloat* params);
    void GetTextureLevelParameteriv(GLuint texture, GLint level, GLenum pname, GLint* params);
    void TexStorage1D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width);
    void TexStorage2D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height);
    void TexStorage3D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height,
                      GLsizei depth);
    void TexStorage2DMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width,
                                 GLsizei height, GLboolean fixedsamplelocations);
    void TexStorage3DMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width,
                                 GLsizei height, GLsizei depth, GLboolean fixedsamplelocations);
    void TexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width,
                       GLsizei height, GLsizei depth, GLenum format, GLenum type, const void* pixels);
    void TexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,
                       GLenum format, GLenum type, const void* pixels);
    void TexSubImage1D(GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format, GLenum type,
                       const GLvoid* pixels);
    void TexParameterf(GLenum target, GLenum pname, GLfloat param);
    void TexParameteri(GLenum target, GLenum pname, GLint param);
    void TexParameterfv(GLenum target, GLenum pname, const GLfloat* params);
    void TexParameteriv(GLenum target, GLenum pname, const GLint* params);
    void TexParameterIiv(GLenum target, GLenum pname, const GLint* params);
    void TexParameterIuiv(GLenum target, GLenum pname, const GLuint* params);

    void TexImage3DMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height,
                               GLsizei depth, GLboolean fixedsamplelocations);
    void TexImage2DMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height,
                               GLboolean fixedsamplelocations);
    void TexImage3D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLsizei depth,
                    GLint border, GLenum format, GLenum type, const void* pixels);
    void TexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border,
                    GLenum format, GLenum type, const void* pixels);
    void TexImage1D(GLenum target, GLint level, GLint internalFormat, GLsizei width, GLint border, GLenum format,
                    GLenum type, const GLvoid* pixels);
    void TexBuffer(GLenum target, GLenum internalformat, GLuint buffer);
    GLboolean IsTexture(GLuint texture);
    void GetTexParameterIuiv(GLenum target, GLenum pname, GLuint* params);
    void GetTexParameterIiv(GLenum target, GLenum pname, GLint* params);
    void GetTexParameteriv(GLenum target, GLenum pname, GLint* params);
    void GetTexParameterfv(GLenum target, GLenum pname, GLfloat* params);
    void GetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint* params);
    void GetTexLevelParameterfv(GLenum target, GLint level, GLenum pname, GLfloat* params);
    void GetMultisamplefv(GLenum pname, GLuint index, GLfloat* val);
    void GetInternalformativ(GLenum target, GLenum internalformat, GLenum pname, GLsizei bufSize, GLint* params);
    void GetCompressedTexImage(GLenum target, GLint level, void* img);
    void GenTextures(GLsizei n, GLuint* textures);
    void DeleteTextures(GLsizei n, const GLuint* textures);
    void CopyTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLint x, GLint y,
                           GLsizei width, GLsizei height);
    void CopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width,
                           GLsizei height);
    void CopyTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLint x, GLint y, GLsizei width);
    void CopyTexImage2D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width,
                        GLsizei height, GLint border);
    void CopyTexImage1D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width,
                        GLint border);
    void CopyImageSubData(GLuint srcName, GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ,
                          GLuint dstName, GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ,
                          GLsizei srcWidth, GLsizei srcHeight, GLsizei srcDepth);
    void CompressedTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width,
                                 GLsizei height, GLsizei depth, GLenum format, GLsizei imageSize, const void* data);
    void CompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                                 GLsizei height, GLenum format, GLsizei imageSize, const void* data);
    void CompressedTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format,
                                 GLsizei imageSize, const void* data);
    void CompressedTexImage3D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height,
                              GLsizei depth, GLint border, GLsizei imageSize, const void* data);
    void CompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height,
                              GLint border, GLsizei imageSize, const void* data);
    void CompressedTexImage1D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLint border,
                              GLsizei imageSize, const void* data);
    void BindTexture(GLenum target, GLuint texture);
    void ActiveTexture(GLenum texture);
} // namespace MobileGL::MG_Impl::GLImpl
