// fixed_int benchmarks: the choices the design left to measurement.
//
// - Signed multiply: the generated bodies multiply the raw words unsigned and subtract a masked correction per negative
//   operand (A).
//   Measured against sign-extending both operands first (B, which is what the generic body does), taking magnitudes and
//   negating the product (C), and clang's own _BitInt lowering.
//   A itself is timed in both of the generator's forms, row by row and column by column, which is how the generator
//   picks one per triple.
// - A quotient known to fit 32 bits: tg::div_floor_ceil<fi32> estimates with one cc::udiv128 step and corrects from one
//   exact remainder (C).
//   Measured against an f64 estimate with the same correction (B), an f64 division trusted outside an epsilon band
//   around the integers (A), and a full long division.
//   On a Zen 4, C is ~30% faster than B and ~2.2x faster than the long division.
//   A is no faster than B, so skipping the exact correction does not pay for the error analysis it rests on.
// - to_f64 correctly rounded against the variant without the sticky bit; the sticky bit costs ~20%, and stays.
//
// Run with
//   uv run dev.py benchmark "tg fixed_int"
// and read one body with
//   uv run dev.py assembly search "probe_" --target typed-geometry-test --preset release-clang

#include "fixed_int-mul-variants.gen.hh"

#include <clean-core/common/macros.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/math/random.hh>
#include <nexus/bench/run.hh>
#include <nexus/test.hh>
#include <typed-geometry/scalar/fixed_int/fixed_arith.hh>

using namespace cc::primitive_defines;
using tg::fi128;
using tg::fi192;
using tg::fi256;
using tg::fi32;
using tg::fi64;

namespace
{
constexpr isize input_count = 1024;

template <class T>
T random_fixed(cc::random& rng, int bits)
{
    T r;
    for (auto i = 0; i < T::limb_count; ++i)
        r.limbs[i] = rng.next_u64();
    return r >> (T::bits - bits); // arithmetic, so the value is a signed bits-bit number
}

/// C: magnitudes, an unsigned product, and the sign put back with a mask.
template <int R, int A, int B>
tg::fixed_int<R> mul_by_magnitudes(tg::fixed_int<A> const& a, tg::fixed_int<B> const& b)
{
    auto const m = tg::impl::mul_op<R, A, B, false>::apply(tg::impl::magnitude(a), tg::impl::magnitude(b));
    auto const negative = a.is_negative() != b.is_negative();
    tg::fixed_uint<R> mask;
    for (auto i = 0; i < mask.limb_count; ++i)
        mask.limbs[i] = tg::impl::mask_if(negative);
    return tg::fixed_int<R>((m ^ mask) - mask);
}

/// B for the quotient: an f64 estimate, within 2^-20 of the quotient (each conversion is within ~2^-52 and the
/// division adds 2^-53), so its floor is floor(x / w) or one off it.
template <int A, int B>
tg::floor_ceil_result<i64> quotient_by_f64(tg::fixed_int<A> const& x, tg::fixed_int<B> const& w)
{
    auto const qd = tg::impl::to_f64_generic<false>(x) / tg::impl::to_f64_generic<false>(w);
    auto t = i64(qd);
    if (f64(t) > qd)
        --t;
    return tg::impl::correct_quotient(x, w, t);
}

/// A: the f64 quotient, trusted unless it lands within an epsilon of an integer, where only an exact check can decide.
/// The epsilon comes from the bound: |q| < 2^26 and a few units of f64 rounding give an error below 2^-24.
template <int A, int B>
tg::floor_ceil_result<i64> quotient_by_epsilon(tg::fixed_int<A> const& x, tg::fixed_int<B> const& w)
{
    auto const qd = x.to_f64() / w.to_f64();
    auto t = i64(qd);
    if (f64(t) > qd)
        --t;
    auto const frac = qd - f64(t);
    constexpr f64 eps = 0x1p-24;
    if (frac > eps && frac < 1.0 - eps)
        return {t, t + 1};
    return tg::impl::correct_quotient(x, w, t);
}

template <int R, int A, int B>
struct mul_inputs
{
    cc::vector<tg::fixed_int<A>> a;
    cc::vector<tg::fixed_int<B>> b;

    explicit mul_inputs(int bits_a, int bits_b)
    {
        auto rng = cc::random(0xF1C5);
        for (isize i = 0; i < input_count; ++i)
        {
            a.push_back(random_fixed<tg::fixed_int<A>>(rng, bits_a));
            b.push_back(random_fixed<tg::fixed_int<B>>(rng, bits_b));
        }
    }

    /// One pass over every pair, folding the results so none of them is dead.
    template <class F>
    void run(char const* name, F&& f) const
    {
        nx::bench::run(name,
                       [&](nx::bench::iteration& it)
                       {
                           u64 acc = 0;
                           for (isize i = 0; i < input_count; ++i)
                           {
                               // the input is kept through its address, exactly as the _BitInt loop does
                               auto const r = f(*nx::bench::keep(&a[i]), b[i]);
                               for (auto k = 0; k < r.limb_count; ++k)
                                   acc ^= r.limbs[k];
                           }
                           nx::bench::sink(acc);
                           it.items(input_count);
                       });
    }
};

template <int R, int A, int B, class Row, class Column>
void bench_signed_mul(int bits_a, int bits_b, Row row, Column column)
{
    auto const in = mul_inputs<R, A, B>(bits_a, bits_b);
    in.run("A generated: unsigned product - masked corrections",
           [](auto const& a, auto const& b) { return tg::impl::mul_op<R, A, B, true>::apply(a, b); });
    // the same A, with its partial products summed row by row and column by column, timed in one run
    in.run("A, row by row", row);
    in.run("A, column by column", column);
    in.run("B generic: sign-extend, then unsigned",
           [](auto const& a, auto const& b) { return tg::impl::mul_generic<R, true>(a, b); });
    in.run("C magnitudes, product, masked negate",
           [](auto const& a, auto const& b) { return mul_by_magnitudes<R>(a, b); });
#if defined(CC_COMPILER_CLANG)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wbit-int-extension"
    // _BitInt holds its own copy of the inputs, so the conversion is outside the loop
    auto xs = cc::vector<_BitInt(A)>();
    auto ys = cc::vector<_BitInt(B)>();
    for (isize i = 0; i < input_count; ++i)
    {
        // assembled at 320 bits, where shifting a word in is defined at every width, then truncated
        unsigned _BitInt(320) x = 0;
        unsigned _BitInt(320) y = 0;
        for (auto k = in.a[i].limb_count - 1; k >= 0; --k)
            x = x << 64 | in.a[i].limbs[k];
        for (auto k = in.b[i].limb_count - 1; k >= 0; --k)
            y = y << 64 | in.b[i].limbs[k];
        xs.push_back(_BitInt(A)(x));
        ys.push_back(_BitInt(B)(y));
    }
    nx::bench::run("clang _BitInt",
                   [&](nx::bench::iteration& it)
                   {
                       u64 acc = 0;
                       for (isize i = 0; i < input_count; ++i)
                       {
                           // keep() takes the address: its asm constraint has no register class for a _BitInt
                           auto const r = _BitInt(R)(*nx::bench::keep(&xs[i])) * _BitInt(R)(ys[i]);
                           // every word is folded: a word nothing reads is a word the compiler never computes
                           for (auto k = 0; k < R / 64; ++k)
                               acc ^= u64(r >> (64 * k));
                       }
                       nx::bench::sink(acc);
                       it.items(input_count);
                   });
#pragma clang diagnostic pop
#endif
}
} // namespace

BENCHMARK("tg fixed_int - signed multiply, fi128 x fi128 -> fi192")
{
    bench_signed_mul<192, 128, 128>(
        80, 90, [](auto const& a, auto const& b) { return tg::impl::mul_variants::row_192_128_128(a, b); },
        [](auto const& a, auto const& b) { return tg::impl::mul_variants::column_192_128_128(a, b); });
}

BENCHMARK("tg fixed_int - signed multiply, fi128 x fi128 -> fi256")
{
    bench_signed_mul<256, 128, 128>(
        127, 127, [](auto const& a, auto const& b) { return tg::impl::mul_variants::row_256_128_128(a, b); },
        [](auto const& a, auto const& b) { return tg::impl::mul_variants::column_256_128_128(a, b); });
}

BENCHMARK("tg fixed_int - signed multiply, fi64 x fi192 -> fi256")
{
    bench_signed_mul<256, 64, 192>(
        63, 180, [](auto const& a, auto const& b) { return tg::impl::mul_variants::row_256_64_192(a, b); },
        [](auto const& a, auto const& b) { return tg::impl::mul_variants::column_256_64_192(a, b); });
}

BENCHMARK("tg fixed_int - quotient known to fit fi32, fi256 / fi192")
{
    auto rng = cc::random(0xD1F);
    auto xs = cc::vector<fi256>();
    auto ws = cc::vector<fi192>();
    for (isize i = 0; i < input_count; ++i)
    {
        auto const w = random_fixed<fi192>(rng, 1 + int(rng.next_u64() % 180)) | fi192(1);
        auto const q = fi256(i64(rng.next_u64() % (u64(1) << 27)) - (i64(1) << 26));
        xs.push_back(tg::mul<fi256>(q, w) + fi256(i64(rng.next_u64() % 3) - 1));
        ws.push_back(w);
    }

    // A variant that is fast because it is wrong would win the table, so every one is checked against long division first.
    for (isize i = 0; i < input_count; ++i)
    {
        auto const floor = i64(tg::div_floor(xs[i], fi256(ws[i])).limbs[0]);
        auto const ceil = i64(tg::div_ceil(xs[i], fi256(ws[i])).limbs[0]);
        for (auto const r :
             {tg::impl::small_quotient(xs[i], ws[i]), quotient_by_f64(xs[i], ws[i]), quotient_by_epsilon(xs[i], ws[i])})
        {
            CHECK(r.floor == floor);
            CHECK(r.ceil == ceil);
        }
    }

    auto const run = [&](char const* name, auto&& f)
    {
        nx::bench::run(name,
                       [&](nx::bench::iteration& it)
                       {
                           i64 acc = 0;
                           for (isize i = 0; i < input_count; ++i)
                           {
                               auto const r = f(nx::bench::keep(xs[i]), ws[i]);
                               acc += r.floor * 3 + r.ceil;
                           }
                           nx::bench::sink(acc);
                           it.items(input_count);
                       });
    };
    run("C udiv128 estimate, exact correction (shipped)",
        [](fi256 const& x, fi192 const& w) { return tg::impl::small_quotient(x, w); });
    run("B f64 estimate, exact correction", [](fi256 const& x, fi192 const& w) { return quotient_by_f64(x, w); });
    run("A f64, exact only near an integer", [](fi256 const& x, fi192 const& w) { return quotient_by_epsilon(x, w); });
    run("Knuth D long division",
        [](fi256 const& x, fi192 const& w)
        {
            auto const wide = fi256(w);
            return tg::floor_ceil_result<i64>{i64(tg::div_floor(x, wide).limbs[0]), i64(tg::div_ceil(x, wide).limbs[0])};
        });
}

BENCHMARK("tg fixed_int - to_f64, fi256")
{
    auto rng = cc::random(0xF64);
    auto xs = cc::vector<fi256>();
    for (isize i = 0; i < input_count; ++i)
        xs.push_back(random_fixed<fi256>(rng, 1 + int(rng.next_u64() % 256)));

    auto const run = [&](char const* name, auto&& f)
    {
        nx::bench::run(name,
                       [&](nx::bench::iteration& it)
                       {
                           f64 acc = 0;
                           for (isize i = 0; i < input_count; ++i)
                               acc += f(nx::bench::keep(xs[i]));
                           nx::bench::sink(acc);
                           it.items(input_count);
                       });
    };
    run("correctly rounded (sticky bit)", [](fi256 const& x) { return x.to_f64(); });
    run("without the sticky bit", [](fi256 const& x) { return tg::impl::to_f64_generic<false>(x); });
}

// =========================================================================================================
// Named probes: one symbol per body worth reading, for `dev.py assembly show`.
// =========================================================================================================

namespace
{
CC_DONT_INLINE fi192 probe_mul_192_128_128(fi128 const& a, fi128 const& b)
{
    return tg::mul<fi192>(a, b);
}

CC_DONT_INLINE fi256 probe_mul_256_128_128(fi128 const& a, fi128 const& b)
{
    return tg::mul<fi256>(a, b);
}

CC_DONT_INLINE fi192 probe_add_192_128_128(fi128 const& a, fi128 const& b)
{
    return tg::add<fi192>(a, b);
}

CC_DONT_INLINE fi192 probe_shl_192(fi192 const& x, int n)
{
    return x << n;
}

CC_DONT_INLINE tg::floor_ceil_result<fi32> probe_div_floor_ceil(fi256 const& x, fi192 const& w)
{
    return tg::div_floor_ceil<fi32>(x, w);
}
} // namespace

TEST("tg fixed_int - codegen probes")
{
    // Called once each, so the probes survive for the disassembler, and checked, so a probe is never a wrong body.
    CHECK(probe_mul_192_128_128(fi128(-3) << 80, fi128(5) << 90) == fi192(-15) << 170);
    CHECK(probe_mul_256_128_128(fi128::min(), fi128::min()) == fi256(1) << 254);
    CHECK(probe_add_192_128_128(fi128::max(), fi128(1)) == fi192(1) << 127);
    CHECK(probe_shl_192(fi192(3), 130) == fi192(3) << 130);
    auto const q = probe_div_floor_ceil(fi256(-7), fi192(2));
    CHECK(q.floor == fi32(-4));
    CHECK(q.ceil == fi32(-3));
}
