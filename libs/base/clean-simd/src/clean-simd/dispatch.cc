#include <clean-core/common/log.hh>
#include <clean-core/platform/cpu_features.hh>
#include <clean-core/record/domain.hh>
#include <clean-simd/dispatch.hh>

namespace cimd
{
CC_REC_DEFINE_DOMAIN(g_rec_domain, "cimd");
} // namespace cimd

cc::atomic<int> cimd::impl::g_active_forces = 0;

namespace
{
thread_local bool g_forced = false;
thread_local cimd::kernel_id g_forced_id = cimd::kernel_id::scalar;
} // namespace

bool cimd::cpu_supports(kernel_id id)
{
    auto const& f = cc::get_cpu_features();
    switch (id)
    {
    case kernel_id::scalar:
        return true;
    case kernel_id::sse2:
        return f.sse2;
    case kernel_id::sse42:
        return f.x86_64_v2;
    case kernel_id::avx2:
        return f.x86_64_v3;
    case kernel_id::avx512:
        return f.x86_64_v4;
    case kernel_id::neon:
        return f.neon;
    case kernel_id::simd128:
        return f.wasm_simd128;
    }
    return false;
}

void cimd::force_kernel(kernel_id id)
{
    if (!g_forced)
        impl::g_active_forces.fetch_add(1, cc::memory_order_relaxed);
    g_forced = true;
    g_forced_id = id;
}

void cimd::reset_forced_kernel()
{
    if (g_forced)
        impl::g_active_forces.fetch_sub(1, cc::memory_order_relaxed);
    g_forced = false;
}

cimd::scoped_forced_kernel::scoped_forced_kernel(kernel_id id)
{
    _had_previous = impl::forced_kernel(_previous);
    force_kernel(id);
}

cimd::scoped_forced_kernel::~scoped_forced_kernel()
{
    if (_had_previous)
        force_kernel(_previous);
    else
        reset_forced_kernel();
}

bool cimd::impl::forced_kernel(kernel_id& out)
{
    if (g_forced)
        out = g_forced_id;
    return g_forced;
}

void cimd::impl::note_resolved(char const* name, kernel_id id)
{
    CC_LOG_INFO("{}: {}", name, kernel_name(id));
}

void cimd::impl::note_unhonoured_force(char const* name, kernel_id forced, kernel_id used)
{
    CC_LOG_DEBUG("{}: {} is forced but not in the table or not on this CPU; running {}", name, kernel_name(forced),
                 kernel_name(used));
}
