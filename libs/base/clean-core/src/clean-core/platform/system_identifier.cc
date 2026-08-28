#include <clean-core/common/macros.hh> // CC_OS_WINDOWS
#include <clean-core/platform/system_identifier.hh>
#include <clean-core/string/char_predicates.hh>
#include <clean-core/string/format.hh>

#if defined(CC_OS_WINDOWS)

#include <clean-core/platform/win32_sanitized.hh>

// winsock2 must precede iphlpapi, whose structs are declared in terms of it, and <Windows.h> under LEAN_AND_MEAN pulls
// in neither.
// Sorted order puts iphlpapi first and does not compile, which is what the formatting escape below is holding off.
// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h> // GetAdaptersAddresses — the hardware addresses, which no other Win32 call reports
#include <lmcons.h>   // UNLEN, the documented bound on a user name
#include <winioctl.h> // IOCTL_STORAGE_QUERY_PROPERTY and its descriptor
// clang-format on

#elif defined(CC_OS_MACOS) || defined(CC_OS_IOS) || defined(CC_OS_TVOS)

#include <ifaddrs.h>
#include <net/if_dl.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <cstdlib>

#elif defined(CC_OS_LINUX) || defined(CC_OS_ANDROID)

#include <clean-core/streams/file_stream.hh>
#include <ifaddrs.h>
#include <netpacket/packet.h>
#include <unistd.h>

#include <cstdlib>

#endif

namespace cc
{
namespace
{
#if !defined(CC_OS_WINDOWS)
/// Six bytes as the colon-separated spelling everyone recognizes.
cc::string mac_as_text(unsigned char const* bytes, isize count)
{
    auto out = cc::string();
    for (isize i = 0; i < count; ++i)
    {
        if (i > 0)
            out += ':';
        out += cc::format("{:02X}", u32(bytes[i]));
    }
    return out;
}

/// Whether these bytes are worth reporting at all.
/// An all-zero address is a virtual adapter that never got one, and it identifies nothing.
bool is_real_mac(unsigned char const* bytes, isize count)
{
    if (count != 6)
        return false;
    for (isize i = 0; i < count; ++i)
        if (bytes[i] != 0)
            return true;
    return false;
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
cc::optional<cc::string> query_hostname()
{
    char buffer[256] = {};
    DWORD size = sizeof(buffer);
    if (!::GetComputerNameExA(ComputerNameDnsHostname, buffer, &size))
        return {};
    return cc::string(cc::string_view(buffer));
}

cc::optional<cc::string> query_username()
{
    char buffer[UNLEN + 1] = {};
    DWORD size = sizeof(buffer);
    if (!::GetUserNameA(buffer, &size))
        return {};
    return cc::string(cc::string_view(buffer));
}

cc::optional<cc::string> query_machine_id()
{
    char buffer[128] = {};
    DWORD size = sizeof(buffer);

    // RRF_SUBKEY_WOW6464KEY matters: a 32-bit process is otherwise redirected to the WOW6432Node view, which has no
    // MachineGuid at all.
    auto const status = ::RegGetValueA(HKEY_LOCAL_MACHINE, R"(SOFTWARE\Microsoft\Cryptography)", "MachineGuid",
                                       RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, buffer, &size);
    if (status != ERROR_SUCCESS || size == 0)
        return {};
    return cc::string(cc::string_view(buffer));
}

void query_mac_addresses(cc::vector<cc::string>& out)
{
    // The documented pattern: ask once for the size, and retry because the adapter set can change in between.
    ULONG size = 16 * 1024;
    auto storage = cc::vector<byte>();

    for (auto attempt = 0; attempt < 3; ++attempt)
    {
        storage.resize_to_uninitialized(isize(size));
        auto* const table = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
        auto const status = ::GetAdaptersAddresses(
            AF_UNSPEC, GAA_FLAG_SKIP_UNICAST | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, table, &size);

        if (status == ERROR_BUFFER_OVERFLOW)
            continue;
        if (status != NO_ERROR)
            return;

        for (auto const* adapter = table; adapter != nullptr; adapter = adapter->Next)
        {
            if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK || adapter->PhysicalAddressLength != 6)
                continue;

            auto text = cc::string();
            auto all_zero = true;
            for (auto i = 0u; i < adapter->PhysicalAddressLength; ++i)
            {
                if (i > 0)
                    text += ':';
                text += cc::format("{:02X}", u32(adapter->PhysicalAddress[i]));
                all_zero = all_zero && adapter->PhysicalAddress[i] == 0;
            }

            if (!all_zero)
                out.push_back(cc::move(text));
        }
        return;
    }
}

void query_disk_serials(cc::vector<cc::string>& out)
{
    for (auto drive = 0; drive < 16; ++drive)
    {
        auto path = cc::format(R"(\\.\PhysicalDrive{})", drive);

        // Zero access rights: a property query needs no read permission, and asking for one would need admin.
        auto* const handle = ::CreateFileA(path.c_str_materialize(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                           OPEN_EXISTING, 0, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            continue;

        auto query = STORAGE_PROPERTY_QUERY{};
        query.PropertyId = StorageDeviceProperty;
        query.QueryType = PropertyStandardQuery;

        byte buffer[1024] = {};
        DWORD written = 0;
        auto const ok = ::DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), buffer,
                                          sizeof(buffer), &written, nullptr);
        ::CloseHandle(handle);

        if (!ok)
            continue;

        auto const& descriptor = *reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR const*>(buffer);
        if (descriptor.SerialNumberOffset == 0 || descriptor.SerialNumberOffset >= written)
            continue;

        auto const serial = cc::string_view(reinterpret_cast<char const*>(buffer) + descriptor.SerialNumberOffset);
        auto trimmed = serial;
        while (!trimmed.empty() && cc::is_space(trimmed.front()))
            trimmed = trimmed.subview(1);
        while (!trimmed.empty() && cc::is_space(trimmed.back()))
            trimmed = trimmed.subview_clamped(0, trimmed.size() - 1);

        if (!trimmed.empty())
            out.push_back(cc::string(trimmed));
    }
}
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
cc::optional<cc::string> query_hostname()
{
    char buffer[256] = {};
    if (::gethostname(buffer, sizeof(buffer) - 1) != 0)
        return {};
    return cc::string(cc::string_view(buffer));
}

cc::optional<cc::string> query_username()
{
    if (auto const* user = std::getenv("USER"); user != nullptr && user[0] != '\0')
        return cc::string(cc::string_view(user));
    return {};
}

cc::optional<cc::string> query_machine_id()
{
    // The IOPlatformUUID is the Darwin equivalent, and reaching it needs IOKit — a framework clean-core does not link.
    // Absent rather than approximated with something that is not stable.
    return {};
}

void query_mac_addresses(cc::vector<cc::string>& out)
{
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0)
        return;

    for (auto const* entry = list; entry != nullptr; entry = entry->ifa_next)
    {
        if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_LINK)
            continue;
        if ((entry->ifa_flags & IFF_LOOPBACK) != 0)
            continue;

        auto const* link = reinterpret_cast<sockaddr_dl const*>(entry->ifa_addr);
        auto const* bytes = reinterpret_cast<unsigned char const*>(LLADDR(link));
        if (is_real_mac(bytes, isize(link->sdl_alen)))
            out.push_back(mac_as_text(bytes, isize(link->sdl_alen)));
    }

    ::freeifaddrs(list);
}

void query_disk_serials(cc::vector<cc::string>&)
{
    // Same story as the machine id: IOKit, which clean-core does not link.
}
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
cc::optional<cc::string> read_trimmed_file(cc::string_view path)
{
    auto adapter = cc::file_read_stream_adapter::open(path);
    if (adapter.has_error())
        return {};

    auto bytes = adapter.value().stream().read_all();
    if (bytes.has_error())
        return {};

    auto const& data = bytes.value();
    auto text = cc::string_view(reinterpret_cast<char const*>(data.data()), data.size());
    while (!text.empty() && cc::is_space(text.front()))
        text = text.subview(1);
    while (!text.empty() && cc::is_space(text.back()))
        text = text.subview_clamped(0, text.size() - 1);

    if (text.empty())
        return {};
    return cc::string(text);
}

cc::optional<cc::string> query_hostname()
{
    char buffer[256] = {};
    if (::gethostname(buffer, sizeof(buffer) - 1) != 0)
        return {};
    return cc::string(cc::string_view(buffer));
}

cc::optional<cc::string> query_username()
{
    for (auto const* name : {"USER", "LOGNAME"})
        if (auto const* value = std::getenv(name); value != nullptr && value[0] != '\0')
            return cc::string(cc::string_view(value));
    return {};
}

cc::optional<cc::string> query_machine_id()
{
    // /etc/machine-id is the systemd one; the dbus copy is what a machine without systemd has.
    if (auto id = read_trimmed_file("/etc/machine-id"); id.has_value())
        return id;
    return read_trimmed_file("/var/lib/dbus/machine-id");
}

void query_mac_addresses(cc::vector<cc::string>& out)
{
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0)
        return;

    for (auto const* entry = list; entry != nullptr; entry = entry->ifa_next)
    {
        if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_PACKET)
            continue;
        if ((entry->ifa_flags & IFF_LOOPBACK) != 0)
            continue;

        auto const* link = reinterpret_cast<sockaddr_ll const*>(entry->ifa_addr);
        if (is_real_mac(link->sll_addr, isize(link->sll_halen)))
            out.push_back(mac_as_text(link->sll_addr, isize(link->sll_halen)));
    }

    ::freeifaddrs(list);
}

void query_disk_serials(cc::vector<cc::string>& out)
{
    // Only the whole-disk devices carry a serial; a partition inherits its parent's and would double-report it.
    auto try_device = [&out](cc::string const& name)
    {
        if (auto serial = read_trimmed_file(cc::format("/sys/block/{}/device/serial", name)); serial.has_value())
            out.push_back(cc::move(serial.value()));
    };

    for (auto index = 0; index < 8; ++index)
    {
        try_device(cc::format("sd{}", char('a' + index))); // SATA and USB
        try_device(cc::format("vd{}", char('a' + index))); // virtio
        try_device(cc::format("nvme{}n1", index));
        try_device(cc::format("mmcblk{}", index));
    }
}
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
cc::optional<cc::string> query_hostname()
{
    return {};
}
cc::optional<cc::string> query_username()
{
    return {};
}
cc::optional<cc::string> query_machine_id()
{
    return {};
}
void query_mac_addresses(cc::vector<cc::string>&)
{
}
void query_disk_serials(cc::vector<cc::string>&)
{
}
} // namespace
} // namespace cc

#endif

cc::system_identifier cc::query_system_identifier(cc::flags<cc::identity_field> which)
{
    auto out = cc::system_identifier();

    if (which.has(cc::identity_field::hostname))
        out.hostname = cc::query_hostname();
    if (which.has(cc::identity_field::username))
        out.username = cc::query_username();
    if (which.has(cc::identity_field::machine_id))
        out.machine_id = cc::query_machine_id();
    if (which.has(cc::identity_field::mac_addresses))
        cc::query_mac_addresses(out.mac_addresses);
    if (which.has(cc::identity_field::disk_serials))
        cc::query_disk_serials(out.disk_serials);

    return out;
}
