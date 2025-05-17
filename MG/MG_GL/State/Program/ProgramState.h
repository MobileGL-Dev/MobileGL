//
// Created by BZLZHH on 2025/5/1.
//

#ifndef MOBILEGL_PROGRAMSTATE_H
#define MOBILEGL_PROGRAMSTATE_H
#define MOBILEGL_GLSTATE_H
#define MOBILEGL_PROGRAM_DEBUGTOOL_H
#define MOBILEGL_GLSLTOOL_H

#include "../../../Includes.h"

struct ShaderObject {
    GLenum type;
    std::string source;
    std::vector<unsigned int> compiledSpirv;
    UncertainBool compiled;
    GLint compileStatus;
    std::string infoLog;
    bool markedForDeletion;
};

struct UniformValue {
    GLenum type;
    std::vector<GLfloat> floatData;
    std::vector<GLint> intData;
    std::vector<GLuint> uintData;
    std::vector<GLboolean> boolData;
    GLsizei count;
    GLsizei numElements;

    UniformValue() : type(0), count(0) {}

    template<typename T>
    void setData(GLenum uniformType, GLsizei count_, const T* values);
};

struct ProgramObject {
    std::vector<GLuint> attachedShaders;
    UncertainBool linked;
    bool dirty;
    GLint linkStatus;
    std::string infoLog;
    ankerl::unordered_map<std::string, GLint> attribBindings;
    ankerl::unordered_map<std::string, GLint> attribLocations;
    ankerl::unordered_map<std::string, GLint> uniformLocations;
    ankerl::unordered_map<std::string, UniformValue> uniformValues;
    bool markedForDeletion;
};

class ProgramState {
public:
    ankerl::unordered_map<GLuint, ShaderObject> shaders_;
    ankerl::unordered_map<GLuint, ProgramObject> programs_;
    std::set<GLuint> freeShaderIds_;
    std::set<GLuint> freeProgramIds_;
    GLuint lastShaderId_ = 0;
    GLuint lastProgramId_ = 0;
    GLuint currentProgram_ = 0;

    ProgramState();
    ~ProgramState();
    
    // Return: the validity of the operation, according to OpenGL 3 standard
    GLenum CreateShader(GLenum type, GLuint* shader);
    GLenum CreateProgram(GLuint* program);
    GLenum DeleteShader(GLuint shader);
    GLenum DeleteProgram(GLuint program);
    GLenum LinkShaderToProgram(GLuint program, GLuint shader);
    GLenum UploadShaderSource(GLuint shader, GLsizei count, const GLchar** string, const GLint* length);
    GLenum BuildShaderStage(GLuint shader);
    GLenum FinalizeProgramPipeline(GLuint program);
    GLenum ActivateRenderProgram(GLuint program);
    GLenum DefineProgramAttributeBinding(GLuint program, GLuint index, const GLchar* name);
    GLint QueryProgramAttributeBinding(GLuint program, const GLchar* name);
    GLint QueryProgramUniformLocation(GLuint program, const GLchar* name);
    GLenum QueryProgramStateIntVector(GLuint program, GLenum pname, GLint* params);
    GLenum QueryShaderStateIntVector(GLuint shader, GLenum pname, GLint* params);

    template<typename T>
    GLenum SetUniform(GLuint program, GLint location, GLsizei count, const T* value, GLenum type);

    template<typename T>
    GLenum SetUniformMatrix(GLuint program, GLint location, GLsizei count, GLboolean transpose, const T* value, GLenum matrixType);
    
#define DECLARE_UNIFORM_FUNCTIONS(type, suffix, vecType) \
    void UpdateUniform##suffix##1(GLint location, type v0); \
    void UpdateUniform##suffix##2(GLint location, type v0, type v1); \
    void UpdateUniform##suffix##3(GLint location, type v0, type v1, type v2); \
    void UpdateUniform##suffix##4(GLint location, type v0, type v1, type v2, type v3); \
    void UpdateUniform##suffix##Vector1(GLint location, GLsizei count, const type* value); \
    void UpdateUniform##suffix##Vector2(GLint location, GLsizei count, const type* value); \
    void UpdateUniform##suffix##Vector3(GLint location, GLsizei count, const type* value); \
    void UpdateUniform##suffix##Vector4(GLint location, GLsizei count, const type* value);
    DECLARE_UNIFORM_FUNCTIONS(GLfloat, Float, GL_FLOAT)
    DECLARE_UNIFORM_FUNCTIONS(GLint, Int, GL_INT)
    DECLARE_UNIFORM_FUNCTIONS(GLuint, UInt, GL_UNSIGNED_INT)
    DECLARE_UNIFORM_FUNCTIONS(GLboolean, Bool, GL_BOOL)
#undef DECLARE_UNIFORM_FUNCTIONS
#define DECLARE_MATRIX_FUNCTIONS(suffix, matrixType) \
    void UpdateUniformMatrix##suffix##Vector(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value);
    DECLARE_MATRIX_FUNCTIONS(2x2, GL_FLOAT_MAT2)
    DECLARE_MATRIX_FUNCTIONS(3x3, GL_FLOAT_MAT3)
    DECLARE_MATRIX_FUNCTIONS(4x4, GL_FLOAT_MAT4)
    DECLARE_MATRIX_FUNCTIONS(2x3, GL_FLOAT_MAT2x3)
    DECLARE_MATRIX_FUNCTIONS(2x4, GL_FLOAT_MAT2x4)
    DECLARE_MATRIX_FUNCTIONS(3x2, GL_FLOAT_MAT3x2)
    DECLARE_MATRIX_FUNCTIONS(3x4, GL_FLOAT_MAT3x4)
    DECLARE_MATRIX_FUNCTIONS(4x2, GL_FLOAT_MAT4x2)
    DECLARE_MATRIX_FUNCTIONS(4x3, GL_FLOAT_MAT4x3)
#undef DECLARE_MATRIX_FUNCTIONS
    
    ProgramObject GetProgramObject(GLuint program) const;
    ShaderObject GetShaderObject(GLuint program) const;
    GLuint GetCurrentProgram() const;
    bool SetProgramStatus(GLuint program, GLboolean status);
    bool SetShaderStatus(GLuint shader, GLboolean status);

private:
    bool ValidateShader_(GLuint shader);
    bool ValidateProgram_(GLuint program);
    void CleanupShaders_();
    GLint GetUniformLocation_(GLuint program, GLint location, const GLchar* name);
};

#endif // MOBILEGL_PROGRAMSTATE_H
