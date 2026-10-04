#pragma once

#include <clean-simd/all.hh>
#include <clean-simd/dispatch.hh>

#include <utility>

// Every operation of every element type, on fixed inputs, written out as bytes: what a dispatched kernel computes,
// compared by a floor TU against the scalar kernel's run.
// Dispatched, so it compiles in the kernel TUs: it includes clean-simd and nothing else, and every helper is a template.

/// Where an element type's inputs sit in battery_io::in.
template <class T>
constexpr int battery_index()
{
    using namespace cimd;
    if constexpr (std::is_same_v<T, f32>)
        return 0;
    else if constexpr (std::is_same_v<T, f64>)
        return 1;
    else if constexpr (std::is_same_v<T, i8>)
        return 2;
    else if constexpr (std::is_same_v<T, i16>)
        return 3;
    else if constexpr (std::is_same_v<T, i32>)
        return 4;
    else if constexpr (std::is_same_v<T, i64>)
        return 5;
    else if constexpr (std::is_same_v<T, u8>)
        return 6;
    else if constexpr (std::is_same_v<T, u16>)
        return 7;
    else if constexpr (std::is_same_v<T, u32>)
        return 8;
    else
        return 9;
}

struct battery_io
{
    cimd::kernel_id ran = cimd::kernel_id::scalar;

    // Per element type, three inputs of 1024 bits each — the widest type every kernel holds in eight registers.
    // Floats are finite and within ±1000, so no result depends on NaN or an out-of-range conversion.
    alignas(64) cimd::u8 in[10][3][128] = {};

    // Bit-exact results, appended as bytes in the order the battery wrote them.
    alignas(64) cimd::u8 out[1 << 18] = {};
    int n = 0;

    // mul_add on floats, which fuses only where the kernel has FMA, compared within a tolerance.
    cimd::f64 approx[8192] = {};
    int napprox = 0;

    // rcp_approx and rsqrt_approx, compared within the relative error every kernel promises.
    cimd::f64 estimate[8192] = {};
    int nestimate = 0;
};

template <class V>
void battery_put(battery_io& io, V const& v)
{
    using T = typename V::element_t;
    alignas(64) T tmp[V::lanes];
    v.store(tmp);
    auto const* bytes = reinterpret_cast<cimd::u8 const*>(tmp);
    for (auto i = 0; i < int(sizeof(tmp)); ++i)
        io.out[io.n++] = bytes[i];
}

template <class T>
void battery_put_scalar(battery_io& io, T x)
{
    auto const* bytes = reinterpret_cast<cimd::u8 const*>(&x);
    for (auto i = 0; i < int(sizeof(T)); ++i)
        io.out[io.n++] = bytes[i];
}

/// Lane i takes lane i + 1, the last lane the first: a shuffle whose pattern depends on N.
template <class V, int... I>
V battery_rotate(V const& v, std::integer_sequence<int, I...>)
{
    return v.template shuffle<((I + 1) % V::lanes)...>();
}

template <class V>
void battery_estimate(battery_io& io, V const& v)
{
    alignas(64) typename V::element_t tmp[V::lanes];
    v.store(tmp);
    for (auto const x : tmp)
        io.estimate[io.nestimate++] = cimd::f64(x);
}

template <class K, class T, int N>
void battery_element(battery_io& io)
{
    using V = cimd::simd<T, N, K>;
    using M = typename V::mask_t;

    auto const* in = reinterpret_cast<T const*>(io.in[battery_index<T>()]);
    auto const a = V::load(in);
    auto const b = V::load(in + 128 / sizeof(T));
    auto const c = V::load(in + 256 / sizeof(T));

    battery_put(io, a.add(b));
    battery_put(io, a.sub(b));
    battery_put(io, a.mul(b));
    battery_put(io, a.min(b));
    battery_put(io, a.max(b));
    battery_put(io, V::iota());
    battery_put(io, V::zero());
    battery_put(io, V(T(3)));
    battery_put(io, a.lt(b).select(a, c));
    battery_put(io, a.ge(c).select(b, T(0)));

    M const masks[] = {a.eq(b), a.ne(b), a.lt(b), a.le(b), a.gt(b), a.ge(b), a.lt(b) & b.lt(c), a.lt(b) | ~b.lt(c)};
    for (auto const m : masks)
    {
        if constexpr (N <= 64)
            battery_put_scalar<cimd::u64>(io, cimd::u64(m.bits()));
        battery_put_scalar<cimd::u8>(io, cimd::u8(cimd::u8(m.any()) | cimd::u8(m.all()) << 1 | cimd::u8(m.none()) << 2));
    }
    if constexpr (N <= 64)
        battery_put_scalar<cimd::u64>(io, cimd::u64(M::from_bits(typename M::bits_t(0x5A5A5A5A5A5A5A5Aull)).bits()));

    // Index vectors from the signed integer of T's width: any bits for permute, which reads only the low ones, and
    // masked into the three input blocks for gather.
    using I = std::conditional_t<
        sizeof(T) == 1, cimd::i8,
        std::conditional_t<sizeof(T) == 2, cimd::i16, std::conditional_t<sizeof(T) == 4, cimd::i32, cimd::i64>>>;
    using IV = cimd::simd<I, N, K>;
    auto const idx = IV::load(reinterpret_cast<I const*>(io.in[battery_index<I>()][2]));
    battery_put(io, a.reverse());
    battery_put(io, battery_rotate(a, std::make_integer_sequence<int, N>{}));
    battery_put(io, a.permute(idx));
    battery_put(io, V::gather(in, idx.bit_and(IV(I(256 / sizeof(T) - 1)))));

    battery_put_scalar<T>(io, a.reduce_add());
    battery_put_scalar<T>(io, a.reduce_min());
    battery_put_scalar<T>(io, a.reduce_max());

    if constexpr (std::is_floating_point_v<T>)
    {
        battery_put(io, a.neg());
        battery_put(io, a.neg().abs());
        battery_put(io, a.div(b));
        battery_put(io, a.abs().sqrt());
        battery_put(io, a.floor());
        battery_put(io, a.ceil());
        battery_put(io, a.round());
        battery_put(io, a.trunc());
        battery_put(io, a.copysign(b));
        battery_put(io, a.template convert<I>());
        // Lanes beyond the integer's range both ways, and NaN, where a.lt(b) does not hold.
        auto const big = a.mul(V(T(sizeof(T) == 4 ? 1e7 : 1e17)));
        auto const zero = V::zero();
        battery_put(io, a.lt(b).select(big, zero.div(zero)).template convert_saturating<I>());
        auto const x = a.abs().add(V(T(0.5)));
        battery_estimate(io, x.rcp_approx());
        battery_estimate(io, x.neg().rcp_approx());
        battery_estimate(io, x.rsqrt_approx());
        alignas(64) T fused[N];
        a.mul_add(b, c).store(fused);
        for (auto i = 0; i < N; ++i)
            io.approx[io.napprox++] = cimd::f64(fused[i]);
    }
    else
    {
        battery_put(io, a.bit_and(b));
        battery_put(io, a.bit_or(b));
        battery_put(io, a.bit_xor(b));
        battery_put(io, a.bit_not());
        battery_put(io, a.mul_add(b, c));
        battery_put(io, a.shl(3));
        battery_put(io, a.shr(int(sizeof(T)) * 8 - 1));
        battery_put(io, a.shr(1));
        if constexpr (std::is_signed_v<T>)
        {
            battery_put(io, a.neg());
            battery_put(io, a.abs());
        }
        if constexpr (sizeof(T) == 4 || sizeof(T) == 8)
        {
            using F = std::conditional_t<sizeof(T) == 4, cimd::f32, cimd::f64>;
            battery_put(io, a.template convert<F>());
        }
    }
}

template <class K, class T>
void battery_type(battery_io& io)
{
    constexpr int lanes128 = 16 / int(sizeof(T));
    battery_element<K, T, lanes128>(io);
    battery_element<K, T, lanes128 * 2>(io);
    battery_element<K, T, lanes128 * 4>(io);
    battery_element<K, T, lanes128 * 8>(io);
}

template <class K>
void battery(battery_io& io)
{
    io.ran = K::id;
    io.n = 0;
    io.napprox = 0;
    io.nestimate = 0;
    battery_type<K, cimd::f32>(io);
    battery_type<K, cimd::f64>(io);
    battery_type<K, cimd::i8>(io);
    battery_type<K, cimd::i16>(io);
    battery_type<K, cimd::i32>(io);
    battery_type<K, cimd::i64>(io);
    battery_type<K, cimd::u8>(io);
    battery_type<K, cimd::u16>(io);
    battery_type<K, cimd::u32>(io);
    battery_type<K, cimd::u64>(io);
}

CIMD_DISPATCH_DECLARE(cimd_battery, battery);
