//
// Created by BZLZHH on 2025/5/1.
//

#ifndef MOBILEGL_VERTEXARRAYSTATE_H
#define MOBILEGL_VERTEXARRAYSTATE_H
#define MOBILEGL_GLSTATE_H

#include "../../../Includes.h"

struct VertexAttribState {
    bool enabled;
    GLint size = 4;
    GLenum type = GL_FLOAT;
    GLboolean normalized = GL_FALSE;
    GLsizei stride = 0;
    const GLvoid* pointer = nullptr;
    GLuint buffer = 0;
    bool isInteger = false;
};

struct VertexArrayObject {
    bool generated = false;
    bool attribDirty = false;
    bool eboDirty = false;
    GLuint elementBuffer = 0;
    ankerl::unordered_map<GLuint, VertexAttribState> attribs;
};

class VertexArrayState {
public:
    VertexArrayState();
    
    // Return: the validity of the operation, according to OpenGL 3 standard
    GLenum GenName(GLuint* array);
    GLenum GenNameN(GLsizei n, GLuint* arrays);
    GLenum Create(GLuint* array);
    GLenum CreateN(GLsizei n, GLuint* arrays);
    GLenum Bind(GLuint array);
    GLenum EnableAttrib(GLuint index);
    GLenum DisableAttrib(GLuint index);
    GLenum SetAttribPointer(GLuint index, GLint size, GLenum type,
                            GLboolean normalized, GLsizei stride,
                            const void* pointer, bool isInteger,
                            GLuint currentArrayBuffer);
    
    GLuint GetBoundElementBuffer();
    VertexArrayObject* GetCurrentVAO();
    bool ValidateGeneratedName(GLuint array);
    bool ValidateAllocatedHandle(GLuint array);

    GLuint currentVao_ = 0;
    ankerl::unordered_map<GLuint, VertexArrayObject> vaos_;
private:
    std::set<GLuint> freeIds_;
    GLuint lastId_ = 0;
};

#endif //MOBILEGL_VERTEXARRAYSTATE_H
