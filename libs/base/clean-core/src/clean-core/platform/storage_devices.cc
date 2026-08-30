#include <clean-core/common/macros.hh> // CC_OS_WINDOWS
#include <clean-core/common/time.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/platform/impl/text_file.hh>
#include <clean-core/platform/storage_devices.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/from_string.hh>

#if defined(CC_OS_WINDOWS)

#include <clean-core/platform/win32_sanitized.hh>
#include <winioctl.h> // IOCTL_DISK_PERFORMANCE and the storage descriptor

#elif defined(CC_OS_MACOS) || defined(CC_OS_IOS) || defined(CC_OS_TVOS)

#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h> // stat, for the st_dev a mount is deduplicated on

#elif defined(CC_OS_LINUX) || defined(CC_OS_ANDROID)

#include <sys/stat.h> // stat, for the st_dev a mount is deduplicated on
#include <sys/statvfs.h>

#endif

namespace cc
{
namespace
{
cc::query_error storage_unsupported(cc::string_view what)
{
    return {.status = cc::query_status::unsupported, .detail = cc::format("{} is not available here", what)};
}

cc::query_error storage_failed(cc::string_view what)
{
    return {.status = cc::query_status::failed, .detail = cc::format("could not read {}", what)};
}

f64 per_second(i64 before, i64 after, f64 interval)
{
    if (interval <= 0 || after < before)
        return 0;
    return f64(after - before) / interval;
}

#if !defined(CC_OS_WINDOWS)
/// Whether this device has already been seen, recording it when it has not.
///
/// One filesystem is reachable at many paths — a bind mount, a container's overlay, a systemd per-service mount, an
/// automounter's entry and the filesystem behind it — and each answers statvfs identically.
/// So a caller summing free space over the list counts one device once per path, which on an ordinary Linux box is a
/// couple of dozen terabytes that do not exist.
bool claim_device(cc::vector<u64>& seen, u64 device)
{
    for (auto const id : seen)
        if (id == device)
            return false;

    seen.push_back(device);
    return true;
}
#endif
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
/// A device opened for a property or performance query.
/// Zero access rights on purpose: both queries work without read permission, and asking for one would need admin.
HANDLE open_device_for_query(cc::string_view id)
{
    auto path = cc::string(id);
    return ::CreateFileA(path.c_str_materialize(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                         nullptr);
}

cc::result<cc::vector<cc::mount_point>, cc::query_error> read_mounts()
{
    char buffer[512] = {};
    auto const length = ::GetLogicalDriveStringsA(sizeof(buffer) - 1, buffer);
    if (length == 0)
        return cc::error(storage_failed("GetLogicalDriveStrings"));

    auto out = cc::vector<cc::mount_point>();
    auto seen = cc::vector<cc::string>();

    // The result is a run of NUL-terminated roots ending in a double NUL.
    for (auto const* root = buffer; *root != 0;)
    {
        auto const root_view = cc::string_view(root);
        root += root_view.size() + 1;

        auto const type = ::GetDriveTypeA(root_view.data());
        if (type == DRIVE_NO_ROOT_DIR || type == DRIVE_CDROM)
            continue;

        // One entry per device: two drive letters can name one volume, which SUBST and a mounted folder both do.
        char volume[64] = {};
        if (::GetVolumeNameForVolumeMountPointA(root_view.data(), volume, sizeof(volume)))
        {
            auto const name = cc::string_view(volume);
            auto already = false;
            for (auto const& id : seen)
                if (cc::string_view(id) == name)
                    already = true;

            if (already)
                continue;

            seen.push_back(cc::string(name));
        }

        ULARGE_INTEGER available = {};
        ULARGE_INTEGER total = {};
        ULARGE_INTEGER free_space = {};
        if (!::GetDiskFreeSpaceExA(root_view.data(), &available, &total, &free_space) || total.QuadPart == 0)
            continue; // an empty card reader, and nothing a dashboard can draw

        char filesystem[64] = {};
        ::GetVolumeInformationA(root_view.data(), nullptr, 0, nullptr, nullptr, nullptr, filesystem, sizeof(filesystem));

        out.push_back({.path = cc::string(root_view),
                       .filesystem = cc::string(cc::string_view(filesystem)),
                       .total_bytes = i64(total.QuadPart),
                       .free_bytes = i64(free_space.QuadPart),
                       .available_bytes = i64(available.QuadPart),
                       .removable = type == DRIVE_REMOVABLE});
    }

    return out;
}

cc::vector<cc::disk_device> read_disks()
{
    auto out = cc::vector<cc::disk_device>();

    for (auto index = 0; index < 32; ++index)
    {
        auto const id = cc::format(R"(\\.\PhysicalDrive{})", index);
        auto* const handle = open_device_for_query(id);
        if (handle == INVALID_HANDLE_VALUE)
            continue;

        auto device = cc::disk_device{.id = id};

        auto query = STORAGE_PROPERTY_QUERY{};
        query.PropertyId = StorageDeviceProperty;
        query.QueryType = PropertyStandardQuery;

        byte descriptor_bytes[1024] = {};
        DWORD written = 0;
        if (::DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), descriptor_bytes,
                              sizeof(descriptor_bytes), &written, nullptr))
        {
            auto const& descriptor = *reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR const*>(descriptor_bytes);
            device.removable = descriptor.RemovableMedia != FALSE;

            if (descriptor.ProductIdOffset != 0 && descriptor.ProductIdOffset < written)
                device.model = cc::string(cc::impl::trimmed(
                    cc::string_view(reinterpret_cast<char const*>(descriptor_bytes) + descriptor.ProductIdOffset)));
        }

        auto geometry = DISK_GEOMETRY_EX{};
        if (::DeviceIoControl(handle, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &geometry, sizeof(geometry),
                              &written, nullptr))
            device.capacity_bytes = i64(geometry.DiskSize.QuadPart);

        ::CloseHandle(handle);
        out.push_back(cc::move(device));
    }

    return out;
}

cc::result<cc::disk_io_counters, cc::query_error> read_io(cc::string_view device_id)
{
    auto* const handle = open_device_for_query(device_id);
    if (handle == INVALID_HANDLE_VALUE)
        return cc::error(storage_unsupported(cc::format("device {}", device_id)));

    auto performance = DISK_PERFORMANCE{};
    DWORD written = 0;
    auto const ok = ::DeviceIoControl(handle, IOCTL_DISK_PERFORMANCE, nullptr, 0, &performance, sizeof(performance),
                                      &written, nullptr);
    ::CloseHandle(handle);

    if (!ok)
        return cc::error(storage_failed(cc::format("IOCTL_DISK_PERFORMANCE on {}", device_id)));

    return cc::disk_io_counters{.bytes_read = i64(performance.BytesRead.QuadPart),
                                .bytes_written = i64(performance.BytesWritten.QuadPart),
                                .read_ops = i64(performance.ReadCount),
                                .write_ops = i64(performance.WriteCount),
                                // IdleTime and the transfer times are in 100 ns units, like every other Windows tick.
                                .busy_secs = f64(performance.ReadTime.QuadPart + performance.WriteTime.QuadPart) * 1e-7};
}

constexpr bool k_has_disk_io = true;
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
cc::result<cc::vector<cc::mount_point>, cc::query_error> read_mounts()
{
    // `struct` is load-bearing: statfs names both a type and a function, and the bare tag resolves to the function.
    struct statfs* mounts = nullptr;
    auto const count = ::getmntinfo(&mounts, MNT_NOWAIT);
    if (count <= 0)
        return cc::error(storage_failed("getmntinfo"));

    auto out = cc::vector<cc::mount_point>();
    auto seen = cc::vector<u64>();
    for (auto i = 0; i < count; ++i)
    {
        auto const& m = mounts[i];
        if (m.f_blocks == 0)
            continue;

        // One entry per device — a firmlink puts one volume under two paths, the way a bind mount does on Linux.
        struct stat info = {};
        if (::stat(m.f_mntonname, &info) != 0 || !cc::claim_device(seen, u64(info.st_dev)))
            continue;

        auto const block = i64(m.f_bsize);
        out.push_back({.path = cc::string(cc::string_view(m.f_mntonname)),
                       .filesystem = cc::string(cc::string_view(m.f_fstypename)),
                       .total_bytes = i64(m.f_blocks) * block,
                       .free_bytes = i64(m.f_bfree) * block,
                       .available_bytes = i64(m.f_bavail) * block,
#if defined(MNT_REMOVABLE)
                       .removable = (m.f_flags & MNT_REMOVABLE) != 0});
#else
                       // Older Darwin SDKs do not define it, and a mount flag is not worth failing a build over.
                       .removable = false});
#endif
    }

    return out;
}

cc::vector<cc::disk_device> read_disks()
{
    // Enumerating physical devices on Darwin means IOKit, which clean-core does not link.
    return {};
}

cc::result<cc::disk_io_counters, cc::query_error> read_io(cc::string_view)
{
    return cc::error(storage_unsupported("per-device disk I/O"));
}

constexpr bool k_has_disk_io = false;
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
cc::result<cc::vector<cc::mount_point>, cc::query_error> read_mounts()
{
    auto const text = cc::impl::read_text_file("/proc/self/mounts");
    if (!text.has_value())
        return cc::error(storage_failed("/proc/self/mounts"));

    auto out = cc::vector<cc::mount_point>();
    auto seen = cc::vector<u64>();

    auto rest = cc::string_view(text.value());
    auto line = cc::string_view();
    while (cc::impl::next_line(rest, line))
    {
        // "device mountpoint fstype options dump pass"
        auto const first = line.find(' ');
        if (first < 0)
            continue;

        auto after_device = cc::impl::trimmed(line.subview(first));
        auto const second = after_device.find(' ');
        if (second < 0)
            continue;

        auto const path = after_device.subview_clamped(0, second);
        auto after_path = cc::impl::trimmed(after_device.subview(second));
        auto const third = after_path.find(' ');
        auto const fs = third < 0 ? after_path : after_path.subview_clamped(0, third);

        auto mount = cc::string(path);

        // `struct` is load-bearing: statvfs names both a type and a function, and the bare tag resolves to the
        // function, which is what makes the almost-always-auto spelling fail to compile here.
        struct statvfs stats = {};
        if (::statvfs(mount.c_str_materialize(), &stats) != 0)
            continue;

        // A filesystem reporting no blocks holds nothing a dashboard can draw, which catches proc, sysfs and cgroup
        // without a name list that would go stale.
        // It does NOT catch tmpfs, devtmpfs, efivarfs or overlay, which are pseudo filesystems with real block counts —
        // those are kept, because a full /tmp is a real problem.
        if (stats.f_blocks == 0)
            continue;

        // One entry per device: everything below this line is a path the caller has already been given.
        struct stat info = {};
        if (::stat(mount.c_str_materialize(), &info) != 0 || !cc::claim_device(seen, u64(info.st_dev)))
            continue;

        auto const block = i64(stats.f_frsize != 0 ? stats.f_frsize : stats.f_bsize);
        out.push_back({.path = cc::string(path),
                       .filesystem = cc::string(fs),
                       .total_bytes = i64(stats.f_blocks) * block,
                       .free_bytes = i64(stats.f_bfree) * block,
                       .available_bytes = i64(stats.f_bavail) * block,
                       .removable = false});
    }

    return out;
}

/// The whole-disk block devices, which are the ones /proc/diskstats accounts separately.
/// A partition inherits its parent's traffic and would double-count it.
cc::vector<cc::disk_device> read_disks()
{
    auto out = cc::vector<cc::disk_device>();

    auto const stats = cc::impl::read_text_file("/proc/diskstats");
    if (!stats.has_value())
        return out;

    auto rest = cc::string_view(stats.value());
    auto line = cc::string_view();
    while (cc::impl::next_line(rest, line))
    {
        auto trimmed_line = cc::impl::trimmed(line);
        if (trimmed_line.empty())
            continue;

        // major minor name ...
        auto after_major = cc::impl::trimmed(trimmed_line.subview(trimmed_line.find(' ') + 1));
        auto after_minor = cc::impl::trimmed(after_major.subview(after_major.find(' ') + 1));
        auto const space = after_minor.find(' ');
        if (space < 0)
            continue;

        auto const name = after_minor.subview_clamped(0, space);

        // Only whole disks have a `queue` directory; a partition does not.
        if (!cc::impl::read_text_file(cc::format("/sys/block/{}/queue/hw_sector_size", name)).has_value())
            continue;

        // `size` is always in 512-byte sectors, whatever the device's own sector size.
        auto const sectors = cc::impl::read_int_file(cc::format("/sys/block/{}/size", name));

        // A zero-sized whole disk is an empty slot — an unbound loop device, a card reader with no card — which is the
        // same nothing the mount loop above filters out, and holds no media to report or to sample.
        if (sectors.has_value() && sectors.value() <= 0)
            continue;

        auto device = cc::disk_device{.id = cc::string(name)};

        if (auto model = cc::impl::read_trimmed_file(cc::format("/sys/block/{}/device/model", name)); model.has_value())
            device.model = cc::move(model.value());

        if (sectors.has_value())
            device.capacity_bytes = sectors.value() * 512;

        device.removable = cc::impl::read_int_file(cc::format("/sys/block/{}/removable", name)).value_or(0) != 0;

        out.push_back(cc::move(device));
    }

    return out;
}

cc::result<cc::disk_io_counters, cc::query_error> read_io(cc::string_view device_id)
{
    auto const text = cc::impl::read_text_file(cc::format("/sys/block/{}/stat", device_id));
    if (!text.has_value())
        return cc::error(storage_unsupported(cc::format("device {}", device_id)));

    // reads, merged, sectors, ms, writes, merged, sectors, ms, in-flight, io_ms, weighted_ms
    f64 values[11] = {};
    auto rest = cc::impl::trimmed(text.value());
    for (auto i = 0; i < 11 && !rest.empty(); ++i)
    {
        auto const next = rest.find(' ');
        auto const token = next < 0 ? rest : rest.subview_clamped(0, next);
        rest = next < 0 ? cc::string_view() : cc::impl::trimmed(rest.subview(next));

        if (auto const value = cc::from_string<i64>(token); value.has_value())
            values[i] = f64(value.value());
    }

    // Sectors here are always 512 bytes, independent of the device's real sector size.
    return cc::disk_io_counters{.bytes_read = i64(values[2]) * 512,
                                .bytes_written = i64(values[6]) * 512,
                                .read_ops = i64(values[0]),
                                .write_ops = i64(values[4]),
                                .busy_secs = values[9] / 1000.0};
}

constexpr bool k_has_disk_io = true;
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
cc::result<cc::vector<cc::mount_point>, cc::query_error> read_mounts()
{
    return cc::error(storage_unsupported("mount points"));
}

cc::vector<cc::disk_device> read_disks()
{
    return {};
}

cc::result<cc::disk_io_counters, cc::query_error> read_io(cc::string_view)
{
    return cc::error(storage_unsupported("per-device disk I/O"));
}

constexpr bool k_has_disk_io = false;
} // namespace
} // namespace cc

#endif

// =========================================================================================================
// Shared
// =========================================================================================================

cc::result<cc::vector<cc::mount_point>, cc::query_error> cc::query_mounts()
{
    return cc::read_mounts();
}

cc::vector<cc::disk_device> cc::enumerate_disks()
{
    return cc::read_disks();
}

cc::result<cc::disk_io_counters, cc::query_error> cc::read_disk_io_counters(cc::string_view device_id)
{
    return cc::read_io(device_id);
}

bool cc::disk_io_sampler::is_supported()
{
    return cc::k_has_disk_io;
}

cc::disk_io_sampler::disk_io_sampler(cc::string_view device_id) : _device_id(device_id)
{
    auto baseline = cc::read_disk_io_counters(_device_id);
    if (baseline.has_value())
    {
        _previous = baseline.value();
        _previous_time_secs = cc::current_time_steady_secs();
        _has_baseline = true;
    }
}

cc::result<cc::disk_io_rate, cc::query_error> cc::disk_io_sampler::sample()
{
    auto current = cc::read_disk_io_counters(_device_id);
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

    auto out = cc::disk_io_rate();
    out.interval_secs = interval;
    out.read_bytes_per_sec = cc::per_second(_previous.bytes_read, next.bytes_read, interval);
    out.write_bytes_per_sec = cc::per_second(_previous.bytes_written, next.bytes_written, interval);
    out.read_ops_per_sec = cc::per_second(_previous.read_ops, next.read_ops, interval);
    out.write_ops_per_sec = cc::per_second(_previous.write_ops, next.write_ops, interval);

    if (_previous.busy_secs.has_value() && next.busy_secs.has_value() && interval > 0)
    {
        auto const busy = next.busy_secs.value() - _previous.busy_secs.value();
        auto const fraction = busy > 0 ? f32(busy / interval) : 0.0f;
        out.busy_fraction = fraction > 1 ? 1.0f : fraction;
    }

    _previous = next;
    _previous_time_secs = now;

    return out;
}
