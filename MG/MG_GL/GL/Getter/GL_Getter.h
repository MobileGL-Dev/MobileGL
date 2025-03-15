//
// Created by BZLZHH on 2025/3/15.
//

#ifndef MOBILEGL_GL_GETTER_H
#define MOBILEGL_GL_GETTER_H

#include "../../../Includes.h"

namespace MG_GL::GL {
    const ::GLubyte* GetString(GLenum name);
    const ::GLubyte* GetStringi(GLenum name, GLuint index);
    ::GLenum GetError();
    void GetIntegerv(GLenum pname, GLint *params);
}

#endif //MOBILEGL_GL_GETTER_H
