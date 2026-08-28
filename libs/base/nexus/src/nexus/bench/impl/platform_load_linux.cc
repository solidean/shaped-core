#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/bench/impl/platform_load.hh>
#include <sched.h>

using namespace cc::primitive_defines;

namespace
{
// The previous reading, so a fraction can be taken between two of them.
// A benchmark run is single-threaded and exclusive, so plain globals are enough here.
u64 s_prev_idle = 0;
u64 s_prev_total = 0;
bool s_have_prev = false;

// The first line of /proc/stat: "cpu  user nice system idle iowait irq softirq steal ...", in USER_HZ ticks.
// Read rather than parsed properly, because only two sums are wanted and the field order is fixed by the kernel.
bool read_proc_stat(u64& idle_out, u64& total_out)
{
    auto stream = cc::open_file_read("/proc/stat");
    if (!stream.has_value())
        return false;

    char buffer[512] = {};
    auto const read = stream.value().read(cc::span<byte>(reinterpret_cast<byte*>(buffer), sizeof(buffer) - 1));
    if (read <= 0)
        return false;

    auto line = cc::string_view(buffer, read);
    if (!line.starts_with("cpu "))
        return false;

    auto total = u64(0);
    auto idle = u64(0);
    auto field = isize(0);
    auto value = u64(0);
    auto in_number = false;

    for (auto i = isize(4); i < isize(line.size()); ++i)
    {
        auto const c = line[i];
        if (c >= '0' && c <= '9')
        {
            value = value * 10 + u64(c - '0');
            in_number = true;
            continue;
        }

        if (in_number)
        {
            total += value;
            // Fields 3 and 4 are idle and iowait, both of which are the machine doing nothing.
            if (field == 3 || field == 4)
                idle += value;
            ++field;
            value = 0;
            in_number = false;
        }

        if (c == '\n')
            break;
    }

    idle_out = idle;
    total_out = total;
    return total > 0;
}
} // namespace

f64 nx::bench::impl::sample_cpu_busy_fraction()
{
    auto idle = u64(0);
    auto total = u64(0);
    if (!read_proc_stat(idle, total))
        return -1;

    if (!s_have_prev)
    {
        s_prev_idle = idle;
        s_prev_total = total;
        s_have_prev = true;
        return -1; // nothing to compare against yet
    }

    auto const d_idle = idle - s_prev_idle;
    auto const d_total = total - s_prev_total;
    s_prev_idle = idle;
    s_prev_total = total;

    if (d_total == 0)
        return -1;

    return 1.0 - (f64(d_idle) / f64(d_total));
}

namespace
{
// What the thread was allowed to run on before it was pinned, so unpinning restores that rather than widening.
//
// Every CPU in the system is the wrong thing to restore: under a cpuset — a container, a cgroup, a `taskset` — asking
// for cores the process may not have fails, the thread stays pinned, and nothing says so.
// Captured on the pin rather than read back in unpin, where the thread's mask is already the pin.
cpu_set_t g_mask_before_pin;
bool g_captured_mask_before_pin = false;
} // namespace

bool nx::bench::impl::pin_current_thread_to_core(int core)
{
    if (core < 0 || core >= CPU_SETSIZE)
        return false;

    g_captured_mask_before_pin = sched_getaffinity(0, sizeof(g_mask_before_pin), &g_mask_before_pin) == 0;

    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    return sched_setaffinity(0, sizeof(set), &set) == 0;
}

void nx::bench::impl::unpin_current_thread()
{
    if (!g_captured_mask_before_pin)
        return;

    (void)sched_setaffinity(0, sizeof(g_mask_before_pin), &g_mask_before_pin);
    g_captured_mask_before_pin = false;
}
