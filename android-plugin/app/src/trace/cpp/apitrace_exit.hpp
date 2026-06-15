#pragma once

#ifdef exit
#pragma push_macro("exit")
#undef exit
#define MOBILEGL_TRACE_RESTORE_EXIT_MACRO
#endif

#include <cstdlib>
#include <stdlib.h>

#ifdef MOBILEGL_TRACE_RESTORE_EXIT_MACRO
#pragma pop_macro("exit")
#undef MOBILEGL_TRACE_RESTORE_EXIT_MACRO
#endif

struct MobileGLRetraceExit {
    int status;
};

extern "C" [[noreturn]] void mobilegl_apitrace_exit(int status);
