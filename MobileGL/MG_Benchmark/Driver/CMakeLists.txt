cmake_minimum_required(VERSION 3.24)

# A real, headless EGL client, deliberately NOT linked against MobileGL: it
# dlopens one EGL provider at runtime ($DRIVERBENCH_EGL_LIB - the system
# libEGL.so.1 for the native driver, or a libMobileGL.so path for either
# MobileGL backend), so the same binary measures all three stacks.
if (NOT UNIX OR APPLE OR ANDROID)
    return()
endif()

add_executable(DriverBench DriverBench.c)
target_link_libraries(DriverBench PRIVATE dl)

add_test(NAME DriverBench COMMAND DriverBench draw_tiny)
set_tests_properties(DriverBench PROPERTIES LABELS benchmark)
