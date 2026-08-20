cmake_minimum_required(VERSION 3.24)

# Deliberately NOT a google-benchmark target: the interesting quantity is a per-stage
# breakdown of one program build, which needs its own clock around sub-steps that share
# set-up, and a plain main() keeps the output a table this can be read straight out of.
add_executable(
        TranspileProfile
        TranspileProfile.cpp
)

target_include_directories(TranspileProfile PRIVATE
        ${MGL_ROOT}/include
        ${MGL_ROOT}/MobileGL
        ${MGL_ROOT}/3rdparty/SPIRV-Reflect
)

target_link_libraries(
        TranspileProfile PRIVATE
        ${LINK_LIBRARIES}
)
