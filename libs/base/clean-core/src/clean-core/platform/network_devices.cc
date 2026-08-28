#include <clean-core/common/macros.hh> // CC_OS_WINDOWS
#include <clean-core/common/time.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/platform/impl/text_file.hh>
#include <clean-core/platform/network_devices.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/from_string.hh>

#if defined(CC_OS_WINDOWS)

#include <clean-core/platform/win32_sanitized.hh>
#include <clean-core/string/conversion.hh> // utf16_to_utf8

// clang-format off
// winsock2 must precede the iphlpapi family, whose structs are declared in terms of it.
// Sorted order puts netioapi first and does not compile.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h> // GetIfTable2, the per-interface counters
// clang-format on

#elif defined(CC_OS_MACOS) || defined(CC_OS_IOS) || defined(CC_OS_TVOS)

#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>

#endif

namespace cc
{
namespace
{
cc::query_error net_unsupported(cc::string_view what)
{
    return {.status = cc::query_status::unsupported, .detail = cc::format("{} is not available here", what)};
}

f64 net_per_second(i64 before, i64 after, f64 interval)
{
    if (interval <= 0 || after < before)
        return 0;
    return f64(after - before) / interval;
}
} // namespace
} // namespace cc

// =========================================================================================================
// Windows
// =========================================================================================================
#if defined(CC_OS_WINDOWS)

namespace cc
{
namespace
{
cc::string wide_name_to_utf8(wchar_t const* text)
{
    auto length = isize(0);
    while (text[length] != 0)
        ++length;
    return cc::utf16_to_utf8(cc::span<char16_t const>(reinterpret_cast<char16_t const*>(text), length));
}

/// Walks the interface table once, handing each REAL row to `visit`.
/// GetIfTable2 allocates, so the free has to happen on every path out.
///
/// Filter interfaces are skipped, and that is not cosmetic.
/// NDIS stacks a pseudo-interface per lightweight filter on top of every adapter — a QoS scheduler, two WFP MAC layers
/// — and each one reports the underlying adapter's counters as its own.
/// One physical NIC therefore appears four times on an ordinary desktop, and anything summing interfaces counts the
/// machine's whole traffic four times over.
template <class VisitF>
bool for_each_interface(VisitF&& visit)
{
    MIB_IF_TABLE2* table = nullptr;
    if (::GetIfTable2(&table) != NO_ERROR || table == nullptr)
        return false;

    for (ULONG i = 0; i < table->NumEntries; ++i)
        if (table->Table[i].InterfaceAndOperStatusFlags.FilterInterface == 0)
            visit(table->Table[i]);

    ::FreeMibTable(table);
    return true;
}

cc::vector<cc::network_interface> read_interfaces()
{
    auto out = cc::vector<cc::network_interface>();

    for_each_interface(
        [&out](MIB_IF_ROW2 const& row)
        {
            auto entry = cc::network_interface();
            entry.id = wide_name_to_utf8(row.Alias);
            entry.description = wide_name_to_utf8(row.Description);
            entry.is_loopback = row.Type == IF_TYPE_SOFTWARE_LOOPBACK;
            entry.is_up = row.OperStatus == IfOperStatusUp;

            if (row.TransmitLinkSpeed != 0 && row.TransmitLinkSpeed != u64(-1))
                entry.link_speed_bps = i64(row.TransmitLinkSpeed);

            if (!entry.id.empty())
                out.push_back(cc::move(entry));
        });

    return out;
}

cc::result<cc::net_counters, cc::query_error> read_counters(cc::string_view interface_id)
{
    auto found = cc::optional<cc::net_counters>();

    for_each_interface(
        [&found, interface_id](MIB_IF_ROW2 const& row)
        {
            if (found.has_value() || wide_name_to_utf8(row.Alias) != interface_id)
                return;

            found = cc::net_counters{.bytes_sent = i64(row.OutOctets),
                                     .bytes_received = i64(row.InOctets),
                                     .packets_sent = i64(row.OutUcastPkts + row.OutNUcastPkts),
                                     .packets_received = i64(row.InUcastPkts + row.InNUcastPkts),
                                     .errors_in = i64(row.InErrors),
                                     .errors_out = i64(row.OutErrors),
                                     .drops_in = i64(row.InDiscards),
                                     .drops_out = i64(row.OutDiscards)};
        });

    if (!found.has_value())
        return cc::error(net_unsupported(cc::format("interface {}", interface_id)));
    return found.value();
}

constexpr bool k_has_net_counters = true;
} // namespace
} // namespace cc

// =========================================================================================================
// macOS and the other Darwin targets
// =========================================================================================================
#elif defined(CC_OS_MACOS) || defined(CC_OS_IOS) || defined(CC_OS_TVOS)

namespace cc
{
namespace
{
cc::vector<cc::network_interface> read_interfaces()
{
    auto out = cc::vector<cc::network_interface>();

    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0)
        return out;

    for (auto const* entry = list; entry != nullptr; entry = entry->ifa_next)
    {
        // The link-layer entry is the one carrying if_data; the AF_INET ones are addresses on the same interface.
        if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_LINK || entry->ifa_data == nullptr)
            continue;

        auto item = cc::network_interface();
        item.id = cc::string(cc::string_view(entry->ifa_name));
        item.is_loopback = (entry->ifa_flags & IFF_LOOPBACK) != 0;
        item.is_up = (entry->ifa_flags & IFF_UP) != 0 && (entry->ifa_flags & IFF_RUNNING) != 0;

        auto const* const data = reinterpret_cast<if_data const*>(entry->ifa_data);
        if (data->ifi_baudrate != 0)
            item.link_speed_bps = i64(data->ifi_baudrate);

        out.push_back(cc::move(item));
    }

    ::freeifaddrs(list);
    return out;
}

cc::result<cc::net_counters, cc::query_error> read_counters(cc::string_view interface_id)
{
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0)
        return cc::error(net_unsupported("getifaddrs"));

    auto found = cc::optional<cc::net_counters>();
    for (auto const* entry = list; entry != nullptr; entry = entry->ifa_next)
    {
        if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_LINK || entry->ifa_data == nullptr)
            continue;
        if (cc::string_view(entry->ifa_name) != interface_id)
            continue;

        auto const* const data = reinterpret_cast<if_data const*>(entry->ifa_data);
        found = cc::net_counters{.bytes_sent = i64(data->ifi_obytes),
                                 .bytes_received = i64(data->ifi_ibytes),
                                 .packets_sent = i64(data->ifi_opackets),
                                 .packets_received = i64(data->ifi_ipackets),
                                 .errors_in = i64(data->ifi_ierrors),
                                 .errors_out = i64(data->ifi_oerrors),
                                 .drops_in = i64(data->ifi_iqdrops),
                                 .drops_out = 0};
        break;
    }

    ::freeifaddrs(list);

    if (!found.has_value())
        return cc::error(net_unsupported(cc::format("interface {}", interface_id)));
    return found.value();
}

constexpr bool k_has_net_counters = true;
} // namespace
} // namespace cc

// =========================================================================================================
// Linux and Android
// =========================================================================================================
#elif defined(CC_OS_LINUX) || defined(CC_OS_ANDROID)

namespace cc
{
namespace
{
/// One line of /proc/net/dev, which is both the interface list and its counters.
///
/// Reading it rather than listing /sys/class/net is deliberate: cc has no directory walk, and this file carries the
/// names and the numbers together.
/// The format is "  name: rx_bytes rx_packets rx_errs rx_drop ... tx_bytes tx_packets tx_errs tx_drop ...".
bool parse_dev_line(cc::string_view line, cc::string& name_out, cc::net_counters& counters_out)
{
    auto const colon = line.find(':');
    if (colon < 0)
        return false;

    name_out = cc::string(cc::impl::trimmed(line.subview_clamped(0, colon)));
    if (name_out.empty())
        return false;

    i64 values[16] = {};
    auto rest = cc::impl::trimmed(line.subview(colon + 1));
    for (auto i = 0; i < 16 && !rest.empty(); ++i)
    {
        auto const next = rest.find(' ');
        auto const token = next < 0 ? rest : rest.subview_clamped(0, next);
        rest = next < 0 ? cc::string_view() : cc::impl::trimmed(rest.subview(next));

        if (auto const value = cc::from_string<i64>(token); value.has_value())
            values[i] = value.value();
    }

    counters_out = {.bytes_sent = values[8],
                    .bytes_received = values[0],
                    .packets_sent = values[9],
                    .packets_received = values[1],
                    .errors_in = values[2],
                    .errors_out = values[10],
                    .drops_in = values[3],
                    .drops_out = values[11]};
    return true;
}

cc::vector<cc::network_interface> read_interfaces()
{
    auto out = cc::vector<cc::network_interface>();

    auto const text = cc::impl::read_text_file("/proc/net/dev");
    if (!text.has_value())
        return out;

    auto rest = cc::string_view(text.value());
    auto line = cc::string_view();

    // Two header lines before the data.
    cc::impl::next_line(rest, line);
    cc::impl::next_line(rest, line);

    while (cc::impl::next_line(rest, line))
    {
        auto name = cc::string();
        auto counters = cc::net_counters();
        if (!parse_dev_line(line, name, counters))
            continue;

        auto item = cc::network_interface{.id = name};

        // Type 772 is ARPHRD_LOOPBACK, which is what the kernel calls lo.
        item.is_loopback = cc::impl::read_int_file(cc::format("/sys/class/net/{}/type", name)).value_or(0) == 772;

        if (auto const state = cc::impl::read_trimmed_file(cc::format("/sys/class/net/{}/operstate", name));
            state.has_value())
            item.is_up = state.value() == "up" || state.value() == "unknown"; // loopback reports "unknown"

        // Reported in Mbit/s, and -1 for a link that has no speed to report.
        if (auto const mbit = cc::impl::read_int_file(cc::format("/sys/class/net/{}/speed", name));
            mbit.has_value() && mbit.value() > 0)
            item.link_speed_bps = mbit.value() * 1'000'000;

        out.push_back(cc::move(item));
    }

    return out;
}

cc::result<cc::net_counters, cc::query_error> read_counters(cc::string_view interface_id)
{
    auto const text = cc::impl::read_text_file("/proc/net/dev");
    if (!text.has_value())
        return cc::error(net_unsupported("/proc/net/dev"));

    auto rest = cc::string_view(text.value());
    auto line = cc::string_view();
    cc::impl::next_line(rest, line);
    cc::impl::next_line(rest, line);

    while (cc::impl::next_line(rest, line))
    {
        auto name = cc::string();
        auto counters = cc::net_counters();
        if (parse_dev_line(line, name, counters) && cc::string_view(name) == interface_id)
            return counters;
    }

    return cc::error(net_unsupported(cc::format("interface {}", interface_id)));
}

constexpr bool k_has_net_counters = true;
} // namespace
} // namespace cc

// =========================================================================================================
// Every other target
// =========================================================================================================
#else

namespace cc
{
namespace
{
cc::vector<cc::network_interface> read_interfaces()
{
    return {};
}

cc::result<cc::net_counters, cc::query_error> read_counters(cc::string_view)
{
    return cc::error(net_unsupported("network counters"));
}

constexpr bool k_has_net_counters = false;
} // namespace
} // namespace cc

#endif

// =========================================================================================================
// Shared
// =========================================================================================================

cc::vector<cc::network_interface> cc::enumerate_network_interfaces()
{
    return cc::read_interfaces();
}

cc::result<cc::net_counters, cc::query_error> cc::read_net_counters(cc::string_view interface_id)
{
    return cc::read_counters(interface_id);
}

bool cc::net_traffic_sampler::is_supported()
{
    return cc::k_has_net_counters;
}

cc::net_traffic_sampler::net_traffic_sampler(cc::string_view interface_id) : _interface_id(interface_id)
{
    auto baseline = cc::read_net_counters(_interface_id);
    if (baseline.has_value())
    {
        _previous = baseline.value();
        _previous_time_secs = cc::current_time_steady_secs();
        _has_baseline = true;
    }
}

cc::result<cc::net_traffic_rate, cc::query_error> cc::net_traffic_sampler::sample()
{
    auto current = cc::read_net_counters(_interface_id);
    if (current.has_error())
        return cc::error(cc::move(current.error()));

    auto const now = cc::current_time_steady_secs();

    if (!_has_baseline)
    {
        _previous = current.value();
        _previous_time_secs = now;
        _has_baseline = true;
        return cc::error(cc::query_error{.status = cc::query_status::failed,
                                         .detail = cc::string("no baseline yet; this call took one")});
    }

    auto const interval = now - _previous_time_secs;
    auto const& next = current.value();

    auto out = cc::net_traffic_rate();
    out.interval_secs = interval;
    out.sent_bytes_per_sec = cc::net_per_second(_previous.bytes_sent, next.bytes_sent, interval);
    out.received_bytes_per_sec = cc::net_per_second(_previous.bytes_received, next.bytes_received, interval);
    out.sent_packets_per_sec = cc::net_per_second(_previous.packets_sent, next.packets_sent, interval);
    out.received_packets_per_sec = cc::net_per_second(_previous.packets_received, next.packets_received, interval);

    _previous = next;
    _previous_time_secs = now;

    return out;
}
