#pragma once

namespace mobilegl_trace_dump {

// Installs the opt-in framebuffer-attachment and named-2D-texture dump hooks. The respective
// environments are MOBILEGL_TRACE_DUMP_FBO_ATTACHMENTS and MOBILEGL_TRACE_DUMP_TEXTURE_2D.
// Safe and cheap to call on every makeCurrent: the environment is consulted once and the hook is
// installed at most once.
void InstallIfRequested();

} // namespace mobilegl_trace_dump
