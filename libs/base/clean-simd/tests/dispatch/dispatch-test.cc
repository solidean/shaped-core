#include "battery.hh"

#include <clean-core/error/optional.hh>
#include <clean-core/math/bit.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// The battery is dispatched through cimd_dispatch, so a kernel above the build's floor runs here the way it would in
// production: compiled in its own TU with its own flags, selected per CPU.
// Every kernel is compared with the scalar kernel's run, bit for bit except mul_add and the estimates.

namespace
{
using battery_fn = void (*)(battery_io&);

// The ten entries, one per element type in battery_index order, run one after another into one battery_io.
struct battery_entry
{
    battery_fn (*dispatch)();
    cimd::kernel_id (*kernel)();
};

#define BATTERY_ENTRY(T)                                                                                      \
    battery_entry                                                                                             \
    {                                                                                                         \
        [] { return CIMD_DISPATCH(cimd_battery_##T); }, [] { return CIMD_DISPATCH_KERNEL(cimd_battery_##T); } \
    }

battery_entry const battery_entries[]
    = {BATTERY_ENTRY(f32), BATTERY_ENTRY(f64), BATTERY_ENTRY(i8),  BATTERY_ENTRY(i16), BATTERY_ENTRY(i32),
       BATTERY_ENTRY(i64), BATTERY_ENTRY(u8),  BATTERY_ENTRY(u16), BATTERY_ENTRY(u32), BATTERY_ENTRY(u64)};

#undef BATTERY_ENTRY

// The kernel every entry's CIMD_DISPATCH_KERNEL names, or nothing where two disagree.
cc::optional<cimd::kernel_id> battery_kernel()
{
    auto const id = battery_entries[0].kernel();
    for (auto const& e : battery_entries)
        if (e.kernel() != id)
            return cc::nullopt;
    return id;
}

void run_battery(battery_io& io)
{
    io.n = 0;
    io.napprox = 0;
    io.nestimate = 0;
    for (auto const& e : battery_entries)
        e.dispatch()(io);
}

template <class T>
void fill_floats(battery_io& io, cc::random& rng)
{
    for (auto k = 0; k < 3; ++k)
    {
        auto* lanes = reinterpret_cast<T*>(io.in[battery_index<T>()][k]);
        for (auto i = 0; i < int(128 / sizeof(T)); ++i)
            lanes[i] = T(rng.uniform(-1000.f, 1000.f));
    }
}

cc::unique_ptr<battery_io> random_io(cc::random& rng)
{
    auto io = cc::make_unique<battery_io>();
    // Integers take any bits; floats stay finite and in range, so nothing compared depends on NaN.
    for (auto& element : io->in)
        for (auto& input : element)
            for (auto& byte : input)
                byte = u8(rng.next_u32());
    fill_floats<f32>(*io, rng);
    fill_floats<f64>(*io, rng);
    return io;
}

cc::unique_ptr<battery_io> run_on(cimd::kernel_id id, battery_io const& inputs)
{
    auto io = cc::make_unique<battery_io>(inputs);
    cimd::scoped_forced_kernel const forced(id);
    run_battery(*io);
    return io;
}

void compare_with_scalar(cimd::kernel_id id)
{
    auto rng = nx::test_random();
    auto const inputs = random_io(rng);
    auto const reference = run_on(cimd::kernel_id::scalar, *inputs);
    auto const got = run_on(id, *inputs);
    auto const kn = cimd::kernel_name(id);

    // Ten element types, a few dozen results each, at four widths or at one: fewer bytes means some were skipped.
    REQUIRE(reference->n >= (CIMD_EXHAUSTIVE_TESTS ? 79880 : 12638)).dump("bytes", reference->n);
    for (auto t = 0; t < 10; ++t)
    {
        REQUIRE(reference->ran[t] == cimd::kernel_id::scalar).dump("entry", t);
        REQUIRE(got->ran[t] == id).dump("entry", t);
        REQUIRE(got->written[t] > 0).dump("entry", t);
        REQUIRE(got->written[t] == reference->written[t]).dump("entry", t);
    }
    REQUIRE(got->n == reference->n);
    REQUIRE(got->napprox == reference->napprox);
    REQUIRE(got->nestimate == reference->nestimate);

    auto first_mismatch = -1;
    auto mismatches = 0;
    for (auto i = 0; i < got->n; ++i)
        if (got->out[i] != reference->out[i])
        {
            first_mismatch = first_mismatch < 0 ? i : first_mismatch;
            ++mismatches;
        }
    CHECK(mismatches == 0).context(kn).dump("first mismatching byte", first_mismatch);

    // mul_add: one rounding or two, each within an f32 ulp of what it rounds.
    for (auto i = 0; i < got->napprox; ++i)
    {
        auto const diff = got->approx[i] - reference->approx[i];
        auto const mag = reference->approx[i];
        CHECK((diff < 0 ? -diff : diff) <= (mag < 0 ? -mag : mag) * 0x1p-22 + 0x1p-10).context(kn);
    }

    // rcp_approx and rsqrt_approx: the scalar kernel's are exact, and every kernel promises 11 bits.
    for (auto i = 0; i < got->nestimate; ++i)
    {
        auto const diff = got->estimate[i] - reference->estimate[i];
        auto const mag = reference->estimate[i];
        CHECK((diff < 0 ? -diff : diff) <= (mag < 0 ? -mag : mag) * 0x1p-11).context(kn);
    }
}

void dispatch_kernel_test(cimd::kernel_id id)
{
    if (!cimd::cpu_supports(id))
        SKIP(cc::format("no {} on this CPU", cimd::kernel_name(id)));
    {
        cimd::scoped_forced_kernel const forced(id);
        auto const kernel = battery_kernel();
        REQUIRE(kernel.has_value());
        if (kernel.value() != id)
            SKIP(cc::format("{} is not compiled into this build", cimd::kernel_name(id)));
    }
    compare_with_scalar(id);
}
} // namespace

TEST("cimd dispatch - the best kernel this CPU runs is the one picked")
{
    // The strongest kernel both in the table and on this CPU; the table lists them weakest first.
    auto expected = cimd::kernel_id::scalar;
    for (auto const id : {cimd::kernel_id::sse2, cimd::kernel_id::sse42, cimd::kernel_id::avx2, cimd::kernel_id::avx512,
                          cimd::kernel_id::neon, cimd::kernel_id::simd128})
    {
        if (!cimd::cpu_supports(id))
            continue;
        cimd::scoped_forced_kernel const forced(id);
        if (battery_kernel() == id)
            expected = id;
    }

    auto rng = nx::test_random();
    auto const inputs = random_io(rng);
    auto io = cc::make_unique<battery_io>(*inputs);
    REQUIRE(battery_kernel() == expected).context(cimd::kernel_name(expected));
    run_battery(*io);
    for (auto const ran : io->ran)
        CHECK(ran == expected).context(cimd::kernel_name(expected));
}

TEST("cimd dispatch - a scoped force restores the one around it")
{
    auto const best = CIMD_DISPATCH_KERNEL(cimd_battery_f32);
    {
        cimd::scoped_forced_kernel const outer(cimd::kernel_id::scalar);
        CHECK(CIMD_DISPATCH_KERNEL(cimd_battery_f32) == cimd::kernel_id::scalar);
        {
            cimd::scoped_forced_kernel const inner(best);
            CHECK(CIMD_DISPATCH_KERNEL(cimd_battery_f32) == best);
        }
        CHECK(CIMD_DISPATCH_KERNEL(cimd_battery_f32) == cimd::kernel_id::scalar);
    }
    CHECK(CIMD_DISPATCH_KERNEL(cimd_battery_f32) == best);
}

TEST("cimd dispatch - a kernel the CPU cannot run is never forced")
{
    auto rng = nx::test_random();
    auto const inputs = random_io(rng);
    for (auto const id : {cimd::kernel_id::sse2, cimd::kernel_id::sse42, cimd::kernel_id::avx2, cimd::kernel_id::avx512,
                          cimd::kernel_id::neon, cimd::kernel_id::simd128})
    {
        auto const got = run_on(id, *inputs);
        for (auto const ran : got->ran)
            CHECK(cimd::cpu_supports(ran)).context(cimd::kernel_name(id));
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
