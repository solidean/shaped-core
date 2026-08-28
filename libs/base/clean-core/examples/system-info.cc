#include <clean-core/platform/network_devices.hh>
#include <clean-core/platform/process_metrics.hh>
#include <clean-core/platform/resource_limits.hh>
#include <clean-core/platform/storage_devices.hh>
#include <clean-core/platform/system_info.hh>
#include <clean-core/platform/system_metrics.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>
#include <clean-core/thread/thread.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// A one-shot dump of everything cc can say about this machine.
//
// It prints what is ABSENT as absent rather than skipping it, because the platform gaps are the interesting part:
// a field that silently disappears reads as a field nobody wanted.
// This is the thing to paste into an issue when a platform reports something strange.

namespace
{
cc::string bytes_as_text(i64 value)
{
    constexpr cc::string_view k_units[] = {"B", "KiB", "MiB", "GiB", "TiB"};

    auto scaled = f64(value);
    auto unit = 0;
    while (scaled >= 1024.0 && unit + 1 < 5)
    {
        scaled /= 1024.0;
        ++unit;
    }
    return unit == 0 ? cc::format("{} B", value) : cc::format("{:.1f} {}", scaled, k_units[unit]);
}

void print_text(cc::string_view label, cc::string_view value)
{
    if (value.empty())
        cc::println("  {:<16} (unavailable)", label);
    else
        cc::println("  {:<16} {}", label, value);
}

void print_bytes(cc::string_view label, cc::optional<i64> value)
{
    if (!value.has_value())
        cc::println("  {:<16} (unavailable)", label);
    else
        cc::println("  {:<16} {}", label, bytes_as_text(value.value()));
}

cc::string_view cache_kind_name(cc::cache_kind kind)
{
    switch (kind)
    {
    case cc::cache_kind::data:
        return "data";
    case cc::cache_kind::instruction:
        return "instr";
    case cc::cache_kind::unified:
        return "unified";
    }
    return "?";
}
} // namespace

EXAMPLE("clean-core/system-info")
{
    auto const& info = cc::get_system_info();

    cc::println("CPU");
    print_text("brand", info.cpu_brand);
    print_text("vendor", info.cpu_vendor);
    print_text("architecture", info.cpu_architecture);
    cc::println("  {:<16} {} logical, {} physical", "totals", info.logical_cores(), info.physical_cores());

    if (info.core_classes.empty())
        cc::println("  {:<16} (unavailable)", "core classes");

    for (auto const& c : info.core_classes)
    {
        auto const name = c.name.empty() ? cc::string_view("cores") : cc::string_view(c.name);
        cc::println("  {:<16} {} physical / {} logical", name, c.physical_cores, c.logical_cores);

        if (c.base_clock_hz.has_value())
            cc::println("    {:<14} {:.2f} GHz", "base clock", f64(c.base_clock_hz.value()) / 1e9);
        if (c.boost_clock_hz.has_value())
            cc::println("    {:<14} {:.2f} GHz", "boost clock", f64(c.boost_clock_hz.value()) / 1e9);

        if (c.caches.empty())
            cc::println("    {:<14} (unavailable)", "caches");

        for (auto const& cache : c.caches)
            cc::println("    L{} {:<11} {:>9}, {} B lines, shared by {}", cache.level, cache_kind_name(cache.kind),
                        bytes_as_text(cache.size_bytes), cache.line_size_bytes, cache.sharing_cores);
    }

    cc::println("");
    cc::println("memory");
    print_bytes("total", info.ram_total_bytes);
    print_bytes("page size", info.page_size_bytes);
    if (info.ram_speed_mts.has_value())
        cc::println("  {:<16} {} MT/s", "speed", info.ram_speed_mts.value());
    else
        cc::println("  {:<16} (unavailable)", "speed");

    if (info.numa_nodes.empty())
        cc::println("  {:<16} (unavailable)", "numa nodes");
    for (auto const& node : info.numa_nodes)
    {
        if (node.memory_bytes.has_value())
            cc::println("  numa node {:<6} {}", node.index, bytes_as_text(node.memory_bytes.value()));
        else
            cc::println("  numa node {:<6} (size unavailable)", node.index);
    }

    cc::println("");
    cc::println("limits");
    auto const limits = cc::query_resource_limits();
    if (limits.cpu_quota.has_value())
        cc::println("  {:<16} {:.2f} CPUs", "cpu quota", limits.cpu_quota.value());
    else
        cc::println("  {:<16} (none)", "cpu quota");
    print_bytes("memory limit", limits.memory_limit_bytes);
    cc::println("  {:<16} {}", "affinity", limits.affinity_cores);
    cc::println("  {:<16} {}", "containerized", limits.containerized ? "yes" : "no");
    cc::println("  {:<16} {}", "hypervisor", limits.hypervisor_present ? "present" : "none");
    cc::println("  {:<16} {}", "workers", cc::recommended_worker_count());

    cc::println("");
    cc::println("live");
    auto const memory = cc::query_memory_usage();
    if (memory.has_value())
    {
        auto const& m = memory.value();
        cc::println("  {:<16} {} / {} ({:.0f}%)", "memory in use", bytes_as_text(m.used_bytes),
                    bytes_as_text(m.total_bytes), 100.0f * m.used_ratio());
        if (m.swap_used_bytes.has_value() && m.swap_total_bytes.has_value())
            cc::println("  {:<16} {} / {}", "swap in use", bytes_as_text(m.swap_used_bytes.value()),
                        bytes_as_text(m.swap_total_bytes.value()));
        if (m.commit_used_bytes.has_value() && m.commit_limit_bytes.has_value())
            cc::println("  {:<16} {} / {}", "commit charge", bytes_as_text(m.commit_used_bytes.value()),
                        bytes_as_text(m.commit_limit_bytes.value()));
    }
    else
    {
        cc::println("  {:<16} (unavailable: {})", "memory in use", memory.error().detail);
    }

    // A load is a rate, so it needs an interval to be a load at all — hence a sampler and a wait rather than a call.
    auto sampler = cc::cpu_load_sampler();
    cc::this_thread_sleep_secs(0.25);
    auto const load = sampler.sample();
    if (load.has_value())
    {
        cc::println("  {:<16} {:.0f}% ({:.1f} cores) over {:.2f} s", "cpu load", 100.0f * load.value().total,
                    load.value().cores_used, load.value().interval_secs);

        auto per_core = cc::string();
        for (auto const core : load.value().per_core)
            per_core += cc::format("{:>4.0f}", 100.0f * core);
        if (!per_core.empty())
            cc::println("  {:<16}{}", "per core %", per_core);
    }
    else
    {
        cc::println("  {:<16} (unavailable: {})", "cpu load", load.error().detail);
    }

    cc::println("");
    cc::println("storage");
    auto const mounts = cc::query_mounts();
    if (mounts.has_value())
        for (auto const& m : mounts.value())
            cc::println("  {:<16} {} free of {} ({}{})", m.path, bytes_as_text(m.available_bytes),
                        bytes_as_text(m.total_bytes),
                        m.filesystem.empty() ? cc::string_view("?") : cc::string_view(m.filesystem),
                        m.removable ? ", removable" : "");
    else
        cc::println("  {:<16} (unavailable: {})", "mounts", mounts.error().detail);

    auto const disks = cc::enumerate_disks();
    if (disks.empty())
        cc::println("  {:<16} (unavailable)", "devices");

    for (auto const& d : disks)
    {
        auto const capacity = d.capacity_bytes.has_value() ? bytes_as_text(d.capacity_bytes.value()) : cc::string("?");
        cc::println("  {:<18} {} ({})", d.id, capacity,
                    d.model.empty() ? cc::string_view("?") : cc::string_view(d.model));

        if (auto const total = cc::read_disk_io_counters(d.id); total.has_value())
            cc::println("    {:<14} {} read, {} written since boot", "totals", bytes_as_text(total.value().bytes_read),
                        bytes_as_text(total.value().bytes_written));

        auto sampler = cc::disk_io_sampler(d.id);
        cc::this_thread_sleep_secs(0.05);
        auto const rate = sampler.sample();
        if (rate.has_value())
            cc::println("    {:<14} {}/s read, {}/s written{}", "io", bytes_as_text(i64(rate.value().read_bytes_per_sec)),
                        bytes_as_text(i64(rate.value().write_bytes_per_sec)),
                        rate.value().busy_fraction.has_value()
                            ? cc::format(", {:.0f}% busy", 100.0f * rate.value().busy_fraction.value())
                            : cc::string());
        else
            cc::println("    {:<14} (unavailable: {})", "io", rate.error().detail);
    }

    cc::println("");
    cc::println("network");
    auto const interfaces = cc::enumerate_network_interfaces();
    if (interfaces.empty())
        cc::println("  {:<18} (unavailable)", "interfaces");

    // Only the ones carrying traffic get a line; a desktop has dozens of virtual adapters that would bury them.
    auto quiet = 0;
    for (auto const& n : interfaces)
    {
        auto const counters = cc::read_net_counters(n.id);
        if (!n.is_up || !counters.has_value() || counters.value().bytes_received + counters.value().bytes_sent == 0)
        {
            ++quiet;
            continue;
        }

        auto const speed = n.link_speed_bps.has_value() ? cc::format("{} Mbit/s", n.link_speed_bps.value() / 1'000'000)
                                                        : cc::string("? Mbit/s");
        cc::println("  {:<18} {}{}", n.id, speed, n.is_loopback ? ", loopback" : "");
        cc::println("    {:<14} {} in, {} out since boot", "totals", bytes_as_text(counters.value().bytes_received),
                    bytes_as_text(counters.value().bytes_sent));
    }

    if (quiet > 0)
        cc::println("  {:<18} {} idle or down", "(other)", quiet);

    cc::println("");
    cc::println("this process");
    auto const self = cc::query_process_usage();
    if (self.has_value())
    {
        auto const& p = self.value();
        cc::println("  {:<16} {} (peak {})", "resident", bytes_as_text(p.resident_bytes),
                    bytes_as_text(p.peak_resident_bytes));
        cc::println("  {:<16} {}", "private", bytes_as_text(p.private_bytes));
        cc::println("  {:<16} {}", "threads", p.thread_count);
        if (p.open_handles.has_value())
            cc::println("  {:<16} {}", "open handles", p.open_handles.value());
        else
            cc::println("  {:<16} (unavailable)", "open handles");
    }
    else
    {
        cc::println("  {:<16} (unavailable: {})", "resident", self.error().detail);
    }

    auto const own = cc::read_process_cpu_counters();
    if (own.has_value())
    {
        cc::println("  {:<16} {:.3f} s user, {:.3f} s kernel", "cpu time", own.value().user_secs,
                    own.value().kernel_secs);
        if (own.value().bytes_read.has_value() && own.value().bytes_written.has_value())
            cc::println("  {:<16} {} read, {} written", "io", bytes_as_text(own.value().bytes_read.value()),
                        bytes_as_text(own.value().bytes_written.value()));
    }

    cc::println("");
    cc::println("OS");
    print_text("name", info.os_name);
    print_text("version", info.os_version);
    print_text("build", info.os_build);
    print_text("kernel", info.kernel_version);
    print_text("timezone", info.timezone_at_start);
    print_text("locale", info.locale_at_start);

    if (info.boot_time_wall_secs > 0)
        cc::println("  {:<16} {:.1f} h", "uptime", info.uptime_secs() / 3600.0);
    else
        cc::println("  {:<16} (unavailable)", "uptime");
}
