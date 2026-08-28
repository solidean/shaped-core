#include <nexus/bench/impl/platform_load.hh>

using namespace cc::primitive_defines;

// The platforms with no answer: macOS, the Apple mobile ones, Android, and the WebAssembly targets.
//
// The API is present everywhere rather than compiled away, so a caller writes the same code and reads the reported
// failure instead of a preprocessor branch.
// macOS is the interesting entry here: it exposes only ADVISORY affinity hints, which cannot honour "this thread runs
// on that core", so reporting false is more honest than setting a hint and claiming a pin.

f64 nx::bench::impl::sample_cpu_busy_fraction()
{
    return -1;
}

bool nx::bench::impl::pin_current_thread_to_core(int core)
{
    (void)core;
    return false;
}

void nx::bench::impl::unpin_current_thread()
{
}
