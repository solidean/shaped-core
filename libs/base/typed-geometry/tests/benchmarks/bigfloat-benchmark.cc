// bigfloat benchmarks: the choices the design left to measurement.
//
// - The inline size K, swept over 1, 2 and 3 (16, 24 and 32 bytes).
//   The expectation, to be checked here: K = 2 holds orient2d's products inline, K = 3 orient3d's terms.
// - Operators against the fused msum, which accumulates a sum of products in one window with at most one allocation.
// - Two input regimes: coordinates of similar magnitude, as geometry has, and exponents spread wide, which spill at any K.
// - A bump-arena prototype over the same kernels, against the value type.
// - tg::float_expansion, Shewchuk's expansions, against bigfloat: as a value type, and as its kernels over stack arrays alone.
// The counters benchmark is the one to read on a loaded machine: instructions retired per call barely move under load.
//
// Run with
//   uv run dev.py benchmark "tg bigfloat"

#include <clean-core/container/vector.hh>
#include <clean-core/math/random.hh>
#include <clean-core/string/print.hh>
#include <nexus/bench/hardware_counters.hh>
#include <nexus/bench/run.hh>
#include <nexus/test.hh>
#include <typed-geometry/scalar/bigfloat/bigfloat.hh>
#include <typed-geometry/scalar/float_expansion/float_expansion.hh>
#include <typed-geometry/scalar/scalar.hh>

#include <type_traits>

using namespace cc::primitive_defines;

namespace
{
constexpr isize input_count = 1024;

/// input_count points of 3 coordinates each.
cc::vector<f64> make_points(bool spread)
{
    auto rng = cc::random(7);
    auto points = cc::vector<f64>();
    for (auto i = 0; i < input_count * 3; ++i)
    {
        auto const m = f64(rng.next_u64() >> 11) * 0x1p-53;
        auto const e = spread ? int(rng.next_u64() % 401) - 200 : 0;
        points.push_back(tg::scale_by_pow2(m, e));
    }
    return points;
}

template <int K>
int orient2d_ops(f64 const* p, f64 const* q, f64 const* r)
{
    using bf = tg::bigfloat_sbo<K>;
    auto const ax = bf(p[0]) - bf(r[0]);
    auto const ay = bf(p[1]) - bf(r[1]);
    auto const bx = bf(q[0]) - bf(r[0]);
    auto const by = bf(q[1]) - bf(r[1]);
    return (ax * by - ay * bx).sign();
}

template <int K>
int orient2d_msum(f64 const* p, f64 const* q, f64 const* r)
{
    using bf = tg::bigfloat_sbo<K>;
    auto const ax = bf(p[0]) - bf(r[0]);
    auto const ay = bf(p[1]) - bf(r[1]);
    auto const bx = bf(q[0]) - bf(r[0]);
    auto const by = bf(q[1]) - bf(r[1]);
    return bf::msum({{ax, by}, {ay, bx, true}}).sign();
}

template <int K>
int orient3d_ops(f64 const* a, f64 const* b, f64 const* c, f64 const* d)
{
    using bf = tg::bigfloat_sbo<K>;
    bf u[3], v[3], w[3];
    for (auto i = 0; i < 3; ++i)
    {
        u[i] = bf(a[i]) - bf(d[i]);
        v[i] = bf(b[i]) - bf(d[i]);
        w[i] = bf(c[i]) - bf(d[i]);
    }
    auto const m0 = v[1] * w[2] - v[2] * w[1];
    auto const m1 = v[2] * w[0] - v[0] * w[2];
    auto const m2 = v[0] * w[1] - v[1] * w[0];
    return (u[0] * m0 + u[1] * m1 + u[2] * m2).sign();
}

template <int K>
int orient3d_msum(f64 const* a, f64 const* b, f64 const* c, f64 const* d)
{
    using bf = tg::bigfloat_sbo<K>;
    bf u[3], v[3], w[3];
    for (auto i = 0; i < 3; ++i)
    {
        u[i] = bf(a[i]) - bf(d[i]);
        v[i] = bf(b[i]) - bf(d[i]);
        w[i] = bf(c[i]) - bf(d[i]);
    }
    auto const m0 = bf::msum({{v[1], w[2]}, {v[2], w[1], true}});
    auto const m1 = bf::msum({{v[2], w[0]}, {v[0], w[2], true}});
    auto const m2 = bf::msum({{v[0], w[1]}, {v[1], w[0], true}});
    return bf::msum({{u[0], m0}, {u[1], m1}, {u[2], m2}}).sign();
}

template <int K>
int orient2d_fe(f64 const* p, f64 const* q, f64 const* r)
{
    using fe = tg::float_expansion_sbo<K>;
    auto const ax = fe(p[0]) - fe(r[0]);
    auto const ay = fe(p[1]) - fe(r[1]);
    auto const bx = fe(q[0]) - fe(r[0]);
    auto const by = fe(q[1]) - fe(r[1]);
    return (ax * by - ay * bx).sign();
}

template <int K>
int orient3d_fe(f64 const* a, f64 const* b, f64 const* c, f64 const* d)
{
    using fe = tg::float_expansion_sbo<K>;
    fe u[3], v[3], w[3];
    for (auto i = 0; i < 3; ++i)
    {
        u[i] = fe(a[i]) - fe(d[i]);
        v[i] = fe(b[i]) - fe(d[i]);
        w[i] = fe(c[i]) - fe(d[i]);
    }
    auto const m0 = v[1] * w[2] - v[2] * w[1];
    auto const m1 = v[2] * w[0] - v[0] * w[2];
    auto const m2 = v[0] * w[1] - v[1] * w[0];
    return (u[0] * m0 + u[1] * m1 + u[2] * m2).sign();
}

/// insphere of a, b, c, d against e, degree 5, in any exact type T: the lifted 4x4 determinant by its 2x2 minors.
template <class T>
int insphere_t(f64 const* a, f64 const* b, f64 const* c, f64 const* d, f64 const* e)
{
    T x[4], y[4], z[4];
    f64 const* const ps[4] = {a, b, c, d};
    for (auto i = 0; i < 4; ++i)
    {
        x[i] = T(ps[i][0]) - T(e[0]);
        y[i] = T(ps[i][1]) - T(e[1]);
        z[i] = T(ps[i][2]) - T(e[2]);
    }
    T l[4];
    for (auto i = 0; i < 4; ++i)
        l[i] = x[i] * x[i] + y[i] * y[i] + z[i] * z[i];
    auto const ab = x[0] * y[1] - x[1] * y[0];
    auto const bc = x[1] * y[2] - x[2] * y[1];
    auto const cd = x[2] * y[3] - x[3] * y[2];
    auto const da = x[3] * y[0] - x[0] * y[3];
    auto const ac = x[0] * y[2] - x[2] * y[0];
    auto const bd = x[1] * y[3] - x[3] * y[1];
    auto const abc = z[2] * ab + z[0] * bc - z[1] * ac;
    auto const bcd = z[3] * bc + z[1] * cd - z[2] * bd;
    auto const cda = z[0] * cd + z[2] * da + z[3] * ac;
    auto const dab = z[1] * da + z[3] * ab + z[0] * bd;
    return (l[3] * abc - l[2] * dab + l[1] * cda - l[0] * bcd).sign();
}

// float_expansion's kernels over stack arrays, as a generated expansion predicate runs them: no value type, no allocation.
namespace raw
{
struct expansion
{
    f64 c[64];
    i32 n = 0;
};

CC_FORCE_INLINE expansion diff(f64 a, f64 b)
{
    expansion r;
    r.n = tg::impl::float_expansion_two_sum(a, -b, r.c);
    return r;
}

CC_FORCE_INLINE expansion mul(expansion const& a, expansion const& b)
{
    expansion r;
    if (a.n == 0 || b.n == 0)
        return r;
    r.n = tg::impl::float_expansion_product(a.c, a.n, b.c, b.n, r.c);
    return r;
}

CC_FORCE_INLINE expansion add(expansion const& a, expansion const& b, bool subtract)
{
    expansion r;
    r.n = tg::impl::float_expansion_sum(a.c, a.n, b.c, b.n, subtract, r.c);
    return r;
}

CC_FORCE_INLINE int sign(expansion const& a)
{
    return a.n == 0 ? 0 : a.c[a.n - 1] > 0.0 ? 1 : -1;
}

int orient2d(f64 const* p, f64 const* q, f64 const* r)
{
    auto const ax = diff(p[0], r[0]);
    auto const ay = diff(p[1], r[1]);
    auto const bx = diff(q[0], r[0]);
    auto const by = diff(q[1], r[1]);
    return sign(add(mul(ax, by), mul(ay, bx), true));
}

int orient3d(f64 const* a, f64 const* b, f64 const* c, f64 const* d)
{
    expansion u[3], v[3], w[3];
    for (auto i = 0; i < 3; ++i)
    {
        u[i] = diff(a[i], d[i]);
        v[i] = diff(b[i], d[i]);
        w[i] = diff(c[i], d[i]);
    }
    auto const m0 = add(mul(v[1], w[2]), mul(v[2], w[1]), true);
    auto const m1 = add(mul(v[2], w[0]), mul(v[0], w[2]), true);
    auto const m2 = add(mul(v[0], w[1]), mul(v[1], w[0]), true);
    return sign(add(add(mul(u[0], m0), mul(u[1], m1), false), mul(u[2], m2), false));
}
} // namespace raw

// A prototype to measure against bigfloat_sbo: values live in a bump arena per predicate call.
// A value is a handle into the arena, trivially destructible; an allocation is one add, and the arena is dropped as a whole.
// A kernel writes its window at the arena top and the top advances by the trimmed size only, so no result is copied.
namespace arena
{
struct scratch
{
    u64* top;
    u64* end;

    /// Room for n limbs at top; a predicate's scratch is sized for its inputs, so running out is the caller's bug.
    CC_FORCE_INLINE u64* room(i32 n)
    {
        CC_ASSERT(end - top >= n, "the predicate's scratch is too small");
        return top;
    }
};

struct value
{
    u64 const* limbs = nullptr;
    i32 size = 0;
    i32 exponent = 0;
    bool negative = false;

    [[nodiscard]] tg::impl::bigfloat_view view() const { return {.limbs = limbs, .size = size, .exponent = exponent}; }
    [[nodiscard]] int sign() const { return size == 0 ? 0 : (negative ? -1 : 1); }
};
static_assert(std::is_trivially_destructible_v<value>);

/// Commits r, computed at s.top.
CC_FORCE_INLINE value commit(scratch& s, tg::impl::bigfloat_raw r, bool negative)
{
    auto const* const limbs = s.top;
    s.top += r.size;
    return {.limbs = limbs, .size = r.size, .exponent = r.size == 0 ? 0 : r.exponent, .negative = r.size != 0 && negative};
}

CC_FORCE_INLINE value of(scratch& s, f64 v)
{
    auto const bits = cc::bit_cast<u64>(v);
    auto const biased = int((bits >> 52) & 0x7ff);
    auto m = bits & ((u64(1) << 52) - 1);
    auto e = -1074;
    if (biased != 0)
    {
        m |= u64(1) << 52;
        e = biased - 1075;
    }
    if (m == 0)
        return {};
    auto const tz = cc::count_trailing_zeroes(m);
    auto* const d = s.room(1);
    d[0] = m >> tz;
    return commit(s, {.size = 1, .exponent = e + tz}, (bits >> 63) != 0);
}

CC_DONT_INLINE value sum_general(scratch& s, value a, value b, bool magnitude_sub)
{
    auto* const d = s.room(tg::impl::bigfloat_sum_limbs(a.view(), b.view()));
    auto const r = tg::impl::bigfloat_add_kernel(d, a.view(), b.view(), magnitude_sub);
    return commit(s, r, a.negative != r.negative);
}

CC_FORCE_INLINE value sum(scratch& s, value a, value b, bool subtract)
{
    if (b.size == 0)
        return a;
    if (a.size == 0)
        return {.limbs = b.limbs, .size = b.size, .exponent = b.exponent, .negative = b.negative != subtract};
    auto const magnitude_sub = a.negative != (b.negative != subtract);
    if (tg::impl::bigfloat_is_small_sum(a.view(), b.view())) [[likely]]
    {
        auto* const d = s.room(3);
        auto const r = (a.size | b.size) == 1 ? tg::impl::bigfloat_add_small<1>(d, a.view(), b.view(), magnitude_sub)
                                              : tg::impl::bigfloat_add_small<2>(d, a.view(), b.view(), magnitude_sub);
        return commit(s, r, a.negative != r.negative);
    }
    return sum_general(s, a, b, magnitude_sub);
}

CC_DONT_INLINE value mul_general(scratch& s, value a, value b)
{
    auto* const d = s.room(a.size + b.size);
    return commit(s, tg::impl::bigfloat_mul_kernel(d, a.view(), b.view()), a.negative != b.negative);
}

CC_FORCE_INLINE value mul(scratch& s, value a, value b)
{
    if (a.size == 0 || b.size == 0)
        return {};
    if (a.size <= 2 && b.size <= 2) [[likely]]
        return commit(s, tg::impl::bigfloat_mul_small(s.room(4), a.view(), b.view()), a.negative != b.negative);
    return mul_general(s, a, b);
}

/// A predicate's scratch: a stack buffer, sized here for the benchmark's predicates at any input.
struct frame
{
    u64 buffer[4096];
    scratch s = {.top = buffer, .end = buffer + 4096};
};

int orient2d(f64 const* p, f64 const* q, f64 const* r)
{
    frame f;
    auto& s = f.s;
    auto const rx = of(s, r[0]);
    auto const ry = of(s, r[1]);
    auto const ax = sum(s, of(s, p[0]), rx, true);
    auto const ay = sum(s, of(s, p[1]), ry, true);
    auto const bx = sum(s, of(s, q[0]), rx, true);
    auto const by = sum(s, of(s, q[1]), ry, true);
    return sum(s, mul(s, ax, by), mul(s, ay, bx), true).sign();
}

int orient3d(f64 const* a, f64 const* b, f64 const* c, f64 const* d)
{
    frame f;
    auto& s = f.s;
    value u[3], v[3], w[3];
    for (auto i = 0; i < 3; ++i)
    {
        auto const di = of(s, d[i]);
        u[i] = sum(s, of(s, a[i]), di, true);
        v[i] = sum(s, of(s, b[i]), di, true);
        w[i] = sum(s, of(s, c[i]), di, true);
    }
    auto const m0 = sum(s, mul(s, v[1], w[2]), mul(s, v[2], w[1]), true);
    auto const m1 = sum(s, mul(s, v[2], w[0]), mul(s, v[0], w[2]), true);
    auto const m2 = sum(s, mul(s, v[0], w[1]), mul(s, v[1], w[0]), true);
    return sum(s, sum(s, mul(s, u[0], m0), mul(s, u[1], m1), false), mul(s, u[2], m2), false).sign();
}
} // namespace arena

int orient2d_f64(f64 const* p, f64 const* q, f64 const* r)
{
    auto const det = (p[0] - r[0]) * (q[1] - r[1]) - (p[1] - r[1]) * (q[0] - r[0]);
    return det > 0 ? 1 : det < 0 ? -1 : 0;
}

int orient3d_f64(f64 const* a, f64 const* b, f64 const* c, f64 const* d)
{
    f64 u[3], v[3], w[3];
    for (auto i = 0; i < 3; ++i)
    {
        u[i] = a[i] - d[i];
        v[i] = b[i] - d[i];
        w[i] = c[i] - d[i];
    }
    auto const det
        = u[0] * (v[1] * w[2] - v[2] * w[1]) + u[1] * (v[2] * w[0] - v[0] * w[2]) + u[2] * (v[0] * w[1] - v[1] * w[0]);
    return det > 0 ? 1 : det < 0 ? -1 : 0;
}

template <class F>
void run_orient2d(char const* name, cc::vector<f64> const& pts, F&& f)
{
    nx::bench::run(name,
                   [&](nx::bench::iteration& it)
                   {
                       auto acc = 0;
                       auto const* const p = nx::bench::keep(pts.data());
                       for (auto i = 0; i + 2 < input_count; ++i)
                           acc += f(p + 3 * i, p + 3 * i + 3, p + 3 * i + 6);
                       nx::bench::sink(acc);
                       it.items(input_count - 2);
                   });
}

template <class F>
void run_orient3d(char const* name, cc::vector<f64> const& pts, F&& f)
{
    nx::bench::run(name,
                   [&](nx::bench::iteration& it)
                   {
                       auto acc = 0;
                       auto const* const p = nx::bench::keep(pts.data());
                       for (auto i = 0; i + 3 < input_count; ++i)
                           acc += f(p + 3 * i, p + 3 * i + 3, p + 3 * i + 6, p + 3 * i + 9);
                       nx::bench::sink(acc);
                       it.items(input_count - 3);
                   });
}

void sweep_orient2d(bool spread)
{
    auto const pts = make_points(spread);
    run_orient2d("f64, inexact", pts, orient2d_f64);
    run_orient2d("K=1 ops", pts, orient2d_ops<1>);
    run_orient2d("K=2 ops", pts, orient2d_ops<2>);
    run_orient2d("K=3 ops", pts, orient2d_ops<3>);
    run_orient2d("K=1 msum", pts, orient2d_msum<1>);
    run_orient2d("K=2 msum", pts, orient2d_msum<2>);
    run_orient2d("K=3 msum", pts, orient2d_msum<3>);
}

void sweep_orient3d(bool spread)
{
    auto const pts = make_points(spread);
    run_orient3d("f64, inexact", pts, orient3d_f64);
    run_orient3d("K=1 ops", pts, orient3d_ops<1>);
    run_orient3d("K=2 ops", pts, orient3d_ops<2>);
    run_orient3d("K=3 ops", pts, orient3d_ops<3>);
    run_orient3d("K=1 msum", pts, orient3d_msum<1>);
    run_orient3d("K=2 msum", pts, orient3d_msum<2>);
    run_orient3d("K=3 msum", pts, orient3d_msum<3>);
}
} // namespace

namespace
{
/// Instructions, core cycles, branches and mispredictions per call, which a loaded machine perturbs far less than times.
template <class F>
void count_per_call(char const* name, isize calls, F&& sweep)
{
    sweep(); // warm the caches, the predictor and the node allocator
    auto const m = nx::bench::measure_hw_counters(sweep,
                                                  {.counters = cc::vector<nx::bench::hw_counter>{
                                                       nx::bench::hw_counter::instructions_retired,
                                                       nx::bench::hw_counter::core_cycles,
                                                       nx::bench::hw_counter::branch_instructions,
                                                       nx::bench::hw_counter::branch_misses,
                                                   },
                                                   .measure_all = true});
    auto const per = [&](nx::bench::hw_counter c)
    {
        auto const v = m.value_of(c);
        return v.has_value() ? f64(v.value()) / f64(calls) : -1.0;
    };
    cc::println("  {:<16} {:>8.1f} ins  {:>8.1f} cyc  {:>6.1f} br  {:>5.2f} miss", name,
                per(nx::bench::hw_counter::instructions_retired), per(nx::bench::hw_counter::core_cycles),
                per(nx::bench::hw_counter::branch_instructions), per(nx::bench::hw_counter::branch_misses));
}

constexpr int counter_repeats = 200;

template <class F>
void count_orient2d(char const* name, cc::vector<f64> const& pts, F f)
{
    count_per_call(name, counter_repeats * (input_count - 2),
                   [&]
                   {
                       auto acc = 0;
                       auto const* const p = nx::bench::keep(pts.data());
                       for (auto r = 0; r < counter_repeats; ++r)
                           for (auto i = 0; i + 2 < input_count; ++i)
                               acc += f(p + 3 * i, p + 3 * i + 3, p + 3 * i + 6);
                       nx::bench::sink(acc);
                   });
}

template <class F>
void count_orient3d(char const* name, cc::vector<f64> const& pts, F f)
{
    count_per_call(name, counter_repeats * (input_count - 3),
                   [&]
                   {
                       auto acc = 0;
                       auto const* const p = nx::bench::keep(pts.data());
                       for (auto r = 0; r < counter_repeats; ++r)
                           for (auto i = 0; i + 3 < input_count; ++i)
                               acc += f(p + 3 * i, p + 3 * i + 3, p + 3 * i + 6, p + 3 * i + 9);
                       nx::bench::sink(acc);
                   });
}

template <class F>
void count_insphere(char const* name, cc::vector<f64> const& pts, F f)
{
    count_per_call(name, counter_repeats / 10 * (input_count - 4),
                   [&]
                   {
                       auto acc = 0;
                       auto const* const p = nx::bench::keep(pts.data());
                       for (auto r = 0; r < counter_repeats / 10; ++r)
                           for (auto i = 0; i + 4 < input_count; ++i)
                               acc += f(p + 3 * i, p + 3 * i + 3, p + 3 * i + 6, p + 3 * i + 9, p + 3 * i + 12);
                       nx::bench::sink(acc);
                   });
}
} // namespace

BENCHMARK("tg bigfloat - counters per call")
{
    for (auto const spread : {false, true})
    {
        auto const pts = make_points(spread);
        auto const* const p = pts.data();
        for (auto i = 0; i + 3 < input_count; ++i)
        {
            CHECK(arena::orient2d(p + 3 * i, p + 3 * i + 3, p + 3 * i + 6)
                  == orient2d_ops<2>(p + 3 * i, p + 3 * i + 3, p + 3 * i + 6));
            CHECK(arena::orient3d(p + 3 * i, p + 3 * i + 3, p + 3 * i + 6, p + 3 * i + 9)
                  == orient3d_ops<2>(p + 3 * i, p + 3 * i + 3, p + 3 * i + 6, p + 3 * i + 9));
            CHECK(orient2d_fe<4>(p + 3 * i, p + 3 * i + 3, p + 3 * i + 6)
                  == orient2d_ops<2>(p + 3 * i, p + 3 * i + 3, p + 3 * i + 6));
            CHECK(raw::orient2d(p + 3 * i, p + 3 * i + 3, p + 3 * i + 6)
                  == orient2d_ops<2>(p + 3 * i, p + 3 * i + 3, p + 3 * i + 6));
            CHECK(orient3d_fe<4>(p + 3 * i, p + 3 * i + 3, p + 3 * i + 6, p + 3 * i + 9)
                  == orient3d_ops<2>(p + 3 * i, p + 3 * i + 3, p + 3 * i + 6, p + 3 * i + 9));
            CHECK(raw::orient3d(p + 3 * i, p + 3 * i + 3, p + 3 * i + 6, p + 3 * i + 9)
                  == orient3d_ops<2>(p + 3 * i, p + 3 * i + 3, p + 3 * i + 6, p + 3 * i + 9));
        }
        for (auto i = 0; i + 4 < input_count; ++i)
            CHECK(insphere_t<tg::float_expansion>(p + 3 * i, p + 3 * i + 3, p + 3 * i + 6, p + 3 * i + 9, p + 3 * i + 12)
                  == insphere_t<tg::bigfloat>(p + 3 * i, p + 3 * i + 3, p + 3 * i + 6, p + 3 * i + 9, p + 3 * i + 12));
        cc::println("orient2d, {}", spread ? "spread exponents" : "similar magnitudes");
        count_orient2d("f64, inexact", pts, orient2d_f64);
        count_orient2d("K=1 ops", pts, orient2d_ops<1>);
        count_orient2d("K=2 ops", pts, orient2d_ops<2>);
        count_orient2d("K=3 ops", pts, orient2d_ops<3>);
        count_orient2d("K=1 msum", pts, orient2d_msum<1>);
        count_orient2d("K=2 msum", pts, orient2d_msum<2>);
        count_orient2d("K=3 msum", pts, orient2d_msum<3>);
        count_orient2d("arena", pts, arena::orient2d);
        count_orient2d("expansion K=2", pts, orient2d_fe<2>);
        count_orient2d("expansion K=4", pts, orient2d_fe<4>);
        count_orient2d("expansion K=8", pts, orient2d_fe<8>);
        count_orient2d("expansion raw", pts, raw::orient2d);
        cc::println("orient3d, {}", spread ? "spread exponents" : "similar magnitudes");
        count_orient3d("f64, inexact", pts, orient3d_f64);
        count_orient3d("K=1 ops", pts, orient3d_ops<1>);
        count_orient3d("K=2 ops", pts, orient3d_ops<2>);
        count_orient3d("K=3 ops", pts, orient3d_ops<3>);
        count_orient3d("K=1 msum", pts, orient3d_msum<1>);
        count_orient3d("K=2 msum", pts, orient3d_msum<2>);
        count_orient3d("K=3 msum", pts, orient3d_msum<3>);
        count_orient3d("arena", pts, arena::orient3d);
        count_orient3d("expansion K=2", pts, orient3d_fe<2>);
        count_orient3d("expansion K=4", pts, orient3d_fe<4>);
        count_orient3d("expansion K=8", pts, orient3d_fe<8>);
        count_orient3d("expansion raw", pts, raw::orient3d);
        cc::println("insphere, {}", spread ? "spread exponents" : "similar magnitudes");
        count_insphere("K=2 ops", pts, insphere_t<tg::bigfloat_sbo<2>>);
        count_insphere("K=3 ops", pts, insphere_t<tg::bigfloat_sbo<3>>);
        count_insphere("expansion K=4", pts, insphere_t<tg::float_expansion_sbo<4>>);
        count_insphere("expansion K=8", pts, insphere_t<tg::float_expansion_sbo<8>>);
    }
}

BENCHMARK("tg bigfloat - orient2d, similar magnitudes")
{
    sweep_orient2d(false);
}

BENCHMARK("tg bigfloat - orient2d, spread exponents")
{
    sweep_orient2d(true);
}

BENCHMARK("tg bigfloat - orient3d, similar magnitudes")
{
    sweep_orient3d(false);
}

BENCHMARK("tg bigfloat - orient3d, spread exponents")
{
    sweep_orient3d(true);
}
