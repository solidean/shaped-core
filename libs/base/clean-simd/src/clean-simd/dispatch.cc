#include <clean-core/platform/cpu_features.hh>
#include <clean-simd/dispatch.hh>

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
    g_forced = true;
    g_forced_id = id;
}

void cimd::reset_forced_kernel()
{
    g_forced = false;
}

bool cimd::impl::forced_kernel(kernel_id& out)
{
    if (g_forced)
        out = g_forced_id;
    return g_forced;
}
