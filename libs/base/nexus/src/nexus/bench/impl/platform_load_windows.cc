#include <clean-core/platform/win32_sanitized.hh>
#include <nexus/bench/impl/platform_load.hh>

using namespace cc::primitive_defines;

namespace
{
u64 to_u64(FILETIME const& t)
{
    return (u64(t.dwHighDateTime) << 32) | u64(t.dwLowDateTime);
}

// The previous reading, so a fraction can be taken between two of them.
// A benchmark run is single-threaded and exclusive, so plain globals are enough here.
u64 s_prev_idle = 0;
u64 s_prev_total = 0;
bool s_have_prev = false;
} // namespace

f64 nx::bench::impl::sample_cpu_busy_fraction()
{
    FILETIME idle = {};
    FILETIME kernel = {};
    FILETIME user = {};
    if (GetSystemTimes(&idle, &kernel, &user) == 0)
        return -1;

    // Kernel time INCLUDES idle time on Windows, which is the trap here: subtracting idle from the sum would
    // double-count it and report a machine busier than it is.
    auto const idle_ticks = to_u64(idle);
    auto const total_ticks = to_u64(kernel) + to_u64(user);

    if (!s_have_prev)
    {
        s_prev_idle = idle_ticks;
        s_prev_total = total_ticks;
        s_have_prev = true;
        return -1; // nothing to compare against yet
    }

    auto const d_idle = idle_ticks - s_prev_idle;
    auto const d_total = total_ticks - s_prev_total;
    s_prev_idle = idle_ticks;
    s_prev_total = total_ticks;

    if (d_total == 0)
        return -1;

    return 1.0 - (f64(d_idle) / f64(d_total));
}

bool nx::bench::impl::pin_current_thread_to_core(int core)
{
    if (core < 0 || core >= 64)
        return false; // one affinity mask covers one processor group, and 64 is its width

    auto const mask = DWORD_PTR(1) << core;
    return SetThreadAffinityMask(GetCurrentThread(), mask) != 0;
}

void nx::bench::impl::unpin_current_thread()
{
    // Every processor in the group, which is what an unpinned thread had to begin with.
    auto process_mask = DWORD_PTR(0);
    auto system_mask = DWORD_PTR(0);
    if (GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask) != 0)
        (void)SetThreadAffinityMask(GetCurrentThread(), process_mask);
}
