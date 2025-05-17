//
// Created by BZLZHH on 2025/4/26.
//

#include "TestDrawing.h"

namespace MG_RHI::GLES::Test {
    constexpr int DISPLAY_COLUMNS = 8;
    constexpr int DISPLAY_ROWS    = 10;
    constexpr int MAX_DISPLAY_TEXTURES = DISPLAY_COLUMNS * DISPLAY_ROWS;

    bool IsTextureComplete(const TextureObject& tex) {
        return !tex.params.mipmapData.empty();
    }

    bool CheckShaderCompileStatus(GLuint shader, const char* type) {
        GLint success;
        ::GLES::glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
        if (!success) {
            GLchar infoLog[512];
            ::GLES::glGetShaderInfoLog(shader, 512, NULL, infoLog);
            MG_Util::Debug::LogE("Shader compilation error (%s): %s", type, infoLog);
            return false;
        }
        return true;
    }

    bool CheckProgramLinkStatus(GLuint program) {
        GLint success;
        ::GLES::glGetProgramiv(program, GL_LINK_STATUS, &success);
        if (!success) {
            GLchar infoLog[512];
            ::GLES::glGetProgramInfoLog(program, 512, NULL, infoLog);
            MG_Util::Debug::LogE("Program link error: %s", infoLog);
            return false;
        }
        return true;
    }

    // TODO: Use the textures state from MG_RHI(another state?) rather than directly from MG_State
    void DrawAllTextures() {
        ::GLES::glClearColor(1,1,1,1);
        ::GLES::glClear(GL_COLOR_BUFFER_BIT);
        
        struct ScreenResources {
            GLuint program = 0;
            GLuint quadVAO = 0;
            GLuint quadVBO = 0;
            GLint uTexLoc = -1;
            GLint uMVPLoc = -1;
            ankerl::unordered_map<GLuint, GLuint> textureCache;
            ankerl::unordered_map<GLuint, bool> textureDirty;
        };

        static ScreenResources sRes;
        static glm::ivec2 lastScreenSize{0, 0};

        GLint viewport[4];
        ::GLES::glGetIntegerv(GL_VIEWPORT, viewport);

        const glm::ivec2 screenSize(viewport[2], viewport[3]);

        if (screenSize != lastScreenSize) {
            if (sRes.quadVAO) {
                ::GLES::glDeleteVertexArrays(1, &sRes.quadVAO);
                ::GLES::glDeleteBuffers(1, &sRes.quadVBO);
                sRes.quadVAO = sRes.quadVBO = 0;
            }
            lastScreenSize = screenSize;
        }

        if (!sRes.program) {
            const char* vsSrc = R"(#version 320 es
            layout(location=0) in vec2 aPos;
            layout(location=1) in vec2 aTexUV;
            out vec2 vUV;
            uniform mat4 uMVP;
            void main() {
                gl_Position = uMVP * vec4(aPos, 0.0, 1.0);
                vUV = aTexUV;
            })";

            const char* fsSrc = R"(#version 320 es
            precision mediump float;
            in vec2 vUV;
            uniform sampler2D uTex;
            out vec4 FragColor;
            void main() {
                FragColor = texture(uTex, vUV);
            })";

            GLuint vs = ::GLES::glCreateShader(GL_VERTEX_SHADER);
            ::GLES::glShaderSource(vs, 1, &vsSrc, NULL);
            ::GLES::glCompileShader(vs);
            if (!CheckShaderCompileStatus(vs, "vertex")) return;

            GLuint fs = ::GLES::glCreateShader(GL_FRAGMENT_SHADER);
            ::GLES::glShaderSource(fs, 1, &fsSrc, NULL);
            ::GLES::glCompileShader(fs);
            if (!CheckShaderCompileStatus(fs, "fragment")) return;

            sRes.program = ::GLES::glCreateProgram();
            ::GLES::glAttachShader(sRes.program, vs);
            ::GLES::glAttachShader(sRes.program, fs);
            ::GLES::glLinkProgram(sRes.program);
            if (!CheckProgramLinkStatus(sRes.program)) return;

            sRes.uTexLoc = ::GLES::glGetUniformLocation(sRes.program, "uTex");
            sRes.uMVPLoc = ::GLES::glGetUniformLocation(sRes.program, "uMVP");

            ::GLES::glDeleteShader(vs);
            ::GLES::glDeleteShader(fs);
        }

        if (!sRes.quadVAO) {
            const float margin = 0.01f;
            const float gridWidth = (2.0f - 2*margin) / DISPLAY_COLUMNS;
            const float gridHeight = (2.0f - 2*margin) / DISPLAY_ROWS;

            std::vector<float> vertices;
            vertices.reserve(MAX_DISPLAY_TEXTURES * 16);

            for (int col = 0; col < DISPLAY_COLUMNS; ++col) {
                for (int row = 0; row < DISPLAY_ROWS; ++row) {
                    const float xStart = -1.0f + margin + col * gridWidth;
                    const float yStart = -1.0f + margin + row * gridHeight;
                    const float xEnd = xStart + gridWidth - margin;
                    const float yEnd = yStart + gridHeight - margin;

                    vertices.insert(vertices.end(), {
                            xStart, yStart, 0.0f, 0.0f,
                            xEnd,   yStart, 1.0f, 0.0f,
                            xEnd,   yEnd,   1.0f, 1.0f,
                            xStart, yEnd,   0.0f, 1.0f
                    });
                }
            }

            ::GLES::glGenVertexArrays(1, &sRes.quadVAO);
            ::GLES::glGenBuffers(1, &sRes.quadVBO);
            ::GLES::glBindVertexArray(sRes.quadVAO);
            ::GLES::glBindBuffer(GL_ARRAY_BUFFER, sRes.quadVBO);
            ::GLES::glBufferData(GL_ARRAY_BUFFER,
                                 vertices.size() * sizeof(float),
                                 vertices.data(),
                                 GL_STATIC_DRAW
            );

            ::GLES::glEnableVertexAttribArray(0);
            ::GLES::glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE,
                                          4 * sizeof(float), (void*)0
            );
            ::GLES::glEnableVertexAttribArray(1);
            ::GLES::glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE,
                                          4 * sizeof(float), (void*)(2 * sizeof(float))
            );
            ::GLES::glBindVertexArray(0);
        }

        std::vector<std::pair<GLuint, TextureObject*>> sortedTextures;
        for (auto&& [stateID, texObj] : MG_State_T::textureState->textures) {
            if (!texObj.generated || texObj.target != GL_TEXTURE_2D) continue;
            if (texObj.params.mipmapData.empty()) continue;
            sortedTextures.emplace_back(stateID, &texObj);
        }
        std::sort(sortedTextures.begin(), sortedTextures.end(),
                  [](auto& a, auto& b) {
                      return a.second->createTimestamp > b.second->createTimestamp;
                  }
        );
        sortedTextures.resize(std::min<size_t>(sortedTextures.size(), MAX_DISPLAY_TEXTURES));

        for (auto&& [stateID, texObj] : sortedTextures) {
            auto& dirtyFlag = sRes.textureDirty[stateID];
            bool needCreate = !sRes.textureCache.count(stateID);

            if (needCreate) {
                GLuint glTexID;
                ::GLES::glGenTextures(1, &glTexID);
                sRes.textureCache[stateID] = glTexID;
                dirtyFlag = true;
            }

            if (dirtyFlag || needCreate) {
                const auto& mip0 = texObj->params.mipmapData.begin()->second;
                if (mip0.hasData) {
                    GLuint glTexID = sRes.textureCache[stateID];
                    ::GLES::glBindTexture(GL_TEXTURE_2D, glTexID);
                    ::GLES::glTexImage2D(GL_TEXTURE_2D, 0, mip0.internalFormat,
                                         mip0.width, mip0.height, 0,
                                         mip0.format, mip0.type, mip0.pixelData.data()
                    );
                    ::GLES::glTexParameteri(GL_TEXTURE_2D,
                                            GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    ::GLES::glTexParameteri(GL_TEXTURE_2D,
                                            GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    ::GLES::glTexParameteri(GL_TEXTURE_2D,
                                            GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    ::GLES::glTexParameteri(GL_TEXTURE_2D,
                                            GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    if (texObj->params.mipmapData.size() > 1) {
                        ::GLES::glGenerateMipmap(GL_TEXTURE_2D);
                    }
                    dirtyFlag = false;
                }
            }
        }

        GLboolean origDepthTest = ::GLES::glIsEnabled(GL_DEPTH_TEST);
        GLint origProgram, origViewport[4];
        ::GLES::glGetIntegerv(GL_VIEWPORT, origViewport);
        ::GLES::glGetIntegerv(GL_CURRENT_PROGRAM, &origProgram);

        ::GLES::glDisable(GL_DEPTH_TEST);
        ::GLES::glUseProgram(sRes.program);
        ::GLES::glUniformMatrix4fv(sRes.uMVPLoc, 1, GL_FALSE,
                                   &glm::mat4(1.0f)[0][0]
        );
        ::GLES::glUniform1i(sRes.uTexLoc, 0);
        ::GLES::glBindVertexArray(sRes.quadVAO);

        for (size_t i = 0; i < sortedTextures.size(); ++i) {
            const GLuint glTexID = sRes.textureCache[sortedTextures[i].first];
            ::GLES::glActiveTexture(GL_TEXTURE0);
            ::GLES::glBindTexture(GL_TEXTURE_2D, glTexID);
            ::GLES::glDrawArrays(GL_TRIANGLE_FAN, static_cast<GLint>(i*4), 4);
        }

        if (origDepthTest) ::GLES::glEnable(GL_DEPTH_TEST);
        ::GLES::glUseProgram(origProgram);
        ::GLES::glViewport(origViewport[0], origViewport[1],
                           origViewport[2], origViewport[3]);
    }
}