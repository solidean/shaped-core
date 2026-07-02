#pragma once

#include <clean-core/fwd.hh>
#include <clean-core/string/string_view.hh>

namespace cc
{
/// Sets the calling thread's OS name (as seen in debuggers and profilers). name is UTF-8.
/// Best-effort: silently truncated where the platform is stricter (15 bytes on Linux) and a
/// no-op where thread naming is unavailable (e.g. single-threaded WebAssembly).
void set_current_thread_name(string_view name);
} // namespace cc
