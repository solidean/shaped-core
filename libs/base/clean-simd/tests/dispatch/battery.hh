#pragma once

#include <clean-simd/all.hh>
#include <clean-simd/dispatch.hh>

// Every slice operation, on fixed inputs, written out as plain lanes: what a dispatched kernel computes, compared by a
// floor TU against the scalar kernel's run.
// Dispatched, so it compiles in the kernel TUs: it includes clean-simd and nothing else, and every helper is a template.

struct battery_io
{
    cimd::kernel_id ran = cimd::kernel_id::scalar;

    cimd::f32 f_in[3][32] = {};
    cimd::i32 i_in[3][32] = {};
    cimd::u32 u_in[3][32] = {};

    // Bit-exact results, in the order the battery wrote them.
    cimd::f32 f_out[4096] = {};
    cimd::i32 i_out[4096] = {};
    cimd::u32 u_out[4096] = {};
    int nf = 0;
    int ni = 0;
    int nu = 0;

    // mul_add, which fuses only where the kernel has FMA.
    cimd::f32 fma_out[256] = {};
    int nfma = 0;
};

template <class T>
T* battery_out(battery_io& io)
{
    if constexpr (std::is_same_v<T, cimd::f32>)
        return io.f_out + io.nf;
    else if constexpr (std::is_same_v<T, cimd::i32>)
        return io.i_out + io.ni;
    else
        return io.u_out + io.nu;
}

template <class T>
void battery_advance(battery_io& io, int n)
{
    if constexpr (std::is_same_v<T, cimd::f32>)
        io.nf += n;
    else if constexpr (std::is_same_v<T, cimd::i32>)
        io.ni += n;
    else
        io.nu += n;
}

template <class V>
void battery_put(battery_io& io, V const& v)
{
    using T = typename V::element_t;
    v.store(battery_out<T>(io));
    battery_advance<T>(io, V::lanes);
}

template <class T>
void battery_put_scalar(battery_io& io, T x)
{
    *battery_out<T>(io) = x;
    battery_advance<T>(io, 1);
}

template <class T>
T const* battery_in(battery_io const& io, int which)
{
    if constexpr (std::is_same_v<T, cimd::f32>)
        return io.f_in[which];
    else if constexpr (std::is_same_v<T, cimd::i32>)
        return io.i_in[which];
    else
        return io.u_in[which];
}

template <class K, class T, int N>
void battery_element(battery_io& io)
{
    using V = cimd::simd<T, N, K>;
    using M = typename V::mask_t;

    auto const a = V::load(battery_in<T>(io, 0));
    auto const b = V::load(battery_in<T>(io, 1));
    auto const c = V::load(battery_in<T>(io, 2));

    battery_put(io, a + b);
    battery_put(io, a - b);
    battery_put(io, a * b);
    battery_put(io, a.min(b));
    battery_put(io, a.max(b));
    battery_put(io, V::iota());
    battery_put(io, V::zero());
    battery_put(io, V(T(3)));
    battery_put(io, (a < b).select(a, c));
    battery_put(io, (a >= c).select(b, T(0)));

    M const masks[] = {a == b, a != b, a<b, a <= b, a> b, a >= b, (a < b) & (b < c), (a < b) | ~(b < c)};
    for (auto const m : masks)
    {
        battery_put_scalar<cimd::u32>(io, cimd::u32(m.bits()));
        battery_put_scalar<cimd::u32>(io, cimd::u32(m.any()) | cimd::u32(m.all()) << 1 | cimd::u32(m.none()) << 2);
    }
    battery_put_scalar<cimd::u32>(io, cimd::u32(M::from_bits(0x5A5A5A5Au).bits()));

    battery_put_scalar<T>(io, a.reduce_add());
    battery_put_scalar<T>(io, a.reduce_min());
    battery_put_scalar<T>(io, a.reduce_max());

    if constexpr (std::is_same_v<T, cimd::f32>)
    {
        battery_put(io, -a);
        battery_put(io, (-a).abs());
        battery_put(io, a.template convert<cimd::i32>());
        a.mul_add(b, c).store(io.fma_out + io.nfma);
        io.nfma += N;
    }
    else
    {
        battery_put(io, a & b);
        battery_put(io, a | b);
        battery_put(io, a ^ b);
        battery_put(io, ~a);
        battery_put(io, a.mul_add(b, c));
        battery_put(io, a.template convert<cimd::f32>());
        if constexpr (std::is_same_v<T, cimd::i32>)
        {
            battery_put(io, -a);
            battery_put(io, a.abs());
        }
    }
}

template <class K>
void battery(battery_io& io)
{
    io.ran = K::id;
    io.nf = io.ni = io.nu = io.nfma = 0;
    battery_element<K, cimd::f32, 4>(io);
    battery_element<K, cimd::f32, 8>(io);
    battery_element<K, cimd::f32, 16>(io);
    battery_element<K, cimd::f32, 32>(io);
    battery_element<K, cimd::i32, 4>(io);
    battery_element<K, cimd::i32, 8>(io);
    battery_element<K, cimd::i32, 16>(io);
    battery_element<K, cimd::i32, 32>(io);
    battery_element<K, cimd::u32, 4>(io);
    battery_element<K, cimd::u32, 8>(io);
    battery_element<K, cimd::u32, 16>(io);
    battery_element<K, cimd::u32, 32>(io);
}

CIMD_DISPATCH_DECLARE(cimd_battery, battery);
