#include <clean-core/common/macros.hh> // CC_OS_WINDOWS
#include <clean-core/common/utility.hh>
#include <clean-core/platform/impl/text_file.hh>
#include <clean-core/platform/resource_limits.hh>
#include <clean-core/platform/system_info.hh>
#include <clean-core/string/from_string.hh>

#if defined(CC_OS_WINDOWS)

#include <clean-core/platform/win32_sanitized.hh>
#include <intrin.h> // __cpuid, for the hypervisor bit

#elif defined(CC_OS_MACOS) || defined(CC_OS_IOS) || defined(CC_OS_TVOS)

#include <unistd.h>

#elif defined(CC_OS_LINUX) || defined(CC_OS_ANDROID)

#include <sched.h>
#include <unistd.h>

#if defined(CC_ARCH_X64) || defined(CC_ARCH_X86)
#include <cpuid.h>
#endif

#endif

using namespace cc::primitive_defines;

namespace cc
{
namespace
{
#if defined(CC_ARCH_X64) || defined(CC_ARCH_X86)
/// Leaf 1, ECX bit 31 — the bit every hypervisor is expected to set and any of them may hide.
bool has_hypervisor_bit()
{
    u32 regs[4] = {};
#if defined(CC_OS_WINDOWS)
    int out[4] = {};
    __cpuid(out, 1);
    for (auto i = 0; i < 4; ++i)
        regs[i] = u32(out[i]);
#else
    if (__get_cpuid(1, &regs[0], &regs[1], &regs[2], &regs[3]) == 0)
        return false;
#endif
    return (regs[2] & (1u << 31)) != 0;
}
#else
bool has_hypervisor_bit()
{
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
void fill_limits(cc::resource_limits& limits)
{
    // GetProcessAffinityMask covers one processor group; a machine with more than 64 logical cores has several, and
    // GetActiveProcessorCount is what sees all of them.
    DWORD_PTR process_mask = 0;
    DWORD_PTR system_mask = 0;
    if (::GetProcessAffinityMask(::GetCurrentProcess(), &process_mask, &system_mask))
    {
        auto count = i32(0);
        for (auto bit = 0u; bit < sizeof(DWORD_PTR) * 8; ++bit)
            if ((process_mask >> bit) & 1)
                ++count;
        limits.affinity_cores = count;
    }

    auto const groups = i32(::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    if (groups > limits.affinity_cores && process_mask == system_mask)
        limits.affinity_cores = groups;

    // A null job handle queries the job this process is in, which is how a Windows container imposes its limits.
    auto cpu_rate = JOBOBJECT_CPU_RATE_CONTROL_INFORMATION{};
    DWORD returned = 0;
    if (::QueryInformationJobObject(nullptr, JobObjectCpuRateControlInformation, &cpu_rate, sizeof(cpu_rate), &returned)
        && (cpu_rate.ControlFlags & JOB_OBJECT_CPU_RATE_CONTROL_ENABLE) != 0
        && (cpu_rate.ControlFlags & JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP) != 0)
    {
        // CpuRate is in hundredths of a percent of the whole machine.
        auto const share = f32(cpu_rate.CpuRate) / 10000.0f;
        auto const machine = f32(cc::get_system_info().logical_cores());
        if (share > 0 && machine > 0)
        {
            limits.cpu_quota = share * machine;
            limits.containerized = true;
        }
    }

    auto extended = JOBOBJECT_EXTENDED_LIMIT_INFORMATION{};
    if (::QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation, &extended, sizeof(extended), &returned))
    {
        if ((extended.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_PROCESS_MEMORY) != 0)
            limits.memory_limit_bytes = i64(extended.ProcessMemoryLimit);
        else if ((extended.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_JOB_MEMORY) != 0)
            limits.memory_limit_bytes = i64(extended.JobMemoryLimit);
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
void fill_limits(cc::resource_limits& limits)
{
    // Darwin has no affinity mask a process can read, and no cgroups: what the machine has is what it may use.
    if (auto const online = ::sysconf(_SC_NPROCESSORS_ONLN); online > 0)
        limits.affinity_cores = i32(online);
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
/// cgroup v2 writes "max 100000" for no limit, and "400000 100000" for four CPUs.
cc::optional<f32> read_cgroup_v2_quota()
{
    auto const text = cc::impl::read_trimmed_file("/sys/fs/cgroup/cpu.max");
    if (!text.has_value())
        return {};

    auto const line = cc::string_view(text.value());
    auto const space = line.find(' ');
    if (space < 0)
        return {};

    auto const quota = line.subview_clamped(0, space);
    if (quota == "max")
        return {};

    auto const quota_value = cc::from_string<i64>(quota);
    auto const period_value = cc::from_string<i64>(cc::impl::trimmed(line.subview(space + 1)));
    if (!quota_value.has_value() || !period_value.has_value() || period_value.value() <= 0)
        return {};

    return f32(f64(quota_value.value()) / f64(period_value.value()));
}

/// cgroup v1 splits the same two numbers across two files, and spells "no limit" as -1.
cc::optional<f32> read_cgroup_v1_quota()
{
    auto const quota = cc::impl::read_int_file("/sys/fs/cgroup/cpu/cpu.cfs_quota_us");
    auto const period = cc::impl::read_int_file("/sys/fs/cgroup/cpu/cpu.cfs_period_us");
    if (!quota.has_value() || !period.has_value() || quota.value() <= 0 || period.value() <= 0)
        return {};

    return f32(f64(quota.value()) / f64(period.value()));
}

cc::optional<i64> read_memory_limit()
{
    // v2 spells no-limit as "max"; v1 as a number so large it means the same thing.
    if (auto const v2 = cc::impl::read_trimmed_file("/sys/fs/cgroup/memory.max"); v2.has_value())
    {
        if (v2.value() == "max")
            return {};
        if (auto const value = cc::from_string<i64>(cc::string_view(v2.value())); value.has_value())
            return value;
    }

    if (auto const v1 = cc::impl::read_int_file("/sys/fs/cgroup/memory/memory.limit_in_bytes"); v1.has_value())
    {
        // The v1 "unlimited" sentinel is page-size dependent and enormous; anything past a petabyte is not a limit.
        if (v1.value() > 0 && v1.value() < (i64(1) << 50))
            return v1;
    }

    return {};
}

bool looks_containerized()
{
    if (cc::impl::read_text_file("/.dockerenv").has_value())
        return true;

    auto const cgroup = cc::impl::read_text_file("/proc/1/cgroup");
    if (!cgroup.has_value())
        return false;

    auto const text = cc::string_view(cgroup.value());
    return text.contains("docker") || text.contains("lxc") || text.contains("kubepods") || text.contains("containerd");
}

void fill_limits(cc::resource_limits& limits)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    if (::sched_getaffinity(0, sizeof(set), &set) == 0)
        limits.affinity_cores = i32(CPU_COUNT(&set));
    else if (auto const online = ::sysconf(_SC_NPROCESSORS_ONLN); online > 0)
        limits.affinity_cores = i32(online);

    limits.cpu_quota = read_cgroup_v2_quota();
    if (!limits.cpu_quota.has_value())
        limits.cpu_quota = read_cgroup_v1_quota();

    limits.memory_limit_bytes = read_memory_limit();
    limits.containerized = looks_containerized();
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
void fill_limits(cc::resource_limits&)
{
}
} // namespace
} // namespace cc

#endif

cc::resource_limits cc::query_resource_limits()
{
    auto limits = cc::resource_limits();
    cc::fill_limits(limits);
    limits.hypervisor_present = cc::has_hypervisor_bit();
    return limits;
}

i32 cc::recommended_worker_count()
{
    auto const limits = cc::query_resource_limits();

    auto count = cc::get_system_info().logical_cores();
    if (limits.affinity_cores > 0 && limits.affinity_cores < count)
        count = limits.affinity_cores;

    if (limits.cpu_quota.has_value())
    {
        // Round up: a 1.5-CPU quota still wants two threads, since one of them is what the half is for.
        auto const from_quota = i32(limits.cpu_quota.value() + 0.999f);
        if (from_quota > 0 && from_quota < count)
            count = from_quota;
    }

    return count < 1 ? 1 : count;
}
