#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/platform/system_metrics.hh> // cc::query_error
#include <clean-core/string/string.hh>

/// Storage, as the two unrelated questions the word "disk" hides.
///
/// **Usage is per mount point**: how full is `C:\`, how much is left on `/home`.
/// It needs no privileges anywhere and is a third of a dashboard's value on its own.
///
/// **I/O is per physical device**: bytes read, bytes written, how busy `nvme0n1` is.
/// It needs a different API on every platform and is unavailable on some.
///
/// One mount can span several devices and one device can host several mounts, so a single "disk" type answering both
/// would be wrong on any machine with a RAID or an LVM volume.
///
/// **Devices are addressed by a string id, never by an index into the enumeration.**
/// Unplug the second of three disks and every later index names a different device, so a sampler keyed on one would
/// silently start differencing two unrelated counters.
/// The id is stable for this process's lifetime, which is all a sampler needs; chasing anything stronger costs
/// privileged access on Windows and buys nothing.

/// One mounted filesystem, and how full it is.
struct cc::mount_point
{
    cc::string path;       ///< "C:\\", "/", "/home"
    cc::string filesystem; ///< "NTFS", "ext4", "apfs"

    i64 total_bytes = 0;

    /// Unused space on the device.
    i64 free_bytes = 0;

    /// Unused space THIS user may actually take.
    ///
    /// Lower than `free_bytes` under a quota, and on any filesystem reserving blocks for root — ext4 keeps 5% by
    /// default, so the two differ on a normal Linux box rather than only in exotic setups.
    /// This is the one a "space left" figure should use.
    i64 available_bytes = 0;

    bool removable = false;
};

/// One physical storage device.
struct cc::disk_device
{
    /// Stable for this process's lifetime, and what a sampler is keyed on.
    /// "\\\\.\\PhysicalDrive0" on Windows, "nvme0n1" or "sda" on Linux.
    cc::string id;

    cc::string model;
    cc::optional<i64> capacity_bytes;
    bool removable = false;
};

/// Monotone per-device I/O counters, since boot.
struct cc::disk_io_counters
{
    i64 bytes_read = 0;
    i64 bytes_written = 0;
    i64 read_ops = 0;
    i64 write_ops = 0;

    /// Seconds the device spent with at least one request outstanding.
    /// Absent where the platform does not account for it.
    cc::optional<f64> busy_secs;
};

/// What one device moved over a sampling interval.
struct cc::disk_io_rate
{
    f64 interval_secs = 0;

    f64 read_bytes_per_sec = 0;
    f64 write_bytes_per_sec = 0;
    f64 read_ops_per_sec = 0;
    f64 write_ops_per_sec = 0;

    /// Fraction of the interval the device was busy, in [0, 1], following cc's load convention.
    /// Absent where `busy_secs` is.
    cc::optional<f32> busy_fraction;
};

/// Per-device I/O, differenced against this sampler's previous reading.
///
/// **Not thread-safe.** Same shape as cc::cpu_load_sampler, and the same reason.
///
/// A device that disappears mid-session — a drive unplugged, which is normal operation rather than an error — makes
/// `sample()` report `unsupported` from then on, never a stale value and never a wild delta.
class cc::disk_io_sampler
{
public:
    /// `device_id` comes from cc::enumerate_disks; the id is copied, so the descriptor need not outlive this.
    explicit disk_io_sampler(cc::string_view device_id);

    [[nodiscard]] cc::result<cc::disk_io_rate, cc::query_error> sample();

    [[nodiscard]] static bool is_supported();

private:
    cc::string _device_id;
    cc::disk_io_counters _previous;
    f64 _previous_time_secs = 0;
    bool _has_baseline = false;
};

namespace cc
{
/// Every mounted filesystem that reports a size.
///
/// Pseudo filesystems — proc, sysfs, cgroup and the rest — are excluded by that rule rather than by a name list, since
/// a list would go stale and a zero-sized mount is nothing a dashboard can draw.
[[nodiscard]] cc::result<cc::vector<cc::mount_point>, cc::query_error> query_mounts();

/// The physical storage devices, or empty where the platform will not enumerate them.
///
/// Whole disks only, and only ones holding media: a partition inherits its parent's traffic and would double-count it,
/// and an empty slot — an unbound loop device, a card reader with no card — reports a zero size and is left out for the
/// same reason a zero-sized mount is.
[[nodiscard]] cc::vector<cc::disk_device> enumerate_disks();

/// The raw monotone counters for one device, for a caller that wants to difference them itself.
[[nodiscard]] cc::result<cc::disk_io_counters, cc::query_error> read_disk_io_counters(cc::string_view device_id);
} // namespace cc
