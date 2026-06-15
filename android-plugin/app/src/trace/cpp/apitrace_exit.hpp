#pragma once

struct MobileGLRetraceExit {
    int status;
};

extern "C" [[noreturn]] void mobilegl_apitrace_exit(int status);
