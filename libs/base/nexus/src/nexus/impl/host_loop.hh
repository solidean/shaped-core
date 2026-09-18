#pragma once

#include <clean-core/function/unique_function.hh>

namespace nx::impl
{
/// Whether this process runs under a host event loop that owns the thread, and so must never be blocked in: WebAssembly under Emscripten.
[[nodiscard]] bool has_host_event_loop();

/// Drives `step` from the host's event loop until it returns true, then ends the process with the code `finish` returns.
///
/// **It never returns** where `has_host_event_loop()` holds: it hands the thread back to the host, whose later callbacks run the remaining steps.
/// A WebGPU callback, a timer or a fetch completion can only run in between, which is the whole reason a test run is stepped there.
/// Everything `step` and `finish` capture must therefore live on the heap.
/// Elsewhere it must not be called.
void run_in_host_loop(cc::unique_function<bool()> step, cc::unique_function<int()> finish);
} // namespace nx::impl
