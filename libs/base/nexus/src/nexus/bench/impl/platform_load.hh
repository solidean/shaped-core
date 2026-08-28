#pragma once

#include <nexus/bench/fwd.hh>

// The two things about the machine that need an OS to answer, behind one seam.
//
// Same shape as the hardware-counter backends next door: one implementation per platform, picked in CMake, and a
// do-nothing one everywhere else so the API is always present rather than conditionally compiled away.

namespace nx::bench::impl
{
/// The share of every core busy across the machine since the last call, or negative where the platform cannot say.
///
/// **Stateful, and the first call returns negative**: CPU time is a running total, so a fraction needs two readings
/// and the first one has nothing to compare against.
[[nodiscard]] f64 sample_cpu_busy_fraction();

/// Bind the calling thread to one core, reporting whether the OS accepted it.
[[nodiscard]] bool pin_current_thread_to_core(int core);

/// Release any binding this thread has.
void unpin_current_thread();
} // namespace nx::bench::impl
