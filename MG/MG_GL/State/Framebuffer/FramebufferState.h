//
// Created by BZLZHH on 2025/5/3.
//

#ifndef MOBILEGL_FRAMEBUFFERSTATE_H
#define MOBILEGL_FRAMEBUFFERSTATE_H
#define MOBILEGL_GLSTATE_H

#include "../../../Includes.h"

struct FramebufferAttachment {
    GLenum type = GL_NONE;
    GLuint handle = 0;
    GLint mipLevel = 0;
    GLint zoffset = 0;
};

struct FramebufferObject {
    bool generated = false;
    ankerl::unordered_map<GLenum, FramebufferAttachment> attachments;
    GLenum status = GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT;
    GLsizei width = 0;
    GLsizei height = 0;
};

class FramebufferState {
public:
    // Return: the validity of the operation
    GLenum Create(GLuint* framebuffer);
    GLenum CreateN(GLsizei n, GLuint* framebuffers);
    GLenum Delete(GLuint framebuffer);
    GLenum Bind(GLenum target, GLuint framebuffer);
    GLenum AttachTexture2D(GLenum target, GLenum attachment,
                           GLenum textarget, GLuint texture, GLint level);
    GLenum ValidateCompleteness(GLenum target);
    FramebufferObject* GetCurrentFBO(GLenum target);
    
    ankerl::unordered_map<GLenum, GLuint> currentBindings_; // READ/DRAW
    
private:
    ankerl::unordered_map<GLuint, FramebufferObject> framebuffers_;
    std::set<GLuint> freeIds_;
    GLuint lastId_ = 0;
    bool ValidateHandle(GLuint framebuffer);
    static void UpdateCompleteness(FramebufferObject& fbo);
    static bool ValidateAttachmentCombination(const FramebufferObject& fbo);
};

#endif //MOBILEGL_FRAMEBUFFERSTATE_H
