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

# The Blaze3D blend toggle, as its own entry.
#
# mc_state_toggle is glEnable(GL_BLEND) / glBlendFuncSeparate / glDrawElements /
# glDisable(GL_BLEND) / glDrawElements, 46 times - the measured vanilla-frame rate, and the exact
# shape ROADMAP.md writes down as the microbenchmark P2 owes the GO/NO-GO. It is the workload the
# whole "push at validate, not in the setter" decision was made for: a per-setter design pays for
# every toggle, and a CSO that is minted twice and then reused pays for none of them.
#
# The case has existed in kBenchCases since P0 and nothing ran it, so nothing noticed if it broke.
# Exposing it costs about 1.2 s inside an existing three-minute job, and it means the number the
# P2 report quotes comes from a case CI has been executing all along rather than from a code path
# whose first run is the day it is measured.
#
# Like the entry above, this runs against whatever $DRIVERBENCH_EGL_LIB names (the system driver
# when unset) - the ctest entry is a "does this case still run" gate, not the measurement. The
# measurement is run_driver_bench.sh against each of {native, espryt, magma}.
add_test(NAME DriverBenchStateToggle COMMAND DriverBench mc_state_toggle)
set_tests_properties(DriverBenchStateToggle PROPERTIES LABELS benchmark)
