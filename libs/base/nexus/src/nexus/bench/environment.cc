#include "environment.hh"

#include <clean-core/common/macros.hh>
#include <clean-core/common/time.hh>
#include <clean-core/platform/system_info.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/thread.hh>
#include <nexus/bench/impl/platform_load.hh>

using namespace cc::primitive_defines;

namespace
{
cc::string_view os_name()
{
#if defined(CC_OS_WINDOWS)
    return "windows";
#elif defined(CC_OS_LINUX)
    return "linux";
#elif defined(CC_OS_MACOS)
    return "macos";
#elif defined(CC_OS_ANDROID)
    return "android";
#elif defined(CC_OS_IOS)
    return "ios";
#elif defined(CC_OS_TVOS)
    return "tvos";
#elif defined(CC_OS_EMSCRIPTEN)
    return "emscripten";
#elif defined(CC_OS_WASI)
    return "wasi";
#else
    return "unknown";
#endif
}

cc::string_view arch_name()
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
    return "unknown";
#endif
}

cc::string_view build_name()
{
#if defined(CC_DEBUG)
    return "debug";
#elif defined(CC_RELWITHDEBINFO)
    return "relwithdebinfo";
#elif defined(CC_RELEASE)
    return "release";
#else
    return "unknown";
#endif
}
} // namespace

nx::bench::system_summary const& nx::bench::describe_system()
{
    static system_summary const s = []
    {
        auto result = system_summary{};
        result.os = cc::string(os_name());
        result.arch = cc::string(arch_name());

        auto const& system = cc::get_system_info();

        // Empty where the platform will not say, which is honest in a way "unknown" was not: a reader can tell a
        // missing answer from a CPU actually called that.
        result.cpu = system.cpu_brand;

        // The MACHINE's count, not cc::recommended_worker_count(): a benchmark report describes the hardware a number
        // was measured on, and a container's quota is a property of the run rather than of the machine.
        result.logical_cores = system.logical_cores();

        result.build = cc::string(build_name());
        result.assertions_enabled = CC_ASSERT_ENABLED != 0;

        // No placeholders left: cc gained the system-information library this field was waiting on.
        result.is_provisional = false;
        return result;
    }();

    return s;
}

nx::bench::load_sample nx::bench::sample_load()
{
    auto s = load_sample{};

    // Two milliseconds: long enough that the steady clock's resolution is noise, short enough that taking one before
    // and one after a run costs nothing anybody notices.
    auto const wall_start = cc::current_time_steady_secs();
    auto const ticks_start = cc::current_cycles();

    auto wall_end = wall_start;
    while (wall_end - wall_start < 0.002)
        wall_end = cc::current_time_steady_secs();

    auto const ticks_end = cc::current_cycles();
    auto const elapsed = wall_end - wall_start;
    if (elapsed > 0)
        s.ticks_per_ns = f64(ticks_end - ticks_start) / (elapsed * 1e9);

    s.cpu_busy_fraction = impl::sample_cpu_busy_fraction();
    return s;
}

bool nx::bench::try_pin_to_core(int core)
{
    return impl::pin_current_thread_to_core(core);
}

void nx::bench::unpin()
{
    impl::unpin_current_thread();
}
