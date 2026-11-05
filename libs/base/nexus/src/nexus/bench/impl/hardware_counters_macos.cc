#include <clean-core/common/time.hh>
#include <nexus/bench/impl/hardware_counters_backend.hh>

// macOS backend: the two fixed counters the kernel keeps per thread, read with no privilege and no setup.
//
// On Apple silicon the kernel's monotonic subsystem counts cycles and retired instructions for every thread all the time (sysctl kern.monotonic.task_thread_counting).
// thread_selfcounts reads the calling thread's pair; the body must run on the calling thread to be counted, as on Linux.
// Unlike Linux's user-space-only events, both counts include the kernel time the thread spends, so a body heavy in syscalls counts more here.
// The configurable counters (branches, cache misses) need kperf, which the kernel gates on root, and are reported unavailable.

// Exported from libsystem_kernel but declared in no SDK header: undocumented, and stable across macOS releases since 10.13.
// Type 1 writes {instructions, cycles}; a non-zero return means the kernel keeps no counts (Intel Macs, some VMs).
extern "C" int thread_selfcounts(int type, void* buf, decltype(sizeof(0)) nbytes);

namespace nx::bench::impl
{
namespace
{
struct fixed_counts
{
    u64 instructions = 0;
    u64 cycles = 0;
};

bool read_fixed_counts(fixed_counts& out)
{
    u64 buf[2] = {};
    if (thread_selfcounts(1, buf, sizeof(buf)) != 0)
        return false;
    out = {.instructions = buf[0], .cycles = buf[1]};
    return true;
}

bool fixed_counts_readable()
{
    static auto const readable = []
    {
        auto probe = fixed_counts();
        return read_fixed_counts(probe);
    }();
    return readable;
}

// Measurable here: the baseline pair and the two fixed counters.
bool is_fixed(hw_counter c)
{
    return c == hw_counter::instructions_retired || c == hw_counter::core_cycles;
}

bool is_baseline(hw_counter c)
{
    return c == hw_counter::elapsed_nanoseconds || c == hw_counter::reference_cycles;
}

// The counters only kperf could read.
constexpr hw_counter s_configurable_counters[] = {
    hw_counter::branch_instructions,  hw_counter::branch_misses,    hw_counter::cache_l1d_misses,
    hw_counter::cache_llc_references, hw_counter::cache_llc_misses, hw_counter::cache_l1d_accesses,
};
} // namespace

cc::vector<backend_counter> backend_enumerate_counters()
{
    auto const fixed = fixed_counts_readable();
    cc::vector<backend_counter> out;
    out.push_back({.id = hw_counter::elapsed_nanoseconds, .available = true});
    out.push_back({.id = hw_counter::reference_cycles, .available = cc::has_cycle_counter()});
    out.push_back({.id = hw_counter::instructions_retired, .native_name = "FIXED_INSTRUCTIONS", .available = fixed});
    out.push_back({.id = hw_counter::core_cycles, .native_name = "FIXED_CYCLES", .available = fixed});
    for (auto const c : s_configurable_counters)
        out.push_back({.id = c, .available = false});
    return out;
}

cc::string backend_setup_hint()
{
    // No setup makes the configurable counters readable without root, so there is nothing to point at.
    return {};
}

cc::vector<hw_counter_sample> backend_measure(cc::function_ref<void()> body, cc::span<hw_counter const> counters)
{
    auto const fixed = fixed_counts_readable();
    auto unreadable_requested = false;
    for (auto const c : counters)
        if (!is_baseline(c) && !(is_fixed(c) && fixed))
            unreadable_requested = true;

    // The fixed counts innermost, so the baseline's own reads stay out of them.
    auto counts_begin = fixed_counts();
    auto counts_end = fixed_counts();
    auto const cycles_begin = cc::current_cycles();
    auto const secs_begin = cc::current_time_steady_secs();
    auto const began = fixed && read_fixed_counts(counts_begin);
    body();
    auto const ended = began && read_fixed_counts(counts_end);
    auto const secs_end = cc::current_time_steady_secs();
    auto const cycles_end = cc::current_cycles();

    if (unreadable_requested)
        warn_pmu_unavailable_once("macOS reads cycles and instructions without privilege; branches and cache counters "
                                  "need kperf, which requires root and is not supported.");

    cc::vector<hw_counter_sample> out;
    out.reserve(counters.size());
    for (auto const c : counters)
    {
        auto const name = cc::string(logical_counter_name(c));
        switch (c)
        {
        case hw_counter::elapsed_nanoseconds:
            out.push_back({.id = c, .name = name, .value = u64((secs_end - secs_begin) * 1e9), .valid = true});
            break;
        case hw_counter::reference_cycles:
            out.push_back({.id = c, .name = name, .value = cycles_end - cycles_begin, .valid = cc::has_cycle_counter()});
            break;
        case hw_counter::instructions_retired:
            out.push_back({.id = c,
                           .name = "FIXED_INSTRUCTIONS",
                           .value = ended ? counts_end.instructions - counts_begin.instructions : 0,
                           .valid = ended});
            break;
        case hw_counter::core_cycles:
            out.push_back({.id = c,
                           .name = "FIXED_CYCLES",
                           .value = ended ? counts_end.cycles - counts_begin.cycles : 0,
                           .valid = ended});
            break;
        default:
            out.push_back({.id = c, .name = name, .value = 0, .valid = false});
            break;
        }
    }
    return out;
}
} // namespace nx::bench::impl
