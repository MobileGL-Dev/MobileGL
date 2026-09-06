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

# WHY EVERY ENTRY HERE CARRIES A PASS_REGULAR_EXPRESSION.
#
# DriverBench prints one CSV row per case it ran and exits 0 whatever it ran. Before this, a ctest
# entry naming a case therefore could not answer the only question it exists to ask: an argument
# matching nothing in kBenchCases selected no case, printed only the header row, and still exited
# 0. DriverBench.c now refuses an unknown case name (exit 2), which closes it at the source - but
# the entry must be able to go red for the reason it exists WITHOUT depending on that check
# staying in the binary, so each entry also requires the case's own output row to appear.
#
# The regex is what a healthy run of that case prints and nothing else does: the case name at the
# start of a line, then the frames / ops-per-frame / median-ms / ns-per-op / fps columns
# (run_case()). A rename, a drop from kBenchCases, a boot_egl() failure or
# a crash part-way through the case all remove that row and turn the entry red.
#
# Note that a PASS_REGULAR_EXPRESSION makes ctest ignore the process exit code (cmCTestRunTest:
# success is `retVal == 0 || !RequiredRegularExpressions.empty()`), which is why the row itself
# has to be the evidence rather than a companion to the rc.
add_test(NAME DriverBench COMMAND DriverBench draw_tiny)
# draw_tiny's a/ops scale with $DRIVERBENCH_DRAWS (main()), so only the shape of
# the row is pinned here, not the column values.
set_tests_properties(DriverBench PROPERTIES
    LABELS benchmark
    PASS_REGULAR_EXPRESSION "(^|\n)draw_tiny,[0-9]+,[0-9]+,[0-9.]+,[0-9.]+,[0-9.]+")

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
# The ops-per-frame column is pinned to 46 here, unlike the entry above: the mc_* cases are
# excluded from the $DRIVERBENCH_DRAWS scaling on purpose ("the mc_* rates are measured and must
# not move, or the numbers stop being comparable", main()), so 46 toggles per frame
# is part of what "this case still runs" means. Change the workload and this entry says so.
set_tests_properties(DriverBenchStateToggle PROPERTIES
    LABELS benchmark
    PASS_REGULAR_EXPRESSION "(^|\n)mc_state_toggle,[0-9]+,46,[0-9.]+,[0-9.]+,[0-9.]+")
