// MobileGL - MobileGL/MG_Protocol/generated_wire_dispatch.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "generated_wire_dispatch.h"
#include "generated_dispatch.h"
#include "gen/wire_full_generated.h"
#include <EGL/egl.h>
#include <vector>
#include "MG_Impl/GLImpl/Buffer/GL_Buffer.h"
#include "MG_Impl/GLImpl/Getter/GL_Getter.h"
#include "MG_Impl/GLImpl/Sampler/GL_Sampler.h"
#include "MG_Impl/GLImpl/Sync/GL_Sync.h"
#include "MG_Impl/GLImpl/Query/GL_Query.h"
#include "MG_Impl/GLImpl/Texture/GL_Texture.h"
#include "MG_Impl/GLImpl/Drawing/GL_Drawing.h"
#include "MG_Impl/GLImpl/Program/GL_Program.h"
#include "MG_Impl/GLImpl/Program/GL_ProgramPipeline.h"
#include "MG_Impl/GLImpl/RenderState/GL_RenderState.h"
#include "MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.h"
#include "MG_Impl/GLImpl/VertexArray/GL_VertexArray.h"
#include "MG_Impl/GLImpl/Debug/GL_Debug.h"

extern "C" {
    void glAccum(GLenum op, GLfloat value);
    void glAccumxOES(GLenum op, GLfixed value);
    GLboolean glAcquireKeyedMutexWin32EXT(GLuint memory, GLuint64 key, GLuint timeout);
    void glActiveProgramEXT(GLuint program);
    void glActiveShaderProgram(GLuint pipeline, GLuint program);
    void glActiveStencilFaceEXT(GLenum face);
    void glActiveTexture(GLenum texture);
    void glActiveVaryingNV(GLuint program, const GLchar* name);
    void glAlphaFragmentOp1ATI(GLenum op, GLuint dst, GLuint dstMod, GLuint arg1, GLuint arg1Rep, GLuint arg1Mod);
    void glAlphaFragmentOp2ATI(GLenum op, GLuint dst, GLuint dstMod, GLuint arg1, GLuint arg1Rep, GLuint arg1Mod, GLuint arg2, GLuint arg2Rep, GLuint arg2Mod);
    void glAlphaFragmentOp3ATI(GLenum op, GLuint dst, GLuint dstMod, GLuint arg1, GLuint arg1Rep, GLuint arg1Mod, GLuint arg2, GLuint arg2Rep, GLuint arg2Mod, GLuint arg3, GLuint arg3Rep, GLuint arg3Mod);
    void glAlphaFunc(GLenum func, GLclampf ref);
    void glAlphaFuncxOES(GLenum func, GLfixed ref);
    void glAlphaToCoverageDitherControlNV(GLenum mode);
    void glApplyFramebufferAttachmentCMAAINTEL(void);
    void glApplyTextureEXT(GLenum mode);
    GLboolean glAreProgramsResidentNV(GLsizei n, const GLuint* programs, GLboolean* residences);
    GLboolean glAreTexturesResident(GLsizei n, const GLuint* textures, GLboolean* residences);
    GLboolean glAreTexturesResidentEXT(GLsizei n, const GLuint* textures, GLboolean* residences);
    void glArrayElement(GLint i);
    void glArrayElementEXT(GLint i);
    void glArrayObjectATI(GLenum array, GLint size, GLenum type, GLsizei stride, GLuint buffer, GLuint offset);
    GLuint glAsyncCopyBufferSubDataNVX(GLsizei waitSemaphoreCount, const GLuint* waitSemaphoreArray, const GLuint64* fenceValueArray, GLuint readGpu, GLbitfield writeGpuMask, GLuint readBuffer, GLuint writeBuffer, GLintptr readOffset, GLintptr writeOffset, GLsizeiptr size, GLsizei signalSemaphoreCount, const GLuint* signalSemaphoreArray, const GLuint64* signalValueArray);
    GLuint glAsyncCopyImageSubDataNVX(GLsizei waitSemaphoreCount, const GLuint* waitSemaphoreArray, const GLuint64* waitValueArray, GLuint srcGpu, GLbitfield dstGpuMask, GLuint srcName, GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ, GLuint dstName, GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ, GLsizei srcWidth, GLsizei srcHeight, GLsizei srcDepth, GLsizei signalSemaphoreCount, const GLuint* signalSemaphoreArray, const GLuint64* signalValueArray);
    void glAsyncMarkerSGIX(GLuint marker);
    void glAttachObjectARB(GLhandleARB containerObj, GLhandleARB obj);
    void glAttachShader(GLuint program, GLuint shader);
    void glBegin(GLenum mode);
    void glBeginConditionalRender(GLuint id, GLenum mode);
    void glBeginConditionalRenderNV(GLuint id, GLenum mode);
    void glBeginConditionalRenderNVX(GLuint id);
    void glBeginFragmentShaderATI(void);
    void glBeginOcclusionQueryNV(GLuint id);
    void glBeginPerfMonitorAMD(GLuint monitor);
    void glBeginPerfQueryINTEL(GLuint queryHandle);
    void glBeginQuery(GLenum target, GLuint id);
    void glBeginQueryIndexed(GLenum target, GLuint index, GLuint id);
    void glBeginTransformFeedback(GLenum primitiveMode);
    void glBeginTransformFeedbackNV(GLenum primitiveMode);
    void glBeginVertexShaderEXT(void);
    void glBeginVideoCaptureNV(GLuint video_capture_slot);
    void glBindAttribLocation(GLuint program, GLuint index, const GLchar* name);
    void glBindBuffer(GLenum target, GLuint buffer);
    void glBindBufferBase(GLenum target, GLuint index, GLuint buffer);
    void glBindBufferBaseNV(GLenum target, GLuint index, GLuint buffer);
    void glBindBufferOffsetEXT(GLenum target, GLuint index, GLuint buffer, GLintptr offset);
    void glBindBufferOffsetNV(GLenum target, GLuint index, GLuint buffer, GLintptr offset);
    void glBindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size);
    void glBindBufferRangeNV(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size);
    void glBindBuffersBase(GLenum target, GLuint first, GLsizei count, const GLuint* buffers);
    void glBindBuffersRange(GLenum target, GLuint first, GLsizei count, const GLuint* buffers, const GLintptr* offsets, const GLsizeiptr* sizes);
    void glBindFragDataLocation(GLuint program, GLuint color, const GLchar* name);
    void glBindFragDataLocationEXT(GLuint program, GLuint color, const GLchar* name);
    void glBindFragDataLocationIndexed(GLuint program, GLuint colorNumber, GLuint index, const GLchar* name);
    void glBindFragmentShaderATI(GLuint id);
    void glBindFramebuffer(GLenum target, GLuint framebuffer);
    void glBindImageTexture(GLuint unit, GLuint texture, GLint level, GLboolean layered, GLint layer, GLenum access, GLenum format);
    void glBindImageTextures(GLuint first, GLsizei count, const GLuint* textures);
    GLuint glBindLightParameterEXT(GLenum light, GLenum value);
    GLuint glBindMaterialParameterEXT(GLenum face, GLenum value);
    void glBindMultiTextureEXT(GLenum texunit, GLenum target, GLuint texture);
    GLuint glBindParameterEXT(GLenum value);
    void glBindProgramARB(GLenum target, GLuint program);
    void glBindProgramNV(GLenum target, GLuint id);
    void glBindProgramPipeline(GLuint pipeline);
    void glBindRenderbuffer(GLenum target, GLuint renderbuffer);
    void glBindSampler(GLuint unit, GLuint sampler);
    void glBindSamplers(GLuint first, GLsizei count, const GLuint* samplers);
    void glBindShadingRateImageNV(GLuint texture);
    GLuint glBindTexGenParameterEXT(GLenum unit, GLenum coord, GLenum value);
    void glBindTexture(GLenum target, GLuint texture);
    void glBindTextureUnit(GLuint unit, GLuint texture);
    GLuint glBindTextureUnitParameterEXT(GLenum unit, GLenum value);
    void glBindTextures(GLuint first, GLsizei count, const GLuint* textures);
    void glBindTransformFeedback(GLenum target, GLuint id);
    void glBindTransformFeedbackNV(GLenum target, GLuint id);
    void glBindVertexArray(GLuint array);
    void glBindVertexArrayAPPLE(GLuint array);
    void glBindVertexBuffer(GLuint bindingindex, GLuint buffer, GLintptr offset, GLsizei stride);
    void glBindVertexBuffers(GLuint first, GLsizei count, const GLuint* buffers, const GLintptr* offsets, const GLsizei* strides);
    void glBindVertexShaderEXT(GLuint id);
    void glBindVideoCaptureStreamBufferNV(GLuint video_capture_slot, GLuint stream, GLenum frame_region, GLintptrARB offset);
    void glBindVideoCaptureStreamTextureNV(GLuint video_capture_slot, GLuint stream, GLenum frame_region, GLenum target, GLuint texture);
    void glBinormal3bEXT(GLbyte bx, GLbyte by, GLbyte bz);
    void glBinormal3bvEXT(const GLbyte* v);
    void glBinormal3dEXT(GLdouble bx, GLdouble by, GLdouble bz);
    void glBinormal3dvEXT(const GLdouble* v);
    void glBinormal3fEXT(GLfloat bx, GLfloat by, GLfloat bz);
    void glBinormal3fvEXT(const GLfloat* v);
    void glBinormal3iEXT(GLint bx, GLint by, GLint bz);
    void glBinormal3ivEXT(const GLint* v);
    void glBinormal3sEXT(GLshort bx, GLshort by, GLshort bz);
    void glBinormal3svEXT(const GLshort* v);
    void glBinormalPointerEXT(GLenum type, GLsizei stride, const void* pointer);
    void glBitmap(GLsizei width, GLsizei height, GLfloat xorig, GLfloat yorig, GLfloat xmove, GLfloat ymove, const GLubyte* bitmap);
    void glBitmapxOES(GLsizei width, GLsizei height, GLfixed xorig, GLfixed yorig, GLfixed xmove, GLfixed ymove, const GLubyte* bitmap);
    void glBlendBarrierKHR(void);
    void glBlendBarrierNV(void);
    void glBlendColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
    void glBlendColorxOES(GLfixed red, GLfixed green, GLfixed blue, GLfixed alpha);
    void glBlendEquation(GLenum mode);
    void glBlendEquationIndexedAMD(GLuint buf, GLenum mode);
    void glBlendEquationSeparate(GLenum modeRGB, GLenum modeAlpha);
    void glBlendEquationSeparateIndexedAMD(GLuint buf, GLenum modeRGB, GLenum modeAlpha);
    void glBlendEquationSeparatei(GLuint buf, GLenum modeRGB, GLenum modeAlpha);
    void glBlendEquationSeparateiARB(GLuint buf, GLenum modeRGB, GLenum modeAlpha);
    void glBlendEquationi(GLuint buf, GLenum mode);
    void glBlendEquationiARB(GLuint buf, GLenum mode);
    void glBlendFunc(GLenum sfactor, GLenum dfactor);
    void glBlendFuncIndexedAMD(GLuint buf, GLenum src, GLenum dst);
    void glBlendFuncSeparate(GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha, GLenum dfactorAlpha);
    void glBlendFuncSeparateINGR(GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha, GLenum dfactorAlpha);
    void glBlendFuncSeparateIndexedAMD(GLuint buf, GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha);
    void glBlendFuncSeparatei(GLuint buf, GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha);
    void glBlendFuncSeparateiARB(GLuint buf, GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha);
    void glBlendFunci(GLuint buf, GLenum src, GLenum dst);
    void glBlendFunciARB(GLuint buf, GLenum src, GLenum dst);
    void glBlendParameteriNV(GLenum pname, GLint value);
    void glBlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter);
    void glBlitNamedFramebuffer(GLuint readFramebuffer, GLuint drawFramebuffer, GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter);
    void glBufferAddressRangeNV(GLenum pname, GLuint index, GLuint64EXT address, GLsizeiptr length);
    void glBufferAttachMemoryNV(GLenum target, GLuint memory, GLuint64 offset);
    void glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage);
    void glBufferPageCommitmentARB(GLenum target, GLintptr offset, GLsizeiptr size, GLboolean commit);
    void glBufferPageCommitmentMemNV(GLenum target, GLintptr offset, GLsizeiptr size, GLuint memory, GLuint64 memOffset, GLboolean commit);
    void glBufferParameteriAPPLE(GLenum target, GLenum pname, GLint param);
    void glBufferStorage(GLenum target, GLsizeiptr size, const void* data, GLbitfield flags);
    void glBufferStorageExternalEXT(GLenum target, GLintptr offset, GLsizeiptr size, GLeglClientBufferEXT clientBuffer, GLbitfield flags);
    void glBufferStorageMemEXT(GLenum target, GLsizeiptr size, GLuint memory, GLuint64 offset);
    void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data);
    void glCallCommandListNV(GLuint list);
    void glCallList(GLuint list);
    void glCallLists(GLsizei n, GLenum type, const GLvoid* lists);
    GLenum glCheckFramebufferStatus(GLenum target);
    GLenum glCheckNamedFramebufferStatus(GLuint framebuffer, GLenum target);
    GLenum glCheckNamedFramebufferStatusEXT(GLuint framebuffer, GLenum target);
    void glClampColor(GLenum target, GLenum clamp);
    void glClampColorARB(GLenum target, GLenum clamp);
    void glClear(GLbitfield mask);
    void glClearAccum(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
    void glClearAccumxOES(GLfixed red, GLfixed green, GLfixed blue, GLfixed alpha);
    void glClearBufferData(GLenum target, GLenum internalformat, GLenum format, GLenum type, const void* data);
    void glClearBufferSubData(GLenum target, GLenum internalformat, GLintptr offset, GLsizeiptr size, GLenum format, GLenum type, const void* data);
    void glClearBufferfi(GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil);
    void glClearBufferfv(GLenum buffer, GLint drawbuffer, const GLfloat* value);
    void glClearBufferiv(GLenum buffer, GLint drawbuffer, const GLint* value);
    void glClearBufferuiv(GLenum buffer, GLint drawbuffer, const GLuint* value);
    void glClearColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
    void glClearColorIiEXT(GLint red, GLint green, GLint blue, GLint alpha);
    void glClearColorIuiEXT(GLuint red, GLuint green, GLuint blue, GLuint alpha);
    void glClearColorxOES(GLfixed red, GLfixed green, GLfixed blue, GLfixed alpha);
    void glClearDepth(GLclampd depth);
    void glClearDepthdNV(GLdouble depth);
    void glClearDepthf(GLfloat d);
    void glClearDepthfOES(GLclampf depth);
    void glClearDepthxOES(GLfixed depth);
    void glClearIndex(GLfloat c);
    void glClearNamedBufferData(GLuint buffer, GLenum internalformat, GLenum format, GLenum type, const void* data);
    void glClearNamedBufferDataEXT(GLuint buffer, GLenum internalformat, GLenum format, GLenum type, const void* data);
    void glClearNamedBufferSubData(GLuint buffer, GLenum internalformat, GLintptr offset, GLsizeiptr size, GLenum format, GLenum type, const void* data);
    void glClearNamedBufferSubDataEXT(GLuint buffer, GLenum internalformat, GLsizeiptr offset, GLsizeiptr size, GLenum format, GLenum type, const void* data);
    void glClearNamedFramebufferfi(GLuint framebuffer, GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil);
    void glClearNamedFramebufferfv(GLuint framebuffer, GLenum buffer, GLint drawbuffer, const GLfloat* value);
    void glClearNamedFramebufferiv(GLuint framebuffer, GLenum buffer, GLint drawbuffer, const GLint* value);
    void glClearNamedFramebufferuiv(GLuint framebuffer, GLenum buffer, GLint drawbuffer, const GLuint* value);
    void glClearStencil(GLint s);
    void glClearTexImage(GLuint texture, GLint level, GLenum format, GLenum type, const void* data);
    void glClearTexSubImage(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, const void* data);
    void glClientActiveTexture(GLenum texture);
    void glClientActiveTextureARB(GLenum texture);
    void glClientActiveVertexStreamATI(GLenum stream);
    void glClientAttribDefaultEXT(GLbitfield mask);
    void glClientWaitSemaphoreui64NVX(GLsizei fenceObjectCount, const GLuint* semaphoreArray, const GLuint64* fenceValueArray);
    GLenum glClientWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout);
    void glClipControl(GLenum origin, GLenum depth);
    void glClipPlane(GLenum plane, const GLdouble* equation);
    void glClipPlanefOES(GLenum plane, const GLfloat* equation);
    void glClipPlanexOES(GLenum plane, const GLfixed* equation);
    void glColor3b(GLbyte red, GLbyte green, GLbyte blue);
    void glColor3bv(const GLbyte* v);
    void glColor3d(GLdouble red, GLdouble green, GLdouble blue);
    void glColor3dv(const GLdouble* v);
    void glColor3f(GLfloat red, GLfloat green, GLfloat blue);
    void glColor3fVertex3fSUN(GLfloat r, GLfloat g, GLfloat b, GLfloat x, GLfloat y, GLfloat z);
    void glColor3fVertex3fvSUN(const GLfloat* c, const GLfloat* v);
    void glColor3fv(const GLfloat* v);
    void glColor3hNV(GLhalfNV red, GLhalfNV green, GLhalfNV blue);
    void glColor3hvNV(const GLhalfNV* v);
    void glColor3i(GLint red, GLint green, GLint blue);
    void glColor3iv(const GLint* v);
    void glColor3s(GLshort red, GLshort green, GLshort blue);
    void glColor3sv(const GLshort* v);
    void glColor3ub(GLubyte red, GLubyte green, GLubyte blue);
    void glColor3ubv(const GLubyte* v);
    void glColor3ui(GLuint red, GLuint green, GLuint blue);
    void glColor3uiv(const GLuint* v);
    void glColor3us(GLushort red, GLushort green, GLushort blue);
    void glColor3usv(const GLushort* v);
    void glColor3xOES(GLfixed red, GLfixed green, GLfixed blue);
    void glColor3xvOES(const GLfixed* components);
    void glColor4b(GLbyte red, GLbyte green, GLbyte blue, GLbyte alpha);
    void glColor4bv(const GLbyte* v);
    void glColor4d(GLdouble red, GLdouble green, GLdouble blue, GLdouble alpha);
    void glColor4dv(const GLdouble* v);
    void glColor4f(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
    void glColor4fNormal3fVertex3fSUN(GLfloat r, GLfloat g, GLfloat b, GLfloat a, GLfloat nx, GLfloat ny, GLfloat nz, GLfloat x, GLfloat y, GLfloat z);
    void glColor4fNormal3fVertex3fvSUN(const GLfloat* c, const GLfloat* n, const GLfloat* v);
    void glColor4fv(const GLfloat* v);
    void glColor4hNV(GLhalfNV red, GLhalfNV green, GLhalfNV blue, GLhalfNV alpha);
    void glColor4hvNV(const GLhalfNV* v);
    void glColor4i(GLint red, GLint green, GLint blue, GLint alpha);
    void glColor4iv(const GLint* v);
    void glColor4s(GLshort red, GLshort green, GLshort blue, GLshort alpha);
    void glColor4sv(const GLshort* v);
    void glColor4ub(GLubyte red, GLubyte green, GLubyte blue, GLubyte alpha);
    void glColor4ubVertex2fSUN(GLubyte r, GLubyte g, GLubyte b, GLubyte a, GLfloat x, GLfloat y);
    void glColor4ubVertex2fvSUN(const GLubyte* c, const GLfloat* v);
    void glColor4ubVertex3fSUN(GLubyte r, GLubyte g, GLubyte b, GLubyte a, GLfloat x, GLfloat y, GLfloat z);
    void glColor4ubVertex3fvSUN(const GLubyte* c, const GLfloat* v);
    void glColor4ubv(const GLubyte* v);
    void glColor4ui(GLuint red, GLuint green, GLuint blue, GLuint alpha);
    void glColor4uiv(const GLuint* v);
    void glColor4us(GLushort red, GLushort green, GLushort blue, GLushort alpha);
    void glColor4usv(const GLushort* v);
    void glColor4xOES(GLfixed red, GLfixed green, GLfixed blue, GLfixed alpha);
    void glColor4xvOES(const GLfixed* components);
    void glColorFormatNV(GLint size, GLenum type, GLsizei stride);
    void glColorFragmentOp1ATI(GLenum op, GLuint dst, GLuint dstMask, GLuint dstMod, GLuint arg1, GLuint arg1Rep, GLuint arg1Mod);
    void glColorFragmentOp2ATI(GLenum op, GLuint dst, GLuint dstMask, GLuint dstMod, GLuint arg1, GLuint arg1Rep, GLuint arg1Mod, GLuint arg2, GLuint arg2Rep, GLuint arg2Mod);
    void glColorFragmentOp3ATI(GLenum op, GLuint dst, GLuint dstMask, GLuint dstMod, GLuint arg1, GLuint arg1Rep, GLuint arg1Mod, GLuint arg2, GLuint arg2Rep, GLuint arg2Mod, GLuint arg3, GLuint arg3Rep, GLuint arg3Mod);
    void glColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha);
    void glColorMaskIndexedEXT(GLuint index, GLboolean r, GLboolean g, GLboolean b, GLboolean a);
    void glColorMaski(GLuint index, GLboolean r, GLboolean g, GLboolean b, GLboolean a);
    void glColorMaterial(GLenum face, GLenum mode);
    void glColorP3ui(GLenum type, GLuint color);
    void glColorP3uiv(GLenum type, const GLuint* color);
    void glColorP4ui(GLenum type, GLuint color);
    void glColorP4uiv(GLenum type, const GLuint* color);
    void glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* ptr);
    void glColorPointerEXT(GLint size, GLenum type, GLsizei stride, GLsizei count, const void* pointer);
    void glColorPointerListIBM(GLint size, GLenum type, GLint stride, const void** pointer, GLint ptrstride);
    void glColorPointervINTEL(GLint size, GLenum type, const void** pointer);
    void glColorSubTable(GLenum target, GLsizei start, GLsizei count, GLenum format, GLenum type, const void* data);
    void glColorSubTableEXT(GLenum target, GLsizei start, GLsizei count, GLenum format, GLenum type, const void* data);
    void glColorTable(GLenum target, GLenum internalformat, GLsizei width, GLenum format, GLenum type, const void* table);
    void glColorTableEXT(GLenum target, GLenum internalFormat, GLsizei width, GLenum format, GLenum type, const void* table);
    void glColorTableParameterfv(GLenum target, GLenum pname, const GLfloat* params);
    void glColorTableParameterfvSGI(GLenum target, GLenum pname, const GLfloat* params);
    void glColorTableParameteriv(GLenum target, GLenum pname, const GLint* params);
    void glColorTableParameterivSGI(GLenum target, GLenum pname, const GLint* params);
    void glColorTableSGI(GLenum target, GLenum internalformat, GLsizei width, GLenum format, GLenum type, const void* table);
    void glCombinerInputNV(GLenum stage, GLenum portion, GLenum variable, GLenum input, GLenum mapping, GLenum componentUsage);
    void glCombinerOutputNV(GLenum stage, GLenum portion, GLenum abOutput, GLenum cdOutput, GLenum sumOutput, GLenum scale, GLenum bias, GLboolean abDotProduct, GLboolean cdDotProduct, GLboolean muxSum);
    void glCombinerParameterfNV(GLenum pname, GLfloat param);
    void glCombinerParameterfvNV(GLenum pname, const GLfloat* params);
    void glCombinerParameteriNV(GLenum pname, GLint param);
    void glCombinerParameterivNV(GLenum pname, const GLint* params);
    void glCombinerStageParameterfvNV(GLenum stage, GLenum pname, const GLfloat* params);
    void glCommandListSegmentsNV(GLuint list, GLuint segments);
    void glCompileCommandListNV(GLuint list);
    void glCompileShader(GLuint shader);
    void glCompileShaderIncludeARB(GLuint shader, GLsizei count, const GLchar* const* path, const GLint* length);
    void glCompressedMultiTexImage1DEXT(GLenum texunit, GLenum target, GLint level, GLenum internalformat, GLsizei width, GLint border, GLsizei imageSize, const void* bits);
    void glCompressedMultiTexImage2DEXT(GLenum texunit, GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void* bits);
    void glCompressedMultiTexImage3DEXT(GLenum texunit, GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLsizei imageSize, const void* bits);
    void glCompressedMultiTexSubImage1DEXT(GLenum texunit, GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format, GLsizei imageSize, const void* bits);
    void glCompressedMultiTexSubImage2DEXT(GLenum texunit, GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLsizei imageSize, const void* bits);
    void glCompressedMultiTexSubImage3DEXT(GLenum texunit, GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLsizei imageSize, const void* bits);
    void glCompressedTexImage1D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLint border, GLsizei imageSize, const void* data);
    void glCompressedTexImage1DARB(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLint border, GLsizei imageSize, const void* data);
    void glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void* data);
    void glCompressedTexImage3D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLsizei imageSize, const void* data);
    void glCompressedTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format, GLsizei imageSize, const void* data);
    void glCompressedTexSubImage1DARB(GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format, GLsizei imageSize, const void* data);
    void glCompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLsizei imageSize, const void* data);
    void glCompressedTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLsizei imageSize, const void* data);
    void glCompressedTextureImage1DEXT(GLuint texture, GLenum target, GLint level, GLenum internalformat, GLsizei width, GLint border, GLsizei imageSize, const void* bits);
    void glCompressedTextureImage2DEXT(GLuint texture, GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void* bits);
    void glCompressedTextureImage3DEXT(GLuint texture, GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLsizei imageSize, const void* bits);
    void glCompressedTextureSubImage1D(GLuint texture, GLint level, GLint xoffset, GLsizei width, GLenum format, GLsizei imageSize, const void* data);
    void glCompressedTextureSubImage1DEXT(GLuint texture, GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format, GLsizei imageSize, const void* bits);
    void glCompressedTextureSubImage2D(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLsizei imageSize, const void* data);
    void glCompressedTextureSubImage2DEXT(GLuint texture, GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLsizei imageSize, const void* bits);
    void glCompressedTextureSubImage3D(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLsizei imageSize, const void* data);
    void glCompressedTextureSubImage3DEXT(GLuint texture, GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLsizei imageSize, const void* bits);
    void glConservativeRasterParameterfNV(GLenum pname, GLfloat value);
    void glConservativeRasterParameteriNV(GLenum pname, GLint param);
    void glConvolutionFilter1D(GLenum target, GLenum internalformat, GLsizei width, GLenum format, GLenum type, const void* image);
    void glConvolutionFilter1DEXT(GLenum target, GLenum internalformat, GLsizei width, GLenum format, GLenum type, const void* image);
    void glConvolutionFilter2D(GLenum target, GLenum internalformat, GLsizei width, GLsizei height, GLenum format, GLenum type, const void* image);
    void glConvolutionFilter2DEXT(GLenum target, GLenum internalformat, GLsizei width, GLsizei height, GLenum format, GLenum type, const void* image);
    void glConvolutionParameterf(GLenum target, GLenum pname, GLfloat params);
    void glConvolutionParameterfEXT(GLenum target, GLenum pname, GLfloat params);
    void glConvolutionParameterfv(GLenum target, GLenum pname, const GLfloat* params);
    void glConvolutionParameterfvEXT(GLenum target, GLenum pname, const GLfloat* params);
    void glConvolutionParameteri(GLenum target, GLenum pname, GLint params);
    void glConvolutionParameteriEXT(GLenum target, GLenum pname, GLint params);
    void glConvolutionParameteriv(GLenum target, GLenum pname, const GLint* params);
    void glConvolutionParameterivEXT(GLenum target, GLenum pname, const GLint* params);
    void glConvolutionParameterxOES(GLenum target, GLenum pname, GLfixed param);
    void glConvolutionParameterxvOES(GLenum target, GLenum pname, const GLfixed* params);
    void glCopyBufferSubData(GLenum readTarget, GLenum writeTarget, GLintptr readOffset, GLintptr writeOffset, GLsizeiptr size);
    void glCopyColorSubTable(GLenum target, GLsizei start, GLint x, GLint y, GLsizei width);
    void glCopyColorSubTableEXT(GLenum target, GLsizei start, GLint x, GLint y, GLsizei width);
    void glCopyColorTable(GLenum target, GLenum internalformat, GLint x, GLint y, GLsizei width);
    void glCopyColorTableSGI(GLenum target, GLenum internalformat, GLint x, GLint y, GLsizei width);
    void glCopyConvolutionFilter1D(GLenum target, GLenum internalformat, GLint x, GLint y, GLsizei width);
    void glCopyConvolutionFilter1DEXT(GLenum target, GLenum internalformat, GLint x, GLint y, GLsizei width);
    void glCopyConvolutionFilter2D(GLenum target, GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height);
    void glCopyConvolutionFilter2DEXT(GLenum target, GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height);
    void glCopyImageSubData(GLuint srcName, GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ, GLuint dstName, GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ, GLsizei srcWidth, GLsizei srcHeight, GLsizei srcDepth);
    void glCopyImageSubDataNV(GLuint srcName, GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ, GLuint dstName, GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ, GLsizei width, GLsizei height, GLsizei depth);
    void glCopyMultiTexImage1DEXT(GLenum texunit, GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLint border);
    void glCopyMultiTexImage2DEXT(GLenum texunit, GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height, GLint border);
    void glCopyMultiTexSubImage1DEXT(GLenum texunit, GLenum target, GLint level, GLint xoffset, GLint x, GLint y, GLsizei width);
    void glCopyMultiTexSubImage2DEXT(GLenum texunit, GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height);
    void glCopyMultiTexSubImage3DEXT(GLenum texunit, GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLint x, GLint y, GLsizei width, GLsizei height);
    void glCopyNamedBufferSubData(GLuint readBuffer, GLuint writeBuffer, GLintptr readOffset, GLintptr writeOffset, GLsizeiptr size);
    void glCopyPathNV(GLuint resultPath, GLuint srcPath);
    void glCopyPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum type);
    void glCopyTexImage1D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLint border);
    void glCopyTexImage1DEXT(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLint border);
    void glCopyTexImage2D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height, GLint border);
    void glCopyTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLint x, GLint y, GLsizei width);
    void glCopyTexSubImage1DEXT(GLenum target, GLint level, GLint xoffset, GLint x, GLint y, GLsizei width);
    void glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height);
    void glCopyTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLint x, GLint y, GLsizei width, GLsizei height);
    void glCopyTextureImage1DEXT(GLuint texture, GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLint border);
    void glCopyTextureImage2DEXT(GLuint texture, GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height, GLint border);
    void glCopyTextureSubImage1D(GLuint texture, GLint level, GLint xoffset, GLint x, GLint y, GLsizei width);
    void glCopyTextureSubImage1DEXT(GLuint texture, GLenum target, GLint level, GLint xoffset, GLint x, GLint y, GLsizei width);
    void glCopyTextureSubImage2D(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height);
    void glCopyTextureSubImage2DEXT(GLuint texture, GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height);
    void glCopyTextureSubImage3D(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLint x, GLint y, GLsizei width, GLsizei height);
    void glCopyTextureSubImage3DEXT(GLuint texture, GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLint x, GLint y, GLsizei width, GLsizei height);
    void glCoverFillPathInstancedNV(GLsizei numPaths, GLenum pathNameType, const void* paths, GLuint pathBase, GLenum coverMode, GLenum transformType, const GLfloat* transformValues);
    void glCoverFillPathNV(GLuint path, GLenum coverMode);
    void glCoverStrokePathInstancedNV(GLsizei numPaths, GLenum pathNameType, const void* paths, GLuint pathBase, GLenum coverMode, GLenum transformType, const GLfloat* transformValues);
    void glCoverStrokePathNV(GLuint path, GLenum coverMode);
    void glCoverageModulationNV(GLenum components);
    void glCoverageModulationTableNV(GLsizei n, const GLfloat* v);
    void glCreateBuffers(GLsizei n, GLuint* buffers);
    void glCreateCommandListsNV(GLsizei n, GLuint* lists);
    void glCreateFramebuffers(GLsizei n, GLuint* framebuffers);
    void glCreateMemoryObjectsEXT(GLsizei n, GLuint* memoryObjects);
    void glCreatePerfQueryINTEL(GLuint queryId, GLuint* queryHandle);
    GLhandleARB glCreateProgramObjectARB(void);
    void glCreateProgramPipelines(GLsizei n, GLuint* pipelines);
    GLuint glCreateProgressFenceNVX(void);
    void glCreateQueries(GLenum target, GLsizei n, GLuint* ids);
    void glCreateRenderbuffers(GLsizei n, GLuint* renderbuffers);
    void glCreateSamplers(GLsizei n, GLuint* samplers);
    void glCreateSemaphoresNV(GLsizei n, GLuint* semaphores);
    GLuint glCreateShader(GLenum type);
    GLhandleARB glCreateShaderObjectARB(GLenum shaderType);
    GLuint glCreateShaderProgramEXT(GLenum type, const GLchar* string);
    GLuint glCreateShaderProgramv(GLenum type, GLsizei count, const GLchar* const* strings);
    void glCreateStatesNV(GLsizei n, GLuint* states);
    GLsync glCreateSyncFromCLeventARB(struct _cl_context* context, struct _cl_event* event, GLbitfield flags);
    void glCreateTextures(GLenum target, GLsizei n, GLuint* textures);
    void glCreateTransformFeedbacks(GLsizei n, GLuint* ids);
    void glCreateVertexArrays(GLsizei n, GLuint* arrays);
    void glCullFace(GLenum mode);
    void glCullParameterdvEXT(GLenum pname, GLdouble* params);
    void glCullParameterfvEXT(GLenum pname, GLfloat* params);
    void glCurrentPaletteMatrixARB(GLint index);
    void glDebugMessageCallback(GLDEBUGPROC callback, const void* userParam);
    void glDebugMessageCallbackAMD(GLDEBUGPROCAMD callback, void* userParam);
    void glDebugMessageControl(GLenum source, GLenum type, GLenum severity, GLsizei count, const GLuint* ids, GLboolean enabled);
    void glDebugMessageEnableAMD(GLenum category, GLenum severity, GLsizei count, const GLuint* ids, GLboolean enabled);
    void glDebugMessageInsert(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* buf);
    void glDebugMessageInsertAMD(GLenum category, GLenum severity, GLuint id, GLsizei length, const GLchar* buf);
    void glDeformSGIX(GLbitfield mask);
    void glDeformationMap3dSGIX(GLenum target, GLdouble u1, GLdouble u2, GLint ustride, GLint uorder, GLdouble v1, GLdouble v2, GLint vstride, GLint vorder, GLdouble w1, GLdouble w2, GLint wstride, GLint worder, const GLdouble* points);
    void glDeformationMap3fSGIX(GLenum target, GLfloat u1, GLfloat u2, GLint ustride, GLint uorder, GLfloat v1, GLfloat v2, GLint vstride, GLint vorder, GLfloat w1, GLfloat w2, GLint wstride, GLint worder, const GLfloat* points);
    void glDeleteAsyncMarkersSGIX(GLuint marker, GLsizei range);
    void glDeleteBuffers(GLsizei n, const GLuint* buffers);
    void glDeleteCommandListsNV(GLsizei n, const GLuint* lists);
    void glDeleteFencesAPPLE(GLsizei n, const GLuint* fences);
    void glDeleteFencesNV(GLsizei n, const GLuint* fences);
    void glDeleteFragmentShaderATI(GLuint id);
    void glDeleteFramebuffers(GLsizei n, const GLuint* framebuffers);
    void glDeleteLists(GLuint list, GLsizei range);
    void glDeleteMemoryObjectsEXT(GLsizei n, const GLuint* memoryObjects);
    void glDeleteNamedStringARB(GLint namelen, const GLchar* name);
    void glDeleteNamesAMD(GLenum identifier, GLuint num, const GLuint* names);
    void glDeleteObjectARB(GLhandleARB obj);
    void glDeleteOcclusionQueriesNV(GLsizei n, const GLuint* ids);
    void glDeletePathsNV(GLuint path, GLsizei range);
    void glDeletePerfMonitorsAMD(GLsizei n, GLuint* monitors);
    void glDeletePerfQueryINTEL(GLuint queryHandle);
    void glDeleteProgram(GLuint program);
    void glDeleteProgramPipelines(GLsizei n, const GLuint* pipelines);
    void glDeleteProgramsARB(GLsizei n, const GLuint* programs);
    void glDeleteProgramsNV(GLsizei n, const GLuint* programs);
    void glDeleteQueries(GLsizei n, const GLuint* ids);
    void glDeleteQueryResourceTagNV(GLsizei n, const GLint* tagIds);
    void glDeleteRenderbuffers(GLsizei n, const GLuint* renderbuffers);
    void glDeleteSamplers(GLsizei count, const GLuint* samplers);
    void glDeleteSemaphoresEXT(GLsizei n, const GLuint* semaphores);
    void glDeleteShader(GLuint shader);
    void glDeleteStatesNV(GLsizei n, const GLuint* states);
    void glDeleteSync(GLsync sync);
    void glDeleteTextures(GLsizei n, const GLuint* textures);
    void glDeleteTransformFeedbacks(GLsizei n, const GLuint* ids);
    void glDeleteTransformFeedbacksNV(GLsizei n, const GLuint* ids);
    void glDeleteVertexArrays(GLsizei n, const GLuint* arrays);
    void glDeleteVertexArraysAPPLE(GLsizei n, const GLuint* arrays);
    void glDeleteVertexShaderEXT(GLuint id);
    void glDepthBoundsEXT(GLclampd zmin, GLclampd zmax);
    void glDepthBoundsdNV(GLdouble zmin, GLdouble zmax);
    void glDepthFunc(GLenum func);
    void glDepthMask(GLboolean flag);
    void glDepthRange(GLclampd near_val, GLclampd far_val);
    void glDepthRangeArraydvNV(GLuint first, GLsizei count, const GLdouble* v);
    void glDepthRangeArrayv(GLuint first, GLsizei count, const GLdouble* v);
    void glDepthRangeIndexed(GLuint index, GLdouble n, GLdouble f);
    void glDepthRangeIndexeddNV(GLuint index, GLdouble n, GLdouble f);
    void glDepthRangedNV(GLdouble zNear, GLdouble zFar);
    void glDepthRangef(GLfloat n, GLfloat f);
    void glDepthRangefOES(GLclampf n, GLclampf f);
    void glDepthRangexOES(GLfixed n, GLfixed f);
    void glDetachObjectARB(GLhandleARB containerObj, GLhandleARB attachedObj);
    void glDetachShader(GLuint program, GLuint shader);
    void glDetailTexFuncSGIS(GLenum target, GLsizei n, const GLfloat* points);
    void glDisable(GLenum cap);
    void glDisableClientState(GLenum cap);
    void glDisableClientStateIndexedEXT(GLenum array, GLuint index);
    void glDisableClientStateiEXT(GLenum array, GLuint index);
    void glDisableIndexedEXT(GLenum target, GLuint index);
    void glDisableVariantClientStateEXT(GLuint id);
    void glDisableVertexArrayAttrib(GLuint vaobj, GLuint index);
    void glDisableVertexArrayAttribEXT(GLuint vaobj, GLuint index);
    void glDisableVertexArrayEXT(GLuint vaobj, GLenum array);
    void glDisableVertexAttribAPPLE(GLuint index, GLenum pname);
    void glDisableVertexAttribArray(GLuint index);
    void glDisablei(GLenum target, GLuint index);
    void glDispatchCompute(GLuint num_groups_x, GLuint num_groups_y, GLuint num_groups_z);
    void glDispatchComputeGroupSizeARB(GLuint num_groups_x, GLuint num_groups_y, GLuint num_groups_z, GLuint group_size_x, GLuint group_size_y, GLuint group_size_z);
    void glDispatchComputeIndirect(GLintptr indirect);
    void glDrawArrays(GLenum mode, GLint first, GLsizei count);
    void glDrawArraysIndirect(GLenum mode, const void* indirect);
    void glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instancecount);
    void glDrawArraysInstancedBaseInstance(GLenum mode, GLint first, GLsizei count, GLsizei instancecount, GLuint baseinstance);
    void glDrawBuffer(GLenum mode);
    void glDrawBuffers(GLsizei n, const GLenum* bufs);
    void glDrawBuffersATI(GLsizei n, const GLenum* bufs);
    void glDrawCommandsAddressNV(GLenum primitiveMode, const GLuint64* indirects, const GLsizei* sizes, GLuint count);
    void glDrawCommandsNV(GLenum primitiveMode, GLuint buffer, const GLintptr* indirects, const GLsizei* sizes, GLuint count);
    void glDrawCommandsStatesAddressNV(const GLuint64* indirects, const GLsizei* sizes, const GLuint* states, const GLuint* fbos, GLuint count);
    void glDrawCommandsStatesNV(GLuint buffer, const GLintptr* indirects, const GLsizei* sizes, const GLuint* states, const GLuint* fbos, GLuint count);
    void glDrawElementArrayAPPLE(GLenum mode, GLint first, GLsizei count);
    void glDrawElementArrayATI(GLenum mode, GLsizei count);
    void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices);
    void glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices, GLint basevertex);
    void glDrawElementsIndirect(GLenum mode, GLenum type, const void* indirect);
    void glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instancecount);
    void glDrawElementsInstancedBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instancecount, GLuint baseinstance);
    void glDrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instancecount, GLint basevertex);
    void glDrawElementsInstancedBaseVertexBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instancecount, GLint basevertex, GLuint baseinstance);
    void glDrawMeshArraysSUN(GLenum mode, GLint first, GLsizei count, GLsizei width);
    void glDrawMeshTasksIndirectNV(GLintptr indirect);
    void glDrawMeshTasksNV(GLuint first, GLuint count);
    void glDrawPixels(GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid* pixels);
    void glDrawRangeElementArrayAPPLE(GLenum mode, GLuint start, GLuint end, GLint first, GLsizei count);
    void glDrawRangeElementArrayATI(GLenum mode, GLuint start, GLuint end, GLsizei count);
    void glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices);
    void glDrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices, GLint basevertex);
    void glDrawTextureNV(GLuint texture, GLuint sampler, GLfloat x0, GLfloat y0, GLfloat x1, GLfloat y1, GLfloat z, GLfloat s0, GLfloat t0, GLfloat s1, GLfloat t1);
    void glDrawTransformFeedback(GLenum mode, GLuint id);
    void glDrawTransformFeedbackInstanced(GLenum mode, GLuint id, GLsizei instancecount);
    void glDrawTransformFeedbackNV(GLenum mode, GLuint id);
    void glDrawTransformFeedbackStream(GLenum mode, GLuint id, GLuint stream);
    void glDrawTransformFeedbackStreamInstanced(GLenum mode, GLuint id, GLuint stream, GLsizei instancecount);
    void glDrawVkImageNV(GLuint64 vkImage, GLuint sampler, GLfloat x0, GLfloat y0, GLfloat x1, GLfloat y1, GLfloat z, GLfloat s0, GLfloat t0, GLfloat s1, GLfloat t1);
    void glEGLImageTargetTexStorageEXT(GLenum target, GLeglImageOES image, const GLint* attrib_list);
    void glEGLImageTargetTextureStorageEXT(GLuint texture, GLeglImageOES image, const GLint* attrib_list);
    void glEdgeFlag(GLboolean flag);
    void glEdgeFlagFormatNV(GLsizei stride);
    void glEdgeFlagPointer(GLsizei stride, const GLvoid* ptr);
    void glEdgeFlagPointerEXT(GLsizei stride, GLsizei count, const GLboolean* pointer);
    void glEdgeFlagPointerListIBM(GLint stride, const GLboolean** pointer, GLint ptrstride);
    void glEdgeFlagv(const GLboolean* flag);
    void glElementPointerAPPLE(GLenum type, const void* pointer);
    void glElementPointerATI(GLenum type, const void* pointer);
    void glEnable(GLenum cap);
    void glEnableClientState(GLenum cap);
    void glEnableClientStateIndexedEXT(GLenum array, GLuint index);
    void glEnableClientStateiEXT(GLenum array, GLuint index);
    void glEnableIndexedEXT(GLenum target, GLuint index);
    void glEnableVariantClientStateEXT(GLuint id);
    void glEnableVertexArrayAttrib(GLuint vaobj, GLuint index);
    void glEnableVertexArrayAttribEXT(GLuint vaobj, GLuint index);
    void glEnableVertexArrayEXT(GLuint vaobj, GLenum array);
    void glEnableVertexAttribAPPLE(GLuint index, GLenum pname);
    void glEnableVertexAttribArray(GLuint index);
    void glEnablei(GLenum target, GLuint index);
    void glEndConditionalRenderNV(void);
    void glEndConditionalRenderNVX(void);
    void glEndFragmentShaderATI(void);
    void glEndOcclusionQueryNV(void);
    void glEndPerfMonitorAMD(GLuint monitor);
    void glEndPerfQueryINTEL(GLuint queryHandle);
    void glEndQuery(GLenum target);
    void glEndQueryIndexed(GLenum target, GLuint index);
    void glEndTransformFeedbackNV(void);
    void glEndVertexShaderEXT(void);
    void glEndVideoCaptureNV(GLuint video_capture_slot);
    void glEvalCoord1d(GLdouble u);
    void glEvalCoord1dv(const GLdouble* u);
    void glEvalCoord1f(GLfloat u);
    void glEvalCoord1fv(const GLfloat* u);
    void glEvalCoord1xOES(GLfixed u);
    void glEvalCoord1xvOES(const GLfixed* coords);
    void glEvalCoord2d(GLdouble u, GLdouble v);
    void glEvalCoord2dv(const GLdouble* u);
    void glEvalCoord2f(GLfloat u, GLfloat v);
    void glEvalCoord2fv(const GLfloat* u);
    void glEvalCoord2xOES(GLfixed u, GLfixed v);
    void glEvalCoord2xvOES(const GLfixed* coords);
    void glEvalMapsNV(GLenum target, GLenum mode);
    void glEvalMesh1(GLenum mode, GLint i1, GLint i2);
    void glEvalMesh2(GLenum mode, GLint i1, GLint i2, GLint j1, GLint j2);
    void glEvalPoint1(GLint i);
    void glEvalPoint2(GLint i, GLint j);
    void glEvaluateDepthValuesARB(void);
    void glExecuteProgramNV(GLenum target, GLuint id, const GLfloat* params);
    void glExtractComponentEXT(GLuint res, GLuint src, GLuint num);
    void glFeedbackBuffer(GLsizei size, GLenum type, GLfloat* buffer);
    void glFeedbackBufferxOES(GLsizei n, GLenum type, const GLfixed* buffer);
    GLsync glFenceSync(GLenum condition, GLbitfield flags);
    void glFinalCombinerInputNV(GLenum variable, GLenum input, GLenum mapping, GLenum componentUsage);
    GLint glFinishAsyncSGIX(GLuint* markerp);
    void glFinishFenceAPPLE(GLuint fence);
    void glFinishFenceNV(GLuint fence);
    void glFinishObjectAPPLE(GLenum object, GLint name);
    void glFinishTextureSUNX(void);
    void glFlushMappedBufferRange(GLenum target, GLintptr offset, GLsizeiptr length);
    void glFlushMappedBufferRangeAPPLE(GLenum target, GLintptr offset, GLsizeiptr size);
    void glFlushMappedNamedBufferRange(GLuint buffer, GLintptr offset, GLsizeiptr length);
    void glFlushMappedNamedBufferRangeEXT(GLuint buffer, GLintptr offset, GLsizeiptr length);
    void glFlushPixelDataRangeNV(GLenum target);
    void glFlushRasterSGIX(void);
    void glFlushStaticDataIBM(GLenum target);
    void glFlushVertexArrayRangeAPPLE(GLsizei length, void* pointer);
    void glFlushVertexArrayRangeNV(void);
    void glFogCoordFormatNV(GLenum type, GLsizei stride);
    void glFogCoordPointer(GLenum type, GLsizei stride, const void* pointer);
    void glFogCoordPointerEXT(GLenum type, GLsizei stride, const void* pointer);
    void glFogCoordPointerListIBM(GLenum type, GLint stride, const void** pointer, GLint ptrstride);
    void glFogCoordd(GLdouble coord);
    void glFogCoorddEXT(GLdouble coord);
    void glFogCoorddv(const GLdouble* coord);
    void glFogCoorddvEXT(const GLdouble* coord);
    void glFogCoordf(GLfloat coord);
    void glFogCoordfEXT(GLfloat coord);
    void glFogCoordfv(const GLfloat* coord);
    void glFogCoordfvEXT(const GLfloat* coord);
    void glFogCoordhNV(GLhalfNV fog);
    void glFogCoordhvNV(const GLhalfNV* fog);
    void glFogFuncSGIS(GLsizei n, const GLfloat* points);
    void glFogf(GLenum pname, GLfloat param);
    void glFogfv(GLenum pname, const GLfloat* params);
    void glFogi(GLenum pname, GLint param);
    void glFogiv(GLenum pname, const GLint* params);
    void glFogxOES(GLenum pname, GLfixed param);
    void glFogxvOES(GLenum pname, const GLfixed* param);
    void glFragmentColorMaterialSGIX(GLenum face, GLenum mode);
    void glFragmentCoverageColorNV(GLuint color);
    void glFragmentLightModelfSGIX(GLenum pname, GLfloat param);
    void glFragmentLightModelfvSGIX(GLenum pname, const GLfloat* params);
    void glFragmentLightModeliSGIX(GLenum pname, GLint param);
    void glFragmentLightModelivSGIX(GLenum pname, const GLint* params);
    void glFragmentLightfSGIX(GLenum light, GLenum pname, GLfloat param);
    void glFragmentLightfvSGIX(GLenum light, GLenum pname, const GLfloat* params);
    void glFragmentLightiSGIX(GLenum light, GLenum pname, GLint param);
    void glFragmentLightivSGIX(GLenum light, GLenum pname, const GLint* params);
    void glFragmentMaterialfSGIX(GLenum face, GLenum pname, GLfloat param);
    void glFragmentMaterialfvSGIX(GLenum face, GLenum pname, const GLfloat* params);
    void glFragmentMaterialiSGIX(GLenum face, GLenum pname, GLint param);
    void glFragmentMaterialivSGIX(GLenum face, GLenum pname, const GLint* params);
    void glFrameTerminatorGREMEDY(void);
    void glFrameZoomSGIX(GLint factor);
    void glFramebufferDrawBufferEXT(GLuint framebuffer, GLenum mode);
    void glFramebufferDrawBuffersEXT(GLuint framebuffer, GLsizei n, const GLenum* bufs);
    void glFramebufferFetchBarrierEXT(void);
    void glFramebufferParameteri(GLenum target, GLenum pname, GLint param);
    void glFramebufferParameteriMESA(GLenum target, GLenum pname, GLint param);
    void glFramebufferReadBufferEXT(GLuint framebuffer, GLenum mode);
    void glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer);
    void glFramebufferSampleLocationsfvARB(GLenum target, GLuint start, GLsizei count, const GLfloat* v);
    void glFramebufferSampleLocationsfvNV(GLenum target, GLuint start, GLsizei count, const GLfloat* v);
    void glFramebufferSamplePositionsfvAMD(GLenum target, GLuint numsamples, GLuint pixelindex, const GLfloat* values);
    void glFramebufferTexture(GLenum target, GLenum attachment, GLuint texture, GLint level);
    void glFramebufferTexture1D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
    void glFramebufferTexture1DEXT(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
    void glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
    void glFramebufferTexture3D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level, GLint zoffset);
    void glFramebufferTexture3DEXT(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level, GLint zoffset);
    void glFramebufferTextureFaceARB(GLenum target, GLenum attachment, GLuint texture, GLint level, GLenum face);
    void glFramebufferTextureFaceEXT(GLenum target, GLenum attachment, GLuint texture, GLint level, GLenum face);
    void glFramebufferTextureLayer(GLenum target, GLenum attachment, GLuint texture, GLint level, GLint layer);
    void glFramebufferTextureMultiviewOVR(GLenum target, GLenum attachment, GLuint texture, GLint level, GLint baseViewIndex, GLsizei numViews);
    void glFreeObjectBufferATI(GLuint buffer);
    void glFrontFace(GLenum mode);
    void glFrustum(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble near_val, GLdouble far_val);
    void glFrustumfOES(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f);
    void glFrustumxOES(GLfixed l, GLfixed r, GLfixed b, GLfixed t, GLfixed n, GLfixed f);
    GLuint glGenAsyncMarkersSGIX(GLsizei range);
    void glGenBuffers(GLsizei n, GLuint* buffers);
    void glGenFencesAPPLE(GLsizei n, GLuint* fences);
    void glGenFencesNV(GLsizei n, GLuint* fences);
    GLuint glGenFragmentShadersATI(GLuint range);
    void glGenFramebuffers(GLsizei n, GLuint* framebuffers);
    GLuint glGenLists(GLsizei range);
    void glGenNamesAMD(GLenum identifier, GLuint num, GLuint* names);
    void glGenOcclusionQueriesNV(GLsizei n, GLuint* ids);
    GLuint glGenPathsNV(GLsizei range);
    void glGenPerfMonitorsAMD(GLsizei n, GLuint* monitors);
    void glGenProgramPipelines(GLsizei n, GLuint* pipelines);
    void glGenProgramsARB(GLsizei n, GLuint* programs);
    void glGenProgramsNV(GLsizei n, GLuint* programs);
    void glGenQueries(GLsizei n, GLuint* ids);
    void glGenQueryResourceTagNV(GLsizei n, GLint* tagIds);
    void glGenRenderbuffers(GLsizei n, GLuint* renderbuffers);
    void glGenSamplers(GLsizei count, GLuint* samplers);
    void glGenSemaphoresEXT(GLsizei n, GLuint* semaphores);
    GLuint glGenSymbolsEXT(GLenum datatype, GLenum storagetype, GLenum range, GLuint components);
    void glGenTextures(GLsizei n, GLuint* textures);
    void glGenTransformFeedbacks(GLsizei n, GLuint* ids);
    void glGenTransformFeedbacksNV(GLsizei n, GLuint* ids);
    void glGenVertexArrays(GLsizei n, GLuint* arrays);
    void glGenVertexArraysAPPLE(GLsizei n, GLuint* arrays);
    GLuint glGenVertexShadersEXT(GLuint range);
    void glGenerateMipmap(GLenum target);
    void glGenerateMultiTexMipmapEXT(GLenum texunit, GLenum target);
    void glGenerateTextureMipmap(GLuint texture);
    void glGenerateTextureMipmapEXT(GLuint texture, GLenum target);
    void glGetActiveAtomicCounterBufferiv(GLuint program, GLuint bufferIndex, GLenum pname, GLint* params);
    void glGetActiveAttrib(GLuint program, GLuint index, GLsizei bufSize, GLsizei* length, GLint* size, GLenum* type, GLchar* name);
    void glGetActiveSubroutineName(GLuint program, GLenum shadertype, GLuint index, GLsizei bufSize, GLsizei* length, GLchar* name);
    void glGetActiveSubroutineUniformName(GLuint program, GLenum shadertype, GLuint index, GLsizei bufSize, GLsizei* length, GLchar* name);
    void glGetActiveSubroutineUniformiv(GLuint program, GLenum shadertype, GLuint index, GLenum pname, GLint* values);
    void glGetActiveUniform(GLuint program, GLuint index, GLsizei bufSize, GLsizei* length, GLint* size, GLenum* type, GLchar* name);
    void glGetActiveUniformBlockName(GLuint program, GLuint uniformBlockIndex, GLsizei bufSize, GLsizei* length, GLchar* uniformBlockName);
    void glGetActiveUniformBlockiv(GLuint program, GLuint uniformBlockIndex, GLenum pname, GLint* params);
    void glGetActiveUniformName(GLuint program, GLuint uniformIndex, GLsizei bufSize, GLsizei* length, GLchar* uniformName);
    void glGetActiveUniformsiv(GLuint program, GLsizei uniformCount, const GLuint* uniformIndices, GLenum pname, GLint* params);
    void glGetActiveVaryingNV(GLuint program, GLuint index, GLsizei bufSize, GLsizei* length, GLsizei* size, GLenum* type, GLchar* name);
    void glGetArrayObjectfvATI(GLenum array, GLenum pname, GLfloat* params);
    void glGetArrayObjectivATI(GLenum array, GLenum pname, GLint* params);
    void glGetAttachedObjectsARB(GLhandleARB containerObj, GLsizei maxCount, GLsizei* count, GLhandleARB* obj);
    void glGetAttachedShaders(GLuint program, GLsizei maxCount, GLsizei* count, GLuint* shaders);
    GLint glGetAttribLocation(GLuint program, const GLchar* name);
    void glGetBooleanIndexedvEXT(GLenum target, GLuint index, GLboolean* data);
    void glGetBooleani_v(GLenum target, GLuint index, GLboolean* data);
    void glGetBooleanv(GLenum pname, GLboolean* data);
    void glGetBufferParameteri64v(GLenum target, GLenum pname, GLint64* params);
    void glGetBufferParameteriv(GLenum target, GLenum pname, GLint* params);
    void glGetBufferParameterui64vNV(GLenum target, GLenum pname, GLuint64EXT* params);
    void glGetBufferPointerv(GLenum target, GLenum pname, void** params);
    void glGetBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, void* data);
    void glGetBufferSubDataARB(GLenum target, GLintptrARB offset, GLsizeiptrARB size, void* data);
    void glGetClipPlane(GLenum plane, GLdouble* equation);
    void glGetClipPlanefOES(GLenum plane, GLfloat* equation);
    void glGetClipPlanexOES(GLenum plane, GLfixed* equation);
    void glGetColorTable(GLenum target, GLenum format, GLenum type, void* table);
    void glGetColorTableEXT(GLenum target, GLenum format, GLenum type, void* data);
    void glGetColorTableParameterfv(GLenum target, GLenum pname, GLfloat* params);
    void glGetColorTableParameterfvEXT(GLenum target, GLenum pname, GLfloat* params);
    void glGetColorTableParameterfvSGI(GLenum target, GLenum pname, GLfloat* params);
    void glGetColorTableParameteriv(GLenum target, GLenum pname, GLint* params);
    void glGetColorTableParameterivEXT(GLenum target, GLenum pname, GLint* params);
    void glGetColorTableParameterivSGI(GLenum target, GLenum pname, GLint* params);
    void glGetColorTableSGI(GLenum target, GLenum format, GLenum type, void* table);
    void glGetCombinerInputParameterfvNV(GLenum stage, GLenum portion, GLenum variable, GLenum pname, GLfloat* params);
    void glGetCombinerInputParameterivNV(GLenum stage, GLenum portion, GLenum variable, GLenum pname, GLint* params);
    void glGetCombinerOutputParameterfvNV(GLenum stage, GLenum portion, GLenum pname, GLfloat* params);
    void glGetCombinerOutputParameterivNV(GLenum stage, GLenum portion, GLenum pname, GLint* params);
    void glGetCombinerStageParameterfvNV(GLenum stage, GLenum pname, GLfloat* params);
    GLuint glGetCommandHeaderNV(GLenum tokenID, GLuint size);
    void glGetCompressedMultiTexImageEXT(GLenum texunit, GLenum target, GLint lod, void* img);
    void glGetCompressedTexImage(GLenum target, GLint level, void* img);
    void glGetCompressedTexImageARB(GLenum target, GLint level, void* img);
    void glGetCompressedTextureImage(GLuint texture, GLint level, GLsizei bufSize, void* pixels);
    void glGetCompressedTextureImageEXT(GLuint texture, GLenum target, GLint lod, void* img);
    void glGetCompressedTextureSubImage(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLsizei bufSize, void* pixels);
    void glGetConvolutionFilter(GLenum target, GLenum format, GLenum type, void* image);
    void glGetConvolutionFilterEXT(GLenum target, GLenum format, GLenum type, void* image);
    void glGetConvolutionParameterfv(GLenum target, GLenum pname, GLfloat* params);
    void glGetConvolutionParameterfvEXT(GLenum target, GLenum pname, GLfloat* params);
    void glGetConvolutionParameteriv(GLenum target, GLenum pname, GLint* params);
    void glGetConvolutionParameterivEXT(GLenum target, GLenum pname, GLint* params);
    void glGetConvolutionParameterxvOES(GLenum target, GLenum pname, GLfixed* params);
    void glGetCoverageModulationTableNV(GLsizei bufSize, GLfloat* v);
    GLuint glGetDebugMessageLog(GLuint count, GLsizei bufSize, GLenum* sources, GLenum* types, GLuint* ids, GLenum* severities, GLsizei* lengths, GLchar* messageLog);
    GLuint glGetDebugMessageLogAMD(GLuint count, GLsizei bufSize, GLenum* categories, GLuint* severities, GLuint* ids, GLsizei* lengths, GLchar* message);
    void glGetDetailTexFuncSGIS(GLenum target, GLfloat* points);
    void glGetDoubleIndexedvEXT(GLenum target, GLuint index, GLdouble* data);
    void glGetDoublei_v(GLenum target, GLuint index, GLdouble* data);
    void glGetDoublei_vEXT(GLenum pname, GLuint index, GLdouble* params);
    void glGetDoublev(GLenum pname, GLdouble* params);
    void glGetFenceivNV(GLuint fence, GLenum pname, GLint* params);
    void glGetFinalCombinerInputParameterfvNV(GLenum variable, GLenum pname, GLfloat* params);
    void glGetFinalCombinerInputParameterivNV(GLenum variable, GLenum pname, GLint* params);
    void glGetFirstPerfQueryIdINTEL(GLuint* queryId);
    void glGetFixedvOES(GLenum pname, GLfixed* params);
    void glGetFloatIndexedvEXT(GLenum target, GLuint index, GLfloat* data);
    void glGetFloati_v(GLenum target, GLuint index, GLfloat* data);
    void glGetFloati_vEXT(GLenum pname, GLuint index, GLfloat* params);
    void glGetFloatv(GLenum pname, GLfloat* data);
    void glGetFogFuncSGIS(GLfloat* points);
    GLint glGetFragDataIndex(GLuint program, const GLchar* name);
    GLint glGetFragDataLocation(GLuint program, const GLchar* name);
    void glGetFragmentLightfvSGIX(GLenum light, GLenum pname, GLfloat* params);
    void glGetFragmentLightivSGIX(GLenum light, GLenum pname, GLint* params);
    void glGetFragmentMaterialfvSGIX(GLenum face, GLenum pname, GLfloat* params);
    void glGetFragmentMaterialivSGIX(GLenum face, GLenum pname, GLint* params);
    void glGetFramebufferAttachmentParameteriv(GLenum target, GLenum attachment, GLenum pname, GLint* params);
    void glGetFramebufferParameterfvAMD(GLenum target, GLenum pname, GLuint numsamples, GLuint pixelindex, GLsizei size, GLfloat* values);
    void glGetFramebufferParameteriv(GLenum target, GLenum pname, GLint* params);
    void glGetFramebufferParameterivMESA(GLenum target, GLenum pname, GLint* params);
    GLhandleARB glGetHandleARB(GLenum pname);
    void glGetHistogram(GLenum target, GLboolean reset, GLenum format, GLenum type, void* values);
    void glGetHistogramEXT(GLenum target, GLboolean reset, GLenum format, GLenum type, void* values);
    void glGetHistogramParameterfv(GLenum target, GLenum pname, GLfloat* params);
    void glGetHistogramParameterfvEXT(GLenum target, GLenum pname, GLfloat* params);
    void glGetHistogramParameteriv(GLenum target, GLenum pname, GLint* params);
    void glGetHistogramParameterivEXT(GLenum target, GLenum pname, GLint* params);
    void glGetHistogramParameterxvOES(GLenum target, GLenum pname, GLfixed* params);
    GLuint64 glGetImageHandleARB(GLuint texture, GLint level, GLboolean layered, GLint layer, GLenum format);
    GLuint64 glGetImageHandleNV(GLuint texture, GLint level, GLboolean layered, GLint layer, GLenum format);
    void glGetImageTransformParameterfvHP(GLenum target, GLenum pname, GLfloat* params);
    void glGetImageTransformParameterivHP(GLenum target, GLenum pname, GLint* params);
    void glGetInfoLogARB(GLhandleARB obj, GLsizei maxLength, GLsizei* length, GLcharARB* infoLog);
    GLint glGetInstrumentsSGIX(void);
    void glGetInteger64i_v(GLenum target, GLuint index, GLint64* data);
    void glGetInteger64v(GLenum pname, GLint64* data);
    void glGetIntegerIndexedvEXT(GLenum target, GLuint index, GLint* data);
    void glGetIntegeri_v(GLenum target, GLuint index, GLint* data);
    void glGetIntegerui64i_vNV(GLenum value, GLuint index, GLuint64EXT* result);
    void glGetIntegerui64vNV(GLenum value, GLuint64EXT* result);
    void glGetIntegerv(GLenum pname, GLint* data);
    void glGetInternalformatSampleivNV(GLenum target, GLenum internalformat, GLsizei samples, GLenum pname, GLsizei count, GLint* params);
    void glGetInternalformati64v(GLenum target, GLenum internalformat, GLenum pname, GLsizei count, GLint64* params);
    void glGetInternalformativ(GLenum target, GLenum internalformat, GLenum pname, GLsizei bufSize, GLint* params);
    void glGetInvariantBooleanvEXT(GLuint id, GLenum value, GLboolean* data);
    void glGetInvariantFloatvEXT(GLuint id, GLenum value, GLfloat* data);
    void glGetInvariantIntegervEXT(GLuint id, GLenum value, GLint* data);
    void glGetLightfv(GLenum light, GLenum pname, GLfloat* params);
    void glGetLightiv(GLenum light, GLenum pname, GLint* params);
    void glGetLightxOES(GLenum light, GLenum pname, GLfixed* params);
    void glGetListParameterfvSGIX(GLuint list, GLenum pname, GLfloat* params);
    void glGetListParameterivSGIX(GLuint list, GLenum pname, GLint* params);
    void glGetLocalConstantBooleanvEXT(GLuint id, GLenum value, GLboolean* data);
    void glGetLocalConstantFloatvEXT(GLuint id, GLenum value, GLfloat* data);
    void glGetLocalConstantIntegervEXT(GLuint id, GLenum value, GLint* data);
    void glGetMapAttribParameterfvNV(GLenum target, GLuint index, GLenum pname, GLfloat* params);
    void glGetMapAttribParameterivNV(GLenum target, GLuint index, GLenum pname, GLint* params);
    void glGetMapControlPointsNV(GLenum target, GLuint index, GLenum type, GLsizei ustride, GLsizei vstride, GLboolean packed, void* points);
    void glGetMapParameterfvNV(GLenum target, GLenum pname, GLfloat* params);
    void glGetMapParameterivNV(GLenum target, GLenum pname, GLint* params);
    void glGetMapdv(GLenum target, GLenum query, GLdouble* v);
    void glGetMapfv(GLenum target, GLenum query, GLfloat* v);
    void glGetMapiv(GLenum target, GLenum query, GLint* v);
    void glGetMapxvOES(GLenum target, GLenum query, GLfixed* v);
    void glGetMaterialfv(GLenum face, GLenum pname, GLfloat* params);
    void glGetMaterialiv(GLenum face, GLenum pname, GLint* params);
    void glGetMaterialxOES(GLenum face, GLenum pname, GLfixed param);
    void glGetMemoryObjectDetachedResourcesuivNV(GLuint memory, GLenum pname, GLint first, GLsizei count, GLuint* params);
    void glGetMemoryObjectParameterivEXT(GLuint memoryObject, GLenum pname, GLint* params);
    void glGetMinmax(GLenum target, GLboolean reset, GLenum format, GLenum type, void* values);
    void glGetMinmaxEXT(GLenum target, GLboolean reset, GLenum format, GLenum type, void* values);
    void glGetMinmaxParameterfv(GLenum target, GLenum pname, GLfloat* params);
    void glGetMinmaxParameterfvEXT(GLenum target, GLenum pname, GLfloat* params);
    void glGetMinmaxParameteriv(GLenum target, GLenum pname, GLint* params);
    void glGetMinmaxParameterivEXT(GLenum target, GLenum pname, GLint* params);
    void glGetMultiTexEnvfvEXT(GLenum texunit, GLenum target, GLenum pname, GLfloat* params);
    void glGetMultiTexEnvivEXT(GLenum texunit, GLenum target, GLenum pname, GLint* params);
    void glGetMultiTexGendvEXT(GLenum texunit, GLenum coord, GLenum pname, GLdouble* params);
    void glGetMultiTexGenfvEXT(GLenum texunit, GLenum coord, GLenum pname, GLfloat* params);
    void glGetMultiTexGenivEXT(GLenum texunit, GLenum coord, GLenum pname, GLint* params);
    void glGetMultiTexImageEXT(GLenum texunit, GLenum target, GLint level, GLenum format, GLenum type, void* pixels);
    void glGetMultiTexLevelParameterfvEXT(GLenum texunit, GLenum target, GLint level, GLenum pname, GLfloat* params);
    void glGetMultiTexLevelParameterivEXT(GLenum texunit, GLenum target, GLint level, GLenum pname, GLint* params);
    void glGetMultiTexParameterIivEXT(GLenum texunit, GLenum target, GLenum pname, GLint* params);
    void glGetMultiTexParameterIuivEXT(GLenum texunit, GLenum target, GLenum pname, GLuint* params);
    void glGetMultiTexParameterfvEXT(GLenum texunit, GLenum target, GLenum pname, GLfloat* params);
    void glGetMultiTexParameterivEXT(GLenum texunit, GLenum target, GLenum pname, GLint* params);
    void glGetMultisamplefv(GLenum pname, GLuint index, GLfloat* val);
    void glGetMultisamplefvNV(GLenum pname, GLuint index, GLfloat* val);
    void glGetNamedBufferParameteri64v(GLuint buffer, GLenum pname, GLint64* params);
    void glGetNamedBufferParameteriv(GLuint buffer, GLenum pname, GLint* params);
    void glGetNamedBufferParameterivEXT(GLuint buffer, GLenum pname, GLint* params);
    void glGetNamedBufferParameterui64vNV(GLuint buffer, GLenum pname, GLuint64EXT* params);
    void glGetNamedBufferPointerv(GLuint buffer, GLenum pname, void** params);
    void glGetNamedBufferPointervEXT(GLuint buffer, GLenum pname, void** params);
    void glGetNamedBufferSubData(GLuint buffer, GLintptr offset, GLsizeiptr size, void* data);
    void glGetNamedBufferSubDataEXT(GLuint buffer, GLintptr offset, GLsizeiptr size, void* data);
    void glGetNamedFramebufferAttachmentParameteriv(GLuint framebuffer, GLenum attachment, GLenum pname, GLint* params);
    void glGetNamedFramebufferAttachmentParameterivEXT(GLuint framebuffer, GLenum attachment, GLenum pname, GLint* params);
    void glGetNamedFramebufferParameterfvAMD(GLuint framebuffer, GLenum pname, GLuint numsamples, GLuint pixelindex, GLsizei size, GLfloat* values);
    void glGetNamedFramebufferParameteriv(GLuint framebuffer, GLenum pname, GLint* param);
    void glGetNamedFramebufferParameterivEXT(GLuint framebuffer, GLenum pname, GLint* params);
    void glGetNamedProgramLocalParameterIivEXT(GLuint program, GLenum target, GLuint index, GLint* params);
    void glGetNamedProgramLocalParameterIuivEXT(GLuint program, GLenum target, GLuint index, GLuint* params);
    void glGetNamedProgramLocalParameterdvEXT(GLuint program, GLenum target, GLuint index, GLdouble* params);
    void glGetNamedProgramLocalParameterfvEXT(GLuint program, GLenum target, GLuint index, GLfloat* params);
    void glGetNamedProgramStringEXT(GLuint program, GLenum target, GLenum pname, void* string);
    void glGetNamedProgramivEXT(GLuint program, GLenum target, GLenum pname, GLint* params);
    void glGetNamedRenderbufferParameteriv(GLuint renderbuffer, GLenum pname, GLint* params);
    void glGetNamedRenderbufferParameterivEXT(GLuint renderbuffer, GLenum pname, GLint* params);
    void glGetNamedStringARB(GLint namelen, const GLchar* name, GLsizei bufSize, GLint* strinen, GLchar* string);
    void glGetNamedStringivARB(GLint namelen, const GLchar* name, GLenum pname, GLint* params);
    void glGetNextPerfQueryIdINTEL(GLuint queryId, GLuint* nextQueryId);
    void glGetObjectBufferfvATI(GLuint buffer, GLenum pname, GLfloat* params);
    void glGetObjectBufferivATI(GLuint buffer, GLenum pname, GLint* params);
    void glGetObjectLabel(GLenum identifier, GLuint name, GLsizei bufSize, GLsizei* length, GLchar* label);
    void glGetObjectParameterfvARB(GLhandleARB obj, GLenum pname, GLfloat* params);
    void glGetObjectParameterivAPPLE(GLenum objectType, GLuint name, GLenum pname, GLint* params);
    void glGetObjectParameterivARB(GLhandleARB obj, GLenum pname, GLint* params);
    void glGetObjectPtrLabel(const void* ptr, GLsizei bufSize, GLsizei* length, GLchar* label);
    void glGetOcclusionQueryivNV(GLuint id, GLenum pname, GLint* params);
    void glGetOcclusionQueryuivNV(GLuint id, GLenum pname, GLuint* params);
    void glGetPathColorGenfvNV(GLenum color, GLenum pname, GLfloat* value);
    void glGetPathColorGenivNV(GLenum color, GLenum pname, GLint* value);
    void glGetPathCommandsNV(GLuint path, GLubyte* commands);
    void glGetPathCoordsNV(GLuint path, GLfloat* coords);
    void glGetPathDashArrayNV(GLuint path, GLfloat* dashArray);
    GLfloat glGetPathLengthNV(GLuint path, GLsizei startSegment, GLsizei numSegments);
    void glGetPathMetricRangeNV(GLbitfield metricQueryMask, GLuint firstPathName, GLsizei numPaths, GLsizei stride, GLfloat* metrics);
    void glGetPathMetricsNV(GLbitfield metricQueryMask, GLsizei numPaths, GLenum pathNameType, const void* paths, GLuint pathBase, GLsizei stride, GLfloat* metrics);
    void glGetPathParameterfvNV(GLuint path, GLenum pname, GLfloat* value);
    void glGetPathParameterivNV(GLuint path, GLenum pname, GLint* value);
    void glGetPathSpacingNV(GLenum pathListMode, GLsizei numPaths, GLenum pathNameType, const void* paths, GLuint pathBase, GLfloat advanceScale, GLfloat kerningScale, GLenum transformType, GLfloat* returnedSpacing);
    void glGetPathTexGenfvNV(GLenum texCoordSet, GLenum pname, GLfloat* value);
    void glGetPathTexGenivNV(GLenum texCoordSet, GLenum pname, GLint* value);
    void glGetPerfCounterInfoINTEL(GLuint queryId, GLuint counterId, GLuint counterNameLength, GLchar* counterName, GLuint counterDescLength, GLchar* counterDesc, GLuint* counterOffset, GLuint* counterDataSize, GLuint* counterTypeEnum, GLuint* counterDataTypeEnum, GLuint64* rawCounterMaxValue);
    void glGetPerfMonitorCounterDataAMD(GLuint monitor, GLenum pname, GLsizei dataSize, GLuint* data, GLint* bytesWritten);
    void glGetPerfMonitorCounterInfoAMD(GLuint group, GLuint counter, GLenum pname, void* data);
    void glGetPerfMonitorCounterStringAMD(GLuint group, GLuint counter, GLsizei bufSize, GLsizei* length, GLchar* counterString);
    void glGetPerfMonitorCountersAMD(GLuint group, GLint* numCounters, GLint* maxActiveCounters, GLsizei counterSize, GLuint* counters);
    void glGetPerfMonitorGroupStringAMD(GLuint group, GLsizei bufSize, GLsizei* length, GLchar* groupString);
    void glGetPerfMonitorGroupsAMD(GLint* numGroups, GLsizei groupsSize, GLuint* groups);
    void glGetPerfQueryDataINTEL(GLuint queryHandle, GLuint flags, GLsizei dataSize, void* data, GLuint* bytesWritten);
    void glGetPerfQueryIdByNameINTEL(GLchar* queryName, GLuint* queryId);
    void glGetPerfQueryInfoINTEL(GLuint queryId, GLuint queryNameLength, GLchar* queryName, GLuint* dataSize, GLuint* noCounters, GLuint* noInstances, GLuint* capsMask);
    void glGetPixelMapfv(GLenum map, GLfloat* values);
    void glGetPixelMapuiv(GLenum map, GLuint* values);
    void glGetPixelMapusv(GLenum map, GLushort* values);
    void glGetPixelMapxv(GLenum map, GLint size, GLfixed* values);
    void glGetPixelTexGenParameterfvSGIS(GLenum pname, GLfloat* params);
    void glGetPixelTexGenParameterivSGIS(GLenum pname, GLint* params);
    void glGetPixelTransformParameterfvEXT(GLenum target, GLenum pname, GLfloat* params);
    void glGetPixelTransformParameterivEXT(GLenum target, GLenum pname, GLint* params);
    void glGetPointerIndexedvEXT(GLenum target, GLuint index, void** data);
    void glGetPointeri_vEXT(GLenum pname, GLuint index, void** params);
    void glGetPointerv(GLenum pname, void** params);
    void glGetPolygonStipple(GLubyte* mask);
    void glGetProgramBinary(GLuint program, GLsizei bufSize, GLsizei* length, GLenum* binaryFormat, void* binary);
    void glGetProgramEnvParameterIivNV(GLenum target, GLuint index, GLint* params);
    void glGetProgramEnvParameterIuivNV(GLenum target, GLuint index, GLuint* params);
    void glGetProgramEnvParameterdvARB(GLenum target, GLuint index, GLdouble* params);
    void glGetProgramEnvParameterfvARB(GLenum target, GLuint index, GLfloat* params);
    void glGetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei* length, GLchar* infoLog);
    void glGetProgramInterfaceiv(GLuint program, GLenum programInterface, GLenum pname, GLint* params);
    void glGetProgramLocalParameterIivNV(GLenum target, GLuint index, GLint* params);
    void glGetProgramLocalParameterIuivNV(GLenum target, GLuint index, GLuint* params);
    void glGetProgramLocalParameterdvARB(GLenum target, GLuint index, GLdouble* params);
    void glGetProgramLocalParameterfvARB(GLenum target, GLuint index, GLfloat* params);
    void glGetProgramNamedParameterdvNV(GLuint id, GLsizei len, const GLubyte* name, GLdouble* params);
    void glGetProgramNamedParameterfvNV(GLuint id, GLsizei len, const GLubyte* name, GLfloat* params);
    void glGetProgramParameterdvNV(GLenum target, GLuint index, GLenum pname, GLdouble* params);
    void glGetProgramParameterfvNV(GLenum target, GLuint index, GLenum pname, GLfloat* params);
    void glGetProgramPipelineInfoLog(GLuint pipeline, GLsizei bufSize, GLsizei* length, GLchar* infoLog);
    void glGetProgramPipelineiv(GLuint pipeline, GLenum pname, GLint* params);
    GLuint glGetProgramResourceIndex(GLuint program, GLenum programInterface, const GLchar* name);
    GLint glGetProgramResourceLocation(GLuint program, GLenum programInterface, const GLchar* name);
    GLint glGetProgramResourceLocationIndex(GLuint program, GLenum programInterface, const GLchar* name);
    void glGetProgramResourceName(GLuint program, GLenum programInterface, GLuint index, GLsizei bufSize, GLsizei* length, GLchar* name);
    void glGetProgramResourcefvNV(GLuint program, GLenum programInterface, GLuint index, GLsizei propCount, const GLenum* props, GLsizei count, GLsizei* length, GLfloat* params);
    void glGetProgramResourceiv(GLuint program, GLenum programInterface, GLuint index, GLsizei propCount, const GLenum* props, GLsizei bufSize, GLsizei* length, GLint* params);
    void glGetProgramStageiv(GLuint program, GLenum shadertype, GLenum pname, GLint* values);
    void glGetProgramStringARB(GLenum target, GLenum pname, void* string);
    void glGetProgramStringNV(GLuint id, GLenum pname, GLubyte* program);
    void glGetProgramSubroutineParameteruivNV(GLenum target, GLuint index, GLuint* param);
    void glGetProgramiv(GLuint program, GLenum pname, GLint* params);
    void glGetProgramivNV(GLuint id, GLenum pname, GLint* params);
    void glGetQueryBufferObjecti64v(GLuint id, GLuint buffer, GLenum pname, GLintptr offset);
    void glGetQueryBufferObjectiv(GLuint id, GLuint buffer, GLenum pname, GLintptr offset);
    void glGetQueryBufferObjectui64v(GLuint id, GLuint buffer, GLenum pname, GLintptr offset);
    void glGetQueryBufferObjectuiv(GLuint id, GLuint buffer, GLenum pname, GLintptr offset);
    void glGetQueryIndexediv(GLenum target, GLuint index, GLenum pname, GLint* params);
    void glGetQueryObjecti64v(GLuint id, GLenum pname, GLint64* params);
    void glGetQueryObjecti64vEXT(GLuint id, GLenum pname, GLint64* params);
    void glGetQueryObjectiv(GLuint id, GLenum pname, GLint* params);
    void glGetQueryObjectivARB(GLuint id, GLenum pname, GLint* params);
    void glGetQueryObjectui64v(GLuint id, GLenum pname, GLuint64* params);
    void glGetQueryObjectui64vEXT(GLuint id, GLenum pname, GLuint64* params);
    void glGetQueryObjectuiv(GLuint id, GLenum pname, GLuint* params);
    void glGetQueryiv(GLenum target, GLenum pname, GLint* params);
    void glGetRenderbufferParameteriv(GLenum target, GLenum pname, GLint* params);
    void glGetSamplerParameterIiv(GLuint sampler, GLenum pname, GLint* params);
    void glGetSamplerParameterIuiv(GLuint sampler, GLenum pname, GLuint* params);
    void glGetSamplerParameterfv(GLuint sampler, GLenum pname, GLfloat* params);
    void glGetSamplerParameteriv(GLuint sampler, GLenum pname, GLint* params);
    void glGetSemaphoreParameterivNV(GLuint semaphore, GLenum pname, GLint* params);
    void glGetSemaphoreParameterui64vEXT(GLuint semaphore, GLenum pname, GLuint64* params);
    void glGetSeparableFilter(GLenum target, GLenum format, GLenum type, void* row, void* column, void* span);
    void glGetSeparableFilterEXT(GLenum target, GLenum format, GLenum type, void* row, void* column, void* span);
    void glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* infoLog);
    void glGetShaderPrecisionFormat(GLenum shadertype, GLenum precisiontype, GLint* range, GLint* precision);
    void glGetShaderSource(GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* source);
    void glGetShaderiv(GLuint shader, GLenum pname, GLint* params);
    void glGetShadingRateImagePaletteNV(GLuint viewport, GLuint entry, GLenum* rate);
    void glGetShadingRateSampleLocationivNV(GLenum rate, GLuint samples, GLuint index, GLint* location);
    void glGetSharpenTexFuncSGIS(GLenum target, GLfloat* points);
    GLushort glGetStageIndexNV(GLenum shadertype);
    const GLubyte* glGetString(GLenum name);
    const GLubyte* glGetStringi(GLenum name, GLuint index);
    GLuint glGetSubroutineIndex(GLuint program, GLenum shadertype, const GLchar* name);
    GLint glGetSubroutineUniformLocation(GLuint program, GLenum shadertype, const GLchar* name);
    void glGetSynciv(GLsync sync, GLenum pname, GLsizei bufSize, GLsizei* length, GLint* values);
    void glGetTexBumpParameterfvATI(GLenum pname, GLfloat* param);
    void glGetTexBumpParameterivATI(GLenum pname, GLint* param);
    void glGetTexEnvfv(GLenum target, GLenum pname, GLfloat* params);
    void glGetTexEnviv(GLenum target, GLenum pname, GLint* params);
    void glGetTexEnvxvOES(GLenum target, GLenum pname, GLfixed* params);
    void glGetTexFilterFuncSGIS(GLenum target, GLenum filter, GLfloat* weights);
    void glGetTexGendv(GLenum coord, GLenum pname, GLdouble* params);
    void glGetTexGenfv(GLenum coord, GLenum pname, GLfloat* params);
    void glGetTexGeniv(GLenum coord, GLenum pname, GLint* params);
    void glGetTexGenxvOES(GLenum coord, GLenum pname, GLfixed* params);
    void glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid* pixels);
    void glGetTexLevelParameterfv(GLenum target, GLint level, GLenum pname, GLfloat* params);
    void glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint* params);
    void glGetTexLevelParameterxvOES(GLenum target, GLint level, GLenum pname, GLfixed* params);
    void glGetTexParameterIiv(GLenum target, GLenum pname, GLint* params);
    void glGetTexParameterIuiv(GLenum target, GLenum pname, GLuint* params);
    void glGetTexParameterPointervAPPLE(GLenum target, GLenum pname, void** params);
    void glGetTexParameterfv(GLenum target, GLenum pname, GLfloat* params);
    void glGetTexParameteriv(GLenum target, GLenum pname, GLint* params);
    void glGetTexParameterxvOES(GLenum target, GLenum pname, GLfixed* params);
    GLuint64 glGetTextureHandleARB(GLuint texture);
    GLuint64 glGetTextureHandleNV(GLuint texture);
    void glGetTextureImage(GLuint texture, GLint level, GLenum format, GLenum type, GLsizei bufSize, void* pixels);
    void glGetTextureImageEXT(GLuint texture, GLenum target, GLint level, GLenum format, GLenum type, void* pixels);
    void glGetTextureLevelParameterfv(GLuint texture, GLint level, GLenum pname, GLfloat* params);
    void glGetTextureLevelParameterfvEXT(GLuint texture, GLenum target, GLint level, GLenum pname, GLfloat* params);
    void glGetTextureLevelParameteriv(GLuint texture, GLint level, GLenum pname, GLint* params);
    void glGetTextureLevelParameterivEXT(GLuint texture, GLenum target, GLint level, GLenum pname, GLint* params);
    void glGetTextureParameterIiv(GLuint texture, GLenum pname, GLint* params);
    void glGetTextureParameterIivEXT(GLuint texture, GLenum target, GLenum pname, GLint* params);
    void glGetTextureParameterIuiv(GLuint texture, GLenum pname, GLuint* params);
    void glGetTextureParameterIuivEXT(GLuint texture, GLenum target, GLenum pname, GLuint* params);
    void glGetTextureParameterfv(GLuint texture, GLenum pname, GLfloat* params);
    void glGetTextureParameterfvEXT(GLuint texture, GLenum target, GLenum pname, GLfloat* params);
    void glGetTextureParameteriv(GLuint texture, GLenum pname, GLint* params);
    void glGetTextureParameterivEXT(GLuint texture, GLenum target, GLenum pname, GLint* params);
    GLuint64 glGetTextureSamplerHandleARB(GLuint texture, GLuint sampler);
    GLuint64 glGetTextureSamplerHandleNV(GLuint texture, GLuint sampler);
    void glGetTextureSubImage(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, GLsizei bufSize, void* pixels);
    void glGetTrackMatrixivNV(GLenum target, GLuint address, GLenum pname, GLint* params);
    void glGetTransformFeedbackVarying(GLuint program, GLuint index, GLsizei bufSize, GLsizei* length, GLsizei* size, GLenum* type, GLchar* name);
    void glGetTransformFeedbackVaryingNV(GLuint program, GLuint index, GLint* location);
    void glGetTransformFeedbacki64_v(GLuint xfb, GLenum pname, GLuint index, GLint64* param);
    void glGetTransformFeedbacki_v(GLuint xfb, GLenum pname, GLuint index, GLint* param);
    void glGetTransformFeedbackiv(GLuint xfb, GLenum pname, GLint* param);
    GLuint glGetUniformBlockIndex(GLuint program, const GLchar* uniformBlockName);
    GLint glGetUniformBufferSizeEXT(GLuint program, GLint location);
    void glGetUniformIndices(GLuint program, GLsizei uniformCount, const GLchar* const* uniformNames, GLuint* uniformIndices);
    GLint glGetUniformLocation(GLuint program, const GLchar* name);
    GLintptr glGetUniformOffsetEXT(GLuint program, GLint location);
    void glGetUniformSubroutineuiv(GLenum shadertype, GLint location, GLuint* params);
    void glGetUniformdv(GLuint program, GLint location, GLdouble* params);
    void glGetUniformfv(GLuint program, GLint location, GLfloat* params);
    void glGetUniformi64vARB(GLuint program, GLint location, GLint64* params);
    void glGetUniformi64vNV(GLuint program, GLint location, GLint64EXT* params);
    void glGetUniformiv(GLuint program, GLint location, GLint* params);
    void glGetUniformui64vARB(GLuint program, GLint location, GLuint64* params);
    void glGetUniformui64vNV(GLuint program, GLint location, GLuint64EXT* params);
    void glGetUniformuiv(GLuint program, GLint location, GLuint* params);
    void glGetUnsignedBytei_vEXT(GLenum target, GLuint index, GLubyte* data);
    void glGetUnsignedBytevEXT(GLenum pname, GLubyte* data);
    void glGetVariantArrayObjectfvATI(GLuint id, GLenum pname, GLfloat* params);
    void glGetVariantArrayObjectivATI(GLuint id, GLenum pname, GLint* params);
    void glGetVariantBooleanvEXT(GLuint id, GLenum value, GLboolean* data);
    void glGetVariantFloatvEXT(GLuint id, GLenum value, GLfloat* data);
    void glGetVariantIntegervEXT(GLuint id, GLenum value, GLint* data);
    void glGetVariantPointervEXT(GLuint id, GLenum value, void** data);
    GLint glGetVaryingLocationNV(GLuint program, const GLchar* name);
    void glGetVertexArrayIndexed64iv(GLuint vaobj, GLuint index, GLenum pname, GLint64* param);
    void glGetVertexArrayIndexediv(GLuint vaobj, GLuint index, GLenum pname, GLint* param);
    void glGetVertexArrayIntegeri_vEXT(GLuint vaobj, GLuint index, GLenum pname, GLint* param);
    void glGetVertexArrayIntegervEXT(GLuint vaobj, GLenum pname, GLint* param);
    void glGetVertexArrayPointeri_vEXT(GLuint vaobj, GLuint index, GLenum pname, void** param);
    void glGetVertexArrayPointervEXT(GLuint vaobj, GLenum pname, void** param);
    void glGetVertexArrayiv(GLuint vaobj, GLenum pname, GLint* param);
    void glGetVertexAttribArrayObjectfvATI(GLuint index, GLenum pname, GLfloat* params);
    void glGetVertexAttribArrayObjectivATI(GLuint index, GLenum pname, GLint* params);
    void glGetVertexAttribIiv(GLuint index, GLenum pname, GLint* params);
    void glGetVertexAttribIuiv(GLuint index, GLenum pname, GLuint* params);
    void glGetVertexAttribLdv(GLuint index, GLenum pname, GLdouble* params);
    void glGetVertexAttribLdvEXT(GLuint index, GLenum pname, GLdouble* params);
    void glGetVertexAttribLi64vNV(GLuint index, GLenum pname, GLint64EXT* params);
    void glGetVertexAttribLui64vARB(GLuint index, GLenum pname, GLuint64EXT* params);
    void glGetVertexAttribLui64vNV(GLuint index, GLenum pname, GLuint64EXT* params);
    void glGetVertexAttribPointerv(GLuint index, GLenum pname, void** pointer);
    void glGetVertexAttribPointervNV(GLuint index, GLenum pname, void** pointer);
    void glGetVertexAttribdv(GLuint index, GLenum pname, GLdouble* params);
    void glGetVertexAttribdvARB(GLuint index, GLenum pname, GLdouble* params);
    void glGetVertexAttribdvNV(GLuint index, GLenum pname, GLdouble* params);
    void glGetVertexAttribfv(GLuint index, GLenum pname, GLfloat* params);
    void glGetVertexAttribfvNV(GLuint index, GLenum pname, GLfloat* params);
    void glGetVertexAttribiv(GLuint index, GLenum pname, GLint* params);
    void glGetVertexAttribivNV(GLuint index, GLenum pname, GLint* params);
    void glGetVideoCaptureStreamdvNV(GLuint video_capture_slot, GLuint stream, GLenum pname, GLdouble* params);
    void glGetVideoCaptureStreamfvNV(GLuint video_capture_slot, GLuint stream, GLenum pname, GLfloat* params);
    void glGetVideoCaptureStreamivNV(GLuint video_capture_slot, GLuint stream, GLenum pname, GLint* params);
    void glGetVideoCaptureivNV(GLuint video_capture_slot, GLenum pname, GLint* params);
    void glGetVideoi64vNV(GLuint video_slot, GLenum pname, GLint64EXT* params);
    void glGetVideoivNV(GLuint video_slot, GLenum pname, GLint* params);
    void glGetVideoui64vNV(GLuint video_slot, GLenum pname, GLuint64EXT* params);
    void glGetVideouivNV(GLuint video_slot, GLenum pname, GLuint* params);
    GLVULKANPROCNV glGetVkProcAddrNV(const GLchar* name);
    void glGetnColorTable(GLenum target, GLenum format, GLenum type, GLsizei bufSize, void* table);
    void glGetnColorTableARB(GLenum target, GLenum format, GLenum type, GLsizei bufSize, void* table);
    void glGetnCompressedTexImage(GLenum target, GLint lod, GLsizei bufSize, void* pixels);
    void glGetnCompressedTexImageARB(GLenum target, GLint lod, GLsizei bufSize, void* img);
    void glGetnConvolutionFilter(GLenum target, GLenum format, GLenum type, GLsizei bufSize, void* image);
    void glGetnConvolutionFilterARB(GLenum target, GLenum format, GLenum type, GLsizei bufSize, void* image);
    void glGetnHistogram(GLenum target, GLboolean reset, GLenum format, GLenum type, GLsizei bufSize, void* values);
    void glGetnHistogramARB(GLenum target, GLboolean reset, GLenum format, GLenum type, GLsizei bufSize, void* values);
    void glGetnMapdv(GLenum target, GLenum query, GLsizei bufSize, GLdouble* v);
    void glGetnMapdvARB(GLenum target, GLenum query, GLsizei bufSize, GLdouble* v);
    void glGetnMapfv(GLenum target, GLenum query, GLsizei bufSize, GLfloat* v);
    void glGetnMapfvARB(GLenum target, GLenum query, GLsizei bufSize, GLfloat* v);
    void glGetnMapiv(GLenum target, GLenum query, GLsizei bufSize, GLint* v);
    void glGetnMapivARB(GLenum target, GLenum query, GLsizei bufSize, GLint* v);
    void glGetnMinmax(GLenum target, GLboolean reset, GLenum format, GLenum type, GLsizei bufSize, void* values);
    void glGetnMinmaxARB(GLenum target, GLboolean reset, GLenum format, GLenum type, GLsizei bufSize, void* values);
    void glGetnPixelMapfv(GLenum map, GLsizei bufSize, GLfloat* values);
    void glGetnPixelMapfvARB(GLenum map, GLsizei bufSize, GLfloat* values);
    void glGetnPixelMapuiv(GLenum map, GLsizei bufSize, GLuint* values);
    void glGetnPixelMapuivARB(GLenum map, GLsizei bufSize, GLuint* values);
    void glGetnPixelMapusv(GLenum map, GLsizei bufSize, GLushort* values);
    void glGetnPixelMapusvARB(GLenum map, GLsizei bufSize, GLushort* values);
    void glGetnPolygonStipple(GLsizei bufSize, GLubyte* pattern);
    void glGetnPolygonStippleARB(GLsizei bufSize, GLubyte* pattern);
    void glGetnSeparableFilter(GLenum target, GLenum format, GLenum type, GLsizei rowBufSize, void* row, GLsizei columnBufSize, void* column, void* span);
    void glGetnSeparableFilterARB(GLenum target, GLenum format, GLenum type, GLsizei rowBufSize, void* row, GLsizei columnBufSize, void* column, void* span);
    void glGetnTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLsizei bufSize, void* pixels);
    void glGetnTexImageARB(GLenum target, GLint level, GLenum format, GLenum type, GLsizei bufSize, void* img);
    void glGetnUniformdv(GLuint program, GLint location, GLsizei bufSize, GLdouble* params);
    void glGetnUniformdvARB(GLuint program, GLint location, GLsizei bufSize, GLdouble* params);
    void glGetnUniformfv(GLuint program, GLint location, GLsizei bufSize, GLfloat* params);
    void glGetnUniformi64vARB(GLuint program, GLint location, GLsizei bufSize, GLint64* params);
    void glGetnUniformiv(GLuint program, GLint location, GLsizei bufSize, GLint* params);
    void glGetnUniformui64vARB(GLuint program, GLint location, GLsizei bufSize, GLuint64* params);
    void glGetnUniformuiv(GLuint program, GLint location, GLsizei bufSize, GLuint* params);
    void glGlobalAlphaFactorbSUN(GLbyte factor);
    void glGlobalAlphaFactordSUN(GLdouble factor);
    void glGlobalAlphaFactorfSUN(GLfloat factor);
    void glGlobalAlphaFactoriSUN(GLint factor);
    void glGlobalAlphaFactorsSUN(GLshort factor);
    void glGlobalAlphaFactorubSUN(GLubyte factor);
    void glGlobalAlphaFactoruiSUN(GLuint factor);
    void glGlobalAlphaFactorusSUN(GLushort factor);
    void glHint(GLenum target, GLenum mode);
    void glHintPGI(GLenum target, GLint mode);
    void glHistogram(GLenum target, GLsizei width, GLenum internalformat, GLboolean sink);
    void glHistogramEXT(GLenum target, GLsizei width, GLenum internalformat, GLboolean sink);
    void glImageTransformParameterfHP(GLenum target, GLenum pname, GLfloat param);
    void glImageTransformParameterfvHP(GLenum target, GLenum pname, const GLfloat* params);
    void glImageTransformParameteriHP(GLenum target, GLenum pname, GLint param);
    void glImageTransformParameterivHP(GLenum target, GLenum pname, const GLint* params);
    void glImportMemoryFdEXT(GLuint memory, GLuint64 size, GLenum handleType, GLint fd);
    void glImportMemoryWin32HandleEXT(GLuint memory, GLuint64 size, GLenum handleType, void* handle);
    void glImportMemoryWin32NameEXT(GLuint memory, GLuint64 size, GLenum handleType, const void* name);
    void glImportSemaphoreFdEXT(GLuint semaphore, GLenum handleType, GLint fd);
    void glImportSemaphoreWin32HandleEXT(GLuint semaphore, GLenum handleType, void* handle);
    void glImportSemaphoreWin32NameEXT(GLuint semaphore, GLenum handleType, const void* name);
    GLsync glImportSyncEXT(GLenum external_sync_type, GLintptr external_sync, GLbitfield flags);
    void glIndexFormatNV(GLenum type, GLsizei stride);
    void glIndexFuncEXT(GLenum func, GLclampf ref);
    void glIndexMask(GLuint mask);
    void glIndexMaterialEXT(GLenum face, GLenum mode);
    void glIndexPointer(GLenum type, GLsizei stride, const GLvoid* ptr);
    void glIndexPointerEXT(GLenum type, GLsizei stride, GLsizei count, const void* pointer);
    void glIndexPointerListIBM(GLenum type, GLint stride, const void** pointer, GLint ptrstride);
    void glIndexd(GLdouble c);
    void glIndexdv(const GLdouble* c);
    void glIndexf(GLfloat c);
    void glIndexfv(const GLfloat* c);
    void glIndexi(GLint c);
    void glIndexiv(const GLint* c);
    void glIndexs(GLshort c);
    void glIndexsv(const GLshort* c);
    void glIndexub(GLubyte c);
    void glIndexubv(const GLubyte* c);
    void glIndexxOES(GLfixed component);
    void glIndexxvOES(const GLfixed* component);
    void glInsertComponentEXT(GLuint res, GLuint src, GLuint num);
    void glInsertEventMarkerEXT(GLsizei length, const GLchar* marker);
    void glInstrumentsBufferSGIX(GLsizei size, GLint* buffer);
    void glInterleavedArrays(GLenum format, GLsizei stride, const GLvoid* pointer);
    void glInterpolatePathsNV(GLuint resultPath, GLuint pathA, GLuint pathB, GLfloat weight);
    void glInvalidateBufferData(GLuint buffer);
    void glInvalidateBufferSubData(GLuint buffer, GLintptr offset, GLsizeiptr length);
    void glInvalidateFramebuffer(GLenum target, GLsizei numAttachments, const GLenum* attachments);
    void glInvalidateNamedFramebufferData(GLuint framebuffer, GLsizei numAttachments, const GLenum* attachments);
    void glInvalidateNamedFramebufferSubData(GLuint framebuffer, GLsizei numAttachments, const GLenum* attachments, GLint x, GLint y, GLsizei width, GLsizei height);
    void glInvalidateSubFramebuffer(GLenum target, GLsizei numAttachments, const GLenum* attachments, GLint x, GLint y, GLsizei width, GLsizei height);
    void glInvalidateTexImage(GLuint texture, GLint level);
    void glInvalidateTexSubImage(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth);
    void glIooInterfaceSGIX(GLenum pname, const void* params);
    GLboolean glIsAsyncMarkerSGIX(GLuint marker);
    GLboolean glIsBuffer(GLuint buffer);
    GLboolean glIsBufferResidentNV(GLenum target);
    GLboolean glIsCommandListNV(GLuint list);
    GLboolean glIsEnabled(GLenum cap);
    GLboolean glIsEnabledIndexedEXT(GLenum target, GLuint index);
    GLboolean glIsEnabledi(GLenum target, GLuint index);
    GLboolean glIsFenceAPPLE(GLuint fence);
    GLboolean glIsFenceNV(GLuint fence);
    GLboolean glIsFramebuffer(GLuint framebuffer);
    GLboolean glIsImageHandleResidentARB(GLuint64 handle);
    GLboolean glIsImageHandleResidentNV(GLuint64 handle);
    GLboolean glIsList(GLuint list);
    GLboolean glIsMemoryObjectEXT(GLuint memoryObject);
    GLboolean glIsNameAMD(GLenum identifier, GLuint name);
    GLboolean glIsNamedBufferResidentNV(GLuint buffer);
    GLboolean glIsNamedStringARB(GLint namelen, const GLchar* name);
    GLboolean glIsObjectBufferATI(GLuint buffer);
    GLboolean glIsOcclusionQueryNV(GLuint id);
    GLboolean glIsPathNV(GLuint path);
    GLboolean glIsPointInFillPathNV(GLuint path, GLuint mask, GLfloat x, GLfloat y);
    GLboolean glIsPointInStrokePathNV(GLuint path, GLfloat x, GLfloat y);
    GLboolean glIsProgram(GLuint program);
    GLboolean glIsProgramNV(GLuint id);
    GLboolean glIsProgramPipeline(GLuint pipeline);
    GLboolean glIsQuery(GLuint id);
    GLboolean glIsRenderbuffer(GLuint renderbuffer);
    GLboolean glIsSampler(GLuint sampler);
    GLboolean glIsSemaphoreEXT(GLuint semaphore);
    GLboolean glIsShader(GLuint shader);
    GLboolean glIsStateNV(GLuint state);
    GLboolean glIsSync(GLsync sync);
    GLboolean glIsTexture(GLuint texture);
    GLboolean glIsTextureHandleResidentARB(GLuint64 handle);
    GLboolean glIsTextureHandleResidentNV(GLuint64 handle);
    GLboolean glIsTransformFeedback(GLuint id);
    GLboolean glIsVariantEnabledEXT(GLuint id, GLenum cap);
    GLboolean glIsVertexArray(GLuint array);
    GLboolean glIsVertexArrayAPPLE(GLuint array);
    GLboolean glIsVertexAttribEnabledAPPLE(GLuint index, GLenum pname);
    void glLGPUCopyImageSubDataNVX(GLuint sourceGpu, GLbitfield destinationGpuMask, GLuint srcName, GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srxY, GLint srcZ, GLuint dstName, GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ, GLsizei width, GLsizei height, GLsizei depth);
    void glLGPUInterlockNVX(void);
    void glLGPUNamedBufferSubDataNVX(GLbitfield gpuMask, GLuint buffer, GLintptr offset, GLsizeiptr size, const void* data);
    void glLabelObjectEXT(GLenum type, GLuint object, GLsizei length, const GLchar* label);
    void glLightEnviSGIX(GLenum pname, GLint param);
    void glLightModelf(GLenum pname, GLfloat param);
    void glLightModelfv(GLenum pname, const GLfloat* params);
    void glLightModeli(GLenum pname, GLint param);
    void glLightModeliv(GLenum pname, const GLint* params);
    void glLightModelxOES(GLenum pname, GLfixed param);
    void glLightModelxvOES(GLenum pname, const GLfixed* param);
    void glLightf(GLenum light, GLenum pname, GLfloat param);
    void glLightfv(GLenum light, GLenum pname, const GLfloat* params);
    void glLighti(GLenum light, GLenum pname, GLint param);
    void glLightiv(GLenum light, GLenum pname, const GLint* params);
    void glLightxOES(GLenum light, GLenum pname, GLfixed param);
    void glLightxvOES(GLenum light, GLenum pname, const GLfixed* params);
    void glLineStipple(GLint factor, GLushort pattern);
    void glLineWidth(GLfloat width);
    void glLineWidthxOES(GLfixed width);
    void glLinkProgram(GLuint program);
    void glListBase(GLuint base);
    void glListDrawCommandsStatesClientNV(GLuint list, GLuint segment, const void** indirects, const GLsizei* sizes, const GLuint* states, const GLuint* fbos, GLuint count);
    void glListParameterfSGIX(GLuint list, GLenum pname, GLfloat param);
    void glListParameterfvSGIX(GLuint list, GLenum pname, const GLfloat* params);
    void glListParameteriSGIX(GLuint list, GLenum pname, GLint param);
    void glListParameterivSGIX(GLuint list, GLenum pname, const GLint* params);
    void glLoadIdentityDeformationMapSGIX(GLbitfield mask);
    void glLoadMatrixd(const GLdouble* m);
    void glLoadMatrixf(const GLfloat* m);
    void glLoadMatrixxOES(const GLfixed* m);
    void glLoadName(GLuint name);
    void glLoadProgramNV(GLenum target, GLuint id, GLsizei len, const GLubyte* program);
    void glLoadTransposeMatrixd(const GLdouble* m);
    void glLoadTransposeMatrixdARB(const GLdouble* m);
    void glLoadTransposeMatrixf(const GLfloat* m);
    void glLoadTransposeMatrixfARB(const GLfloat* m);
    void glLoadTransposeMatrixxOES(const GLfixed* m);
    void glLockArraysEXT(GLint first, GLsizei count);
    void glLogicOp(GLenum opcode);
    void glMakeBufferNonResidentNV(GLenum target);
    void glMakeBufferResidentNV(GLenum target, GLenum access);
    void glMakeImageHandleNonResidentARB(GLuint64 handle);
    void glMakeImageHandleNonResidentNV(GLuint64 handle);
    void glMakeImageHandleResidentARB(GLuint64 handle, GLenum access);
    void glMakeImageHandleResidentNV(GLuint64 handle, GLenum access);
    void glMakeNamedBufferNonResidentNV(GLuint buffer);
    void glMakeNamedBufferResidentNV(GLuint buffer, GLenum access);
    void glMakeTextureHandleNonResidentARB(GLuint64 handle);
    void glMakeTextureHandleNonResidentNV(GLuint64 handle);
    void glMakeTextureHandleResidentARB(GLuint64 handle);
    void glMakeTextureHandleResidentNV(GLuint64 handle);
    void glMap1d(GLenum target, GLdouble u1, GLdouble u2, GLint stride, GLint order, const GLdouble* points);
    void glMap1f(GLenum target, GLfloat u1, GLfloat u2, GLint stride, GLint order, const GLfloat* points);
    void glMap1xOES(GLenum target, GLfixed u1, GLfixed u2, GLint stride, GLint order, GLfixed points);
    void glMap2d(GLenum target, GLdouble u1, GLdouble u2, GLint ustride, GLint uorder, GLdouble v1, GLdouble v2, GLint vstride, GLint vorder, const GLdouble* points);
    void glMap2f(GLenum target, GLfloat u1, GLfloat u2, GLint ustride, GLint uorder, GLfloat v1, GLfloat v2, GLint vstride, GLint vorder, const GLfloat* points);
    void glMap2xOES(GLenum target, GLfixed u1, GLfixed u2, GLint ustride, GLint uorder, GLfixed v1, GLfixed v2, GLint vstride, GLint vorder, GLfixed points);
    void* glMapBuffer(GLenum target, GLenum access);
    void* glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access);
    void glMapControlPointsNV(GLenum target, GLuint index, GLenum type, GLsizei ustride, GLsizei vstride, GLint uorder, GLint vorder, GLboolean packed, const void* points);
    void glMapGrid1d(GLint un, GLdouble u1, GLdouble u2);
    void glMapGrid1f(GLint un, GLfloat u1, GLfloat u2);
    void glMapGrid1xOES(GLint n, GLfixed u1, GLfixed u2);
    void glMapGrid2d(GLint un, GLdouble u1, GLdouble u2, GLint vn, GLdouble v1, GLdouble v2);
    void glMapGrid2f(GLint un, GLfloat u1, GLfloat u2, GLint vn, GLfloat v1, GLfloat v2);
    void glMapGrid2xOES(GLint n, GLfixed u1, GLfixed u2, GLfixed v1, GLfixed v2);
    void glMapParameterfvNV(GLenum target, GLenum pname, const GLfloat* params);
    void glMapParameterivNV(GLenum target, GLenum pname, const GLint* params);
    void glMapVertexAttrib1dAPPLE(GLuint index, GLuint size, GLdouble u1, GLdouble u2, GLint stride, GLint order, const GLdouble* points);
    void glMapVertexAttrib1fAPPLE(GLuint index, GLuint size, GLfloat u1, GLfloat u2, GLint stride, GLint order, const GLfloat* points);
    void glMapVertexAttrib2dAPPLE(GLuint index, GLuint size, GLdouble u1, GLdouble u2, GLint ustride, GLint uorder, GLdouble v1, GLdouble v2, GLint vstride, GLint vorder, const GLdouble* points);
    void glMapVertexAttrib2fAPPLE(GLuint index, GLuint size, GLfloat u1, GLfloat u2, GLint ustride, GLint uorder, GLfloat v1, GLfloat v2, GLint vstride, GLint vorder, const GLfloat* points);
    void glMaterialf(GLenum face, GLenum pname, GLfloat param);
    void glMaterialfv(GLenum face, GLenum pname, const GLfloat* params);
    void glMateriali(GLenum face, GLenum pname, GLint param);
    void glMaterialiv(GLenum face, GLenum pname, const GLint* params);
    void glMaterialxOES(GLenum face, GLenum pname, GLfixed param);
    void glMaterialxvOES(GLenum face, GLenum pname, const GLfixed* param);
    void glMatrixFrustumEXT(GLenum mode, GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble zNear, GLdouble zFar);
    void glMatrixIndexPointerARB(GLint size, GLenum type, GLsizei stride, const void* pointer);
    void glMatrixIndexubvARB(GLint size, const GLubyte* indices);
    void glMatrixIndexuivARB(GLint size, const GLuint* indices);
    void glMatrixIndexusvARB(GLint size, const GLushort* indices);
    void glMatrixLoad3x2fNV(GLenum matrixMode, const GLfloat* m);
    void glMatrixLoad3x3fNV(GLenum matrixMode, const GLfloat* m);
    void glMatrixLoadIdentityEXT(GLenum mode);
    void glMatrixLoadTranspose3x3fNV(GLenum matrixMode, const GLfloat* m);
    void glMatrixLoadTransposedEXT(GLenum mode, const GLdouble* m);
    void glMatrixLoadTransposefEXT(GLenum mode, const GLfloat* m);
    void glMatrixLoaddEXT(GLenum mode, const GLdouble* m);
    void glMatrixLoadfEXT(GLenum mode, const GLfloat* m);
    void glMatrixMode(GLenum mode);
    void glMatrixMult3x2fNV(GLenum matrixMode, const GLfloat* m);
    void glMatrixMult3x3fNV(GLenum matrixMode, const GLfloat* m);
    void glMatrixMultTranspose3x3fNV(GLenum matrixMode, const GLfloat* m);
    void glMatrixMultTransposedEXT(GLenum mode, const GLdouble* m);
    void glMatrixMultTransposefEXT(GLenum mode, const GLfloat* m);
    void glMatrixMultdEXT(GLenum mode, const GLdouble* m);
    void glMatrixMultfEXT(GLenum mode, const GLfloat* m);
    void glMatrixOrthoEXT(GLenum mode, GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble zNear, GLdouble zFar);
    void glMatrixPopEXT(GLenum mode);
    void glMatrixPushEXT(GLenum mode);
    void glMatrixRotatedEXT(GLenum mode, GLdouble ane, GLdouble x, GLdouble y, GLdouble z);
    void glMatrixRotatefEXT(GLenum mode, GLfloat ane, GLfloat x, GLfloat y, GLfloat z);
    void glMatrixScaledEXT(GLenum mode, GLdouble x, GLdouble y, GLdouble z);
    void glMatrixScalefEXT(GLenum mode, GLfloat x, GLfloat y, GLfloat z);
    void glMatrixTranslatedEXT(GLenum mode, GLdouble x, GLdouble y, GLdouble z);
    void glMatrixTranslatefEXT(GLenum mode, GLfloat x, GLfloat y, GLfloat z);
    void glMaxShaderCompilerThreadsARB(GLuint count);
    void glMaxShaderCompilerThreadsKHR(GLuint count);
    void glMemoryBarrier(GLbitfield barriers);
    void glMemoryBarrierByRegion(GLbitfield barriers);
    void glMemoryObjectParameterivEXT(GLuint memoryObject, GLenum pname, const GLint* params);
    void glMinSampleShading(GLfloat value);
    void glMinmax(GLenum target, GLenum internalformat, GLboolean sink);
    void glMinmaxEXT(GLenum target, GLenum internalformat, GLboolean sink);
    void glMultMatrixd(const GLdouble* m);
    void glMultMatrixf(const GLfloat* m);
    void glMultMatrixxOES(const GLfixed* m);
    void glMultTransposeMatrixd(const GLdouble* m);
    void glMultTransposeMatrixdARB(const GLdouble* m);
    void glMultTransposeMatrixf(const GLfloat* m);
    void glMultTransposeMatrixfARB(const GLfloat* m);
    void glMultTransposeMatrixxOES(const GLfixed* m);
    void glMultiDrawArrays(GLenum mode, const GLint* first, const GLsizei* count, GLsizei drawcount);
    void glMultiDrawArraysEXT(GLenum mode, const GLint* first, const GLsizei* count, GLsizei primcount);
    void glMultiDrawArraysIndirect(GLenum mode, const void* indirect, GLsizei drawcount, GLsizei stride);
    void glMultiDrawArraysIndirectAMD(GLenum mode, const void* indirect, GLsizei primcount, GLsizei stride);
    void glMultiDrawArraysIndirectBindlessCountNV(GLenum mode, const void* indirect, GLsizei drawCount, GLsizei maxDrawCount, GLsizei stride, GLint vertexBufferCount);
    void glMultiDrawArraysIndirectBindlessNV(GLenum mode, const void* indirect, GLsizei drawCount, GLsizei stride, GLint vertexBufferCount);
    void glMultiDrawArraysIndirectCount(GLenum mode, const void* indirect, GLintptr drawcount, GLsizei maxdrawcount, GLsizei stride);
    void glMultiDrawArraysIndirectCountARB(GLenum mode, const void* indirect, GLintptr drawcount, GLsizei maxdrawcount, GLsizei stride);
    void glMultiDrawElementArrayAPPLE(GLenum mode, const GLint* first, const GLsizei* count, GLsizei primcount);
    void glMultiDrawElements(GLenum mode, const GLsizei* count, GLenum type, const void* const* indices, GLsizei drawcount);
    void glMultiDrawElementsBaseVertex(GLenum mode, const GLsizei* count, GLenum type, const void* const* indices, GLsizei drawcount, const GLint* basevertex);
    void glMultiDrawElementsEXT(GLenum mode, const GLsizei* count, GLenum type, const void* const* indices, GLsizei primcount);
    void glMultiDrawElementsIndirect(GLenum mode, GLenum type, const void* indirect, GLsizei drawcount, GLsizei stride);
    void glMultiDrawElementsIndirectAMD(GLenum mode, GLenum type, const void* indirect, GLsizei primcount, GLsizei stride);
    void glMultiDrawElementsIndirectBindlessCountNV(GLenum mode, GLenum type, const void* indirect, GLsizei drawCount, GLsizei maxDrawCount, GLsizei stride, GLint vertexBufferCount);
    void glMultiDrawElementsIndirectBindlessNV(GLenum mode, GLenum type, const void* indirect, GLsizei drawCount, GLsizei stride, GLint vertexBufferCount);
    void glMultiDrawElementsIndirectCount(GLenum mode, GLenum type, const void* indirect, GLintptr drawcount, GLsizei maxdrawcount, GLsizei stride);
    void glMultiDrawElementsIndirectCountARB(GLenum mode, GLenum type, const void* indirect, GLintptr drawcount, GLsizei maxdrawcount, GLsizei stride);
    void glMultiDrawMeshTasksIndirectCountNV(GLintptr indirect, GLintptr drawcount, GLsizei maxdrawcount, GLsizei stride);
    void glMultiDrawMeshTasksIndirectNV(GLintptr indirect, GLsizei drawcount, GLsizei stride);
    void glMultiDrawRangeElementArrayAPPLE(GLenum mode, GLuint start, GLuint end, const GLint* first, const GLsizei* count, GLsizei primcount);
    void glMultiModeDrawArraysIBM(const GLenum* mode, const GLint* first, const GLsizei* count, GLsizei primcount, GLint modestride);
    void glMultiModeDrawElementsIBM(const GLenum* mode, const GLsizei* count, GLenum type, const void* const* indices, GLsizei primcount, GLint modestride);
    void glMultiTexBufferEXT(GLenum texunit, GLenum target, GLenum internalformat, GLuint buffer);
    void glMultiTexCoord1bOES(GLenum texture, GLbyte s);
    void glMultiTexCoord1bvOES(GLenum texture, const GLbyte* coords);
    void glMultiTexCoord1d(GLenum target, GLdouble s);
    void glMultiTexCoord1dARB(GLenum target, GLdouble s);
    void glMultiTexCoord1dv(GLenum target, const GLdouble* v);
    void glMultiTexCoord1dvARB(GLenum target, const GLdouble* v);
    void glMultiTexCoord1f(GLenum target, GLfloat s);
    void glMultiTexCoord1fARB(GLenum target, GLfloat s);
    void glMultiTexCoord1fv(GLenum target, const GLfloat* v);
    void glMultiTexCoord1fvARB(GLenum target, const GLfloat* v);
    void glMultiTexCoord1hNV(GLenum target, GLhalfNV s);
    void glMultiTexCoord1hvNV(GLenum target, const GLhalfNV* v);
    void glMultiTexCoord1i(GLenum target, GLint s);
    void glMultiTexCoord1iARB(GLenum target, GLint s);
    void glMultiTexCoord1iv(GLenum target, const GLint* v);
    void glMultiTexCoord1ivARB(GLenum target, const GLint* v);
    void glMultiTexCoord1s(GLenum target, GLshort s);
    void glMultiTexCoord1sARB(GLenum target, GLshort s);
    void glMultiTexCoord1sv(GLenum target, const GLshort* v);
    void glMultiTexCoord1svARB(GLenum target, const GLshort* v);
    void glMultiTexCoord1xOES(GLenum texture, GLfixed s);
    void glMultiTexCoord1xvOES(GLenum texture, const GLfixed* coords);
    void glMultiTexCoord2bOES(GLenum texture, GLbyte s, GLbyte t);
    void glMultiTexCoord2bvOES(GLenum texture, const GLbyte* coords);
    void glMultiTexCoord2d(GLenum target, GLdouble s, GLdouble t);
    void glMultiTexCoord2dARB(GLenum target, GLdouble s, GLdouble t);
    void glMultiTexCoord2dv(GLenum target, const GLdouble* v);
    void glMultiTexCoord2dvARB(GLenum target, const GLdouble* v);
    void glMultiTexCoord2f(GLenum target, GLfloat s, GLfloat t);
    void glMultiTexCoord2fARB(GLenum target, GLfloat s, GLfloat t);
    void glMultiTexCoord2fv(GLenum target, const GLfloat* v);
    void glMultiTexCoord2fvARB(GLenum target, const GLfloat* v);
    void glMultiTexCoord2hNV(GLenum target, GLhalfNV s, GLhalfNV t);
    void glMultiTexCoord2hvNV(GLenum target, const GLhalfNV* v);
    void glMultiTexCoord2i(GLenum target, GLint s, GLint t);
    void glMultiTexCoord2iARB(GLenum target, GLint s, GLint t);
    void glMultiTexCoord2iv(GLenum target, const GLint* v);
    void glMultiTexCoord2ivARB(GLenum target, const GLint* v);
    void glMultiTexCoord2s(GLenum target, GLshort s, GLshort t);
    void glMultiTexCoord2sARB(GLenum target, GLshort s, GLshort t);
    void glMultiTexCoord2sv(GLenum target, const GLshort* v);
    void glMultiTexCoord2svARB(GLenum target, const GLshort* v);
    void glMultiTexCoord2xOES(GLenum texture, GLfixed s, GLfixed t);
    void glMultiTexCoord2xvOES(GLenum texture, const GLfixed* coords);
    void glMultiTexCoord3bOES(GLenum texture, GLbyte s, GLbyte t, GLbyte r);
    void glMultiTexCoord3bvOES(GLenum texture, const GLbyte* coords);
    void glMultiTexCoord3d(GLenum target, GLdouble s, GLdouble t, GLdouble r);
    void glMultiTexCoord3dARB(GLenum target, GLdouble s, GLdouble t, GLdouble r);
    void glMultiTexCoord3dv(GLenum target, const GLdouble* v);
    void glMultiTexCoord3dvARB(GLenum target, const GLdouble* v);
    void glMultiTexCoord3f(GLenum target, GLfloat s, GLfloat t, GLfloat r);
    void glMultiTexCoord3fARB(GLenum target, GLfloat s, GLfloat t, GLfloat r);
    void glMultiTexCoord3fv(GLenum target, const GLfloat* v);
    void glMultiTexCoord3fvARB(GLenum target, const GLfloat* v);
    void glMultiTexCoord3hNV(GLenum target, GLhalfNV s, GLhalfNV t, GLhalfNV r);
    void glMultiTexCoord3hvNV(GLenum target, const GLhalfNV* v);
    void glMultiTexCoord3i(GLenum target, GLint s, GLint t, GLint r);
    void glMultiTexCoord3iARB(GLenum target, GLint s, GLint t, GLint r);
    void glMultiTexCoord3iv(GLenum target, const GLint* v);
    void glMultiTexCoord3ivARB(GLenum target, const GLint* v);
    void glMultiTexCoord3s(GLenum target, GLshort s, GLshort t, GLshort r);
    void glMultiTexCoord3sARB(GLenum target, GLshort s, GLshort t, GLshort r);
    void glMultiTexCoord3sv(GLenum target, const GLshort* v);
    void glMultiTexCoord3svARB(GLenum target, const GLshort* v);
    void glMultiTexCoord3xOES(GLenum texture, GLfixed s, GLfixed t, GLfixed r);
    void glMultiTexCoord3xvOES(GLenum texture, const GLfixed* coords);
    void glMultiTexCoord4bOES(GLenum texture, GLbyte s, GLbyte t, GLbyte r, GLbyte q);
    void glMultiTexCoord4bvOES(GLenum texture, const GLbyte* coords);
    void glMultiTexCoord4d(GLenum target, GLdouble s, GLdouble t, GLdouble r, GLdouble q);
    void glMultiTexCoord4dARB(GLenum target, GLdouble s, GLdouble t, GLdouble r, GLdouble q);
    void glMultiTexCoord4dv(GLenum target, const GLdouble* v);
    void glMultiTexCoord4dvARB(GLenum target, const GLdouble* v);
    void glMultiTexCoord4f(GLenum target, GLfloat s, GLfloat t, GLfloat r, GLfloat q);
    void glMultiTexCoord4fARB(GLenum target, GLfloat s, GLfloat t, GLfloat r, GLfloat q);
    void glMultiTexCoord4fv(GLenum target, const GLfloat* v);
    void glMultiTexCoord4fvARB(GLenum target, const GLfloat* v);
    void glMultiTexCoord4hNV(GLenum target, GLhalfNV s, GLhalfNV t, GLhalfNV r, GLhalfNV q);
    void glMultiTexCoord4hvNV(GLenum target, const GLhalfNV* v);
    void glMultiTexCoord4i(GLenum target, GLint s, GLint t, GLint r, GLint q);
    void glMultiTexCoord4iARB(GLenum target, GLint s, GLint t, GLint r, GLint q);
    void glMultiTexCoord4iv(GLenum target, const GLint* v);
    void glMultiTexCoord4ivARB(GLenum target, const GLint* v);
    void glMultiTexCoord4s(GLenum target, GLshort s, GLshort t, GLshort r, GLshort q);
    void glMultiTexCoord4sARB(GLenum target, GLshort s, GLshort t, GLshort r, GLshort q);
    void glMultiTexCoord4sv(GLenum target, const GLshort* v);
    void glMultiTexCoord4svARB(GLenum target, const GLshort* v);
    void glMultiTexCoord4xOES(GLenum texture, GLfixed s, GLfixed t, GLfixed r, GLfixed q);
    void glMultiTexCoord4xvOES(GLenum texture, const GLfixed* coords);
    void glMultiTexCoordP1ui(GLenum texture, GLenum type, GLuint coords);
    void glMultiTexCoordP1uiv(GLenum texture, GLenum type, const GLuint* coords);
    void glMultiTexCoordP2ui(GLenum texture, GLenum type, GLuint coords);
    void glMultiTexCoordP2uiv(GLenum texture, GLenum type, const GLuint* coords);
    void glMultiTexCoordP3ui(GLenum texture, GLenum type, GLuint coords);
    void glMultiTexCoordP3uiv(GLenum texture, GLenum type, const GLuint* coords);
    void glMultiTexCoordP4ui(GLenum texture, GLenum type, GLuint coords);
    void glMultiTexCoordP4uiv(GLenum texture, GLenum type, const GLuint* coords);
    void glMultiTexCoordPointerEXT(GLenum texunit, GLint size, GLenum type, GLsizei stride, const void* pointer);
    void glMultiTexEnvfEXT(GLenum texunit, GLenum target, GLenum pname, GLfloat param);
    void glMultiTexEnvfvEXT(GLenum texunit, GLenum target, GLenum pname, const GLfloat* params);
    void glMultiTexEnviEXT(GLenum texunit, GLenum target, GLenum pname, GLint param);
    void glMultiTexEnvivEXT(GLenum texunit, GLenum target, GLenum pname, const GLint* params);
    void glMultiTexGendEXT(GLenum texunit, GLenum coord, GLenum pname, GLdouble param);
    void glMultiTexGendvEXT(GLenum texunit, GLenum coord, GLenum pname, const GLdouble* params);
    void glMultiTexGenfEXT(GLenum texunit, GLenum coord, GLenum pname, GLfloat param);
    void glMultiTexGenfvEXT(GLenum texunit, GLenum coord, GLenum pname, const GLfloat* params);
    void glMultiTexGeniEXT(GLenum texunit, GLenum coord, GLenum pname, GLint param);
    void glMultiTexGenivEXT(GLenum texunit, GLenum coord, GLenum pname, const GLint* params);
    void glMultiTexImage1DEXT(GLenum texunit, GLenum target, GLint level, GLint internalformat, GLsizei width, GLint border, GLenum format, GLenum type, const void* pixels);
    void glMultiTexImage2DEXT(GLenum texunit, GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void* pixels);
    void glMultiTexImage3DEXT(GLenum texunit, GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type, const void* pixels);
    void glMultiTexParameterIivEXT(GLenum texunit, GLenum target, GLenum pname, const GLint* params);
    void glMultiTexParameterIuivEXT(GLenum texunit, GLenum target, GLenum pname, const GLuint* params);
    void glMultiTexParameterfEXT(GLenum texunit, GLenum target, GLenum pname, GLfloat param);
    void glMultiTexParameterfvEXT(GLenum texunit, GLenum target, GLenum pname, const GLfloat* params);
    void glMultiTexParameteriEXT(GLenum texunit, GLenum target, GLenum pname, GLint param);
    void glMultiTexParameterivEXT(GLenum texunit, GLenum target, GLenum pname, const GLint* params);
    void glMultiTexRenderbufferEXT(GLenum texunit, GLenum target, GLuint renderbuffer);
    void glMultiTexSubImage1DEXT(GLenum texunit, GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format, GLenum type, const void* pixels);
    void glMultiTexSubImage2DEXT(GLenum texunit, GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void* pixels);
    void glMultiTexSubImage3DEXT(GLenum texunit, GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, const void* pixels);
    void glMulticastBarrierNV(void);
    void glMulticastBlitFramebufferNV(GLuint srcGpu, GLuint dstGpu, GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter);
    void glMulticastBufferSubDataNV(GLbitfield gpuMask, GLuint buffer, GLintptr offset, GLsizeiptr size, const void* data);
    void glMulticastCopyBufferSubDataNV(GLuint readGpu, GLbitfield writeGpuMask, GLuint readBuffer, GLuint writeBuffer, GLintptr readOffset, GLintptr writeOffset, GLsizeiptr size);
    void glMulticastCopyImageSubDataNV(GLuint srcGpu, GLbitfield dstGpuMask, GLuint srcName, GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ, GLuint dstName, GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ, GLsizei srcWidth, GLsizei srcHeight, GLsizei srcDepth);
    void glMulticastFramebufferSampleLocationsfvNV(GLuint gpu, GLuint framebuffer, GLuint start, GLsizei count, const GLfloat* v);
    void glMulticastGetQueryObjecti64vNV(GLuint gpu, GLuint id, GLenum pname, GLint64* params);
    void glMulticastGetQueryObjectivNV(GLuint gpu, GLuint id, GLenum pname, GLint* params);
    void glMulticastGetQueryObjectui64vNV(GLuint gpu, GLuint id, GLenum pname, GLuint64* params);
    void glMulticastGetQueryObjectuivNV(GLuint gpu, GLuint id, GLenum pname, GLuint* params);
    void glMulticastScissorArrayvNVX(GLuint gpu, GLuint first, GLsizei count, const GLint* v);
    void glMulticastViewportArrayvNVX(GLuint gpu, GLuint first, GLsizei count, const GLfloat* v);
    void glMulticastViewportPositionWScaleNVX(GLuint gpu, GLuint index, GLfloat xcoeff, GLfloat ycoeff);
    void glMulticastWaitSyncNV(GLuint signalGpu, GLbitfield waitGpuMask);
    void glNamedBufferAttachMemoryNV(GLuint buffer, GLuint memory, GLuint64 offset);
    void glNamedBufferData(GLuint buffer, GLsizeiptr size, const void* data, GLenum usage);
    void glNamedBufferDataEXT(GLuint buffer, GLsizeiptr size, const void* data, GLenum usage);
    void glNamedBufferPageCommitmentARB(GLuint buffer, GLintptr offset, GLsizeiptr size, GLboolean commit);
    void glNamedBufferPageCommitmentEXT(GLuint buffer, GLintptr offset, GLsizeiptr size, GLboolean commit);
    void glNamedBufferPageCommitmentMemNV(GLuint buffer, GLintptr offset, GLsizeiptr size, GLuint memory, GLuint64 memOffset, GLboolean commit);
    void glNamedBufferStorage(GLuint buffer, GLsizeiptr size, const void* data, GLbitfield flags);
    void glNamedBufferStorageEXT(GLuint buffer, GLsizeiptr size, const void* data, GLbitfield flags);
    void glNamedBufferStorageExternalEXT(GLuint buffer, GLintptr offset, GLsizeiptr size, GLeglClientBufferEXT clientBuffer, GLbitfield flags);
    void glNamedBufferStorageMemEXT(GLuint buffer, GLsizeiptr size, GLuint memory, GLuint64 offset);
    void glNamedBufferSubData(GLuint buffer, GLintptr offset, GLsizeiptr size, const void* data);
    void glNamedBufferSubDataEXT(GLuint buffer, GLintptr offset, GLsizeiptr size, const void* data);
    void glNamedCopyBufferSubDataEXT(GLuint readBuffer, GLuint writeBuffer, GLintptr readOffset, GLintptr writeOffset, GLsizeiptr size);
    void glNamedFramebufferDrawBuffer(GLuint framebuffer, GLenum buf);
    void glNamedFramebufferDrawBuffers(GLuint framebuffer, GLsizei n, const GLenum* bufs);
    void glNamedFramebufferParameteri(GLuint framebuffer, GLenum pname, GLint param);
    void glNamedFramebufferParameteriEXT(GLuint framebuffer, GLenum pname, GLint param);
    void glNamedFramebufferReadBuffer(GLuint framebuffer, GLenum src);
    void glNamedFramebufferRenderbuffer(GLuint framebuffer, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer);
    void glNamedFramebufferRenderbufferEXT(GLuint framebuffer, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer);
    void glNamedFramebufferSampleLocationsfvARB(GLuint framebuffer, GLuint start, GLsizei count, const GLfloat* v);
    void glNamedFramebufferSampleLocationsfvNV(GLuint framebuffer, GLuint start, GLsizei count, const GLfloat* v);
    void glNamedFramebufferSamplePositionsfvAMD(GLuint framebuffer, GLuint numsamples, GLuint pixelindex, const GLfloat* values);
    void glNamedFramebufferTexture(GLuint framebuffer, GLenum attachment, GLuint texture, GLint level);
    void glNamedFramebufferTexture1DEXT(GLuint framebuffer, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
    void glNamedFramebufferTexture2DEXT(GLuint framebuffer, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
    void glNamedFramebufferTexture3DEXT(GLuint framebuffer, GLenum attachment, GLenum textarget, GLuint texture, GLint level, GLint zoffset);
    void glNamedFramebufferTextureEXT(GLuint framebuffer, GLenum attachment, GLuint texture, GLint level);
    void glNamedFramebufferTextureFaceEXT(GLuint framebuffer, GLenum attachment, GLuint texture, GLint level, GLenum face);
    void glNamedFramebufferTextureLayer(GLuint framebuffer, GLenum attachment, GLuint texture, GLint level, GLint layer);
    void glNamedFramebufferTextureLayerEXT(GLuint framebuffer, GLenum attachment, GLuint texture, GLint level, GLint layer);
    void glNamedProgramLocalParameter4dEXT(GLuint program, GLenum target, GLuint index, GLdouble x, GLdouble y, GLdouble z, GLdouble w);
    void glNamedProgramLocalParameter4dvEXT(GLuint program, GLenum target, GLuint index, const GLdouble* params);
    void glNamedProgramLocalParameter4fEXT(GLuint program, GLenum target, GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
    void glNamedProgramLocalParameter4fvEXT(GLuint program, GLenum target, GLuint index, const GLfloat* params);
    void glNamedProgramLocalParameterI4iEXT(GLuint program, GLenum target, GLuint index, GLint x, GLint y, GLint z, GLint w);
    void glNamedProgramLocalParameterI4ivEXT(GLuint program, GLenum target, GLuint index, const GLint* params);
    void glNamedProgramLocalParameterI4uiEXT(GLuint program, GLenum target, GLuint index, GLuint x, GLuint y, GLuint z, GLuint w);
    void glNamedProgramLocalParameterI4uivEXT(GLuint program, GLenum target, GLuint index, const GLuint* params);
    void glNamedProgramLocalParameters4fvEXT(GLuint program, GLenum target, GLuint index, GLsizei count, const GLfloat* params);
    void glNamedProgramLocalParametersI4ivEXT(GLuint program, GLenum target, GLuint index, GLsizei count, const GLint* params);
    void glNamedProgramLocalParametersI4uivEXT(GLuint program, GLenum target, GLuint index, GLsizei count, const GLuint* params);
    void glNamedProgramStringEXT(GLuint program, GLenum target, GLenum format, GLsizei len, const void* string);
    void glNamedRenderbufferStorage(GLuint renderbuffer, GLenum internalformat, GLsizei width, GLsizei height);
    void glNamedRenderbufferStorageEXT(GLuint renderbuffer, GLenum internalformat, GLsizei width, GLsizei height);
    void glNamedRenderbufferStorageMultisample(GLuint renderbuffer, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height);
    void glNamedRenderbufferStorageMultisampleAdvancedAMD(GLuint renderbuffer, GLsizei samples, GLsizei storageSamples, GLenum internalformat, GLsizei width, GLsizei height);
    void glNamedRenderbufferStorageMultisampleCoverageEXT(GLuint renderbuffer, GLsizei coverageSamples, GLsizei colorSamples, GLenum internalformat, GLsizei width, GLsizei height);
    void glNamedRenderbufferStorageMultisampleEXT(GLuint renderbuffer, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height);
    void glNamedStringARB(GLenum type, GLint namelen, const GLchar* name, GLint strinen, const GLchar* string);
    void glNewList(GLuint list, GLenum mode);
    GLuint glNewObjectBufferATI(GLsizei size, const void* pointer, GLenum usage);
    void glNormal3b(GLbyte nx, GLbyte ny, GLbyte nz);
    void glNormal3bv(const GLbyte* v);
    void glNormal3d(GLdouble nx, GLdouble ny, GLdouble nz);
    void glNormal3dv(const GLdouble* v);
    void glNormal3f(GLfloat nx, GLfloat ny, GLfloat nz);
    void glNormal3fVertex3fSUN(GLfloat nx, GLfloat ny, GLfloat nz, GLfloat x, GLfloat y, GLfloat z);
    void glNormal3fVertex3fvSUN(const GLfloat* n, const GLfloat* v);
    void glNormal3fv(const GLfloat* v);
    void glNormal3hNV(GLhalfNV nx, GLhalfNV ny, GLhalfNV nz);
    void glNormal3hvNV(const GLhalfNV* v);
    void glNormal3i(GLint nx, GLint ny, GLint nz);
    void glNormal3iv(const GLint* v);
    void glNormal3s(GLshort nx, GLshort ny, GLshort nz);
    void glNormal3sv(const GLshort* v);
    void glNormal3xOES(GLfixed nx, GLfixed ny, GLfixed nz);
    void glNormal3xvOES(const GLfixed* coords);
    void glNormalFormatNV(GLenum type, GLsizei stride);
    void glNormalP3ui(GLenum type, GLuint coords);
    void glNormalP3uiv(GLenum type, const GLuint* coords);
    void glNormalPointer(GLenum type, GLsizei stride, const GLvoid* ptr);
    void glNormalPointerEXT(GLenum type, GLsizei stride, GLsizei count, const void* pointer);
    void glNormalPointerListIBM(GLenum type, GLint stride, const void** pointer, GLint ptrstride);
    void glNormalPointervINTEL(GLenum type, const void** pointer);
    void glNormalStream3bATI(GLenum stream, GLbyte nx, GLbyte ny, GLbyte nz);
    void glNormalStream3bvATI(GLenum stream, const GLbyte* coords);
    void glNormalStream3dATI(GLenum stream, GLdouble nx, GLdouble ny, GLdouble nz);
    void glNormalStream3dvATI(GLenum stream, const GLdouble* coords);
    void glNormalStream3fATI(GLenum stream, GLfloat nx, GLfloat ny, GLfloat nz);
    void glNormalStream3fvATI(GLenum stream, const GLfloat* coords);
    void glNormalStream3iATI(GLenum stream, GLint nx, GLint ny, GLint nz);
    void glNormalStream3ivATI(GLenum stream, const GLint* coords);
    void glNormalStream3sATI(GLenum stream, GLshort nx, GLshort ny, GLshort nz);
    void glNormalStream3svATI(GLenum stream, const GLshort* coords);
    void glObjectLabel(GLenum identifier, GLuint name, GLsizei length, const GLchar* label);
    void glObjectPtrLabel(const void* ptr, GLsizei length, const GLchar* label);
    GLenum glObjectPurgeableAPPLE(GLenum objectType, GLuint name, GLenum option);
    GLenum glObjectUnpurgeableAPPLE(GLenum objectType, GLuint name, GLenum option);
    void glOrtho(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble near_val, GLdouble far_val);
    void glOrthofOES(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f);
    void glOrthoxOES(GLfixed l, GLfixed r, GLfixed b, GLfixed t, GLfixed n, GLfixed f);
    void glPNTrianesfATI(GLenum pname, GLfloat param);
    void glPNTrianesiATI(GLenum pname, GLint param);
    void glPassTexCoordATI(GLuint dst, GLuint coord, GLenum swizzle);
    void glPassThrough(GLfloat token);
    void glPassThroughxOES(GLfixed token);
    void glPatchParameterfv(GLenum pname, const GLfloat* values);
    void glPatchParameteri(GLenum pname, GLint value);
    void glPathColorGenNV(GLenum color, GLenum genMode, GLenum colorFormat, const GLfloat* coeffs);
    void glPathCommandsNV(GLuint path, GLsizei numCommands, const GLubyte* commands, GLsizei numCoords, GLenum coordType, const void* coords);
    void glPathCoordsNV(GLuint path, GLsizei numCoords, GLenum coordType, const void* coords);
    void glPathCoverDepthFuncNV(GLenum func);
    void glPathDashArrayNV(GLuint path, GLsizei dashCount, const GLfloat* dashArray);
    void glPathFogGenNV(GLenum genMode);
    GLenum glPathGlyphIndexArrayNV(GLuint firstPathName, GLenum fontTarget, const void* fontName, GLbitfield fontStyle, GLuint firstGlyphIndex, GLsizei numGlyphs, GLuint pathParameterTemplate, GLfloat emScale);
    GLenum glPathGlyphIndexRangeNV(GLenum fontTarget, const void* fontName, GLbitfield fontStyle, GLuint pathParameterTemplate, GLfloat emScale, GLuint* baseAndCount);
    void glPathGlyphRangeNV(GLuint firstPathName, GLenum fontTarget, const void* fontName, GLbitfield fontStyle, GLuint firstGlyph, GLsizei numGlyphs, GLenum handleMissingGlyphs, GLuint pathParameterTemplate, GLfloat emScale);
    void glPathGlyphsNV(GLuint firstPathName, GLenum fontTarget, const void* fontName, GLbitfield fontStyle, GLsizei numGlyphs, GLenum type, const void* charcodes, GLenum handleMissingGlyphs, GLuint pathParameterTemplate, GLfloat emScale);
    GLenum glPathMemoryGlyphIndexArrayNV(GLuint firstPathName, GLenum fontTarget, GLsizeiptr fontSize, const void* fontData, GLsizei faceIndex, GLuint firstGlyphIndex, GLsizei numGlyphs, GLuint pathParameterTemplate, GLfloat emScale);
    void glPathParameterfNV(GLuint path, GLenum pname, GLfloat value);
    void glPathParameterfvNV(GLuint path, GLenum pname, const GLfloat* value);
    void glPathParameteriNV(GLuint path, GLenum pname, GLint value);
    void glPathParameterivNV(GLuint path, GLenum pname, const GLint* value);
    void glPathStencilDepthOffsetNV(GLfloat factor, GLfloat units);
    void glPathStencilFuncNV(GLenum func, GLint ref, GLuint mask);
    void glPathStringNV(GLuint path, GLenum format, GLsizei length, const void* pathString);
    void glPathSubCommandsNV(GLuint path, GLsizei commandStart, GLsizei commandsToDelete, GLsizei numCommands, const GLubyte* commands, GLsizei numCoords, GLenum coordType, const void* coords);
    void glPathSubCoordsNV(GLuint path, GLsizei coordStart, GLsizei numCoords, GLenum coordType, const void* coords);
    void glPathTexGenNV(GLenum texCoordSet, GLenum genMode, GLint components, const GLfloat* coeffs);
    void glPauseTransformFeedbackNV(void);
    void glPixelDataRangeNV(GLenum target, GLsizei length, const void* pointer);
    void glPixelMapfv(GLenum map, GLsizei mapsize, const GLfloat* values);
    void glPixelMapuiv(GLenum map, GLsizei mapsize, const GLuint* values);
    void glPixelMapusv(GLenum map, GLsizei mapsize, const GLushort* values);
    void glPixelMapx(GLenum map, GLint size, const GLfixed* values);
    void glPixelStoref(GLenum pname, GLfloat param);
    void glPixelStorei(GLenum pname, GLint param);
    void glPixelStorex(GLenum pname, GLfixed param);
    void glPixelTexGenParameterfSGIS(GLenum pname, GLfloat param);
    void glPixelTexGenParameterfvSGIS(GLenum pname, const GLfloat* params);
    void glPixelTexGenParameteriSGIS(GLenum pname, GLint param);
    void glPixelTexGenParameterivSGIS(GLenum pname, const GLint* params);
    void glPixelTexGenSGIX(GLenum mode);
    void glPixelTransferf(GLenum pname, GLfloat param);
    void glPixelTransferi(GLenum pname, GLint param);
    void glPixelTransferxOES(GLenum pname, GLfixed param);
    void glPixelTransformParameterfEXT(GLenum target, GLenum pname, GLfloat param);
    void glPixelTransformParameterfvEXT(GLenum target, GLenum pname, const GLfloat* params);
    void glPixelTransformParameteriEXT(GLenum target, GLenum pname, GLint param);
    void glPixelTransformParameterivEXT(GLenum target, GLenum pname, const GLint* params);
    void glPixelZoom(GLfloat xfactor, GLfloat yfactor);
    void glPixelZoomxOES(GLfixed xfactor, GLfixed yfactor);
    GLboolean glPointAlongPathNV(GLuint path, GLsizei startSegment, GLsizei numSegments, GLfloat distance, GLfloat* x, GLfloat* y, GLfloat* tangentX, GLfloat* tangentY);
    void glPointParameterf(GLenum pname, GLfloat param);
    void glPointParameterfARB(GLenum pname, GLfloat param);
    void glPointParameterfEXT(GLenum pname, GLfloat param);
    void glPointParameterfSGIS(GLenum pname, GLfloat param);
    void glPointParameterfv(GLenum pname, const GLfloat* params);
    void glPointParameterfvARB(GLenum pname, const GLfloat* params);
    void glPointParameterfvEXT(GLenum pname, const GLfloat* params);
    void glPointParameterfvSGIS(GLenum pname, const GLfloat* params);
    void glPointParameteri(GLenum pname, GLint param);
    void glPointParameteriNV(GLenum pname, GLint param);
    void glPointParameteriv(GLenum pname, const GLint* params);
    void glPointParameterivNV(GLenum pname, const GLint* params);
    void glPointParameterxvOES(GLenum pname, const GLfixed* params);
    void glPointSize(GLfloat size);
    void glPointSizexOES(GLfixed size);
    GLint glPollAsyncSGIX(GLuint* markerp);
    GLint glPollInstrumentsSGIX(GLint* marker_p);
    void glPolygonMode(GLenum face, GLenum mode);
    void glPolygonOffset(GLfloat factor, GLfloat units);
    void glPolygonOffsetClamp(GLfloat factor, GLfloat units, GLfloat clamp);
    void glPolygonOffsetClampEXT(GLfloat factor, GLfloat units, GLfloat clamp);
    void glPolygonOffsetxOES(GLfixed factor, GLfixed units);
    void glPolygonStipple(const GLubyte* mask);
    void glPopGroupMarkerEXT(void);
    void glPresentFrameDualFillNV(GLuint video_slot, GLuint64EXT minPresentTime, GLuint beginPresentTimeId, GLuint presentDurationId, GLenum type, GLenum target0, GLuint fill0, GLenum target1, GLuint fill1, GLenum target2, GLuint fill2, GLenum target3, GLuint fill3);
    void glPresentFrameKeyedNV(GLuint video_slot, GLuint64EXT minPresentTime, GLuint beginPresentTimeId, GLuint presentDurationId, GLenum type, GLenum target0, GLuint fill0, GLuint key0, GLenum target1, GLuint fill1, GLuint key1);
    void glPrimitiveBoundingBox(GLfloat minX, GLfloat minY, GLfloat minZ, GLfloat minW, GLfloat maxX, GLfloat maxY, GLfloat maxZ, GLfloat maxW);
    void glPrimitiveBoundingBoxARB(GLfloat minX, GLfloat minY, GLfloat minZ, GLfloat minW, GLfloat maxX, GLfloat maxY, GLfloat maxZ, GLfloat maxW);
    void glPrimitiveRestartIndex(GLuint index);
    void glPrimitiveRestartIndexNV(GLuint index);
    void glPrimitiveRestartNV(void);
    void glPrioritizeTextures(GLsizei n, const GLuint* textures, const GLclampf* priorities);
    void glPrioritizeTexturesEXT(GLsizei n, const GLuint* textures, const GLclampf* priorities);
    void glPrioritizeTexturesxOES(GLsizei n, const GLuint* textures, const GLfixed* priorities);
    void glProgramBinary(GLuint program, GLenum binaryFormat, const void* binary, GLsizei length);
    void glProgramBufferParametersIivNV(GLenum target, GLuint bindingIndex, GLuint wordIndex, GLsizei count, const GLint* params);
    void glProgramBufferParametersIuivNV(GLenum target, GLuint bindingIndex, GLuint wordIndex, GLsizei count, const GLuint* params);
    void glProgramBufferParametersfvNV(GLenum target, GLuint bindingIndex, GLuint wordIndex, GLsizei count, const GLfloat* params);
    void glProgramEnvParameter4dARB(GLenum target, GLuint index, GLdouble x, GLdouble y, GLdouble z, GLdouble w);
    void glProgramEnvParameter4dvARB(GLenum target, GLuint index, const GLdouble* params);
    void glProgramEnvParameter4fARB(GLenum target, GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
    void glProgramEnvParameter4fvARB(GLenum target, GLuint index, const GLfloat* params);
    void glProgramEnvParameterI4iNV(GLenum target, GLuint index, GLint x, GLint y, GLint z, GLint w);
    void glProgramEnvParameterI4ivNV(GLenum target, GLuint index, const GLint* params);
    void glProgramEnvParameterI4uiNV(GLenum target, GLuint index, GLuint x, GLuint y, GLuint z, GLuint w);
    void glProgramEnvParameterI4uivNV(GLenum target, GLuint index, const GLuint* params);
    void glProgramEnvParameters4fvEXT(GLenum target, GLuint index, GLsizei count, const GLfloat* params);
    void glProgramEnvParametersI4ivNV(GLenum target, GLuint index, GLsizei count, const GLint* params);
    void glProgramEnvParametersI4uivNV(GLenum target, GLuint index, GLsizei count, const GLuint* params);
    void glProgramLocalParameter4dARB(GLenum target, GLuint index, GLdouble x, GLdouble y, GLdouble z, GLdouble w);
    void glProgramLocalParameter4dvARB(GLenum target, GLuint index, const GLdouble* params);
    void glProgramLocalParameter4fARB(GLenum target, GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
    void glProgramLocalParameter4fvARB(GLenum target, GLuint index, const GLfloat* params);
    void glProgramLocalParameterI4iNV(GLenum target, GLuint index, GLint x, GLint y, GLint z, GLint w);
    void glProgramLocalParameterI4ivNV(GLenum target, GLuint index, const GLint* params);
    void glProgramLocalParameterI4uiNV(GLenum target, GLuint index, GLuint x, GLuint y, GLuint z, GLuint w);
    void glProgramLocalParameterI4uivNV(GLenum target, GLuint index, const GLuint* params);
    void glProgramLocalParameters4fvEXT(GLenum target, GLuint index, GLsizei count, const GLfloat* params);
    void glProgramLocalParametersI4ivNV(GLenum target, GLuint index, GLsizei count, const GLint* params);
    void glProgramLocalParametersI4uivNV(GLenum target, GLuint index, GLsizei count, const GLuint* params);
    void glProgramNamedParameter4dNV(GLuint id, GLsizei len, const GLubyte* name, GLdouble x, GLdouble y, GLdouble z, GLdouble w);
    void glProgramNamedParameter4dvNV(GLuint id, GLsizei len, const GLubyte* name, const GLdouble* v);
    void glProgramNamedParameter4fNV(GLuint id, GLsizei len, const GLubyte* name, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
    void glProgramNamedParameter4fvNV(GLuint id, GLsizei len, const GLubyte* name, const GLfloat* v);
    void glProgramParameter4dNV(GLenum target, GLuint index, GLdouble x, GLdouble y, GLdouble z, GLdouble w);
    void glProgramParameter4dvNV(GLenum target, GLuint index, const GLdouble* v);
    void glProgramParameter4fNV(GLenum target, GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
    void glProgramParameter4fvNV(GLenum target, GLuint index, const GLfloat* v);
    void glProgramParameteri(GLuint program, GLenum pname, GLint value);
    void glProgramParameters4dvNV(GLenum target, GLuint index, GLsizei count, const GLdouble* v);
    void glProgramParameters4fvNV(GLenum target, GLuint index, GLsizei count, const GLfloat* v);
    void glProgramPathFragmentInputGenNV(GLuint program, GLint location, GLenum genMode, GLint components, const GLfloat* coeffs);
    void glProgramStringARB(GLenum target, GLenum format, GLsizei len, const void* string);
    void glProgramSubroutineParametersuivNV(GLenum target, GLsizei count, const GLuint* params);
    void glProgramUniform1d(GLuint program, GLint location, GLdouble v0);
    void glProgramUniform1dEXT(GLuint program, GLint location, GLdouble x);
    void glProgramUniform1dv(GLuint program, GLint location, GLsizei count, const GLdouble* value);
    void glProgramUniform1dvEXT(GLuint program, GLint location, GLsizei count, const GLdouble* value);
    void glProgramUniform1f(GLuint program, GLint location, GLfloat v0);
    void glProgramUniform1fv(GLuint program, GLint location, GLsizei count, const GLfloat* value);
    void glProgramUniform1i(GLuint program, GLint location, GLint v0);
    void glProgramUniform1i64ARB(GLuint program, GLint location, GLint64 x);
    void glProgramUniform1i64NV(GLuint program, GLint location, GLint64EXT x);
    void glProgramUniform1i64vARB(GLuint program, GLint location, GLsizei count, const GLint64* value);
    void glProgramUniform1i64vNV(GLuint program, GLint location, GLsizei count, const GLint64EXT* value);
    void glProgramUniform1iv(GLuint program, GLint location, GLsizei count, const GLint* value);
    void glProgramUniform1ui(GLuint program, GLint location, GLuint v0);
    void glProgramUniform1ui64ARB(GLuint program, GLint location, GLuint64 x);
    void glProgramUniform1ui64NV(GLuint program, GLint location, GLuint64EXT x);
    void glProgramUniform1ui64vARB(GLuint program, GLint location, GLsizei count, const GLuint64* value);
    void glProgramUniform1ui64vNV(GLuint program, GLint location, GLsizei count, const GLuint64EXT* value);
    void glProgramUniform1uiv(GLuint program, GLint location, GLsizei count, const GLuint* value);
    void glProgramUniform2d(GLuint program, GLint location, GLdouble v0, GLdouble v1);
    void glProgramUniform2dEXT(GLuint program, GLint location, GLdouble x, GLdouble y);
    void glProgramUniform2dv(GLuint program, GLint location, GLsizei count, const GLdouble* value);
    void glProgramUniform2dvEXT(GLuint program, GLint location, GLsizei count, const GLdouble* value);
    void glProgramUniform2f(GLuint program, GLint location, GLfloat v0, GLfloat v1);
    void glProgramUniform2fv(GLuint program, GLint location, GLsizei count, const GLfloat* value);
    void glProgramUniform2i(GLuint program, GLint location, GLint v0, GLint v1);
    void glProgramUniform2i64ARB(GLuint program, GLint location, GLint64 x, GLint64 y);
    void glProgramUniform2i64NV(GLuint program, GLint location, GLint64EXT x, GLint64EXT y);
    void glProgramUniform2i64vARB(GLuint program, GLint location, GLsizei count, const GLint64* value);
    void glProgramUniform2i64vNV(GLuint program, GLint location, GLsizei count, const GLint64EXT* value);
    void glProgramUniform2iv(GLuint program, GLint location, GLsizei count, const GLint* value);
    void glProgramUniform2ui(GLuint program, GLint location, GLuint v0, GLuint v1);
    void glProgramUniform2ui64ARB(GLuint program, GLint location, GLuint64 x, GLuint64 y);
    void glProgramUniform2ui64NV(GLuint program, GLint location, GLuint64EXT x, GLuint64EXT y);
    void glProgramUniform2ui64vARB(GLuint program, GLint location, GLsizei count, const GLuint64* value);
    void glProgramUniform2ui64vNV(GLuint program, GLint location, GLsizei count, const GLuint64EXT* value);
    void glProgramUniform2uiv(GLuint program, GLint location, GLsizei count, const GLuint* value);
    void glProgramUniform3d(GLuint program, GLint location, GLdouble v0, GLdouble v1, GLdouble v2);
    void glProgramUniform3dEXT(GLuint program, GLint location, GLdouble x, GLdouble y, GLdouble z);
    void glProgramUniform3dv(GLuint program, GLint location, GLsizei count, const GLdouble* value);
    void glProgramUniform3dvEXT(GLuint program, GLint location, GLsizei count, const GLdouble* value);
    void glProgramUniform3f(GLuint program, GLint location, GLfloat v0, GLfloat v1, GLfloat v2);
    void glProgramUniform3fv(GLuint program, GLint location, GLsizei count, const GLfloat* value);
    void glProgramUniform3i(GLuint program, GLint location, GLint v0, GLint v1, GLint v2);
    void glProgramUniform3i64ARB(GLuint program, GLint location, GLint64 x, GLint64 y, GLint64 z);
    void glProgramUniform3i64NV(GLuint program, GLint location, GLint64EXT x, GLint64EXT y, GLint64EXT z);
    void glProgramUniform3i64vARB(GLuint program, GLint location, GLsizei count, const GLint64* value);
    void glProgramUniform3i64vNV(GLuint program, GLint location, GLsizei count, const GLint64EXT* value);
    void glProgramUniform3iv(GLuint program, GLint location, GLsizei count, const GLint* value);
    void glProgramUniform3ui(GLuint program, GLint location, GLuint v0, GLuint v1, GLuint v2);
    void glProgramUniform3ui64ARB(GLuint program, GLint location, GLuint64 x, GLuint64 y, GLuint64 z);
    void glProgramUniform3ui64NV(GLuint program, GLint location, GLuint64EXT x, GLuint64EXT y, GLuint64EXT z);
    void glProgramUniform3ui64vARB(GLuint program, GLint location, GLsizei count, const GLuint64* value);
    void glProgramUniform3ui64vNV(GLuint program, GLint location, GLsizei count, const GLuint64EXT* value);
    void glProgramUniform3uiv(GLuint program, GLint location, GLsizei count, const GLuint* value);
    void glProgramUniform4d(GLuint program, GLint location, GLdouble v0, GLdouble v1, GLdouble v2, GLdouble v3);
    void glProgramUniform4dEXT(GLuint program, GLint location, GLdouble x, GLdouble y, GLdouble z, GLdouble w);
    void glProgramUniform4dv(GLuint program, GLint location, GLsizei count, const GLdouble* value);
    void glProgramUniform4dvEXT(GLuint program, GLint location, GLsizei count, const GLdouble* value);
    void glProgramUniform4f(GLuint program, GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3);
    void glProgramUniform4fv(GLuint program, GLint location, GLsizei count, const GLfloat* value);
    void glProgramUniform4i(GLuint program, GLint location, GLint v0, GLint v1, GLint v2, GLint v3);
    void glProgramUniform4i64ARB(GLuint program, GLint location, GLint64 x, GLint64 y, GLint64 z, GLint64 w);
    void glProgramUniform4i64NV(GLuint program, GLint location, GLint64EXT x, GLint64EXT y, GLint64EXT z, GLint64EXT w);
    void glProgramUniform4i64vARB(GLuint program, GLint location, GLsizei count, const GLint64* value);
    void glProgramUniform4i64vNV(GLuint program, GLint location, GLsizei count, const GLint64EXT* value);
    void glProgramUniform4iv(GLuint program, GLint location, GLsizei count, const GLint* value);
    void glProgramUniform4ui(GLuint program, GLint location, GLuint v0, GLuint v1, GLuint v2, GLuint v3);
    void glProgramUniform4ui64ARB(GLuint program, GLint location, GLuint64 x, GLuint64 y, GLuint64 z, GLuint64 w);
    void glProgramUniform4ui64NV(GLuint program, GLint location, GLuint64EXT x, GLuint64EXT y, GLuint64EXT z, GLuint64EXT w);
    void glProgramUniform4ui64vARB(GLuint program, GLint location, GLsizei count, const GLuint64* value);
    void glProgramUniform4ui64vNV(GLuint program, GLint location, GLsizei count, const GLuint64EXT* value);
    void glProgramUniform4uiv(GLuint program, GLint location, GLsizei count, const GLuint* value);
    void glProgramUniformHandleui64ARB(GLuint program, GLint location, GLuint64 value);
    void glProgramUniformHandleui64NV(GLuint program, GLint location, GLuint64 value);
    void glProgramUniformHandleui64vARB(GLuint program, GLint location, GLsizei count, const GLuint64* values);
    void glProgramUniformHandleui64vNV(GLuint program, GLint location, GLsizei count, const GLuint64* values);
    void glProgramUniformMatrix2dv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLdouble* value);
    void glProgramUniformMatrix2dvEXT(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLdouble* value);
    void glProgramUniformMatrix2fv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat* value);
    void glProgramUniformMatrix2x3dv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLdouble* value);
    void glProgramUniformMatrix2x3dvEXT(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLdouble* value);
    void glProgramUniformMatrix2x3fv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat* value);
    void glProgramUniformMatrix2x4dv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLdouble* value);
    void glProgramUniformMatrix2x4dvEXT(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLdouble* value);
    void glProgramUniformMatrix2x4fv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat* value);
    void glProgramUniformMatrix3dv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLdouble* value);
    void glProgramUniformMatrix3dvEXT(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLdouble* value);
    void glProgramUniformMatrix3fv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat* value);
    void glProgramUniformMatrix3x2dv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLdouble* value);
    void glProgramUniformMatrix3x2dvEXT(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLdouble* value);
    void glProgramUniformMatrix3x2fv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat* value);
    void glProgramUniformMatrix3x4dv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLdouble* value);
    void glProgramUniformMatrix3x4dvEXT(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLdouble* value);
    void glProgramUniformMatrix3x4fv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat* value);
    void glProgramUniformMatrix4dv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLdouble* value);
    void glProgramUniformMatrix4dvEXT(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLdouble* value);
    void glProgramUniformMatrix4fv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat* value);
    void glProgramUniformMatrix4x2dv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLdouble* value);
    void glProgramUniformMatrix4x2dvEXT(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLdouble* value);
    void glProgramUniformMatrix4x2fv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat* value);
    void glProgramUniformMatrix4x3dv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLdouble* value);
    void glProgramUniformMatrix4x3dvEXT(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLdouble* value);
    void glProgramUniformMatrix4x3fv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat* value);
    void glProgramUniformui64NV(GLuint program, GLint location, GLuint64EXT value);
    void glProgramUniformui64vNV(GLuint program, GLint location, GLsizei count, const GLuint64EXT* value);
    void glProgramVertexLimitNV(GLenum target, GLint limit);
    void glProvokingVertex(GLenum mode);
    void glProvokingVertexEXT(GLenum mode);
    void glPushAttrib(GLbitfield mask);
    void glPushClientAttrib(GLbitfield mask);
    void glPushClientAttribDefaultEXT(GLbitfield mask);
    void glPushDebugGroup(GLenum source, GLuint id, GLsizei length, const GLchar* message);
    void glPushGroupMarkerEXT(GLsizei length, const GLchar* marker);
    void glPushName(GLuint name);
    void glQueryCounter(GLuint id, GLenum target);
    GLbitfield glQueryMatrixxOES(GLfixed* mantissa, GLint* exponent);
    void glQueryObjectParameteruiAMD(GLenum target, GLuint id, GLenum pname, GLuint param);
    GLint glQueryResourceNV(GLenum queryType, GLint tagId, GLuint count, GLint* buffer);
    void glQueryResourceTagNV(GLint tagId, const GLchar* tagString);
    void glRasterPos2d(GLdouble x, GLdouble y);
    void glRasterPos2dv(const GLdouble* v);
    void glRasterPos2f(GLfloat x, GLfloat y);
    void glRasterPos2fv(const GLfloat* v);
    void glRasterPos2i(GLint x, GLint y);
    void glRasterPos2iv(const GLint* v);
    void glRasterPos2s(GLshort x, GLshort y);
    void glRasterPos2sv(const GLshort* v);
    void glRasterPos2xOES(GLfixed x, GLfixed y);
    void glRasterPos2xvOES(const GLfixed* coords);
    void glRasterPos3d(GLdouble x, GLdouble y, GLdouble z);
    void glRasterPos3dv(const GLdouble* v);
    void glRasterPos3f(GLfloat x, GLfloat y, GLfloat z);
    void glRasterPos3fv(const GLfloat* v);
    void glRasterPos3i(GLint x, GLint y, GLint z);
    void glRasterPos3iv(const GLint* v);
    void glRasterPos3s(GLshort x, GLshort y, GLshort z);
    void glRasterPos3sv(const GLshort* v);
    void glRasterPos3xOES(GLfixed x, GLfixed y, GLfixed z);
    void glRasterPos3xvOES(const GLfixed* coords);
    void glRasterPos4d(GLdouble x, GLdouble y, GLdouble z, GLdouble w);
    void glRasterPos4dv(const GLdouble* v);
    void glRasterPos4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w);
    void glRasterPos4fv(const GLfloat* v);
    void glRasterPos4i(GLint x, GLint y, GLint z, GLint w);
    void glRasterPos4iv(const GLint* v);
    void glRasterPos4s(GLshort x, GLshort y, GLshort z, GLshort w);
    void glRasterPos4sv(const GLshort* v);
    void glRasterPos4xOES(GLfixed x, GLfixed y, GLfixed z, GLfixed w);
    void glRasterPos4xvOES(const GLfixed* coords);
    void glRasterSamplesEXT(GLuint samples, GLboolean fixedsamplelocations);
    void glReadBuffer(GLenum src);
    void glReadInstrumentsSGIX(GLint marker);
    void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels);
    void glReadnPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLsizei bufSize, void* data);
    void glRectd(GLdouble x1, GLdouble y1, GLdouble x2, GLdouble y2);
    void glRectdv(const GLdouble* v1, const GLdouble* v2);
    void glRectf(GLfloat x1, GLfloat y1, GLfloat x2, GLfloat y2);
    void glRectfv(const GLfloat* v1, const GLfloat* v2);
    void glRecti(GLint x1, GLint y1, GLint x2, GLint y2);
    void glRectiv(const GLint* v1, const GLint* v2);
    void glRects(GLshort x1, GLshort y1, GLshort x2, GLshort y2);
    void glRectsv(const GLshort* v1, const GLshort* v2);
    void glRectxOES(GLfixed x1, GLfixed y1, GLfixed x2, GLfixed y2);
    void glRectxvOES(const GLfixed* v1, const GLfixed* v2);
    void glReferencePlaneSGIX(const GLdouble* equation);
    GLboolean glReleaseKeyedMutexWin32EXT(GLuint memory, GLuint64 key);
    void glRenderGpuMaskNV(GLbitfield mask);
    GLint glRenderMode(GLenum mode);
    void glRenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height);
    void glRenderbufferStorageMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height);
    void glRenderbufferStorageMultisampleAdvancedAMD(GLenum target, GLsizei samples, GLsizei storageSamples, GLenum internalformat, GLsizei width, GLsizei height);
    void glRenderbufferStorageMultisampleCoverageNV(GLenum target, GLsizei coverageSamples, GLsizei colorSamples, GLenum internalformat, GLsizei width, GLsizei height);
    void glReplacementCodePointerSUN(GLenum type, GLsizei stride, const void** pointer);
    void glReplacementCodeubSUN(GLubyte code);
    void glReplacementCodeubvSUN(const GLubyte* code);
    void glReplacementCodeuiColor3fVertex3fSUN(GLuint rc, GLfloat r, GLfloat g, GLfloat b, GLfloat x, GLfloat y, GLfloat z);
    void glReplacementCodeuiColor3fVertex3fvSUN(const GLuint* rc, const GLfloat* c, const GLfloat* v);
    void glReplacementCodeuiColor4fNormal3fVertex3fSUN(GLuint rc, GLfloat r, GLfloat g, GLfloat b, GLfloat a, GLfloat nx, GLfloat ny, GLfloat nz, GLfloat x, GLfloat y, GLfloat z);
    void glReplacementCodeuiColor4fNormal3fVertex3fvSUN(const GLuint* rc, const GLfloat* c, const GLfloat* n, const GLfloat* v);
    void glReplacementCodeuiColor4ubVertex3fSUN(GLuint rc, GLubyte r, GLubyte g, GLubyte b, GLubyte a, GLfloat x, GLfloat y, GLfloat z);
    void glReplacementCodeuiColor4ubVertex3fvSUN(const GLuint* rc, const GLubyte* c, const GLfloat* v);
    void glReplacementCodeuiNormal3fVertex3fSUN(GLuint rc, GLfloat nx, GLfloat ny, GLfloat nz, GLfloat x, GLfloat y, GLfloat z);
    void glReplacementCodeuiNormal3fVertex3fvSUN(const GLuint* rc, const GLfloat* n, const GLfloat* v);
    void glReplacementCodeuiSUN(GLuint code);
    void glReplacementCodeuiTexCoord2fColor4fNormal3fVertex3fSUN(GLuint rc, GLfloat s, GLfloat t, GLfloat r, GLfloat g, GLfloat b, GLfloat a, GLfloat nx, GLfloat ny, GLfloat nz, GLfloat x, GLfloat y, GLfloat z);
    void glReplacementCodeuiTexCoord2fColor4fNormal3fVertex3fvSUN(const GLuint* rc, const GLfloat* tc, const GLfloat* c, const GLfloat* n, const GLfloat* v);
    void glReplacementCodeuiTexCoord2fNormal3fVertex3fSUN(GLuint rc, GLfloat s, GLfloat t, GLfloat nx, GLfloat ny, GLfloat nz, GLfloat x, GLfloat y, GLfloat z);
    void glReplacementCodeuiTexCoord2fNormal3fVertex3fvSUN(const GLuint* rc, const GLfloat* tc, const GLfloat* n, const GLfloat* v);
    void glReplacementCodeuiTexCoord2fVertex3fSUN(GLuint rc, GLfloat s, GLfloat t, GLfloat x, GLfloat y, GLfloat z);
    void glReplacementCodeuiTexCoord2fVertex3fvSUN(const GLuint* rc, const GLfloat* tc, const GLfloat* v);
    void glReplacementCodeuiVertex3fSUN(GLuint rc, GLfloat x, GLfloat y, GLfloat z);
    void glReplacementCodeuiVertex3fvSUN(const GLuint* rc, const GLfloat* v);
    void glReplacementCodeuivSUN(const GLuint* code);
    void glReplacementCodeusSUN(GLushort code);
    void glReplacementCodeusvSUN(const GLushort* code);
    void glRequestResidentProgramsNV(GLsizei n, const GLuint* programs);
    void glResetHistogram(GLenum target);
    void glResetHistogramEXT(GLenum target);
    void glResetMemoryObjectParameterNV(GLuint memory, GLenum pname);
    void glResetMinmax(GLenum target);
    void glResetMinmaxEXT(GLenum target);
    void glResizeBuffersMESA(void);
    void glResolveDepthValuesNV(void);
    void glResumeTransformFeedbackNV(void);
    void glRotated(GLdouble ane, GLdouble x, GLdouble y, GLdouble z);
    void glRotatef(GLfloat ane, GLfloat x, GLfloat y, GLfloat z);
    void glRotatexOES(GLfixed ane, GLfixed x, GLfixed y, GLfixed z);
    void glSampleCoverage(GLfloat value, GLboolean invert);
    void glSampleMapATI(GLuint dst, GLuint interp, GLenum swizzle);
    void glSampleMaskEXT(GLclampf value, GLboolean invert);
    void glSampleMaskIndexedNV(GLuint index, GLbitfield mask);
    void glSampleMaskSGIS(GLclampf value, GLboolean invert);
    void glSampleMaski(GLuint maskNumber, GLbitfield mask);
    void glSamplePatternEXT(GLenum pattern);
    void glSamplePatternSGIS(GLenum pattern);
    void glSamplerParameterIiv(GLuint sampler, GLenum pname, const GLint* param);
    void glSamplerParameterIuiv(GLuint sampler, GLenum pname, const GLuint* param);
    void glSamplerParameterf(GLuint sampler, GLenum pname, GLfloat param);
    void glSamplerParameterfv(GLuint sampler, GLenum pname, const GLfloat* param);
    void glSamplerParameteri(GLuint sampler, GLenum pname, GLint param);
    void glSamplerParameteriv(GLuint sampler, GLenum pname, const GLint* param);
    void glScaled(GLdouble x, GLdouble y, GLdouble z);
    void glScalef(GLfloat x, GLfloat y, GLfloat z);
    void glScalexOES(GLfixed x, GLfixed y, GLfixed z);
    void glScissor(GLint x, GLint y, GLsizei width, GLsizei height);
    void glScissorArrayv(GLuint first, GLsizei count, const GLint* v);
    void glScissorExclusiveArrayvNV(GLuint first, GLsizei count, const GLint* v);
    void glScissorExclusiveNV(GLint x, GLint y, GLsizei width, GLsizei height);
    void glScissorIndexed(GLuint index, GLint left, GLint bottom, GLsizei width, GLsizei height);
    void glScissorIndexedv(GLuint index, const GLint* v);
    void glSecondaryColor3b(GLbyte red, GLbyte green, GLbyte blue);
    void glSecondaryColor3bEXT(GLbyte red, GLbyte green, GLbyte blue);
    void glSecondaryColor3bv(const GLbyte* v);
    void glSecondaryColor3bvEXT(const GLbyte* v);
    void glSecondaryColor3d(GLdouble red, GLdouble green, GLdouble blue);
    void glSecondaryColor3dEXT(GLdouble red, GLdouble green, GLdouble blue);
    void glSecondaryColor3dv(const GLdouble* v);
    void glSecondaryColor3dvEXT(const GLdouble* v);
    void glSecondaryColor3f(GLfloat red, GLfloat green, GLfloat blue);
    void glSecondaryColor3fEXT(GLfloat red, GLfloat green, GLfloat blue);
    void glSecondaryColor3fv(const GLfloat* v);
    void glSecondaryColor3fvEXT(const GLfloat* v);
    void glSecondaryColor3hNV(GLhalfNV red, GLhalfNV green, GLhalfNV blue);
    void glSecondaryColor3hvNV(const GLhalfNV* v);
    void glSecondaryColor3i(GLint red, GLint green, GLint blue);
    void glSecondaryColor3iEXT(GLint red, GLint green, GLint blue);
    void glSecondaryColor3iv(const GLint* v);
    void glSecondaryColor3ivEXT(const GLint* v);
    void glSecondaryColor3s(GLshort red, GLshort green, GLshort blue);
    void glSecondaryColor3sEXT(GLshort red, GLshort green, GLshort blue);
    void glSecondaryColor3sv(const GLshort* v);
    void glSecondaryColor3svEXT(const GLshort* v);
    void glSecondaryColor3ub(GLubyte red, GLubyte green, GLubyte blue);
    void glSecondaryColor3ubEXT(GLubyte red, GLubyte green, GLubyte blue);
    void glSecondaryColor3ubv(const GLubyte* v);
    void glSecondaryColor3ubvEXT(const GLubyte* v);
    void glSecondaryColor3ui(GLuint red, GLuint green, GLuint blue);
    void glSecondaryColor3uiEXT(GLuint red, GLuint green, GLuint blue);
    void glSecondaryColor3uiv(const GLuint* v);
    void glSecondaryColor3uivEXT(const GLuint* v);
    void glSecondaryColor3us(GLushort red, GLushort green, GLushort blue);
    void glSecondaryColor3usEXT(GLushort red, GLushort green, GLushort blue);
    void glSecondaryColor3usv(const GLushort* v);
    void glSecondaryColor3usvEXT(const GLushort* v);
    void glSecondaryColorFormatNV(GLint size, GLenum type, GLsizei stride);
    void glSecondaryColorP3ui(GLenum type, GLuint color);
    void glSecondaryColorP3uiv(GLenum type, const GLuint* color);
    void glSecondaryColorPointer(GLint size, GLenum type, GLsizei stride, const void* pointer);
    void glSecondaryColorPointerEXT(GLint size, GLenum type, GLsizei stride, const void* pointer);
    void glSecondaryColorPointerListIBM(GLint size, GLenum type, GLint stride, const void** pointer, GLint ptrstride);
    void glSelectBuffer(GLsizei size, GLuint* buffer);
    void glSelectPerfMonitorCountersAMD(GLuint monitor, GLboolean enable, GLuint group, GLint numCounters, GLuint* counterList);
    void glSemaphoreParameterivNV(GLuint semaphore, GLenum pname, const GLint* params);
    void glSemaphoreParameterui64vEXT(GLuint semaphore, GLenum pname, const GLuint64* params);
    void glSeparableFilter2D(GLenum target, GLenum internalformat, GLsizei width, GLsizei height, GLenum format, GLenum type, const void* row, const void* column);
    void glSeparableFilter2DEXT(GLenum target, GLenum internalformat, GLsizei width, GLsizei height, GLenum format, GLenum type, const void* row, const void* column);
    void glSetFenceAPPLE(GLuint fence);
    void glSetFenceNV(GLuint fence, GLenum condition);
    void glSetFragmentShaderConstantATI(GLuint dst, const GLfloat* value);
    void glSetInvariantEXT(GLuint id, GLenum type, const void* addr);
    void glSetLocalConstantEXT(GLuint id, GLenum type, const void* addr);
    void glSetMultisamplefvAMD(GLenum pname, GLuint index, const GLfloat* val);
    void glShadeModel(GLenum mode);
    void glShaderBinary(GLsizei count, const GLuint* shaders, GLenum binaryformat, const void* binary, GLsizei length);
    void glShaderOp1EXT(GLenum op, GLuint res, GLuint arg1);
    void glShaderOp2EXT(GLenum op, GLuint res, GLuint arg1, GLuint arg2);
    void glShaderOp3EXT(GLenum op, GLuint res, GLuint arg1, GLuint arg2, GLuint arg3);
    void glShaderSource(GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length);
    void glShaderStorageBlockBinding(GLuint program, GLuint storageBlockIndex, GLuint storageBlockBinding);
    void glShadingRateImageBarrierNV(GLboolean synchronize);
    void glShadingRateImagePaletteNV(GLuint viewport, GLuint first, GLsizei count, const GLenum* rates);
    void glShadingRateSampleOrderCustomNV(GLenum rate, GLuint samples, const GLint* locations);
    void glShadingRateSampleOrderNV(GLenum order);
    void glSharpenTexFuncSGIS(GLenum target, GLsizei n, const GLfloat* points);
    void glSignalSemaphoreEXT(GLuint semaphore, GLuint numBufferBarriers, const GLuint* buffers, GLuint numTextureBarriers, const GLuint* textures, const GLenum* dstLayouts);
    void glSignalSemaphoreui64NVX(GLuint signalGpu, GLsizei fenceObjectCount, const GLuint* semaphoreArray, const GLuint64* fenceValueArray);
    void glSignalVkFenceNV(GLuint64 vkFence);
    void glSignalVkSemaphoreNV(GLuint64 vkSemaphore);
    void glSpecializeShader(GLuint shader, const GLchar* pEntryPoint, GLuint numSpecializationConstants, const GLuint* pConstantIndex, const GLuint* pConstantValue);
    void glSpecializeShaderARB(GLuint shader, const GLchar* pEntryPoint, GLuint numSpecializationConstants, const GLuint* pConstantIndex, const GLuint* pConstantValue);
    void glSpriteParameterfSGIX(GLenum pname, GLfloat param);
    void glSpriteParameterfvSGIX(GLenum pname, const GLfloat* params);
    void glSpriteParameteriSGIX(GLenum pname, GLint param);
    void glSpriteParameterivSGIX(GLenum pname, const GLint* params);
    void glStartInstrumentsSGIX(void);
    void glStateCaptureNV(GLuint state, GLenum mode);
    void glStencilClearTagEXT(GLsizei stencilTagBits, GLuint stencilClearTag);
    void glStencilFillPathInstancedNV(GLsizei numPaths, GLenum pathNameType, const void* paths, GLuint pathBase, GLenum fillMode, GLuint mask, GLenum transformType, const GLfloat* transformValues);
    void glStencilFillPathNV(GLuint path, GLenum fillMode, GLuint mask);
    void glStencilFunc(GLenum func, GLint ref, GLuint mask);
    void glStencilFuncSeparate(GLenum face, GLenum func, GLint ref, GLuint mask);
    void glStencilFuncSeparateATI(GLenum frontfunc, GLenum backfunc, GLint ref, GLuint mask);
    void glStencilMask(GLuint mask);
    void glStencilMaskSeparate(GLenum face, GLuint mask);
    void glStencilOp(GLenum fail, GLenum zfail, GLenum zpass);
    void glStencilOpSeparate(GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass);
    void glStencilOpSeparateATI(GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass);
    void glStencilOpValueAMD(GLenum face, GLuint value);
    void glStencilStrokePathInstancedNV(GLsizei numPaths, GLenum pathNameType, const void* paths, GLuint pathBase, GLint reference, GLuint mask, GLenum transformType, const GLfloat* transformValues);
    void glStencilStrokePathNV(GLuint path, GLint reference, GLuint mask);
    void glStencilThenCoverFillPathInstancedNV(GLsizei numPaths, GLenum pathNameType, const void* paths, GLuint pathBase, GLenum fillMode, GLuint mask, GLenum coverMode, GLenum transformType, const GLfloat* transformValues);
    void glStencilThenCoverFillPathNV(GLuint path, GLenum fillMode, GLuint mask, GLenum coverMode);
    void glStencilThenCoverStrokePathInstancedNV(GLsizei numPaths, GLenum pathNameType, const void* paths, GLuint pathBase, GLint reference, GLuint mask, GLenum coverMode, GLenum transformType, const GLfloat* transformValues);
    void glStencilThenCoverStrokePathNV(GLuint path, GLint reference, GLuint mask, GLenum coverMode);
    void glStopInstrumentsSGIX(GLint marker);
    void glStringMarkerGREMEDY(GLsizei len, const void* string);
    void glSubpixelPrecisionBiasNV(GLuint xbits, GLuint ybits);
    void glSwizzleEXT(GLuint res, GLuint in, GLenum outX, GLenum outY, GLenum outZ, GLenum outW);
    void glSyncTextureINTEL(GLuint texture);
    void glTagSampleBufferSGIX(void);
    void glTangent3bEXT(GLbyte tx, GLbyte ty, GLbyte tz);
    void glTangent3bvEXT(const GLbyte* v);
    void glTangent3dEXT(GLdouble tx, GLdouble ty, GLdouble tz);
    void glTangent3dvEXT(const GLdouble* v);
    void glTangent3fEXT(GLfloat tx, GLfloat ty, GLfloat tz);
    void glTangent3fvEXT(const GLfloat* v);
    void glTangent3iEXT(GLint tx, GLint ty, GLint tz);
    void glTangent3ivEXT(const GLint* v);
    void glTangent3sEXT(GLshort tx, GLshort ty, GLshort tz);
    void glTangent3svEXT(const GLshort* v);
    void glTangentPointerEXT(GLenum type, GLsizei stride, const void* pointer);
    void glTbufferMask3DFX(GLuint mask);
    void glTessellationFactorAMD(GLfloat factor);
    void glTessellationModeAMD(GLenum mode);
    GLboolean glTestFenceAPPLE(GLuint fence);
    GLboolean glTestFenceNV(GLuint fence);
    GLboolean glTestObjectAPPLE(GLenum object, GLuint name);
    void glTexAttachMemoryNV(GLenum target, GLuint memory, GLuint64 offset);
    void glTexBuffer(GLenum target, GLenum internalformat, GLuint buffer);
    void glTexBufferRange(GLenum target, GLenum internalformat, GLuint buffer, GLintptr offset, GLsizeiptr size);
    void glTexBumpParameterfvATI(GLenum pname, const GLfloat* param);
    void glTexBumpParameterivATI(GLenum pname, const GLint* param);
    void glTexCoord1bOES(GLbyte s);
    void glTexCoord1bvOES(const GLbyte* coords);
    void glTexCoord1d(GLdouble s);
    void glTexCoord1dv(const GLdouble* v);
    void glTexCoord1f(GLfloat s);
    void glTexCoord1fv(const GLfloat* v);
    void glTexCoord1hNV(GLhalfNV s);
    void glTexCoord1hvNV(const GLhalfNV* v);
    void glTexCoord1i(GLint s);
    void glTexCoord1iv(const GLint* v);
    void glTexCoord1s(GLshort s);
    void glTexCoord1sv(const GLshort* v);
    void glTexCoord1xOES(GLfixed s);
    void glTexCoord1xvOES(const GLfixed* coords);
    void glTexCoord2bOES(GLbyte s, GLbyte t);
    void glTexCoord2bvOES(const GLbyte* coords);
    void glTexCoord2d(GLdouble s, GLdouble t);
    void glTexCoord2dv(const GLdouble* v);
    void glTexCoord2f(GLfloat s, GLfloat t);
    void glTexCoord2fColor3fVertex3fSUN(GLfloat s, GLfloat t, GLfloat r, GLfloat g, GLfloat b, GLfloat x, GLfloat y, GLfloat z);
    void glTexCoord2fColor3fVertex3fvSUN(const GLfloat* tc, const GLfloat* c, const GLfloat* v);
    void glTexCoord2fColor4fNormal3fVertex3fSUN(GLfloat s, GLfloat t, GLfloat r, GLfloat g, GLfloat b, GLfloat a, GLfloat nx, GLfloat ny, GLfloat nz, GLfloat x, GLfloat y, GLfloat z);
    void glTexCoord2fColor4fNormal3fVertex3fvSUN(const GLfloat* tc, const GLfloat* c, const GLfloat* n, const GLfloat* v);
    void glTexCoord2fColor4ubVertex3fSUN(GLfloat s, GLfloat t, GLubyte r, GLubyte g, GLubyte b, GLubyte a, GLfloat x, GLfloat y, GLfloat z);
    void glTexCoord2fColor4ubVertex3fvSUN(const GLfloat* tc, const GLubyte* c, const GLfloat* v);
    void glTexCoord2fNormal3fVertex3fSUN(GLfloat s, GLfloat t, GLfloat nx, GLfloat ny, GLfloat nz, GLfloat x, GLfloat y, GLfloat z);
    void glTexCoord2fNormal3fVertex3fvSUN(const GLfloat* tc, const GLfloat* n, const GLfloat* v);
    void glTexCoord2fVertex3fSUN(GLfloat s, GLfloat t, GLfloat x, GLfloat y, GLfloat z);
    void glTexCoord2fVertex3fvSUN(const GLfloat* tc, const GLfloat* v);
    void glTexCoord2fv(const GLfloat* v);
    void glTexCoord2hNV(GLhalfNV s, GLhalfNV t);
    void glTexCoord2hvNV(const GLhalfNV* v);
    void glTexCoord2i(GLint s, GLint t);
    void glTexCoord2iv(const GLint* v);
    void glTexCoord2s(GLshort s, GLshort t);
    void glTexCoord2sv(const GLshort* v);
    void glTexCoord2xOES(GLfixed s, GLfixed t);
    void glTexCoord2xvOES(const GLfixed* coords);
    void glTexCoord3bOES(GLbyte s, GLbyte t, GLbyte r);
    void glTexCoord3bvOES(const GLbyte* coords);
    void glTexCoord3d(GLdouble s, GLdouble t, GLdouble r);
    void glTexCoord3dv(const GLdouble* v);
    void glTexCoord3f(GLfloat s, GLfloat t, GLfloat r);
    void glTexCoord3fv(const GLfloat* v);
    void glTexCoord3hNV(GLhalfNV s, GLhalfNV t, GLhalfNV r);
    void glTexCoord3hvNV(const GLhalfNV* v);
    void glTexCoord3i(GLint s, GLint t, GLint r);
    void glTexCoord3iv(const GLint* v);
    void glTexCoord3s(GLshort s, GLshort t, GLshort r);
    void glTexCoord3sv(const GLshort* v);
    void glTexCoord3xOES(GLfixed s, GLfixed t, GLfixed r);
    void glTexCoord3xvOES(const GLfixed* coords);
    void glTexCoord4bOES(GLbyte s, GLbyte t, GLbyte r, GLbyte q);
    void glTexCoord4bvOES(const GLbyte* coords);
    void glTexCoord4d(GLdouble s, GLdouble t, GLdouble r, GLdouble q);
    void glTexCoord4dv(const GLdouble* v);
    void glTexCoord4f(GLfloat s, GLfloat t, GLfloat r, GLfloat q);
    void glTexCoord4fColor4fNormal3fVertex4fSUN(GLfloat s, GLfloat t, GLfloat p, GLfloat q, GLfloat r, GLfloat g, GLfloat b, GLfloat a, GLfloat nx, GLfloat ny, GLfloat nz, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
    void glTexCoord4fColor4fNormal3fVertex4fvSUN(const GLfloat* tc, const GLfloat* c, const GLfloat* n, const GLfloat* v);
    void glTexCoord4fVertex4fSUN(GLfloat s, GLfloat t, GLfloat p, GLfloat q, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
    void glTexCoord4fVertex4fvSUN(const GLfloat* tc, const GLfloat* v);
    void glTexCoord4fv(const GLfloat* v);
    void glTexCoord4hNV(GLhalfNV s, GLhalfNV t, GLhalfNV r, GLhalfNV q);
    void glTexCoord4hvNV(const GLhalfNV* v);
    void glTexCoord4i(GLint s, GLint t, GLint r, GLint q);
    void glTexCoord4iv(const GLint* v);
    void glTexCoord4s(GLshort s, GLshort t, GLshort r, GLshort q);
    void glTexCoord4sv(const GLshort* v);
    void glTexCoord4xOES(GLfixed s, GLfixed t, GLfixed r, GLfixed q);
    void glTexCoord4xvOES(const GLfixed* coords);
    void glTexCoordFormatNV(GLint size, GLenum type, GLsizei stride);
    void glTexCoordP1ui(GLenum type, GLuint coords);
    void glTexCoordP1uiv(GLenum type, const GLuint* coords);
    void glTexCoordP2ui(GLenum type, GLuint coords);
    void glTexCoordP2uiv(GLenum type, const GLuint* coords);
    void glTexCoordP3ui(GLenum type, GLuint coords);
    void glTexCoordP3uiv(GLenum type, const GLuint* coords);
    void glTexCoordP4ui(GLenum type, GLuint coords);
    void glTexCoordP4uiv(GLenum type, const GLuint* coords);
    void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* ptr);
    void glTexCoordPointerEXT(GLint size, GLenum type, GLsizei stride, GLsizei count, const void* pointer);
    void glTexCoordPointerListIBM(GLint size, GLenum type, GLint stride, const void** pointer, GLint ptrstride);
    void glTexCoordPointervINTEL(GLint size, GLenum type, const void** pointer);
    void glTexEnvf(GLenum target, GLenum pname, GLfloat param);
    void glTexEnvfv(GLenum target, GLenum pname, const GLfloat* params);
    void glTexEnvi(GLenum target, GLenum pname, GLint param);
    void glTexEnviv(GLenum target, GLenum pname, const GLint* params);
    void glTexEnvxOES(GLenum target, GLenum pname, GLfixed param);
    void glTexEnvxvOES(GLenum target, GLenum pname, const GLfixed* params);
    void glTexFilterFuncSGIS(GLenum target, GLenum filter, GLsizei n, const GLfloat* weights);
    void glTexGend(GLenum coord, GLenum pname, GLdouble param);
    void glTexGendv(GLenum coord, GLenum pname, const GLdouble* params);
    void glTexGenf(GLenum coord, GLenum pname, GLfloat param);
    void glTexGenfv(GLenum coord, GLenum pname, const GLfloat* params);
    void glTexGeni(GLenum coord, GLenum pname, GLint param);
    void glTexGeniv(GLenum coord, GLenum pname, const GLint* params);
    void glTexGenxOES(GLenum coord, GLenum pname, GLfixed param);
    void glTexGenxvOES(GLenum coord, GLenum pname, const GLfixed* params);
    void glTexImage1D(GLenum target, GLint level, GLint internalFormat, GLsizei width, GLint border, GLenum format, GLenum type, const GLvoid* pixels);
    void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void* pixels);
    void glTexImage2DMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height, GLboolean fixedsamplelocations);
    void glTexImage2DMultisampleCoverageNV(GLenum target, GLsizei coverageSamples, GLsizei colorSamples, GLint internalFormat, GLsizei width, GLsizei height, GLboolean fixedSampleLocations);
    void glTexImage3D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type, const void* pixels);
    void glTexImage3DMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth, GLboolean fixedsamplelocations);
    void glTexImage3DMultisampleCoverageNV(GLenum target, GLsizei coverageSamples, GLsizei colorSamples, GLint internalFormat, GLsizei width, GLsizei height, GLsizei depth, GLboolean fixedSampleLocations);
    void glTexImage4DSGIS(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth, GLsizei size4d, GLint border, GLenum format, GLenum type, const void* pixels);
    void glTexPageCommitmentARB(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLboolean commit);
    void glTexPageCommitmentMemNV(GLenum target, GLint layer, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLuint memory, GLuint64 offset, GLboolean commit);
    void glTexParameterIiv(GLenum target, GLenum pname, const GLint* params);
    void glTexParameterIuiv(GLenum target, GLenum pname, const GLuint* params);
    void glTexParameterf(GLenum target, GLenum pname, GLfloat param);
    void glTexParameterfv(GLenum target, GLenum pname, const GLfloat* params);
    void glTexParameteri(GLenum target, GLenum pname, GLint param);
    void glTexParameteriv(GLenum target, GLenum pname, const GLint* params);
    void glTexParameterxOES(GLenum target, GLenum pname, GLfixed param);
    void glTexParameterxvOES(GLenum target, GLenum pname, const GLfixed* params);
    void glTexRenderbufferNV(GLenum target, GLuint renderbuffer);
    void glTexStorage1DEXT(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width);
    void glTexStorage2D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height);
    void glTexStorage2DMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height, GLboolean fixedsamplelocations);
    void glTexStorage3D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth);
    void glTexStorage3DMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth, GLboolean fixedsamplelocations);
    void glTexStorageMem1DEXT(GLenum target, GLsizei levels, GLenum internalFormat, GLsizei width, GLuint memory, GLuint64 offset);
    void glTexStorageMem2DEXT(GLenum target, GLsizei levels, GLenum internalFormat, GLsizei width, GLsizei height, GLuint memory, GLuint64 offset);
    void glTexStorageMem2DMultisampleEXT(GLenum target, GLsizei samples, GLenum internalFormat, GLsizei width, GLsizei height, GLboolean fixedSampleLocations, GLuint memory, GLuint64 offset);
    void glTexStorageMem3DEXT(GLenum target, GLsizei levels, GLenum internalFormat, GLsizei width, GLsizei height, GLsizei depth, GLuint memory, GLuint64 offset);
    void glTexStorageMem3DMultisampleEXT(GLenum target, GLsizei samples, GLenum internalFormat, GLsizei width, GLsizei height, GLsizei depth, GLboolean fixedSampleLocations, GLuint memory, GLuint64 offset);
    void glTexStorageSparseAMD(GLenum target, GLenum internalFormat, GLsizei width, GLsizei height, GLsizei depth, GLsizei layers, GLbitfield flags);
    void glTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format, GLenum type, const GLvoid* pixels);
    void glTexSubImage1DEXT(GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format, GLenum type, const void* pixels);
    void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void* pixels);
    void glTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, const void* pixels);
    void glTexSubImage4DSGIS(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLint woffset, GLsizei width, GLsizei height, GLsizei depth, GLsizei size4d, GLenum format, GLenum type, const void* pixels);
    void glTextureAttachMemoryNV(GLuint texture, GLuint memory, GLuint64 offset);
    void glTextureBarrier(void);
    void glTextureBarrierNV(void);
    void glTextureBuffer(GLuint texture, GLenum internalformat, GLuint buffer);
    void glTextureBufferEXT(GLuint texture, GLenum target, GLenum internalformat, GLuint buffer);
    void glTextureBufferRange(GLuint texture, GLenum internalformat, GLuint buffer, GLintptr offset, GLsizeiptr size);
    void glTextureBufferRangeEXT(GLuint texture, GLenum target, GLenum internalformat, GLuint buffer, GLintptr offset, GLsizeiptr size);
    void glTextureColorMaskSGIS(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha);
    void glTextureImage1DEXT(GLuint texture, GLenum target, GLint level, GLint internalformat, GLsizei width, GLint border, GLenum format, GLenum type, const void* pixels);
    void glTextureImage2DEXT(GLuint texture, GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void* pixels);
    void glTextureImage2DMultisampleCoverageNV(GLuint texture, GLenum target, GLsizei coverageSamples, GLsizei colorSamples, GLint internalFormat, GLsizei width, GLsizei height, GLboolean fixedSampleLocations);
    void glTextureImage2DMultisampleNV(GLuint texture, GLenum target, GLsizei samples, GLint internalFormat, GLsizei width, GLsizei height, GLboolean fixedSampleLocations);
    void glTextureImage3DEXT(GLuint texture, GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type, const void* pixels);
    void glTextureImage3DMultisampleCoverageNV(GLuint texture, GLenum target, GLsizei coverageSamples, GLsizei colorSamples, GLint internalFormat, GLsizei width, GLsizei height, GLsizei depth, GLboolean fixedSampleLocations);
    void glTextureImage3DMultisampleNV(GLuint texture, GLenum target, GLsizei samples, GLint internalFormat, GLsizei width, GLsizei height, GLsizei depth, GLboolean fixedSampleLocations);
    void glTextureLightEXT(GLenum pname);
    void glTextureMaterialEXT(GLenum face, GLenum mode);
    void glTextureNormalEXT(GLenum mode);
    void glTexturePageCommitmentEXT(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLboolean commit);
    void glTexturePageCommitmentMemNV(GLuint texture, GLint layer, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLuint memory, GLuint64 offset, GLboolean commit);
    void glTextureParameterIiv(GLuint texture, GLenum pname, const GLint* params);
    void glTextureParameterIivEXT(GLuint texture, GLenum target, GLenum pname, const GLint* params);
    void glTextureParameterIuiv(GLuint texture, GLenum pname, const GLuint* params);
    void glTextureParameterIuivEXT(GLuint texture, GLenum target, GLenum pname, const GLuint* params);
    void glTextureParameterf(GLuint texture, GLenum pname, GLfloat param);
    void glTextureParameterfEXT(GLuint texture, GLenum target, GLenum pname, GLfloat param);
    void glTextureParameterfv(GLuint texture, GLenum pname, const GLfloat* param);
    void glTextureParameterfvEXT(GLuint texture, GLenum target, GLenum pname, const GLfloat* params);
    void glTextureParameteri(GLuint texture, GLenum pname, GLint param);
    void glTextureParameteriEXT(GLuint texture, GLenum target, GLenum pname, GLint param);
    void glTextureParameteriv(GLuint texture, GLenum pname, const GLint* param);
    void glTextureParameterivEXT(GLuint texture, GLenum target, GLenum pname, const GLint* params);
    void glTextureRangeAPPLE(GLenum target, GLsizei length, const void* pointer);
    void glTextureRenderbufferEXT(GLuint texture, GLenum target, GLuint renderbuffer);
    void glTextureStorage1D(GLuint texture, GLsizei levels, GLenum internalformat, GLsizei width);
    void glTextureStorage1DEXT(GLuint texture, GLenum target, GLsizei levels, GLenum internalformat, GLsizei width);
    void glTextureStorage2D(GLuint texture, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height);
    void glTextureStorage2DEXT(GLuint texture, GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height);
    void glTextureStorage2DMultisample(GLuint texture, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height, GLboolean fixedsamplelocations);
    void glTextureStorage2DMultisampleEXT(GLuint texture, GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height, GLboolean fixedsamplelocations);
    void glTextureStorage3D(GLuint texture, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth);
    void glTextureStorage3DEXT(GLuint texture, GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth);
    void glTextureStorage3DMultisample(GLuint texture, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth, GLboolean fixedsamplelocations);
    void glTextureStorage3DMultisampleEXT(GLuint texture, GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth, GLboolean fixedsamplelocations);
    void glTextureStorageMem1DEXT(GLuint texture, GLsizei levels, GLenum internalFormat, GLsizei width, GLuint memory, GLuint64 offset);
    void glTextureStorageMem2DEXT(GLuint texture, GLsizei levels, GLenum internalFormat, GLsizei width, GLsizei height, GLuint memory, GLuint64 offset);
    void glTextureStorageMem2DMultisampleEXT(GLuint texture, GLsizei samples, GLenum internalFormat, GLsizei width, GLsizei height, GLboolean fixedSampleLocations, GLuint memory, GLuint64 offset);
    void glTextureStorageMem3DEXT(GLuint texture, GLsizei levels, GLenum internalFormat, GLsizei width, GLsizei height, GLsizei depth, GLuint memory, GLuint64 offset);
    void glTextureStorageMem3DMultisampleEXT(GLuint texture, GLsizei samples, GLenum internalFormat, GLsizei width, GLsizei height, GLsizei depth, GLboolean fixedSampleLocations, GLuint memory, GLuint64 offset);
    void glTextureStorageSparseAMD(GLuint texture, GLenum target, GLenum internalFormat, GLsizei width, GLsizei height, GLsizei depth, GLsizei layers, GLbitfield flags);
    void glTextureSubImage1D(GLuint texture, GLint level, GLint xoffset, GLsizei width, GLenum format, GLenum type, const void* pixels);
    void glTextureSubImage1DEXT(GLuint texture, GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format, GLenum type, const void* pixels);
    void glTextureSubImage2D(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void* pixels);
    void glTextureSubImage2DEXT(GLuint texture, GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void* pixels);
    void glTextureSubImage3D(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, const void* pixels);
    void glTextureSubImage3DEXT(GLuint texture, GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, const void* pixels);
    void glTextureView(GLuint texture, GLenum target, GLuint origtexture, GLenum internalformat, GLuint minlevel, GLuint numlevels, GLuint minlayer, GLuint numlayers);
    void glTrackMatrixNV(GLenum target, GLuint address, GLenum matrix, GLenum transform);
    void glTransformFeedbackAttribsNV(GLsizei count, const GLint* attribs, GLenum bufferMode);
    void glTransformFeedbackBufferBase(GLuint xfb, GLuint index, GLuint buffer);
    void glTransformFeedbackBufferRange(GLuint xfb, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size);
    void glTransformFeedbackStreamAttribsNV(GLsizei count, const GLint* attribs, GLsizei nbuffers, const GLint* bufstreams, GLenum bufferMode);
    void glTransformFeedbackVaryings(GLuint program, GLsizei count, const GLchar* const* varyings, GLenum bufferMode);
    void glTransformFeedbackVaryingsNV(GLuint program, GLsizei count, const GLint* locations, GLenum bufferMode);
    void glTransformPathNV(GLuint resultPath, GLuint srcPath, GLenum transformType, const GLfloat* transformValues);
    void glTranslated(GLdouble x, GLdouble y, GLdouble z);
    void glTranslatef(GLfloat x, GLfloat y, GLfloat z);
    void glTranslatexOES(GLfixed x, GLfixed y, GLfixed z);
    void glUniform1d(GLint location, GLdouble x);
    void glUniform1dv(GLint location, GLsizei count, const GLdouble* value);
    void glUniform1f(GLint location, GLfloat v0);
    void glUniform1fv(GLint location, GLsizei count, const GLfloat* value);
    void glUniform1i(GLint location, GLint v0);
    void glUniform1i64ARB(GLint location, GLint64 x);
    void glUniform1i64NV(GLint location, GLint64EXT x);
    void glUniform1i64vARB(GLint location, GLsizei count, const GLint64* value);
    void glUniform1i64vNV(GLint location, GLsizei count, const GLint64EXT* value);
    void glUniform1iv(GLint location, GLsizei count, const GLint* value);
    void glUniform1ui(GLint location, GLuint v0);
    void glUniform1ui64ARB(GLint location, GLuint64 x);
    void glUniform1ui64NV(GLint location, GLuint64EXT x);
    void glUniform1ui64vARB(GLint location, GLsizei count, const GLuint64* value);
    void glUniform1ui64vNV(GLint location, GLsizei count, const GLuint64EXT* value);
    void glUniform1uiv(GLint location, GLsizei count, const GLuint* value);
    void glUniform2d(GLint location, GLdouble x, GLdouble y);
    void glUniform2dv(GLint location, GLsizei count, const GLdouble* value);
    void glUniform2f(GLint location, GLfloat v0, GLfloat v1);
    void glUniform2fv(GLint location, GLsizei count, const GLfloat* value);
    void glUniform2i(GLint location, GLint v0, GLint v1);
    void glUniform2i64ARB(GLint location, GLint64 x, GLint64 y);
    void glUniform2i64NV(GLint location, GLint64EXT x, GLint64EXT y);
    void glUniform2i64vARB(GLint location, GLsizei count, const GLint64* value);
    void glUniform2i64vNV(GLint location, GLsizei count, const GLint64EXT* value);
    void glUniform2iv(GLint location, GLsizei count, const GLint* value);
    void glUniform2ui(GLint location, GLuint v0, GLuint v1);
    void glUniform2ui64ARB(GLint location, GLuint64 x, GLuint64 y);
    void glUniform2ui64NV(GLint location, GLuint64EXT x, GLuint64EXT y);
    void glUniform2ui64vARB(GLint location, GLsizei count, const GLuint64* value);
    void glUniform2ui64vNV(GLint location, GLsizei count, const GLuint64EXT* value);
    void glUniform2uiv(GLint location, GLsizei count, const GLuint* value);
    void glUniform3d(GLint location, GLdouble x, GLdouble y, GLdouble z);
    void glUniform3dv(GLint location, GLsizei count, const GLdouble* value);
    void glUniform3f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2);
    void glUniform3fv(GLint location, GLsizei count, const GLfloat* value);
    void glUniform3i(GLint location, GLint v0, GLint v1, GLint v2);
    void glUniform3i64ARB(GLint location, GLint64 x, GLint64 y, GLint64 z);
    void glUniform3i64NV(GLint location, GLint64EXT x, GLint64EXT y, GLint64EXT z);
    void glUniform3i64vARB(GLint location, GLsizei count, const GLint64* value);
    void glUniform3i64vNV(GLint location, GLsizei count, const GLint64EXT* value);
    void glUniform3iv(GLint location, GLsizei count, const GLint* value);
    void glUniform3ui(GLint location, GLuint v0, GLuint v1, GLuint v2);
    void glUniform3ui64ARB(GLint location, GLuint64 x, GLuint64 y, GLuint64 z);
    void glUniform3ui64NV(GLint location, GLuint64EXT x, GLuint64EXT y, GLuint64EXT z);
    void glUniform3ui64vARB(GLint location, GLsizei count, const GLuint64* value);
    void glUniform3ui64vNV(GLint location, GLsizei count, const GLuint64EXT* value);
    void glUniform3uiv(GLint location, GLsizei count, const GLuint* value);
    void glUniform4d(GLint location, GLdouble x, GLdouble y, GLdouble z, GLdouble w);
    void glUniform4dv(GLint location, GLsizei count, const GLdouble* value);
    void glUniform4f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3);
    void glUniform4fv(GLint location, GLsizei count, const GLfloat* value);
    void glUniform4i(GLint location, GLint v0, GLint v1, GLint v2, GLint v3);
    void glUniform4i64ARB(GLint location, GLint64 x, GLint64 y, GLint64 z, GLint64 w);
    void glUniform4i64NV(GLint location, GLint64EXT x, GLint64EXT y, GLint64EXT z, GLint64EXT w);
    void glUniform4i64vARB(GLint location, GLsizei count, const GLint64* value);
    void glUniform4i64vNV(GLint location, GLsizei count, const GLint64EXT* value);
    void glUniform4iv(GLint location, GLsizei count, const GLint* value);
    void glUniform4ui(GLint location, GLuint v0, GLuint v1, GLuint v2, GLuint v3);
    void glUniform4ui64ARB(GLint location, GLuint64 x, GLuint64 y, GLuint64 z, GLuint64 w);
    void glUniform4ui64NV(GLint location, GLuint64EXT x, GLuint64EXT y, GLuint64EXT z, GLuint64EXT w);
    void glUniform4ui64vARB(GLint location, GLsizei count, const GLuint64* value);
    void glUniform4ui64vNV(GLint location, GLsizei count, const GLuint64EXT* value);
    void glUniform4uiv(GLint location, GLsizei count, const GLuint* value);
    void glUniformBlockBinding(GLuint program, GLuint uniformBlockIndex, GLuint uniformBlockBinding);
    void glUniformBufferEXT(GLuint program, GLint location, GLuint buffer);
    void glUniformHandleui64ARB(GLint location, GLuint64 value);
    void glUniformHandleui64NV(GLint location, GLuint64 value);
    void glUniformHandleui64vARB(GLint location, GLsizei count, const GLuint64* value);
    void glUniformHandleui64vNV(GLint location, GLsizei count, const GLuint64* value);
    void glUniformMatrix2dv(GLint location, GLsizei count, GLboolean transpose, const GLdouble* value);
    void glUniformMatrix2fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value);
    void glUniformMatrix2x3dv(GLint location, GLsizei count, GLboolean transpose, const GLdouble* value);
    void glUniformMatrix2x3fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value);
    void glUniformMatrix2x4dv(GLint location, GLsizei count, GLboolean transpose, const GLdouble* value);
    void glUniformMatrix2x4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value);
    void glUniformMatrix3dv(GLint location, GLsizei count, GLboolean transpose, const GLdouble* value);
    void glUniformMatrix3fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value);
    void glUniformMatrix3x2dv(GLint location, GLsizei count, GLboolean transpose, const GLdouble* value);
    void glUniformMatrix3x2fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value);
    void glUniformMatrix3x4dv(GLint location, GLsizei count, GLboolean transpose, const GLdouble* value);
    void glUniformMatrix3x4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value);
    void glUniformMatrix4dv(GLint location, GLsizei count, GLboolean transpose, const GLdouble* value);
    void glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value);
    void glUniformMatrix4x2dv(GLint location, GLsizei count, GLboolean transpose, const GLdouble* value);
    void glUniformMatrix4x2fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value);
    void glUniformMatrix4x3dv(GLint location, GLsizei count, GLboolean transpose, const GLdouble* value);
    void glUniformMatrix4x3fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value);
    void glUniformSubroutinesuiv(GLenum shadertype, GLsizei count, const GLuint* indices);
    void glUniformui64NV(GLint location, GLuint64EXT value);
    void glUniformui64vNV(GLint location, GLsizei count, const GLuint64EXT* value);
    void glUnlockArraysEXT(void);
    GLboolean glUnmapBuffer(GLenum target);
    GLboolean glUnmapNamedBuffer(GLuint buffer);
    GLboolean glUnmapNamedBufferEXT(GLuint buffer);
    void glUnmapObjectBufferATI(GLuint buffer);
    void glUnmapTexture2DINTEL(GLuint texture, GLint level);
    void glUpdateObjectBufferATI(GLuint buffer, GLuint offset, GLsizei size, const void* pointer, GLenum preserve);
    void glUploadGpuMaskNVX(GLbitfield mask);
    void glUseProgram(GLuint program);
    void glUseProgramObjectARB(GLhandleARB programObj);
    void glUseProgramStages(GLuint pipeline, GLbitfield stages, GLuint program);
    void glUseShaderProgramEXT(GLenum type, GLuint program);
    void glVDPAUFiniNV(void);
    void glVDPAUGetSurfaceivNV(GLvdpauSurfaceNV surface, GLenum pname, GLsizei count, GLsizei* length, GLint* values);
    void glVDPAUInitNV(const void* vdpDevice, const void* getProcAddress);
    GLboolean glVDPAUIsSurfaceNV(GLvdpauSurfaceNV surface);
    void glVDPAUMapSurfacesNV(GLsizei numSurfaces, const GLvdpauSurfaceNV* surfaces);
    GLvdpauSurfaceNV glVDPAURegisterOutputSurfaceNV(const void* vdpSurface, GLenum target, GLsizei numTextureNames, const GLuint* textureNames);
    GLvdpauSurfaceNV glVDPAURegisterVideoSurfaceNV(const void* vdpSurface, GLenum target, GLsizei numTextureNames, const GLuint* textureNames);
    GLvdpauSurfaceNV glVDPAURegisterVideoSurfaceWithPictureStructureNV(const void* vdpSurface, GLenum target, GLsizei numTextureNames, const GLuint* textureNames, GLboolean isFrameStructure);
    void glVDPAUSurfaceAccessNV(GLvdpauSurfaceNV surface, GLenum access);
    void glVDPAUUnmapSurfacesNV(GLsizei numSurface, const GLvdpauSurfaceNV* surfaces);
    void glVDPAUUnregisterSurfaceNV(GLvdpauSurfaceNV surface);
    void glValidateProgram(GLuint program);
    void glValidateProgramPipeline(GLuint pipeline);
    void glVariantArrayObjectATI(GLuint id, GLenum type, GLsizei stride, GLuint buffer, GLuint offset);
    void glVariantPointerEXT(GLuint id, GLenum type, GLuint stride, const void* addr);
    void glVariantbvEXT(GLuint id, const GLbyte* addr);
    void glVariantdvEXT(GLuint id, const GLdouble* addr);
    void glVariantfvEXT(GLuint id, const GLfloat* addr);
    void glVariantivEXT(GLuint id, const GLint* addr);
    void glVariantsvEXT(GLuint id, const GLshort* addr);
    void glVariantubvEXT(GLuint id, const GLubyte* addr);
    void glVariantuivEXT(GLuint id, const GLuint* addr);
    void glVariantusvEXT(GLuint id, const GLushort* addr);
    void glVertex2bOES(GLbyte x, GLbyte y);
    void glVertex2bvOES(const GLbyte* coords);
    void glVertex2d(GLdouble x, GLdouble y);
    void glVertex2dv(const GLdouble* v);
    void glVertex2f(GLfloat x, GLfloat y);
    void glVertex2fv(const GLfloat* v);
    void glVertex2hNV(GLhalfNV x, GLhalfNV y);
    void glVertex2hvNV(const GLhalfNV* v);
    void glVertex2i(GLint x, GLint y);
    void glVertex2iv(const GLint* v);
    void glVertex2s(GLshort x, GLshort y);
    void glVertex2sv(const GLshort* v);
    void glVertex2xOES(GLfixed x);
    void glVertex2xvOES(const GLfixed* coords);
    void glVertex3bOES(GLbyte x, GLbyte y, GLbyte z);
    void glVertex3bvOES(const GLbyte* coords);
    void glVertex3d(GLdouble x, GLdouble y, GLdouble z);
    void glVertex3dv(const GLdouble* v);
    void glVertex3f(GLfloat x, GLfloat y, GLfloat z);
    void glVertex3fv(const GLfloat* v);
    void glVertex3hNV(GLhalfNV x, GLhalfNV y, GLhalfNV z);
    void glVertex3hvNV(const GLhalfNV* v);
    void glVertex3i(GLint x, GLint y, GLint z);
    void glVertex3iv(const GLint* v);
    void glVertex3s(GLshort x, GLshort y, GLshort z);
    void glVertex3sv(const GLshort* v);
    void glVertex3xOES(GLfixed x, GLfixed y);
    void glVertex3xvOES(const GLfixed* coords);
    void glVertex4bOES(GLbyte x, GLbyte y, GLbyte z, GLbyte w);
    void glVertex4bvOES(const GLbyte* coords);
    void glVertex4d(GLdouble x, GLdouble y, GLdouble z, GLdouble w);
    void glVertex4dv(const GLdouble* v);
    void glVertex4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w);
    void glVertex4fv(const GLfloat* v);
    void glVertex4hNV(GLhalfNV x, GLhalfNV y, GLhalfNV z, GLhalfNV w);
    void glVertex4hvNV(const GLhalfNV* v);
    void glVertex4i(GLint x, GLint y, GLint z, GLint w);
    void glVertex4iv(const GLint* v);
    void glVertex4s(GLshort x, GLshort y, GLshort z, GLshort w);
    void glVertex4sv(const GLshort* v);
    void glVertex4xOES(GLfixed x, GLfixed y, GLfixed z);
    void glVertex4xvOES(const GLfixed* coords);
    void glVertexArrayAttribBinding(GLuint vaobj, GLuint attribindex, GLuint bindingindex);
    void glVertexArrayAttribFormat(GLuint vaobj, GLuint attribindex, GLint size, GLenum type, GLboolean normalized, GLuint relativeoffset);
    void glVertexArrayAttribIFormat(GLuint vaobj, GLuint attribindex, GLint size, GLenum type, GLuint relativeoffset);
    void glVertexArrayAttribLFormat(GLuint vaobj, GLuint attribindex, GLint size, GLenum type, GLuint relativeoffset);
    void glVertexArrayBindVertexBufferEXT(GLuint vaobj, GLuint bindingindex, GLuint buffer, GLintptr offset, GLsizei stride);
    void glVertexArrayBindingDivisor(GLuint vaobj, GLuint bindingindex, GLuint divisor);
    void glVertexArrayColorOffsetEXT(GLuint vaobj, GLuint buffer, GLint size, GLenum type, GLsizei stride, GLintptr offset);
    void glVertexArrayEdgeFlagOffsetEXT(GLuint vaobj, GLuint buffer, GLsizei stride, GLintptr offset);
    void glVertexArrayElementBuffer(GLuint vaobj, GLuint buffer);
    void glVertexArrayFogCoordOffsetEXT(GLuint vaobj, GLuint buffer, GLenum type, GLsizei stride, GLintptr offset);
    void glVertexArrayIndexOffsetEXT(GLuint vaobj, GLuint buffer, GLenum type, GLsizei stride, GLintptr offset);
    void glVertexArrayMultiTexCoordOffsetEXT(GLuint vaobj, GLuint buffer, GLenum texunit, GLint size, GLenum type, GLsizei stride, GLintptr offset);
    void glVertexArrayNormalOffsetEXT(GLuint vaobj, GLuint buffer, GLenum type, GLsizei stride, GLintptr offset);
    void glVertexArrayParameteriAPPLE(GLenum pname, GLint param);
    void glVertexArrayRangeAPPLE(GLsizei length, void* pointer);
    void glVertexArrayRangeNV(GLsizei length, const void* pointer);
    void glVertexArraySecondaryColorOffsetEXT(GLuint vaobj, GLuint buffer, GLint size, GLenum type, GLsizei stride, GLintptr offset);
    void glVertexArrayTexCoordOffsetEXT(GLuint vaobj, GLuint buffer, GLint size, GLenum type, GLsizei stride, GLintptr offset);
    void glVertexArrayVertexAttribBindingEXT(GLuint vaobj, GLuint attribindex, GLuint bindingindex);
    void glVertexArrayVertexAttribDivisorEXT(GLuint vaobj, GLuint index, GLuint divisor);
    void glVertexArrayVertexAttribFormatEXT(GLuint vaobj, GLuint attribindex, GLint size, GLenum type, GLboolean normalized, GLuint relativeoffset);
    void glVertexArrayVertexAttribIFormatEXT(GLuint vaobj, GLuint attribindex, GLint size, GLenum type, GLuint relativeoffset);
    void glVertexArrayVertexAttribIOffsetEXT(GLuint vaobj, GLuint buffer, GLuint index, GLint size, GLenum type, GLsizei stride, GLintptr offset);
    void glVertexArrayVertexAttribLFormatEXT(GLuint vaobj, GLuint attribindex, GLint size, GLenum type, GLuint relativeoffset);
    void glVertexArrayVertexAttribLOffsetEXT(GLuint vaobj, GLuint buffer, GLuint index, GLint size, GLenum type, GLsizei stride, GLintptr offset);
    void glVertexArrayVertexAttribOffsetEXT(GLuint vaobj, GLuint buffer, GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, GLintptr offset);
    void glVertexArrayVertexBindingDivisorEXT(GLuint vaobj, GLuint bindingindex, GLuint divisor);
    void glVertexArrayVertexBuffer(GLuint vaobj, GLuint bindingindex, GLuint buffer, GLintptr offset, GLsizei stride);
    void glVertexArrayVertexBuffers(GLuint vaobj, GLuint first, GLsizei count, const GLuint* buffers, const GLintptr* offsets, const GLsizei* strides);
    void glVertexArrayVertexOffsetEXT(GLuint vaobj, GLuint buffer, GLint size, GLenum type, GLsizei stride, GLintptr offset);
    void glVertexAttrib1d(GLuint index, GLdouble x);
    void glVertexAttrib1dARB(GLuint index, GLdouble x);
    void glVertexAttrib1dNV(GLuint index, GLdouble x);
    void glVertexAttrib1dv(GLuint index, const GLdouble* v);
    void glVertexAttrib1dvARB(GLuint index, const GLdouble* v);
    void glVertexAttrib1dvNV(GLuint index, const GLdouble* v);
    void glVertexAttrib1f(GLuint index, GLfloat x);
    void glVertexAttrib1fNV(GLuint index, GLfloat x);
    void glVertexAttrib1fv(GLuint index, const GLfloat* v);
    void glVertexAttrib1fvNV(GLuint index, const GLfloat* v);
    void glVertexAttrib1hNV(GLuint index, GLhalfNV x);
    void glVertexAttrib1hvNV(GLuint index, const GLhalfNV* v);
    void glVertexAttrib1s(GLuint index, GLshort x);
    void glVertexAttrib1sARB(GLuint index, GLshort x);
    void glVertexAttrib1sNV(GLuint index, GLshort x);
    void glVertexAttrib1sv(GLuint index, const GLshort* v);
    void glVertexAttrib1svARB(GLuint index, const GLshort* v);
    void glVertexAttrib1svNV(GLuint index, const GLshort* v);
    void glVertexAttrib2d(GLuint index, GLdouble x, GLdouble y);
    void glVertexAttrib2dARB(GLuint index, GLdouble x, GLdouble y);
    void glVertexAttrib2dNV(GLuint index, GLdouble x, GLdouble y);
    void glVertexAttrib2dv(GLuint index, const GLdouble* v);
    void glVertexAttrib2dvARB(GLuint index, const GLdouble* v);
    void glVertexAttrib2dvNV(GLuint index, const GLdouble* v);
    void glVertexAttrib2f(GLuint index, GLfloat x, GLfloat y);
    void glVertexAttrib2fNV(GLuint index, GLfloat x, GLfloat y);
    void glVertexAttrib2fv(GLuint index, const GLfloat* v);
    void glVertexAttrib2fvNV(GLuint index, const GLfloat* v);
    void glVertexAttrib2hNV(GLuint index, GLhalfNV x, GLhalfNV y);
    void glVertexAttrib2hvNV(GLuint index, const GLhalfNV* v);
    void glVertexAttrib2s(GLuint index, GLshort x, GLshort y);
    void glVertexAttrib2sARB(GLuint index, GLshort x, GLshort y);
    void glVertexAttrib2sNV(GLuint index, GLshort x, GLshort y);
    void glVertexAttrib2sv(GLuint index, const GLshort* v);
    void glVertexAttrib2svARB(GLuint index, const GLshort* v);
    void glVertexAttrib2svNV(GLuint index, const GLshort* v);
    void glVertexAttrib3d(GLuint index, GLdouble x, GLdouble y, GLdouble z);
    void glVertexAttrib3dARB(GLuint index, GLdouble x, GLdouble y, GLdouble z);
    void glVertexAttrib3dNV(GLuint index, GLdouble x, GLdouble y, GLdouble z);
    void glVertexAttrib3dv(GLuint index, const GLdouble* v);
    void glVertexAttrib3dvARB(GLuint index, const GLdouble* v);
    void glVertexAttrib3dvNV(GLuint index, const GLdouble* v);
    void glVertexAttrib3f(GLuint index, GLfloat x, GLfloat y, GLfloat z);
    void glVertexAttrib3fNV(GLuint index, GLfloat x, GLfloat y, GLfloat z);
    void glVertexAttrib3fv(GLuint index, const GLfloat* v);
    void glVertexAttrib3fvNV(GLuint index, const GLfloat* v);
    void glVertexAttrib3hNV(GLuint index, GLhalfNV x, GLhalfNV y, GLhalfNV z);
    void glVertexAttrib3hvNV(GLuint index, const GLhalfNV* v);
    void glVertexAttrib3s(GLuint index, GLshort x, GLshort y, GLshort z);
    void glVertexAttrib3sARB(GLuint index, GLshort x, GLshort y, GLshort z);
    void glVertexAttrib3sNV(GLuint index, GLshort x, GLshort y, GLshort z);
    void glVertexAttrib3sv(GLuint index, const GLshort* v);
    void glVertexAttrib3svARB(GLuint index, const GLshort* v);
    void glVertexAttrib3svNV(GLuint index, const GLshort* v);
    void glVertexAttrib4Nbv(GLuint index, const GLbyte* v);
    void glVertexAttrib4NbvARB(GLuint index, const GLbyte* v);
    void glVertexAttrib4Niv(GLuint index, const GLint* v);
    void glVertexAttrib4NivARB(GLuint index, const GLint* v);
    void glVertexAttrib4Nsv(GLuint index, const GLshort* v);
    void glVertexAttrib4NsvARB(GLuint index, const GLshort* v);
    void glVertexAttrib4Nub(GLuint index, GLubyte x, GLubyte y, GLubyte z, GLubyte w);
    void glVertexAttrib4NubARB(GLuint index, GLubyte x, GLubyte y, GLubyte z, GLubyte w);
    void glVertexAttrib4Nubv(GLuint index, const GLubyte* v);
    void glVertexAttrib4NubvARB(GLuint index, const GLubyte* v);
    void glVertexAttrib4Nuiv(GLuint index, const GLuint* v);
    void glVertexAttrib4NuivARB(GLuint index, const GLuint* v);
    void glVertexAttrib4Nusv(GLuint index, const GLushort* v);
    void glVertexAttrib4NusvARB(GLuint index, const GLushort* v);
    void glVertexAttrib4bv(GLuint index, const GLbyte* v);
    void glVertexAttrib4bvARB(GLuint index, const GLbyte* v);
    void glVertexAttrib4d(GLuint index, GLdouble x, GLdouble y, GLdouble z, GLdouble w);
    void glVertexAttrib4dARB(GLuint index, GLdouble x, GLdouble y, GLdouble z, GLdouble w);
    void glVertexAttrib4dNV(GLuint index, GLdouble x, GLdouble y, GLdouble z, GLdouble w);
    void glVertexAttrib4dv(GLuint index, const GLdouble* v);
    void glVertexAttrib4dvARB(GLuint index, const GLdouble* v);
    void glVertexAttrib4dvNV(GLuint index, const GLdouble* v);
    void glVertexAttrib4f(GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
    void glVertexAttrib4fNV(GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
    void glVertexAttrib4fv(GLuint index, const GLfloat* v);
    void glVertexAttrib4fvNV(GLuint index, const GLfloat* v);
    void glVertexAttrib4hNV(GLuint index, GLhalfNV x, GLhalfNV y, GLhalfNV z, GLhalfNV w);
    void glVertexAttrib4hvNV(GLuint index, const GLhalfNV* v);
    void glVertexAttrib4iv(GLuint index, const GLint* v);
    void glVertexAttrib4ivARB(GLuint index, const GLint* v);
    void glVertexAttrib4s(GLuint index, GLshort x, GLshort y, GLshort z, GLshort w);
    void glVertexAttrib4sARB(GLuint index, GLshort x, GLshort y, GLshort z, GLshort w);
    void glVertexAttrib4sNV(GLuint index, GLshort x, GLshort y, GLshort z, GLshort w);
    void glVertexAttrib4sv(GLuint index, const GLshort* v);
    void glVertexAttrib4svARB(GLuint index, const GLshort* v);
    void glVertexAttrib4svNV(GLuint index, const GLshort* v);
    void glVertexAttrib4ubNV(GLuint index, GLubyte x, GLubyte y, GLubyte z, GLubyte w);
    void glVertexAttrib4ubv(GLuint index, const GLubyte* v);
    void glVertexAttrib4ubvARB(GLuint index, const GLubyte* v);
    void glVertexAttrib4ubvNV(GLuint index, const GLubyte* v);
    void glVertexAttrib4uiv(GLuint index, const GLuint* v);
    void glVertexAttrib4uivARB(GLuint index, const GLuint* v);
    void glVertexAttrib4usv(GLuint index, const GLushort* v);
    void glVertexAttrib4usvARB(GLuint index, const GLushort* v);
    void glVertexAttribArrayObjectATI(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, GLuint buffer, GLuint offset);
    void glVertexAttribBinding(GLuint attribindex, GLuint bindingindex);
    void glVertexAttribDivisor(GLuint index, GLuint divisor);
    void glVertexAttribFormat(GLuint attribindex, GLint size, GLenum type, GLboolean normalized, GLuint relativeoffset);
    void glVertexAttribFormatNV(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride);
    void glVertexAttribI1i(GLuint index, GLint x);
    void glVertexAttribI1iEXT(GLuint index, GLint x);
    void glVertexAttribI1iv(GLuint index, const GLint* v);
    void glVertexAttribI1ivEXT(GLuint index, const GLint* v);
    void glVertexAttribI1ui(GLuint index, GLuint x);
    void glVertexAttribI1uiEXT(GLuint index, GLuint x);
    void glVertexAttribI1uiv(GLuint index, const GLuint* v);
    void glVertexAttribI1uivEXT(GLuint index, const GLuint* v);
    void glVertexAttribI2i(GLuint index, GLint x, GLint y);
    void glVertexAttribI2iEXT(GLuint index, GLint x, GLint y);
    void glVertexAttribI2iv(GLuint index, const GLint* v);
    void glVertexAttribI2ivEXT(GLuint index, const GLint* v);
    void glVertexAttribI2ui(GLuint index, GLuint x, GLuint y);
    void glVertexAttribI2uiEXT(GLuint index, GLuint x, GLuint y);
    void glVertexAttribI2uiv(GLuint index, const GLuint* v);
    void glVertexAttribI2uivEXT(GLuint index, const GLuint* v);
    void glVertexAttribI3i(GLuint index, GLint x, GLint y, GLint z);
    void glVertexAttribI3iEXT(GLuint index, GLint x, GLint y, GLint z);
    void glVertexAttribI3iv(GLuint index, const GLint* v);
    void glVertexAttribI3ivEXT(GLuint index, const GLint* v);
    void glVertexAttribI3ui(GLuint index, GLuint x, GLuint y, GLuint z);
    void glVertexAttribI3uiEXT(GLuint index, GLuint x, GLuint y, GLuint z);
    void glVertexAttribI3uiv(GLuint index, const GLuint* v);
    void glVertexAttribI3uivEXT(GLuint index, const GLuint* v);
    void glVertexAttribI4bv(GLuint index, const GLbyte* v);
    void glVertexAttribI4bvEXT(GLuint index, const GLbyte* v);
    void glVertexAttribI4i(GLuint index, GLint x, GLint y, GLint z, GLint w);
    void glVertexAttribI4iv(GLuint index, const GLint* v);
    void glVertexAttribI4sv(GLuint index, const GLshort* v);
    void glVertexAttribI4svEXT(GLuint index, const GLshort* v);
    void glVertexAttribI4ubv(GLuint index, const GLubyte* v);
    void glVertexAttribI4ubvEXT(GLuint index, const GLubyte* v);
    void glVertexAttribI4ui(GLuint index, GLuint x, GLuint y, GLuint z, GLuint w);
    void glVertexAttribI4uiv(GLuint index, const GLuint* v);
    void glVertexAttribI4usv(GLuint index, const GLushort* v);
    void glVertexAttribI4usvEXT(GLuint index, const GLushort* v);
    void glVertexAttribIFormat(GLuint attribindex, GLint size, GLenum type, GLuint relativeoffset);
    void glVertexAttribIFormatNV(GLuint index, GLint size, GLenum type, GLsizei stride);
    void glVertexAttribIPointer(GLuint index, GLint size, GLenum type, GLsizei stride, const void* pointer);
    void glVertexAttribL1d(GLuint index, GLdouble x);
    void glVertexAttribL1dEXT(GLuint index, GLdouble x);
    void glVertexAttribL1dv(GLuint index, const GLdouble* v);
    void glVertexAttribL1dvEXT(GLuint index, const GLdouble* v);
    void glVertexAttribL1i64NV(GLuint index, GLint64EXT x);
    void glVertexAttribL1i64vNV(GLuint index, const GLint64EXT* v);
    void glVertexAttribL1ui64ARB(GLuint index, GLuint64EXT x);
    void glVertexAttribL1ui64NV(GLuint index, GLuint64EXT x);
    void glVertexAttribL1ui64vARB(GLuint index, const GLuint64EXT* v);
    void glVertexAttribL1ui64vNV(GLuint index, const GLuint64EXT* v);
    void glVertexAttribL2d(GLuint index, GLdouble x, GLdouble y);
    void glVertexAttribL2dEXT(GLuint index, GLdouble x, GLdouble y);
    void glVertexAttribL2dv(GLuint index, const GLdouble* v);
    void glVertexAttribL2dvEXT(GLuint index, const GLdouble* v);
    void glVertexAttribL2i64NV(GLuint index, GLint64EXT x, GLint64EXT y);
    void glVertexAttribL2i64vNV(GLuint index, const GLint64EXT* v);
    void glVertexAttribL2ui64NV(GLuint index, GLuint64EXT x, GLuint64EXT y);
    void glVertexAttribL2ui64vNV(GLuint index, const GLuint64EXT* v);
    void glVertexAttribL3d(GLuint index, GLdouble x, GLdouble y, GLdouble z);
    void glVertexAttribL3dEXT(GLuint index, GLdouble x, GLdouble y, GLdouble z);
    void glVertexAttribL3dv(GLuint index, const GLdouble* v);
    void glVertexAttribL3dvEXT(GLuint index, const GLdouble* v);
    void glVertexAttribL3i64NV(GLuint index, GLint64EXT x, GLint64EXT y, GLint64EXT z);
    void glVertexAttribL3i64vNV(GLuint index, const GLint64EXT* v);
    void glVertexAttribL3ui64NV(GLuint index, GLuint64EXT x, GLuint64EXT y, GLuint64EXT z);
    void glVertexAttribL3ui64vNV(GLuint index, const GLuint64EXT* v);
    void glVertexAttribL4d(GLuint index, GLdouble x, GLdouble y, GLdouble z, GLdouble w);
    void glVertexAttribL4dEXT(GLuint index, GLdouble x, GLdouble y, GLdouble z, GLdouble w);
    void glVertexAttribL4dv(GLuint index, const GLdouble* v);
    void glVertexAttribL4dvEXT(GLuint index, const GLdouble* v);
    void glVertexAttribL4i64NV(GLuint index, GLint64EXT x, GLint64EXT y, GLint64EXT z, GLint64EXT w);
    void glVertexAttribL4i64vNV(GLuint index, const GLint64EXT* v);
    void glVertexAttribL4ui64NV(GLuint index, GLuint64EXT x, GLuint64EXT y, GLuint64EXT z, GLuint64EXT w);
    void glVertexAttribL4ui64vNV(GLuint index, const GLuint64EXT* v);
    void glVertexAttribLFormat(GLuint attribindex, GLint size, GLenum type, GLuint relativeoffset);
    void glVertexAttribLFormatNV(GLuint index, GLint size, GLenum type, GLsizei stride);
    void glVertexAttribLPointer(GLuint index, GLint size, GLenum type, GLsizei stride, const void* pointer);
    void glVertexAttribLPointerEXT(GLuint index, GLint size, GLenum type, GLsizei stride, const void* pointer);
    void glVertexAttribP1ui(GLuint index, GLenum type, GLboolean normalized, GLuint value);
    void glVertexAttribP1uiv(GLuint index, GLenum type, GLboolean normalized, const GLuint* value);
    void glVertexAttribP2ui(GLuint index, GLenum type, GLboolean normalized, GLuint value);
    void glVertexAttribP2uiv(GLuint index, GLenum type, GLboolean normalized, const GLuint* value);
    void glVertexAttribP3ui(GLuint index, GLenum type, GLboolean normalized, GLuint value);
    void glVertexAttribP3uiv(GLuint index, GLenum type, GLboolean normalized, const GLuint* value);
    void glVertexAttribP4ui(GLuint index, GLenum type, GLboolean normalized, GLuint value);
    void glVertexAttribP4uiv(GLuint index, GLenum type, GLboolean normalized, const GLuint* value);
    void glVertexAttribParameteriAMD(GLuint index, GLenum pname, GLint param);
    void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer);
    void glVertexAttribPointerNV(GLuint index, GLint fsize, GLenum type, GLsizei stride, const void* pointer);
    void glVertexAttribs1dvNV(GLuint index, GLsizei count, const GLdouble* v);
    void glVertexAttribs1fvNV(GLuint index, GLsizei count, const GLfloat* v);
    void glVertexAttribs1hvNV(GLuint index, GLsizei n, const GLhalfNV* v);
    void glVertexAttribs1svNV(GLuint index, GLsizei count, const GLshort* v);
    void glVertexAttribs2dvNV(GLuint index, GLsizei count, const GLdouble* v);
    void glVertexAttribs2fvNV(GLuint index, GLsizei count, const GLfloat* v);
    void glVertexAttribs2hvNV(GLuint index, GLsizei n, const GLhalfNV* v);
    void glVertexAttribs2svNV(GLuint index, GLsizei count, const GLshort* v);
    void glVertexAttribs3dvNV(GLuint index, GLsizei count, const GLdouble* v);
    void glVertexAttribs3fvNV(GLuint index, GLsizei count, const GLfloat* v);
    void glVertexAttribs3hvNV(GLuint index, GLsizei n, const GLhalfNV* v);
    void glVertexAttribs3svNV(GLuint index, GLsizei count, const GLshort* v);
    void glVertexAttribs4dvNV(GLuint index, GLsizei count, const GLdouble* v);
    void glVertexAttribs4fvNV(GLuint index, GLsizei count, const GLfloat* v);
    void glVertexAttribs4hvNV(GLuint index, GLsizei n, const GLhalfNV* v);
    void glVertexAttribs4svNV(GLuint index, GLsizei count, const GLshort* v);
    void glVertexAttribs4ubvNV(GLuint index, GLsizei count, const GLubyte* v);
    void glVertexBindingDivisor(GLuint bindingindex, GLuint divisor);
    void glVertexBlendARB(GLint count);
    void glVertexBlendEnvfATI(GLenum pname, GLfloat param);
    void glVertexBlendEnviATI(GLenum pname, GLint param);
    void glVertexFormatNV(GLint size, GLenum type, GLsizei stride);
    void glVertexP2ui(GLenum type, GLuint value);
    void glVertexP2uiv(GLenum type, const GLuint* value);
    void glVertexP3ui(GLenum type, GLuint value);
    void glVertexP3uiv(GLenum type, const GLuint* value);
    void glVertexP4ui(GLenum type, GLuint value);
    void glVertexP4uiv(GLenum type, const GLuint* value);
    void glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* ptr);
    void glVertexPointerEXT(GLint size, GLenum type, GLsizei stride, GLsizei count, const void* pointer);
    void glVertexPointerListIBM(GLint size, GLenum type, GLint stride, const void** pointer, GLint ptrstride);
    void glVertexPointervINTEL(GLint size, GLenum type, const void** pointer);
    void glVertexStream1dATI(GLenum stream, GLdouble x);
    void glVertexStream1dvATI(GLenum stream, const GLdouble* coords);
    void glVertexStream1fATI(GLenum stream, GLfloat x);
    void glVertexStream1fvATI(GLenum stream, const GLfloat* coords);
    void glVertexStream1iATI(GLenum stream, GLint x);
    void glVertexStream1ivATI(GLenum stream, const GLint* coords);
    void glVertexStream1sATI(GLenum stream, GLshort x);
    void glVertexStream1svATI(GLenum stream, const GLshort* coords);
    void glVertexStream2dATI(GLenum stream, GLdouble x, GLdouble y);
    void glVertexStream2dvATI(GLenum stream, const GLdouble* coords);
    void glVertexStream2fATI(GLenum stream, GLfloat x, GLfloat y);
    void glVertexStream2fvATI(GLenum stream, const GLfloat* coords);
    void glVertexStream2iATI(GLenum stream, GLint x, GLint y);
    void glVertexStream2ivATI(GLenum stream, const GLint* coords);
    void glVertexStream2sATI(GLenum stream, GLshort x, GLshort y);
    void glVertexStream2svATI(GLenum stream, const GLshort* coords);
    void glVertexStream3dATI(GLenum stream, GLdouble x, GLdouble y, GLdouble z);
    void glVertexStream3dvATI(GLenum stream, const GLdouble* coords);
    void glVertexStream3fATI(GLenum stream, GLfloat x, GLfloat y, GLfloat z);
    void glVertexStream3fvATI(GLenum stream, const GLfloat* coords);
    void glVertexStream3iATI(GLenum stream, GLint x, GLint y, GLint z);
    void glVertexStream3ivATI(GLenum stream, const GLint* coords);
    void glVertexStream3sATI(GLenum stream, GLshort x, GLshort y, GLshort z);
    void glVertexStream3svATI(GLenum stream, const GLshort* coords);
    void glVertexStream4dATI(GLenum stream, GLdouble x, GLdouble y, GLdouble z, GLdouble w);
    void glVertexStream4dvATI(GLenum stream, const GLdouble* coords);
    void glVertexStream4fATI(GLenum stream, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
    void glVertexStream4fvATI(GLenum stream, const GLfloat* coords);
    void glVertexStream4iATI(GLenum stream, GLint x, GLint y, GLint z, GLint w);
    void glVertexStream4ivATI(GLenum stream, const GLint* coords);
    void glVertexStream4sATI(GLenum stream, GLshort x, GLshort y, GLshort z, GLshort w);
    void glVertexStream4svATI(GLenum stream, const GLshort* coords);
    void glVertexWeightPointerEXT(GLint size, GLenum type, GLsizei stride, const void* pointer);
    void glVertexWeightfEXT(GLfloat weight);
    void glVertexWeightfvEXT(const GLfloat* weight);
    void glVertexWeighthNV(GLhalfNV weight);
    void glVertexWeighthvNV(const GLhalfNV* weight);
    GLenum glVideoCaptureNV(GLuint video_capture_slot, GLuint* sequence_num, GLuint64EXT* capture_time);
    void glVideoCaptureStreamParameterdvNV(GLuint video_capture_slot, GLuint stream, GLenum pname, const GLdouble* params);
    void glVideoCaptureStreamParameterfvNV(GLuint video_capture_slot, GLuint stream, GLenum pname, const GLfloat* params);
    void glVideoCaptureStreamParameterivNV(GLuint video_capture_slot, GLuint stream, GLenum pname, const GLint* params);
    void glViewport(GLint x, GLint y, GLsizei width, GLsizei height);
    void glViewportArrayv(GLuint first, GLsizei count, const GLfloat* v);
    void glViewportIndexedf(GLuint index, GLfloat x, GLfloat y, GLfloat w, GLfloat h);
    void glViewportIndexedfv(GLuint index, const GLfloat* v);
    void glViewportPositionWScaleNV(GLuint index, GLfloat xcoeff, GLfloat ycoeff);
    void glViewportSwizzleNV(GLuint index, GLenum swizzlex, GLenum swizzley, GLenum swizzlez, GLenum swizzlew);
    void glWaitSemaphoreEXT(GLuint semaphore, GLuint numBufferBarriers, const GLuint* buffers, GLuint numTextureBarriers, const GLuint* textures, const GLenum* srcLayouts);
    void glWaitSemaphoreui64NVX(GLuint waitGpu, GLsizei fenceObjectCount, const GLuint* semaphoreArray, const GLuint64* fenceValueArray);
    void glWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout);
    void glWaitVkSemaphoreNV(GLuint64 vkSemaphore);
    void glWeightPathsNV(GLuint resultPath, GLsizei numPaths, const GLuint* paths, const GLfloat* weights);
    void glWeightPointerARB(GLint size, GLenum type, GLsizei stride, const void* pointer);
    void glWeightbvARB(GLint size, const GLbyte* weights);
    void glWeightdvARB(GLint size, const GLdouble* weights);
    void glWeightfvARB(GLint size, const GLfloat* weights);
    void glWeightivARB(GLint size, const GLint* weights);
    void glWeightsvARB(GLint size, const GLshort* weights);
    void glWeightubvARB(GLint size, const GLubyte* weights);
    void glWeightuivARB(GLint size, const GLuint* weights);
    void glWeightusvARB(GLint size, const GLushort* weights);
    void glWindowPos2d(GLdouble x, GLdouble y);
    void glWindowPos2dARB(GLdouble x, GLdouble y);
    void glWindowPos2dMESA(GLdouble x, GLdouble y);
    void glWindowPos2dv(const GLdouble* v);
    void glWindowPos2dvARB(const GLdouble* v);
    void glWindowPos2dvMESA(const GLdouble* v);
    void glWindowPos2f(GLfloat x, GLfloat y);
    void glWindowPos2fARB(GLfloat x, GLfloat y);
    void glWindowPos2fMESA(GLfloat x, GLfloat y);
    void glWindowPos2fv(const GLfloat* v);
    void glWindowPos2fvARB(const GLfloat* v);
    void glWindowPos2fvMESA(const GLfloat* v);
    void glWindowPos2i(GLint x, GLint y);
    void glWindowPos2iARB(GLint x, GLint y);
    void glWindowPos2iMESA(GLint x, GLint y);
    void glWindowPos2iv(const GLint* v);
    void glWindowPos2ivARB(const GLint* v);
    void glWindowPos2ivMESA(const GLint* v);
    void glWindowPos2s(GLshort x, GLshort y);
    void glWindowPos2sARB(GLshort x, GLshort y);
    void glWindowPos2sMESA(GLshort x, GLshort y);
    void glWindowPos2sv(const GLshort* v);
    void glWindowPos2svARB(const GLshort* v);
    void glWindowPos2svMESA(const GLshort* v);
    void glWindowPos3d(GLdouble x, GLdouble y, GLdouble z);
    void glWindowPos3dARB(GLdouble x, GLdouble y, GLdouble z);
    void glWindowPos3dMESA(GLdouble x, GLdouble y, GLdouble z);
    void glWindowPos3dv(const GLdouble* v);
    void glWindowPos3dvARB(const GLdouble* v);
    void glWindowPos3dvMESA(const GLdouble* v);
    void glWindowPos3f(GLfloat x, GLfloat y, GLfloat z);
    void glWindowPos3fARB(GLfloat x, GLfloat y, GLfloat z);
    void glWindowPos3fMESA(GLfloat x, GLfloat y, GLfloat z);
    void glWindowPos3fv(const GLfloat* v);
    void glWindowPos3fvARB(const GLfloat* v);
    void glWindowPos3fvMESA(const GLfloat* v);
    void glWindowPos3i(GLint x, GLint y, GLint z);
    void glWindowPos3iARB(GLint x, GLint y, GLint z);
    void glWindowPos3iMESA(GLint x, GLint y, GLint z);
    void glWindowPos3iv(const GLint* v);
    void glWindowPos3ivARB(const GLint* v);
    void glWindowPos3ivMESA(const GLint* v);
    void glWindowPos3s(GLshort x, GLshort y, GLshort z);
    void glWindowPos3sARB(GLshort x, GLshort y, GLshort z);
    void glWindowPos3sMESA(GLshort x, GLshort y, GLshort z);
    void glWindowPos3sv(const GLshort* v);
    void glWindowPos3svARB(const GLshort* v);
    void glWindowPos3svMESA(const GLshort* v);
    void glWindowPos4dMESA(GLdouble x, GLdouble y, GLdouble z, GLdouble w);
    void glWindowPos4dvMESA(const GLdouble* v);
    void glWindowPos4fMESA(GLfloat x, GLfloat y, GLfloat z, GLfloat w);
    void glWindowPos4fvMESA(const GLfloat* v);
    void glWindowPos4iMESA(GLint x, GLint y, GLint z, GLint w);
    void glWindowPos4ivMESA(const GLint* v);
    void glWindowPos4sMESA(GLshort x, GLshort y, GLshort z, GLshort w);
    void glWindowPos4svMESA(const GLshort* v);
    void glWindowRectanesEXT(GLenum mode, GLsizei count, const GLint* box);
    void glWriteMaskEXT(GLuint res, GLuint in, GLenum outX, GLenum outY, GLenum outZ, GLenum outW);
    EGLBoolean eglBindAPI(EGLenum api);
    EGLBoolean eglBindTexImage(EGLDisplay dpy, EGLSurface surface, EGLint buffer);
    EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint* attrib_list, EGLConfig* configs, EGLint config_size, EGLint* num_config);
    EGLint eglClientWaitSync(EGLDisplay dpy, EGLSync sync, EGLint flags, EGLTime timeout);
    EGLBoolean eglCopyBuffers(EGLDisplay dpy, EGLSurface surface, EGLNativePixmapType target);
    EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext shareCtx, const EGLint* attrib_list);
    EGLImage eglCreateImage(EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer, const EGLAttrib* attrib_list);
    EGLSurface eglCreatePbufferFromClientBuffer(EGLDisplay dpy, EGLenum buftype, EGLClientBuffer buffer, EGLConfig config, const EGLint* attrib_list);
    EGLSurface eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config, const EGLint* attrib_list);
    EGLSurface eglCreatePixmapSurface(EGLDisplay dpy, EGLConfig config, EGLNativePixmapType pixmap, const EGLint* attrib_list);
    EGLSurface eglCreatePlatformPixmapSurface(EGLDisplay dpy, EGLConfig config, void* native_pixmap, const EGLAttrib* attrib_list);
    EGLSurface eglCreatePlatformWindowSurface(EGLDisplay dpy, EGLConfig config, void* native_window, const EGLAttrib* attrib_list);
    EGLSync eglCreateSync(EGLDisplay dpy, EGLenum type, const EGLAttrib* attrib_list);
    EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, NativeWindowType window, const EGLint* attrib_list);
    EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx);
    EGLBoolean eglDestroyImage(EGLDisplay dpy, EGLImage image);
    EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface surface);
    EGLBoolean eglDestroySync(EGLDisplay dpy, EGLSync sync);
    EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint* value);
    EGLBoolean eglGetConfigs(EGLDisplay dpy, EGLConfig* configs, EGLint config_size, EGLint* num_config);
    EGLContext eglGetCurrentContext(void);
    EGLDisplay eglGetCurrentDisplay(void);
    EGLSurface eglGetCurrentSurface(EGLint readdraw);
    EGLDisplay eglGetDisplay(NativeDisplayType display);
    EGLint eglGetError();
    EGLDisplay eglGetPlatformDisplay(EGLenum platform, void* native_display, const EGLAttrib* attrib_list);
    EGLDisplay eglGetPlatformDisplayEXT(EGLenum platform, void* native_display, const EGLint* attrib_list);
    __eglMustCastToProperFunctionPointerType eglGetProcAddress(const char* name);
    EGLBoolean eglGetSyncAttrib(EGLDisplay dpy, EGLSync sync, EGLint attribute, EGLAttrib* value);
    EGLBoolean eglInitialize(EGLDisplay dpy, EGLint* major, EGLint* minor);
    EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx);
    EGLenum eglQueryAPI(void);
    EGLBoolean eglQueryContext(EGLDisplay dpy, EGLContext ctx, EGLint attribute, EGLint* value);
    char const* eglQueryString(EGLDisplay display, EGLint name);
    EGLBoolean eglQuerySurface(EGLDisplay display, EGLSurface surface, EGLint attribute, EGLint* value);
    EGLBoolean eglReleaseTexImage(EGLDisplay dpy, EGLSurface surface, EGLint buffer);
    EGLBoolean eglReleaseThread(void);
    EGLBoolean eglSurfaceAttrib(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint value);
    EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface draw);
    EGLBoolean eglSwapInterval(EGLDisplay dpy, EGLint interval);
    EGLBoolean eglTerminate(EGLDisplay dpy);
    EGLBoolean eglWaitClient(void);
    EGLBoolean eglWaitGL(void);
    EGLBoolean eglWaitNative(EGLint engine);
    EGLBoolean eglWaitSync(EGLDisplay dpy, EGLSync sync, EGLint flags);
}

namespace {
    const void* ResolveShm(const MobileGL::Protocol::WireFull::ShmRegion* region,
                           const void* const* receivedShm, uint32_t receivedShmCount) {
        if (region == nullptr || receivedShm == nullptr || receivedShmCount == 0) {
            return nullptr;
        }
        const auto* base = static_cast<const uint8_t*>(receivedShm[0]);
        return base + region->offset();
    }
} // namespace

namespace MobileGL::Protocol::Wire {

    namespace { thread_local bool s_returnValid = false; thread_local int64_t s_returnI64 = 0; thread_local std::vector<uint8_t> s_returnBytes; }
    void WireDispatchResetReturn() { s_returnValid = false; s_returnI64 = 0; s_returnBytes.clear(); }
    bool WireDispatchReturnValid() { return s_returnValid; }
    int64_t WireDispatchReturnI64() { return s_returnI64; }
    void WireDispatchStoreI64(int64_t value) { s_returnI64 = value; s_returnValid = true; }
    void WireDispatchStoreU64(uint64_t value) { s_returnI64 = static_cast<int64_t>(value); s_returnValid = true; }
    void WireDispatchStoreBytes(const uint8_t* data, uint32_t size) {
        if (data != nullptr && size > 0) { s_returnBytes.assign(data, data + size); }
    }
    const uint8_t* WireDispatchBytes() {
        return s_returnBytes.empty() ? nullptr : s_returnBytes.data();
    }
    uint32_t WireDispatchBytesSize() {
        return static_cast<uint32_t>(s_returnBytes.size());
    }

    uint32_t WireDispatchCall(uint32_t opcode, uint32_t sessionId,
                               const void* payloadBytes, uint64_t payloadSize,
                               const void* const* receivedShm, uint32_t receivedShmCount,
                               uint32_t outCapacity) {
        (void)sessionId;
        WireDispatchResetReturn();
        if (payloadBytes == nullptr || payloadSize < 4) {
            return 1u;
        }
        switch (opcode) {

        case 1: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEglBindAPI>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::eglBindAPI(static_cast<EGLenum>(p->api()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 2: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEglBindTexImage>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::eglBindTexImage(reinterpret_cast<EGLDisplay>(static_cast<std::uintptr_t>(p->dpy())), reinterpret_cast<EGLSurface>(static_cast<std::uintptr_t>(p->surface())), static_cast<EGLint>(p->buffer()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 3: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEglChooseConfig>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_configs(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            std::vector<int32_t>_out_num_config(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            { const auto _ret = ::eglChooseConfig(reinterpret_cast<EGLDisplay>(static_cast<std::uintptr_t>(p->dpy())), reinterpret_cast<const EGLint*>((p->attrib_list() == nullptr ? nullptr : p->attrib_list()->data())), reinterpret_cast<EGLConfig*>(_out_configs.data()), static_cast<EGLint>(p->config_size()), reinterpret_cast<EGLint*>(_out_num_config.data()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_configs.data()),
                                  static_cast<uint32_t>(_out_configs.size() * 4));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_num_config.data()),
                                  static_cast<uint32_t>(_out_num_config.size() * 4));
            return 0u;
        }

        case 4: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEglClientWaitSync>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::eglClientWaitSync(reinterpret_cast<EGLDisplay>(static_cast<std::uintptr_t>(p->dpy())), reinterpret_cast<EGLSync>(static_cast<std::uintptr_t>(p->sync())), static_cast<EGLint>(p->flags()), static_cast<EGLTime>(p->timeout()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 5: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEglCopyBuffers>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::eglCopyBuffers(reinterpret_cast<EGLDisplay>(static_cast<std::uintptr_t>(p->dpy())), reinterpret_cast<EGLSurface>(static_cast<std::uintptr_t>(p->surface())), reinterpret_cast<EGLNativePixmapType>(static_cast<std::uintptr_t>(p->target())));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 6: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEglCreateContext>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::eglCreateContext(reinterpret_cast<EGLDisplay>(static_cast<std::uintptr_t>(p->dpy())), reinterpret_cast<EGLConfig>(static_cast<std::uintptr_t>(p->config())), reinterpret_cast<EGLContext>(static_cast<std::uintptr_t>(p->shareCtx())), reinterpret_cast<const EGLint*>((p->attrib_list() == nullptr ? nullptr : p->attrib_list()->data())));
              WireDispatchStoreU64(reinterpret_cast<std::uintptr_t>(_ret)); }
            return 0u;
        }

        case 7: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEglCreateImage>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::eglCreateImage(reinterpret_cast<EGLDisplay>(static_cast<std::uintptr_t>(p->dpy())), reinterpret_cast<EGLContext>(static_cast<std::uintptr_t>(p->ctx())), static_cast<EGLenum>(p->target()), reinterpret_cast<EGLClientBuffer>(static_cast<std::uintptr_t>(p->buffer())), reinterpret_cast<const EGLAttrib*>((p->attrib_list() == nullptr ? nullptr : p->attrib_list()->data())));
              WireDispatchStoreU64(reinterpret_cast<std::uintptr_t>(_ret)); }
            return 0u;
        }

        case 8: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEglCreatePbufferFromClientBuffer>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::eglCreatePbufferFromClientBuffer(reinterpret_cast<EGLDisplay>(static_cast<std::uintptr_t>(p->dpy())), static_cast<EGLenum>(p->buftype()), reinterpret_cast<EGLClientBuffer>(static_cast<std::uintptr_t>(p->buffer())), reinterpret_cast<EGLConfig>(static_cast<std::uintptr_t>(p->config())), reinterpret_cast<const EGLint*>((p->attrib_list() == nullptr ? nullptr : p->attrib_list()->data())));
              WireDispatchStoreU64(reinterpret_cast<std::uintptr_t>(_ret)); }
            return 0u;
        }

        case 9: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEglCreatePbufferSurface>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::eglCreatePbufferSurface(reinterpret_cast<EGLDisplay>(static_cast<std::uintptr_t>(p->dpy())), reinterpret_cast<EGLConfig>(static_cast<std::uintptr_t>(p->config())), reinterpret_cast<const EGLint*>((p->attrib_list() == nullptr ? nullptr : p->attrib_list()->data())));
              WireDispatchStoreU64(reinterpret_cast<std::uintptr_t>(_ret)); }
            return 0u;
        }

        case 10: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEglCreatePixmapSurface>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::eglCreatePixmapSurface(reinterpret_cast<EGLDisplay>(static_cast<std::uintptr_t>(p->dpy())), reinterpret_cast<EGLConfig>(static_cast<std::uintptr_t>(p->config())), reinterpret_cast<EGLNativePixmapType>(static_cast<std::uintptr_t>(p->pixmap())), reinterpret_cast<const EGLint*>((p->attrib_list() == nullptr ? nullptr : p->attrib_list()->data())));
              WireDispatchStoreU64(reinterpret_cast<std::uintptr_t>(_ret)); }
            return 0u;
        }

        case 11: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEglCreatePlatformPixmapSurface>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::eglCreatePlatformPixmapSurface(reinterpret_cast<EGLDisplay>(static_cast<std::uintptr_t>(p->dpy())), reinterpret_cast<EGLConfig>(static_cast<std::uintptr_t>(p->config())), reinterpret_cast<void*>(static_cast<std::uintptr_t>(p->native_pixmap())), reinterpret_cast<const EGLAttrib*>((p->attrib_list() == nullptr ? nullptr : p->attrib_list()->data())));
              WireDispatchStoreU64(reinterpret_cast<std::uintptr_t>(_ret)); }
            return 0u;
        }

        case 12: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEglCreatePlatformWindowSurface>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::eglCreatePlatformWindowSurface(reinterpret_cast<EGLDisplay>(static_cast<std::uintptr_t>(p->dpy())), reinterpret_cast<EGLConfig>(static_cast<std::uintptr_t>(p->config())), reinterpret_cast<void*>(static_cast<std::uintptr_t>(p->native_window())), reinterpret_cast<const EGLAttrib*>((p->attrib_list() == nullptr ? nullptr : p->attrib_list()->data())));
              WireDispatchStoreU64(reinterpret_cast<std::uintptr_t>(_ret)); }
            return 0u;
        }

        case 13: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEglCreateSync>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::eglCreateSync(reinterpret_cast<EGLDisplay>(static_cast<std::uintptr_t>(p->dpy())), static_cast<EGLenum>(p->type()), reinterpret_cast<const EGLAttrib*>((p->attrib_list() == nullptr ? nullptr : p->attrib_list()->data())));
              WireDispatchStoreU64(reinterpret_cast<std::uintptr_t>(_ret)); }
            return 0u;
        }

        case 14: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEglCreateWindowSurface>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::eglCreateWindowSurface(reinterpret_cast<EGLDisplay>(static_cast<std::uintptr_t>(p->dpy())), reinterpret_cast<EGLConfig>(static_cast<std::uintptr_t>(p->config())), reinterpret_cast<NativeWindowType>(static_cast<std::uintptr_t>(p->window())), reinterpret_cast<const EGLint*>((p->attrib_list() == nullptr ? nullptr : p->attrib_list()->data())));
              WireDispatchStoreU64(reinterpret_cast<std::uintptr_t>(_ret)); }
            return 0u;
        }

        case 15: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEglDestroyContext>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::eglDestroyContext(reinterpret_cast<EGLDisplay>(static_cast<std::uintptr_t>(p->dpy())), reinterpret_cast<EGLContext>(static_cast<std::uintptr_t>(p->ctx())));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 16: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEglDestroyImage>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::eglDestroyImage(reinterpret_cast<EGLDisplay>(static_cast<std::uintptr_t>(p->dpy())), reinterpret_cast<EGLImage>(static_cast<std::uintptr_t>(p->image())));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 17: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEglDestroySurface>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::eglDestroySurface(reinterpret_cast<EGLDisplay>(static_cast<std::uintptr_t>(p->dpy())), reinterpret_cast<EGLSurface>(static_cast<std::uintptr_t>(p->surface())));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 18: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEglDestroySync>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::eglDestroySync(reinterpret_cast<EGLDisplay>(static_cast<std::uintptr_t>(p->dpy())), reinterpret_cast<EGLSync>(static_cast<std::uintptr_t>(p->sync())));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 19: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEglGetConfigAttrib>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_value(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            { const auto _ret = ::eglGetConfigAttrib(reinterpret_cast<EGLDisplay>(static_cast<std::uintptr_t>(p->dpy())), reinterpret_cast<EGLConfig>(static_cast<std::uintptr_t>(p->config())), static_cast<EGLint>(p->attribute()), reinterpret_cast<EGLint*>(_out_value.data()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_value.data()),
                                  static_cast<uint32_t>(_out_value.size() * 4));
            return 0u;
        }

        case 20: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEglGetConfigs>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_configs(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            std::vector<int32_t>_out_num_config(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            { const auto _ret = ::eglGetConfigs(reinterpret_cast<EGLDisplay>(static_cast<std::uintptr_t>(p->dpy())), reinterpret_cast<EGLConfig*>(_out_configs.data()), static_cast<EGLint>(p->config_size()), reinterpret_cast<EGLint*>(_out_num_config.data()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_configs.data()),
                                  static_cast<uint32_t>(_out_configs.size() * 4));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_num_config.data()),
                                  static_cast<uint32_t>(_out_num_config.size() * 4));
            return 0u;
        }

        case 23: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEglGetCurrentSurface>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::eglGetCurrentSurface(static_cast<EGLint>(p->readdraw()));
              WireDispatchStoreU64(reinterpret_cast<std::uintptr_t>(_ret)); }
            return 0u;
        }

        case 24: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEglGetDisplay>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::eglGetDisplay(reinterpret_cast<NativeDisplayType>(static_cast<std::uintptr_t>(p->display())));
              WireDispatchStoreU64(reinterpret_cast<std::uintptr_t>(_ret)); }
            return 0u;
        }

        case 25: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEglGetError>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::eglGetError();
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 26: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEglGetPlatformDisplay>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::eglGetPlatformDisplay(static_cast<EGLenum>(p->platform()), reinterpret_cast<void*>(static_cast<std::uintptr_t>(p->native_display())), reinterpret_cast<const EGLAttrib*>((p->attrib_list() == nullptr ? nullptr : p->attrib_list()->data())));
              WireDispatchStoreU64(reinterpret_cast<std::uintptr_t>(_ret)); }
            return 0u;
        }

        case 27: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEglGetPlatformDisplayEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::eglGetPlatformDisplayEXT(static_cast<EGLenum>(p->platform()), reinterpret_cast<void*>(static_cast<std::uintptr_t>(p->native_display())), reinterpret_cast<const EGLint*>((p->attrib_list() == nullptr ? nullptr : p->attrib_list()->data())));
              WireDispatchStoreU64(reinterpret_cast<std::uintptr_t>(_ret)); }
            return 0u;
        }

        case 29: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEglGetSyncAttrib>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint64_t>_out_value(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            { const auto _ret = ::eglGetSyncAttrib(reinterpret_cast<EGLDisplay>(static_cast<std::uintptr_t>(p->dpy())), reinterpret_cast<EGLSync>(static_cast<std::uintptr_t>(p->sync())), static_cast<EGLint>(p->attribute()), reinterpret_cast<EGLAttrib*>(_out_value.data()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_value.data()),
                                  static_cast<uint32_t>(_out_value.size() * 8));
            return 0u;
        }

        case 30: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEglInitialize>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_major(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            std::vector<int32_t>_out_minor(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            { const auto _ret = ::eglInitialize(reinterpret_cast<EGLDisplay>(static_cast<std::uintptr_t>(p->dpy())), reinterpret_cast<EGLint*>(_out_major.data()), reinterpret_cast<EGLint*>(_out_minor.data()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_major.data()),
                                  static_cast<uint32_t>(_out_major.size() * 4));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_minor.data()),
                                  static_cast<uint32_t>(_out_minor.size() * 4));
            return 0u;
        }

        case 31: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEglMakeCurrent>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::eglMakeCurrent(reinterpret_cast<EGLDisplay>(static_cast<std::uintptr_t>(p->dpy())), reinterpret_cast<EGLSurface>(static_cast<std::uintptr_t>(p->draw())), reinterpret_cast<EGLSurface>(static_cast<std::uintptr_t>(p->read())), reinterpret_cast<EGLContext>(static_cast<std::uintptr_t>(p->ctx())));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 33: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEglQueryContext>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_value(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            { const auto _ret = ::eglQueryContext(reinterpret_cast<EGLDisplay>(static_cast<std::uintptr_t>(p->dpy())), reinterpret_cast<EGLContext>(static_cast<std::uintptr_t>(p->ctx())), static_cast<EGLint>(p->attribute()), reinterpret_cast<EGLint*>(_out_value.data()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_value.data()),
                                  static_cast<uint32_t>(_out_value.size() * 4));
            return 0u;
        }

        case 34: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEglQueryString>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::eglQueryString(reinterpret_cast<EGLDisplay>(static_cast<std::uintptr_t>(p->display())), static_cast<EGLint>(p->name()));
              WireDispatchStoreU64(reinterpret_cast<std::uintptr_t>(_ret)); }
            return 0u;
        }

        case 35: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEglQuerySurface>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_value(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            { const auto _ret = ::eglQuerySurface(reinterpret_cast<EGLDisplay>(static_cast<std::uintptr_t>(p->display())), reinterpret_cast<EGLSurface>(static_cast<std::uintptr_t>(p->surface())), static_cast<EGLint>(p->attribute()), reinterpret_cast<EGLint*>(_out_value.data()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_value.data()),
                                  static_cast<uint32_t>(_out_value.size() * 4));
            return 0u;
        }

        case 36: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEglReleaseTexImage>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::eglReleaseTexImage(reinterpret_cast<EGLDisplay>(static_cast<std::uintptr_t>(p->dpy())), reinterpret_cast<EGLSurface>(static_cast<std::uintptr_t>(p->surface())), static_cast<EGLint>(p->buffer()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 38: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEglSurfaceAttrib>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::eglSurfaceAttrib(reinterpret_cast<EGLDisplay>(static_cast<std::uintptr_t>(p->dpy())), reinterpret_cast<EGLSurface>(static_cast<std::uintptr_t>(p->surface())), static_cast<EGLint>(p->attribute()), static_cast<EGLint>(p->value()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 39: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEglSwapBuffers>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::eglSwapBuffers(reinterpret_cast<EGLDisplay>(static_cast<std::uintptr_t>(p->dpy())), reinterpret_cast<EGLSurface>(static_cast<std::uintptr_t>(p->draw())));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 40: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEglSwapInterval>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::eglSwapInterval(reinterpret_cast<EGLDisplay>(static_cast<std::uintptr_t>(p->dpy())), static_cast<EGLint>(p->interval()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 41: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEglTerminate>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::eglTerminate(reinterpret_cast<EGLDisplay>(static_cast<std::uintptr_t>(p->dpy())));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 44: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEglWaitNative>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::eglWaitNative(static_cast<EGLint>(p->engine()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 45: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEglWaitSync>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::eglWaitSync(reinterpret_cast<EGLDisplay>(static_cast<std::uintptr_t>(p->dpy())), reinterpret_cast<EGLSync>(static_cast<std::uintptr_t>(p->sync())), static_cast<EGLint>(p->flags()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 46: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenActiveProgramEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glActiveProgramEXT(static_cast<GLuint>(p->program()));
            return 0u;
        }

        case 47: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenActiveShaderProgram>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glActiveShaderProgram(static_cast<GLuint>(p->pipeline()), static_cast<GLuint>(p->program()));
            return 0u;
        }

        case 48: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenActiveTexture>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glActiveTexture(static_cast<GLenum>(p->texture()));
            return 0u;
        }

        case 50: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenAttachShader>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glAttachShader(static_cast<GLuint>(p->program()), static_cast<GLuint>(p->shader()));
            return 0u;
        }

        case 51: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBeginConditionalRender>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBeginConditionalRender(static_cast<GLuint>(p->id()), static_cast<GLenum>(p->mode()));
            return 0u;
        }

        case 52: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBeginConditionalRenderNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBeginConditionalRenderNV(static_cast<GLuint>(p->id()), static_cast<GLenum>(p->mode()));
            return 0u;
        }

        case 53: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBeginPerfMonitorAMD>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBeginPerfMonitorAMD(static_cast<GLuint>(p->monitor()));
            return 0u;
        }

        case 54: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBeginPerfQueryINTEL>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBeginPerfQueryINTEL(static_cast<GLuint>(p->queryHandle()));
            return 0u;
        }

        case 55: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBeginQuery>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBeginQuery(static_cast<GLenum>(p->target()), static_cast<GLuint>(p->id()));
            return 0u;
        }

        case 56: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBeginQueryIndexed>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBeginQueryIndexed(static_cast<GLenum>(p->target()), static_cast<GLuint>(p->index()), static_cast<GLuint>(p->id()));
            return 0u;
        }

        case 57: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBeginTransformFeedback>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBeginTransformFeedback(static_cast<GLenum>(p->primitiveMode()));
            return 0u;
        }

        case 58: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBindAttribLocation>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBindAttribLocation(static_cast<GLuint>(p->program()), static_cast<GLuint>(p->index()), (p->name() == nullptr ? nullptr : p->name()->c_str()));
            return 0u;
        }

        case 59: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBindBuffer>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBindBuffer(static_cast<GLenum>(p->target()), static_cast<GLuint>(p->buffer()));
            return 0u;
        }

        case 60: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBindBufferBase>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBindBufferBase(static_cast<GLenum>(p->target()), static_cast<GLuint>(p->index()), static_cast<GLuint>(p->buffer()));
            return 0u;
        }

        case 61: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBindBufferRange>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBindBufferRange(static_cast<GLenum>(p->target()), static_cast<GLuint>(p->index()), static_cast<GLuint>(p->buffer()), static_cast<GLintptr>(p->offset()), static_cast<GLsizeiptr>(p->size()));
            return 0u;
        }

        case 62: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBindBuffersBase>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBindBuffersBase(static_cast<GLenum>(p->target()), static_cast<GLuint>(p->first()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLuint*>((p->buffers() == nullptr ? nullptr : p->buffers()->data())));
            return 0u;
        }

        case 64: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBindFragDataLocation>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBindFragDataLocation(static_cast<GLuint>(p->program()), static_cast<GLuint>(p->color()), (p->name() == nullptr ? nullptr : p->name()->c_str()));
            return 0u;
        }

        case 65: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBindFragDataLocationIndexed>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBindFragDataLocationIndexed(static_cast<GLuint>(p->program()), static_cast<GLuint>(p->colorNumber()), static_cast<GLuint>(p->index()), (p->name() == nullptr ? nullptr : p->name()->c_str()));
            return 0u;
        }

        case 66: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBindFramebuffer>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBindFramebuffer(static_cast<GLenum>(p->target()), static_cast<GLuint>(p->framebuffer()));
            return 0u;
        }

        case 68: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBindImageTexture>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBindImageTexture(static_cast<GLuint>(p->unit()), static_cast<GLuint>(p->texture()), static_cast<GLint>(p->level()), static_cast<GLboolean>(p->layered()), static_cast<GLint>(p->layer()), static_cast<GLenum>(p->access()), static_cast<GLenum>(p->format()));
            return 0u;
        }

        case 69: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBindImageTextures>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBindImageTextures(static_cast<GLuint>(p->first()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLuint*>((p->textures() == nullptr ? nullptr : p->textures()->data())));
            return 0u;
        }

        case 70: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBindMultiTextureEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBindMultiTextureEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->target()), static_cast<GLuint>(p->texture()));
            return 0u;
        }

        case 71: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBindProgramPipeline>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBindProgramPipeline(static_cast<GLuint>(p->pipeline()));
            return 0u;
        }

        case 72: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBindRenderbuffer>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBindRenderbuffer(static_cast<GLenum>(p->target()), static_cast<GLuint>(p->renderbuffer()));
            return 0u;
        }

        case 74: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBindSampler>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBindSampler(static_cast<GLuint>(p->unit()), static_cast<GLuint>(p->sampler()));
            return 0u;
        }

        case 75: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBindSamplers>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBindSamplers(static_cast<GLuint>(p->first()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLuint*>((p->samplers() == nullptr ? nullptr : p->samplers()->data())));
            return 0u;
        }

        case 76: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBindShadingRateImageNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBindShadingRateImageNV(static_cast<GLuint>(p->texture()));
            return 0u;
        }

        case 77: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBindTexture>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBindTexture(static_cast<GLenum>(p->target()), static_cast<GLuint>(p->texture()));
            return 0u;
        }

        case 78: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBindTextureUnit>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBindTextureUnit(static_cast<GLuint>(p->unit()), static_cast<GLuint>(p->texture()));
            return 0u;
        }

        case 79: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBindTextures>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBindTextures(static_cast<GLuint>(p->first()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLuint*>((p->textures() == nullptr ? nullptr : p->textures()->data())));
            return 0u;
        }

        case 80: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBindTransformFeedback>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBindTransformFeedback(static_cast<GLenum>(p->target()), static_cast<GLuint>(p->id()));
            return 0u;
        }

        case 81: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBindVertexArray>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBindVertexArray(static_cast<GLuint>(p->array()));
            return 0u;
        }

        case 82: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBindVertexBuffer>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBindVertexBuffer(static_cast<GLuint>(p->bindingindex()), static_cast<GLuint>(p->buffer()), static_cast<GLintptr>(p->offset()), static_cast<GLsizei>(p->stride()));
            return 0u;
        }

        case 86: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBlendColor>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBlendColor(static_cast<GLfloat>(p->red()), static_cast<GLfloat>(p->green()), static_cast<GLfloat>(p->blue()), static_cast<GLfloat>(p->alpha()));
            return 0u;
        }

        case 87: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBlendEquation>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBlendEquation(static_cast<GLenum>(p->mode()));
            return 0u;
        }

        case 88: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBlendEquationSeparate>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBlendEquationSeparate(static_cast<GLenum>(p->modeRGB()), static_cast<GLenum>(p->modeAlpha()));
            return 0u;
        }

        case 89: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBlendEquationSeparatei>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBlendEquationSeparatei(static_cast<GLuint>(p->buf()), static_cast<GLenum>(p->modeRGB()), static_cast<GLenum>(p->modeAlpha()));
            return 0u;
        }

        case 90: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBlendEquationSeparateiARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBlendEquationSeparateiARB(static_cast<GLuint>(p->buf()), static_cast<GLenum>(p->modeRGB()), static_cast<GLenum>(p->modeAlpha()));
            return 0u;
        }

        case 91: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBlendEquationi>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBlendEquationi(static_cast<GLuint>(p->buf()), static_cast<GLenum>(p->mode()));
            return 0u;
        }

        case 92: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBlendEquationiARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBlendEquationiARB(static_cast<GLuint>(p->buf()), static_cast<GLenum>(p->mode()));
            return 0u;
        }

        case 93: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBlendFunc>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBlendFunc(static_cast<GLenum>(p->sfactor()), static_cast<GLenum>(p->dfactor()));
            return 0u;
        }

        case 94: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBlendFuncSeparate>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBlendFuncSeparate(static_cast<GLenum>(p->sfactorRGB()), static_cast<GLenum>(p->dfactorRGB()), static_cast<GLenum>(p->sfactorAlpha()), static_cast<GLenum>(p->dfactorAlpha()));
            return 0u;
        }

        case 95: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBlendFuncSeparatei>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBlendFuncSeparatei(static_cast<GLuint>(p->buf()), static_cast<GLenum>(p->srcRGB()), static_cast<GLenum>(p->dstRGB()), static_cast<GLenum>(p->srcAlpha()), static_cast<GLenum>(p->dstAlpha()));
            return 0u;
        }

        case 96: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBlendFuncSeparateiARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBlendFuncSeparateiARB(static_cast<GLuint>(p->buf()), static_cast<GLenum>(p->srcRGB()), static_cast<GLenum>(p->dstRGB()), static_cast<GLenum>(p->srcAlpha()), static_cast<GLenum>(p->dstAlpha()));
            return 0u;
        }

        case 97: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBlendFunci>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBlendFunci(static_cast<GLuint>(p->buf()), static_cast<GLenum>(p->src()), static_cast<GLenum>(p->dst()));
            return 0u;
        }

        case 98: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBlendFunciARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBlendFunciARB(static_cast<GLuint>(p->buf()), static_cast<GLenum>(p->src()), static_cast<GLenum>(p->dst()));
            return 0u;
        }

        case 99: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBlendParameteriNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBlendParameteriNV(static_cast<GLenum>(p->pname()), static_cast<GLint>(p->value()));
            return 0u;
        }

        case 100: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBlitFramebuffer>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBlitFramebuffer(static_cast<GLint>(p->srcX0()), static_cast<GLint>(p->srcY0()), static_cast<GLint>(p->srcX1()), static_cast<GLint>(p->srcY1()), static_cast<GLint>(p->dstX0()), static_cast<GLint>(p->dstY0()), static_cast<GLint>(p->dstX1()), static_cast<GLint>(p->dstY1()), static_cast<GLbitfield>(p->mask()), static_cast<GLenum>(p->filter()));
            return 0u;
        }

        case 101: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBlitNamedFramebuffer>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBlitNamedFramebuffer(static_cast<GLuint>(p->readFramebuffer()), static_cast<GLuint>(p->drawFramebuffer()), static_cast<GLint>(p->srcX0()), static_cast<GLint>(p->srcY0()), static_cast<GLint>(p->srcX1()), static_cast<GLint>(p->srcY1()), static_cast<GLint>(p->dstX0()), static_cast<GLint>(p->dstY0()), static_cast<GLint>(p->dstX1()), static_cast<GLint>(p->dstY1()), static_cast<GLbitfield>(p->mask()), static_cast<GLenum>(p->filter()));
            return 0u;
        }

        case 102: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBufferAddressRangeNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBufferAddressRangeNV(static_cast<GLenum>(p->pname()), static_cast<GLuint>(p->index()), p->address(), static_cast<GLsizeiptr>(p->length()));
            return 0u;
        }

        case 103: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBufferAttachMemoryNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBufferAttachMemoryNV(static_cast<GLenum>(p->target()), static_cast<GLuint>(p->memory()), static_cast<GLuint64>(p->offset()));
            return 0u;
        }

        case 104: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBufferData>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBufferData(static_cast<GLenum>(p->target()), static_cast<GLsizeiptr>(p->size()), ResolveShm(p->data(), receivedShm, receivedShmCount), static_cast<GLenum>(p->usage()));
            return 0u;
        }

        case 105: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBufferPageCommitmentARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBufferPageCommitmentARB(static_cast<GLenum>(p->target()), static_cast<GLintptr>(p->offset()), static_cast<GLsizeiptr>(p->size()), static_cast<GLboolean>(p->commit()));
            return 0u;
        }

        case 106: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBufferPageCommitmentMemNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBufferPageCommitmentMemNV(static_cast<GLenum>(p->target()), static_cast<GLintptr>(p->offset()), static_cast<GLsizeiptr>(p->size()), static_cast<GLuint>(p->memory()), static_cast<GLuint64>(p->memOffset()), static_cast<GLboolean>(p->commit()));
            return 0u;
        }

        case 107: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBufferStorage>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBufferStorage(static_cast<GLenum>(p->target()), static_cast<GLsizeiptr>(p->size()), ResolveShm(p->data(), receivedShm, receivedShmCount), static_cast<GLbitfield>(p->flags()));
            return 0u;
        }

        case 108: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenBufferSubData>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glBufferSubData(static_cast<GLenum>(p->target()), static_cast<GLintptr>(p->offset()), static_cast<GLsizeiptr>(p->size()), ResolveShm(p->data(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 109: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCallCommandListNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCallCommandListNV(static_cast<GLuint>(p->list()));
            return 0u;
        }

        case 110: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCheckFramebufferStatus>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glCheckFramebufferStatus(static_cast<GLenum>(p->target()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 112: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCheckNamedFramebufferStatus>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glCheckNamedFramebufferStatus(static_cast<GLuint>(p->framebuffer()), static_cast<GLenum>(p->target()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 113: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCheckNamedFramebufferStatusEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glCheckNamedFramebufferStatusEXT(static_cast<GLuint>(p->framebuffer()), static_cast<GLenum>(p->target()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 114: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenClampColor>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glClampColor(static_cast<GLenum>(p->target()), static_cast<GLenum>(p->clamp()));
            return 0u;
        }

        case 115: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenClear>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glClear(static_cast<GLbitfield>(p->mask()));
            return 0u;
        }

        case 116: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenClearBufferData>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glClearBufferData(static_cast<GLenum>(p->target()), static_cast<GLenum>(p->internalformat()), static_cast<GLenum>(p->format()), static_cast<GLenum>(p->type()), ResolveShm(p->data(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 117: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenClearBufferSubData>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glClearBufferSubData(static_cast<GLenum>(p->target()), static_cast<GLenum>(p->internalformat()), static_cast<GLintptr>(p->offset()), static_cast<GLsizeiptr>(p->size()), static_cast<GLenum>(p->format()), static_cast<GLenum>(p->type()), ResolveShm(p->data(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 118: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenClearBufferfi>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glClearBufferfi(static_cast<GLenum>(p->buffer()), static_cast<GLint>(p->drawbuffer()), static_cast<GLfloat>(p->depth()), static_cast<GLint>(p->stencil()));
            return 0u;
        }

        case 119: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenClearBufferfv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glClearBufferfv(static_cast<GLenum>(p->buffer()), static_cast<GLint>(p->drawbuffer()), reinterpret_cast<const GLfloat*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 120: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenClearBufferiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glClearBufferiv(static_cast<GLenum>(p->buffer()), static_cast<GLint>(p->drawbuffer()), reinterpret_cast<const GLint*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 121: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenClearBufferuiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glClearBufferuiv(static_cast<GLenum>(p->buffer()), static_cast<GLint>(p->drawbuffer()), reinterpret_cast<const GLuint*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 122: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenClearColor>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glClearColor(static_cast<GLfloat>(p->red()), static_cast<GLfloat>(p->green()), static_cast<GLfloat>(p->blue()), static_cast<GLfloat>(p->alpha()));
            return 0u;
        }

        case 123: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenClearDepth>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glClearDepth(static_cast<GLclampd>(p->depth()));
            return 0u;
        }

        case 124: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenClearDepthdNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glClearDepthdNV(static_cast<GLdouble>(p->depth()));
            return 0u;
        }

        case 125: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenClearDepthf>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glClearDepthf(static_cast<GLfloat>(p->d()));
            return 0u;
        }

        case 126: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenClearNamedBufferData>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glClearNamedBufferData(static_cast<GLuint>(p->buffer()), static_cast<GLenum>(p->internalformat()), static_cast<GLenum>(p->format()), static_cast<GLenum>(p->type()), ResolveShm(p->data(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 127: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenClearNamedBufferDataEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glClearNamedBufferDataEXT(static_cast<GLuint>(p->buffer()), static_cast<GLenum>(p->internalformat()), static_cast<GLenum>(p->format()), static_cast<GLenum>(p->type()), ResolveShm(p->data(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 128: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenClearNamedBufferSubData>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glClearNamedBufferSubData(static_cast<GLuint>(p->buffer()), static_cast<GLenum>(p->internalformat()), static_cast<GLintptr>(p->offset()), static_cast<GLsizeiptr>(p->size()), static_cast<GLenum>(p->format()), static_cast<GLenum>(p->type()), ResolveShm(p->data(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 129: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenClearNamedBufferSubDataEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glClearNamedBufferSubDataEXT(static_cast<GLuint>(p->buffer()), static_cast<GLenum>(p->internalformat()), static_cast<GLsizeiptr>(p->offset()), static_cast<GLsizeiptr>(p->size()), static_cast<GLenum>(p->format()), static_cast<GLenum>(p->type()), ResolveShm(p->data(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 130: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenClearNamedFramebufferfi>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glClearNamedFramebufferfi(static_cast<GLuint>(p->framebuffer()), static_cast<GLenum>(p->buffer()), static_cast<GLint>(p->drawbuffer()), static_cast<GLfloat>(p->depth()), static_cast<GLint>(p->stencil()));
            return 0u;
        }

        case 131: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenClearNamedFramebufferfv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glClearNamedFramebufferfv(static_cast<GLuint>(p->framebuffer()), static_cast<GLenum>(p->buffer()), static_cast<GLint>(p->drawbuffer()), reinterpret_cast<const GLfloat*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 132: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenClearNamedFramebufferiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glClearNamedFramebufferiv(static_cast<GLuint>(p->framebuffer()), static_cast<GLenum>(p->buffer()), static_cast<GLint>(p->drawbuffer()), reinterpret_cast<const GLint*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 133: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenClearNamedFramebufferuiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glClearNamedFramebufferuiv(static_cast<GLuint>(p->framebuffer()), static_cast<GLenum>(p->buffer()), static_cast<GLint>(p->drawbuffer()), reinterpret_cast<const GLuint*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 134: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenClearStencil>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glClearStencil(static_cast<GLint>(p->s()));
            return 0u;
        }

        case 135: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenClearTexImage>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glClearTexImage(static_cast<GLuint>(p->texture()), static_cast<GLint>(p->level()), static_cast<GLenum>(p->format()), static_cast<GLenum>(p->type()), ResolveShm(p->data(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 136: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenClearTexSubImage>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glClearTexSubImage(static_cast<GLuint>(p->texture()), static_cast<GLint>(p->level()), static_cast<GLint>(p->xoffset()), static_cast<GLint>(p->yoffset()), static_cast<GLint>(p->zoffset()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLsizei>(p->depth()), static_cast<GLenum>(p->format()), static_cast<GLenum>(p->type()), ResolveShm(p->data(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 137: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenClientAttribDefaultEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glClientAttribDefaultEXT(static_cast<GLbitfield>(p->mask()));
            return 0u;
        }

        case 138: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenClientWaitSync>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glClientWaitSync(reinterpret_cast<GLsync>(static_cast<std::uintptr_t>(p->sync())), static_cast<GLbitfield>(p->flags()), static_cast<GLuint64>(p->timeout()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 139: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenClipControl>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glClipControl(static_cast<GLenum>(p->origin()), static_cast<GLenum>(p->depth()));
            return 0u;
        }

        case 140: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenColorFormatNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glColorFormatNV(static_cast<GLint>(p->size()), static_cast<GLenum>(p->type()), static_cast<GLsizei>(p->stride()));
            return 0u;
        }

        case 141: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenColorMask>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glColorMask(static_cast<GLboolean>(p->red()), static_cast<GLboolean>(p->green()), static_cast<GLboolean>(p->blue()), static_cast<GLboolean>(p->alpha()));
            return 0u;
        }

        case 142: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenColorMaski>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glColorMaski(static_cast<GLuint>(p->index()), static_cast<GLboolean>(p->r()), static_cast<GLboolean>(p->g()), static_cast<GLboolean>(p->b()), static_cast<GLboolean>(p->a()));
            return 0u;
        }

        case 143: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCommandListSegmentsNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCommandListSegmentsNV(static_cast<GLuint>(p->list()), static_cast<GLuint>(p->segments()));
            return 0u;
        }

        case 144: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCompileCommandListNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCompileCommandListNV(static_cast<GLuint>(p->list()));
            return 0u;
        }

        case 145: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCompileShader>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCompileShader(static_cast<GLuint>(p->shader()));
            return 0u;
        }

        case 147: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCompressedMultiTexImage1DEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCompressedMultiTexImage1DEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLenum>(p->internalformat()), static_cast<GLsizei>(p->width()), static_cast<GLint>(p->border()), static_cast<GLsizei>(p->imageSize()), ResolveShm(p->bits(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 148: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCompressedMultiTexImage2DEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCompressedMultiTexImage2DEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLenum>(p->internalformat()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLint>(p->border()), static_cast<GLsizei>(p->imageSize()), ResolveShm(p->bits(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 149: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCompressedMultiTexImage3DEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCompressedMultiTexImage3DEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLenum>(p->internalformat()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLsizei>(p->depth()), static_cast<GLint>(p->border()), static_cast<GLsizei>(p->imageSize()), ResolveShm(p->bits(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 150: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCompressedMultiTexSubImage1DEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCompressedMultiTexSubImage1DEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLint>(p->xoffset()), static_cast<GLsizei>(p->width()), static_cast<GLenum>(p->format()), static_cast<GLsizei>(p->imageSize()), ResolveShm(p->bits(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 151: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCompressedMultiTexSubImage2DEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCompressedMultiTexSubImage2DEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLint>(p->xoffset()), static_cast<GLint>(p->yoffset()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLenum>(p->format()), static_cast<GLsizei>(p->imageSize()), ResolveShm(p->bits(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 152: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCompressedMultiTexSubImage3DEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCompressedMultiTexSubImage3DEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLint>(p->xoffset()), static_cast<GLint>(p->yoffset()), static_cast<GLint>(p->zoffset()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLsizei>(p->depth()), static_cast<GLenum>(p->format()), static_cast<GLsizei>(p->imageSize()), ResolveShm(p->bits(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 153: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCompressedTexImage1D>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCompressedTexImage1D(static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLenum>(p->internalformat()), static_cast<GLsizei>(p->width()), static_cast<GLint>(p->border()), static_cast<GLsizei>(p->imageSize()), ResolveShm(p->data(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 154: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCompressedTexImage2D>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCompressedTexImage2D(static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLenum>(p->internalformat()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLint>(p->border()), static_cast<GLsizei>(p->imageSize()), ResolveShm(p->data(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 155: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCompressedTexImage3D>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCompressedTexImage3D(static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLenum>(p->internalformat()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLsizei>(p->depth()), static_cast<GLint>(p->border()), static_cast<GLsizei>(p->imageSize()), ResolveShm(p->data(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 156: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCompressedTexSubImage1D>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCompressedTexSubImage1D(static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLint>(p->xoffset()), static_cast<GLsizei>(p->width()), static_cast<GLenum>(p->format()), static_cast<GLsizei>(p->imageSize()), ResolveShm(p->data(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 157: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCompressedTexSubImage2D>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCompressedTexSubImage2D(static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLint>(p->xoffset()), static_cast<GLint>(p->yoffset()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLenum>(p->format()), static_cast<GLsizei>(p->imageSize()), ResolveShm(p->data(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 158: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCompressedTexSubImage3D>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCompressedTexSubImage3D(static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLint>(p->xoffset()), static_cast<GLint>(p->yoffset()), static_cast<GLint>(p->zoffset()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLsizei>(p->depth()), static_cast<GLenum>(p->format()), static_cast<GLsizei>(p->imageSize()), ResolveShm(p->data(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 159: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCompressedTextureImage1DEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCompressedTextureImage1DEXT(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLenum>(p->internalformat()), static_cast<GLsizei>(p->width()), static_cast<GLint>(p->border()), static_cast<GLsizei>(p->imageSize()), ResolveShm(p->bits(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 160: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCompressedTextureImage2DEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCompressedTextureImage2DEXT(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLenum>(p->internalformat()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLint>(p->border()), static_cast<GLsizei>(p->imageSize()), ResolveShm(p->bits(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 161: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCompressedTextureImage3DEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCompressedTextureImage3DEXT(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLenum>(p->internalformat()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLsizei>(p->depth()), static_cast<GLint>(p->border()), static_cast<GLsizei>(p->imageSize()), ResolveShm(p->bits(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 162: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCompressedTextureSubImage1D>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCompressedTextureSubImage1D(static_cast<GLuint>(p->texture()), static_cast<GLint>(p->level()), static_cast<GLint>(p->xoffset()), static_cast<GLsizei>(p->width()), static_cast<GLenum>(p->format()), static_cast<GLsizei>(p->imageSize()), ResolveShm(p->data(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 163: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCompressedTextureSubImage1DEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCompressedTextureSubImage1DEXT(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLint>(p->xoffset()), static_cast<GLsizei>(p->width()), static_cast<GLenum>(p->format()), static_cast<GLsizei>(p->imageSize()), ResolveShm(p->bits(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 164: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCompressedTextureSubImage2D>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCompressedTextureSubImage2D(static_cast<GLuint>(p->texture()), static_cast<GLint>(p->level()), static_cast<GLint>(p->xoffset()), static_cast<GLint>(p->yoffset()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLenum>(p->format()), static_cast<GLsizei>(p->imageSize()), ResolveShm(p->data(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 165: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCompressedTextureSubImage2DEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCompressedTextureSubImage2DEXT(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLint>(p->xoffset()), static_cast<GLint>(p->yoffset()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLenum>(p->format()), static_cast<GLsizei>(p->imageSize()), ResolveShm(p->bits(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 166: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCompressedTextureSubImage3D>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCompressedTextureSubImage3D(static_cast<GLuint>(p->texture()), static_cast<GLint>(p->level()), static_cast<GLint>(p->xoffset()), static_cast<GLint>(p->yoffset()), static_cast<GLint>(p->zoffset()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLsizei>(p->depth()), static_cast<GLenum>(p->format()), static_cast<GLsizei>(p->imageSize()), ResolveShm(p->data(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 167: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCompressedTextureSubImage3DEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCompressedTextureSubImage3DEXT(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLint>(p->xoffset()), static_cast<GLint>(p->yoffset()), static_cast<GLint>(p->zoffset()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLsizei>(p->depth()), static_cast<GLenum>(p->format()), static_cast<GLsizei>(p->imageSize()), ResolveShm(p->bits(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 168: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenConservativeRasterParameterfNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glConservativeRasterParameterfNV(static_cast<GLenum>(p->pname()), static_cast<GLfloat>(p->value()));
            return 0u;
        }

        case 169: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenConservativeRasterParameteriNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glConservativeRasterParameteriNV(static_cast<GLenum>(p->pname()), static_cast<GLint>(p->param()));
            return 0u;
        }

        case 170: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCopyBufferSubData>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCopyBufferSubData(static_cast<GLenum>(p->readTarget()), static_cast<GLenum>(p->writeTarget()), static_cast<GLintptr>(p->readOffset()), static_cast<GLintptr>(p->writeOffset()), static_cast<GLsizeiptr>(p->size()));
            return 0u;
        }

        case 171: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCopyImageSubData>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCopyImageSubData(static_cast<GLuint>(p->srcName()), static_cast<GLenum>(p->srcTarget()), static_cast<GLint>(p->srcLevel()), static_cast<GLint>(p->srcX()), static_cast<GLint>(p->srcY()), static_cast<GLint>(p->srcZ()), static_cast<GLuint>(p->dstName()), static_cast<GLenum>(p->dstTarget()), static_cast<GLint>(p->dstLevel()), static_cast<GLint>(p->dstX()), static_cast<GLint>(p->dstY()), static_cast<GLint>(p->dstZ()), static_cast<GLsizei>(p->srcWidth()), static_cast<GLsizei>(p->srcHeight()), static_cast<GLsizei>(p->srcDepth()));
            return 0u;
        }

        case 172: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCopyMultiTexImage1DEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCopyMultiTexImage1DEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLenum>(p->internalformat()), static_cast<GLint>(p->x()), static_cast<GLint>(p->y()), static_cast<GLsizei>(p->width()), static_cast<GLint>(p->border()));
            return 0u;
        }

        case 173: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCopyMultiTexImage2DEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCopyMultiTexImage2DEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLenum>(p->internalformat()), static_cast<GLint>(p->x()), static_cast<GLint>(p->y()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLint>(p->border()));
            return 0u;
        }

        case 174: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCopyMultiTexSubImage1DEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCopyMultiTexSubImage1DEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLint>(p->xoffset()), static_cast<GLint>(p->x()), static_cast<GLint>(p->y()), static_cast<GLsizei>(p->width()));
            return 0u;
        }

        case 175: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCopyMultiTexSubImage2DEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCopyMultiTexSubImage2DEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLint>(p->xoffset()), static_cast<GLint>(p->yoffset()), static_cast<GLint>(p->x()), static_cast<GLint>(p->y()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()));
            return 0u;
        }

        case 176: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCopyMultiTexSubImage3DEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCopyMultiTexSubImage3DEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLint>(p->xoffset()), static_cast<GLint>(p->yoffset()), static_cast<GLint>(p->zoffset()), static_cast<GLint>(p->x()), static_cast<GLint>(p->y()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()));
            return 0u;
        }

        case 177: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCopyNamedBufferSubData>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCopyNamedBufferSubData(static_cast<GLuint>(p->readBuffer()), static_cast<GLuint>(p->writeBuffer()), static_cast<GLintptr>(p->readOffset()), static_cast<GLintptr>(p->writeOffset()), static_cast<GLsizeiptr>(p->size()));
            return 0u;
        }

        case 178: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCopyPathNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCopyPathNV(static_cast<GLuint>(p->resultPath()), static_cast<GLuint>(p->srcPath()));
            return 0u;
        }

        case 179: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCopyTexImage1D>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCopyTexImage1D(static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLenum>(p->internalformat()), static_cast<GLint>(p->x()), static_cast<GLint>(p->y()), static_cast<GLsizei>(p->width()), static_cast<GLint>(p->border()));
            return 0u;
        }

        case 180: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCopyTexImage2D>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCopyTexImage2D(static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLenum>(p->internalformat()), static_cast<GLint>(p->x()), static_cast<GLint>(p->y()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLint>(p->border()));
            return 0u;
        }

        case 181: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCopyTexSubImage1D>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCopyTexSubImage1D(static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLint>(p->xoffset()), static_cast<GLint>(p->x()), static_cast<GLint>(p->y()), static_cast<GLsizei>(p->width()));
            return 0u;
        }

        case 182: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCopyTexSubImage2D>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCopyTexSubImage2D(static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLint>(p->xoffset()), static_cast<GLint>(p->yoffset()), static_cast<GLint>(p->x()), static_cast<GLint>(p->y()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()));
            return 0u;
        }

        case 183: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCopyTexSubImage3D>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCopyTexSubImage3D(static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLint>(p->xoffset()), static_cast<GLint>(p->yoffset()), static_cast<GLint>(p->zoffset()), static_cast<GLint>(p->x()), static_cast<GLint>(p->y()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()));
            return 0u;
        }

        case 184: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCopyTextureImage1DEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCopyTextureImage1DEXT(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLenum>(p->internalformat()), static_cast<GLint>(p->x()), static_cast<GLint>(p->y()), static_cast<GLsizei>(p->width()), static_cast<GLint>(p->border()));
            return 0u;
        }

        case 185: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCopyTextureImage2DEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCopyTextureImage2DEXT(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLenum>(p->internalformat()), static_cast<GLint>(p->x()), static_cast<GLint>(p->y()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLint>(p->border()));
            return 0u;
        }

        case 186: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCopyTextureSubImage1D>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCopyTextureSubImage1D(static_cast<GLuint>(p->texture()), static_cast<GLint>(p->level()), static_cast<GLint>(p->xoffset()), static_cast<GLint>(p->x()), static_cast<GLint>(p->y()), static_cast<GLsizei>(p->width()));
            return 0u;
        }

        case 187: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCopyTextureSubImage1DEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCopyTextureSubImage1DEXT(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLint>(p->xoffset()), static_cast<GLint>(p->x()), static_cast<GLint>(p->y()), static_cast<GLsizei>(p->width()));
            return 0u;
        }

        case 188: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCopyTextureSubImage2D>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCopyTextureSubImage2D(static_cast<GLuint>(p->texture()), static_cast<GLint>(p->level()), static_cast<GLint>(p->xoffset()), static_cast<GLint>(p->yoffset()), static_cast<GLint>(p->x()), static_cast<GLint>(p->y()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()));
            return 0u;
        }

        case 189: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCopyTextureSubImage2DEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCopyTextureSubImage2DEXT(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLint>(p->xoffset()), static_cast<GLint>(p->yoffset()), static_cast<GLint>(p->x()), static_cast<GLint>(p->y()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()));
            return 0u;
        }

        case 190: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCopyTextureSubImage3D>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCopyTextureSubImage3D(static_cast<GLuint>(p->texture()), static_cast<GLint>(p->level()), static_cast<GLint>(p->xoffset()), static_cast<GLint>(p->yoffset()), static_cast<GLint>(p->zoffset()), static_cast<GLint>(p->x()), static_cast<GLint>(p->y()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()));
            return 0u;
        }

        case 191: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCopyTextureSubImage3DEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCopyTextureSubImage3DEXT(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLint>(p->xoffset()), static_cast<GLint>(p->yoffset()), static_cast<GLint>(p->zoffset()), static_cast<GLint>(p->x()), static_cast<GLint>(p->y()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()));
            return 0u;
        }

        case 192: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCoverFillPathInstancedNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCoverFillPathInstancedNV(static_cast<GLsizei>(p->numPaths()), static_cast<GLenum>(p->pathNameType()), ResolveShm(p->paths(), receivedShm, receivedShmCount), static_cast<GLuint>(p->pathBase()), static_cast<GLenum>(p->coverMode()), static_cast<GLenum>(p->transformType()), reinterpret_cast<const GLfloat*>((p->transformValues() == nullptr ? nullptr : p->transformValues()->data())));
            return 0u;
        }

        case 193: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCoverFillPathNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCoverFillPathNV(static_cast<GLuint>(p->path()), static_cast<GLenum>(p->coverMode()));
            return 0u;
        }

        case 194: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCoverStrokePathInstancedNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCoverStrokePathInstancedNV(static_cast<GLsizei>(p->numPaths()), static_cast<GLenum>(p->pathNameType()), ResolveShm(p->paths(), receivedShm, receivedShmCount), static_cast<GLuint>(p->pathBase()), static_cast<GLenum>(p->coverMode()), static_cast<GLenum>(p->transformType()), reinterpret_cast<const GLfloat*>((p->transformValues() == nullptr ? nullptr : p->transformValues()->data())));
            return 0u;
        }

        case 195: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCoverStrokePathNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCoverStrokePathNV(static_cast<GLuint>(p->path()), static_cast<GLenum>(p->coverMode()));
            return 0u;
        }

        case 196: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCoverageModulationNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCoverageModulationNV(static_cast<GLenum>(p->components()));
            return 0u;
        }

        case 197: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCoverageModulationTableNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCoverageModulationTableNV(static_cast<GLsizei>(p->n()), reinterpret_cast<const GLfloat*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 198: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCreateBuffers>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_buffers(static_cast<size_t>((outCapacity != 0 && outCapacity < static_cast<uint32_t>(p->n()) ? outCapacity : static_cast<uint32_t>(p->n()))));
            ::glCreateBuffers(static_cast<GLsizei>(p->n()), reinterpret_cast<GLuint*>(_out_buffers.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_buffers.data()),
                                  static_cast<uint32_t>(_out_buffers.size() * 4));
            return 0u;
        }

        case 199: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCreateCommandListsNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_lists(static_cast<size_t>((outCapacity != 0 && outCapacity < static_cast<uint32_t>(p->n()) ? outCapacity : static_cast<uint32_t>(p->n()))));
            ::glCreateCommandListsNV(static_cast<GLsizei>(p->n()), reinterpret_cast<GLuint*>(_out_lists.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_lists.data()),
                                  static_cast<uint32_t>(_out_lists.size() * 4));
            return 0u;
        }

        case 200: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCreateFramebuffers>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_framebuffers(static_cast<size_t>((outCapacity != 0 && outCapacity < static_cast<uint32_t>(p->n()) ? outCapacity : static_cast<uint32_t>(p->n()))));
            ::glCreateFramebuffers(static_cast<GLsizei>(p->n()), reinterpret_cast<GLuint*>(_out_framebuffers.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_framebuffers.data()),
                                  static_cast<uint32_t>(_out_framebuffers.size() * 4));
            return 0u;
        }

        case 201: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCreatePerfQueryINTEL>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_queryHandle(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glCreatePerfQueryINTEL(static_cast<GLuint>(p->queryId()), reinterpret_cast<GLuint*>(_out_queryHandle.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_queryHandle.data()),
                                  static_cast<uint32_t>(_out_queryHandle.size() * 4));
            return 0u;
        }

        case 203: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCreateProgramPipelines>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_pipelines(static_cast<size_t>((outCapacity != 0 && outCapacity < static_cast<uint32_t>(p->n()) ? outCapacity : static_cast<uint32_t>(p->n()))));
            ::glCreateProgramPipelines(static_cast<GLsizei>(p->n()), reinterpret_cast<GLuint*>(_out_pipelines.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_pipelines.data()),
                                  static_cast<uint32_t>(_out_pipelines.size() * 4));
            return 0u;
        }

        case 204: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCreateQueries>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_ids(static_cast<size_t>((outCapacity != 0 && outCapacity < static_cast<uint32_t>(p->n()) ? outCapacity : static_cast<uint32_t>(p->n()))));
            ::glCreateQueries(static_cast<GLenum>(p->target()), static_cast<GLsizei>(p->n()), reinterpret_cast<GLuint*>(_out_ids.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_ids.data()),
                                  static_cast<uint32_t>(_out_ids.size() * 4));
            return 0u;
        }

        case 205: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCreateRenderbuffers>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_renderbuffers(static_cast<size_t>((outCapacity != 0 && outCapacity < static_cast<uint32_t>(p->n()) ? outCapacity : static_cast<uint32_t>(p->n()))));
            ::glCreateRenderbuffers(static_cast<GLsizei>(p->n()), reinterpret_cast<GLuint*>(_out_renderbuffers.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_renderbuffers.data()),
                                  static_cast<uint32_t>(_out_renderbuffers.size() * 4));
            return 0u;
        }

        case 206: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCreateSamplers>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_samplers(static_cast<size_t>((outCapacity != 0 && outCapacity < static_cast<uint32_t>(p->n()) ? outCapacity : static_cast<uint32_t>(p->n()))));
            ::glCreateSamplers(static_cast<GLsizei>(p->n()), reinterpret_cast<GLuint*>(_out_samplers.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_samplers.data()),
                                  static_cast<uint32_t>(_out_samplers.size() * 4));
            return 0u;
        }

        case 207: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCreateShader>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glCreateShader(static_cast<GLenum>(p->type()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 208: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCreateShaderProgramEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glCreateShaderProgramEXT(static_cast<GLenum>(p->type()), (p->string() == nullptr ? nullptr : p->string()->c_str()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 210: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCreateStatesNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_states(static_cast<size_t>((outCapacity != 0 && outCapacity < static_cast<uint32_t>(p->n()) ? outCapacity : static_cast<uint32_t>(p->n()))));
            ::glCreateStatesNV(static_cast<GLsizei>(p->n()), reinterpret_cast<GLuint*>(_out_states.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_states.data()),
                                  static_cast<uint32_t>(_out_states.size() * 4));
            return 0u;
        }

        case 212: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCreateTextures>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_textures(static_cast<size_t>((outCapacity != 0 && outCapacity < static_cast<uint32_t>(p->n()) ? outCapacity : static_cast<uint32_t>(p->n()))));
            ::glCreateTextures(static_cast<GLenum>(p->target()), static_cast<GLsizei>(p->n()), reinterpret_cast<GLuint*>(_out_textures.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_textures.data()),
                                  static_cast<uint32_t>(_out_textures.size() * 4));
            return 0u;
        }

        case 213: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCreateTransformFeedbacks>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_ids(static_cast<size_t>((outCapacity != 0 && outCapacity < static_cast<uint32_t>(p->n()) ? outCapacity : static_cast<uint32_t>(p->n()))));
            ::glCreateTransformFeedbacks(static_cast<GLsizei>(p->n()), reinterpret_cast<GLuint*>(_out_ids.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_ids.data()),
                                  static_cast<uint32_t>(_out_ids.size() * 4));
            return 0u;
        }

        case 214: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCreateVertexArrays>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_arrays(static_cast<size_t>((outCapacity != 0 && outCapacity < static_cast<uint32_t>(p->n()) ? outCapacity : static_cast<uint32_t>(p->n()))));
            ::glCreateVertexArrays(static_cast<GLsizei>(p->n()), reinterpret_cast<GLuint*>(_out_arrays.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_arrays.data()),
                                  static_cast<uint32_t>(_out_arrays.size() * 4));
            return 0u;
        }

        case 215: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenCullFace>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glCullFace(static_cast<GLenum>(p->mode()));
            return 0u;
        }

        case 218: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDebugMessageControl>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDebugMessageControl(static_cast<GLenum>(p->source()), static_cast<GLenum>(p->type()), static_cast<GLenum>(p->severity()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLuint*>((p->ids() == nullptr ? nullptr : p->ids()->data())), static_cast<GLboolean>(p->enabled()));
            return 0u;
        }

        case 220: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDebugMessageInsert>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDebugMessageInsert(static_cast<GLenum>(p->source()), static_cast<GLenum>(p->type()), static_cast<GLuint>(p->id()), static_cast<GLenum>(p->severity()), static_cast<GLsizei>(p->length()), (p->buf() == nullptr ? nullptr : p->buf()->c_str()));
            return 0u;
        }

        case 222: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDeleteBuffers>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDeleteBuffers(static_cast<GLsizei>(p->n()), reinterpret_cast<const GLuint*>((p->buffers() == nullptr ? nullptr : p->buffers()->data())));
            return 0u;
        }

        case 223: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDeleteCommandListsNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDeleteCommandListsNV(static_cast<GLsizei>(p->n()), reinterpret_cast<const GLuint*>((p->lists() == nullptr ? nullptr : p->lists()->data())));
            return 0u;
        }

        case 224: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDeleteFramebuffers>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDeleteFramebuffers(static_cast<GLsizei>(p->n()), reinterpret_cast<const GLuint*>((p->framebuffers() == nullptr ? nullptr : p->framebuffers()->data())));
            return 0u;
        }

        case 226: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDeleteNamedStringARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDeleteNamedStringARB(static_cast<GLint>(p->namelen()), (p->name() == nullptr ? nullptr : p->name()->c_str()));
            return 0u;
        }

        case 227: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDeletePathsNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDeletePathsNV(static_cast<GLuint>(p->path()), static_cast<GLsizei>(p->range()));
            return 0u;
        }

        case 228: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDeletePerfMonitorsAMD>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_monitors(static_cast<size_t>((outCapacity != 0 && outCapacity < static_cast<uint32_t>(p->n()) ? outCapacity : static_cast<uint32_t>(p->n()))));
            ::glDeletePerfMonitorsAMD(static_cast<GLsizei>(p->n()), reinterpret_cast<GLuint*>(_out_monitors.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_monitors.data()),
                                  static_cast<uint32_t>(_out_monitors.size() * 4));
            return 0u;
        }

        case 229: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDeletePerfQueryINTEL>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDeletePerfQueryINTEL(static_cast<GLuint>(p->queryHandle()));
            return 0u;
        }

        case 230: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDeleteProgram>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDeleteProgram(static_cast<GLuint>(p->program()));
            return 0u;
        }

        case 231: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDeleteProgramPipelines>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDeleteProgramPipelines(static_cast<GLsizei>(p->n()), reinterpret_cast<const GLuint*>((p->pipelines() == nullptr ? nullptr : p->pipelines()->data())));
            return 0u;
        }

        case 232: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDeleteQueries>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDeleteQueries(static_cast<GLsizei>(p->n()), reinterpret_cast<const GLuint*>((p->ids() == nullptr ? nullptr : p->ids()->data())));
            return 0u;
        }

        case 233: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDeleteRenderbuffers>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDeleteRenderbuffers(static_cast<GLsizei>(p->n()), reinterpret_cast<const GLuint*>((p->renderbuffers() == nullptr ? nullptr : p->renderbuffers()->data())));
            return 0u;
        }

        case 235: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDeleteSamplers>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDeleteSamplers(static_cast<GLsizei>(p->count()), reinterpret_cast<const GLuint*>((p->samplers() == nullptr ? nullptr : p->samplers()->data())));
            return 0u;
        }

        case 236: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDeleteShader>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDeleteShader(static_cast<GLuint>(p->shader()));
            return 0u;
        }

        case 237: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDeleteStatesNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDeleteStatesNV(static_cast<GLsizei>(p->n()), reinterpret_cast<const GLuint*>((p->states() == nullptr ? nullptr : p->states()->data())));
            return 0u;
        }

        case 238: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDeleteSync>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDeleteSync(reinterpret_cast<GLsync>(static_cast<std::uintptr_t>(p->sync())));
            return 0u;
        }

        case 239: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDeleteTextures>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDeleteTextures(static_cast<GLsizei>(p->n()), reinterpret_cast<const GLuint*>((p->textures() == nullptr ? nullptr : p->textures()->data())));
            return 0u;
        }

        case 240: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDeleteTransformFeedbacks>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDeleteTransformFeedbacks(static_cast<GLsizei>(p->n()), reinterpret_cast<const GLuint*>((p->ids() == nullptr ? nullptr : p->ids()->data())));
            return 0u;
        }

        case 241: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDeleteVertexArrays>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDeleteVertexArrays(static_cast<GLsizei>(p->n()), reinterpret_cast<const GLuint*>((p->arrays() == nullptr ? nullptr : p->arrays()->data())));
            return 0u;
        }

        case 242: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDepthBoundsdNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDepthBoundsdNV(static_cast<GLdouble>(p->zmin()), static_cast<GLdouble>(p->zmax()));
            return 0u;
        }

        case 243: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDepthFunc>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDepthFunc(static_cast<GLenum>(p->func()));
            return 0u;
        }

        case 244: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDepthMask>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDepthMask(static_cast<GLboolean>(p->flag()));
            return 0u;
        }

        case 245: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDepthRange>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDepthRange(static_cast<GLclampd>(p->near_val()), static_cast<GLclampd>(p->far_val()));
            return 0u;
        }

        case 246: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDepthRangeArraydvNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDepthRangeArraydvNV(static_cast<GLuint>(p->first()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLdouble*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 247: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDepthRangeArrayv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDepthRangeArrayv(static_cast<GLuint>(p->first()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLdouble*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 248: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDepthRangeIndexed>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDepthRangeIndexed(static_cast<GLuint>(p->index()), static_cast<GLdouble>(p->n()), static_cast<GLdouble>(p->f()));
            return 0u;
        }

        case 249: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDepthRangeIndexeddNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDepthRangeIndexeddNV(static_cast<GLuint>(p->index()), static_cast<GLdouble>(p->n()), static_cast<GLdouble>(p->f()));
            return 0u;
        }

        case 250: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDepthRangedNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDepthRangedNV(static_cast<GLdouble>(p->zNear()), static_cast<GLdouble>(p->zFar()));
            return 0u;
        }

        case 251: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDepthRangef>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDepthRangef(static_cast<GLfloat>(p->n()), static_cast<GLfloat>(p->f()));
            return 0u;
        }

        case 252: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDetachShader>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDetachShader(static_cast<GLuint>(p->program()), static_cast<GLuint>(p->shader()));
            return 0u;
        }

        case 253: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDisable>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDisable(static_cast<GLenum>(p->cap()));
            return 0u;
        }

        case 254: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDisableClientStateIndexedEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDisableClientStateIndexedEXT(static_cast<GLenum>(p->array()), static_cast<GLuint>(p->index()));
            return 0u;
        }

        case 255: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDisableClientStateiEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDisableClientStateiEXT(static_cast<GLenum>(p->array()), static_cast<GLuint>(p->index()));
            return 0u;
        }

        case 256: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDisableIndexedEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDisableIndexedEXT(static_cast<GLenum>(p->target()), static_cast<GLuint>(p->index()));
            return 0u;
        }

        case 257: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDisableVertexArrayAttrib>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDisableVertexArrayAttrib(static_cast<GLuint>(p->vaobj()), static_cast<GLuint>(p->index()));
            return 0u;
        }

        case 258: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDisableVertexArrayAttribEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDisableVertexArrayAttribEXT(static_cast<GLuint>(p->vaobj()), static_cast<GLuint>(p->index()));
            return 0u;
        }

        case 259: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDisableVertexArrayEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDisableVertexArrayEXT(static_cast<GLuint>(p->vaobj()), static_cast<GLenum>(p->array()));
            return 0u;
        }

        case 260: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDisableVertexAttribArray>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDisableVertexAttribArray(static_cast<GLuint>(p->index()));
            return 0u;
        }

        case 261: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDisablei>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDisablei(static_cast<GLenum>(p->target()), static_cast<GLuint>(p->index()));
            return 0u;
        }

        case 262: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDispatchCompute>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDispatchCompute(static_cast<GLuint>(p->num_groups_x()), static_cast<GLuint>(p->num_groups_y()), static_cast<GLuint>(p->num_groups_z()));
            return 0u;
        }

        case 263: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDispatchComputeGroupSizeARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDispatchComputeGroupSizeARB(static_cast<GLuint>(p->num_groups_x()), static_cast<GLuint>(p->num_groups_y()), static_cast<GLuint>(p->num_groups_z()), static_cast<GLuint>(p->group_size_x()), static_cast<GLuint>(p->group_size_y()), static_cast<GLuint>(p->group_size_z()));
            return 0u;
        }

        case 264: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDispatchComputeIndirect>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDispatchComputeIndirect(static_cast<GLintptr>(p->indirect()));
            return 0u;
        }

        case 265: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDrawArrays>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDrawArrays(static_cast<GLenum>(p->mode()), static_cast<GLint>(p->first()), static_cast<GLsizei>(p->count()));
            return 0u;
        }

        case 266: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDrawArraysIndirect>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDrawArraysIndirect(static_cast<GLenum>(p->mode()), ResolveShm(p->indirect(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 267: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDrawArraysInstanced>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDrawArraysInstanced(static_cast<GLenum>(p->mode()), static_cast<GLint>(p->first()), static_cast<GLsizei>(p->count()), static_cast<GLsizei>(p->instancecount()));
            return 0u;
        }

        case 269: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDrawArraysInstancedBaseInstance>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDrawArraysInstancedBaseInstance(static_cast<GLenum>(p->mode()), static_cast<GLint>(p->first()), static_cast<GLsizei>(p->count()), static_cast<GLsizei>(p->instancecount()), static_cast<GLuint>(p->baseinstance()));
            return 0u;
        }

        case 271: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDrawBuffer>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDrawBuffer(static_cast<GLenum>(p->mode()));
            return 0u;
        }

        case 272: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDrawBuffers>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDrawBuffers(static_cast<GLsizei>(p->n()), reinterpret_cast<const GLenum*>((p->bufs() == nullptr ? nullptr : p->bufs()->data())));
            return 0u;
        }

        case 273: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDrawCommandsAddressNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDrawCommandsAddressNV(static_cast<GLenum>(p->primitiveMode()), reinterpret_cast<const GLuint64*>((p->indirects() == nullptr ? nullptr : p->indirects()->data())), reinterpret_cast<const GLsizei*>((p->sizes() == nullptr ? nullptr : p->sizes()->data())), static_cast<GLuint>(p->count()));
            return 0u;
        }

        case 275: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDrawCommandsStatesAddressNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDrawCommandsStatesAddressNV(reinterpret_cast<const GLuint64*>((p->indirects() == nullptr ? nullptr : p->indirects()->data())), reinterpret_cast<const GLsizei*>((p->sizes() == nullptr ? nullptr : p->sizes()->data())), reinterpret_cast<const GLuint*>((p->states() == nullptr ? nullptr : p->states()->data())), reinterpret_cast<const GLuint*>((p->fbos() == nullptr ? nullptr : p->fbos()->data())), static_cast<GLuint>(p->count()));
            return 0u;
        }

        case 277: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDrawElements>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDrawElements(static_cast<GLenum>(p->mode()), static_cast<GLsizei>(p->count()), static_cast<GLenum>(p->type()), ResolveShm(p->indices(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 278: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDrawElementsBaseVertex>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDrawElementsBaseVertex(static_cast<GLenum>(p->mode()), static_cast<GLsizei>(p->count()), static_cast<GLenum>(p->type()), ResolveShm(p->indices(), receivedShm, receivedShmCount), static_cast<GLint>(p->basevertex()));
            return 0u;
        }

        case 279: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDrawElementsIndirect>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDrawElementsIndirect(static_cast<GLenum>(p->mode()), static_cast<GLenum>(p->type()), ResolveShm(p->indirect(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 280: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDrawElementsInstanced>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDrawElementsInstanced(static_cast<GLenum>(p->mode()), static_cast<GLsizei>(p->count()), static_cast<GLenum>(p->type()), ResolveShm(p->indices(), receivedShm, receivedShmCount), static_cast<GLsizei>(p->instancecount()));
            return 0u;
        }

        case 282: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDrawElementsInstancedBaseInstance>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDrawElementsInstancedBaseInstance(static_cast<GLenum>(p->mode()), static_cast<GLsizei>(p->count()), static_cast<GLenum>(p->type()), ResolveShm(p->indices(), receivedShm, receivedShmCount), static_cast<GLsizei>(p->instancecount()), static_cast<GLuint>(p->baseinstance()));
            return 0u;
        }

        case 283: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDrawElementsInstancedBaseVertex>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDrawElementsInstancedBaseVertex(static_cast<GLenum>(p->mode()), static_cast<GLsizei>(p->count()), static_cast<GLenum>(p->type()), ResolveShm(p->indices(), receivedShm, receivedShmCount), static_cast<GLsizei>(p->instancecount()), static_cast<GLint>(p->basevertex()));
            return 0u;
        }

        case 284: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDrawElementsInstancedBaseVertexBaseInstance>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDrawElementsInstancedBaseVertexBaseInstance(static_cast<GLenum>(p->mode()), static_cast<GLsizei>(p->count()), static_cast<GLenum>(p->type()), ResolveShm(p->indices(), receivedShm, receivedShmCount), static_cast<GLsizei>(p->instancecount()), static_cast<GLint>(p->basevertex()), static_cast<GLuint>(p->baseinstance()));
            return 0u;
        }

        case 288: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDrawMeshTasksIndirectNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDrawMeshTasksIndirectNV(static_cast<GLintptr>(p->indirect()));
            return 0u;
        }

        case 289: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDrawMeshTasksNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDrawMeshTasksNV(static_cast<GLuint>(p->first()), static_cast<GLuint>(p->count()));
            return 0u;
        }

        case 290: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDrawRangeElements>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDrawRangeElements(static_cast<GLenum>(p->mode()), static_cast<GLuint>(p->start()), static_cast<GLuint>(p->end()), static_cast<GLsizei>(p->count()), static_cast<GLenum>(p->type()), ResolveShm(p->indices(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 291: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDrawRangeElementsBaseVertex>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDrawRangeElementsBaseVertex(static_cast<GLenum>(p->mode()), static_cast<GLuint>(p->start()), static_cast<GLuint>(p->end()), static_cast<GLsizei>(p->count()), static_cast<GLenum>(p->type()), ResolveShm(p->indices(), receivedShm, receivedShmCount), static_cast<GLint>(p->basevertex()));
            return 0u;
        }

        case 292: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDrawTransformFeedback>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDrawTransformFeedback(static_cast<GLenum>(p->mode()), static_cast<GLuint>(p->id()));
            return 0u;
        }

        case 293: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDrawTransformFeedbackInstanced>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDrawTransformFeedbackInstanced(static_cast<GLenum>(p->mode()), static_cast<GLuint>(p->id()), static_cast<GLsizei>(p->instancecount()));
            return 0u;
        }

        case 294: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDrawTransformFeedbackStream>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDrawTransformFeedbackStream(static_cast<GLenum>(p->mode()), static_cast<GLuint>(p->id()), static_cast<GLuint>(p->stream()));
            return 0u;
        }

        case 295: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDrawTransformFeedbackStreamInstanced>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDrawTransformFeedbackStreamInstanced(static_cast<GLenum>(p->mode()), static_cast<GLuint>(p->id()), static_cast<GLuint>(p->stream()), static_cast<GLsizei>(p->instancecount()));
            return 0u;
        }

        case 296: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenDrawVkImageNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glDrawVkImageNV(static_cast<GLuint64>(p->vkImage()), static_cast<GLuint>(p->sampler()), static_cast<GLfloat>(p->x0()), static_cast<GLfloat>(p->y0()), static_cast<GLfloat>(p->x1()), static_cast<GLfloat>(p->y1()), static_cast<GLfloat>(p->z()), static_cast<GLfloat>(p->s0()), static_cast<GLfloat>(p->t0()), static_cast<GLfloat>(p->s1()), static_cast<GLfloat>(p->t1()));
            return 0u;
        }

        case 299: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEdgeFlagFormatNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glEdgeFlagFormatNV(static_cast<GLsizei>(p->stride()));
            return 0u;
        }

        case 300: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEnable>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glEnable(static_cast<GLenum>(p->cap()));
            return 0u;
        }

        case 301: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEnableClientStateIndexedEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glEnableClientStateIndexedEXT(static_cast<GLenum>(p->array()), static_cast<GLuint>(p->index()));
            return 0u;
        }

        case 302: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEnableClientStateiEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glEnableClientStateiEXT(static_cast<GLenum>(p->array()), static_cast<GLuint>(p->index()));
            return 0u;
        }

        case 303: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEnableIndexedEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glEnableIndexedEXT(static_cast<GLenum>(p->target()), static_cast<GLuint>(p->index()));
            return 0u;
        }

        case 304: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEnableVertexArrayAttrib>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glEnableVertexArrayAttrib(static_cast<GLuint>(p->vaobj()), static_cast<GLuint>(p->index()));
            return 0u;
        }

        case 305: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEnableVertexArrayAttribEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glEnableVertexArrayAttribEXT(static_cast<GLuint>(p->vaobj()), static_cast<GLuint>(p->index()));
            return 0u;
        }

        case 306: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEnableVertexArrayEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glEnableVertexArrayEXT(static_cast<GLuint>(p->vaobj()), static_cast<GLenum>(p->array()));
            return 0u;
        }

        case 307: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEnableVertexAttribArray>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glEnableVertexAttribArray(static_cast<GLuint>(p->index()));
            return 0u;
        }

        case 308: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEnablei>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glEnablei(static_cast<GLenum>(p->target()), static_cast<GLuint>(p->index()));
            return 0u;
        }

        case 311: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEndPerfMonitorAMD>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glEndPerfMonitorAMD(static_cast<GLuint>(p->monitor()));
            return 0u;
        }

        case 312: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEndPerfQueryINTEL>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glEndPerfQueryINTEL(static_cast<GLuint>(p->queryHandle()));
            return 0u;
        }

        case 313: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEndQuery>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glEndQuery(static_cast<GLenum>(p->target()));
            return 0u;
        }

        case 314: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenEndQueryIndexed>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glEndQueryIndexed(static_cast<GLenum>(p->target()), static_cast<GLuint>(p->index()));
            return 0u;
        }

        case 317: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenFenceSync>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glFenceSync(static_cast<GLenum>(p->condition()), static_cast<GLbitfield>(p->flags()));
              WireDispatchStoreU64(reinterpret_cast<std::uintptr_t>(_ret)); }
            return 0u;
        }

        case 320: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenFlushMappedBufferRange>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glFlushMappedBufferRange(static_cast<GLenum>(p->target()), static_cast<GLintptr>(p->offset()), static_cast<GLsizeiptr>(p->length()));
            return 0u;
        }

        case 321: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenFlushMappedNamedBufferRange>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glFlushMappedNamedBufferRange(static_cast<GLuint>(p->buffer()), static_cast<GLintptr>(p->offset()), static_cast<GLsizeiptr>(p->length()));
            return 0u;
        }

        case 322: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenFlushMappedNamedBufferRangeEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glFlushMappedNamedBufferRangeEXT(static_cast<GLuint>(p->buffer()), static_cast<GLintptr>(p->offset()), static_cast<GLsizeiptr>(p->length()));
            return 0u;
        }

        case 323: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenFogCoordFormatNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glFogCoordFormatNV(static_cast<GLenum>(p->type()), static_cast<GLsizei>(p->stride()));
            return 0u;
        }

        case 324: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenFragmentCoverageColorNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glFragmentCoverageColorNV(static_cast<GLuint>(p->color()));
            return 0u;
        }

        case 325: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenFramebufferDrawBufferEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glFramebufferDrawBufferEXT(static_cast<GLuint>(p->framebuffer()), static_cast<GLenum>(p->mode()));
            return 0u;
        }

        case 326: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenFramebufferDrawBuffersEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glFramebufferDrawBuffersEXT(static_cast<GLuint>(p->framebuffer()), static_cast<GLsizei>(p->n()), reinterpret_cast<const GLenum*>((p->bufs() == nullptr ? nullptr : p->bufs()->data())));
            return 0u;
        }

        case 328: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenFramebufferParameteri>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glFramebufferParameteri(static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), static_cast<GLint>(p->param()));
            return 0u;
        }

        case 329: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenFramebufferParameteriMESA>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glFramebufferParameteriMESA(static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), static_cast<GLint>(p->param()));
            return 0u;
        }

        case 330: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenFramebufferReadBufferEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glFramebufferReadBufferEXT(static_cast<GLuint>(p->framebuffer()), static_cast<GLenum>(p->mode()));
            return 0u;
        }

        case 331: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenFramebufferRenderbuffer>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glFramebufferRenderbuffer(static_cast<GLenum>(p->target()), static_cast<GLenum>(p->attachment()), static_cast<GLenum>(p->renderbuffertarget()), static_cast<GLuint>(p->renderbuffer()));
            return 0u;
        }

        case 333: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenFramebufferSampleLocationsfvARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glFramebufferSampleLocationsfvARB(static_cast<GLenum>(p->target()), static_cast<GLuint>(p->start()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLfloat*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 334: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenFramebufferSampleLocationsfvNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glFramebufferSampleLocationsfvNV(static_cast<GLenum>(p->target()), static_cast<GLuint>(p->start()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLfloat*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 336: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenFramebufferTexture>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glFramebufferTexture(static_cast<GLenum>(p->target()), static_cast<GLenum>(p->attachment()), static_cast<GLuint>(p->texture()), static_cast<GLint>(p->level()));
            return 0u;
        }

        case 337: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenFramebufferTexture1D>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glFramebufferTexture1D(static_cast<GLenum>(p->target()), static_cast<GLenum>(p->attachment()), static_cast<GLenum>(p->textarget()), static_cast<GLuint>(p->texture()), static_cast<GLint>(p->level()));
            return 0u;
        }

        case 338: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenFramebufferTexture1DEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glFramebufferTexture1DEXT(static_cast<GLenum>(p->target()), static_cast<GLenum>(p->attachment()), static_cast<GLenum>(p->textarget()), static_cast<GLuint>(p->texture()), static_cast<GLint>(p->level()));
            return 0u;
        }

        case 339: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenFramebufferTexture2D>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glFramebufferTexture2D(static_cast<GLenum>(p->target()), static_cast<GLenum>(p->attachment()), static_cast<GLenum>(p->textarget()), static_cast<GLuint>(p->texture()), static_cast<GLint>(p->level()));
            return 0u;
        }

        case 341: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenFramebufferTexture3D>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glFramebufferTexture3D(static_cast<GLenum>(p->target()), static_cast<GLenum>(p->attachment()), static_cast<GLenum>(p->textarget()), static_cast<GLuint>(p->texture()), static_cast<GLint>(p->level()), static_cast<GLint>(p->zoffset()));
            return 0u;
        }

        case 342: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenFramebufferTexture3DEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glFramebufferTexture3DEXT(static_cast<GLenum>(p->target()), static_cast<GLenum>(p->attachment()), static_cast<GLenum>(p->textarget()), static_cast<GLuint>(p->texture()), static_cast<GLint>(p->level()), static_cast<GLint>(p->zoffset()));
            return 0u;
        }

        case 344: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenFramebufferTextureFaceARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glFramebufferTextureFaceARB(static_cast<GLenum>(p->target()), static_cast<GLenum>(p->attachment()), static_cast<GLuint>(p->texture()), static_cast<GLint>(p->level()), static_cast<GLenum>(p->face()));
            return 0u;
        }

        case 345: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenFramebufferTextureLayer>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glFramebufferTextureLayer(static_cast<GLenum>(p->target()), static_cast<GLenum>(p->attachment()), static_cast<GLuint>(p->texture()), static_cast<GLint>(p->level()), static_cast<GLint>(p->layer()));
            return 0u;
        }

        case 347: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenFramebufferTextureMultiviewOVR>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glFramebufferTextureMultiviewOVR(static_cast<GLenum>(p->target()), static_cast<GLenum>(p->attachment()), static_cast<GLuint>(p->texture()), static_cast<GLint>(p->level()), static_cast<GLint>(p->baseViewIndex()), static_cast<GLsizei>(p->numViews()));
            return 0u;
        }

        case 348: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenFrontFace>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glFrontFace(static_cast<GLenum>(p->mode()));
            return 0u;
        }

        case 349: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGenBuffers>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_buffers(static_cast<size_t>((outCapacity != 0 && outCapacity < static_cast<uint32_t>(p->n()) ? outCapacity : static_cast<uint32_t>(p->n()))));
            ::glGenBuffers(static_cast<GLsizei>(p->n()), reinterpret_cast<GLuint*>(_out_buffers.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_buffers.data()),
                                  static_cast<uint32_t>(_out_buffers.size() * 4));
            return 0u;
        }

        case 350: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGenFramebuffers>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_framebuffers(static_cast<size_t>((outCapacity != 0 && outCapacity < static_cast<uint32_t>(p->n()) ? outCapacity : static_cast<uint32_t>(p->n()))));
            ::glGenFramebuffers(static_cast<GLsizei>(p->n()), reinterpret_cast<GLuint*>(_out_framebuffers.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_framebuffers.data()),
                                  static_cast<uint32_t>(_out_framebuffers.size() * 4));
            return 0u;
        }

        case 352: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGenPathsNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glGenPathsNV(static_cast<GLsizei>(p->range()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 353: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGenPerfMonitorsAMD>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_monitors(static_cast<size_t>((outCapacity != 0 && outCapacity < static_cast<uint32_t>(p->n()) ? outCapacity : static_cast<uint32_t>(p->n()))));
            ::glGenPerfMonitorsAMD(static_cast<GLsizei>(p->n()), reinterpret_cast<GLuint*>(_out_monitors.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_monitors.data()),
                                  static_cast<uint32_t>(_out_monitors.size() * 4));
            return 0u;
        }

        case 354: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGenProgramPipelines>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_pipelines(static_cast<size_t>((outCapacity != 0 && outCapacity < static_cast<uint32_t>(p->n()) ? outCapacity : static_cast<uint32_t>(p->n()))));
            ::glGenProgramPipelines(static_cast<GLsizei>(p->n()), reinterpret_cast<GLuint*>(_out_pipelines.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_pipelines.data()),
                                  static_cast<uint32_t>(_out_pipelines.size() * 4));
            return 0u;
        }

        case 355: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGenQueries>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_ids(static_cast<size_t>((outCapacity != 0 && outCapacity < static_cast<uint32_t>(p->n()) ? outCapacity : static_cast<uint32_t>(p->n()))));
            ::glGenQueries(static_cast<GLsizei>(p->n()), reinterpret_cast<GLuint*>(_out_ids.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_ids.data()),
                                  static_cast<uint32_t>(_out_ids.size() * 4));
            return 0u;
        }

        case 356: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGenRenderbuffers>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_renderbuffers(static_cast<size_t>((outCapacity != 0 && outCapacity < static_cast<uint32_t>(p->n()) ? outCapacity : static_cast<uint32_t>(p->n()))));
            ::glGenRenderbuffers(static_cast<GLsizei>(p->n()), reinterpret_cast<GLuint*>(_out_renderbuffers.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_renderbuffers.data()),
                                  static_cast<uint32_t>(_out_renderbuffers.size() * 4));
            return 0u;
        }

        case 358: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGenSamplers>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_samplers(static_cast<size_t>((outCapacity != 0 && outCapacity < static_cast<uint32_t>(p->count()) ? outCapacity : static_cast<uint32_t>(p->count()))));
            ::glGenSamplers(static_cast<GLsizei>(p->count()), reinterpret_cast<GLuint*>(_out_samplers.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_samplers.data()),
                                  static_cast<uint32_t>(_out_samplers.size() * 4));
            return 0u;
        }

        case 359: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGenTextures>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_textures(static_cast<size_t>((outCapacity != 0 && outCapacity < static_cast<uint32_t>(p->n()) ? outCapacity : static_cast<uint32_t>(p->n()))));
            ::glGenTextures(static_cast<GLsizei>(p->n()), reinterpret_cast<GLuint*>(_out_textures.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_textures.data()),
                                  static_cast<uint32_t>(_out_textures.size() * 4));
            return 0u;
        }

        case 360: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGenTransformFeedbacks>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_ids(static_cast<size_t>((outCapacity != 0 && outCapacity < static_cast<uint32_t>(p->n()) ? outCapacity : static_cast<uint32_t>(p->n()))));
            ::glGenTransformFeedbacks(static_cast<GLsizei>(p->n()), reinterpret_cast<GLuint*>(_out_ids.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_ids.data()),
                                  static_cast<uint32_t>(_out_ids.size() * 4));
            return 0u;
        }

        case 361: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGenVertexArrays>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_arrays(static_cast<size_t>((outCapacity != 0 && outCapacity < static_cast<uint32_t>(p->n()) ? outCapacity : static_cast<uint32_t>(p->n()))));
            ::glGenVertexArrays(static_cast<GLsizei>(p->n()), reinterpret_cast<GLuint*>(_out_arrays.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_arrays.data()),
                                  static_cast<uint32_t>(_out_arrays.size() * 4));
            return 0u;
        }

        case 362: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGenerateMipmap>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glGenerateMipmap(static_cast<GLenum>(p->target()));
            return 0u;
        }

        case 364: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGenerateMultiTexMipmapEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glGenerateMultiTexMipmapEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->target()));
            return 0u;
        }

        case 365: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGenerateTextureMipmap>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glGenerateTextureMipmap(static_cast<GLuint>(p->texture()));
            return 0u;
        }

        case 366: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGenerateTextureMipmapEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glGenerateTextureMipmapEXT(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->target()));
            return 0u;
        }

        case 367: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetActiveAtomicCounterBufferiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetActiveAtomicCounterBufferiv(static_cast<GLuint>(p->program()), static_cast<GLuint>(p->bufferIndex()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 371: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetActiveSubroutineUniformiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_values(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetActiveSubroutineUniformiv(static_cast<GLuint>(p->program()), static_cast<GLenum>(p->shadertype()), static_cast<GLuint>(p->index()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_values.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_values.data()),
                                  static_cast<uint32_t>(_out_values.size() * 4));
            return 0u;
        }

        case 374: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetActiveUniformBlockiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetActiveUniformBlockiv(static_cast<GLuint>(p->program()), static_cast<GLuint>(p->uniformBlockIndex()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 376: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetActiveUniformsiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetActiveUniformsiv(static_cast<GLuint>(p->program()), static_cast<GLsizei>(p->uniformCount()), reinterpret_cast<const GLuint*>((p->uniformIndices() == nullptr ? nullptr : p->uniformIndices()->data())), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 377: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetAttachedShaders>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_count(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            std::vector<uint32_t>_out_shaders(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetAttachedShaders(static_cast<GLuint>(p->program()), static_cast<GLsizei>(p->maxCount()), reinterpret_cast<GLsizei*>(_out_count.data()), reinterpret_cast<GLuint*>(_out_shaders.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_count.data()),
                                  static_cast<uint32_t>(_out_count.size() * 4));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_shaders.data()),
                                  static_cast<uint32_t>(_out_shaders.size() * 4));
            return 0u;
        }

        case 378: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetAttribLocation>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glGetAttribLocation(static_cast<GLuint>(p->program()), (p->name() == nullptr ? nullptr : p->name()->c_str()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 379: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetBooleanIndexedvEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint8_t>_out_data(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetBooleanIndexedvEXT(static_cast<GLenum>(p->target()), static_cast<GLuint>(p->index()), reinterpret_cast<GLboolean*>(_out_data.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_data.data()),
                                  static_cast<uint32_t>(_out_data.size() * 1));
            return 0u;
        }

        case 380: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetBooleani_v>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint8_t>_out_data(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetBooleani_v(static_cast<GLenum>(p->target()), static_cast<GLuint>(p->index()), reinterpret_cast<GLboolean*>(_out_data.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_data.data()),
                                  static_cast<uint32_t>(_out_data.size() * 1));
            return 0u;
        }

        case 381: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetBooleanv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint8_t>_out_data(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetBooleanv(static_cast<GLenum>(p->pname()), reinterpret_cast<GLboolean*>(_out_data.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_data.data()),
                                  static_cast<uint32_t>(_out_data.size() * 1));
            return 0u;
        }

        case 382: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetBufferParameteri64v>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int64_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetBufferParameteri64v(static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint64*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 8));
            return 0u;
        }

        case 383: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetBufferParameteriv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetBufferParameteriv(static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 384: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetBufferParameterui64vNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint64_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetBufferParameterui64vNV(static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLuint64EXT*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 8));
            return 0u;
        }

        case 386: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetBufferSubData>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glGetBufferSubData(static_cast<GLenum>(p->target()), static_cast<GLintptr>(p->offset()), static_cast<GLsizeiptr>(p->size()), const_cast<void*>(ResolveShm(p->data(), receivedShm, receivedShmCount)));
            return 0u;
        }

        case 387: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetCommandHeaderNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glGetCommandHeaderNV(static_cast<GLenum>(p->tokenID()), static_cast<GLuint>(p->size()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 388: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetCompressedMultiTexImageEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glGetCompressedMultiTexImageEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->target()), static_cast<GLint>(p->lod()), const_cast<void*>(ResolveShm(p->img(), receivedShm, receivedShmCount)));
            return 0u;
        }

        case 389: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetCompressedTexImage>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glGetCompressedTexImage(static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), const_cast<void*>(ResolveShm(p->img(), receivedShm, receivedShmCount)));
            return 0u;
        }

        case 390: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetCompressedTextureImage>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glGetCompressedTextureImage(static_cast<GLuint>(p->texture()), static_cast<GLint>(p->level()), static_cast<GLsizei>(p->bufSize()), const_cast<void*>(ResolveShm(p->pixels(), receivedShm, receivedShmCount)));
            return 0u;
        }

        case 391: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetCompressedTextureImageEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glGetCompressedTextureImageEXT(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->target()), static_cast<GLint>(p->lod()), const_cast<void*>(ResolveShm(p->img(), receivedShm, receivedShmCount)));
            return 0u;
        }

        case 392: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetCompressedTextureSubImage>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glGetCompressedTextureSubImage(static_cast<GLuint>(p->texture()), static_cast<GLint>(p->level()), static_cast<GLint>(p->xoffset()), static_cast<GLint>(p->yoffset()), static_cast<GLint>(p->zoffset()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLsizei>(p->depth()), static_cast<GLsizei>(p->bufSize()), const_cast<void*>(ResolveShm(p->pixels(), receivedShm, receivedShmCount)));
            return 0u;
        }

        case 393: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetCoverageModulationTableNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<float>_out_v(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetCoverageModulationTableNV(static_cast<GLsizei>(p->bufSize()), reinterpret_cast<GLfloat*>(_out_v.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_v.data()),
                                  static_cast<uint32_t>(_out_v.size() * 4));
            return 0u;
        }

        case 396: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetDoubleIndexedvEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<double>_out_data(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetDoubleIndexedvEXT(static_cast<GLenum>(p->target()), static_cast<GLuint>(p->index()), reinterpret_cast<GLdouble*>(_out_data.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_data.data()),
                                  static_cast<uint32_t>(_out_data.size() * 8));
            return 0u;
        }

        case 397: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetDoublei_v>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<double>_out_data(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetDoublei_v(static_cast<GLenum>(p->target()), static_cast<GLuint>(p->index()), reinterpret_cast<GLdouble*>(_out_data.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_data.data()),
                                  static_cast<uint32_t>(_out_data.size() * 8));
            return 0u;
        }

        case 398: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetDoublei_vEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<double>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetDoublei_vEXT(static_cast<GLenum>(p->pname()), static_cast<GLuint>(p->index()), reinterpret_cast<GLdouble*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 8));
            return 0u;
        }

        case 399: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetDoublev>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<double>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetDoublev(static_cast<GLenum>(p->pname()), reinterpret_cast<GLdouble*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 8));
            return 0u;
        }

        case 401: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetFirstPerfQueryIdINTEL>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_queryId(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetFirstPerfQueryIdINTEL(reinterpret_cast<GLuint*>(_out_queryId.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_queryId.data()),
                                  static_cast<uint32_t>(_out_queryId.size() * 4));
            return 0u;
        }

        case 402: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetFloatIndexedvEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<float>_out_data(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetFloatIndexedvEXT(static_cast<GLenum>(p->target()), static_cast<GLuint>(p->index()), reinterpret_cast<GLfloat*>(_out_data.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_data.data()),
                                  static_cast<uint32_t>(_out_data.size() * 4));
            return 0u;
        }

        case 403: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetFloati_v>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<float>_out_data(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetFloati_v(static_cast<GLenum>(p->target()), static_cast<GLuint>(p->index()), reinterpret_cast<GLfloat*>(_out_data.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_data.data()),
                                  static_cast<uint32_t>(_out_data.size() * 4));
            return 0u;
        }

        case 404: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetFloati_vEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<float>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetFloati_vEXT(static_cast<GLenum>(p->pname()), static_cast<GLuint>(p->index()), reinterpret_cast<GLfloat*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 405: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetFloatv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<float>_out_data(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetFloatv(static_cast<GLenum>(p->pname()), reinterpret_cast<GLfloat*>(_out_data.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_data.data()),
                                  static_cast<uint32_t>(_out_data.size() * 4));
            return 0u;
        }

        case 406: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetFragDataIndex>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glGetFragDataIndex(static_cast<GLuint>(p->program()), (p->name() == nullptr ? nullptr : p->name()->c_str()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 407: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetFragDataLocation>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glGetFragDataLocation(static_cast<GLuint>(p->program()), (p->name() == nullptr ? nullptr : p->name()->c_str()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 409: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetFramebufferAttachmentParameteriv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetFramebufferAttachmentParameteriv(static_cast<GLenum>(p->target()), static_cast<GLenum>(p->attachment()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 411: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetFramebufferParameteriv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetFramebufferParameteriv(static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 413: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetFramebufferParameterivMESA>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetFramebufferParameterivMESA(static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 416: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetImageHandleARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glGetImageHandleARB(static_cast<GLuint>(p->texture()), static_cast<GLint>(p->level()), static_cast<GLboolean>(p->layered()), static_cast<GLint>(p->layer()), static_cast<GLenum>(p->format()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 417: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetImageHandleNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glGetImageHandleNV(static_cast<GLuint>(p->texture()), static_cast<GLint>(p->level()), static_cast<GLboolean>(p->layered()), static_cast<GLint>(p->layer()), static_cast<GLenum>(p->format()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 418: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetInteger64i_v>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int64_t>_out_data(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetInteger64i_v(static_cast<GLenum>(p->target()), static_cast<GLuint>(p->index()), reinterpret_cast<GLint64*>(_out_data.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_data.data()),
                                  static_cast<uint32_t>(_out_data.size() * 8));
            return 0u;
        }

        case 419: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetInteger64v>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int64_t>_out_data(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetInteger64v(static_cast<GLenum>(p->pname()), reinterpret_cast<GLint64*>(_out_data.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_data.data()),
                                  static_cast<uint32_t>(_out_data.size() * 8));
            return 0u;
        }

        case 420: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetIntegerIndexedvEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_data(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetIntegerIndexedvEXT(static_cast<GLenum>(p->target()), static_cast<GLuint>(p->index()), reinterpret_cast<GLint*>(_out_data.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_data.data()),
                                  static_cast<uint32_t>(_out_data.size() * 4));
            return 0u;
        }

        case 421: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetIntegeri_v>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_data(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetIntegeri_v(static_cast<GLenum>(p->target()), static_cast<GLuint>(p->index()), reinterpret_cast<GLint*>(_out_data.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_data.data()),
                                  static_cast<uint32_t>(_out_data.size() * 4));
            return 0u;
        }

        case 422: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetIntegerui64i_vNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint64_t>_out_result(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetIntegerui64i_vNV(static_cast<GLenum>(p->value()), static_cast<GLuint>(p->index()), reinterpret_cast<GLuint64EXT*>(_out_result.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_result.data()),
                                  static_cast<uint32_t>(_out_result.size() * 8));
            return 0u;
        }

        case 423: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetIntegerui64vNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint64_t>_out_result(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetIntegerui64vNV(static_cast<GLenum>(p->value()), reinterpret_cast<GLuint64EXT*>(_out_result.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_result.data()),
                                  static_cast<uint32_t>(_out_result.size() * 8));
            return 0u;
        }

        case 424: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetIntegerv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_data(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetIntegerv(static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_data.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_data.data()),
                                  static_cast<uint32_t>(_out_data.size() * 4));
            return 0u;
        }

        case 425: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetInternalformatSampleivNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 && outCapacity < static_cast<uint32_t>(p->count()) ? outCapacity : static_cast<uint32_t>(p->count()))));
            ::glGetInternalformatSampleivNV(static_cast<GLenum>(p->target()), static_cast<GLenum>(p->internalformat()), static_cast<GLsizei>(p->samples()), static_cast<GLenum>(p->pname()), static_cast<GLsizei>(p->count()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 426: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetInternalformati64v>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int64_t>_out_params(static_cast<size_t>((outCapacity != 0 && outCapacity < static_cast<uint32_t>(p->count()) ? outCapacity : static_cast<uint32_t>(p->count()))));
            ::glGetInternalformati64v(static_cast<GLenum>(p->target()), static_cast<GLenum>(p->internalformat()), static_cast<GLenum>(p->pname()), static_cast<GLsizei>(p->count()), reinterpret_cast<GLint64*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 8));
            return 0u;
        }

        case 427: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetInternalformativ>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetInternalformativ(static_cast<GLenum>(p->target()), static_cast<GLenum>(p->internalformat()), static_cast<GLenum>(p->pname()), static_cast<GLsizei>(p->bufSize()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 428: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetMemoryObjectDetachedResourcesuivNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_params(static_cast<size_t>((outCapacity != 0 && outCapacity < static_cast<uint32_t>(p->count()) ? outCapacity : static_cast<uint32_t>(p->count()))));
            ::glGetMemoryObjectDetachedResourcesuivNV(static_cast<GLuint>(p->memory()), static_cast<GLenum>(p->pname()), static_cast<GLint>(p->first()), static_cast<GLsizei>(p->count()), reinterpret_cast<GLuint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 429: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetMultiTexEnvfvEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<float>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetMultiTexEnvfvEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLfloat*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 430: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetMultiTexEnvivEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetMultiTexEnvivEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 431: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetMultiTexGendvEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<double>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetMultiTexGendvEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->coord()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLdouble*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 8));
            return 0u;
        }

        case 432: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetMultiTexGenfvEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<float>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetMultiTexGenfvEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->coord()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLfloat*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 433: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetMultiTexGenivEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetMultiTexGenivEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->coord()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 434: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetMultiTexImageEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glGetMultiTexImageEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLenum>(p->format()), static_cast<GLenum>(p->type()), const_cast<void*>(ResolveShm(p->pixels(), receivedShm, receivedShmCount)));
            return 0u;
        }

        case 435: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetMultiTexLevelParameterfvEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<float>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetMultiTexLevelParameterfvEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLfloat*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 436: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetMultiTexLevelParameterivEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetMultiTexLevelParameterivEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 437: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetMultiTexParameterIivEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetMultiTexParameterIivEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 438: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetMultiTexParameterIuivEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetMultiTexParameterIuivEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLuint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 439: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetMultiTexParameterfvEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<float>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetMultiTexParameterfvEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLfloat*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 440: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetMultiTexParameterivEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetMultiTexParameterivEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 441: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetMultisamplefv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<float>_out_val(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetMultisamplefv(static_cast<GLenum>(p->pname()), static_cast<GLuint>(p->index()), reinterpret_cast<GLfloat*>(_out_val.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_val.data()),
                                  static_cast<uint32_t>(_out_val.size() * 4));
            return 0u;
        }

        case 442: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetNamedBufferParameteri64v>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int64_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetNamedBufferParameteri64v(static_cast<GLuint>(p->buffer()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint64*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 8));
            return 0u;
        }

        case 443: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetNamedBufferParameteriv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetNamedBufferParameteriv(static_cast<GLuint>(p->buffer()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 444: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetNamedBufferParameterivEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetNamedBufferParameterivEXT(static_cast<GLuint>(p->buffer()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 445: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetNamedBufferParameterui64vNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint64_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetNamedBufferParameterui64vNV(static_cast<GLuint>(p->buffer()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLuint64EXT*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 8));
            return 0u;
        }

        case 448: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetNamedBufferSubData>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glGetNamedBufferSubData(static_cast<GLuint>(p->buffer()), static_cast<GLintptr>(p->offset()), static_cast<GLsizeiptr>(p->size()), const_cast<void*>(ResolveShm(p->data(), receivedShm, receivedShmCount)));
            return 0u;
        }

        case 449: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetNamedBufferSubDataEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glGetNamedBufferSubDataEXT(static_cast<GLuint>(p->buffer()), static_cast<GLintptr>(p->offset()), static_cast<GLsizeiptr>(p->size()), const_cast<void*>(ResolveShm(p->data(), receivedShm, receivedShmCount)));
            return 0u;
        }

        case 450: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetNamedFramebufferAttachmentParameteriv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetNamedFramebufferAttachmentParameteriv(static_cast<GLuint>(p->framebuffer()), static_cast<GLenum>(p->attachment()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 451: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetNamedFramebufferAttachmentParameterivEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetNamedFramebufferAttachmentParameterivEXT(static_cast<GLuint>(p->framebuffer()), static_cast<GLenum>(p->attachment()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 452: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetNamedFramebufferParameteriv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_param(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetNamedFramebufferParameteriv(static_cast<GLuint>(p->framebuffer()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_param.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_param.data()),
                                  static_cast<uint32_t>(_out_param.size() * 4));
            return 0u;
        }

        case 453: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetNamedFramebufferParameterivEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetNamedFramebufferParameterivEXT(static_cast<GLuint>(p->framebuffer()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 454: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetNamedProgramLocalParameterIivEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetNamedProgramLocalParameterIivEXT(static_cast<GLuint>(p->program()), static_cast<GLenum>(p->target()), static_cast<GLuint>(p->index()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 455: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetNamedProgramLocalParameterIuivEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetNamedProgramLocalParameterIuivEXT(static_cast<GLuint>(p->program()), static_cast<GLenum>(p->target()), static_cast<GLuint>(p->index()), reinterpret_cast<GLuint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 456: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetNamedProgramLocalParameterdvEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<double>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetNamedProgramLocalParameterdvEXT(static_cast<GLuint>(p->program()), static_cast<GLenum>(p->target()), static_cast<GLuint>(p->index()), reinterpret_cast<GLdouble*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 8));
            return 0u;
        }

        case 457: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetNamedProgramLocalParameterfvEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<float>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetNamedProgramLocalParameterfvEXT(static_cast<GLuint>(p->program()), static_cast<GLenum>(p->target()), static_cast<GLuint>(p->index()), reinterpret_cast<GLfloat*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 458: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetNamedProgramStringEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glGetNamedProgramStringEXT(static_cast<GLuint>(p->program()), static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), const_cast<void*>(ResolveShm(p->string(), receivedShm, receivedShmCount)));
            return 0u;
        }

        case 459: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetNamedProgramivEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetNamedProgramivEXT(static_cast<GLuint>(p->program()), static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 460: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetNamedRenderbufferParameteriv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetNamedRenderbufferParameteriv(static_cast<GLuint>(p->renderbuffer()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 461: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetNamedRenderbufferParameterivEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetNamedRenderbufferParameterivEXT(static_cast<GLuint>(p->renderbuffer()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 463: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetNamedStringivARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetNamedStringivARB(static_cast<GLint>(p->namelen()), (p->name() == nullptr ? nullptr : p->name()->c_str()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 464: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetNextPerfQueryIdINTEL>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_nextQueryId(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetNextPerfQueryIdINTEL(static_cast<GLuint>(p->queryId()), reinterpret_cast<GLuint*>(_out_nextQueryId.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_nextQueryId.data()),
                                  static_cast<uint32_t>(_out_nextQueryId.size() * 4));
            return 0u;
        }

        case 468: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetPathCommandsNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_commands(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetPathCommandsNV(static_cast<GLuint>(p->path()), reinterpret_cast<GLubyte*>(_out_commands.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_commands.data()),
                                  static_cast<uint32_t>(_out_commands.size() * 4));
            return 0u;
        }

        case 469: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetPathCoordsNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<float>_out_coords(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetPathCoordsNV(static_cast<GLuint>(p->path()), reinterpret_cast<GLfloat*>(_out_coords.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_coords.data()),
                                  static_cast<uint32_t>(_out_coords.size() * 4));
            return 0u;
        }

        case 470: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetPathDashArrayNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<float>_out_dashArray(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetPathDashArrayNV(static_cast<GLuint>(p->path()), reinterpret_cast<GLfloat*>(_out_dashArray.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_dashArray.data()),
                                  static_cast<uint32_t>(_out_dashArray.size() * 4));
            return 0u;
        }

        case 471: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetPathLengthNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glGetPathLengthNV(static_cast<GLuint>(p->path()), static_cast<GLsizei>(p->startSegment()), static_cast<GLsizei>(p->numSegments()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 472: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetPathMetricRangeNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<float>_out_metrics(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetPathMetricRangeNV(static_cast<GLbitfield>(p->metricQueryMask()), static_cast<GLuint>(p->firstPathName()), static_cast<GLsizei>(p->numPaths()), static_cast<GLsizei>(p->stride()), reinterpret_cast<GLfloat*>(_out_metrics.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_metrics.data()),
                                  static_cast<uint32_t>(_out_metrics.size() * 4));
            return 0u;
        }

        case 473: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetPathMetricsNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<float>_out_metrics(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetPathMetricsNV(static_cast<GLbitfield>(p->metricQueryMask()), static_cast<GLsizei>(p->numPaths()), static_cast<GLenum>(p->pathNameType()), ResolveShm(p->paths(), receivedShm, receivedShmCount), static_cast<GLuint>(p->pathBase()), static_cast<GLsizei>(p->stride()), reinterpret_cast<GLfloat*>(_out_metrics.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_metrics.data()),
                                  static_cast<uint32_t>(_out_metrics.size() * 4));
            return 0u;
        }

        case 474: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetPathParameterfvNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<float>_out_value(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetPathParameterfvNV(static_cast<GLuint>(p->path()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLfloat*>(_out_value.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_value.data()),
                                  static_cast<uint32_t>(_out_value.size() * 4));
            return 0u;
        }

        case 475: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetPathParameterivNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_value(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetPathParameterivNV(static_cast<GLuint>(p->path()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_value.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_value.data()),
                                  static_cast<uint32_t>(_out_value.size() * 4));
            return 0u;
        }

        case 476: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetPathSpacingNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<float>_out_returnedSpacing(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetPathSpacingNV(static_cast<GLenum>(p->pathListMode()), static_cast<GLsizei>(p->numPaths()), static_cast<GLenum>(p->pathNameType()), ResolveShm(p->paths(), receivedShm, receivedShmCount), static_cast<GLuint>(p->pathBase()), static_cast<GLfloat>(p->advanceScale()), static_cast<GLfloat>(p->kerningScale()), static_cast<GLenum>(p->transformType()), reinterpret_cast<GLfloat*>(_out_returnedSpacing.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_returnedSpacing.data()),
                                  static_cast<uint32_t>(_out_returnedSpacing.size() * 4));
            return 0u;
        }

        case 478: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetPerfMonitorCounterDataAMD>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_data(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            std::vector<int32_t>_out_bytesWritten(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetPerfMonitorCounterDataAMD(static_cast<GLuint>(p->monitor()), static_cast<GLenum>(p->pname()), static_cast<GLsizei>(p->dataSize()), reinterpret_cast<GLuint*>(_out_data.data()), reinterpret_cast<GLint*>(_out_bytesWritten.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_data.data()),
                                  static_cast<uint32_t>(_out_data.size() * 4));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_bytesWritten.data()),
                                  static_cast<uint32_t>(_out_bytesWritten.size() * 4));
            return 0u;
        }

        case 479: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetPerfMonitorCounterInfoAMD>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glGetPerfMonitorCounterInfoAMD(static_cast<GLuint>(p->group()), static_cast<GLuint>(p->counter()), static_cast<GLenum>(p->pname()), const_cast<void*>(ResolveShm(p->data(), receivedShm, receivedShmCount)));
            return 0u;
        }

        case 481: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetPerfMonitorCountersAMD>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_numCounters(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            std::vector<int32_t>_out_maxActiveCounters(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            std::vector<uint32_t>_out_counters(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetPerfMonitorCountersAMD(static_cast<GLuint>(p->group()), reinterpret_cast<GLint*>(_out_numCounters.data()), reinterpret_cast<GLint*>(_out_maxActiveCounters.data()), static_cast<GLsizei>(p->counterSize()), reinterpret_cast<GLuint*>(_out_counters.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_numCounters.data()),
                                  static_cast<uint32_t>(_out_numCounters.size() * 4));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_maxActiveCounters.data()),
                                  static_cast<uint32_t>(_out_maxActiveCounters.size() * 4));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_counters.data()),
                                  static_cast<uint32_t>(_out_counters.size() * 4));
            return 0u;
        }

        case 483: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetPerfMonitorGroupsAMD>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_numGroups(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            std::vector<uint32_t>_out_groups(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetPerfMonitorGroupsAMD(reinterpret_cast<GLint*>(_out_numGroups.data()), static_cast<GLsizei>(p->groupsSize()), reinterpret_cast<GLuint*>(_out_groups.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_numGroups.data()),
                                  static_cast<uint32_t>(_out_numGroups.size() * 4));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_groups.data()),
                                  static_cast<uint32_t>(_out_groups.size() * 4));
            return 0u;
        }

        case 484: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetPerfQueryDataINTEL>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_bytesWritten(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetPerfQueryDataINTEL(static_cast<GLuint>(p->queryHandle()), static_cast<GLuint>(p->flags()), static_cast<GLsizei>(p->dataSize()), const_cast<void*>(ResolveShm(p->data(), receivedShm, receivedShmCount)), reinterpret_cast<GLuint*>(_out_bytesWritten.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_bytesWritten.data()),
                                  static_cast<uint32_t>(_out_bytesWritten.size() * 4));
            return 0u;
        }

        case 490: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetProgramBinary>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_length(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            std::vector<uint32_t>_out_binaryFormat(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetProgramBinary(static_cast<GLuint>(p->program()), static_cast<GLsizei>(p->bufSize()), reinterpret_cast<GLsizei*>(_out_length.data()), reinterpret_cast<GLenum*>(_out_binaryFormat.data()), const_cast<void*>(ResolveShm(p->binary(), receivedShm, receivedShmCount)));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_length.data()),
                                  static_cast<uint32_t>(_out_length.size() * 4));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_binaryFormat.data()),
                                  static_cast<uint32_t>(_out_binaryFormat.size() * 4));
            return 0u;
        }

        case 492: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetProgramInterfaceiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetProgramInterfaceiv(static_cast<GLuint>(p->program()), static_cast<GLenum>(p->programInterface()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 494: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetProgramPipelineiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetProgramPipelineiv(static_cast<GLuint>(p->pipeline()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 495: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetProgramResourceIndex>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glGetProgramResourceIndex(static_cast<GLuint>(p->program()), static_cast<GLenum>(p->programInterface()), (p->name() == nullptr ? nullptr : p->name()->c_str()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 496: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetProgramResourceLocation>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glGetProgramResourceLocation(static_cast<GLuint>(p->program()), static_cast<GLenum>(p->programInterface()), (p->name() == nullptr ? nullptr : p->name()->c_str()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 497: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetProgramResourceLocationIndex>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glGetProgramResourceLocationIndex(static_cast<GLuint>(p->program()), static_cast<GLenum>(p->programInterface()), (p->name() == nullptr ? nullptr : p->name()->c_str()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 499: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetProgramResourcefvNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_length(static_cast<size_t>((outCapacity != 0 && outCapacity < static_cast<uint32_t>(p->count()) ? outCapacity : static_cast<uint32_t>(p->count()))));
            std::vector<float>_out_params(static_cast<size_t>((outCapacity != 0 && outCapacity < static_cast<uint32_t>(p->count()) ? outCapacity : static_cast<uint32_t>(p->count()))));
            ::glGetProgramResourcefvNV(static_cast<GLuint>(p->program()), static_cast<GLenum>(p->programInterface()), static_cast<GLuint>(p->index()), static_cast<GLsizei>(p->propCount()), reinterpret_cast<const GLenum*>((p->props() == nullptr ? nullptr : p->props()->data())), static_cast<GLsizei>(p->count()), reinterpret_cast<GLsizei*>(_out_length.data()), reinterpret_cast<GLfloat*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_length.data()),
                                  static_cast<uint32_t>(_out_length.size() * 4));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 500: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetProgramResourceiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_length(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetProgramResourceiv(static_cast<GLuint>(p->program()), static_cast<GLenum>(p->programInterface()), static_cast<GLuint>(p->index()), static_cast<GLsizei>(p->propCount()), reinterpret_cast<const GLenum*>((p->props() == nullptr ? nullptr : p->props()->data())), static_cast<GLsizei>(p->bufSize()), reinterpret_cast<GLsizei*>(_out_length.data()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_length.data()),
                                  static_cast<uint32_t>(_out_length.size() * 4));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 501: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetProgramStageiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_values(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetProgramStageiv(static_cast<GLuint>(p->program()), static_cast<GLenum>(p->shadertype()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_values.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_values.data()),
                                  static_cast<uint32_t>(_out_values.size() * 4));
            return 0u;
        }

        case 502: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetProgramiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetProgramiv(static_cast<GLuint>(p->program()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 503: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetQueryBufferObjecti64v>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glGetQueryBufferObjecti64v(static_cast<GLuint>(p->id()), static_cast<GLuint>(p->buffer()), static_cast<GLenum>(p->pname()), static_cast<GLintptr>(p->offset()));
            return 0u;
        }

        case 504: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetQueryBufferObjectiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glGetQueryBufferObjectiv(static_cast<GLuint>(p->id()), static_cast<GLuint>(p->buffer()), static_cast<GLenum>(p->pname()), static_cast<GLintptr>(p->offset()));
            return 0u;
        }

        case 505: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetQueryBufferObjectui64v>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glGetQueryBufferObjectui64v(static_cast<GLuint>(p->id()), static_cast<GLuint>(p->buffer()), static_cast<GLenum>(p->pname()), static_cast<GLintptr>(p->offset()));
            return 0u;
        }

        case 506: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetQueryBufferObjectuiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glGetQueryBufferObjectuiv(static_cast<GLuint>(p->id()), static_cast<GLuint>(p->buffer()), static_cast<GLenum>(p->pname()), static_cast<GLintptr>(p->offset()));
            return 0u;
        }

        case 507: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetQueryIndexediv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetQueryIndexediv(static_cast<GLenum>(p->target()), static_cast<GLuint>(p->index()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 508: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetQueryObjecti64v>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int64_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetQueryObjecti64v(static_cast<GLuint>(p->id()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint64*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 8));
            return 0u;
        }

        case 509: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetQueryObjectiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetQueryObjectiv(static_cast<GLuint>(p->id()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 510: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetQueryObjectui64v>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint64_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetQueryObjectui64v(static_cast<GLuint>(p->id()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLuint64*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 8));
            return 0u;
        }

        case 511: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetQueryObjectuiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetQueryObjectuiv(static_cast<GLuint>(p->id()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLuint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 512: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetQueryiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetQueryiv(static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 513: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetRenderbufferParameteriv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetRenderbufferParameteriv(static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 515: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetSamplerParameterIiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetSamplerParameterIiv(static_cast<GLuint>(p->sampler()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 516: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetSamplerParameterIuiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetSamplerParameterIuiv(static_cast<GLuint>(p->sampler()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLuint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 517: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetSamplerParameterfv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<float>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetSamplerParameterfv(static_cast<GLuint>(p->sampler()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLfloat*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 518: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetSamplerParameteriv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetSamplerParameteriv(static_cast<GLuint>(p->sampler()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 520: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetShaderPrecisionFormat>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_range(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            std::vector<int32_t>_out_precision(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetShaderPrecisionFormat(static_cast<GLenum>(p->shadertype()), static_cast<GLenum>(p->precisiontype()), reinterpret_cast<GLint*>(_out_range.data()), reinterpret_cast<GLint*>(_out_precision.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_range.data()),
                                  static_cast<uint32_t>(_out_range.size() * 4));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_precision.data()),
                                  static_cast<uint32_t>(_out_precision.size() * 4));
            return 0u;
        }

        case 522: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetShaderiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetShaderiv(static_cast<GLuint>(p->shader()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 523: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetShadingRateImagePaletteNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_rate(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetShadingRateImagePaletteNV(static_cast<GLuint>(p->viewport()), static_cast<GLuint>(p->entry()), reinterpret_cast<GLenum*>(_out_rate.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_rate.data()),
                                  static_cast<uint32_t>(_out_rate.size() * 4));
            return 0u;
        }

        case 524: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetShadingRateSampleLocationivNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_location(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetShadingRateSampleLocationivNV(static_cast<GLenum>(p->rate()), static_cast<GLuint>(p->samples()), static_cast<GLuint>(p->index()), reinterpret_cast<GLint*>(_out_location.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_location.data()),
                                  static_cast<uint32_t>(_out_location.size() * 4));
            return 0u;
        }

        case 525: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetStageIndexNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glGetStageIndexNV(static_cast<GLenum>(p->shadertype()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 526: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetString>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glGetString(static_cast<GLenum>(p->name()));
              WireDispatchStoreU64(reinterpret_cast<std::uintptr_t>(_ret)); }
            return 0u;
        }

        case 527: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetStringi>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glGetStringi(static_cast<GLenum>(p->name()), static_cast<GLuint>(p->index()));
              WireDispatchStoreU64(reinterpret_cast<std::uintptr_t>(_ret)); }
            return 0u;
        }

        case 528: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetSubroutineIndex>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glGetSubroutineIndex(static_cast<GLuint>(p->program()), static_cast<GLenum>(p->shadertype()), (p->name() == nullptr ? nullptr : p->name()->c_str()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 529: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetSubroutineUniformLocation>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glGetSubroutineUniformLocation(static_cast<GLuint>(p->program()), static_cast<GLenum>(p->shadertype()), (p->name() == nullptr ? nullptr : p->name()->c_str()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 530: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetSynciv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_length(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            std::vector<int32_t>_out_values(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetSynciv(reinterpret_cast<GLsync>(static_cast<std::uintptr_t>(p->sync())), static_cast<GLenum>(p->pname()), static_cast<GLsizei>(p->bufSize()), reinterpret_cast<GLsizei*>(_out_length.data()), reinterpret_cast<GLint*>(_out_values.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_length.data()),
                                  static_cast<uint32_t>(_out_length.size() * 4));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_values.data()),
                                  static_cast<uint32_t>(_out_values.size() * 4));
            return 0u;
        }

        case 531: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetTexImage>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glGetTexImage(static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLenum>(p->format()), static_cast<GLenum>(p->type()), const_cast<void*>(ResolveShm(p->pixels(), receivedShm, receivedShmCount)));
            return 0u;
        }

        case 532: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetTexLevelParameterfv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<float>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetTexLevelParameterfv(static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLfloat*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 533: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetTexLevelParameteriv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetTexLevelParameteriv(static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 534: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetTexParameterIiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetTexParameterIiv(static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 535: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetTexParameterIuiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetTexParameterIuiv(static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLuint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 536: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetTexParameterfv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<float>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetTexParameterfv(static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLfloat*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 537: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetTexParameteriv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetTexParameteriv(static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 538: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetTextureHandleARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glGetTextureHandleARB(static_cast<GLuint>(p->texture()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 539: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetTextureHandleNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glGetTextureHandleNV(static_cast<GLuint>(p->texture()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 540: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetTextureImage>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glGetTextureImage(static_cast<GLuint>(p->texture()), static_cast<GLint>(p->level()), static_cast<GLenum>(p->format()), static_cast<GLenum>(p->type()), static_cast<GLsizei>(p->bufSize()), const_cast<void*>(ResolveShm(p->pixels(), receivedShm, receivedShmCount)));
            return 0u;
        }

        case 541: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetTextureImageEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glGetTextureImageEXT(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLenum>(p->format()), static_cast<GLenum>(p->type()), const_cast<void*>(ResolveShm(p->pixels(), receivedShm, receivedShmCount)));
            return 0u;
        }

        case 542: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetTextureLevelParameterfv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<float>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetTextureLevelParameterfv(static_cast<GLuint>(p->texture()), static_cast<GLint>(p->level()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLfloat*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 543: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetTextureLevelParameterfvEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<float>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetTextureLevelParameterfvEXT(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLfloat*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 544: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetTextureLevelParameteriv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetTextureLevelParameteriv(static_cast<GLuint>(p->texture()), static_cast<GLint>(p->level()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 545: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetTextureLevelParameterivEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetTextureLevelParameterivEXT(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 546: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetTextureParameterIiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetTextureParameterIiv(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 547: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetTextureParameterIivEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetTextureParameterIivEXT(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 548: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetTextureParameterIuiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetTextureParameterIuiv(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLuint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 549: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetTextureParameterIuivEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetTextureParameterIuivEXT(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLuint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 550: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetTextureParameterfv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<float>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetTextureParameterfv(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLfloat*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 551: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetTextureParameterfvEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<float>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetTextureParameterfvEXT(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLfloat*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 552: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetTextureParameteriv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetTextureParameteriv(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 553: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetTextureParameterivEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetTextureParameterivEXT(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 554: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetTextureSamplerHandleARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glGetTextureSamplerHandleARB(static_cast<GLuint>(p->texture()), static_cast<GLuint>(p->sampler()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 555: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetTextureSamplerHandleNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glGetTextureSamplerHandleNV(static_cast<GLuint>(p->texture()), static_cast<GLuint>(p->sampler()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 556: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetTextureSubImage>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glGetTextureSubImage(static_cast<GLuint>(p->texture()), static_cast<GLint>(p->level()), static_cast<GLint>(p->xoffset()), static_cast<GLint>(p->yoffset()), static_cast<GLint>(p->zoffset()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLsizei>(p->depth()), static_cast<GLenum>(p->format()), static_cast<GLenum>(p->type()), static_cast<GLsizei>(p->bufSize()), const_cast<void*>(ResolveShm(p->pixels(), receivedShm, receivedShmCount)));
            return 0u;
        }

        case 558: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetTransformFeedbacki64_v>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int64_t>_out_param(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetTransformFeedbacki64_v(static_cast<GLuint>(p->xfb()), static_cast<GLenum>(p->pname()), static_cast<GLuint>(p->index()), reinterpret_cast<GLint64*>(_out_param.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_param.data()),
                                  static_cast<uint32_t>(_out_param.size() * 8));
            return 0u;
        }

        case 559: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetTransformFeedbacki_v>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_param(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetTransformFeedbacki_v(static_cast<GLuint>(p->xfb()), static_cast<GLenum>(p->pname()), static_cast<GLuint>(p->index()), reinterpret_cast<GLint*>(_out_param.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_param.data()),
                                  static_cast<uint32_t>(_out_param.size() * 4));
            return 0u;
        }

        case 560: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetTransformFeedbackiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_param(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetTransformFeedbackiv(static_cast<GLuint>(p->xfb()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_param.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_param.data()),
                                  static_cast<uint32_t>(_out_param.size() * 4));
            return 0u;
        }

        case 561: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetUniformBlockIndex>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glGetUniformBlockIndex(static_cast<GLuint>(p->program()), (p->uniformBlockName() == nullptr ? nullptr : p->uniformBlockName()->c_str()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 563: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetUniformLocation>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glGetUniformLocation(static_cast<GLuint>(p->program()), (p->name() == nullptr ? nullptr : p->name()->c_str()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 564: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetUniformSubroutineuiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetUniformSubroutineuiv(static_cast<GLenum>(p->shadertype()), static_cast<GLint>(p->location()), reinterpret_cast<GLuint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 565: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetUniformdv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<double>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetUniformdv(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), reinterpret_cast<GLdouble*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 8));
            return 0u;
        }

        case 566: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetUniformfv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<float>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetUniformfv(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), reinterpret_cast<GLfloat*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 567: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetUniformi64vARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int64_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetUniformi64vARB(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), reinterpret_cast<GLint64*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 8));
            return 0u;
        }

        case 568: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetUniformi64vNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int64_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetUniformi64vNV(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), reinterpret_cast<GLint64EXT*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 8));
            return 0u;
        }

        case 569: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetUniformiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetUniformiv(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 570: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetUniformui64vARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint64_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetUniformui64vARB(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), reinterpret_cast<GLuint64*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 8));
            return 0u;
        }

        case 571: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetUniformui64vNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint64_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetUniformui64vNV(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), reinterpret_cast<GLuint64EXT*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 8));
            return 0u;
        }

        case 572: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetUniformuiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetUniformuiv(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), reinterpret_cast<GLuint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 573: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetVertexArrayIndexed64iv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int64_t>_out_param(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetVertexArrayIndexed64iv(static_cast<GLuint>(p->vaobj()), static_cast<GLuint>(p->index()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint64*>(_out_param.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_param.data()),
                                  static_cast<uint32_t>(_out_param.size() * 8));
            return 0u;
        }

        case 574: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetVertexArrayIndexediv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_param(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetVertexArrayIndexediv(static_cast<GLuint>(p->vaobj()), static_cast<GLuint>(p->index()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_param.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_param.data()),
                                  static_cast<uint32_t>(_out_param.size() * 4));
            return 0u;
        }

        case 575: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetVertexArrayIntegeri_vEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_param(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetVertexArrayIntegeri_vEXT(static_cast<GLuint>(p->vaobj()), static_cast<GLuint>(p->index()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_param.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_param.data()),
                                  static_cast<uint32_t>(_out_param.size() * 4));
            return 0u;
        }

        case 576: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetVertexArrayIntegervEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_param(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetVertexArrayIntegervEXT(static_cast<GLuint>(p->vaobj()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_param.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_param.data()),
                                  static_cast<uint32_t>(_out_param.size() * 4));
            return 0u;
        }

        case 579: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetVertexArrayiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_param(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetVertexArrayiv(static_cast<GLuint>(p->vaobj()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_param.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_param.data()),
                                  static_cast<uint32_t>(_out_param.size() * 4));
            return 0u;
        }

        case 580: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetVertexAttribIiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetVertexAttribIiv(static_cast<GLuint>(p->index()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 581: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetVertexAttribIuiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetVertexAttribIuiv(static_cast<GLuint>(p->index()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLuint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 582: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetVertexAttribLdv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<double>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetVertexAttribLdv(static_cast<GLuint>(p->index()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLdouble*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 8));
            return 0u;
        }

        case 583: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetVertexAttribLi64vNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int64_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetVertexAttribLi64vNV(static_cast<GLuint>(p->index()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint64EXT*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 8));
            return 0u;
        }

        case 584: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetVertexAttribLui64vARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint64_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetVertexAttribLui64vARB(static_cast<GLuint>(p->index()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLuint64EXT*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 8));
            return 0u;
        }

        case 585: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetVertexAttribLui64vNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint64_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetVertexAttribLui64vNV(static_cast<GLuint>(p->index()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLuint64EXT*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 8));
            return 0u;
        }

        case 587: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetVertexAttribdv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<double>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetVertexAttribdv(static_cast<GLuint>(p->index()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLdouble*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 8));
            return 0u;
        }

        case 588: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetVertexAttribfv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<float>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetVertexAttribfv(static_cast<GLuint>(p->index()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLfloat*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 589: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetVertexAttribiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetVertexAttribiv(static_cast<GLuint>(p->index()), static_cast<GLenum>(p->pname()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 590: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetVkProcAddrNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glGetVkProcAddrNV((p->name() == nullptr ? nullptr : p->name()->c_str()));
              WireDispatchStoreU64(reinterpret_cast<std::uintptr_t>(_ret)); }
            return 0u;
        }

        case 591: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetnCompressedTexImage>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glGetnCompressedTexImage(static_cast<GLenum>(p->target()), static_cast<GLint>(p->lod()), static_cast<GLsizei>(p->bufSize()), const_cast<void*>(ResolveShm(p->pixels(), receivedShm, receivedShmCount)));
            return 0u;
        }

        case 592: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetnCompressedTexImageARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glGetnCompressedTexImageARB(static_cast<GLenum>(p->target()), static_cast<GLint>(p->lod()), static_cast<GLsizei>(p->bufSize()), const_cast<void*>(ResolveShm(p->img(), receivedShm, receivedShmCount)));
            return 0u;
        }

        case 593: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetnTexImage>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glGetnTexImage(static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLenum>(p->format()), static_cast<GLenum>(p->type()), static_cast<GLsizei>(p->bufSize()), const_cast<void*>(ResolveShm(p->pixels(), receivedShm, receivedShmCount)));
            return 0u;
        }

        case 594: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetnTexImageARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glGetnTexImageARB(static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLenum>(p->format()), static_cast<GLenum>(p->type()), static_cast<GLsizei>(p->bufSize()), const_cast<void*>(ResolveShm(p->img(), receivedShm, receivedShmCount)));
            return 0u;
        }

        case 595: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetnUniformdv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<double>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetnUniformdv(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->bufSize()), reinterpret_cast<GLdouble*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 8));
            return 0u;
        }

        case 596: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetnUniformdvARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<double>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetnUniformdvARB(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->bufSize()), reinterpret_cast<GLdouble*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 8));
            return 0u;
        }

        case 597: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetnUniformfv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<float>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetnUniformfv(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->bufSize()), reinterpret_cast<GLfloat*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 599: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetnUniformi64vARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int64_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetnUniformi64vARB(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->bufSize()), reinterpret_cast<GLint64*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 8));
            return 0u;
        }

        case 600: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetnUniformiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<int32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetnUniformiv(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->bufSize()), reinterpret_cast<GLint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 602: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetnUniformui64vARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint64_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetnUniformui64vARB(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->bufSize()), reinterpret_cast<GLuint64*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 8));
            return 0u;
        }

        case 603: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenGetnUniformuiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_params(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glGetnUniformuiv(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->bufSize()), reinterpret_cast<GLuint*>(_out_params.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_params.data()),
                                  static_cast<uint32_t>(_out_params.size() * 4));
            return 0u;
        }

        case 605: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenHint>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glHint(static_cast<GLenum>(p->target()), static_cast<GLenum>(p->mode()));
            return 0u;
        }

        case 606: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenIndexFormatNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glIndexFormatNV(static_cast<GLenum>(p->type()), static_cast<GLsizei>(p->stride()));
            return 0u;
        }

        case 607: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenInsertEventMarkerEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glInsertEventMarkerEXT(static_cast<GLsizei>(p->length()), (p->marker() == nullptr ? nullptr : p->marker()->c_str()));
            return 0u;
        }

        case 608: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenInterpolatePathsNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glInterpolatePathsNV(static_cast<GLuint>(p->resultPath()), static_cast<GLuint>(p->pathA()), static_cast<GLuint>(p->pathB()), static_cast<GLfloat>(p->weight()));
            return 0u;
        }

        case 609: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenInvalidateBufferData>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glInvalidateBufferData(static_cast<GLuint>(p->buffer()));
            return 0u;
        }

        case 610: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenInvalidateBufferSubData>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glInvalidateBufferSubData(static_cast<GLuint>(p->buffer()), static_cast<GLintptr>(p->offset()), static_cast<GLsizeiptr>(p->length()));
            return 0u;
        }

        case 611: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenInvalidateFramebuffer>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glInvalidateFramebuffer(static_cast<GLenum>(p->target()), static_cast<GLsizei>(p->numAttachments()), reinterpret_cast<const GLenum*>((p->attachments() == nullptr ? nullptr : p->attachments()->data())));
            return 0u;
        }

        case 612: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenInvalidateNamedFramebufferData>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glInvalidateNamedFramebufferData(static_cast<GLuint>(p->framebuffer()), static_cast<GLsizei>(p->numAttachments()), reinterpret_cast<const GLenum*>((p->attachments() == nullptr ? nullptr : p->attachments()->data())));
            return 0u;
        }

        case 613: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenInvalidateNamedFramebufferSubData>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glInvalidateNamedFramebufferSubData(static_cast<GLuint>(p->framebuffer()), static_cast<GLsizei>(p->numAttachments()), reinterpret_cast<const GLenum*>((p->attachments() == nullptr ? nullptr : p->attachments()->data())), static_cast<GLint>(p->x()), static_cast<GLint>(p->y()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()));
            return 0u;
        }

        case 614: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenInvalidateSubFramebuffer>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glInvalidateSubFramebuffer(static_cast<GLenum>(p->target()), static_cast<GLsizei>(p->numAttachments()), reinterpret_cast<const GLenum*>((p->attachments() == nullptr ? nullptr : p->attachments()->data())), static_cast<GLint>(p->x()), static_cast<GLint>(p->y()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()));
            return 0u;
        }

        case 615: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenInvalidateTexImage>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glInvalidateTexImage(static_cast<GLuint>(p->texture()), static_cast<GLint>(p->level()));
            return 0u;
        }

        case 616: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenInvalidateTexSubImage>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glInvalidateTexSubImage(static_cast<GLuint>(p->texture()), static_cast<GLint>(p->level()), static_cast<GLint>(p->xoffset()), static_cast<GLint>(p->yoffset()), static_cast<GLint>(p->zoffset()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLsizei>(p->depth()));
            return 0u;
        }

        case 617: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenIsBuffer>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glIsBuffer(static_cast<GLuint>(p->buffer()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 618: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenIsBufferResidentNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glIsBufferResidentNV(static_cast<GLenum>(p->target()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 619: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenIsCommandListNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glIsCommandListNV(static_cast<GLuint>(p->list()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 620: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenIsEnabled>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glIsEnabled(static_cast<GLenum>(p->cap()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 621: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenIsEnabledIndexedEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glIsEnabledIndexedEXT(static_cast<GLenum>(p->target()), static_cast<GLuint>(p->index()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 622: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenIsEnabledi>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glIsEnabledi(static_cast<GLenum>(p->target()), static_cast<GLuint>(p->index()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 623: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenIsFramebuffer>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glIsFramebuffer(static_cast<GLuint>(p->framebuffer()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 625: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenIsImageHandleResidentARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glIsImageHandleResidentARB(static_cast<GLuint64>(p->handle()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 626: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenIsImageHandleResidentNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glIsImageHandleResidentNV(static_cast<GLuint64>(p->handle()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 627: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenIsNamedBufferResidentNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glIsNamedBufferResidentNV(static_cast<GLuint>(p->buffer()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 628: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenIsNamedStringARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glIsNamedStringARB(static_cast<GLint>(p->namelen()), (p->name() == nullptr ? nullptr : p->name()->c_str()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 629: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenIsPathNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glIsPathNV(static_cast<GLuint>(p->path()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 630: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenIsPointInFillPathNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glIsPointInFillPathNV(static_cast<GLuint>(p->path()), static_cast<GLuint>(p->mask()), static_cast<GLfloat>(p->x()), static_cast<GLfloat>(p->y()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 631: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenIsPointInStrokePathNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glIsPointInStrokePathNV(static_cast<GLuint>(p->path()), static_cast<GLfloat>(p->x()), static_cast<GLfloat>(p->y()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 632: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenIsProgram>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glIsProgram(static_cast<GLuint>(p->program()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 633: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenIsProgramPipeline>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glIsProgramPipeline(static_cast<GLuint>(p->pipeline()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 634: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenIsQuery>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glIsQuery(static_cast<GLuint>(p->id()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 635: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenIsRenderbuffer>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glIsRenderbuffer(static_cast<GLuint>(p->renderbuffer()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 637: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenIsSampler>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glIsSampler(static_cast<GLuint>(p->sampler()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 638: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenIsShader>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glIsShader(static_cast<GLuint>(p->shader()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 639: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenIsStateNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glIsStateNV(static_cast<GLuint>(p->state()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 640: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenIsSync>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glIsSync(reinterpret_cast<GLsync>(static_cast<std::uintptr_t>(p->sync())));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 641: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenIsTexture>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glIsTexture(static_cast<GLuint>(p->texture()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 642: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenIsTextureHandleResidentARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glIsTextureHandleResidentARB(static_cast<GLuint64>(p->handle()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 643: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenIsTextureHandleResidentNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glIsTextureHandleResidentNV(static_cast<GLuint64>(p->handle()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 644: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenIsTransformFeedback>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glIsTransformFeedback(static_cast<GLuint>(p->id()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 646: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenIsVertexArray>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glIsVertexArray(static_cast<GLuint>(p->array()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 647: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenLabelObjectEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glLabelObjectEXT(static_cast<GLenum>(p->type()), static_cast<GLuint>(p->object()), static_cast<GLsizei>(p->length()), (p->label() == nullptr ? nullptr : p->label()->c_str()));
            return 0u;
        }

        case 648: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenLineWidth>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glLineWidth(static_cast<GLfloat>(p->width()));
            return 0u;
        }

        case 649: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenLinkProgram>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glLinkProgram(static_cast<GLuint>(p->program()));
            return 0u;
        }

        case 651: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenLogicOp>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glLogicOp(static_cast<GLenum>(p->opcode()));
            return 0u;
        }

        case 652: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMakeBufferNonResidentNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMakeBufferNonResidentNV(static_cast<GLenum>(p->target()));
            return 0u;
        }

        case 653: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMakeBufferResidentNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMakeBufferResidentNV(static_cast<GLenum>(p->target()), static_cast<GLenum>(p->access()));
            return 0u;
        }

        case 654: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMakeImageHandleNonResidentARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMakeImageHandleNonResidentARB(static_cast<GLuint64>(p->handle()));
            return 0u;
        }

        case 655: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMakeImageHandleNonResidentNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMakeImageHandleNonResidentNV(static_cast<GLuint64>(p->handle()));
            return 0u;
        }

        case 656: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMakeImageHandleResidentARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMakeImageHandleResidentARB(static_cast<GLuint64>(p->handle()), static_cast<GLenum>(p->access()));
            return 0u;
        }

        case 657: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMakeImageHandleResidentNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMakeImageHandleResidentNV(static_cast<GLuint64>(p->handle()), static_cast<GLenum>(p->access()));
            return 0u;
        }

        case 658: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMakeNamedBufferNonResidentNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMakeNamedBufferNonResidentNV(static_cast<GLuint>(p->buffer()));
            return 0u;
        }

        case 659: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMakeNamedBufferResidentNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMakeNamedBufferResidentNV(static_cast<GLuint>(p->buffer()), static_cast<GLenum>(p->access()));
            return 0u;
        }

        case 660: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMakeTextureHandleNonResidentARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMakeTextureHandleNonResidentARB(static_cast<GLuint64>(p->handle()));
            return 0u;
        }

        case 661: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMakeTextureHandleNonResidentNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMakeTextureHandleNonResidentNV(static_cast<GLuint64>(p->handle()));
            return 0u;
        }

        case 662: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMakeTextureHandleResidentARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMakeTextureHandleResidentARB(static_cast<GLuint64>(p->handle()));
            return 0u;
        }

        case 663: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMakeTextureHandleResidentNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMakeTextureHandleResidentNV(static_cast<GLuint64>(p->handle()));
            return 0u;
        }

        case 664: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMapBuffer>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glMapBuffer(static_cast<GLenum>(p->target()), static_cast<GLenum>(p->access()));
              WireDispatchStoreU64(reinterpret_cast<std::uintptr_t>(_ret)); }
            return 0u;
        }

        case 665: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMapBufferRange>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glMapBufferRange(static_cast<GLenum>(p->target()), static_cast<GLintptr>(p->offset()), static_cast<GLsizeiptr>(p->length()), static_cast<GLbitfield>(p->access()));
              WireDispatchStoreU64(reinterpret_cast<std::uintptr_t>(_ret)); }
            return 0u;
        }

        case 670: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMatrixFrustumEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMatrixFrustumEXT(static_cast<GLenum>(p->mode()), static_cast<GLdouble>(p->left()), static_cast<GLdouble>(p->right()), static_cast<GLdouble>(p->bottom()), static_cast<GLdouble>(p->top()), static_cast<GLdouble>(p->zNear()), static_cast<GLdouble>(p->zFar()));
            return 0u;
        }

        case 671: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMatrixLoad3x2fNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMatrixLoad3x2fNV(static_cast<GLenum>(p->matrixMode()), reinterpret_cast<const GLfloat*>((p->m() == nullptr ? nullptr : p->m()->data())));
            return 0u;
        }

        case 672: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMatrixLoad3x3fNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMatrixLoad3x3fNV(static_cast<GLenum>(p->matrixMode()), reinterpret_cast<const GLfloat*>((p->m() == nullptr ? nullptr : p->m()->data())));
            return 0u;
        }

        case 673: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMatrixLoadIdentityEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMatrixLoadIdentityEXT(static_cast<GLenum>(p->mode()));
            return 0u;
        }

        case 674: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMatrixLoadTranspose3x3fNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMatrixLoadTranspose3x3fNV(static_cast<GLenum>(p->matrixMode()), reinterpret_cast<const GLfloat*>((p->m() == nullptr ? nullptr : p->m()->data())));
            return 0u;
        }

        case 675: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMatrixLoadTransposedEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMatrixLoadTransposedEXT(static_cast<GLenum>(p->mode()), reinterpret_cast<const GLdouble*>((p->m() == nullptr ? nullptr : p->m()->data())));
            return 0u;
        }

        case 676: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMatrixLoadTransposefEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMatrixLoadTransposefEXT(static_cast<GLenum>(p->mode()), reinterpret_cast<const GLfloat*>((p->m() == nullptr ? nullptr : p->m()->data())));
            return 0u;
        }

        case 677: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMatrixLoaddEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMatrixLoaddEXT(static_cast<GLenum>(p->mode()), reinterpret_cast<const GLdouble*>((p->m() == nullptr ? nullptr : p->m()->data())));
            return 0u;
        }

        case 678: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMatrixLoadfEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMatrixLoadfEXT(static_cast<GLenum>(p->mode()), reinterpret_cast<const GLfloat*>((p->m() == nullptr ? nullptr : p->m()->data())));
            return 0u;
        }

        case 679: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMatrixMult3x2fNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMatrixMult3x2fNV(static_cast<GLenum>(p->matrixMode()), reinterpret_cast<const GLfloat*>((p->m() == nullptr ? nullptr : p->m()->data())));
            return 0u;
        }

        case 680: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMatrixMult3x3fNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMatrixMult3x3fNV(static_cast<GLenum>(p->matrixMode()), reinterpret_cast<const GLfloat*>((p->m() == nullptr ? nullptr : p->m()->data())));
            return 0u;
        }

        case 681: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMatrixMultTranspose3x3fNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMatrixMultTranspose3x3fNV(static_cast<GLenum>(p->matrixMode()), reinterpret_cast<const GLfloat*>((p->m() == nullptr ? nullptr : p->m()->data())));
            return 0u;
        }

        case 682: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMatrixMultTransposedEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMatrixMultTransposedEXT(static_cast<GLenum>(p->mode()), reinterpret_cast<const GLdouble*>((p->m() == nullptr ? nullptr : p->m()->data())));
            return 0u;
        }

        case 683: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMatrixMultTransposefEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMatrixMultTransposefEXT(static_cast<GLenum>(p->mode()), reinterpret_cast<const GLfloat*>((p->m() == nullptr ? nullptr : p->m()->data())));
            return 0u;
        }

        case 684: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMatrixMultdEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMatrixMultdEXT(static_cast<GLenum>(p->mode()), reinterpret_cast<const GLdouble*>((p->m() == nullptr ? nullptr : p->m()->data())));
            return 0u;
        }

        case 685: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMatrixMultfEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMatrixMultfEXT(static_cast<GLenum>(p->mode()), reinterpret_cast<const GLfloat*>((p->m() == nullptr ? nullptr : p->m()->data())));
            return 0u;
        }

        case 686: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMatrixOrthoEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMatrixOrthoEXT(static_cast<GLenum>(p->mode()), static_cast<GLdouble>(p->left()), static_cast<GLdouble>(p->right()), static_cast<GLdouble>(p->bottom()), static_cast<GLdouble>(p->top()), static_cast<GLdouble>(p->zNear()), static_cast<GLdouble>(p->zFar()));
            return 0u;
        }

        case 687: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMatrixPopEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMatrixPopEXT(static_cast<GLenum>(p->mode()));
            return 0u;
        }

        case 688: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMatrixPushEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMatrixPushEXT(static_cast<GLenum>(p->mode()));
            return 0u;
        }

        case 689: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMatrixRotatedEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMatrixRotatedEXT(static_cast<GLenum>(p->mode()), static_cast<GLdouble>(p->ane()), static_cast<GLdouble>(p->x()), static_cast<GLdouble>(p->y()), static_cast<GLdouble>(p->z()));
            return 0u;
        }

        case 690: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMatrixRotatefEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMatrixRotatefEXT(static_cast<GLenum>(p->mode()), static_cast<GLfloat>(p->ane()), static_cast<GLfloat>(p->x()), static_cast<GLfloat>(p->y()), static_cast<GLfloat>(p->z()));
            return 0u;
        }

        case 691: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMatrixScaledEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMatrixScaledEXT(static_cast<GLenum>(p->mode()), static_cast<GLdouble>(p->x()), static_cast<GLdouble>(p->y()), static_cast<GLdouble>(p->z()));
            return 0u;
        }

        case 692: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMatrixScalefEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMatrixScalefEXT(static_cast<GLenum>(p->mode()), static_cast<GLfloat>(p->x()), static_cast<GLfloat>(p->y()), static_cast<GLfloat>(p->z()));
            return 0u;
        }

        case 693: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMatrixTranslatedEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMatrixTranslatedEXT(static_cast<GLenum>(p->mode()), static_cast<GLdouble>(p->x()), static_cast<GLdouble>(p->y()), static_cast<GLdouble>(p->z()));
            return 0u;
        }

        case 694: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMatrixTranslatefEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMatrixTranslatefEXT(static_cast<GLenum>(p->mode()), static_cast<GLfloat>(p->x()), static_cast<GLfloat>(p->y()), static_cast<GLfloat>(p->z()));
            return 0u;
        }

        case 695: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMaxShaderCompilerThreadsARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMaxShaderCompilerThreadsARB(static_cast<GLuint>(p->count()));
            return 0u;
        }

        case 696: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMaxShaderCompilerThreadsKHR>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMaxShaderCompilerThreadsKHR(static_cast<GLuint>(p->count()));
            return 0u;
        }

        case 697: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMemoryBarrier>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMemoryBarrier(static_cast<GLbitfield>(p->barriers()));
            return 0u;
        }

        case 698: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMemoryBarrierByRegion>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMemoryBarrierByRegion(static_cast<GLbitfield>(p->barriers()));
            return 0u;
        }

        case 699: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMinSampleShading>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMinSampleShading(static_cast<GLfloat>(p->value()));
            return 0u;
        }

        case 701: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMultiDrawArrays>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMultiDrawArrays(static_cast<GLenum>(p->mode()), reinterpret_cast<const GLint*>((p->first() == nullptr ? nullptr : p->first()->data())), reinterpret_cast<const GLsizei*>((p->count() == nullptr ? nullptr : p->count()->data())), static_cast<GLsizei>(p->drawcount()));
            return 0u;
        }

        case 702: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMultiDrawArraysIndirect>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMultiDrawArraysIndirect(static_cast<GLenum>(p->mode()), ResolveShm(p->indirect(), receivedShm, receivedShmCount), static_cast<GLsizei>(p->drawcount()), static_cast<GLsizei>(p->stride()));
            return 0u;
        }

        case 703: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMultiDrawArraysIndirectBindlessCountNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMultiDrawArraysIndirectBindlessCountNV(static_cast<GLenum>(p->mode()), ResolveShm(p->indirect(), receivedShm, receivedShmCount), static_cast<GLsizei>(p->drawCount()), static_cast<GLsizei>(p->maxDrawCount()), static_cast<GLsizei>(p->stride()), static_cast<GLint>(p->vertexBufferCount()));
            return 0u;
        }

        case 704: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMultiDrawArraysIndirectBindlessNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMultiDrawArraysIndirectBindlessNV(static_cast<GLenum>(p->mode()), ResolveShm(p->indirect(), receivedShm, receivedShmCount), static_cast<GLsizei>(p->drawCount()), static_cast<GLsizei>(p->stride()), static_cast<GLint>(p->vertexBufferCount()));
            return 0u;
        }

        case 705: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMultiDrawArraysIndirectCount>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMultiDrawArraysIndirectCount(static_cast<GLenum>(p->mode()), ResolveShm(p->indirect(), receivedShm, receivedShmCount), static_cast<GLintptr>(p->drawcount()), static_cast<GLsizei>(p->maxdrawcount()), static_cast<GLsizei>(p->stride()));
            return 0u;
        }

        case 706: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMultiDrawArraysIndirectCountARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMultiDrawArraysIndirectCountARB(static_cast<GLenum>(p->mode()), ResolveShm(p->indirect(), receivedShm, receivedShmCount), static_cast<GLintptr>(p->drawcount()), static_cast<GLsizei>(p->maxdrawcount()), static_cast<GLsizei>(p->stride()));
            return 0u;
        }

        case 709: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMultiDrawElementsIndirect>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMultiDrawElementsIndirect(static_cast<GLenum>(p->mode()), static_cast<GLenum>(p->type()), ResolveShm(p->indirect(), receivedShm, receivedShmCount), static_cast<GLsizei>(p->drawcount()), static_cast<GLsizei>(p->stride()));
            return 0u;
        }

        case 710: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMultiDrawElementsIndirectBindlessCountNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMultiDrawElementsIndirectBindlessCountNV(static_cast<GLenum>(p->mode()), static_cast<GLenum>(p->type()), ResolveShm(p->indirect(), receivedShm, receivedShmCount), static_cast<GLsizei>(p->drawCount()), static_cast<GLsizei>(p->maxDrawCount()), static_cast<GLsizei>(p->stride()), static_cast<GLint>(p->vertexBufferCount()));
            return 0u;
        }

        case 711: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMultiDrawElementsIndirectBindlessNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMultiDrawElementsIndirectBindlessNV(static_cast<GLenum>(p->mode()), static_cast<GLenum>(p->type()), ResolveShm(p->indirect(), receivedShm, receivedShmCount), static_cast<GLsizei>(p->drawCount()), static_cast<GLsizei>(p->stride()), static_cast<GLint>(p->vertexBufferCount()));
            return 0u;
        }

        case 712: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMultiDrawElementsIndirectCount>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMultiDrawElementsIndirectCount(static_cast<GLenum>(p->mode()), static_cast<GLenum>(p->type()), ResolveShm(p->indirect(), receivedShm, receivedShmCount), static_cast<GLintptr>(p->drawcount()), static_cast<GLsizei>(p->maxdrawcount()), static_cast<GLsizei>(p->stride()));
            return 0u;
        }

        case 713: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMultiDrawElementsIndirectCountARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMultiDrawElementsIndirectCountARB(static_cast<GLenum>(p->mode()), static_cast<GLenum>(p->type()), ResolveShm(p->indirect(), receivedShm, receivedShmCount), static_cast<GLintptr>(p->drawcount()), static_cast<GLsizei>(p->maxdrawcount()), static_cast<GLsizei>(p->stride()));
            return 0u;
        }

        case 715: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMultiDrawMeshTasksIndirectCountNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMultiDrawMeshTasksIndirectCountNV(static_cast<GLintptr>(p->indirect()), static_cast<GLintptr>(p->drawcount()), static_cast<GLsizei>(p->maxdrawcount()), static_cast<GLsizei>(p->stride()));
            return 0u;
        }

        case 717: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMultiDrawMeshTasksIndirectNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMultiDrawMeshTasksIndirectNV(static_cast<GLintptr>(p->indirect()), static_cast<GLsizei>(p->drawcount()), static_cast<GLsizei>(p->stride()));
            return 0u;
        }

        case 718: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMultiTexBufferEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMultiTexBufferEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->target()), static_cast<GLenum>(p->internalformat()), static_cast<GLuint>(p->buffer()));
            return 0u;
        }

        case 719: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMultiTexCoordPointerEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMultiTexCoordPointerEXT(static_cast<GLenum>(p->texunit()), static_cast<GLint>(p->size()), static_cast<GLenum>(p->type()), static_cast<GLsizei>(p->stride()), ResolveShm(p->pointer(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 720: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMultiTexEnvfEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMultiTexEnvfEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), static_cast<GLfloat>(p->param()));
            return 0u;
        }

        case 721: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMultiTexEnvfvEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMultiTexEnvfvEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), reinterpret_cast<const GLfloat*>((p->params() == nullptr ? nullptr : p->params()->data())));
            return 0u;
        }

        case 722: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMultiTexEnviEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMultiTexEnviEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), static_cast<GLint>(p->param()));
            return 0u;
        }

        case 723: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMultiTexEnvivEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMultiTexEnvivEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), reinterpret_cast<const GLint*>((p->params() == nullptr ? nullptr : p->params()->data())));
            return 0u;
        }

        case 724: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMultiTexGendEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMultiTexGendEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->coord()), static_cast<GLenum>(p->pname()), static_cast<GLdouble>(p->param()));
            return 0u;
        }

        case 725: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMultiTexGendvEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMultiTexGendvEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->coord()), static_cast<GLenum>(p->pname()), reinterpret_cast<const GLdouble*>((p->params() == nullptr ? nullptr : p->params()->data())));
            return 0u;
        }

        case 726: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMultiTexGenfEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMultiTexGenfEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->coord()), static_cast<GLenum>(p->pname()), static_cast<GLfloat>(p->param()));
            return 0u;
        }

        case 727: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMultiTexGenfvEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMultiTexGenfvEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->coord()), static_cast<GLenum>(p->pname()), reinterpret_cast<const GLfloat*>((p->params() == nullptr ? nullptr : p->params()->data())));
            return 0u;
        }

        case 728: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMultiTexGeniEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMultiTexGeniEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->coord()), static_cast<GLenum>(p->pname()), static_cast<GLint>(p->param()));
            return 0u;
        }

        case 729: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMultiTexGenivEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMultiTexGenivEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->coord()), static_cast<GLenum>(p->pname()), reinterpret_cast<const GLint*>((p->params() == nullptr ? nullptr : p->params()->data())));
            return 0u;
        }

        case 730: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMultiTexImage1DEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMultiTexImage1DEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLint>(p->internalformat()), static_cast<GLsizei>(p->width()), static_cast<GLint>(p->border()), static_cast<GLenum>(p->format()), static_cast<GLenum>(p->type()), ResolveShm(p->pixels(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 731: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMultiTexImage2DEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMultiTexImage2DEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLint>(p->internalformat()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLint>(p->border()), static_cast<GLenum>(p->format()), static_cast<GLenum>(p->type()), ResolveShm(p->pixels(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 732: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMultiTexImage3DEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMultiTexImage3DEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLint>(p->internalformat()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLsizei>(p->depth()), static_cast<GLint>(p->border()), static_cast<GLenum>(p->format()), static_cast<GLenum>(p->type()), ResolveShm(p->pixels(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 733: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMultiTexParameterIivEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMultiTexParameterIivEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), reinterpret_cast<const GLint*>((p->params() == nullptr ? nullptr : p->params()->data())));
            return 0u;
        }

        case 734: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMultiTexParameterIuivEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMultiTexParameterIuivEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), reinterpret_cast<const GLuint*>((p->params() == nullptr ? nullptr : p->params()->data())));
            return 0u;
        }

        case 735: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMultiTexParameterfEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMultiTexParameterfEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), static_cast<GLfloat>(p->param()));
            return 0u;
        }

        case 736: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMultiTexParameterfvEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMultiTexParameterfvEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), reinterpret_cast<const GLfloat*>((p->params() == nullptr ? nullptr : p->params()->data())));
            return 0u;
        }

        case 737: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMultiTexParameteriEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMultiTexParameteriEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), static_cast<GLint>(p->param()));
            return 0u;
        }

        case 738: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMultiTexParameterivEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMultiTexParameterivEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), reinterpret_cast<const GLint*>((p->params() == nullptr ? nullptr : p->params()->data())));
            return 0u;
        }

        case 739: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMultiTexRenderbufferEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMultiTexRenderbufferEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->target()), static_cast<GLuint>(p->renderbuffer()));
            return 0u;
        }

        case 740: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMultiTexSubImage1DEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMultiTexSubImage1DEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLint>(p->xoffset()), static_cast<GLsizei>(p->width()), static_cast<GLenum>(p->format()), static_cast<GLenum>(p->type()), ResolveShm(p->pixels(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 741: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMultiTexSubImage2DEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMultiTexSubImage2DEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLint>(p->xoffset()), static_cast<GLint>(p->yoffset()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLenum>(p->format()), static_cast<GLenum>(p->type()), ResolveShm(p->pixels(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 742: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenMultiTexSubImage3DEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glMultiTexSubImage3DEXT(static_cast<GLenum>(p->texunit()), static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLint>(p->xoffset()), static_cast<GLint>(p->yoffset()), static_cast<GLint>(p->zoffset()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLsizei>(p->depth()), static_cast<GLenum>(p->format()), static_cast<GLenum>(p->type()), ResolveShm(p->pixels(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 743: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedBufferAttachMemoryNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedBufferAttachMemoryNV(static_cast<GLuint>(p->buffer()), static_cast<GLuint>(p->memory()), static_cast<GLuint64>(p->offset()));
            return 0u;
        }

        case 744: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedBufferData>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedBufferData(static_cast<GLuint>(p->buffer()), static_cast<GLsizeiptr>(p->size()), ResolveShm(p->data(), receivedShm, receivedShmCount), static_cast<GLenum>(p->usage()));
            return 0u;
        }

        case 745: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedBufferDataEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedBufferDataEXT(static_cast<GLuint>(p->buffer()), static_cast<GLsizeiptr>(p->size()), ResolveShm(p->data(), receivedShm, receivedShmCount), static_cast<GLenum>(p->usage()));
            return 0u;
        }

        case 746: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedBufferPageCommitmentARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedBufferPageCommitmentARB(static_cast<GLuint>(p->buffer()), static_cast<GLintptr>(p->offset()), static_cast<GLsizeiptr>(p->size()), static_cast<GLboolean>(p->commit()));
            return 0u;
        }

        case 747: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedBufferPageCommitmentEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedBufferPageCommitmentEXT(static_cast<GLuint>(p->buffer()), static_cast<GLintptr>(p->offset()), static_cast<GLsizeiptr>(p->size()), static_cast<GLboolean>(p->commit()));
            return 0u;
        }

        case 748: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedBufferPageCommitmentMemNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedBufferPageCommitmentMemNV(static_cast<GLuint>(p->buffer()), static_cast<GLintptr>(p->offset()), static_cast<GLsizeiptr>(p->size()), static_cast<GLuint>(p->memory()), static_cast<GLuint64>(p->memOffset()), static_cast<GLboolean>(p->commit()));
            return 0u;
        }

        case 749: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedBufferStorage>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedBufferStorage(static_cast<GLuint>(p->buffer()), static_cast<GLsizeiptr>(p->size()), ResolveShm(p->data(), receivedShm, receivedShmCount), static_cast<GLbitfield>(p->flags()));
            return 0u;
        }

        case 750: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedBufferStorageEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedBufferStorageEXT(static_cast<GLuint>(p->buffer()), static_cast<GLsizeiptr>(p->size()), ResolveShm(p->data(), receivedShm, receivedShmCount), static_cast<GLbitfield>(p->flags()));
            return 0u;
        }

        case 751: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedBufferSubData>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedBufferSubData(static_cast<GLuint>(p->buffer()), static_cast<GLintptr>(p->offset()), static_cast<GLsizeiptr>(p->size()), ResolveShm(p->data(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 752: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedBufferSubDataEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedBufferSubDataEXT(static_cast<GLuint>(p->buffer()), static_cast<GLintptr>(p->offset()), static_cast<GLsizeiptr>(p->size()), ResolveShm(p->data(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 753: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedCopyBufferSubDataEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedCopyBufferSubDataEXT(static_cast<GLuint>(p->readBuffer()), static_cast<GLuint>(p->writeBuffer()), static_cast<GLintptr>(p->readOffset()), static_cast<GLintptr>(p->writeOffset()), static_cast<GLsizeiptr>(p->size()));
            return 0u;
        }

        case 754: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedFramebufferDrawBuffer>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedFramebufferDrawBuffer(static_cast<GLuint>(p->framebuffer()), static_cast<GLenum>(p->buf()));
            return 0u;
        }

        case 755: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedFramebufferDrawBuffers>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedFramebufferDrawBuffers(static_cast<GLuint>(p->framebuffer()), static_cast<GLsizei>(p->n()), reinterpret_cast<const GLenum*>((p->bufs() == nullptr ? nullptr : p->bufs()->data())));
            return 0u;
        }

        case 756: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedFramebufferParameteri>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedFramebufferParameteri(static_cast<GLuint>(p->framebuffer()), static_cast<GLenum>(p->pname()), static_cast<GLint>(p->param()));
            return 0u;
        }

        case 757: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedFramebufferParameteriEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedFramebufferParameteriEXT(static_cast<GLuint>(p->framebuffer()), static_cast<GLenum>(p->pname()), static_cast<GLint>(p->param()));
            return 0u;
        }

        case 758: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedFramebufferReadBuffer>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedFramebufferReadBuffer(static_cast<GLuint>(p->framebuffer()), static_cast<GLenum>(p->src()));
            return 0u;
        }

        case 759: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedFramebufferRenderbuffer>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedFramebufferRenderbuffer(static_cast<GLuint>(p->framebuffer()), static_cast<GLenum>(p->attachment()), static_cast<GLenum>(p->renderbuffertarget()), static_cast<GLuint>(p->renderbuffer()));
            return 0u;
        }

        case 760: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedFramebufferRenderbufferEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedFramebufferRenderbufferEXT(static_cast<GLuint>(p->framebuffer()), static_cast<GLenum>(p->attachment()), static_cast<GLenum>(p->renderbuffertarget()), static_cast<GLuint>(p->renderbuffer()));
            return 0u;
        }

        case 761: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedFramebufferSampleLocationsfvARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedFramebufferSampleLocationsfvARB(static_cast<GLuint>(p->framebuffer()), static_cast<GLuint>(p->start()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLfloat*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 762: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedFramebufferSampleLocationsfvNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedFramebufferSampleLocationsfvNV(static_cast<GLuint>(p->framebuffer()), static_cast<GLuint>(p->start()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLfloat*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 763: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedFramebufferTexture>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedFramebufferTexture(static_cast<GLuint>(p->framebuffer()), static_cast<GLenum>(p->attachment()), static_cast<GLuint>(p->texture()), static_cast<GLint>(p->level()));
            return 0u;
        }

        case 764: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedFramebufferTexture1DEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedFramebufferTexture1DEXT(static_cast<GLuint>(p->framebuffer()), static_cast<GLenum>(p->attachment()), static_cast<GLenum>(p->textarget()), static_cast<GLuint>(p->texture()), static_cast<GLint>(p->level()));
            return 0u;
        }

        case 765: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedFramebufferTexture2DEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedFramebufferTexture2DEXT(static_cast<GLuint>(p->framebuffer()), static_cast<GLenum>(p->attachment()), static_cast<GLenum>(p->textarget()), static_cast<GLuint>(p->texture()), static_cast<GLint>(p->level()));
            return 0u;
        }

        case 766: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedFramebufferTexture3DEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedFramebufferTexture3DEXT(static_cast<GLuint>(p->framebuffer()), static_cast<GLenum>(p->attachment()), static_cast<GLenum>(p->textarget()), static_cast<GLuint>(p->texture()), static_cast<GLint>(p->level()), static_cast<GLint>(p->zoffset()));
            return 0u;
        }

        case 767: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedFramebufferTextureEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedFramebufferTextureEXT(static_cast<GLuint>(p->framebuffer()), static_cast<GLenum>(p->attachment()), static_cast<GLuint>(p->texture()), static_cast<GLint>(p->level()));
            return 0u;
        }

        case 768: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedFramebufferTextureFaceEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedFramebufferTextureFaceEXT(static_cast<GLuint>(p->framebuffer()), static_cast<GLenum>(p->attachment()), static_cast<GLuint>(p->texture()), static_cast<GLint>(p->level()), static_cast<GLenum>(p->face()));
            return 0u;
        }

        case 769: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedFramebufferTextureLayer>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedFramebufferTextureLayer(static_cast<GLuint>(p->framebuffer()), static_cast<GLenum>(p->attachment()), static_cast<GLuint>(p->texture()), static_cast<GLint>(p->level()), static_cast<GLint>(p->layer()));
            return 0u;
        }

        case 770: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedFramebufferTextureLayerEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedFramebufferTextureLayerEXT(static_cast<GLuint>(p->framebuffer()), static_cast<GLenum>(p->attachment()), static_cast<GLuint>(p->texture()), static_cast<GLint>(p->level()), static_cast<GLint>(p->layer()));
            return 0u;
        }

        case 772: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedProgramLocalParameter4dEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedProgramLocalParameter4dEXT(static_cast<GLuint>(p->program()), static_cast<GLenum>(p->target()), static_cast<GLuint>(p->index()), static_cast<GLdouble>(p->x()), static_cast<GLdouble>(p->y()), static_cast<GLdouble>(p->z()), static_cast<GLdouble>(p->w()));
            return 0u;
        }

        case 773: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedProgramLocalParameter4dvEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedProgramLocalParameter4dvEXT(static_cast<GLuint>(p->program()), static_cast<GLenum>(p->target()), static_cast<GLuint>(p->index()), reinterpret_cast<const GLdouble*>((p->params() == nullptr ? nullptr : p->params()->data())));
            return 0u;
        }

        case 774: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedProgramLocalParameter4fEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedProgramLocalParameter4fEXT(static_cast<GLuint>(p->program()), static_cast<GLenum>(p->target()), static_cast<GLuint>(p->index()), static_cast<GLfloat>(p->x()), static_cast<GLfloat>(p->y()), static_cast<GLfloat>(p->z()), static_cast<GLfloat>(p->w()));
            return 0u;
        }

        case 775: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedProgramLocalParameter4fvEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedProgramLocalParameter4fvEXT(static_cast<GLuint>(p->program()), static_cast<GLenum>(p->target()), static_cast<GLuint>(p->index()), reinterpret_cast<const GLfloat*>((p->params() == nullptr ? nullptr : p->params()->data())));
            return 0u;
        }

        case 776: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedProgramLocalParameterI4iEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedProgramLocalParameterI4iEXT(static_cast<GLuint>(p->program()), static_cast<GLenum>(p->target()), static_cast<GLuint>(p->index()), static_cast<GLint>(p->x()), static_cast<GLint>(p->y()), static_cast<GLint>(p->z()), static_cast<GLint>(p->w()));
            return 0u;
        }

        case 777: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedProgramLocalParameterI4ivEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedProgramLocalParameterI4ivEXT(static_cast<GLuint>(p->program()), static_cast<GLenum>(p->target()), static_cast<GLuint>(p->index()), reinterpret_cast<const GLint*>((p->params() == nullptr ? nullptr : p->params()->data())));
            return 0u;
        }

        case 778: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedProgramLocalParameterI4uiEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedProgramLocalParameterI4uiEXT(static_cast<GLuint>(p->program()), static_cast<GLenum>(p->target()), static_cast<GLuint>(p->index()), static_cast<GLuint>(p->x()), static_cast<GLuint>(p->y()), static_cast<GLuint>(p->z()), static_cast<GLuint>(p->w()));
            return 0u;
        }

        case 779: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedProgramLocalParameterI4uivEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedProgramLocalParameterI4uivEXT(static_cast<GLuint>(p->program()), static_cast<GLenum>(p->target()), static_cast<GLuint>(p->index()), reinterpret_cast<const GLuint*>((p->params() == nullptr ? nullptr : p->params()->data())));
            return 0u;
        }

        case 780: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedProgramLocalParameters4fvEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedProgramLocalParameters4fvEXT(static_cast<GLuint>(p->program()), static_cast<GLenum>(p->target()), static_cast<GLuint>(p->index()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLfloat*>((p->params() == nullptr ? nullptr : p->params()->data())));
            return 0u;
        }

        case 781: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedProgramLocalParametersI4ivEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedProgramLocalParametersI4ivEXT(static_cast<GLuint>(p->program()), static_cast<GLenum>(p->target()), static_cast<GLuint>(p->index()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLint*>((p->params() == nullptr ? nullptr : p->params()->data())));
            return 0u;
        }

        case 782: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedProgramLocalParametersI4uivEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedProgramLocalParametersI4uivEXT(static_cast<GLuint>(p->program()), static_cast<GLenum>(p->target()), static_cast<GLuint>(p->index()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLuint*>((p->params() == nullptr ? nullptr : p->params()->data())));
            return 0u;
        }

        case 783: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedProgramStringEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedProgramStringEXT(static_cast<GLuint>(p->program()), static_cast<GLenum>(p->target()), static_cast<GLenum>(p->format()), static_cast<GLsizei>(p->len()), ResolveShm(p->string(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 784: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedRenderbufferStorage>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedRenderbufferStorage(static_cast<GLuint>(p->renderbuffer()), static_cast<GLenum>(p->internalformat()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()));
            return 0u;
        }

        case 785: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedRenderbufferStorageEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedRenderbufferStorageEXT(static_cast<GLuint>(p->renderbuffer()), static_cast<GLenum>(p->internalformat()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()));
            return 0u;
        }

        case 786: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedRenderbufferStorageMultisample>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedRenderbufferStorageMultisample(static_cast<GLuint>(p->renderbuffer()), static_cast<GLsizei>(p->samples()), static_cast<GLenum>(p->internalformat()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()));
            return 0u;
        }

        case 787: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedRenderbufferStorageMultisampleAdvancedAMD>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedRenderbufferStorageMultisampleAdvancedAMD(static_cast<GLuint>(p->renderbuffer()), static_cast<GLsizei>(p->samples()), static_cast<GLsizei>(p->storageSamples()), static_cast<GLenum>(p->internalformat()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()));
            return 0u;
        }

        case 788: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedRenderbufferStorageMultisampleCoverageEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedRenderbufferStorageMultisampleCoverageEXT(static_cast<GLuint>(p->renderbuffer()), static_cast<GLsizei>(p->coverageSamples()), static_cast<GLsizei>(p->colorSamples()), static_cast<GLenum>(p->internalformat()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()));
            return 0u;
        }

        case 789: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedRenderbufferStorageMultisampleEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedRenderbufferStorageMultisampleEXT(static_cast<GLuint>(p->renderbuffer()), static_cast<GLsizei>(p->samples()), static_cast<GLenum>(p->internalformat()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()));
            return 0u;
        }

        case 790: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNamedStringARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNamedStringARB(static_cast<GLenum>(p->type()), static_cast<GLint>(p->namelen()), (p->name() == nullptr ? nullptr : p->name()->c_str()), static_cast<GLint>(p->strinen()), (p->string() == nullptr ? nullptr : p->string()->c_str()));
            return 0u;
        }

        case 791: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenNormalFormatNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glNormalFormatNV(static_cast<GLenum>(p->type()), static_cast<GLsizei>(p->stride()));
            return 0u;
        }

        case 792: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenObjectLabel>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glObjectLabel(static_cast<GLenum>(p->identifier()), static_cast<GLuint>(p->name()), static_cast<GLsizei>(p->length()), (p->label() == nullptr ? nullptr : p->label()->c_str()));
            return 0u;
        }

        case 793: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenObjectPtrLabel>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glObjectPtrLabel(ResolveShm(p->ptr(), receivedShm, receivedShmCount), static_cast<GLsizei>(p->length()), (p->label() == nullptr ? nullptr : p->label()->c_str()));
            return 0u;
        }

        case 794: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenPatchParameterfv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glPatchParameterfv(static_cast<GLenum>(p->pname()), reinterpret_cast<const GLfloat*>((p->values() == nullptr ? nullptr : p->values()->data())));
            return 0u;
        }

        case 795: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenPatchParameteri>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glPatchParameteri(static_cast<GLenum>(p->pname()), static_cast<GLint>(p->value()));
            return 0u;
        }

        case 796: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenPathCommandsNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glPathCommandsNV(static_cast<GLuint>(p->path()), static_cast<GLsizei>(p->numCommands()), reinterpret_cast<const GLubyte*>((p->commands() == nullptr ? nullptr : p->commands()->data())), static_cast<GLsizei>(p->numCoords()), static_cast<GLenum>(p->coordType()), ResolveShm(p->coords(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 797: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenPathCoordsNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glPathCoordsNV(static_cast<GLuint>(p->path()), static_cast<GLsizei>(p->numCoords()), static_cast<GLenum>(p->coordType()), ResolveShm(p->coords(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 798: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenPathCoverDepthFuncNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glPathCoverDepthFuncNV(static_cast<GLenum>(p->func()));
            return 0u;
        }

        case 799: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenPathDashArrayNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glPathDashArrayNV(static_cast<GLuint>(p->path()), static_cast<GLsizei>(p->dashCount()), reinterpret_cast<const GLfloat*>((p->dashArray() == nullptr ? nullptr : p->dashArray()->data())));
            return 0u;
        }

        case 800: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenPathGlyphIndexArrayNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glPathGlyphIndexArrayNV(static_cast<GLuint>(p->firstPathName()), static_cast<GLenum>(p->fontTarget()), ResolveShm(p->fontName(), receivedShm, receivedShmCount), static_cast<GLbitfield>(p->fontStyle()), static_cast<GLuint>(p->firstGlyphIndex()), static_cast<GLsizei>(p->numGlyphs()), static_cast<GLuint>(p->pathParameterTemplate()), static_cast<GLfloat>(p->emScale()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 801: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenPathGlyphIndexRangeNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_baseAndCount(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            { const auto _ret = ::glPathGlyphIndexRangeNV(static_cast<GLenum>(p->fontTarget()), ResolveShm(p->fontName(), receivedShm, receivedShmCount), static_cast<GLbitfield>(p->fontStyle()), static_cast<GLuint>(p->pathParameterTemplate()), static_cast<GLfloat>(p->emScale()), reinterpret_cast<GLuint*>(_out_baseAndCount.data()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_baseAndCount.data()),
                                  static_cast<uint32_t>(_out_baseAndCount.size() * 4));
            return 0u;
        }

        case 802: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenPathGlyphRangeNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glPathGlyphRangeNV(static_cast<GLuint>(p->firstPathName()), static_cast<GLenum>(p->fontTarget()), ResolveShm(p->fontName(), receivedShm, receivedShmCount), static_cast<GLbitfield>(p->fontStyle()), static_cast<GLuint>(p->firstGlyph()), static_cast<GLsizei>(p->numGlyphs()), static_cast<GLenum>(p->handleMissingGlyphs()), static_cast<GLuint>(p->pathParameterTemplate()), static_cast<GLfloat>(p->emScale()));
            return 0u;
        }

        case 803: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenPathGlyphsNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glPathGlyphsNV(static_cast<GLuint>(p->firstPathName()), static_cast<GLenum>(p->fontTarget()), ResolveShm(p->fontName(), receivedShm, receivedShmCount), static_cast<GLbitfield>(p->fontStyle()), static_cast<GLsizei>(p->numGlyphs()), static_cast<GLenum>(p->type()), ResolveShm(p->charcodes(), receivedShm, receivedShmCount), static_cast<GLenum>(p->handleMissingGlyphs()), static_cast<GLuint>(p->pathParameterTemplate()), static_cast<GLfloat>(p->emScale()));
            return 0u;
        }

        case 804: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenPathMemoryGlyphIndexArrayNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glPathMemoryGlyphIndexArrayNV(static_cast<GLuint>(p->firstPathName()), static_cast<GLenum>(p->fontTarget()), static_cast<GLsizeiptr>(p->fontSize()), ResolveShm(p->fontData(), receivedShm, receivedShmCount), static_cast<GLsizei>(p->faceIndex()), static_cast<GLuint>(p->firstGlyphIndex()), static_cast<GLsizei>(p->numGlyphs()), static_cast<GLuint>(p->pathParameterTemplate()), static_cast<GLfloat>(p->emScale()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 805: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenPathParameterfNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glPathParameterfNV(static_cast<GLuint>(p->path()), static_cast<GLenum>(p->pname()), static_cast<GLfloat>(p->value()));
            return 0u;
        }

        case 806: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenPathParameterfvNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glPathParameterfvNV(static_cast<GLuint>(p->path()), static_cast<GLenum>(p->pname()), reinterpret_cast<const GLfloat*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 807: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenPathParameteriNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glPathParameteriNV(static_cast<GLuint>(p->path()), static_cast<GLenum>(p->pname()), static_cast<GLint>(p->value()));
            return 0u;
        }

        case 808: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenPathParameterivNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glPathParameterivNV(static_cast<GLuint>(p->path()), static_cast<GLenum>(p->pname()), reinterpret_cast<const GLint*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 809: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenPathStencilDepthOffsetNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glPathStencilDepthOffsetNV(static_cast<GLfloat>(p->factor()), static_cast<GLfloat>(p->units()));
            return 0u;
        }

        case 810: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenPathStencilFuncNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glPathStencilFuncNV(static_cast<GLenum>(p->func()), static_cast<GLint>(p->ref()), static_cast<GLuint>(p->mask()));
            return 0u;
        }

        case 811: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenPathStringNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glPathStringNV(static_cast<GLuint>(p->path()), static_cast<GLenum>(p->format()), static_cast<GLsizei>(p->length()), ResolveShm(p->pathString(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 812: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenPathSubCommandsNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glPathSubCommandsNV(static_cast<GLuint>(p->path()), static_cast<GLsizei>(p->commandStart()), static_cast<GLsizei>(p->commandsToDelete()), static_cast<GLsizei>(p->numCommands()), reinterpret_cast<const GLubyte*>((p->commands() == nullptr ? nullptr : p->commands()->data())), static_cast<GLsizei>(p->numCoords()), static_cast<GLenum>(p->coordType()), ResolveShm(p->coords(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 813: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenPathSubCoordsNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glPathSubCoordsNV(static_cast<GLuint>(p->path()), static_cast<GLsizei>(p->coordStart()), static_cast<GLsizei>(p->numCoords()), static_cast<GLenum>(p->coordType()), ResolveShm(p->coords(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 815: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenPixelStoref>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glPixelStoref(static_cast<GLenum>(p->pname()), static_cast<GLfloat>(p->param()));
            return 0u;
        }

        case 816: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenPixelStorei>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glPixelStorei(static_cast<GLenum>(p->pname()), static_cast<GLint>(p->param()));
            return 0u;
        }

        case 817: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenPointAlongPathNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<float>_out_x(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            std::vector<float>_out_y(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            std::vector<float>_out_tangentX(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            std::vector<float>_out_tangentY(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            { const auto _ret = ::glPointAlongPathNV(static_cast<GLuint>(p->path()), static_cast<GLsizei>(p->startSegment()), static_cast<GLsizei>(p->numSegments()), static_cast<GLfloat>(p->distance()), reinterpret_cast<GLfloat*>(_out_x.data()), reinterpret_cast<GLfloat*>(_out_y.data()), reinterpret_cast<GLfloat*>(_out_tangentX.data()), reinterpret_cast<GLfloat*>(_out_tangentY.data()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_x.data()),
                                  static_cast<uint32_t>(_out_x.size() * 4));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_y.data()),
                                  static_cast<uint32_t>(_out_y.size() * 4));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_tangentX.data()),
                                  static_cast<uint32_t>(_out_tangentX.size() * 4));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_tangentY.data()),
                                  static_cast<uint32_t>(_out_tangentY.size() * 4));
            return 0u;
        }

        case 818: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenPointParameterf>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glPointParameterf(static_cast<GLenum>(p->pname()), static_cast<GLfloat>(p->param()));
            return 0u;
        }

        case 819: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenPointParameterfv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glPointParameterfv(static_cast<GLenum>(p->pname()), reinterpret_cast<const GLfloat*>((p->params() == nullptr ? nullptr : p->params()->data())));
            return 0u;
        }

        case 820: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenPointParameteri>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glPointParameteri(static_cast<GLenum>(p->pname()), static_cast<GLint>(p->param()));
            return 0u;
        }

        case 821: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenPointParameteriv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glPointParameteriv(static_cast<GLenum>(p->pname()), reinterpret_cast<const GLint*>((p->params() == nullptr ? nullptr : p->params()->data())));
            return 0u;
        }

        case 822: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenPointSize>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glPointSize(static_cast<GLfloat>(p->size()));
            return 0u;
        }

        case 823: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenPolygonMode>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glPolygonMode(static_cast<GLenum>(p->face()), static_cast<GLenum>(p->mode()));
            return 0u;
        }

        case 824: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenPolygonOffset>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glPolygonOffset(static_cast<GLfloat>(p->factor()), static_cast<GLfloat>(p->units()));
            return 0u;
        }

        case 825: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenPolygonOffsetClamp>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glPolygonOffsetClamp(static_cast<GLfloat>(p->factor()), static_cast<GLfloat>(p->units()), static_cast<GLfloat>(p->clamp()));
            return 0u;
        }

        case 826: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenPolygonOffsetClampEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glPolygonOffsetClampEXT(static_cast<GLfloat>(p->factor()), static_cast<GLfloat>(p->units()), static_cast<GLfloat>(p->clamp()));
            return 0u;
        }

        case 829: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenPrimitiveBoundingBoxARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glPrimitiveBoundingBoxARB(static_cast<GLfloat>(p->minX()), static_cast<GLfloat>(p->minY()), static_cast<GLfloat>(p->minZ()), static_cast<GLfloat>(p->minW()), static_cast<GLfloat>(p->maxX()), static_cast<GLfloat>(p->maxY()), static_cast<GLfloat>(p->maxZ()), static_cast<GLfloat>(p->maxW()));
            return 0u;
        }

        case 830: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenPrimitiveRestartIndex>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glPrimitiveRestartIndex(static_cast<GLuint>(p->index()));
            return 0u;
        }

        case 831: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramBinary>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramBinary(static_cast<GLuint>(p->program()), static_cast<GLenum>(p->binaryFormat()), ResolveShm(p->binary(), receivedShm, receivedShmCount), static_cast<GLsizei>(p->length()));
            return 0u;
        }

        case 832: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramParameteri>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramParameteri(static_cast<GLuint>(p->program()), static_cast<GLenum>(p->pname()), static_cast<GLint>(p->value()));
            return 0u;
        }

        case 834: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramPathFragmentInputGenNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramPathFragmentInputGenNV(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLenum>(p->genMode()), static_cast<GLint>(p->components()), reinterpret_cast<const GLfloat*>((p->coeffs() == nullptr ? nullptr : p->coeffs()->data())));
            return 0u;
        }

        case 835: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform1d>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform1d(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLdouble>(p->v0()));
            return 0u;
        }

        case 836: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform1dEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform1dEXT(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLdouble>(p->x()));
            return 0u;
        }

        case 837: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform1dv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform1dv(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLdouble*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 838: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform1dvEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform1dvEXT(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLdouble*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 839: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform1f>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform1f(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLfloat>(p->v0()));
            return 0u;
        }

        case 841: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform1fv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform1fv(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLfloat*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 843: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform1i>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform1i(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLint>(p->v0()));
            return 0u;
        }

        case 844: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform1i64ARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform1i64ARB(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLint64>(p->x()));
            return 0u;
        }

        case 845: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform1i64NV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform1i64NV(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), p->x());
            return 0u;
        }

        case 846: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform1i64vARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform1i64vARB(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLint64*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 847: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform1i64vNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform1i64vNV(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLint64EXT*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 849: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform1iv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform1iv(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLint*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 851: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform1ui>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform1ui(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLuint>(p->v0()));
            return 0u;
        }

        case 852: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform1ui64ARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform1ui64ARB(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLuint64>(p->x()));
            return 0u;
        }

        case 853: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform1ui64NV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform1ui64NV(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), p->x());
            return 0u;
        }

        case 854: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform1ui64vARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform1ui64vARB(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLuint64*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 855: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform1ui64vNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform1ui64vNV(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLuint64EXT*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 857: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform1uiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform1uiv(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLuint*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 859: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform2d>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform2d(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLdouble>(p->v0()), static_cast<GLdouble>(p->v1()));
            return 0u;
        }

        case 860: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform2dEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform2dEXT(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLdouble>(p->x()), static_cast<GLdouble>(p->y()));
            return 0u;
        }

        case 861: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform2dv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform2dv(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLdouble*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 862: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform2dvEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform2dvEXT(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLdouble*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 863: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform2f>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform2f(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLfloat>(p->v0()), static_cast<GLfloat>(p->v1()));
            return 0u;
        }

        case 865: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform2fv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform2fv(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLfloat*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 867: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform2i>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform2i(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLint>(p->v0()), static_cast<GLint>(p->v1()));
            return 0u;
        }

        case 868: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform2i64ARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform2i64ARB(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLint64>(p->x()), static_cast<GLint64>(p->y()));
            return 0u;
        }

        case 869: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform2i64NV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform2i64NV(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), p->x(), p->y());
            return 0u;
        }

        case 870: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform2i64vARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform2i64vARB(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLint64*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 871: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform2i64vNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform2i64vNV(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLint64EXT*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 873: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform2iv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform2iv(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLint*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 875: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform2ui>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform2ui(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLuint>(p->v0()), static_cast<GLuint>(p->v1()));
            return 0u;
        }

        case 876: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform2ui64ARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform2ui64ARB(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLuint64>(p->x()), static_cast<GLuint64>(p->y()));
            return 0u;
        }

        case 877: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform2ui64NV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform2ui64NV(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), p->x(), p->y());
            return 0u;
        }

        case 878: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform2ui64vARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform2ui64vARB(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLuint64*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 879: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform2ui64vNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform2ui64vNV(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLuint64EXT*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 881: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform2uiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform2uiv(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLuint*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 883: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform3d>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform3d(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLdouble>(p->v0()), static_cast<GLdouble>(p->v1()), static_cast<GLdouble>(p->v2()));
            return 0u;
        }

        case 884: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform3dEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform3dEXT(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLdouble>(p->x()), static_cast<GLdouble>(p->y()), static_cast<GLdouble>(p->z()));
            return 0u;
        }

        case 885: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform3dv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform3dv(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLdouble*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 886: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform3dvEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform3dvEXT(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLdouble*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 887: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform3f>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform3f(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLfloat>(p->v0()), static_cast<GLfloat>(p->v1()), static_cast<GLfloat>(p->v2()));
            return 0u;
        }

        case 889: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform3fv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform3fv(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLfloat*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 891: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform3i>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform3i(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLint>(p->v0()), static_cast<GLint>(p->v1()), static_cast<GLint>(p->v2()));
            return 0u;
        }

        case 892: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform3i64ARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform3i64ARB(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLint64>(p->x()), static_cast<GLint64>(p->y()), static_cast<GLint64>(p->z()));
            return 0u;
        }

        case 893: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform3i64NV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform3i64NV(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), p->x(), p->y(), p->z());
            return 0u;
        }

        case 894: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform3i64vARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform3i64vARB(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLint64*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 895: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform3i64vNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform3i64vNV(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLint64EXT*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 897: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform3iv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform3iv(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLint*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 899: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform3ui>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform3ui(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLuint>(p->v0()), static_cast<GLuint>(p->v1()), static_cast<GLuint>(p->v2()));
            return 0u;
        }

        case 900: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform3ui64ARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform3ui64ARB(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLuint64>(p->x()), static_cast<GLuint64>(p->y()), static_cast<GLuint64>(p->z()));
            return 0u;
        }

        case 901: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform3ui64NV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform3ui64NV(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), p->x(), p->y(), p->z());
            return 0u;
        }

        case 902: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform3ui64vARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform3ui64vARB(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLuint64*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 903: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform3ui64vNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform3ui64vNV(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLuint64EXT*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 905: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform3uiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform3uiv(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLuint*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 907: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform4d>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform4d(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLdouble>(p->v0()), static_cast<GLdouble>(p->v1()), static_cast<GLdouble>(p->v2()), static_cast<GLdouble>(p->v3()));
            return 0u;
        }

        case 908: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform4dEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform4dEXT(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLdouble>(p->x()), static_cast<GLdouble>(p->y()), static_cast<GLdouble>(p->z()), static_cast<GLdouble>(p->w()));
            return 0u;
        }

        case 909: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform4dv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform4dv(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLdouble*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 910: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform4dvEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform4dvEXT(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLdouble*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 911: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform4f>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform4f(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLfloat>(p->v0()), static_cast<GLfloat>(p->v1()), static_cast<GLfloat>(p->v2()), static_cast<GLfloat>(p->v3()));
            return 0u;
        }

        case 913: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform4fv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform4fv(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLfloat*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 915: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform4i>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform4i(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLint>(p->v0()), static_cast<GLint>(p->v1()), static_cast<GLint>(p->v2()), static_cast<GLint>(p->v3()));
            return 0u;
        }

        case 916: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform4i64ARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform4i64ARB(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLint64>(p->x()), static_cast<GLint64>(p->y()), static_cast<GLint64>(p->z()), static_cast<GLint64>(p->w()));
            return 0u;
        }

        case 917: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform4i64NV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform4i64NV(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), p->x(), p->y(), p->z(), p->w());
            return 0u;
        }

        case 918: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform4i64vARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform4i64vARB(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLint64*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 919: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform4i64vNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform4i64vNV(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLint64EXT*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 921: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform4iv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform4iv(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLint*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 923: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform4ui>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform4ui(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLuint>(p->v0()), static_cast<GLuint>(p->v1()), static_cast<GLuint>(p->v2()), static_cast<GLuint>(p->v3()));
            return 0u;
        }

        case 924: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform4ui64ARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform4ui64ARB(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLuint64>(p->x()), static_cast<GLuint64>(p->y()), static_cast<GLuint64>(p->z()), static_cast<GLuint64>(p->w()));
            return 0u;
        }

        case 925: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform4ui64NV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform4ui64NV(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), p->x(), p->y(), p->z(), p->w());
            return 0u;
        }

        case 926: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform4ui64vARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform4ui64vARB(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLuint64*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 927: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform4ui64vNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform4ui64vNV(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLuint64EXT*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 929: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniform4uiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniform4uiv(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLuint*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 931: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniformHandleui64ARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniformHandleui64ARB(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLuint64>(p->value()));
            return 0u;
        }

        case 932: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniformHandleui64NV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniformHandleui64NV(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLuint64>(p->value()));
            return 0u;
        }

        case 933: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniformHandleui64vARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniformHandleui64vARB(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLuint64*>((p->values() == nullptr ? nullptr : p->values()->data())));
            return 0u;
        }

        case 934: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniformHandleui64vNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniformHandleui64vNV(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLuint64*>((p->values() == nullptr ? nullptr : p->values()->data())));
            return 0u;
        }

        case 935: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniformMatrix2dv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniformMatrix2dv(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLdouble*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 936: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniformMatrix2dvEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniformMatrix2dvEXT(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLdouble*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 937: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniformMatrix2fv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniformMatrix2fv(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLfloat*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 939: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniformMatrix2x3dv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniformMatrix2x3dv(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLdouble*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 940: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniformMatrix2x3dvEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniformMatrix2x3dvEXT(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLdouble*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 941: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniformMatrix2x3fv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniformMatrix2x3fv(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLfloat*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 943: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniformMatrix2x4dv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniformMatrix2x4dv(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLdouble*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 944: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniformMatrix2x4dvEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniformMatrix2x4dvEXT(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLdouble*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 945: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniformMatrix2x4fv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniformMatrix2x4fv(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLfloat*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 947: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniformMatrix3dv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniformMatrix3dv(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLdouble*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 948: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniformMatrix3dvEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniformMatrix3dvEXT(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLdouble*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 949: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniformMatrix3fv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniformMatrix3fv(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLfloat*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 951: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniformMatrix3x2dv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniformMatrix3x2dv(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLdouble*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 952: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniformMatrix3x2dvEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniformMatrix3x2dvEXT(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLdouble*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 953: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniformMatrix3x2fv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniformMatrix3x2fv(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLfloat*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 955: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniformMatrix3x4dv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniformMatrix3x4dv(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLdouble*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 956: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniformMatrix3x4dvEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniformMatrix3x4dvEXT(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLdouble*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 957: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniformMatrix3x4fv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniformMatrix3x4fv(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLfloat*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 959: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniformMatrix4dv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniformMatrix4dv(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLdouble*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 960: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniformMatrix4dvEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniformMatrix4dvEXT(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLdouble*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 961: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniformMatrix4fv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniformMatrix4fv(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLfloat*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 963: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniformMatrix4x2dv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniformMatrix4x2dv(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLdouble*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 964: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniformMatrix4x2dvEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniformMatrix4x2dvEXT(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLdouble*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 965: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniformMatrix4x2fv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniformMatrix4x2fv(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLfloat*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 967: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniformMatrix4x3dv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniformMatrix4x3dv(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLdouble*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 968: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniformMatrix4x3dvEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniformMatrix4x3dvEXT(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLdouble*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 969: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniformMatrix4x3fv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniformMatrix4x3fv(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLfloat*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 971: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniformui64NV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniformui64NV(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), p->value());
            return 0u;
        }

        case 972: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProgramUniformui64vNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProgramUniformui64vNV(static_cast<GLuint>(p->program()), static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLuint64EXT*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 973: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenProvokingVertex>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glProvokingVertex(static_cast<GLenum>(p->mode()));
            return 0u;
        }

        case 974: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenPushClientAttribDefaultEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glPushClientAttribDefaultEXT(static_cast<GLbitfield>(p->mask()));
            return 0u;
        }

        case 975: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenPushDebugGroup>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glPushDebugGroup(static_cast<GLenum>(p->source()), static_cast<GLuint>(p->id()), static_cast<GLsizei>(p->length()), (p->message() == nullptr ? nullptr : p->message()->c_str()));
            return 0u;
        }

        case 976: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenPushGroupMarkerEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glPushGroupMarkerEXT(static_cast<GLsizei>(p->length()), (p->marker() == nullptr ? nullptr : p->marker()->c_str()));
            return 0u;
        }

        case 977: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenQueryCounter>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glQueryCounter(static_cast<GLuint>(p->id()), static_cast<GLenum>(p->target()));
            return 0u;
        }

        case 978: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenRasterSamplesEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glRasterSamplesEXT(static_cast<GLuint>(p->samples()), static_cast<GLboolean>(p->fixedsamplelocations()));
            return 0u;
        }

        case 979: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenReadBuffer>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glReadBuffer(static_cast<GLenum>(p->src()));
            return 0u;
        }

        case 980: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenReadPixels>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glReadPixels(static_cast<GLint>(p->x()), static_cast<GLint>(p->y()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLenum>(p->format()), static_cast<GLenum>(p->type()), const_cast<void*>(ResolveShm(p->pixels(), receivedShm, receivedShmCount)));
            return 0u;
        }

        case 981: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenReadnPixels>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glReadnPixels(static_cast<GLint>(p->x()), static_cast<GLint>(p->y()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLenum>(p->format()), static_cast<GLenum>(p->type()), static_cast<GLsizei>(p->bufSize()), const_cast<void*>(ResolveShm(p->data(), receivedShm, receivedShmCount)));
            return 0u;
        }

        case 984: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenRenderbufferStorage>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glRenderbufferStorage(static_cast<GLenum>(p->target()), static_cast<GLenum>(p->internalformat()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()));
            return 0u;
        }

        case 986: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenRenderbufferStorageMultisample>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glRenderbufferStorageMultisample(static_cast<GLenum>(p->target()), static_cast<GLsizei>(p->samples()), static_cast<GLenum>(p->internalformat()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()));
            return 0u;
        }

        case 987: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenRenderbufferStorageMultisampleAdvancedAMD>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glRenderbufferStorageMultisampleAdvancedAMD(static_cast<GLenum>(p->target()), static_cast<GLsizei>(p->samples()), static_cast<GLsizei>(p->storageSamples()), static_cast<GLenum>(p->internalformat()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()));
            return 0u;
        }

        case 988: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenRenderbufferStorageMultisampleCoverageNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glRenderbufferStorageMultisampleCoverageNV(static_cast<GLenum>(p->target()), static_cast<GLsizei>(p->coverageSamples()), static_cast<GLsizei>(p->colorSamples()), static_cast<GLenum>(p->internalformat()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()));
            return 0u;
        }

        case 989: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenResetMemoryObjectParameterNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glResetMemoryObjectParameterNV(static_cast<GLuint>(p->memory()), static_cast<GLenum>(p->pname()));
            return 0u;
        }

        case 992: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenSampleCoverage>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glSampleCoverage(static_cast<GLfloat>(p->value()), static_cast<GLboolean>(p->invert()));
            return 0u;
        }

        case 993: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenSampleMaski>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glSampleMaski(static_cast<GLuint>(p->maskNumber()), static_cast<GLbitfield>(p->mask()));
            return 0u;
        }

        case 994: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenSamplerParameterIiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glSamplerParameterIiv(static_cast<GLuint>(p->sampler()), static_cast<GLenum>(p->pname()), reinterpret_cast<const GLint*>((p->param() == nullptr ? nullptr : p->param()->data())));
            return 0u;
        }

        case 995: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenSamplerParameterIuiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glSamplerParameterIuiv(static_cast<GLuint>(p->sampler()), static_cast<GLenum>(p->pname()), reinterpret_cast<const GLuint*>((p->param() == nullptr ? nullptr : p->param()->data())));
            return 0u;
        }

        case 996: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenSamplerParameterf>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glSamplerParameterf(static_cast<GLuint>(p->sampler()), static_cast<GLenum>(p->pname()), static_cast<GLfloat>(p->param()));
            return 0u;
        }

        case 997: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenSamplerParameterfv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glSamplerParameterfv(static_cast<GLuint>(p->sampler()), static_cast<GLenum>(p->pname()), reinterpret_cast<const GLfloat*>((p->param() == nullptr ? nullptr : p->param()->data())));
            return 0u;
        }

        case 998: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenSamplerParameteri>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glSamplerParameteri(static_cast<GLuint>(p->sampler()), static_cast<GLenum>(p->pname()), static_cast<GLint>(p->param()));
            return 0u;
        }

        case 999: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenSamplerParameteriv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glSamplerParameteriv(static_cast<GLuint>(p->sampler()), static_cast<GLenum>(p->pname()), reinterpret_cast<const GLint*>((p->param() == nullptr ? nullptr : p->param()->data())));
            return 0u;
        }

        case 1000: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenScissor>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glScissor(static_cast<GLint>(p->x()), static_cast<GLint>(p->y()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()));
            return 0u;
        }

        case 1001: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenScissorArrayv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glScissorArrayv(static_cast<GLuint>(p->first()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLint*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1002: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenScissorExclusiveArrayvNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glScissorExclusiveArrayvNV(static_cast<GLuint>(p->first()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLint*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1003: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenScissorExclusiveNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glScissorExclusiveNV(static_cast<GLint>(p->x()), static_cast<GLint>(p->y()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()));
            return 0u;
        }

        case 1004: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenScissorIndexed>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glScissorIndexed(static_cast<GLuint>(p->index()), static_cast<GLint>(p->left()), static_cast<GLint>(p->bottom()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()));
            return 0u;
        }

        case 1005: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenScissorIndexedv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glScissorIndexedv(static_cast<GLuint>(p->index()), reinterpret_cast<const GLint*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1006: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenSecondaryColorFormatNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glSecondaryColorFormatNV(static_cast<GLint>(p->size()), static_cast<GLenum>(p->type()), static_cast<GLsizei>(p->stride()));
            return 0u;
        }

        case 1007: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenSelectPerfMonitorCountersAMD>(payloadBytes);
            if (p == nullptr) { return 1u; }
            std::vector<uint32_t>_out_counterList(static_cast<size_t>((outCapacity != 0 ? outCapacity : 1u)));
            ::glSelectPerfMonitorCountersAMD(static_cast<GLuint>(p->monitor()), static_cast<GLboolean>(p->enable()), static_cast<GLuint>(p->group()), static_cast<GLint>(p->numCounters()), reinterpret_cast<GLuint*>(_out_counterList.data()));
            WireDispatchStoreBytes(reinterpret_cast<const uint8_t*>(_out_counterList.data()),
                                  static_cast<uint32_t>(_out_counterList.size() * 4));
            return 0u;
        }

        case 1008: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenShaderBinary>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glShaderBinary(static_cast<GLsizei>(p->count()), reinterpret_cast<const GLuint*>((p->shaders() == nullptr ? nullptr : p->shaders()->data())), static_cast<GLenum>(p->binaryformat()), ResolveShm(p->binary(), receivedShm, receivedShmCount), static_cast<GLsizei>(p->length()));
            return 0u;
        }

        case 1010: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenShaderStorageBlockBinding>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glShaderStorageBlockBinding(static_cast<GLuint>(p->program()), static_cast<GLuint>(p->storageBlockIndex()), static_cast<GLuint>(p->storageBlockBinding()));
            return 0u;
        }

        case 1013: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenShadingRateImageBarrierNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glShadingRateImageBarrierNV(static_cast<GLboolean>(p->synchronize()));
            return 0u;
        }

        case 1014: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenShadingRateImagePaletteNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glShadingRateImagePaletteNV(static_cast<GLuint>(p->viewport()), static_cast<GLuint>(p->first()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLenum*>((p->rates() == nullptr ? nullptr : p->rates()->data())));
            return 0u;
        }

        case 1015: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenShadingRateSampleOrderCustomNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glShadingRateSampleOrderCustomNV(static_cast<GLenum>(p->rate()), static_cast<GLuint>(p->samples()), reinterpret_cast<const GLint*>((p->locations() == nullptr ? nullptr : p->locations()->data())));
            return 0u;
        }

        case 1016: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenShadingRateSampleOrderNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glShadingRateSampleOrderNV(static_cast<GLenum>(p->order()));
            return 0u;
        }

        case 1017: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenSignalVkFenceNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glSignalVkFenceNV(static_cast<GLuint64>(p->vkFence()));
            return 0u;
        }

        case 1018: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenSignalVkSemaphoreNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glSignalVkSemaphoreNV(static_cast<GLuint64>(p->vkSemaphore()));
            return 0u;
        }

        case 1019: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenSpecializeShader>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glSpecializeShader(static_cast<GLuint>(p->shader()), (p->pEntryPoint() == nullptr ? nullptr : p->pEntryPoint()->c_str()), static_cast<GLuint>(p->numSpecializationConstants()), reinterpret_cast<const GLuint*>((p->pConstantIndex() == nullptr ? nullptr : p->pConstantIndex()->data())), reinterpret_cast<const GLuint*>((p->pConstantValue() == nullptr ? nullptr : p->pConstantValue()->data())));
            return 0u;
        }

        case 1020: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenSpecializeShaderARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glSpecializeShaderARB(static_cast<GLuint>(p->shader()), (p->pEntryPoint() == nullptr ? nullptr : p->pEntryPoint()->c_str()), static_cast<GLuint>(p->numSpecializationConstants()), reinterpret_cast<const GLuint*>((p->pConstantIndex() == nullptr ? nullptr : p->pConstantIndex()->data())), reinterpret_cast<const GLuint*>((p->pConstantValue() == nullptr ? nullptr : p->pConstantValue()->data())));
            return 0u;
        }

        case 1021: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenStateCaptureNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glStateCaptureNV(static_cast<GLuint>(p->state()), static_cast<GLenum>(p->mode()));
            return 0u;
        }

        case 1022: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenStencilFillPathInstancedNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glStencilFillPathInstancedNV(static_cast<GLsizei>(p->numPaths()), static_cast<GLenum>(p->pathNameType()), ResolveShm(p->paths(), receivedShm, receivedShmCount), static_cast<GLuint>(p->pathBase()), static_cast<GLenum>(p->fillMode()), static_cast<GLuint>(p->mask()), static_cast<GLenum>(p->transformType()), reinterpret_cast<const GLfloat*>((p->transformValues() == nullptr ? nullptr : p->transformValues()->data())));
            return 0u;
        }

        case 1023: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenStencilFillPathNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glStencilFillPathNV(static_cast<GLuint>(p->path()), static_cast<GLenum>(p->fillMode()), static_cast<GLuint>(p->mask()));
            return 0u;
        }

        case 1024: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenStencilFunc>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glStencilFunc(static_cast<GLenum>(p->func()), static_cast<GLint>(p->ref()), static_cast<GLuint>(p->mask()));
            return 0u;
        }

        case 1025: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenStencilFuncSeparate>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glStencilFuncSeparate(static_cast<GLenum>(p->face()), static_cast<GLenum>(p->func()), static_cast<GLint>(p->ref()), static_cast<GLuint>(p->mask()));
            return 0u;
        }

        case 1026: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenStencilMask>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glStencilMask(static_cast<GLuint>(p->mask()));
            return 0u;
        }

        case 1027: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenStencilMaskSeparate>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glStencilMaskSeparate(static_cast<GLenum>(p->face()), static_cast<GLuint>(p->mask()));
            return 0u;
        }

        case 1028: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenStencilOp>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glStencilOp(static_cast<GLenum>(p->fail()), static_cast<GLenum>(p->zfail()), static_cast<GLenum>(p->zpass()));
            return 0u;
        }

        case 1029: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenStencilOpSeparate>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glStencilOpSeparate(static_cast<GLenum>(p->face()), static_cast<GLenum>(p->sfail()), static_cast<GLenum>(p->dpfail()), static_cast<GLenum>(p->dppass()));
            return 0u;
        }

        case 1030: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenStencilStrokePathInstancedNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glStencilStrokePathInstancedNV(static_cast<GLsizei>(p->numPaths()), static_cast<GLenum>(p->pathNameType()), ResolveShm(p->paths(), receivedShm, receivedShmCount), static_cast<GLuint>(p->pathBase()), static_cast<GLint>(p->reference()), static_cast<GLuint>(p->mask()), static_cast<GLenum>(p->transformType()), reinterpret_cast<const GLfloat*>((p->transformValues() == nullptr ? nullptr : p->transformValues()->data())));
            return 0u;
        }

        case 1031: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenStencilStrokePathNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glStencilStrokePathNV(static_cast<GLuint>(p->path()), static_cast<GLint>(p->reference()), static_cast<GLuint>(p->mask()));
            return 0u;
        }

        case 1032: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenStencilThenCoverFillPathInstancedNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glStencilThenCoverFillPathInstancedNV(static_cast<GLsizei>(p->numPaths()), static_cast<GLenum>(p->pathNameType()), ResolveShm(p->paths(), receivedShm, receivedShmCount), static_cast<GLuint>(p->pathBase()), static_cast<GLenum>(p->fillMode()), static_cast<GLuint>(p->mask()), static_cast<GLenum>(p->coverMode()), static_cast<GLenum>(p->transformType()), reinterpret_cast<const GLfloat*>((p->transformValues() == nullptr ? nullptr : p->transformValues()->data())));
            return 0u;
        }

        case 1033: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenStencilThenCoverFillPathNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glStencilThenCoverFillPathNV(static_cast<GLuint>(p->path()), static_cast<GLenum>(p->fillMode()), static_cast<GLuint>(p->mask()), static_cast<GLenum>(p->coverMode()));
            return 0u;
        }

        case 1034: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenStencilThenCoverStrokePathInstancedNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glStencilThenCoverStrokePathInstancedNV(static_cast<GLsizei>(p->numPaths()), static_cast<GLenum>(p->pathNameType()), ResolveShm(p->paths(), receivedShm, receivedShmCount), static_cast<GLuint>(p->pathBase()), static_cast<GLint>(p->reference()), static_cast<GLuint>(p->mask()), static_cast<GLenum>(p->coverMode()), static_cast<GLenum>(p->transformType()), reinterpret_cast<const GLfloat*>((p->transformValues() == nullptr ? nullptr : p->transformValues()->data())));
            return 0u;
        }

        case 1035: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenStencilThenCoverStrokePathNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glStencilThenCoverStrokePathNV(static_cast<GLuint>(p->path()), static_cast<GLint>(p->reference()), static_cast<GLuint>(p->mask()), static_cast<GLenum>(p->coverMode()));
            return 0u;
        }

        case 1036: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenSubpixelPrecisionBiasNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glSubpixelPrecisionBiasNV(static_cast<GLuint>(p->xbits()), static_cast<GLuint>(p->ybits()));
            return 0u;
        }

        case 1037: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTexAttachMemoryNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTexAttachMemoryNV(static_cast<GLenum>(p->target()), static_cast<GLuint>(p->memory()), static_cast<GLuint64>(p->offset()));
            return 0u;
        }

        case 1038: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTexBuffer>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTexBuffer(static_cast<GLenum>(p->target()), static_cast<GLenum>(p->internalformat()), static_cast<GLuint>(p->buffer()));
            return 0u;
        }

        case 1040: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTexBufferRange>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTexBufferRange(static_cast<GLenum>(p->target()), static_cast<GLenum>(p->internalformat()), static_cast<GLuint>(p->buffer()), static_cast<GLintptr>(p->offset()), static_cast<GLsizeiptr>(p->size()));
            return 0u;
        }

        case 1041: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTexCoordFormatNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTexCoordFormatNV(static_cast<GLint>(p->size()), static_cast<GLenum>(p->type()), static_cast<GLsizei>(p->stride()));
            return 0u;
        }

        case 1042: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTexImage1D>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTexImage1D(static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLint>(p->internalFormat()), static_cast<GLsizei>(p->width()), static_cast<GLint>(p->border()), static_cast<GLenum>(p->format()), static_cast<GLenum>(p->type()), ResolveShm(p->pixels(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 1043: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTexImage2D>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTexImage2D(static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLint>(p->internalformat()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLint>(p->border()), static_cast<GLenum>(p->format()), static_cast<GLenum>(p->type()), ResolveShm(p->pixels(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 1044: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTexImage2DMultisample>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTexImage2DMultisample(static_cast<GLenum>(p->target()), static_cast<GLsizei>(p->samples()), static_cast<GLenum>(p->internalformat()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLboolean>(p->fixedsamplelocations()));
            return 0u;
        }

        case 1045: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTexImage3D>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTexImage3D(static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLint>(p->internalformat()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLsizei>(p->depth()), static_cast<GLint>(p->border()), static_cast<GLenum>(p->format()), static_cast<GLenum>(p->type()), ResolveShm(p->pixels(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 1046: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTexImage3DMultisample>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTexImage3DMultisample(static_cast<GLenum>(p->target()), static_cast<GLsizei>(p->samples()), static_cast<GLenum>(p->internalformat()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLsizei>(p->depth()), static_cast<GLboolean>(p->fixedsamplelocations()));
            return 0u;
        }

        case 1047: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTexPageCommitmentARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTexPageCommitmentARB(static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLint>(p->xoffset()), static_cast<GLint>(p->yoffset()), static_cast<GLint>(p->zoffset()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLsizei>(p->depth()), static_cast<GLboolean>(p->commit()));
            return 0u;
        }

        case 1048: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTexPageCommitmentMemNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTexPageCommitmentMemNV(static_cast<GLenum>(p->target()), static_cast<GLint>(p->layer()), static_cast<GLint>(p->level()), static_cast<GLint>(p->xoffset()), static_cast<GLint>(p->yoffset()), static_cast<GLint>(p->zoffset()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLsizei>(p->depth()), static_cast<GLuint>(p->memory()), static_cast<GLuint64>(p->offset()), static_cast<GLboolean>(p->commit()));
            return 0u;
        }

        case 1049: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTexParameterIiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTexParameterIiv(static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), reinterpret_cast<const GLint*>((p->params() == nullptr ? nullptr : p->params()->data())));
            return 0u;
        }

        case 1050: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTexParameterIuiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTexParameterIuiv(static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), reinterpret_cast<const GLuint*>((p->params() == nullptr ? nullptr : p->params()->data())));
            return 0u;
        }

        case 1051: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTexParameterf>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTexParameterf(static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), static_cast<GLfloat>(p->param()));
            return 0u;
        }

        case 1052: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTexParameterfv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTexParameterfv(static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), reinterpret_cast<const GLfloat*>((p->params() == nullptr ? nullptr : p->params()->data())));
            return 0u;
        }

        case 1053: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTexParameteri>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTexParameteri(static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), static_cast<GLint>(p->param()));
            return 0u;
        }

        case 1054: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTexParameteriv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTexParameteriv(static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), reinterpret_cast<const GLint*>((p->params() == nullptr ? nullptr : p->params()->data())));
            return 0u;
        }

        case 1056: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTexStorage1DEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTexStorage1DEXT(static_cast<GLenum>(p->target()), static_cast<GLsizei>(p->levels()), static_cast<GLenum>(p->internalformat()), static_cast<GLsizei>(p->width()));
            return 0u;
        }

        case 1057: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTexStorage2D>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTexStorage2D(static_cast<GLenum>(p->target()), static_cast<GLsizei>(p->levels()), static_cast<GLenum>(p->internalformat()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()));
            return 0u;
        }

        case 1059: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTexStorage2DMultisample>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTexStorage2DMultisample(static_cast<GLenum>(p->target()), static_cast<GLsizei>(p->samples()), static_cast<GLenum>(p->internalformat()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLboolean>(p->fixedsamplelocations()));
            return 0u;
        }

        case 1060: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTexStorage3D>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTexStorage3D(static_cast<GLenum>(p->target()), static_cast<GLsizei>(p->levels()), static_cast<GLenum>(p->internalformat()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLsizei>(p->depth()));
            return 0u;
        }

        case 1062: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTexStorage3DMultisample>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTexStorage3DMultisample(static_cast<GLenum>(p->target()), static_cast<GLsizei>(p->samples()), static_cast<GLenum>(p->internalformat()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLsizei>(p->depth()), static_cast<GLboolean>(p->fixedsamplelocations()));
            return 0u;
        }

        case 1063: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTexSubImage1D>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTexSubImage1D(static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLint>(p->xoffset()), static_cast<GLsizei>(p->width()), static_cast<GLenum>(p->format()), static_cast<GLenum>(p->type()), ResolveShm(p->pixels(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 1064: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTexSubImage2D>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTexSubImage2D(static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLint>(p->xoffset()), static_cast<GLint>(p->yoffset()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLenum>(p->format()), static_cast<GLenum>(p->type()), ResolveShm(p->pixels(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 1065: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTexSubImage3D>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTexSubImage3D(static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLint>(p->xoffset()), static_cast<GLint>(p->yoffset()), static_cast<GLint>(p->zoffset()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLsizei>(p->depth()), static_cast<GLenum>(p->format()), static_cast<GLenum>(p->type()), ResolveShm(p->pixels(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 1066: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTextureAttachMemoryNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTextureAttachMemoryNV(static_cast<GLuint>(p->texture()), static_cast<GLuint>(p->memory()), static_cast<GLuint64>(p->offset()));
            return 0u;
        }

        case 1069: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTextureBuffer>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTextureBuffer(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->internalformat()), static_cast<GLuint>(p->buffer()));
            return 0u;
        }

        case 1070: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTextureBufferEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTextureBufferEXT(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->target()), static_cast<GLenum>(p->internalformat()), static_cast<GLuint>(p->buffer()));
            return 0u;
        }

        case 1071: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTextureBufferRange>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTextureBufferRange(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->internalformat()), static_cast<GLuint>(p->buffer()), static_cast<GLintptr>(p->offset()), static_cast<GLsizeiptr>(p->size()));
            return 0u;
        }

        case 1072: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTextureBufferRangeEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTextureBufferRangeEXT(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->target()), static_cast<GLenum>(p->internalformat()), static_cast<GLuint>(p->buffer()), static_cast<GLintptr>(p->offset()), static_cast<GLsizeiptr>(p->size()));
            return 0u;
        }

        case 1073: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTextureImage1DEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTextureImage1DEXT(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLint>(p->internalformat()), static_cast<GLsizei>(p->width()), static_cast<GLint>(p->border()), static_cast<GLenum>(p->format()), static_cast<GLenum>(p->type()), ResolveShm(p->pixels(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 1074: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTextureImage2DEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTextureImage2DEXT(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLint>(p->internalformat()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLint>(p->border()), static_cast<GLenum>(p->format()), static_cast<GLenum>(p->type()), ResolveShm(p->pixels(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 1075: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTextureImage3DEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTextureImage3DEXT(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLint>(p->internalformat()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLsizei>(p->depth()), static_cast<GLint>(p->border()), static_cast<GLenum>(p->format()), static_cast<GLenum>(p->type()), ResolveShm(p->pixels(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 1076: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTexturePageCommitmentEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTexturePageCommitmentEXT(static_cast<GLuint>(p->texture()), static_cast<GLint>(p->level()), static_cast<GLint>(p->xoffset()), static_cast<GLint>(p->yoffset()), static_cast<GLint>(p->zoffset()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLsizei>(p->depth()), static_cast<GLboolean>(p->commit()));
            return 0u;
        }

        case 1077: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTexturePageCommitmentMemNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTexturePageCommitmentMemNV(static_cast<GLuint>(p->texture()), static_cast<GLint>(p->layer()), static_cast<GLint>(p->level()), static_cast<GLint>(p->xoffset()), static_cast<GLint>(p->yoffset()), static_cast<GLint>(p->zoffset()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLsizei>(p->depth()), static_cast<GLuint>(p->memory()), static_cast<GLuint64>(p->offset()), static_cast<GLboolean>(p->commit()));
            return 0u;
        }

        case 1078: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTextureParameterIiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTextureParameterIiv(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->pname()), reinterpret_cast<const GLint*>((p->params() == nullptr ? nullptr : p->params()->data())));
            return 0u;
        }

        case 1079: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTextureParameterIivEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTextureParameterIivEXT(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), reinterpret_cast<const GLint*>((p->params() == nullptr ? nullptr : p->params()->data())));
            return 0u;
        }

        case 1080: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTextureParameterIuiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTextureParameterIuiv(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->pname()), reinterpret_cast<const GLuint*>((p->params() == nullptr ? nullptr : p->params()->data())));
            return 0u;
        }

        case 1081: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTextureParameterIuivEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTextureParameterIuivEXT(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), reinterpret_cast<const GLuint*>((p->params() == nullptr ? nullptr : p->params()->data())));
            return 0u;
        }

        case 1082: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTextureParameterf>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTextureParameterf(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->pname()), static_cast<GLfloat>(p->param()));
            return 0u;
        }

        case 1083: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTextureParameterfEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTextureParameterfEXT(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), static_cast<GLfloat>(p->param()));
            return 0u;
        }

        case 1084: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTextureParameterfv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTextureParameterfv(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->pname()), reinterpret_cast<const GLfloat*>((p->param() == nullptr ? nullptr : p->param()->data())));
            return 0u;
        }

        case 1085: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTextureParameterfvEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTextureParameterfvEXT(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), reinterpret_cast<const GLfloat*>((p->params() == nullptr ? nullptr : p->params()->data())));
            return 0u;
        }

        case 1086: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTextureParameteri>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTextureParameteri(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->pname()), static_cast<GLint>(p->param()));
            return 0u;
        }

        case 1087: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTextureParameteriEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTextureParameteriEXT(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), static_cast<GLint>(p->param()));
            return 0u;
        }

        case 1088: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTextureParameteriv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTextureParameteriv(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->pname()), reinterpret_cast<const GLint*>((p->param() == nullptr ? nullptr : p->param()->data())));
            return 0u;
        }

        case 1089: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTextureParameterivEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTextureParameterivEXT(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->target()), static_cast<GLenum>(p->pname()), reinterpret_cast<const GLint*>((p->params() == nullptr ? nullptr : p->params()->data())));
            return 0u;
        }

        case 1090: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTextureRenderbufferEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTextureRenderbufferEXT(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->target()), static_cast<GLuint>(p->renderbuffer()));
            return 0u;
        }

        case 1091: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTextureStorage1D>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTextureStorage1D(static_cast<GLuint>(p->texture()), static_cast<GLsizei>(p->levels()), static_cast<GLenum>(p->internalformat()), static_cast<GLsizei>(p->width()));
            return 0u;
        }

        case 1092: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTextureStorage1DEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTextureStorage1DEXT(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->target()), static_cast<GLsizei>(p->levels()), static_cast<GLenum>(p->internalformat()), static_cast<GLsizei>(p->width()));
            return 0u;
        }

        case 1093: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTextureStorage2D>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTextureStorage2D(static_cast<GLuint>(p->texture()), static_cast<GLsizei>(p->levels()), static_cast<GLenum>(p->internalformat()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()));
            return 0u;
        }

        case 1094: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTextureStorage2DEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTextureStorage2DEXT(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->target()), static_cast<GLsizei>(p->levels()), static_cast<GLenum>(p->internalformat()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()));
            return 0u;
        }

        case 1095: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTextureStorage2DMultisample>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTextureStorage2DMultisample(static_cast<GLuint>(p->texture()), static_cast<GLsizei>(p->samples()), static_cast<GLenum>(p->internalformat()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLboolean>(p->fixedsamplelocations()));
            return 0u;
        }

        case 1096: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTextureStorage2DMultisampleEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTextureStorage2DMultisampleEXT(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->target()), static_cast<GLsizei>(p->samples()), static_cast<GLenum>(p->internalformat()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLboolean>(p->fixedsamplelocations()));
            return 0u;
        }

        case 1097: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTextureStorage3D>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTextureStorage3D(static_cast<GLuint>(p->texture()), static_cast<GLsizei>(p->levels()), static_cast<GLenum>(p->internalformat()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLsizei>(p->depth()));
            return 0u;
        }

        case 1098: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTextureStorage3DEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTextureStorage3DEXT(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->target()), static_cast<GLsizei>(p->levels()), static_cast<GLenum>(p->internalformat()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLsizei>(p->depth()));
            return 0u;
        }

        case 1099: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTextureStorage3DMultisample>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTextureStorage3DMultisample(static_cast<GLuint>(p->texture()), static_cast<GLsizei>(p->samples()), static_cast<GLenum>(p->internalformat()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLsizei>(p->depth()), static_cast<GLboolean>(p->fixedsamplelocations()));
            return 0u;
        }

        case 1100: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTextureStorage3DMultisampleEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTextureStorage3DMultisampleEXT(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->target()), static_cast<GLsizei>(p->samples()), static_cast<GLenum>(p->internalformat()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLsizei>(p->depth()), static_cast<GLboolean>(p->fixedsamplelocations()));
            return 0u;
        }

        case 1101: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTextureSubImage1D>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTextureSubImage1D(static_cast<GLuint>(p->texture()), static_cast<GLint>(p->level()), static_cast<GLint>(p->xoffset()), static_cast<GLsizei>(p->width()), static_cast<GLenum>(p->format()), static_cast<GLenum>(p->type()), ResolveShm(p->pixels(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 1102: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTextureSubImage1DEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTextureSubImage1DEXT(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLint>(p->xoffset()), static_cast<GLsizei>(p->width()), static_cast<GLenum>(p->format()), static_cast<GLenum>(p->type()), ResolveShm(p->pixels(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 1103: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTextureSubImage2D>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTextureSubImage2D(static_cast<GLuint>(p->texture()), static_cast<GLint>(p->level()), static_cast<GLint>(p->xoffset()), static_cast<GLint>(p->yoffset()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLenum>(p->format()), static_cast<GLenum>(p->type()), ResolveShm(p->pixels(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 1104: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTextureSubImage2DEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTextureSubImage2DEXT(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLint>(p->xoffset()), static_cast<GLint>(p->yoffset()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLenum>(p->format()), static_cast<GLenum>(p->type()), ResolveShm(p->pixels(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 1105: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTextureSubImage3D>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTextureSubImage3D(static_cast<GLuint>(p->texture()), static_cast<GLint>(p->level()), static_cast<GLint>(p->xoffset()), static_cast<GLint>(p->yoffset()), static_cast<GLint>(p->zoffset()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLsizei>(p->depth()), static_cast<GLenum>(p->format()), static_cast<GLenum>(p->type()), ResolveShm(p->pixels(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 1106: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTextureSubImage3DEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTextureSubImage3DEXT(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->target()), static_cast<GLint>(p->level()), static_cast<GLint>(p->xoffset()), static_cast<GLint>(p->yoffset()), static_cast<GLint>(p->zoffset()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()), static_cast<GLsizei>(p->depth()), static_cast<GLenum>(p->format()), static_cast<GLenum>(p->type()), ResolveShm(p->pixels(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 1107: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTextureView>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTextureView(static_cast<GLuint>(p->texture()), static_cast<GLenum>(p->target()), static_cast<GLuint>(p->origtexture()), static_cast<GLenum>(p->internalformat()), static_cast<GLuint>(p->minlevel()), static_cast<GLuint>(p->numlevels()), static_cast<GLuint>(p->minlayer()), static_cast<GLuint>(p->numlayers()));
            return 0u;
        }

        case 1108: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTransformFeedbackBufferBase>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTransformFeedbackBufferBase(static_cast<GLuint>(p->xfb()), static_cast<GLuint>(p->index()), static_cast<GLuint>(p->buffer()));
            return 0u;
        }

        case 1109: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTransformFeedbackBufferRange>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTransformFeedbackBufferRange(static_cast<GLuint>(p->xfb()), static_cast<GLuint>(p->index()), static_cast<GLuint>(p->buffer()), static_cast<GLintptr>(p->offset()), static_cast<GLsizeiptr>(p->size()));
            return 0u;
        }

        case 1111: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenTransformPathNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glTransformPathNV(static_cast<GLuint>(p->resultPath()), static_cast<GLuint>(p->srcPath()), static_cast<GLenum>(p->transformType()), reinterpret_cast<const GLfloat*>((p->transformValues() == nullptr ? nullptr : p->transformValues()->data())));
            return 0u;
        }

        case 1112: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform1d>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform1d(static_cast<GLint>(p->location()), static_cast<GLdouble>(p->x()));
            return 0u;
        }

        case 1113: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform1dv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform1dv(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLdouble*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1114: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform1f>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform1f(static_cast<GLint>(p->location()), static_cast<GLfloat>(p->v0()));
            return 0u;
        }

        case 1115: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform1fv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform1fv(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLfloat*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1116: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform1i>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform1i(static_cast<GLint>(p->location()), static_cast<GLint>(p->v0()));
            return 0u;
        }

        case 1117: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform1i64ARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform1i64ARB(static_cast<GLint>(p->location()), static_cast<GLint64>(p->x()));
            return 0u;
        }

        case 1118: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform1i64NV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform1i64NV(static_cast<GLint>(p->location()), p->x());
            return 0u;
        }

        case 1119: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform1i64vARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform1i64vARB(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLint64*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1120: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform1i64vNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform1i64vNV(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLint64EXT*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1121: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform1iv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform1iv(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLint*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1122: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform1ui>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform1ui(static_cast<GLint>(p->location()), static_cast<GLuint>(p->v0()));
            return 0u;
        }

        case 1123: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform1ui64ARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform1ui64ARB(static_cast<GLint>(p->location()), static_cast<GLuint64>(p->x()));
            return 0u;
        }

        case 1124: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform1ui64NV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform1ui64NV(static_cast<GLint>(p->location()), p->x());
            return 0u;
        }

        case 1125: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform1ui64vARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform1ui64vARB(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLuint64*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1126: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform1ui64vNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform1ui64vNV(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLuint64EXT*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1127: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform1uiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform1uiv(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLuint*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1128: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform2d>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform2d(static_cast<GLint>(p->location()), static_cast<GLdouble>(p->x()), static_cast<GLdouble>(p->y()));
            return 0u;
        }

        case 1129: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform2dv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform2dv(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLdouble*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1130: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform2f>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform2f(static_cast<GLint>(p->location()), static_cast<GLfloat>(p->v0()), static_cast<GLfloat>(p->v1()));
            return 0u;
        }

        case 1131: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform2fv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform2fv(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLfloat*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1132: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform2i>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform2i(static_cast<GLint>(p->location()), static_cast<GLint>(p->v0()), static_cast<GLint>(p->v1()));
            return 0u;
        }

        case 1133: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform2i64ARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform2i64ARB(static_cast<GLint>(p->location()), static_cast<GLint64>(p->x()), static_cast<GLint64>(p->y()));
            return 0u;
        }

        case 1134: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform2i64NV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform2i64NV(static_cast<GLint>(p->location()), p->x(), p->y());
            return 0u;
        }

        case 1135: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform2i64vARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform2i64vARB(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLint64*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1136: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform2i64vNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform2i64vNV(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLint64EXT*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1137: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform2iv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform2iv(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLint*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1138: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform2ui>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform2ui(static_cast<GLint>(p->location()), static_cast<GLuint>(p->v0()), static_cast<GLuint>(p->v1()));
            return 0u;
        }

        case 1139: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform2ui64ARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform2ui64ARB(static_cast<GLint>(p->location()), static_cast<GLuint64>(p->x()), static_cast<GLuint64>(p->y()));
            return 0u;
        }

        case 1140: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform2ui64NV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform2ui64NV(static_cast<GLint>(p->location()), p->x(), p->y());
            return 0u;
        }

        case 1141: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform2ui64vARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform2ui64vARB(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLuint64*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1142: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform2ui64vNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform2ui64vNV(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLuint64EXT*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1143: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform2uiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform2uiv(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLuint*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1144: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform3d>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform3d(static_cast<GLint>(p->location()), static_cast<GLdouble>(p->x()), static_cast<GLdouble>(p->y()), static_cast<GLdouble>(p->z()));
            return 0u;
        }

        case 1145: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform3dv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform3dv(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLdouble*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1146: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform3f>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform3f(static_cast<GLint>(p->location()), static_cast<GLfloat>(p->v0()), static_cast<GLfloat>(p->v1()), static_cast<GLfloat>(p->v2()));
            return 0u;
        }

        case 1147: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform3fv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform3fv(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLfloat*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1148: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform3i>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform3i(static_cast<GLint>(p->location()), static_cast<GLint>(p->v0()), static_cast<GLint>(p->v1()), static_cast<GLint>(p->v2()));
            return 0u;
        }

        case 1149: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform3i64ARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform3i64ARB(static_cast<GLint>(p->location()), static_cast<GLint64>(p->x()), static_cast<GLint64>(p->y()), static_cast<GLint64>(p->z()));
            return 0u;
        }

        case 1150: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform3i64NV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform3i64NV(static_cast<GLint>(p->location()), p->x(), p->y(), p->z());
            return 0u;
        }

        case 1151: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform3i64vARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform3i64vARB(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLint64*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1152: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform3i64vNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform3i64vNV(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLint64EXT*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1153: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform3iv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform3iv(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLint*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1154: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform3ui>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform3ui(static_cast<GLint>(p->location()), static_cast<GLuint>(p->v0()), static_cast<GLuint>(p->v1()), static_cast<GLuint>(p->v2()));
            return 0u;
        }

        case 1155: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform3ui64ARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform3ui64ARB(static_cast<GLint>(p->location()), static_cast<GLuint64>(p->x()), static_cast<GLuint64>(p->y()), static_cast<GLuint64>(p->z()));
            return 0u;
        }

        case 1156: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform3ui64NV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform3ui64NV(static_cast<GLint>(p->location()), p->x(), p->y(), p->z());
            return 0u;
        }

        case 1157: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform3ui64vARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform3ui64vARB(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLuint64*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1158: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform3ui64vNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform3ui64vNV(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLuint64EXT*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1159: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform3uiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform3uiv(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLuint*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1160: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform4d>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform4d(static_cast<GLint>(p->location()), static_cast<GLdouble>(p->x()), static_cast<GLdouble>(p->y()), static_cast<GLdouble>(p->z()), static_cast<GLdouble>(p->w()));
            return 0u;
        }

        case 1161: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform4dv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform4dv(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLdouble*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1162: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform4f>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform4f(static_cast<GLint>(p->location()), static_cast<GLfloat>(p->v0()), static_cast<GLfloat>(p->v1()), static_cast<GLfloat>(p->v2()), static_cast<GLfloat>(p->v3()));
            return 0u;
        }

        case 1163: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform4fv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform4fv(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLfloat*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1164: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform4i>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform4i(static_cast<GLint>(p->location()), static_cast<GLint>(p->v0()), static_cast<GLint>(p->v1()), static_cast<GLint>(p->v2()), static_cast<GLint>(p->v3()));
            return 0u;
        }

        case 1165: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform4i64ARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform4i64ARB(static_cast<GLint>(p->location()), static_cast<GLint64>(p->x()), static_cast<GLint64>(p->y()), static_cast<GLint64>(p->z()), static_cast<GLint64>(p->w()));
            return 0u;
        }

        case 1166: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform4i64NV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform4i64NV(static_cast<GLint>(p->location()), p->x(), p->y(), p->z(), p->w());
            return 0u;
        }

        case 1167: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform4i64vARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform4i64vARB(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLint64*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1168: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform4i64vNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform4i64vNV(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLint64EXT*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1169: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform4iv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform4iv(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLint*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1170: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform4ui>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform4ui(static_cast<GLint>(p->location()), static_cast<GLuint>(p->v0()), static_cast<GLuint>(p->v1()), static_cast<GLuint>(p->v2()), static_cast<GLuint>(p->v3()));
            return 0u;
        }

        case 1171: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform4ui64ARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform4ui64ARB(static_cast<GLint>(p->location()), static_cast<GLuint64>(p->x()), static_cast<GLuint64>(p->y()), static_cast<GLuint64>(p->z()), static_cast<GLuint64>(p->w()));
            return 0u;
        }

        case 1172: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform4ui64NV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform4ui64NV(static_cast<GLint>(p->location()), p->x(), p->y(), p->z(), p->w());
            return 0u;
        }

        case 1173: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform4ui64vARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform4ui64vARB(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLuint64*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1174: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform4ui64vNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform4ui64vNV(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLuint64EXT*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1175: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniform4uiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniform4uiv(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLuint*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1176: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniformBlockBinding>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniformBlockBinding(static_cast<GLuint>(p->program()), static_cast<GLuint>(p->uniformBlockIndex()), static_cast<GLuint>(p->uniformBlockBinding()));
            return 0u;
        }

        case 1177: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniformHandleui64ARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniformHandleui64ARB(static_cast<GLint>(p->location()), static_cast<GLuint64>(p->value()));
            return 0u;
        }

        case 1178: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniformHandleui64NV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniformHandleui64NV(static_cast<GLint>(p->location()), static_cast<GLuint64>(p->value()));
            return 0u;
        }

        case 1179: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniformHandleui64vARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniformHandleui64vARB(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLuint64*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1180: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniformHandleui64vNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniformHandleui64vNV(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLuint64*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1181: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniformMatrix2dv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniformMatrix2dv(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLdouble*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1182: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniformMatrix2fv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniformMatrix2fv(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLfloat*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1183: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniformMatrix2x3dv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniformMatrix2x3dv(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLdouble*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1184: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniformMatrix2x3fv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniformMatrix2x3fv(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLfloat*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1185: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniformMatrix2x4dv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniformMatrix2x4dv(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLdouble*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1186: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniformMatrix2x4fv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniformMatrix2x4fv(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLfloat*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1187: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniformMatrix3dv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniformMatrix3dv(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLdouble*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1188: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniformMatrix3fv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniformMatrix3fv(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLfloat*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1189: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniformMatrix3x2dv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniformMatrix3x2dv(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLdouble*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1190: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniformMatrix3x2fv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniformMatrix3x2fv(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLfloat*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1191: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniformMatrix3x4dv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniformMatrix3x4dv(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLdouble*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1192: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniformMatrix3x4fv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniformMatrix3x4fv(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLfloat*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1193: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniformMatrix4dv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniformMatrix4dv(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLdouble*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1194: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniformMatrix4fv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniformMatrix4fv(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLfloat*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1195: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniformMatrix4x2dv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniformMatrix4x2dv(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLdouble*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1196: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniformMatrix4x2fv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniformMatrix4x2fv(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLfloat*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1197: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniformMatrix4x3dv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniformMatrix4x3dv(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLdouble*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1198: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniformMatrix4x3fv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniformMatrix4x3fv(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), static_cast<GLboolean>(p->transpose()), reinterpret_cast<const GLfloat*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1199: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniformSubroutinesuiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniformSubroutinesuiv(static_cast<GLenum>(p->shadertype()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLuint*>((p->indices() == nullptr ? nullptr : p->indices()->data())));
            return 0u;
        }

        case 1200: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniformui64NV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniformui64NV(static_cast<GLint>(p->location()), p->value());
            return 0u;
        }

        case 1201: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUniformui64vNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUniformui64vNV(static_cast<GLint>(p->location()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLuint64EXT*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1202: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUnmapBuffer>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glUnmapBuffer(static_cast<GLenum>(p->target()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 1203: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUnmapNamedBuffer>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glUnmapNamedBuffer(static_cast<GLuint>(p->buffer()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 1204: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUnmapNamedBufferEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            { const auto _ret = ::glUnmapNamedBufferEXT(static_cast<GLuint>(p->buffer()));
              WireDispatchStoreI64(static_cast<std::int64_t>(_ret)); }
            return 0u;
        }

        case 1205: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUseProgram>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUseProgram(static_cast<GLuint>(p->program()));
            return 0u;
        }

        case 1206: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUseProgramStages>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUseProgramStages(static_cast<GLuint>(p->pipeline()), static_cast<GLbitfield>(p->stages()), static_cast<GLuint>(p->program()));
            return 0u;
        }

        case 1207: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenUseShaderProgramEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glUseShaderProgramEXT(static_cast<GLenum>(p->type()), static_cast<GLuint>(p->program()));
            return 0u;
        }

        case 1208: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenValidateProgram>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glValidateProgram(static_cast<GLuint>(p->program()));
            return 0u;
        }

        case 1209: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenValidateProgramPipeline>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glValidateProgramPipeline(static_cast<GLuint>(p->pipeline()));
            return 0u;
        }

        case 1210: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexArrayAttribBinding>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexArrayAttribBinding(static_cast<GLuint>(p->vaobj()), static_cast<GLuint>(p->attribindex()), static_cast<GLuint>(p->bindingindex()));
            return 0u;
        }

        case 1211: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexArrayAttribFormat>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexArrayAttribFormat(static_cast<GLuint>(p->vaobj()), static_cast<GLuint>(p->attribindex()), static_cast<GLint>(p->size()), static_cast<GLenum>(p->type()), static_cast<GLboolean>(p->normalized()), static_cast<GLuint>(p->relativeoffset()));
            return 0u;
        }

        case 1212: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexArrayAttribIFormat>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexArrayAttribIFormat(static_cast<GLuint>(p->vaobj()), static_cast<GLuint>(p->attribindex()), static_cast<GLint>(p->size()), static_cast<GLenum>(p->type()), static_cast<GLuint>(p->relativeoffset()));
            return 0u;
        }

        case 1213: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexArrayAttribLFormat>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexArrayAttribLFormat(static_cast<GLuint>(p->vaobj()), static_cast<GLuint>(p->attribindex()), static_cast<GLint>(p->size()), static_cast<GLenum>(p->type()), static_cast<GLuint>(p->relativeoffset()));
            return 0u;
        }

        case 1214: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexArrayBindVertexBufferEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexArrayBindVertexBufferEXT(static_cast<GLuint>(p->vaobj()), static_cast<GLuint>(p->bindingindex()), static_cast<GLuint>(p->buffer()), static_cast<GLintptr>(p->offset()), static_cast<GLsizei>(p->stride()));
            return 0u;
        }

        case 1215: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexArrayBindingDivisor>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexArrayBindingDivisor(static_cast<GLuint>(p->vaobj()), static_cast<GLuint>(p->bindingindex()), static_cast<GLuint>(p->divisor()));
            return 0u;
        }

        case 1216: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexArrayColorOffsetEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexArrayColorOffsetEXT(static_cast<GLuint>(p->vaobj()), static_cast<GLuint>(p->buffer()), static_cast<GLint>(p->size()), static_cast<GLenum>(p->type()), static_cast<GLsizei>(p->stride()), static_cast<GLintptr>(p->offset()));
            return 0u;
        }

        case 1217: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexArrayEdgeFlagOffsetEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexArrayEdgeFlagOffsetEXT(static_cast<GLuint>(p->vaobj()), static_cast<GLuint>(p->buffer()), static_cast<GLsizei>(p->stride()), static_cast<GLintptr>(p->offset()));
            return 0u;
        }

        case 1218: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexArrayElementBuffer>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexArrayElementBuffer(static_cast<GLuint>(p->vaobj()), static_cast<GLuint>(p->buffer()));
            return 0u;
        }

        case 1219: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexArrayFogCoordOffsetEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexArrayFogCoordOffsetEXT(static_cast<GLuint>(p->vaobj()), static_cast<GLuint>(p->buffer()), static_cast<GLenum>(p->type()), static_cast<GLsizei>(p->stride()), static_cast<GLintptr>(p->offset()));
            return 0u;
        }

        case 1220: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexArrayIndexOffsetEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexArrayIndexOffsetEXT(static_cast<GLuint>(p->vaobj()), static_cast<GLuint>(p->buffer()), static_cast<GLenum>(p->type()), static_cast<GLsizei>(p->stride()), static_cast<GLintptr>(p->offset()));
            return 0u;
        }

        case 1221: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexArrayMultiTexCoordOffsetEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexArrayMultiTexCoordOffsetEXT(static_cast<GLuint>(p->vaobj()), static_cast<GLuint>(p->buffer()), static_cast<GLenum>(p->texunit()), static_cast<GLint>(p->size()), static_cast<GLenum>(p->type()), static_cast<GLsizei>(p->stride()), static_cast<GLintptr>(p->offset()));
            return 0u;
        }

        case 1222: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexArrayNormalOffsetEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexArrayNormalOffsetEXT(static_cast<GLuint>(p->vaobj()), static_cast<GLuint>(p->buffer()), static_cast<GLenum>(p->type()), static_cast<GLsizei>(p->stride()), static_cast<GLintptr>(p->offset()));
            return 0u;
        }

        case 1223: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexArraySecondaryColorOffsetEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexArraySecondaryColorOffsetEXT(static_cast<GLuint>(p->vaobj()), static_cast<GLuint>(p->buffer()), static_cast<GLint>(p->size()), static_cast<GLenum>(p->type()), static_cast<GLsizei>(p->stride()), static_cast<GLintptr>(p->offset()));
            return 0u;
        }

        case 1224: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexArrayTexCoordOffsetEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexArrayTexCoordOffsetEXT(static_cast<GLuint>(p->vaobj()), static_cast<GLuint>(p->buffer()), static_cast<GLint>(p->size()), static_cast<GLenum>(p->type()), static_cast<GLsizei>(p->stride()), static_cast<GLintptr>(p->offset()));
            return 0u;
        }

        case 1225: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexArrayVertexAttribBindingEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexArrayVertexAttribBindingEXT(static_cast<GLuint>(p->vaobj()), static_cast<GLuint>(p->attribindex()), static_cast<GLuint>(p->bindingindex()));
            return 0u;
        }

        case 1226: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexArrayVertexAttribDivisorEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexArrayVertexAttribDivisorEXT(static_cast<GLuint>(p->vaobj()), static_cast<GLuint>(p->index()), static_cast<GLuint>(p->divisor()));
            return 0u;
        }

        case 1227: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexArrayVertexAttribFormatEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexArrayVertexAttribFormatEXT(static_cast<GLuint>(p->vaobj()), static_cast<GLuint>(p->attribindex()), static_cast<GLint>(p->size()), static_cast<GLenum>(p->type()), static_cast<GLboolean>(p->normalized()), static_cast<GLuint>(p->relativeoffset()));
            return 0u;
        }

        case 1228: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexArrayVertexAttribIFormatEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexArrayVertexAttribIFormatEXT(static_cast<GLuint>(p->vaobj()), static_cast<GLuint>(p->attribindex()), static_cast<GLint>(p->size()), static_cast<GLenum>(p->type()), static_cast<GLuint>(p->relativeoffset()));
            return 0u;
        }

        case 1229: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexArrayVertexAttribIOffsetEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexArrayVertexAttribIOffsetEXT(static_cast<GLuint>(p->vaobj()), static_cast<GLuint>(p->buffer()), static_cast<GLuint>(p->index()), static_cast<GLint>(p->size()), static_cast<GLenum>(p->type()), static_cast<GLsizei>(p->stride()), static_cast<GLintptr>(p->offset()));
            return 0u;
        }

        case 1230: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexArrayVertexAttribLFormatEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexArrayVertexAttribLFormatEXT(static_cast<GLuint>(p->vaobj()), static_cast<GLuint>(p->attribindex()), static_cast<GLint>(p->size()), static_cast<GLenum>(p->type()), static_cast<GLuint>(p->relativeoffset()));
            return 0u;
        }

        case 1231: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexArrayVertexAttribLOffsetEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexArrayVertexAttribLOffsetEXT(static_cast<GLuint>(p->vaobj()), static_cast<GLuint>(p->buffer()), static_cast<GLuint>(p->index()), static_cast<GLint>(p->size()), static_cast<GLenum>(p->type()), static_cast<GLsizei>(p->stride()), static_cast<GLintptr>(p->offset()));
            return 0u;
        }

        case 1232: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexArrayVertexAttribOffsetEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexArrayVertexAttribOffsetEXT(static_cast<GLuint>(p->vaobj()), static_cast<GLuint>(p->buffer()), static_cast<GLuint>(p->index()), static_cast<GLint>(p->size()), static_cast<GLenum>(p->type()), static_cast<GLboolean>(p->normalized()), static_cast<GLsizei>(p->stride()), static_cast<GLintptr>(p->offset()));
            return 0u;
        }

        case 1233: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexArrayVertexBindingDivisorEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexArrayVertexBindingDivisorEXT(static_cast<GLuint>(p->vaobj()), static_cast<GLuint>(p->bindingindex()), static_cast<GLuint>(p->divisor()));
            return 0u;
        }

        case 1234: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexArrayVertexBuffer>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexArrayVertexBuffer(static_cast<GLuint>(p->vaobj()), static_cast<GLuint>(p->bindingindex()), static_cast<GLuint>(p->buffer()), static_cast<GLintptr>(p->offset()), static_cast<GLsizei>(p->stride()));
            return 0u;
        }

        case 1236: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexArrayVertexOffsetEXT>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexArrayVertexOffsetEXT(static_cast<GLuint>(p->vaobj()), static_cast<GLuint>(p->buffer()), static_cast<GLint>(p->size()), static_cast<GLenum>(p->type()), static_cast<GLsizei>(p->stride()), static_cast<GLintptr>(p->offset()));
            return 0u;
        }

        case 1237: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttrib1d>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttrib1d(static_cast<GLuint>(p->index()), static_cast<GLdouble>(p->x()));
            return 0u;
        }

        case 1238: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttrib1dv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttrib1dv(static_cast<GLuint>(p->index()), reinterpret_cast<const GLdouble*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1239: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttrib1f>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttrib1f(static_cast<GLuint>(p->index()), static_cast<GLfloat>(p->x()));
            return 0u;
        }

        case 1240: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttrib1fv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttrib1fv(static_cast<GLuint>(p->index()), reinterpret_cast<const GLfloat*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1241: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttrib1s>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttrib1s(static_cast<GLuint>(p->index()), static_cast<GLshort>(p->x()));
            return 0u;
        }

        case 1242: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttrib1sv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttrib1sv(static_cast<GLuint>(p->index()), reinterpret_cast<const GLshort*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1243: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttrib2d>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttrib2d(static_cast<GLuint>(p->index()), static_cast<GLdouble>(p->x()), static_cast<GLdouble>(p->y()));
            return 0u;
        }

        case 1244: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttrib2dv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttrib2dv(static_cast<GLuint>(p->index()), reinterpret_cast<const GLdouble*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1245: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttrib2f>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttrib2f(static_cast<GLuint>(p->index()), static_cast<GLfloat>(p->x()), static_cast<GLfloat>(p->y()));
            return 0u;
        }

        case 1246: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttrib2fv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttrib2fv(static_cast<GLuint>(p->index()), reinterpret_cast<const GLfloat*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1247: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttrib2s>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttrib2s(static_cast<GLuint>(p->index()), static_cast<GLshort>(p->x()), static_cast<GLshort>(p->y()));
            return 0u;
        }

        case 1248: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttrib2sv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttrib2sv(static_cast<GLuint>(p->index()), reinterpret_cast<const GLshort*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1249: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttrib3d>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttrib3d(static_cast<GLuint>(p->index()), static_cast<GLdouble>(p->x()), static_cast<GLdouble>(p->y()), static_cast<GLdouble>(p->z()));
            return 0u;
        }

        case 1250: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttrib3dv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttrib3dv(static_cast<GLuint>(p->index()), reinterpret_cast<const GLdouble*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1251: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttrib3f>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttrib3f(static_cast<GLuint>(p->index()), static_cast<GLfloat>(p->x()), static_cast<GLfloat>(p->y()), static_cast<GLfloat>(p->z()));
            return 0u;
        }

        case 1252: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttrib3fv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttrib3fv(static_cast<GLuint>(p->index()), reinterpret_cast<const GLfloat*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1253: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttrib3s>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttrib3s(static_cast<GLuint>(p->index()), static_cast<GLshort>(p->x()), static_cast<GLshort>(p->y()), static_cast<GLshort>(p->z()));
            return 0u;
        }

        case 1254: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttrib3sv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttrib3sv(static_cast<GLuint>(p->index()), reinterpret_cast<const GLshort*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1255: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttrib4Nbv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttrib4Nbv(static_cast<GLuint>(p->index()), reinterpret_cast<const GLbyte*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1256: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttrib4Niv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttrib4Niv(static_cast<GLuint>(p->index()), reinterpret_cast<const GLint*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1257: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttrib4Nsv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttrib4Nsv(static_cast<GLuint>(p->index()), reinterpret_cast<const GLshort*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1258: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttrib4Nub>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttrib4Nub(static_cast<GLuint>(p->index()), static_cast<GLubyte>(p->x()), static_cast<GLubyte>(p->y()), static_cast<GLubyte>(p->z()), static_cast<GLubyte>(p->w()));
            return 0u;
        }

        case 1259: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttrib4Nubv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttrib4Nubv(static_cast<GLuint>(p->index()), reinterpret_cast<const GLubyte*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1260: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttrib4Nuiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttrib4Nuiv(static_cast<GLuint>(p->index()), reinterpret_cast<const GLuint*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1261: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttrib4Nusv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttrib4Nusv(static_cast<GLuint>(p->index()), reinterpret_cast<const GLushort*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1262: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttrib4bv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttrib4bv(static_cast<GLuint>(p->index()), reinterpret_cast<const GLbyte*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1263: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttrib4d>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttrib4d(static_cast<GLuint>(p->index()), static_cast<GLdouble>(p->x()), static_cast<GLdouble>(p->y()), static_cast<GLdouble>(p->z()), static_cast<GLdouble>(p->w()));
            return 0u;
        }

        case 1264: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttrib4dv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttrib4dv(static_cast<GLuint>(p->index()), reinterpret_cast<const GLdouble*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1265: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttrib4f>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttrib4f(static_cast<GLuint>(p->index()), static_cast<GLfloat>(p->x()), static_cast<GLfloat>(p->y()), static_cast<GLfloat>(p->z()), static_cast<GLfloat>(p->w()));
            return 0u;
        }

        case 1266: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttrib4fv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttrib4fv(static_cast<GLuint>(p->index()), reinterpret_cast<const GLfloat*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1267: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttrib4iv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttrib4iv(static_cast<GLuint>(p->index()), reinterpret_cast<const GLint*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1268: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttrib4s>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttrib4s(static_cast<GLuint>(p->index()), static_cast<GLshort>(p->x()), static_cast<GLshort>(p->y()), static_cast<GLshort>(p->z()), static_cast<GLshort>(p->w()));
            return 0u;
        }

        case 1269: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttrib4sv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttrib4sv(static_cast<GLuint>(p->index()), reinterpret_cast<const GLshort*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1270: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttrib4ubv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttrib4ubv(static_cast<GLuint>(p->index()), reinterpret_cast<const GLubyte*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1271: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttrib4uiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttrib4uiv(static_cast<GLuint>(p->index()), reinterpret_cast<const GLuint*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1272: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttrib4usv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttrib4usv(static_cast<GLuint>(p->index()), reinterpret_cast<const GLushort*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1273: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribBinding>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribBinding(static_cast<GLuint>(p->attribindex()), static_cast<GLuint>(p->bindingindex()));
            return 0u;
        }

        case 1274: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribDivisor>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribDivisor(static_cast<GLuint>(p->index()), static_cast<GLuint>(p->divisor()));
            return 0u;
        }

        case 1276: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribFormat>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribFormat(static_cast<GLuint>(p->attribindex()), static_cast<GLint>(p->size()), static_cast<GLenum>(p->type()), static_cast<GLboolean>(p->normalized()), static_cast<GLuint>(p->relativeoffset()));
            return 0u;
        }

        case 1277: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribFormatNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribFormatNV(static_cast<GLuint>(p->index()), static_cast<GLint>(p->size()), static_cast<GLenum>(p->type()), static_cast<GLboolean>(p->normalized()), static_cast<GLsizei>(p->stride()));
            return 0u;
        }

        case 1278: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribI1i>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribI1i(static_cast<GLuint>(p->index()), static_cast<GLint>(p->x()));
            return 0u;
        }

        case 1279: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribI1iv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribI1iv(static_cast<GLuint>(p->index()), reinterpret_cast<const GLint*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1280: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribI1ui>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribI1ui(static_cast<GLuint>(p->index()), static_cast<GLuint>(p->x()));
            return 0u;
        }

        case 1281: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribI1uiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribI1uiv(static_cast<GLuint>(p->index()), reinterpret_cast<const GLuint*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1282: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribI2i>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribI2i(static_cast<GLuint>(p->index()), static_cast<GLint>(p->x()), static_cast<GLint>(p->y()));
            return 0u;
        }

        case 1283: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribI2iv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribI2iv(static_cast<GLuint>(p->index()), reinterpret_cast<const GLint*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1284: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribI2ui>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribI2ui(static_cast<GLuint>(p->index()), static_cast<GLuint>(p->x()), static_cast<GLuint>(p->y()));
            return 0u;
        }

        case 1285: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribI2uiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribI2uiv(static_cast<GLuint>(p->index()), reinterpret_cast<const GLuint*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1286: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribI3i>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribI3i(static_cast<GLuint>(p->index()), static_cast<GLint>(p->x()), static_cast<GLint>(p->y()), static_cast<GLint>(p->z()));
            return 0u;
        }

        case 1287: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribI3iv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribI3iv(static_cast<GLuint>(p->index()), reinterpret_cast<const GLint*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1288: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribI3ui>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribI3ui(static_cast<GLuint>(p->index()), static_cast<GLuint>(p->x()), static_cast<GLuint>(p->y()), static_cast<GLuint>(p->z()));
            return 0u;
        }

        case 1289: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribI3uiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribI3uiv(static_cast<GLuint>(p->index()), reinterpret_cast<const GLuint*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1290: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribI4bv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribI4bv(static_cast<GLuint>(p->index()), reinterpret_cast<const GLbyte*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1291: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribI4i>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribI4i(static_cast<GLuint>(p->index()), static_cast<GLint>(p->x()), static_cast<GLint>(p->y()), static_cast<GLint>(p->z()), static_cast<GLint>(p->w()));
            return 0u;
        }

        case 1292: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribI4iv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribI4iv(static_cast<GLuint>(p->index()), reinterpret_cast<const GLint*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1293: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribI4sv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribI4sv(static_cast<GLuint>(p->index()), reinterpret_cast<const GLshort*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1294: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribI4ubv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribI4ubv(static_cast<GLuint>(p->index()), reinterpret_cast<const GLubyte*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1295: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribI4ui>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribI4ui(static_cast<GLuint>(p->index()), static_cast<GLuint>(p->x()), static_cast<GLuint>(p->y()), static_cast<GLuint>(p->z()), static_cast<GLuint>(p->w()));
            return 0u;
        }

        case 1296: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribI4uiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribI4uiv(static_cast<GLuint>(p->index()), reinterpret_cast<const GLuint*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1297: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribI4usv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribI4usv(static_cast<GLuint>(p->index()), reinterpret_cast<const GLushort*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1298: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribIFormat>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribIFormat(static_cast<GLuint>(p->attribindex()), static_cast<GLint>(p->size()), static_cast<GLenum>(p->type()), static_cast<GLuint>(p->relativeoffset()));
            return 0u;
        }

        case 1299: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribIFormatNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribIFormatNV(static_cast<GLuint>(p->index()), static_cast<GLint>(p->size()), static_cast<GLenum>(p->type()), static_cast<GLsizei>(p->stride()));
            return 0u;
        }

        case 1300: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribIPointer>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribIPointer(static_cast<GLuint>(p->index()), static_cast<GLint>(p->size()), static_cast<GLenum>(p->type()), static_cast<GLsizei>(p->stride()), ResolveShm(p->pointer(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 1301: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribL1d>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribL1d(static_cast<GLuint>(p->index()), static_cast<GLdouble>(p->x()));
            return 0u;
        }

        case 1302: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribL1dv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribL1dv(static_cast<GLuint>(p->index()), reinterpret_cast<const GLdouble*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1303: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribL1i64NV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribL1i64NV(static_cast<GLuint>(p->index()), p->x());
            return 0u;
        }

        case 1304: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribL1i64vNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribL1i64vNV(static_cast<GLuint>(p->index()), reinterpret_cast<const GLint64EXT*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1305: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribL1ui64ARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribL1ui64ARB(static_cast<GLuint>(p->index()), p->x());
            return 0u;
        }

        case 1306: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribL1ui64NV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribL1ui64NV(static_cast<GLuint>(p->index()), p->x());
            return 0u;
        }

        case 1307: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribL1ui64vARB>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribL1ui64vARB(static_cast<GLuint>(p->index()), reinterpret_cast<const GLuint64EXT*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1308: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribL1ui64vNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribL1ui64vNV(static_cast<GLuint>(p->index()), reinterpret_cast<const GLuint64EXT*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1309: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribL2d>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribL2d(static_cast<GLuint>(p->index()), static_cast<GLdouble>(p->x()), static_cast<GLdouble>(p->y()));
            return 0u;
        }

        case 1310: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribL2dv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribL2dv(static_cast<GLuint>(p->index()), reinterpret_cast<const GLdouble*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1311: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribL2i64NV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribL2i64NV(static_cast<GLuint>(p->index()), p->x(), p->y());
            return 0u;
        }

        case 1312: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribL2i64vNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribL2i64vNV(static_cast<GLuint>(p->index()), reinterpret_cast<const GLint64EXT*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1313: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribL2ui64NV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribL2ui64NV(static_cast<GLuint>(p->index()), p->x(), p->y());
            return 0u;
        }

        case 1314: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribL2ui64vNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribL2ui64vNV(static_cast<GLuint>(p->index()), reinterpret_cast<const GLuint64EXT*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1315: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribL3d>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribL3d(static_cast<GLuint>(p->index()), static_cast<GLdouble>(p->x()), static_cast<GLdouble>(p->y()), static_cast<GLdouble>(p->z()));
            return 0u;
        }

        case 1316: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribL3dv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribL3dv(static_cast<GLuint>(p->index()), reinterpret_cast<const GLdouble*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1317: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribL3i64NV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribL3i64NV(static_cast<GLuint>(p->index()), p->x(), p->y(), p->z());
            return 0u;
        }

        case 1318: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribL3i64vNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribL3i64vNV(static_cast<GLuint>(p->index()), reinterpret_cast<const GLint64EXT*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1319: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribL3ui64NV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribL3ui64NV(static_cast<GLuint>(p->index()), p->x(), p->y(), p->z());
            return 0u;
        }

        case 1320: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribL3ui64vNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribL3ui64vNV(static_cast<GLuint>(p->index()), reinterpret_cast<const GLuint64EXT*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1321: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribL4d>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribL4d(static_cast<GLuint>(p->index()), static_cast<GLdouble>(p->x()), static_cast<GLdouble>(p->y()), static_cast<GLdouble>(p->z()), static_cast<GLdouble>(p->w()));
            return 0u;
        }

        case 1322: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribL4dv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribL4dv(static_cast<GLuint>(p->index()), reinterpret_cast<const GLdouble*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1323: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribL4i64NV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribL4i64NV(static_cast<GLuint>(p->index()), p->x(), p->y(), p->z(), p->w());
            return 0u;
        }

        case 1324: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribL4i64vNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribL4i64vNV(static_cast<GLuint>(p->index()), reinterpret_cast<const GLint64EXT*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1325: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribL4ui64NV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribL4ui64NV(static_cast<GLuint>(p->index()), p->x(), p->y(), p->z(), p->w());
            return 0u;
        }

        case 1326: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribL4ui64vNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribL4ui64vNV(static_cast<GLuint>(p->index()), reinterpret_cast<const GLuint64EXT*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1327: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribLFormat>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribLFormat(static_cast<GLuint>(p->attribindex()), static_cast<GLint>(p->size()), static_cast<GLenum>(p->type()), static_cast<GLuint>(p->relativeoffset()));
            return 0u;
        }

        case 1328: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribLFormatNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribLFormatNV(static_cast<GLuint>(p->index()), static_cast<GLint>(p->size()), static_cast<GLenum>(p->type()), static_cast<GLsizei>(p->stride()));
            return 0u;
        }

        case 1329: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribLPointer>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribLPointer(static_cast<GLuint>(p->index()), static_cast<GLint>(p->size()), static_cast<GLenum>(p->type()), static_cast<GLsizei>(p->stride()), ResolveShm(p->pointer(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 1330: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribP1ui>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribP1ui(static_cast<GLuint>(p->index()), static_cast<GLenum>(p->type()), static_cast<GLboolean>(p->normalized()), static_cast<GLuint>(p->value()));
            return 0u;
        }

        case 1331: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribP1uiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribP1uiv(static_cast<GLuint>(p->index()), static_cast<GLenum>(p->type()), static_cast<GLboolean>(p->normalized()), reinterpret_cast<const GLuint*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1332: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribP2ui>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribP2ui(static_cast<GLuint>(p->index()), static_cast<GLenum>(p->type()), static_cast<GLboolean>(p->normalized()), static_cast<GLuint>(p->value()));
            return 0u;
        }

        case 1333: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribP2uiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribP2uiv(static_cast<GLuint>(p->index()), static_cast<GLenum>(p->type()), static_cast<GLboolean>(p->normalized()), reinterpret_cast<const GLuint*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1334: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribP3ui>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribP3ui(static_cast<GLuint>(p->index()), static_cast<GLenum>(p->type()), static_cast<GLboolean>(p->normalized()), static_cast<GLuint>(p->value()));
            return 0u;
        }

        case 1335: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribP3uiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribP3uiv(static_cast<GLuint>(p->index()), static_cast<GLenum>(p->type()), static_cast<GLboolean>(p->normalized()), reinterpret_cast<const GLuint*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1336: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribP4ui>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribP4ui(static_cast<GLuint>(p->index()), static_cast<GLenum>(p->type()), static_cast<GLboolean>(p->normalized()), static_cast<GLuint>(p->value()));
            return 0u;
        }

        case 1337: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribP4uiv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribP4uiv(static_cast<GLuint>(p->index()), static_cast<GLenum>(p->type()), static_cast<GLboolean>(p->normalized()), reinterpret_cast<const GLuint*>((p->value() == nullptr ? nullptr : p->value()->data())));
            return 0u;
        }

        case 1338: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexAttribPointer>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexAttribPointer(static_cast<GLuint>(p->index()), static_cast<GLint>(p->size()), static_cast<GLenum>(p->type()), static_cast<GLboolean>(p->normalized()), static_cast<GLsizei>(p->stride()), ResolveShm(p->pointer(), receivedShm, receivedShmCount));
            return 0u;
        }

        case 1339: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexBindingDivisor>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexBindingDivisor(static_cast<GLuint>(p->bindingindex()), static_cast<GLuint>(p->divisor()));
            return 0u;
        }

        case 1340: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenVertexFormatNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glVertexFormatNV(static_cast<GLint>(p->size()), static_cast<GLenum>(p->type()), static_cast<GLsizei>(p->stride()));
            return 0u;
        }

        case 1341: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenViewport>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glViewport(static_cast<GLint>(p->x()), static_cast<GLint>(p->y()), static_cast<GLsizei>(p->width()), static_cast<GLsizei>(p->height()));
            return 0u;
        }

        case 1342: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenViewportArrayv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glViewportArrayv(static_cast<GLuint>(p->first()), static_cast<GLsizei>(p->count()), reinterpret_cast<const GLfloat*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1343: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenViewportIndexedf>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glViewportIndexedf(static_cast<GLuint>(p->index()), static_cast<GLfloat>(p->x()), static_cast<GLfloat>(p->y()), static_cast<GLfloat>(p->w()), static_cast<GLfloat>(p->h()));
            return 0u;
        }

        case 1344: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenViewportIndexedfv>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glViewportIndexedfv(static_cast<GLuint>(p->index()), reinterpret_cast<const GLfloat*>((p->v() == nullptr ? nullptr : p->v()->data())));
            return 0u;
        }

        case 1345: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenViewportPositionWScaleNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glViewportPositionWScaleNV(static_cast<GLuint>(p->index()), static_cast<GLfloat>(p->xcoeff()), static_cast<GLfloat>(p->ycoeff()));
            return 0u;
        }

        case 1346: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenViewportSwizzleNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glViewportSwizzleNV(static_cast<GLuint>(p->index()), static_cast<GLenum>(p->swizzlex()), static_cast<GLenum>(p->swizzley()), static_cast<GLenum>(p->swizzlez()), static_cast<GLenum>(p->swizzlew()));
            return 0u;
        }

        case 1347: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenWaitSync>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glWaitSync(reinterpret_cast<GLsync>(static_cast<std::uintptr_t>(p->sync())), static_cast<GLbitfield>(p->flags()), static_cast<GLuint64>(p->timeout()));
            return 0u;
        }

        case 1348: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenWaitVkSemaphoreNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glWaitVkSemaphoreNV(static_cast<GLuint64>(p->vkSemaphore()));
            return 0u;
        }

        case 1349: {
            const auto* p = ::flatbuffers::GetRoot<MobileGL::Protocol::WireFull::GenWeightPathsNV>(payloadBytes);
            if (p == nullptr) { return 1u; }
            ::glWeightPathsNV(static_cast<GLuint>(p->resultPath()), static_cast<GLsizei>(p->numPaths()), reinterpret_cast<const GLuint*>((p->paths() == nullptr ? nullptr : p->paths()->data())), reinterpret_cast<const GLfloat*>((p->weights() == nullptr ? nullptr : p->weights()->data())));
            return 0u;
        }

        case 21:
            return 1u;

        case 22:
            return 1u;

        case 28:
            return 1u;

        case 32:
            return 1u;

        case 37:
            return 1u;

        case 42:
            return 1u;

        case 43:
            return 1u;

        case 49:
            return 1u;

        case 63:
            return 1u;

        case 67:
            return 1u;

        case 73:
            return 1u;

        case 83:
            return 1u;

        case 84:
            return 1u;

        case 85:
            return 1u;

        case 111:
            return 1u;

        case 146:
            return 1u;

        case 202:
            return 1u;

        case 209:
            return 1u;

        case 211:
            return 1u;

        case 216:
            return 1u;

        case 217:
            return 1u;

        case 219:
            return 1u;

        case 221:
            return 1u;

        case 225:
            return 1u;

        case 234:
            return 1u;

        case 268:
            return 1u;

        case 270:
            return 1u;

        case 274:
            return 1u;

        case 276:
            return 1u;

        case 281:
            return 1u;

        case 285:
            return 1u;

        case 286:
            return 1u;

        case 287:
            return 1u;

        case 297:
            return 1u;

        case 298:
            return 1u;

        case 309:
            return 1u;

        case 310:
            return 1u;

        case 315:
            return 1u;

        case 316:
            return 1u;

        case 318:
            return 1u;

        case 319:
            return 1u;

        case 327:
            return 1u;

        case 332:
            return 1u;

        case 335:
            return 1u;

        case 340:
            return 1u;

        case 343:
            return 1u;

        case 346:
            return 1u;

        case 351:
            return 1u;

        case 357:
            return 1u;

        case 363:
            return 1u;

        case 368:
            return 1u;

        case 369:
            return 1u;

        case 370:
            return 1u;

        case 372:
            return 1u;

        case 373:
            return 1u;

        case 375:
            return 1u;

        case 385:
            return 1u;

        case 394:
            return 1u;

        case 395:
            return 1u;

        case 400:
            return 1u;

        case 408:
            return 1u;

        case 410:
            return 1u;

        case 412:
            return 1u;

        case 414:
            return 1u;

        case 415:
            return 1u;

        case 446:
            return 1u;

        case 447:
            return 1u;

        case 462:
            return 1u;

        case 465:
            return 1u;

        case 466:
            return 1u;

        case 467:
            return 1u;

        case 477:
            return 1u;

        case 480:
            return 1u;

        case 482:
            return 1u;

        case 485:
            return 1u;

        case 486:
            return 1u;

        case 487:
            return 1u;

        case 488:
            return 1u;

        case 489:
            return 1u;

        case 491:
            return 1u;

        case 493:
            return 1u;

        case 498:
            return 1u;

        case 514:
            return 1u;

        case 519:
            return 1u;

        case 521:
            return 1u;

        case 557:
            return 1u;

        case 562:
            return 1u;

        case 577:
            return 1u;

        case 578:
            return 1u;

        case 586:
            return 1u;

        case 598:
            return 1u;

        case 601:
            return 1u;

        case 604:
            return 1u;

        case 624:
            return 1u;

        case 636:
            return 1u;

        case 645:
            return 1u;

        case 650:
            return 1u;

        case 666:
            return 1u;

        case 667:
            return 1u;

        case 668:
            return 1u;

        case 669:
            return 1u;

        case 700:
            return 1u;

        case 707:
            return 1u;

        case 708:
            return 1u;

        case 714:
            return 1u;

        case 716:
            return 1u;

        case 771:
            return 1u;

        case 814:
            return 1u;

        case 827:
            return 1u;

        case 828:
            return 1u;

        case 833:
            return 1u;

        case 840:
            return 1u;

        case 842:
            return 1u;

        case 848:
            return 1u;

        case 850:
            return 1u;

        case 856:
            return 1u;

        case 858:
            return 1u;

        case 864:
            return 1u;

        case 866:
            return 1u;

        case 872:
            return 1u;

        case 874:
            return 1u;

        case 880:
            return 1u;

        case 882:
            return 1u;

        case 888:
            return 1u;

        case 890:
            return 1u;

        case 896:
            return 1u;

        case 898:
            return 1u;

        case 904:
            return 1u;

        case 906:
            return 1u;

        case 912:
            return 1u;

        case 914:
            return 1u;

        case 920:
            return 1u;

        case 922:
            return 1u;

        case 928:
            return 1u;

        case 930:
            return 1u;

        case 938:
            return 1u;

        case 942:
            return 1u;

        case 946:
            return 1u;

        case 950:
            return 1u;

        case 954:
            return 1u;

        case 958:
            return 1u;

        case 962:
            return 1u;

        case 966:
            return 1u;

        case 970:
            return 1u;

        case 982:
            return 1u;

        case 983:
            return 1u;

        case 985:
            return 1u;

        case 990:
            return 1u;

        case 991:
            return 1u;

        case 1009:
            return 1u;

        case 1011:
            return 1u;

        case 1012:
            return 1u;

        case 1039:
            return 1u;

        case 1055:
            return 1u;

        case 1058:
            return 1u;

        case 1061:
            return 1u;

        case 1067:
            return 1u;

        case 1068:
            return 1u;

        case 1110:
            return 1u;

        case 1235:
            return 1u;

        case 1275:
            return 1u;

        case 1350:
            return 1u;

        case 1351:
            return 1u;

        case 1352:
            return 1u;

        case 1353:
            return 1u;

        case 1354:
            return 1u;

        case 1355:
            return 1u;

        case 1356:
            return 1u;

        case 1357:
            return 1u;

        case 1358:
            return 1u;

        case 1359:
            return 1u;

        case 1360:
            return 1u;

        case 1361:
            return 1u;

        case 1362:
            return 1u;

        case 1363:
            return 1u;

        case 1364:
            return 1u;

        case 1365:
            return 1u;

        case 1366:
            return 1u;

        case 1367:
            return 1u;

        case 1368:
            return 1u;

        case 1369:
            return 1u;

        case 1370:
            return 1u;

        case 1371:
            return 1u;

        case 1372:
            return 1u;

        case 1373:
            return 1u;

        case 1374:
            return 1u;

        case 1375:
            return 1u;

        case 1376:
            return 1u;

        case 1377:
            return 1u;

        case 1378:
            return 1u;

        case 1379:
            return 1u;

        case 1380:
            return 1u;

        case 1381:
            return 1u;

        case 1382:
            return 1u;

        case 1383:
            return 1u;

        case 1384:
            return 1u;

        case 1385:
            return 1u;

        case 1386:
            return 1u;

        case 1387:
            return 1u;

        case 1388:
            return 1u;

        case 1389:
            return 1u;

        case 1390:
            return 1u;

        case 1391:
            return 1u;

        case 1392:
            return 1u;

        case 1393:
            return 1u;

        case 1394:
            return 1u;

        case 1395:
            return 1u;

        case 1396:
            return 1u;

        default:
            return 1u;
        }
    }
} // namespace MobileGL::Protocol::Wire

// End of File
