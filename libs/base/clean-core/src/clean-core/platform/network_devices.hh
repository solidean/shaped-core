#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/platform/system_metrics.hh> // cc::query_error
#include <clean-core/string/string.hh>

/// Network interfaces and what they have carried.
///
/// The same shape as platform/storage_devices.hh, and for the same reasons.
/// Interfaces are plural and the set changes while a program runs — a VPN comes up, a dock is unplugged — so a sampler
/// is keyed on a stable string id rather than on a position in the enumeration.
///
/// This is traffic accounting, not packet capture.
/// Every number here is a counter the OS already keeps, readable without privileges on every platform that has it.

/// One network interface.
struct cc::network_interface
{
    /// Stable for this process's lifetime, and what a sampler is keyed on.
    /// The adapter alias on Windows, "eth0" or "wlan0" on Linux, "en0" on Darwin.
    cc::string id;

    /// A human-readable name where the platform has a second one — the hardware description on Windows.
    /// Empty where the id is already the only name there is.
    cc::string description;

    bool is_loopback = false;

    /// Whether the interface is up and carrying traffic, as opposed to present but disconnected.
    bool is_up = false;

    /// Negotiated link speed in bits per second.
    ///
    /// Absent for an interface that has no such notion, and **often wrong where present**: a disconnected adapter
    /// reports its maximum, and a virtual one usually reports whatever its driver invented.
    /// Deliberately not turned into a utilization percentage here, because a full-duplex link has two directions and
    /// any single ratio has to pick one silently.
    cc::optional<i64> link_speed_bps;
};

/// Monotone per-interface counters, since the interface came up.
struct cc::net_counters
{
    i64 bytes_sent = 0;
    i64 bytes_received = 0;
    i64 packets_sent = 0;
    i64 packets_received = 0;

    i64 errors_in = 0;
    i64 errors_out = 0;
    i64 drops_in = 0;
    i64 drops_out = 0;
};

/// What one interface carried over a sampling interval.
struct cc::net_traffic_rate
{
    f64 interval_secs = 0;

    f64 sent_bytes_per_sec = 0;
    f64 received_bytes_per_sec = 0;
    f64 sent_packets_per_sec = 0;
    f64 received_packets_per_sec = 0;
};

/// Per-interface traffic, differenced against this sampler's previous reading.
///
/// **Not thread-safe.** Same shape as cc::cpu_load_sampler, and the same reason.
///
/// An interface that goes away — a VPN dropped, a dock unplugged — makes `sample()` report `unsupported` from then on,
/// never a stale value and never a wild delta.
class cc::net_traffic_sampler
{
public:
    /// `interface_id` comes from cc::enumerate_network_interfaces; the id is copied.
    explicit net_traffic_sampler(cc::string_view interface_id);

    [[nodiscard]] cc::result<cc::net_traffic_rate, cc::query_error> sample();

    [[nodiscard]] static bool is_supported();

private:
    cc::string _interface_id;
    cc::net_counters _previous;
    f64 _previous_time_secs = 0;
    bool _has_baseline = false;
};

namespace cc
{
/// Every network interface the OS reports, loopback included.
///
/// Loopback is kept rather than filtered: it carries real traffic on a machine talking to itself, and a caller that
/// does not want it has `is_loopback` to say so.
///
/// **Filter pseudo-interfaces are excluded**, which matters more than it sounds.
/// Windows stacks one per NDIS lightweight filter on every adapter, each reporting the adapter's own counters, so a
/// single NIC shows up four times on an ordinary desktop and a sum over interfaces quadruples the machine's traffic.
[[nodiscard]] cc::vector<cc::network_interface> enumerate_network_interfaces();

/// The raw monotone counters for one interface, for a caller that wants to difference them itself.
[[nodiscard]] cc::result<cc::net_counters, cc::query_error> read_net_counters(cc::string_view interface_id);
} // namespace cc
