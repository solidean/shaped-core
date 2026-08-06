#pragma once

#include <shaped-graphics/fwd.hh>

namespace sg
{
/// The process-global reload generation: a monotonic counter that moves whenever cached, content-derived GPU state is invalidated and should be rebuilt.
/// That is shaders, and the pipelines built from them.
/// sg owns it as the lowest common meeting point between producer and consumer.
/// A producer such as the shader library bumps it via signal_reload() on hot reload; a consumer such as a render_routine reads it to decide whether to re-run its init.
/// sg itself does not consume it: its pipeline cache is content-keyed and rebuilds on its own.
/// Only the fact that the value changed is meaningful.
[[nodiscard]] u64 reload_generation();

/// Bumps reload_generation() by one.
/// Call it after replacing content that derived GPU state was built from.
void signal_reload();
} // namespace sg
