#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/platform/resource_snapshot.hh>
#include <clean-core/platform/system_info.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/record.hh>
#include <clean-core/record/stamp.hh>
#include <clean-core/record/writer.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/atomic.hh>

using namespace cc::primitive_defines;

namespace cc::rec
{
namespace
{
/// A fixed table rather than a growing one: contributors are registered at start-up by whole libraries, so the count is
/// the number of libraries in the program and never the number of anything dynamic.
constexpr isize k_max_contributors = 16;

struct contributor
{
    char const* name = nullptr;
    rec::stamp_provider provider = nullptr;

    /// One descriptor per contributor, filled at registration.
    /// A recording site's descriptor is normally constant-initialized; these cannot be, because the set is not known
    /// until the libraries that own them have started.
    rec::desc desc = {};
};

contributor g_contributors[k_max_contributors] = {};
cc::atomic<isize> g_contributor_count = {0};

/// The machine description, encoded once.
///
/// cc::system_info cannot change while the process runs, so its bytes are built on first use and every later stamp is a
/// memcpy of them however many recordings open.
cc::string const& system_section()
{
    static auto const encoded = []
    {
        auto const& info = cc::get_system_info();

        auto out = cc::string();
        out.appendf("cpu.brand={}\n", info.cpu_brand);
        out.appendf("cpu.vendor={}\n", info.cpu_vendor);
        out.appendf("cpu.arch={}\n", info.cpu_architecture);
        out.appendf("cpu.logical_cores={}\n", info.logical_cores());
        out.appendf("cpu.physical_cores={}\n", info.physical_cores());

        for (isize i = 0; i < info.core_classes.size(); ++i)
        {
            auto const& c = info.core_classes[i];
            auto const name = c.name.empty() ? cc::string_view("cores") : cc::string_view(c.name);
            out.appendf("cpu.class.{}.name={}\n", i, name);
            out.appendf("cpu.class.{}.physical={}\n", i, c.physical_cores);
            out.appendf("cpu.class.{}.logical={}\n", i, c.logical_cores);
        }

        if (auto const l3 = info.largest_cache_bytes(3); l3.has_value())
            out.appendf("cpu.l3_bytes={}\n", l3.value());
        if (info.ram_total_bytes.has_value())
            out.appendf("ram.total_bytes={}\n", info.ram_total_bytes.value());

        out.appendf("os.name={}\n", info.os_name);
        out.appendf("os.version={}\n", info.os_version);
        out.appendf("os.build={}\n", info.os_build);
        out.appendf("os.kernel={}\n", info.kernel_version);
        out.appendf("os.timezone_at_start={}\n", info.timezone_at_start);
        out.appendf("os.locale_at_start={}\n", info.locale_at_start);
        out.appendf("os.boot_time_wall_secs={:.0f}\n", info.boot_time_wall_secs);

        return out;
    }();
    return encoded;
}

/// The levels at this instant, encoded fresh — that is the point of taking one at open and again at close.
cc::string resources_section()
{
    auto const snapshot = cc::take_resource_snapshot();

    auto out = cc::string();
    out.appendf("at_wall_secs={:.3f}\n", snapshot.at_wall_secs);

    if (snapshot.memory.has_value())
    {
        out.appendf("memory.used_bytes={}\n", snapshot.memory.value().used_bytes);
        out.appendf("memory.available_bytes={}\n", snapshot.memory.value().available_bytes);
    }

    if (snapshot.process.has_value())
    {
        out.appendf("process.resident_bytes={}\n", snapshot.process.value().resident_bytes);
        out.appendf("process.peak_resident_bytes={}\n", snapshot.process.value().peak_resident_bytes);
        out.appendf("process.threads={}\n", snapshot.process.value().thread_count);
    }

    out.appendf("limits.affinity_cores={}\n", snapshot.limits.affinity_cores);
    if (snapshot.limits.cpu_quota.has_value())
        out.appendf("limits.cpu_quota={:.3f}\n", snapshot.limits.cpu_quota.value());
    if (snapshot.limits.memory_limit_bytes.has_value())
        out.appendf("limits.memory_limit_bytes={}\n", snapshot.limits.memory_limit_bytes.value());
    out.appendf("limits.containerized={}\n", snapshot.limits.containerized ? 1 : 0);

    for (auto const& m : snapshot.mounts)
        out.appendf("mount.{}.available_bytes={}\n", m.path, m.available_bytes);

    return out;
}

/// Writes one section as an event, with the bytes copied straight into the chunk.
void emit_section(rec::desc const& d, cc::span<byte const> bytes)
{
    if (bytes.empty())
        return;

    auto writer = rec::open_event(d, bytes.size());
    if (!writer.is_open())
        return;

    // The reservation may be shorter than asked for, and a truncated stamp is better than none: the section is
    // key=value lines, so a reader still gets every line that fit.
    auto const target = writer.payload();
    auto const written = target.size() < bytes.size() ? target.size() : bytes.size();
    cc::memcpy(target.data(), bytes.data(), written);
    writer.commit(written);
}
} // namespace
} // namespace cc::rec

bool cc::rec::register_stamp_contributor(char const* name, cc::rec::stamp_provider provider)
{
    if (name == nullptr || provider == nullptr)
        return false;

    auto const count = cc::rec::g_contributor_count.load(cc::memory_order_acquire);

    // Replacing rather than appending, so a library registering twice does not emit its section twice.
    for (isize i = 0; i < count; ++i)
        if (cc::string_view(cc::rec::g_contributors[i].name) == cc::string_view(name))
        {
            cc::rec::g_contributors[i].provider = provider;
            return true;
        }

    if (count >= cc::rec::k_max_contributors)
        return false;

    cc::rec::g_contributors[count].name = name;
    cc::rec::g_contributors[count].provider = provider;
    cc::rec::g_contributors[count].desc = {
        .kind = rec::event_kind::stamp,
        .lvl = rec::level::info,
        .enable_bit = rec::enable_bit_of(rec::category::values),
        .name = name,
        .dom = &cc::g_rec_domain,
        .fixed_payload_size = rec::desc::variable_payload,
    };

    cc::rec::g_contributor_count.store(count + 1, cc::memory_order_release);
    return true;
}

void cc::rec::emit_stamp(cc::rec::stamp_moment moment)
{
    // The machine description is worth having once per recording rather than twice: it cannot have changed.
    if (moment == rec::stamp_moment::open)
    {
        static constexpr auto system_desc = rec::desc{
            .kind = rec::event_kind::stamp,
            .lvl = rec::level::info,
            .enable_bit = rec::enable_bit_of(rec::category::values),
            .name = "cc.system",
            .dom = &cc::g_rec_domain,
            .fixed_payload_size = rec::desc::variable_payload,
        };
        auto const& section = cc::rec::system_section();
        cc::rec::emit_section(system_desc,
                              cc::span<byte const>(reinterpret_cast<byte const*>(section.data()), section.size()));
    }

    // The levels, at both ends, because the difference between them is what makes the pair worth taking.
    {
        static constexpr auto resources_desc = rec::desc{
            .kind = rec::event_kind::stamp,
            .lvl = rec::level::info,
            .enable_bit = rec::enable_bit_of(rec::category::values),
            .name = "cc.resources",
            .dom = &cc::g_rec_domain,
            .fixed_payload_size = rec::desc::variable_payload,
        };
        auto const section = cc::rec::resources_section();
        cc::rec::emit_section(resources_desc,
                              cc::span<byte const>(reinterpret_cast<byte const*>(section.data()), section.size()));
    }

    auto const count = cc::rec::g_contributor_count.load(cc::memory_order_acquire);
    for (isize i = 0; i < count; ++i)
    {
        auto const& c = cc::rec::g_contributors[i];
        if (c.provider == nullptr)
            continue;

        cc::rec::emit_section(c.desc, c.provider(moment));
    }
}
