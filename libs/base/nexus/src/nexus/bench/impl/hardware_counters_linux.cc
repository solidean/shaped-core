#include <clean-core/container/fixed_vector.hh>
#include <linux/perf_event.h>
#include <nexus/bench/impl/baseline.hh>
#include <nexus/bench/impl/hardware_counters_backend.hh>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

// Linux backend: hardware counters via perf_event_open(2).
//
// The PMU counters are opened as one event group (a leader plus members sharing its group_fd) so a single
// read() returns all of them consistently. Access is gated by /proc/sys/kernel/perf_event_paranoid and is
// commonly blocked inside containers/sandboxes; when opening fails we degrade to the baseline (elapsed time
// + reference cycles) and warn once. We measure user space only (exclude_kernel/exclude_hv).
//
// reference_cycles stays the rdtsc baseline (not a perf event) so it matches the other platforms and is
// always available regardless of perf access.

namespace nx::bench::impl
{
namespace
{
// The perf event backing one logical PMU counter, in `perf list` spelling.
struct perf_event_desc
{
    hw_counter id;
    u32 type;   // perf_type_id
    u64 config; // event selector (PERF_COUNT_* or the HW_CACHE triple)
    char const* name;
};

constexpr u64 hw_cache_config(u64 cache, u64 op, u64 result)
{
    return cache | (op << 8) | (result << 16);
}

// The PMU counters this backend knows how to open. reference_cycles/elapsed_nanoseconds are baseline, not
// here. L2 as a distinct counter is not portable in generic perf and is left for a future raw-event pass.
constexpr perf_event_desc s_pmu_events[] = {
    {hw_counter::instructions_retired, PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS, "instructions"},
    {hw_counter::branch_instructions, PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_INSTRUCTIONS, "branch-instructions"},
    {hw_counter::branch_misses, PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES, "branch-misses"},
    {hw_counter::cache_l1d_misses, PERF_TYPE_HW_CACHE,
     hw_cache_config(PERF_COUNT_HW_CACHE_L1D, PERF_COUNT_HW_CACHE_OP_READ, PERF_COUNT_HW_CACHE_RESULT_MISS),
     "L1-dcache-load-misses"},
    {hw_counter::cache_llc_references, PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_REFERENCES, "cache-references"},
    {hw_counter::cache_llc_misses, PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES, "cache-misses"},
};

constexpr auto s_max_pmu = isize(sizeof(s_pmu_events) / sizeof(s_pmu_events[0]));

bool is_baseline(hw_counter c)
{
    return c == hw_counter::elapsed_nanoseconds || c == hw_counter::reference_cycles;
}

perf_event_desc const* find_event(hw_counter c)
{
    for (auto const& e : s_pmu_events)
        if (e.id == c)
            return &e;
    return nullptr;
}

long perf_event_open(perf_event_attr* attr, pid_t pid, int cpu, int group_fd, unsigned long flags)
{
    return ::syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

// Open one perf event for the current thread (user space only), optionally joining `group_fd`'s group.
// Returns the fd, or -1 on failure. The leader (group_fd == -1) is created disabled.
int open_event(perf_event_desc const& e, int group_fd)
{
    auto attr = perf_event_attr{};
    attr.size = sizeof(attr);
    attr.type = e.type;
    attr.config = e.config;
    attr.disabled = group_fd == -1 ? 1 : 0;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    attr.read_format
        = PERF_FORMAT_GROUP | PERF_FORMAT_ID | PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;

    return int(perf_event_open(&attr, 0, -1, group_fd, 0));
}
} // namespace

cc::vector<backend_counter> backend_enumerate_counters()
{
    cc::vector<backend_counter> out;
    out.push_back({.id = hw_counter::elapsed_nanoseconds, .available = true});
    out.push_back({.id = hw_counter::reference_cycles, .available = has_reference_cycles()});

    // Availability = can we actually open it right now (paranoid level / sandbox permitting).
    for (auto const& e : s_pmu_events)
    {
        auto const fd = open_event(e, -1);
        auto const ok = fd >= 0;
        if (ok)
            ::close(fd);
        out.push_back({.id = e.id, .native_name = cc::string(e.name), .available = ok});
    }
    return out;
}

cc::vector<hw_counter_sample> backend_measure(cc::function_ref<void()> body, cc::span<hw_counter const> counters)
{
    // Open a group for every requested PMU counter; the first successful open is the group leader.
    cc::fixed_vector<int, s_max_pmu> fds;
    cc::fixed_vector<u64, s_max_pmu> ids;          // perf id per fd, to match read-back values
    cc::fixed_vector<hw_counter, s_max_pmu> which; // logical counter per fd
    auto leader_fd = -1;
    auto pmu_requested = false;

    for (auto const c : counters)
    {
        if (is_baseline(c))
            continue;
        pmu_requested = true;

        auto const* e = find_event(c);
        if (e == nullptr)
            continue;

        auto const fd = open_event(*e, leader_fd);
        if (fd < 0)
            continue; // degrade this counter; keep any that did open

        if (leader_fd == -1)
            leader_fd = fd;

        auto id = u64(0);
        ::ioctl(fd, PERF_EVENT_IOC_ID, &id);

        fds.push_back(fd);
        ids.push_back(id);
        which.push_back(c);
    }

    auto const have_group = leader_fd != -1;
    if (have_group)
    {
        ::ioctl(leader_fd, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
        ::ioctl(leader_fd, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
    }

    // Bracket the single invocation as tightly as possible.
    auto const cycles_begin = read_reference_cycles();
    auto const ns_begin = steady_now_ns();
    body();
    auto const ns_end = steady_now_ns();
    auto const cycles_end = read_reference_cycles();

    // Read the whole group in one shot: { nr, time_enabled, time_running, {value,id} * nr }.
    u64 raw[3 + 2 * s_max_pmu] = {};
    auto group_ok = false;
    if (have_group)
    {
        ::ioctl(leader_fd, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
        group_ok = ::read(leader_fd, raw, sizeof(raw)) > 0;
    }

    // Resolve a scaled value for a given perf id, or fall back to invalid.
    auto value_for_id = [&](u64 id, u64& out_value) -> bool
    {
        if (!group_ok)
            return false;
        auto const nr = raw[0];
        auto const time_enabled = raw[1];
        auto const time_running = raw[2];
        for (u64 i = 0; i < nr; ++i)
        {
            auto const value = raw[3 + 2 * i + 0];
            auto const entry_id = raw[3 + 2 * i + 1];
            if (entry_id != id)
                continue;
            if (time_running == 0)
                return false; // event never got to run
            // Scale up when the group was multiplexed (running < enabled).
            out_value = time_running < time_enabled ? u64(double(value) * double(time_enabled) / double(time_running))
                                                    : value;
            return true;
        }
        return false;
    };

    if (pmu_requested && !group_ok)
        warn_pmu_unavailable_once("perf_event_open failed (check /proc/sys/kernel/perf_event_paranoid, or a "
                                  "container/sandbox may block it).");

    cc::vector<hw_counter_sample> out;
    out.reserve(counters.size());
    for (auto const c : counters)
    {
        auto const name = cc::string(logical_counter_name(c));
        if (c == hw_counter::elapsed_nanoseconds)
        {
            out.push_back({.id = c, .name = name, .value = ns_end - ns_begin, .valid = true});
            continue;
        }
        if (c == hw_counter::reference_cycles)
        {
            out.push_back({.id = c, .name = name, .value = cycles_end - cycles_begin, .valid = has_reference_cycles()});
            continue;
        }

        // A PMU counter: locate its fd, use its native name, and pull the scaled value.
        auto value = u64(0);
        auto valid = false;
        auto native = name;
        for (auto i = isize(0); i < fds.size(); ++i)
        {
            if (which[i] != c)
                continue;
            if (auto const* e = find_event(c))
                native = cc::string(e->name);
            valid = value_for_id(ids[i], value);
            break;
        }
        out.push_back({.id = c, .name = native, .value = value, .valid = valid});
    }

    for (auto const fd : fds)
        ::close(fd);

    return out;
}
} // namespace nx::bench::impl
