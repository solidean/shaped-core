#include "battery.hh"

#include <clean-core/math/bit.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// The battery is dispatched through cimd_dispatch, so a kernel above the build's floor runs here the way it would in
// production: compiled in its own TU with its own flags, selected per CPU.
// Every kernel is compared with the scalar kernel's run, bit for bit except mul_add.

namespace
{
cc::unique_ptr<battery_io> random_io(cc::random& rng)
{
    auto io = cc::make_unique<battery_io>();
    for (auto k = 0; k < 3; ++k)
        for (auto i = 0; i < 32; ++i)
        {
            io->f_in[k][i] = rng.uniform(-1000.f, 1000.f);
            io->i_in[k][i] = i32(rng.next_u32());
            io->u_in[k][i] = rng.next_u32();
        }
    return io;
}

cc::unique_ptr<battery_io> run_on(cimd::kernel_id id, battery_io const& inputs)
{
    auto io = cc::make_unique<battery_io>(inputs);
    cimd::scoped_forced_kernel const forced(id);
    CIMD_DISPATCH(cimd_battery)(*io);
    return io;
}

void compare_with_scalar(cimd::kernel_id id)
{
    auto rng = nx::test_random();
    auto const inputs = random_io(rng);
    auto const reference = run_on(cimd::kernel_id::scalar, *inputs);
    auto const got = run_on(id, *inputs);
    auto const kn = cimd::kernel_name(id);

    REQUIRE(reference->ran == cimd::kernel_id::scalar);
    REQUIRE(got->ran == id);
    REQUIRE(got->nf == reference->nf);
    REQUIRE(got->ni == reference->ni);
    REQUIRE(got->nu == reference->nu);
    REQUIRE(got->nfma == reference->nfma);

    auto mismatches = 0;
    for (auto i = 0; i < got->nf; ++i)
        mismatches += cc::bit_cast<u32>(got->f_out[i]) != cc::bit_cast<u32>(reference->f_out[i]) ? 1 : 0;
    for (auto i = 0; i < got->ni; ++i)
        mismatches += got->i_out[i] != reference->i_out[i] ? 1 : 0;
    for (auto i = 0; i < got->nu; ++i)
        mismatches += got->u_out[i] != reference->u_out[i] ? 1 : 0;
    CHECK(mismatches == 0).context(kn);

    // mul_add: one rounding or two, each within an f32 ulp of what it rounds.
    for (auto i = 0; i < got->nfma; ++i)
    {
        auto const diff = f64(got->fma_out[i]) - f64(reference->fma_out[i]);
        auto const mag = f64(reference->fma_out[i]);
        CHECK((diff < 0 ? -diff : diff) <= (mag < 0 ? -mag : mag) * 0x1p-22 + 0x1p-10).context(kn);
    }
}

void dispatch_kernel_test(cimd::kernel_id id)
{
    if (!cimd::cpu_supports(id))
        SKIP(cc::format("no {} on this CPU", cimd::kernel_name(id)));
    compare_with_scalar(id);
}
} // namespace

TEST("cimd dispatch - the best kernel this CPU runs is the one picked")
{
    auto rng = nx::test_random();
    auto const inputs = random_io(rng);
    auto io = cc::make_unique<battery_io>(*inputs);
    CIMD_DISPATCH(cimd_battery)(*io);
    CHECK(cimd::cpu_supports(io->ran));
    CHECK(io->ran != cimd::kernel_id::scalar);
}

TEST("cimd dispatch - a kernel the CPU cannot run is never forced")
{
    auto rng = nx::test_random();
    auto const inputs = random_io(rng);
    for (auto const id : {cimd::kernel_id::sse2, cimd::kernel_id::sse42, cimd::kernel_id::avx2, cimd::kernel_id::avx512,
                          cimd::kernel_id::neon, cimd::kernel_id::simd128})
    {
        auto const got = run_on(id, *inputs);
        CHECK(cimd::cpu_supports(got->ran)).context(cimd::kernel_name(id));
    }
}

#if defined(CC_ARCH_X64)
TEST("cimd dispatch - sse2 agrees with scalar")
{
    dispatch_kernel_test(cimd::kernel_id::sse2);
}

TEST("cimd dispatch - sse42 agrees with scalar")
{
    dispatch_kernel_test(cimd::kernel_id::sse42);
}

TEST("cimd dispatch - avx2 agrees with scalar")
{
    dispatch_kernel_test(cimd::kernel_id::avx2);
}

TEST("cimd dispatch - avx512 agrees with scalar")
{
    dispatch_kernel_test(cimd::kernel_id::avx512);
}
#endif

#if defined(CC_ARCH_ARM64)
TEST("cimd dispatch - neon agrees with scalar")
{
    dispatch_kernel_test(cimd::kernel_id::neon);
}
#endif

#if defined(__wasm_simd128__)
TEST("cimd dispatch - simd128 agrees with scalar")
{
    dispatch_kernel_test(cimd::kernel_id::simd128);
}
#endif
