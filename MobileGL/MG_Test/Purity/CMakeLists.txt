cmake_minimum_required(VERSION 3.14)

# The P0.5 include-closure assertions (ROADMAP P0.5; ARCHITECTURE.md:501 gate A): no MobileGL
# compilation, no GL context. Text mode only here, because this CTestTestfile also runs on the
# `test` job's runner (test.yml:149-203), which has no submodules; the compiler-backed run is the
# include-graph-check job and the developer's local `--mode both`.
find_package(Python3 COMPONENTS Interpreter)
if (Python3_Interpreter_FOUND)
    add_test(NAME MobileGLPurity.IncludeClosure
             COMMAND ${Python3_EXECUTABLE} ${MGL_ROOT}/scripts/check_include_closure.py --mode text --self-test --require-all)
    # ctest -L unit (test.yml:198-203). NO ENVIRONMENT property: it replaces the job env (ARCHITECTURE.md:567).
    set_tests_properties(MobileGLPurity.IncludeClosure PROPERTIES LABELS unit)
else()
    message(STATUS "MobileGLPurity.IncludeClosure not registered: no python3 interpreter (Windows is not a correctness gate)")
endif()
