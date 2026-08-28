#include <clean-core/platform/system_info.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>
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
