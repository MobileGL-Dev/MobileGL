#pragma once

namespace mobilegl_trace_dump {

// Installs the framebuffer-attachment dump hook when MOBILEGL_TRACE_DUMP_FBO_ATTACHMENTS
// describes at least one dump point. Safe and cheap to call on every makeCurrent: the
// environment is consulted once and the hook is installed at most once.
void InstallIfRequested();

} // namespace mobilegl_trace_dump
