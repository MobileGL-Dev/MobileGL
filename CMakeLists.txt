cmake_minimum_required(VERSION 3.22.1)

project("MobileGL")

option(MOBILEGL_BUILD_TEST        "Build MobileGL tests"                             ON )
option(MOBILEGL_BUILD_BENCHMARK   "Build MobileGL benchmarks"                        ON )
# Headless end-to-end GPU scenarios (MobileGL/MG_IntegrationTest). They need a
# real GPU/ICD to do anything, so they are off by default for CI; every scenario
# skips cleanly where there is none. Registered under the `integration-gpu`
# ctest label so a run can select or exclude them.
option(MOBILEGL_BUILD_INTEGRATION_TEST "Build MobileGL headless GPU integration tests" OFF)
option(MOBILEGL_FORCE_RELEASE_OPT "Enable Release optimization flags in Debug build" ON )
option(MOBILEGL_ENABLE_TRACY      "Enable tracy for profiling"                       OFF)
option(MOBILEGL_BUILD_TRACE_REPLAY "Build desktop apitrace replay runner"            OFF)
option(MOBILEGL_TRACE_ANGLE_VARIANTS "Enable signed trace-APK ANGLE variant loading" OFF)
option(MOBILEGL_IOS               "Build MobileGL for iOS instead of macOS when APPLE is set" OFF)
set(MOBILEGL_LOG_ACTIVE_LEVEL "MOBILEGL_LOG_LEVEL_INFO" CACHE STRING "MobileGL active log level macro")
set(MOBILEGL_VULKAN_LIBRARY "" CACHE FILEPATH "Vulkan loader/MoltenVK library to link for iOS builds")

if (ANDROID)
    set(MOBILEGL_BUILD_TEST       OFF CACHE BOOL "Build MobileGL tests"      FORCE)
    set(MOBILEGL_BUILD_BENCHMARK  OFF CACHE BOOL "Build MobileGL benchmarks" FORCE)

    # ------- Android API level policy: minimum 26, decided here and only here -------
    # MobileGL ships against API 26: the codebase must not use any API introduced
    # after 26. That usage constraint is enforced where it is real - the shipping
    # gradle build compiles at minSdk 26, where a newer API is simply undeclared
    # and fails to compile. Configuring at a HIGHER level is therefore allowed
    # (nothing in the tree may rely on it), but a LOWER level would change the
    # libc contract underneath the shipped library and is refused.
    #
    # This has to live at configure time because the level cannot be corrected
    # from a source header. A `#define __ANDROID_API__ 26` in a common header
    # only rewrites the macro for the bionic headers that happen to be included
    # after it; any libc++ header pulled in earlier has already latched its
    # feature macros at the real configure-time level. libc++ and bionic then
    # disagree about which symbols exist - libc++ calls e.g.
    # pthread_cond_clockwait while bionic, re-read at the lowered level, has
    # hidden its declaration. MobileGL/Defines.h carried exactly that pin from
    # the first commit until it was removed; this guard is what replaces it.
    #
    # Read the level back from the compiler target triple first. Its trailing
    # number (aarch64-none-linux-android26) is precisely what clang turns into
    # __ANDROID_API__, so it cannot disagree with the compile itself, and it is
    # already past every NDK normalisation step - codename aliases, "latest",
    # and per-ABI minimum pull-ups. ANDROID_PLATFORM_LEVEL is the fallback for
    # generators/languages where the triple variable is not populated.
    #
    # Note CMAKE_SYSTEM_VERSION is deliberately NOT consulted: it holds the API
    # level only under the NDK's newer toolchain path, and is a meaningless 1
    # when ANDROID_USE_LEGACY_TOOLCHAIN_FILE is on (which is what AGP has been
    # defaulting to). Reading it would fail every legacy-mode build.
    set(MOBILEGL_ANDROID_API_LEVEL 26)

    set(_mobilegl_android_api "")
    foreach (_mobilegl_api_triple "${CMAKE_CXX_COMPILER_TARGET}"
                                  "${CMAKE_C_COMPILER_TARGET}")
        if (NOT _mobilegl_android_api AND
            _mobilegl_api_triple MATCHES "-android([0-9]+)$")
            set(_mobilegl_android_api "${CMAKE_MATCH_1}")
        endif()
    endforeach()

    foreach (_mobilegl_api_var ANDROID_PLATFORM_LEVEL ANDROID_NATIVE_API_LEVEL
                               ANDROID_PLATFORM)
        if (NOT _mobilegl_android_api AND ${_mobilegl_api_var})
            string(REGEX REPLACE "^android-" ""
                   _mobilegl_android_api "${${_mobilegl_api_var}}")
        endif()
    endforeach()

    if (NOT _mobilegl_android_api MATCHES "^[0-9]+$")
        message(FATAL_ERROR
            "MobileGL: could not determine the Android API level (got "
            "\"${_mobilegl_android_api}\"). Configure with the NDK toolchain "
            "file and -DANDROID_PLATFORM=android-${MOBILEGL_ANDROID_API_LEVEL}.")
    elseif (_mobilegl_android_api LESS MOBILEGL_ANDROID_API_LEVEL)
        message(FATAL_ERROR
            "MobileGL requires at least Android API ${MOBILEGL_ANDROID_API_LEVEL}, "
            "but this build resolved to API ${_mobilegl_android_api}.\n"
            "Configure with -DANDROID_PLATFORM=android-${MOBILEGL_ANDROID_API_LEVEL} "
            "(gradle builds get this from minSdk ${MOBILEGL_ANDROID_API_LEVEL}, so "
            "check that minSdk instead of adding an override).")
    elseif (_mobilegl_android_api GREATER MOBILEGL_ANDROID_API_LEVEL)
        message(STATUS
            "MobileGL: configuring at Android API ${_mobilegl_android_api} "
            "(> shipping minimum ${MOBILEGL_ANDROID_API_LEVEL}). Allowed, but the "
            "tree must not use post-${MOBILEGL_ANDROID_API_LEVEL} APIs - the "
            "minSdk-${MOBILEGL_ANDROID_API_LEVEL} gradle build is the enforcing "
            "compile.")
    endif()

    message(STATUS "MobileGL: Android API level ${_mobilegl_android_api}")

    unset(_mobilegl_android_api)
    unset(_mobilegl_api_var)
    unset(_mobilegl_api_triple)
endif()

option(MOBILEGL_ENABLE_LTO "Build with ThinLTO/IPO" OFF)

if ((NOT CMAKE_BUILD_TYPE STREQUAL "Debug" OR MOBILEGL_FORCE_RELEASE_OPT) AND MOBILEGL_ENABLE_LTO)
    # Check if ThinLTO or LTO is suppported
    include(CheckIPOSupported)
    include(CheckCCompilerFlag)
    include(CheckCXXCompilerFlag)

    check_ipo_supported(RESULT LTOSupported OUTPUT LTOError)

    check_c_compiler_flag("-flto" HAS_LTO_C)
    check_cxx_compiler_flag("-flto" HAS_LTO_CXX)

    if (LTOSupported OR (HAS_LTO_C AND HAS_LTO_CXX))
        # Check ThinLTO
        check_c_compiler_flag("-flto=thin" HAS_THINLTO_C)
        check_cxx_compiler_flag("-flto=thin" HAS_THINLTO_CXX)
        if (HAS_THINLTO_C AND HAS_THINLTO_CXX)
            message(STATUS "ThinLTO supported, using -flto=thin")

            add_compile_options(-flto=thin)
            add_link_options(-flto=thin)
        else()
            # ThinLTO is not supported
            message(STATUS "ThinLTO not available, fallback to CMAKE IPO")
            set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
        endif()
    else()
        message(STATUS "IPO not supported: ${LTOError}")
    endif()

    if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT MATCHES "AppleClang")
        add_compile_options(-O3 -ffunction-sections -fdata-sections)
        add_link_options(-Wl,--gc-sections)
    elseif (CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
#        add_compile_options(/O2)
    else ()
        add_compile_options(-O2)
    endif()
endif()

if (CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
    add_compile_options(/Zc:preprocessor)
    add_compile_options(/Zc:__cplusplus)
endif()

enable_language(CXX)

set(CMAKE_CXX_STANDARD 23)

set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if (MSVC)
    set(CMAKE_CXX_FLAGS "/EHsc ${CMAKE_CXX_FLAGS}")
endif()

# Generate MGGitHash.h
execute_process(
    COMMAND git rev-parse HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_COMMIT_HASH_FULL
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

string(SUBSTRING "${GIT_COMMIT_HASH_FULL}" 0 7 GIT_COMMIT_HASH_SHORT)

configure_file(
    ${CMAKE_SOURCE_DIR}/MobileGL/MG_Util/Miscellany/MGGitHash.h.in
    ${CMAKE_BINARY_DIR}/generated/MGGitHash.h
    @ONLY
)

include_directories(${CMAKE_BINARY_DIR}/generated)

set(ENABLE_RTTI                ON  CACHE BOOL "Enables RTTI (will be took by glslang)" FORCE)

set(ENABLE_GLSLANG_BINARIES    OFF CACHE BOOL "Enable glslangValidator/spirv-remap" FORCE)
set(ENABLE_SPVREMAPPER         OFF CACHE BOOL "Enable SPVRemapper" FORCE)
set(ENABLE_OPT                 ON  CACHE BOOL "Enable SPIRV-Tools opt usage in glslang" FORCE)
set(BUILD_EXTERNAL             ON  CACHE BOOL "Build external deps in External/" FORCE)
set(ENABLE_GLSLANG_INSTALL     OFF CACHE BOOL "Install glslang targets" FORCE)
set(SPIRV_SKIP_EXECUTABLES     ON  CACHE BOOL "Skip building SPIRV-Tools executables" FORCE)

set(SPIRV_CROSS_C_API          ON  CACHE BOOL "Enable C API" FORCE)
set(SPIRV_CROSS_ENABLE_GLSL    ON  CACHE BOOL "Enable GLSL backend" FORCE)
set(SPIRV_CROSS_ENABLE_HLSL    OFF CACHE BOOL "Disable HLSL backend" FORCE)
set(SPIRV_CROSS_ENABLE_MSL     OFF CACHE BOOL "Disable MSL backend" FORCE)
set(SPIRV_CROSS_ENABLE_CPP     OFF CACHE BOOL "Disable C++ API target" FORCE)
set(SPIRV_CROSS_CLI            OFF CACHE BOOL "Disable CLI binary" FORCE)
set(SPIRV_CROSS_STATIC         ON  CACHE BOOL "Prefer static libs" FORCE)

set(SPIRV_REFLECT_EXECUTABLE     OFF CACHE BOOL "Build spirv-reflect executable" FORCE)
set(SPIRV_REFLECT_STATIC_LIB     ON  CACHE BOOL "Build a SPIRV-Reflect static library" FORCE)
set(SPIRV_REFLECT_BUILD_TESTS    OFF CACHE BOOL "Build the SPIRV-Reflect test suite" FORCE)
set(SPIRV_REFLECT_ENABLE_ASSERTS OFF CACHE BOOL  "Enable asserts for debugging" FORCE)
set(SPIRV_REFLECT_ENABLE_ASAN    OFF CACHE BOOL "Use address sanitization" FORCE)
set(SPIRV_REFLECT_INSTALL        OFF CACHE BOOL "Whether to install" FORCE)

# add_subdirectory(3rdparty/DiligentCore)
add_subdirectory(3rdparty/glslang)
add_subdirectory(3rdparty/SPIRV-Cross)
add_subdirectory(3rdparty/VulkanMemoryAllocator)
add_subdirectory(3rdparty/Vulkan-Headers)
add_subdirectory(3rdparty/Vulkan-Utility-Libraries)
add_subdirectory(3rdparty/SPIRV-Reflect)

set(XXHASH_BUILD_XXHSUM OFF)
option(BUILD_SHARED_LIBS OFF)
add_subdirectory(3rdparty/xxHash/build/cmake xxhash_build EXCLUDE_FROM_ALL)

set(TRACY_ENABLE ${MOBILEGL_ENABLE_TRACY} CACHE BOOL "Enable Tracy, this is an internal variable" FORCE)

if (TRACY_ENABLE)
    set(TRACY_ON_DEMAND            ON  CACHE BOOL "Enable profiling only connected" FORCE)
    set(TRACY_NO_EXIT              OFF CACHE BOOL "Don't exit Tracy until connected" FORCE)
    set(TRACY_DELAYED_INIT         ON  CACHE BOOL "Don't init Tracy on library load" FORCE)
    set(TRACY_MANUAL_LIFETIME      ON  CACHE BOOL "Manually control Tracy lifetime" FORCE)
    set(TRACY_NO_CRASH_HANDLER     ON  CACHE BOOL "Disable crash handling" FORCE)

    add_subdirectory(3rdparty/tracy)
endif ()

set(SOURCE_FILES
    MobileGL/Init.cpp
    MobileGL/GlobalObjects.cpp
    MobileGL/ConfigLoader.cpp

    MobileGL/MG_Util/Debug/Log.cpp

    MobileGL/MG_Util/Async/JobNode.cpp
    MobileGL/MG_Util/Async/ShaderCompilePool.cpp

    MobileGL/MG_Util/Math/VectorTypes.cpp
    MobileGL/MG_Util/Metrics/TextureMetrics.cpp

    MobileGL/MG_Util/Metrics/BufferMetrics.cpp

    MobileGL/MG_Util/Converters/GLToStr/GLEnumConverter.cpp
    MobileGL/MG_Util/Converters/EGLToStr/EGLEnumConverter.cpp
    MobileGL/MG_Util/Converters/MGToStr/DataTypeConverter.cpp
    MobileGL/MG_Util/Converters/MGToStr/RenderStateEnumConverter.cpp
    MobileGL/MG_Util/Converters/MGToStr/GLExtensionConverter.cpp
    MobileGL/MG_Util/Converters/MGToStr/BufferEnumConverter.cpp
    MobileGL/MG_Util/Converters/MGToStr/FramebufferEnumConverter.cpp
    MobileGL/MG_Util/Converters/MGToStr/TextureEnumConverter.cpp
    MobileGL/MG_Util/Converters/GLToGlslang/ProgramEnumConverter.cpp
    MobileGL/MG_Util/Converters/MGToGL/ErrorCodeConverter.cpp
    MobileGL/MG_Util/Converters/MGToGL/BufferEnumConverter.cpp
    MobileGL/MG_Util/Converters/MGToGL/FramebufferEnumConverter.cpp
    MobileGL/MG_Util/Converters/MGToGL/TextureEnumConverter.cpp
    MobileGL/MG_Util/Converters/MGToGL/DataTypeConverter.cpp
    MobileGL/MG_Util/Converters/MGToGL/RenderStateEnumConverter.cpp
    MobileGL/MG_Util/Converters/MGToGL/ProgramEnumConverter.cpp
    MobileGL/MG_Util/Converters/GLToMG/BufferEnumConverter.cpp
    MobileGL/MG_Util/Converters/GLToMG/FramebufferEnumConverter.cpp
    MobileGL/MG_Util/Converters/GLToMG/TextureEnumConverter.cpp
    MobileGL/MG_Util/Converters/GLToMG/DataTypeConverter.cpp
    MobileGL/MG_Util/Converters/GLToMG/RenderStateEnumConverter.cpp
    MobileGL/MG_Util/Converters/GLToMG/ProgramEnumConverter.cpp
    MobileGL/MG_Util/Converters/MGToMG/TextureEnumConverter.cpp
    MobileGL/MG_Util/Converters/MGToVk/RenderStateEnumConverter.cpp
    MobileGL/MG_Util/Converters/MGToVk/TextureEnumConverter.cpp

    MobileGL/MG_Util/Classifiers/TextureEnumClassifier.cpp

    MobileGL/MG_Util/ShaderTranspiler/CompileEnv.cpp
    MobileGL/MG_Util/ShaderTranspiler/ShaderCompiler.cpp
    MobileGL/MG_Util/ShaderTranspiler/SpvcSession.cpp
    MobileGL/MG_Util/ShaderTranspiler/ShaderSourceProcessor.cpp
    MobileGL/MG_Util/ShaderTranspiler/TranslationCache.cpp
    MobileGL/MG_Util/ShaderTranspiler/glslang/TMglGlslIoResolver.cpp
    MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/FlattenInterfaceStructPass.cpp
    MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/EliminateFloatEqualsZeroPass.cpp
    MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/RenameSamplerFunctionParameterPass.cpp
    MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/RenameBuiltinShadowingFunctionsPass.cpp
    MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/DecomposeWorkgroupVec3Pass.cpp
    MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/DecoratePositionInvariantPass.cpp
    MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/DemoteFloat64Pass.cpp
    MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/FlattenFloat64StorageBlockPass.cpp
    MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/LowerDrawParametersPass.cpp
    MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/LowerViewportIndexPass.cpp
    MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/PackDoubleVertexInputsPass.cpp
    MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/FlattenXfbInterfaceBlocksPass.cpp
    MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/UniquifyIoBlockNamesPass.cpp
    MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/StripIoBlockLocationsPass.cpp
    MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/SplitArrayVertexInputsPass.cpp
    MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/RebaseInstanceIndexPass.cpp
    MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/ZeroBaseVertexPass.cpp
    MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/DeriveNumSubgroupsPass.cpp
    MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/FixIterationRPBarrierPass.cpp
    MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/FixIterationRPSubgroupScratchPass.cpp
    MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/EmulateSubgroupsPass.cpp
    MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/NormalizeRectCoordinatesPass.cpp
    MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/Lower1DArrayImagesPass.cpp
    MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/Lower1DSampledImagesPass.cpp
    MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/BakeImageFormatsPass.cpp
    MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/WidenImageFormatsPass.cpp
    MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/ClampMultisampleFetchPass.cpp
    MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/PrivateToEntryLocalPass.cpp
    MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/StripUniformLocationsPass.cpp
    MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/StripUboMemberRelaxedPrecisionPass.cpp
    MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/StripNoPerspectivePass.cpp
    MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/EmulateNoPerspectivePass.cpp
    MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/LegalizeFragmentOutputIndexPass.cpp
    MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/LegalizeResourceArrayIndexPass.cpp
    MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/FlattenAtomicCounterBlockPass.cpp

    MobileGL/MG_Util/BackendLoaders/OpenGL/Loader.cpp
    MobileGL/MG_Util/BackendLoaders/Vulkan/Loader.cpp

    MobileGL/MG_Util/SelfTest/DriverBugProbes.cpp
    MobileGL/MG_Util/SelfTest/DriverPost.cpp
    MobileGL/MG_Util/SelfTest/DriverPostIterationRPWitness.cpp
    MobileGL/MG_Util/SelfTest/PrimitivesGeneratedNoXfbProbe.cpp

    MobileGL/MG_Util/Texture/PixelStoreProcessor.cpp
    MobileGL/MG_Util/Texture/TextureFormatProcessor.cpp

    MobileGL/MG_Impl/GLXImpl/Exporting/Definitions.cpp
    MobileGL/MG_Impl/GLXImpl/GLXImpl.cpp
    MobileGL/MG_Impl/GLXImpl/LookUp/LookUp.cpp

    MobileGL/MG_Impl/EGLImpl/Exporting/Definitions.cpp
    MobileGL/MG_Impl/EGLImpl/EGLImpl.cpp
    
    # @INSERTION_POINT:SOURCE_FILE_GLIMPL@ #
    MobileGL/MG_Impl/GLImpl/Sampler/Validators.cpp
    MobileGL/MG_Impl/GLImpl/Sampler/GL_Sampler.cpp
    MobileGL/MG_Impl/GLImpl/Drawing/GL_Drawing.cpp
    MobileGL/MG_Impl/GLImpl/RenderState/GL_RenderState.cpp
    MobileGL/MG_Impl/GLImpl/Framebuffer/Validators.cpp
    MobileGL/MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.cpp
    MobileGL/MG_Impl/GLImpl/Program/GL_Program.cpp
    MobileGL/MG_Impl/GLImpl/Program/ProgramInterface.cpp
    MobileGL/MG_Impl/GLImpl/Program/GL_ProgramPipeline.cpp
    MobileGL/MG_Impl/GLImpl/Texture/GL_Texture.cpp
    MobileGL/MG_Impl/GLImpl/Debug/GL_Debug.cpp
    MobileGL/MG_Impl/GLImpl/Texture/Validators.cpp
    MobileGL/MG_Impl/GLImpl/Texture/ProxyTexture.cpp
    MobileGL/MG_Impl/GLImpl/VertexArray/GL_VertexArray.cpp
    MobileGL/MG_Impl/GLImpl/VertexArray/Validators.cpp
    MobileGL/MG_Impl/GLImpl/Buffer/GL_Buffer.cpp
    MobileGL/MG_Impl/GLImpl/Buffer/Validators.cpp
    MobileGL/MG_Impl/GLImpl/Exporting/Definitions.cpp
    MobileGL/MG_Impl/GLImpl/Getter/GL_Getter.cpp
    MobileGL/MG_Impl/GLImpl/Sync/GL_Sync.cpp
    MobileGL/MG_Impl/GLImpl/Query/GL_Query.cpp

    MobileGL/MG_Impl/Init.cpp
    MobileGL/MG_Impl/GetProcAddress.cpp

    MobileGL/MG_Backend/Init.cpp
    MobileGL/MG_Backend/BackendObject.cpp

    MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp
    MobileGL/MG_Backend/DirectGLES/BackendObject_DirectGLES.cpp
    MobileGL/MG_Backend/DirectGLES/Utils.cpp
    MobileGL/MG_Backend/DirectGLES/Managers.cpp
    MobileGL/MG_Backend/DirectGLES/MultiDraw.cpp

    MobileGL/MG_Backend/DirectVulkan/DirectVulkan.cpp
    MobileGL/MG_Backend/DirectVulkan/BackendObject_DirectVulkan.cpp
    MobileGL/MG_Backend/DirectVulkan/VmaImpl.cpp
    MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp
    MobileGL/MG_Backend/DirectVulkan/Renderer/SwapchainObject.cpp
    MobileGL/MG_Backend/DirectVulkan/Renderer/FrameContext.cpp
    MobileGL/MG_Backend/DirectVulkan/Renderer/PipelineFactory.cpp
    MobileGL/MG_Backend/DirectVulkan/Renderer/ProgramFactory.cpp
        MobileGL/MG_Backend/DirectVulkan/Renderer/UniformManager.cpp
    MobileGL/MG_Backend/DirectVulkan/Renderer/BufferArena.cpp
    MobileGL/MG_Backend/DirectVulkan/Renderer/VkBufferManager.cpp
    MobileGL/MG_Backend/DirectVulkan/Renderer/VertexInputStateBuilder.cpp
    MobileGL/MG_Backend/DirectVulkan/Renderer/VertexInputStateFactory.cpp
    MobileGL/MG_Backend/DirectVulkan/Renderer/VkBufferObject.cpp
    MobileGL/MG_Backend/DirectVulkan/Renderer/VkTextureManager.cpp
    MobileGL/MG_Backend/DirectVulkan/Renderer/VkTimerQueryManager.cpp
    MobileGL/MG_Backend/DirectVulkan/Renderer/VkSamplerManager.cpp
    MobileGL/MG_Backend/DirectVulkan/Renderer/VkClearManager.cpp
    MobileGL/MG_Backend/DirectVulkan/Renderer/VkRenderPassManager.cpp

    MobileGL/MG_State/GLState/Core.cpp
    MobileGL/MG_State/EGLState/Core.cpp
    MobileGL/MG_State/GLState/ErrorState/Error.cpp
    MobileGL/MG_State/GLState/BufferState/BufferState.cpp
    MobileGL/MG_State/GLState/BufferState/BufferObject.cpp
    MobileGL/MG_State/GLState/VertexArrayState/VertexArrayState.cpp
    MobileGL/MG_State/GLState/VertexArrayState/VertexArrayObject.cpp
    MobileGL/MG_State/GLState/TextureState/MipmapStorage.cpp
    MobileGL/MG_State/GLState/TextureState/TextureObject.cpp
    MobileGL/MG_State/GLState/TextureState/TextureObject1D.cpp
    MobileGL/MG_State/GLState/TextureState/TextureObject2D.cpp
    MobileGL/MG_State/GLState/TextureState/TextureObject2DCube.cpp
    MobileGL/MG_State/GLState/TextureState/TextureObject3D.cpp
    MobileGL/MG_State/GLState/TextureState/TextureObjectBuffer.cpp
    MobileGL/MG_State/GLState/TextureState/TextureObjectView.cpp
    MobileGL/MG_State/GLState/TextureState/TextureUnit.cpp
    MobileGL/MG_State/GLState/TextureState/TextureState.cpp
    MobileGL/MG_State/GLState/ProgramState/ProgramObject.cpp
    MobileGL/MG_State/GLState/ProgramState/ProgramLinkTask.cpp
    MobileGL/MG_State/GLState/ProgramState/ProgramTranslationCache.cpp
    MobileGL/MG_State/GLState/ProgramState/ProgramSpirvTask.cpp
    MobileGL/MG_State/GLState/ProgramState/ShaderCompileTask.cpp
    MobileGL/MG_State/GLState/ProgramState/ShaderObject.cpp
    MobileGL/MG_State/GLState/ProgramState/ShaderPreprocessCache.cpp
    MobileGL/MG_State/GLState/ProgramState/ShaderCompileAdoptionMap.cpp
    MobileGL/MG_State/GLState/ProgramState/ProgramState.cpp
    MobileGL/MG_State/GLState/RenderState/RenderState.cpp
    MobileGL/MG_State/GLState/FramebufferState/FramebufferObject.cpp
    MobileGL/MG_State/GLState/FramebufferState/FramebufferState.cpp
    MobileGL/MG_State/GLState/SamplerState/SamplerObject.cpp
    MobileGL/MG_State/GLState/SamplerState/SamplerState.cpp
    MobileGL/MG_State/GLState/RenderbufferState/RenderbufferObject.cpp
    MobileGL/MG_State/GLState/RenderbufferState/RenderbufferState.cpp
)

if (APPLE AND NOT MOBILEGL_IOS)
    list(APPEND SOURCE_FILES
        MobileGL/MG_Impl/CGLImpl/CGLImpl.cpp
        MobileGL/MG_Impl/CGLImpl/Exporting/Definitions.cpp
        MobileGL/MG_Impl/DyldInterpose/DyldInterpose.cpp
        MobileGL/MG_Impl/NSOpenGLImpl/NSOpenGLImpl.cpp
    )
endif()

if (ANDROID)
    list(APPEND SOURCE_FILES
        MobileGL/MG_Util/SelfTest/DriverPostJni.cpp
        MobileGL/MG_Util/SelfTest/DriverBenchJni.cpp
    )
endif()

if (WIN32)
    list(APPEND SOURCE_FILES
        MobileGL/MG_Impl/WGLImpl/WGLImpl.cpp
        MobileGL/MG_Impl/WGLImpl/Exporting/Definitions.cpp
    )
endif()

# The shader-compile pool runs standalone Asio on real threads. This host's glibc (>= 2.34)
# merged pthread into libc, so it links without asking, but the NDK and musl are not
# guaranteed to be as forgiving - ask for it explicitly rather than rely on the accident.
find_package(Threads REQUIRED)

set(MOBILEGL_LINK_LIBRARIES
        glslang::glslang
        spirv-cross-c
        SPIRV-Tools-opt
        SPIRV-Tools
        xxHash::xxhash
        GPUOpen::VulkanMemoryAllocator
        Vulkan::UtilityHeaders
        spirv-reflect-static
        Threads::Threads
)

set(MOBILEGL_COMPILE_DEF
        -DVMA_STATIC_VULKAN_FUNCTIONS=0
        -DVMA_DYNAMIC_VULKAN_FUNCTIONS=1
        -DVMA_VULKAN_VERSION=1001000
        # Header-only Asio, no Boost, no deprecated interfaces. Set on the definition list
        # rather than per-target so the shared library and the _s static target agree.
        -DASIO_STANDALONE
        -DASIO_NO_DEPRECATED
)

message(STATUS "MOBILEGL_COMPILE_DEF=${MOBILEGL_COMPILE_DEF}")

set(MOBILEGL_INCLUDE_DIR
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/MobileGL
        ${spirv-tools_SOURCE_DIR}
        ${spirv-tools_SOURCE_DIR}/include
        ${spirv-tools_BINARY_DIR}
        ${SPIRV-Headers_SOURCE_DIR}/include
        # Header-only submodule: no add_subdirectory, no link target. Only
        # MG_Util/Async/ShaderCompilePool.cpp includes it, and it stays behind that file's
        # pimpl so no consumer target needs this path.
        ${CMAKE_SOURCE_DIR}/3rdparty/asio/include
)

add_library(${CMAKE_PROJECT_NAME} SHARED
    ${SOURCE_FILES}
)

if (WIN32)
    # The wgl* entry points are exported via .def (see the comment in wgl.def);
    # only the shared library links it.
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE
        MobileGL/MG_Impl/WGLImpl/Exporting/wgl.def
    )
endif()

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES
        C_VISIBILITY_PRESET default
        CXX_VISIBILITY_PRESET default
        VISIBILITY_INLINES_HIDDEN OFF
    )
else()
    set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )
endif()

target_include_directories(${CMAKE_PROJECT_NAME} PUBLIC
    ${MOBILEGL_INCLUDE_DIR}
)

target_link_libraries(${CMAKE_PROJECT_NAME}
    PUBLIC
    ${MOBILEGL_LINK_LIBRARIES}
)

target_compile_definitions(${CMAKE_PROJECT_NAME}
        PUBLIC
        ${MOBILEGL_COMPILE_DEF}
        MOBILEGL_LOG_ACTIVE_LEVEL=${MOBILEGL_LOG_ACTIVE_LEVEL}
        $<$<BOOL:${MOBILEGL_TRACE_ANGLE_VARIANTS}>:MOBILEGL_TRACE_ANGLE_VARIANTS=1>
)

if(UNIX AND NOT APPLE AND NOT ANDROID)
    foreach(MOBILEGL_LOADER_ALIAS
            libEGL.so libEGL.so.1)
        add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E create_symlink
                    "$<TARGET_FILE_NAME:${CMAKE_PROJECT_NAME}>"
                    "$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/${MOBILEGL_LOADER_ALIAS}"
            COMMENT "Creating ${MOBILEGL_LOADER_ALIAS} alias for Linux GL/EGL loaders"
        )
    endforeach()
endif()

if(WIN32)
    # Drop-in for the classic GL loader path: a copy named opengl32.dll placed
    # next to a host executable is what LoadLibrary("opengl32.dll") and gdi32's
    # pixel-format forwarding will resolve.
    add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:${CMAKE_PROJECT_NAME}>"
                "$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/opengl32.dll"
        COMMENT "Creating opengl32.dll drop-in copy"
    )
endif()

if(NOT ANDROID)
    add_library(${CMAKE_PROJECT_NAME}_s STATIC 
        ${SOURCE_FILES}
    )

    if (CMAKE_BUILD_TYPE STREQUAL "Debug")
        set_target_properties(${CMAKE_PROJECT_NAME}_s PROPERTIES
            C_VISIBILITY_PRESET default
            CXX_VISIBILITY_PRESET default
            VISIBILITY_INLINES_HIDDEN OFF
        )
    else()
        set_target_properties(${CMAKE_PROJECT_NAME}_s PROPERTIES
            C_VISIBILITY_PRESET hidden
            CXX_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN ON
        )
    endif()

    target_include_directories(${CMAKE_PROJECT_NAME}_s PUBLIC
        ${MOBILEGL_INCLUDE_DIR}
    )

    target_link_libraries(${CMAKE_PROJECT_NAME}_s
        PUBLIC
        ${MOBILEGL_LINK_LIBRARIES}
    )

    target_compile_definitions(${CMAKE_PROJECT_NAME}_s
            PUBLIC
            ${MOBILEGL_COMPILE_DEF}
            MOBILEGL_LOG_ACTIVE_LEVEL=${MOBILEGL_LOG_ACTIVE_LEVEL}
    )
endif()

if (TRACY_ENABLE)
    target_link_libraries(${CMAKE_PROJECT_NAME} PUBLIC Tracy::TracyClient)
    target_link_libraries(${CMAKE_PROJECT_NAME}_s PUBLIC Tracy::TracyClient)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PUBLIC -DTRACY_ENABLE)
    target_compile_definitions(${CMAKE_PROJECT_NAME}_s PUBLIC -DTRACY_ENABLE)
endif ()

if (ANDROID)
    target_link_libraries(${CMAKE_PROJECT_NAME} PUBLIC
        android
        log
        vulkan
    )
endif()

if (APPLE AND NOT MOBILEGL_IOS)
    # MobileGL statically embeds glslang, SPIRV-Tools, and SPIRV-Cross. When
    # this dylib is injected with DYLD_INSERT_LIBRARIES, exporting those C++
    # symbols interposes incompatible copies embedded by host libraries such
    # as shaderc. Keep only the public GL/EGL/CGL loader surface globally
    # visible; GetProcAddress can still return pointers to hidden internals.
    set(MOBILEGL_MACOS_EXPORTED_SYMBOLS
        "${CMAKE_CURRENT_SOURCE_DIR}/MobileGL/MG_Impl/DyldInterpose/ExportedSymbols.txt")
    target_link_options(${CMAKE_PROJECT_NAME} PRIVATE
        "LINKER:-exported_symbols_list,${MOBILEGL_MACOS_EXPORTED_SYMBOLS}")
    set_property(TARGET ${CMAKE_PROJECT_NAME} APPEND PROPERTY
        LINK_DEPENDS "${MOBILEGL_MACOS_EXPORTED_SYMBOLS}")

    target_link_libraries(${CMAKE_PROJECT_NAME} PUBLIC
        "-framework Cocoa"
        "-framework CoreVideo"
        "-framework QuartzCore"
        "-framework Foundation"
        "-framework OpenGL"
        objc)
    if(TARGET ${CMAKE_PROJECT_NAME}_s)
        target_link_libraries(${CMAKE_PROJECT_NAME}_s PUBLIC
            "-framework Cocoa"
            "-framework CoreVideo"
            "-framework QuartzCore"
            "-framework Foundation"
            "-framework OpenGL"
            objc)
    endif()
endif()

if (APPLE AND MOBILEGL_IOS)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PUBLIC MOBILEGL_IOS=1 _LIBCPP_DISABLE_AVAILABILITY)
    target_link_libraries(${CMAKE_PROJECT_NAME} PUBLIC
        "-framework CoreGraphics"
        "-framework Foundation"
        "-framework QuartzCore"
        objc)
    if (MOBILEGL_VULKAN_LIBRARY)
        target_link_libraries(${CMAKE_PROJECT_NAME} PUBLIC "${MOBILEGL_VULKAN_LIBRARY}")
    endif()

    if(TARGET ${CMAKE_PROJECT_NAME}_s)
        target_compile_definitions(${CMAKE_PROJECT_NAME}_s PUBLIC MOBILEGL_IOS=1 _LIBCPP_DISABLE_AVAILABILITY)
        target_link_libraries(${CMAKE_PROJECT_NAME}_s PUBLIC
            "-framework CoreGraphics"
            "-framework Foundation"
            "-framework QuartzCore"
            objc)
        if (MOBILEGL_VULKAN_LIBRARY)
            target_link_libraries(${CMAKE_PROJECT_NAME}_s PUBLIC "${MOBILEGL_VULKAN_LIBRARY}")
        endif()
    endif()
endif()

if (NOT ANDROID AND NOT MOBILEGL_IOS)
    find_package(Vulkan)
    if (Vulkan_FOUND)
        target_link_libraries(${CMAKE_PROJECT_NAME} PUBLIC Vulkan::Vulkan Vulkan::Headers)
        target_link_libraries(${CMAKE_PROJECT_NAME}_s PUBLIC Vulkan::Vulkan Vulkan::Headers)
        target_include_directories(${CMAKE_PROJECT_NAME} PUBLIC ${Vulkan_INCLUDE_DIR})
        target_include_directories(${CMAKE_PROJECT_NAME}_s PUBLIC ${Vulkan_INCLUDE_DIR})
    endif ()
endif ()

if (NOT ANDROID)
    # Enable testing in the top-level scope so a CTestTestfile.cmake is emitted
    # at the build-tree root. This lets `ctest` be invoked from the top-level
    # build directory (IDE "run all tests", CI) and discover every test in the
    # subdirectories below, instead of having to descend into each
    # MG_Test/MG_Benchmark subdirectory. Tests are tagged with CTest labels
    # (unit / benchmark / integration), so e.g. `ctest -L unit` selects just
    # the unit suite.
    enable_testing()

    if (MOBILEGL_BUILD_TEST)
        add_subdirectory(MobileGL/MG_Test)
    endif()

    # After MG_Test so googletest is already available when the unit tests are
    # built; the module fetches its own copy when they are not.
    if (MOBILEGL_BUILD_INTEGRATION_TEST)
        add_subdirectory(MobileGL/MG_IntegrationTest)
    endif()

    if (MOBILEGL_BUILD_BENCHMARK)
        add_subdirectory(MobileGL/MG_Benchmark)
    endif()

    if (MOBILEGL_BUILD_TRACE_REPLAY)
        add_subdirectory(tools/trace_replay)
    endif()
endif()

# The integration binary is also useful as a standalone adb-shell executable.
# Android cannot use the desktop-only MobileGL_s target, so its CMake module
# links libMobileGL.so and creates an AImageReader-backed window instead.
if (ANDROID AND MOBILEGL_BUILD_INTEGRATION_TEST)
    add_subdirectory(MobileGL/MG_IntegrationTest)
endif()
