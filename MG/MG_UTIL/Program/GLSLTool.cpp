//
// Created by BZLZHH on 2025/5/3.
//

#include "GLSLTool.h"

namespace MG_Util::Program {
    std::string CompileGLSLToTShader(GLenum shaderType, const std::string& source, glslang::TShader *&shader) {
        std::string infoLog;
        using namespace glslang;
        
        int glslVersion = QueryGLSLVersion(source);
        EShLanguage language = GetEShLanguageByShaderType(shaderType);
        if (language == EShLanguage::EShLangCount) {
            infoLog += "Error: [Preprocess] Unsupported shader type: " +
                      std::to_string(shaderType);
            TShader tmpShader(EShLanguage::EShLangVertex);
            return infoLog;
        }

        shader = new TShader(language);
        const char *src = source.c_str();
        shader->setStrings(&src, 1);
        shader->setEnvInput(EShSourceGlsl, language, EShClientVulkan, glslVersion);
        shader->setEnvClient(EShClientOpenGL, EShTargetOpenGL_450);
        shader->setEnvTarget(EShTargetSpv, EShTargetSpv_1_6);
        shader->setAutoMapLocations(true);
        shader->setAutoMapBindings(true);

        // Is InitResources() really correct?
        TBuiltInResource resources = InitResources();

        if (!shader->parse(&resources, glslVersion, true, EShMsgDefault)) {
            infoLog += "Error: [glslang] Cannot compile the " + GetShaderTypeName(shaderType) + ":\n"
                      + std::to_string(shader->getInfoLog());
            return infoLog;
        }
        
        return {};
    }

    std::vector<std::vector<unsigned>> CompileMultipleShadersToSPIRV(const ProgramState& state, ProgramObject& prog, std::string& infoLog) {
        using namespace glslang;

        std::vector<EShLanguage> usedShaderTypes;
        TProgram program;
        for (GLuint shaderId : prog.attachedShaders) {
            auto shaderObject = state.GetShaderObject(shaderId);
            EShLanguage shLanguage = GetEShLanguageByShaderType(shaderObject.type);
            TShader* shader = nullptr;
            std::string infoLogOfShader = CompileGLSLToTShader(shaderObject.type, shaderObject.source, shader);
            if (!infoLogOfShader.empty()) {
                infoLog = "Error: [glslang] Cannot compile " + GetShaderTypeName(shaderObject.type) +
                        " :\n" + infoLogOfShader;
                return {};
            }
            program.addShader(shader);
            usedShaderTypes.push_back(shLanguage);
        }
        if (!program.link(EShMsgDefault)) {
            infoLog = "Error: [glslang] Cannot link the program:\n" + std::to_string(program.getInfoLog());
            return {};
        }

        SpvOptions spvOptions;
        spvOptions.disableOptimizer = false;

        std::vector<std::vector<unsigned>> allSpirv;
        for(auto type : usedShaderTypes) {
            std::vector<unsigned> spirv;
            GlslangToSpv(*program.getIntermediate(type), spirv, &spvOptions);
            allSpirv.push_back(spirv);
        }

        return allSpirv;
    }
    
    std::vector<unsigned> CompileGLSLToSPIRV(GLenum shaderType, const std::string &source, std::string &infoLog) {
        using namespace glslang;

        TShader* shader = nullptr;
        std::string infoLogOfShader = CompileGLSLToTShader(shaderType, source, shader);
        if (!infoLogOfShader.empty()) {
            infoLog = "Error: [glslang] Cannot compile " + GetShaderTypeName(shaderType) + ":\n" + infoLogOfShader;
            return {};
        }
        TProgram program;
        program.addShader(shader);
        if (!program.link(EShMsgDefault)) {
            infoLog = "Error: [glslang] Cannot link the program of the single shader:\n" + std::to_string(program.getInfoLog());
            return {};
        }
        std::vector<unsigned> spirv;
        
        SpvOptions spvOptions;
        spvOptions.disableOptimizer = false;
        EShLanguage language = GetEShLanguageByShaderType(shaderType);
        GlslangToSpv(*program.getIntermediate(language), spirv, &spvOptions);

        return spirv;
    }

    void ReflectSPIRVUniforms(const std::vector<std::vector<unsigned>>& allSpirv, ProgramObject& prog, std::string& infoLog) {
        spvc_context context = nullptr;

        if (spvc_context_create(&context) != SPVC_SUCCESS) {
            infoLog = "Failed to create SPIRV-Cross context";
            return;
        }

        ankerl::unordered_set<GLint> used_locations;
        GLint auto_location = 0;
        for (auto spirv: allSpirv) {
            spvc_parsed_ir ir = nullptr;
            spvc_compiler compiler = nullptr;
            spvc_resources resources = nullptr;
            
            spvc_result result = spvc_context_parse_spirv(context, spirv.data(), spirv.size(), &ir);
            if (result != SPVC_SUCCESS) {
                infoLog = "SPIR-V parsing failed";
                spvc_context_destroy(context);
                return;
            }

            result = spvc_context_create_compiler(context, SPVC_BACKEND_GLSL, ir,
                                                  SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &compiler);
            if (result != SPVC_SUCCESS) {
                infoLog = "Failed to create SPIRV-Cross compiler";
                spvc_context_destroy(context);
                return;
            }

            result = spvc_compiler_create_shader_resources(compiler, &resources);
            if (result != SPVC_SUCCESS) {
                infoLog = "Failed to get shader resources";
                spvc_context_destroy(context);
                return;
            }

            const spvc_reflected_resource *uniform_buffers = nullptr;
            const spvc_reflected_resource *uniform_vars = nullptr;
            const spvc_reflected_resource *sampled_images = nullptr;
            size_t uniform_buffer_count = 0;
            size_t uniform_var_count = 0;
            size_t sampled_image_count = 0;

            spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_UNIFORM_BUFFER,
                                                      &uniform_buffers, &uniform_buffer_count);
            spvc_resources_get_resource_list_for_type(resources,
                                                      SPVC_RESOURCE_TYPE_GL_PLAIN_UNIFORM,
                                                      &uniform_vars, &uniform_var_count);
            spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_SAMPLED_IMAGE,
                                                      &sampled_images, &sampled_image_count);

            std::vector<const spvc_reflected_resource *> all_resources;
            for (size_t i = 0; i < uniform_buffer_count; ++i)
                all_resources.push_back(&uniform_buffers[i]);
            for (size_t i = 0; i < uniform_var_count; ++i)
                all_resources.push_back(&uniform_vars[i]);
            for (size_t i = 0; i < sampled_image_count; ++i)
                all_resources.push_back(&sampled_images[i]);

            for (const auto res_ptr: all_resources) {
                const spvc_reflected_resource &res = *res_ptr;
                const char *name = res.name;
                if (name[0] == '_') continue;

                unsigned spirv_location = spvc_compiler_get_decoration(compiler, res.id,
                                                                       SpvDecorationLocation);
                GLint final_location;

                if (spirv_location > 0) {
                    if (used_locations.find(spirv_location) != used_locations.end()) {
                        infoLog += "\nLocation conflict for uniform: " + std::string(name);
                        continue;
                    }
                    final_location = spirv_location;
                } else {
                    while (used_locations.find(auto_location) != used_locations.end()) {
                        auto_location++;
                    }
                    final_location = auto_location;
                }

                used_locations.insert(final_location);

                spvc_type_id type_id = res.type_id;
                spvc_type type = spvc_compiler_get_type_handle(compiler, type_id);
                spvc_basetype base_type = spvc_type_get_basetype(type);
                GLenum gl_type = GL_NONE;

                if (base_type == SPVC_BASETYPE_SAMPLED_IMAGE) {
                    SpvDim dim = spvc_type_get_image_dimension(type);
                    bool is_array = spvc_type_get_image_arrayed(type);
                    bool is_shadow = spvc_type_get_image_is_depth(type);
                    bool is_ms = spvc_type_get_image_multisampled(type);

                    switch (dim) {
                        case SpvDim1D:
                            gl_type = is_array ? GL_SAMPLER_1D_ARRAY : GL_SAMPLER_1D;
                            if (is_shadow)
                                gl_type = is_array ? GL_SAMPLER_1D_ARRAY_SHADOW
                                                   : GL_SAMPLER_1D_SHADOW;
                            break;
                        case SpvDim2D:
                            if (is_ms) {
                                gl_type = is_array ? GL_SAMPLER_2D_MULTISAMPLE_ARRAY
                                                   : GL_SAMPLER_2D_MULTISAMPLE;
                            } else {
                                gl_type = is_array ? GL_SAMPLER_2D_ARRAY : GL_SAMPLER_2D;
                                if (is_shadow)
                                    gl_type = is_array ? GL_SAMPLER_2D_ARRAY_SHADOW
                                                       : GL_SAMPLER_2D_SHADOW;
                            }
                            break;
                        case SpvDim3D:
                            gl_type = GL_SAMPLER_3D;
                            break;
                        case SpvDimCube:
                            gl_type = is_array ? GL_SAMPLER_CUBE_MAP_ARRAY : GL_SAMPLER_CUBE;
                            if (is_shadow)
                                gl_type = is_array ? GL_SAMPLER_CUBE_MAP_ARRAY_SHADOW
                                                   : GL_SAMPLER_CUBE_SHADOW;
                            break;
                        case SpvDimRect:
                            gl_type = is_shadow ? GL_SAMPLER_2D_RECT_SHADOW : GL_SAMPLER_2D_RECT;
                            break;
                        case SpvDimBuffer:
                            gl_type = GL_SAMPLER_BUFFER;
                            break;
                        default:
                            gl_type = GL_SAMPLER_2D;
                    }

                    spvc_basetype sampled_base = spvc_type_get_basetype(type);
                    if (sampled_base == SPVC_BASETYPE_INT32) {
                        switch (gl_type) {
                            case GL_SAMPLER_1D:
                                gl_type = GL_INT_SAMPLER_1D;
                                break;
                            case GL_SAMPLER_2D:
                                gl_type = GL_INT_SAMPLER_2D;
                                break;
                            case GL_SAMPLER_3D:
                                gl_type = GL_INT_SAMPLER_3D;
                                break;
                            case GL_SAMPLER_CUBE:
                                gl_type = GL_INT_SAMPLER_CUBE;
                                break;
                            case GL_SAMPLER_1D_ARRAY:
                                gl_type = GL_INT_SAMPLER_1D_ARRAY;
                                break;
                            case GL_SAMPLER_2D_ARRAY:
                                gl_type = GL_INT_SAMPLER_2D_ARRAY;
                                break;
                            case GL_SAMPLER_CUBE_MAP_ARRAY:
                                gl_type = GL_INT_SAMPLER_CUBE_MAP_ARRAY;
                                break;
                            case GL_SAMPLER_2D_RECT:
                                gl_type = GL_INT_SAMPLER_2D_RECT;
                                break;
                            case GL_SAMPLER_BUFFER:
                                gl_type = GL_INT_SAMPLER_BUFFER;
                                break;
                            case GL_SAMPLER_2D_MULTISAMPLE:
                                gl_type = GL_INT_SAMPLER_2D_MULTISAMPLE;
                                break;
                            case GL_SAMPLER_2D_MULTISAMPLE_ARRAY:
                                gl_type = GL_INT_SAMPLER_2D_MULTISAMPLE_ARRAY;
                                break;
                        }
                    } else if (sampled_base == SPVC_BASETYPE_UINT32) {
                        switch (gl_type) {
                            case GL_SAMPLER_1D:
                                gl_type = GL_UNSIGNED_INT_SAMPLER_1D;
                                break;
                            case GL_SAMPLER_2D:
                                gl_type = GL_UNSIGNED_INT_SAMPLER_2D;
                                break;
                            case GL_SAMPLER_3D:
                                gl_type = GL_UNSIGNED_INT_SAMPLER_3D;
                                break;
                            case GL_SAMPLER_CUBE:
                                gl_type = GL_UNSIGNED_INT_SAMPLER_CUBE;
                                break;
                            case GL_SAMPLER_1D_ARRAY:
                                gl_type = GL_UNSIGNED_INT_SAMPLER_1D_ARRAY;
                                break;
                            case GL_SAMPLER_2D_ARRAY:
                                gl_type = GL_UNSIGNED_INT_SAMPLER_2D_ARRAY;
                                break;
                            case GL_SAMPLER_CUBE_MAP_ARRAY:
                                gl_type = GL_UNSIGNED_INT_SAMPLER_CUBE_MAP_ARRAY;
                                break;
                            case GL_SAMPLER_2D_RECT:
                                gl_type = GL_UNSIGNED_INT_SAMPLER_2D_RECT;
                                break;
                            case GL_SAMPLER_BUFFER:
                                gl_type = GL_UNSIGNED_INT_SAMPLER_BUFFER;
                                break;
                            case GL_SAMPLER_2D_MULTISAMPLE:
                                gl_type = GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE;
                                break;
                            case GL_SAMPLER_2D_MULTISAMPLE_ARRAY:
                                gl_type = GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE_ARRAY;
                                break;
                        }
                    }
                } else {
                    unsigned vec_size = spvc_type_get_vector_size(type);
                    unsigned columns = spvc_type_get_columns(type);

                    switch (base_type) {
                        case SPVC_BASETYPE_FP32:
                            if (columns > 1) {
                                if (vec_size == 2 && columns == 2) gl_type = GL_FLOAT_MAT2;
                                else if (vec_size == 3 && columns == 3) gl_type = GL_FLOAT_MAT3;
                                else if (vec_size == 4 && columns == 4) gl_type = GL_FLOAT_MAT4;
                                else if (vec_size == 3 && columns == 2) gl_type = GL_FLOAT_MAT2x3;
                                else if (vec_size == 4 && columns == 2) gl_type = GL_FLOAT_MAT2x4;
                                else if (vec_size == 2 && columns == 3) gl_type = GL_FLOAT_MAT3x2;
                                else if (vec_size == 4 && columns == 3) gl_type = GL_FLOAT_MAT3x4;
                                else if (vec_size == 2 && columns == 4) gl_type = GL_FLOAT_MAT4x2;
                                else if (vec_size == 3 && columns == 4) gl_type = GL_FLOAT_MAT4x3;
                            } else {
                                switch (vec_size) {
                                    case 1:
                                        gl_type = GL_FLOAT;
                                        break;
                                    case 2:
                                        gl_type = GL_FLOAT_VEC2;
                                        break;
                                    case 3:
                                        gl_type = GL_FLOAT_VEC3;
                                        break;
                                    case 4:
                                        gl_type = GL_FLOAT_VEC4;
                                        break;
                                }
                            }
                            break;
                        case SPVC_BASETYPE_INT32:
                            switch (vec_size) {
                                case 1:
                                    gl_type = GL_INT;
                                    break;
                                case 2:
                                    gl_type = GL_INT_VEC2;
                                    break;
                                case 3:
                                    gl_type = GL_INT_VEC3;
                                    break;
                                case 4:
                                    gl_type = GL_INT_VEC4;
                                    break;
                            }
                            break;
                        case SPVC_BASETYPE_UINT32:
                            switch (vec_size) {
                                case 1:
                                    gl_type = GL_UNSIGNED_INT;
                                    break;
                                case 2:
                                    gl_type = GL_UNSIGNED_INT_VEC2;
                                    break;
                                case 3:
                                    gl_type = GL_UNSIGNED_INT_VEC3;
                                    break;
                                case 4:
                                    gl_type = GL_UNSIGNED_INT_VEC4;
                                    break;
                            }
                            break;
                        case SPVC_BASETYPE_BOOLEAN:
                            switch (vec_size) {
                                case 1:
                                    gl_type = GL_BOOL;
                                    break;
                                case 2:
                                    gl_type = GL_BOOL_VEC2;
                                    break;
                                case 3:
                                    gl_type = GL_BOOL_VEC3;
                                    break;
                                case 4:
                                    gl_type = GL_BOOL_VEC4;
                                    break;
                            }
                            break;
                        default:
                            continue;
                    }
                }

                prog.uniformLocations[name] = final_location;
                UniformValue uniformValue;
                uniformValue.type = gl_type;
                prog.uniformValues[name] = uniformValue;
            }
        }
        spvc_context_destroy(context);
    }

    std::string CompileSPIRVToGLSL(std::vector<unsigned int> spirv, uint glslVersion, bool isES) {
        spvc_context context = nullptr;
        spvc_parsed_ir ir = nullptr;
        spvc_compiler compiler_glsl = nullptr;
        spvc_compiler_options options = nullptr;
        spvc_resources resources = nullptr;
        const spvc_reflected_resource *list = nullptr;
        const char *result = nullptr;
        size_t count;

        const SpvId *p_spirv = spirv.data();
        size_t word_count = spirv.size();

        spvc_context_create(&context);
        spvc_context_parse_spirv(context, p_spirv, word_count, &ir);
        spvc_context_create_compiler(context, SPVC_BACKEND_GLSL, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &compiler_glsl);
        spvc_compiler_create_shader_resources(compiler_glsl, &resources);
        spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_UNIFORM_BUFFER, &list, &count);
        spvc_compiler_create_compiler_options(compiler_glsl, &options);
        spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_GLSL_VERSION, glslVersion);
        spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_ES, isES ? SPVC_TRUE : SPVC_FALSE);
        spvc_compiler_install_compiler_options(compiler_glsl, options);
        spvc_compiler_compile(compiler_glsl, &result);

        if (!result) {
            return{};
        }

        std::string essl = result;

        spvc_context_destroy(context);

        return essl;
    }
}