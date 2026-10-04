// f16 benchmarks: the operations tg::half_float computes on its bits, against widening to f32 and narrowing back.
// Comparison, rounding and base two stay on the bits because baseline x64 (SSE2) converts in software and has no
// rounding instruction, while with F16C two conversions cost about what the integer path does.
// These measure that claim on whatever the build targets.
// The default SC_X64_LEVEL=v3 has F16C; a v1 build shows the software side.
//
// Run with
//   uv run dev.py benchmark "tg f16"

#include <clean-core/math/random.hh>
#include <nexus/bench/run.hh>
#include <nexus/test.hh>
#include <typed-geometry/scalar/half_float.hh>
#include <typed-geometry/scalar/scalar.hh>

using namespace cc::primitive_defines;
using tg::f16;

namespace
{
constexpr isize input_count = 1024;

struct inputs
{
    f16 values[input_count] = {};
    int shifts[input_count] = {};
};

inputs make_inputs()
{
    auto rng = cc::random(42);
    auto r = inputs();
    for (auto i = 0; i < input_count; ++i)
    {
        auto h = f16::make_from_bits(u16(rng.next_u32()));
        while (!h.is_finite())
            h = f16::make_from_bits(u16(rng.next_u32()));
        r.values[i] = h;
        r.shifts[i] = rng.uniform(-8, 8);
    }
    return r;
}
} // namespace

BENCHMARK("tg f16 - compare: bits vs f32")
{
    auto const in = make_inputs();

    nx::bench::run("bits",
                   [&](nx::bench::iteration& it)
                   {
                       auto count = 0;
                       for (auto i = 0; i + 1 < input_count; ++i)
                           count += nx::bench::keep(in.values[i]) < in.values[i + 1] ? 1 : 0;
                       nx::bench::sink(count);
                       it.items(input_count - 1);
                   });
    nx::bench::run("through f32",
                   [&](nx::bench::iteration& it)
                   {
                       auto count = 0;
                       for (auto i = 0; i + 1 < input_count; ++i)
                           count += nx::bench::keep(in.values[i]).to_f32() < in.values[i + 1].to_f32() ? 1 : 0;
                       nx::bench::sink(count);
                       it.items(input_count - 1);
                   });
}

BENCHMARK("tg f16 - floor: bits vs f32")
{
    auto const in = make_inputs();

    nx::bench::run("bits",
                   [&](nx::bench::iteration& it)
                   {
                       auto acc = u16(0);
                       for (auto i = 0; i < input_count; ++i)
                           acc ^= tg::floor(nx::bench::keep(in.values[i])).bits();
                       nx::bench::sink(acc);
                       it.items(input_count);
                   });
    nx::bench::run("through f32",
                   [&](nx::bench::iteration& it)
                   {
                       auto acc = u16(0);
                       for (auto i = 0; i < input_count; ++i)
                           acc ^= f16(tg::floor(nx::bench::keep(in.values[i]).to_f32())).bits();
                       nx::bench::sink(acc);
                       it.items(input_count);
                   });
}

BENCHMARK("tg f16 - scale_by_pow2: bits vs f32")
{
    auto const in = make_inputs();

    nx::bench::run("bits",
                   [&](nx::bench::iteration& it)
                   {
                       auto acc = u16(0);
                       for (auto i = 0; i < input_count; ++i)
                           acc ^= tg::scale_by_pow2(nx::bench::keep(in.values[i]), in.shifts[i]).bits();
                       nx::bench::sink(acc);
                       it.items(input_count);
                   });
    nx::bench::run("through f32",
                   [&](nx::bench::iteration& it)
                   {
                       auto acc = u16(0);
                       for (auto i = 0; i < input_count; ++i)
                           acc ^= f16(tg::scale_by_pow2(nx::bench::keep(in.values[i]).to_f32(), in.shifts[i])).bits();
                       nx::bench::sink(acc);
                       it.items(input_count);
                   });
}
