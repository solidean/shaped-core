#include <clean-core/common/macros.hh> // CC_OS_WINDOWS
#include <clean-core/common/time.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/platform/impl/text_file.hh>
#include <clean-core/platform/system_info.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/from_string.hh>

#if defined(CC_OS_WINDOWS)

#include <clean-core/platform/win32_sanitized.hh>
#include <clean-core/string/conversion.hh> // utf16_to_utf8
#include <intrin.h>                        // __cpuid, for the brand string no Win32 call reports

#elif defined(CC_OS_MACOS) || defined(CC_OS_IOS) || defined(CC_OS_TVOS)

#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdlib>

#elif defined(CC_OS_LINUX) || defined(CC_OS_ANDROID)

#include <clean-core/streams/file_stream.hh>
#include <sys/utsname.h>
#include <unistd.h>

#include <cstdlib>

#if defined(CC_ARCH_X64) || defined(CC_ARCH_X86)
#include <cpuid.h> // __get_cpuid, for the brand string /proc/cpuinfo omits on some kernels
#endif

#elif defined(CC_OS_EMSCRIPTEN)

#include <unistd.h> // sysconf, the one machine fact a wasm target can answer

#endif

using namespace cc::primitive_defines;

namespace cc
{
namespace
{
constexpr cc::string_view architecture_name()
{
#if defined(CC_ARCH_X64)
    return "x64";
#elif defined(CC_ARCH_ARM64)
    return "arm64";
#elif defined(CC_ARCH_X86)
    return "x86";
#elif defined(CC_ARCH_ARM32)
    return "arm32";
#elif defined(CC_ARCH_WASM32)
    return "wasm32";
#else
    return "";
#endif
}

#if defined(CC_ARCH_X64) || defined(CC_ARCH_X86)

/// The x86 brand string, which is the only place the marketing name lives.
/// Empty where the CPU does not implement the extended leaves, which nothing since roughly 2005 does not.
cc::string x86_brand_string()
{
    u32 regs[4] = {};
    auto const cpuid = [&regs](u32 leaf)
    {
#if defined(CC_OS_WINDOWS)
        int out[4] = {};
        __cpuid(out, int(leaf));
        for (auto i = 0; i < 4; ++i)
            regs[i] = u32(out[i]);
        return true;
#else
        return __get_cpuid(leaf, &regs[0], &regs[1], &regs[2], &regs[3]) != 0;
#endif
    };

    if (!cpuid(0x80000000u) || regs[0] < 0x80000004u)
        return {};

    char brand[49] = {};
    for (u32 leaf = 0; leaf < 3; ++leaf)
    {
        if (!cpuid(0x80000002u + leaf))
            return {};
        for (auto i = 0; i < 4; ++i)
            for (auto b = 0; b < 4; ++b)
                brand[leaf * 16 + u32(i) * 4 + u32(b)] = char((regs[i] >> (b * 8)) & 0xFF);
    }

    return cc::string(cc::impl::trimmed(cc::string_view(brand)));
}

cc::string x86_vendor_string()
{
    u32 regs[4] = {};
#if defined(CC_OS_WINDOWS)
    int out[4] = {};
    __cpuid(out, 0);
    for (auto i = 0; i < 4; ++i)
        regs[i] = u32(out[i]);
#else
    if (__get_cpuid(0, &regs[0], &regs[1], &regs[2], &regs[3]) == 0)
        return {};
#endif

    char vendor[13] = {};
    u32 const order[3] = {regs[1], regs[3], regs[2]}; // ebx, edx, ecx is the order the vendor string is spelled in
    for (auto i = 0; i < 3; ++i)
        for (auto b = 0; b < 4; ++b)
            vendor[u32(i) * 4 + u32(b)] = char((order[i] >> (b * 8)) & 0xFF);
    return cc::string(cc::string_view(vendor));
}

#endif // x86

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
/// A NUL-terminated wide Win32 string as utf-8.
cc::string wide_to_utf8(wchar_t const* text)
{
    auto length = isize(0);
    while (text[length] != 0)
        ++length;
    return cc::utf16_to_utf8(cc::span<char16_t const>(reinterpret_cast<char16_t const*>(text), length));
}

cc::optional<cc::string> registry_string(char const* key, char const* value)
{
    char buffer[512] = {};
    DWORD size = sizeof(buffer);
    auto const status = ::RegGetValueA(HKEY_LOCAL_MACHINE, key, value, RRF_RT_REG_SZ, nullptr, buffer, &size);
    if (status != ERROR_SUCCESS || size == 0)
        return {};
    return cc::string(cc::string_view(buffer));
}

cc::optional<i64> registry_dword(char const* key, char const* value)
{
    DWORD data = 0;
    DWORD size = sizeof(data);
    auto const status = ::RegGetValueA(HKEY_LOCAL_MACHINE, key, value, RRF_RT_REG_DWORD, nullptr, &data, &size);
    if (status != ERROR_SUCCESS)
        return {};
    return i64(data);
}

/// One pass over GetLogicalProcessorInformationEx, which is the only Win32 call that reports the efficiency class.
void fill_topology(cc::system_info& info)
{
    DWORD bytes = 0;
    ::GetLogicalProcessorInformationEx(RelationAll, nullptr, &bytes);
    if (bytes == 0)
        return;

    auto storage = cc::vector<byte>();
    storage.resize_to_uninitialized(isize(bytes));
    auto* const base = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(storage.data());
    if (!::GetLogicalProcessorInformationEx(RelationAll, base, &bytes))
        return;

    // Efficiency class is a small integer, higher meaning faster; one core class per distinct value.
    struct class_accum
    {
        BYTE efficiency = 0;
        i32 physical = 0;
        i32 logical = 0;
        cc::vector<cc::cpu_cache_level> caches;
    };
    auto classes = cc::vector<class_accum>();

    auto find_class = [&classes](BYTE efficiency) -> class_accum&
    {
        for (auto& c : classes)
            if (c.efficiency == efficiency)
                return c;
        classes.push_back({.efficiency = efficiency});
        return classes.back();
    };

    auto const popcount_mask = [](KAFFINITY mask)
    {
        auto n = i32(0);
        for (auto bit = 0u; bit < sizeof(KAFFINITY) * 8; ++bit)
            if ((mask >> bit) & 1)
                ++n;
        return n;
    };

    auto const* cursor = reinterpret_cast<byte const*>(base);
    auto const* const stop = cursor + bytes;
    while (cursor < stop)
    {
        auto const& entry = *reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX const*>(cursor);
        cursor += entry.Size;

        if (entry.Relationship == RelationProcessorCore)
        {
            auto& c = find_class(entry.Processor.EfficiencyClass);
            c.physical += 1;
            for (auto g = 0; g < i32(entry.Processor.GroupCount); ++g)
                c.logical += popcount_mask(entry.Processor.GroupMask[g].Mask);
        }
        else if (entry.Relationship == RelationNumaNode)
        {
            info.numa_nodes.push_back({.index = i32(entry.NumaNode.NodeNumber)});
        }
        else if (entry.Relationship == RelationCache)
        {
            auto const& cache = entry.Cache;
            auto const kind = cache.Type == CacheInstruction ? cc::cache_kind::instruction
                            : cache.Type == CacheData        ? cc::cache_kind::data
                                                             : cc::cache_kind::unified;

            // A cache record carries no efficiency class, so it is attributed by the cores it is shared with.
            auto const level = cc::cpu_cache_level{.level = i32(cache.Level),
                                                   .kind = kind,
                                                   .size_bytes = i64(cache.CacheSize),
                                                   .line_size_bytes = i32(cache.LineSize),
                                                   .sharing_cores = popcount_mask(cache.GroupMask.Mask)};

            // Identical records repeat once per core and collapse; two DIFFERENT caches at one level do not.
            // A 7950X3D's two CCDs carry a 96 MiB and a 32 MiB L3, and folding them loses the smaller one.
            for (auto& c : classes)
            {
                auto already = false;
                for (auto const& existing : c.caches)
                    if (existing.level == level.level && existing.kind == level.kind
                        && existing.size_bytes == level.size_bytes && existing.sharing_cores == level.sharing_cores)
                        already = true;

                if (!already)
                    c.caches.push_back(level);
            }
        }
    }

    // Descending performance, which is the order a reader expects and the order the names below assume.
    for (isize i = 0; i < classes.size(); ++i)
        for (isize j = i + 1; j < classes.size(); ++j)
            if (classes[j].efficiency > classes[i].efficiency)
                cc::swap(classes[i], classes[j]);

    for (isize i = 0; i < classes.size(); ++i)
    {
        auto name = cc::string();
        if (classes.size() > 1)
            name = i == 0 ? cc::string("performance") : cc::string("efficiency");

        info.core_classes.push_back({.name = cc::move(name),
                                     .physical_cores = classes[i].physical,
                                     .logical_cores = classes[i].logical,
                                     .caches = cc::move(classes[i].caches)});
    }
}

void fill_platform(cc::system_info& info)
{
#if defined(CC_ARCH_X64) || defined(CC_ARCH_X86)
    info.cpu_brand = cc::x86_brand_string();
    info.cpu_vendor = cc::x86_vendor_string();
#endif

    fill_topology(info);

    auto memory = MEMORYSTATUSEX{};
    memory.dwLength = sizeof(memory);
    if (::GlobalMemoryStatusEx(&memory))
        info.ram_total_bytes = i64(memory.ullTotalPhys);

    auto sys = SYSTEM_INFO{};
    ::GetSystemInfo(&sys);
    info.page_size_bytes = i64(sys.dwPageSize);

    info.os_name = "Windows";
    constexpr auto k_version_key = R"(SOFTWARE\Microsoft\Windows NT\CurrentVersion)";
    if (auto const display = registry_string(k_version_key, "DisplayVersion"); display.has_value())
        info.os_version = display.value();
    if (auto const build = registry_string(k_version_key, "CurrentBuildNumber"); build.has_value())
        info.os_build = build.value();

    // NOT ProductName: it still reads "Windows 10 Pro" on Windows 11, which is exactly the plausible-looking wrong
    // answer this design refuses to report.
    auto const major = registry_dword(k_version_key, "CurrentMajorVersionNumber");
    auto const minor = registry_dword(k_version_key, "CurrentMinorVersionNumber");
    auto const revision = registry_dword(k_version_key, "UBR");
    if (major.has_value() && !info.os_build.empty())
        info.kernel_version
            = cc::format("{}.{}.{}.{}", major.value(), minor.value_or(0), info.os_build, revision.value_or(0));

    // GetTickCount64 is milliseconds since boot, so the boot instant is now minus that.
    info.boot_time_wall_secs = cc::current_time_wall_secs() - f64(::GetTickCount64()) / 1000.0;

    auto tz = TIME_ZONE_INFORMATION{};
    if (::GetTimeZoneInformation(&tz) != TIME_ZONE_ID_INVALID)
    {
        info.timezone_at_start = wide_to_utf8(tz.StandardName);
    }

    wchar_t locale[LOCALE_NAME_MAX_LENGTH] = {};
    if (::GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH) > 0)
    {
        info.locale_at_start = wide_to_utf8(locale);
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
cc::optional<i64> sysctl_int(char const* name)
{
    i64 value = 0;
    auto size = sizeof(value);
    if (::sysctlbyname(name, &value, &size, nullptr, 0) != 0)
        return {};
    return value;
}

cc::optional<cc::string> sysctl_string(char const* name)
{
    size_t size = 0;
    if (::sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0)
        return {};

    auto buffer = cc::vector<char>();
    buffer.resize_to_uninitialized(isize(size));
    if (::sysctlbyname(name, buffer.data(), &size, nullptr, 0) != 0)
        return {};

    return cc::string(cc::impl::trimmed(cc::string_view(buffer.data())));
}

/// One perflevel, which is Darwin's own name for a core class and maps onto it exactly.
void fill_perflevel(cc::system_info& info, i32 index, i32 count)
{
    auto const key = [index](char const* suffix) { return cc::format("hw.perflevel{}.{}", index, suffix); };

    auto physical = sysctl_int(key("physicalcpu").c_str_materialize());
    auto logical = sysctl_int(key("logicalcpu").c_str_materialize());
    if (!physical.has_value() || !logical.has_value())
        return;

    auto caches = cc::vector<cc::cpu_cache_level>();
    auto const line = sysctl_int("hw.cachelinesize").value_or(0);
    auto const add_cache = [&caches, line](i32 level, cc::cache_kind kind, cc::optional<i64> size, i32 shared)
    {
        if (size.has_value() && size.value() > 0)
            caches.push_back({.level = level,
                              .kind = kind,
                              .size_bytes = size.value(),
                              .line_size_bytes = i32(line),
                              .sharing_cores = shared});
    };

    add_cache(1, cc::cache_kind::data, sysctl_int(key("l1dcachesize").c_str_materialize()), 1);
    add_cache(1, cc::cache_kind::instruction, sysctl_int(key("l1icachesize").c_str_materialize()), 1);

    // Apple's L2 is per cluster, and cpusperl2 is how many cores that cluster holds.
    auto const per_l2 = sysctl_int(key("cpusperl2").c_str_materialize()).value_or(i64(logical.value()));
    add_cache(2, cc::cache_kind::unified, sysctl_int(key("l2cachesize").c_str_materialize()), i32(per_l2));

    auto name = cc::string();
    if (count > 1)
        name = index == 0 ? cc::string("performance") : cc::string("efficiency");

    info.core_classes.push_back({.name = cc::move(name),
                                 .physical_cores = i32(physical.value()),
                                 .logical_cores = i32(logical.value()),
                                 .caches = cc::move(caches)});
}

void fill_platform(cc::system_info& info)
{
    if (auto brand = sysctl_string("machdep.cpu.brand_string"); brand.has_value())
        info.cpu_brand = cc::move(brand.value());
    if (auto vendor = sysctl_string("machdep.cpu.vendor"); vendor.has_value())
        info.cpu_vendor = cc::move(vendor.value());
    else
        info.cpu_vendor = "Apple";

    // hw.nperflevels is the P/E split, and it is absent on the Intel Macs that have no such split.
    auto const levels = i32(sysctl_int("hw.nperflevels").value_or(1));
    for (auto i = 0; i < levels; ++i)
        fill_perflevel(info, i, levels);

    if (info.core_classes.empty())
    {
        auto const physical = sysctl_int("hw.physicalcpu").value_or(0);
        auto const logical = sysctl_int("hw.logicalcpu").value_or(0);
        if (logical > 0)
            info.core_classes.push_back({.physical_cores = i32(physical), .logical_cores = i32(logical)});
    }

    info.ram_total_bytes = sysctl_int("hw.memsize");
    info.page_size_bytes = sysctl_int("hw.pagesize");

    info.os_name = "macOS";
    if (auto version = sysctl_string("kern.osproductversion"); version.has_value())
        info.os_version = cc::move(version.value());
    if (auto build = sysctl_string("kern.osversion"); build.has_value())
        info.os_build = cc::move(build.value());
    if (auto kernel = sysctl_string("kern.version"); kernel.has_value())
        info.kernel_version = cc::move(kernel.value());

    auto boot = timeval{};
    auto boot_size = sizeof(boot);
    if (::sysctlbyname("kern.boottime", &boot, &boot_size, nullptr, 0) == 0)
        info.boot_time_wall_secs = f64(boot.tv_sec) + f64(boot.tv_usec) / 1e6;

    if (auto const* tz = std::getenv("TZ"); tz != nullptr)
        info.timezone_at_start = cc::string(cc::string_view(tz));
    for (auto const* name : {"LC_ALL", "LC_CTYPE", "LANG"})
        if (auto const* value = std::getenv(name); value != nullptr && value[0] != '\0')
        {
            info.locale_at_start = cc::string(cc::string_view(value));
            break;
        }
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
/// The count of CPUs a "0-3,8" style cpu-list names, which is how sysfs spells every set of cores.
i32 count_cpu_list(cc::string_view list)
{
    auto total = i32(0);
    auto rest = cc::impl::trimmed(list);
    while (!rest.empty())
    {
        auto const comma = rest.find(',');
        auto item = comma < 0 ? rest : rest.subview_clamped(0, comma);
        rest = comma < 0 ? cc::string_view() : rest.subview(comma + 1);

        auto const dash = item.find('-');
        if (dash < 0)
        {
            total += 1;
            continue;
        }

        auto const lo = cc::from_string<i64>(item.subview_clamped(0, dash));
        auto const hi = cc::from_string<i64>(item.subview(dash + 1));
        if (lo.has_value() && hi.has_value() && hi.value() >= lo.value())
            total += i32(hi.value() - lo.value() + 1);
    }
    return total;
}

/// A sysfs cache size, which is written as "32K" or "8192K" rather than as bytes.
cc::optional<i64> parse_cache_size(cc::string_view text)
{
    auto s = cc::impl::trimmed(text);
    if (s.empty())
        return {};

    auto multiplier = i64(1);
    if (s.back() == 'K' || s.back() == 'k')
        multiplier = 1024;
    else if (s.back() == 'M' || s.back() == 'm')
        multiplier = 1024 * 1024;

    if (multiplier != 1)
        s = s.subview_clamped(0, s.size() - 1);

    auto const value = cc::from_string<i64>(s);
    if (!value.has_value())
        return {};
    return value.value() * multiplier;
}

void fill_caches(cc::vector<cc::cpu_cache_level>& out, i32 cpu)
{
    for (auto index = 0; index < 8; ++index)
    {
        auto const dir = cc::format("/sys/devices/system/cpu/cpu{}/cache/index{}", cpu, index);

        auto const level = cc::impl::read_int_file(cc::format("{}/level", dir));
        if (!level.has_value())
            break;

        auto const type = cc::impl::read_text_file(cc::format("{}/type", dir));
        auto kind = cc::cache_kind::unified;
        if (type.has_value())
        {
            auto const t = cc::impl::trimmed(type.value());
            if (t == "Data")
                kind = cc::cache_kind::data;
            else if (t == "Instruction")
                kind = cc::cache_kind::instruction;
        }

        auto size = cc::optional<i64>();
        if (auto const text = cc::impl::read_text_file(cc::format("{}/size", dir)); text.has_value())
            size = parse_cache_size(text.value());
        if (!size.has_value())
            continue;

        auto shared = i32(1);
        if (auto const list = cc::impl::read_text_file(cc::format("{}/shared_cpu_list", dir)); list.has_value())
            shared = count_cpu_list(list.value());

        out.push_back(
            {.level = i32(level.value()),
             .kind = kind,
             .size_bytes = size.value(),
             .line_size_bytes = i32(cc::impl::read_int_file(cc::format("{}/coherency_line_size", dir)).value_or(0)),
             .sharing_cores = shared});
    }
}

void fill_topology(cc::system_info& info)
{
    // `possible` rather than `online`: Android routinely parks cores, and a parked core is still part of the machine.
    auto cpu_count = i32(0);
    if (auto const possible = cc::impl::read_text_file("/sys/devices/system/cpu/possible"); possible.has_value())
        cpu_count = count_cpu_list(possible.value());
    if (cpu_count <= 0)
        cpu_count = i32(::sysconf(_SC_NPROCESSORS_CONF));
    if (cpu_count <= 0)
        return;

    // Cores of one capacity are one class; a machine with no cpu_capacity has exactly one.
    struct class_accum
    {
        i64 capacity = 0;
        i32 logical = 0;
        cc::vector<i32> core_ids;
        cc::vector<cc::cpu_cache_level> caches;
    };
    auto classes = cc::vector<class_accum>();

    for (auto cpu = 0; cpu < cpu_count; ++cpu)
    {
        auto const base = cc::format("/sys/devices/system/cpu/cpu{}", cpu);
        auto const capacity = cc::impl::read_int_file(cc::format("{}/cpu_capacity", base)).value_or(0);

        auto* found = static_cast<class_accum*>(nullptr);
        for (auto& c : classes)
            if (c.capacity == capacity)
                found = &c;
        if (found == nullptr)
        {
            classes.push_back({.capacity = capacity});
            found = &classes.back();
        }

        found->logical += 1;

        // core_id is unique only within a package, so the pair is what identifies a physical core.
        auto const core_id = cc::impl::read_int_file(cc::format("{}/topology/core_id", base)).value_or(cpu);
        auto const package = cc::impl::read_int_file(cc::format("{}/topology/physical_package_id", base)).value_or(0);
        auto const key = i32(package * 4096 + core_id);

        auto known = false;
        for (auto const id : found->core_ids)
            if (id == key)
                known = true;
        if (!known)
            found->core_ids.push_back(key);

        if (found->caches.empty())
            fill_caches(found->caches, cpu);
    }

    for (isize i = 0; i < classes.size(); ++i)
        for (isize j = i + 1; j < classes.size(); ++j)
            if (classes[j].capacity > classes[i].capacity)
                cc::swap(classes[i], classes[j]);

    for (isize i = 0; i < classes.size(); ++i)
    {
        auto name = cc::string();
        if (classes.size() > 1)
            name = i == 0 ? cc::string("performance") : cc::string("efficiency");

        info.core_classes.push_back({.name = cc::move(name),
                                     .physical_cores = i32(classes[i].core_ids.size()),
                                     .logical_cores = classes[i].logical,
                                     .caches = cc::move(classes[i].caches)});
    }

    for (auto node = 0; node < 64; ++node)
    {
        auto const meminfo = cc::impl::read_text_file(cc::format("/sys/devices/system/node/node{}/meminfo", node));
        if (!meminfo.has_value())
            break;

        auto entry = cc::numa_node{.index = node};

        // "Node 0 MemTotal:  65432 kB" — the key carries the node number, so it is matched by prefix rather than by
        // an exact field name.
        auto rest = cc::string_view(meminfo.value());
        auto line = cc::string_view();
        while (cc::impl::next_line(rest, line))
        {
            auto const colon = line.find(':');
            if (colon < 0 || !line.subview_clamped(0, colon).contains("MemTotal"))
                continue;

            auto number = cc::impl::trimmed(line.subview(colon + 1));
            if (auto const space = number.find(' '); space >= 0)
                number = number.subview_clamped(0, space);
            if (auto const value = cc::from_string<i64>(number); value.has_value())
                entry.memory_bytes = value.value() * 1024;
        }

        info.numa_nodes.push_back(entry);
    }
}

void fill_platform(cc::system_info& info)
{
#if defined(CC_ARCH_X64) || defined(CC_ARCH_X86)
    info.cpu_brand = cc::x86_brand_string();
    info.cpu_vendor = cc::x86_vendor_string();
#endif

    if (auto const cpuinfo = cc::impl::read_text_file("/proc/cpuinfo"); cpuinfo.has_value())
    {
        if (info.cpu_brand.empty())
            if (auto model = cc::impl::field_from(cpuinfo.value(), "model name", ':'); model.has_value())
                info.cpu_brand = cc::move(model.value());
        if (info.cpu_brand.empty())
            if (auto hardware = cc::impl::field_from(cpuinfo.value(), "Hardware", ':'); hardware.has_value())
                info.cpu_brand = cc::move(hardware.value());
        if (info.cpu_vendor.empty())
            if (auto vendor = cc::impl::field_from(cpuinfo.value(), "vendor_id", ':'); vendor.has_value())
                info.cpu_vendor = cc::move(vendor.value());
    }

    fill_topology(info);

    if (auto const meminfo = cc::impl::read_text_file("/proc/meminfo"); meminfo.has_value())
        if (auto const total = cc::impl::field_from(meminfo.value(), "MemTotal", ':'); total.has_value())
        {
            auto number = cc::string_view(total.value());
            if (auto const space = number.find(' '); space >= 0)
                number = number.subview_clamped(0, space);
            if (auto const kb = cc::from_string<i64>(number); kb.has_value())
                info.ram_total_bytes = kb.value() * 1024;
        }

    if (auto const page = ::sysconf(_SC_PAGESIZE); page > 0)
        info.page_size_bytes = i64(page);

    info.os_name = "Linux";
    if (auto const release = cc::impl::read_text_file("/etc/os-release"); release.has_value())
    {
        if (auto name = cc::impl::field_from(release.value(), "NAME", '='); name.has_value())
            info.os_name = cc::move(name.value());
        if (auto version = cc::impl::field_from(release.value(), "VERSION_ID", '='); version.has_value())
            info.os_version = cc::move(version.value());
    }

    auto uts = utsname{};
    if (::uname(&uts) == 0)
    {
        info.kernel_version = cc::string(cc::string_view(uts.release));
        info.os_build = cc::string(cc::string_view(uts.version));
        if (info.os_version.empty())
            info.os_version = cc::string(cc::string_view(uts.release));
    }

    // /proc/uptime's first field is seconds since boot, so the boot instant is now minus that.
    if (auto const uptime = cc::impl::read_text_file("/proc/uptime"); uptime.has_value())
    {
        auto first = cc::impl::trimmed(uptime.value());
        if (auto const space = first.find(' '); space >= 0)
            first = first.subview_clamped(0, space);
        if (auto const secs = cc::from_string<f64>(first); secs.has_value())
            info.boot_time_wall_secs = cc::current_time_wall_secs() - secs.value();
    }

    if (auto const* tz = std::getenv("TZ"); tz != nullptr && tz[0] != '\0')
        info.timezone_at_start = cc::string(cc::string_view(tz));
    else if (auto const zone = cc::impl::read_text_file("/etc/timezone"); zone.has_value())
        info.timezone_at_start = cc::string(cc::impl::trimmed(zone.value()));
    else
    {
        // /etc/timezone is a Debian file, and most of the world is systemd: there the zone name exists only as what
        // /etc/localtime points at, as ".../zoneinfo/Europe/Berlin".
        char link[512] = {};
        auto const length = ::readlink("/etc/localtime", link, sizeof(link) - 1);
        if (length > 0)
        {
            auto const target = cc::string_view(link, isize(length));
            constexpr auto k_marker = cc::string_view("zoneinfo/");
            if (auto const at = target.find(k_marker); at >= 0)
                info.timezone_at_start = cc::string(target.subview(at + k_marker.size()));
        }
    }

    for (auto const* name : {"LC_ALL", "LC_CTYPE", "LANG"})
        if (auto const* value = std::getenv(name); value != nullptr && value[0] != '\0')
        {
            info.locale_at_start = cc::string(cc::string_view(value));
            break;
        }
}
} // namespace
} // namespace cc

// =========================================================================================================
// Every other target: wasm, WASI and anything new
// =========================================================================================================
#else

namespace cc
{
namespace
{
/// A logical core count is the one thing these targets can answer, so it is the one thing filled in.
/// Everything else stays empty rather than guessed: a wasm heap size reported as "total RAM" would read exactly like a
/// real answer, which is the failure mode this design exists to avoid.
void fill_platform(cc::system_info& info)
{
#if defined(CC_OS_EMSCRIPTEN)
    if (auto const online = ::sysconf(_SC_NPROCESSORS_ONLN); online > 0)
        info.core_classes.push_back({.physical_cores = i32(online), .logical_cores = i32(online)});
#else
    (void)info;
#endif
}
} // namespace
} // namespace cc

#endif

// =========================================================================================================
// Shared
// =========================================================================================================

i32 cc::system_info::logical_cores() const
{
    auto total = i32(0);
    for (auto const& c : core_classes)
        total += c.logical_cores;
    return total;
}

i32 cc::system_info::physical_cores() const
{
    auto total = i32(0);
    for (auto const& c : core_classes)
        total += c.physical_cores;
    return total;
}

f64 cc::system_info::uptime_secs() const
{
    if (boot_time_wall_secs <= 0)
        return 0;
    return cc::current_time_wall_secs() - boot_time_wall_secs;
}

cc::optional<i64> cc::system_info::largest_cache_bytes(i32 level) const
{
    auto best = cc::optional<i64>();
    for (auto const& c : core_classes)
        for (auto const& cache : c.caches)
            if (cache.level == level && (!best.has_value() || cache.size_bytes > best.value()))
                best = cache.size_bytes;
    return best;
}

cc::system_info const& cc::get_system_info()
{
    static auto const info = []
    {
        auto built = cc::system_info();
        built.cpu_architecture = cc::string(cc::architecture_name());
        cc::fill_platform(built);
        return built;
    }();
    return info;
}
