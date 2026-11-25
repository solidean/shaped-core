#include "bounds.hh"

#include <clean-core/math/bit.hh>
#include <shaped-graphics-language/builtins/impl/soft_math.hh>

using namespace sgl;
using namespace sgl::check;

namespace
{
[[nodiscard]] f32 infinity()
{
    return cc::bit_cast<f32>(0x7f80'0000u);
}

[[nodiscard]] bool is_finite(f32 x)
{
    return (cc::bit_cast<u32>(x) & 0x7f80'0000u) != 0x7f80'0000u;
}

[[nodiscard]] bool is_float_like(value_kind k)
{
    return k == value_kind::scalar_float || k == value_kind::scalar_half;
}

/// The least magnitude a normal value of `k` has; below it, a flushing target may give zero.
[[nodiscard]] f32 least_normal_of(value_kind k)
{
    return k == value_kind::scalar_half ? 6.103515625e-05f : 1.17549435e-38f;
}

/// A float or a half leaf's exact value, as a float.
[[nodiscard]] f32 value_of(scalar s)
{
    return s.kind == value_kind::scalar_half ? builtins::impl::float_of_half_bits(s.bits) : s.as_float();
}

/// The next float above finite `x`; above either zero, the least denormal.
[[nodiscard]] f32 next_up(f32 x)
{
    auto const bits = cc::bit_cast<u32>(x);
    if ((bits & 0x7fff'ffffu) == 0)
        return cc::bit_cast<f32>(1u);
    return cc::bit_cast<f32>(x > 0 ? bits + 1 : bits - 1);
}

[[nodiscard]] f32 next_down(f32 x)
{
    return -next_up(-x);
}

/// The least and the most of what was added, or unbounded once something that is no finite float was.
struct hull
{
    f32 least = 0;
    f32 most = 0;
    bool is_empty = true;
    bool is_unbounded = false;

    void add(f32 x)
    {
        if (!is_finite(x))
        {
            is_unbounded = true;
            return;
        }
        least = is_empty || x < least ? x : least;
        most = is_empty || x > most ? x : most;
        is_empty = false;
    }

    void add(hull const& h)
    {
        is_unbounded = is_unbounded || h.is_unbounded;
        if (!h.is_empty)
        {
            add(h.least);
            add(h.most);
        }
    }
};

/// What one builtin is to the licenses: a conversion, which rounds at every level, or an elementwise float product.
struct call_shape
{
    bool is_conversion = false;
    bool is_product = false;
};

[[nodiscard]] call_shape shape_of(builtins::function_record const& record)
{
    auto const signature = cc::string_view(record.signature);
    auto shape = call_shape{.is_conversion = signature.contains("@operator(\"as\")") || record.takes_literal_at_default};
    // a matrix product sums products, which a contraction may keep unrounded beyond one ulp of each lane
    shape.is_product = signature.contains("@operator(\"*\")");
    for (auto const& p : record.parameters)
        shape.is_product = shape.is_product && !cc::string_view(p).starts_with("mat");
    return shape;
}

[[nodiscard]] bool is_scalar_type(cc::string_view name)
{
    return name == "float" || name == "half";
}

/// Result `j` of the elementwise product `record` over `in`, exact in a double, as a contraction keeps it; NaN for a
/// product of another shape.
[[nodiscard]] f64 exact_product(builtins::function_record const& record, cc::span<scalar const> in, isize j, isize lanes)
{
    if (record.parameters.size() != 2)
        return cc::bit_cast<f64>(0x7ff8'0000'0000'0000ull);
    auto const a = is_scalar_type(record.parameters[0]) ? 0 : j;
    auto const b = (is_scalar_type(record.parameters[0]) ? 1 : lanes) + (is_scalar_type(record.parameters[1]) ? 0 : j);
    if (b >= in.size())
        return cc::bit_cast<f64>(0x7ff8'0000'0000'0000ull);
    return f64(in[a].as_float()) * f64(in[b].as_float());
}

/// Adds to `h` what a GPU may give for a result the evaluator gave as `wide`, a 32-bit leaf, narrowed to `kind`.
/// `product` is the result exact, where the call is a product a contraction may keep unrounded, and NaN elsewhere.
void add_result(hull& h, f32 wide, f64 product, value_kind kind, call_shape shape, impl::float_licenses licenses)
{
    auto const rounded = kind == value_kind::scalar_half
                           ? builtins::impl::float_of_half_bits(builtins::impl::half_bits_of(wide))
                           : wide;
    h.add(rounded);
    if (kind == value_kind::scalar_half && licenses.keeps_half_at_float && !shape.is_conversion)
        h.add(wide);
    if (kind == value_kind::scalar_float && licenses.contracts && product == product && is_finite(wide))
    {
        // the exact product, rounded outward to the floats on either side of it
        auto const nearest = f32(product);
        h.add(f64(nearest) > product ? next_down(nearest) : nearest);
        h.add(f64(nearest) < product ? next_up(nearest) : nearest);
    }
    if (licenses.flushes_denormals && is_finite(rounded) && rounded != 0
        && (rounded < 0 ? -rounded : rounded) < least_normal_of(kind))
        h.add(0.0f);
}

/// Result `j` of `record` over `args` as a value number: one builtin over the same values, ranged ones by their
/// origins, is one value, as a GPU's compiler merges one expression written twice before it decides a contraction.
[[nodiscard]] u32 origin_of(builtins::function_record const& record, cc::span<scalar const> args, isize j)
{
    auto h = u64(0xcbf2'9ce4'8422'2325ull);
    auto const mix = [&](u64 v)
    {
        h ^= v;
        h *= 0x0000'0100'0000'01b3ull;
    };
    mix(reinterpret_cast<u64>(&record));
    mix(u64(j));
    for (auto const& a : args)
        mix(a.is_ranged && a.origin != 0 ? (u64(a.origin) << 32) | 1u : a.wide_bits() << 1);
    auto const folded = u32(h ^ (h >> 32));
    return folded == 0 ? 1u : folded;
}

/// One value a bounding run varies, the leaves among the call's that hold it, and the values it takes there.
/// Leaves of one origin are one value, so they vary together.
struct varied
{
    cc::vector<isize> leaves;
    u32 origin = 0;
    f32 values[3] = {};
    i32 count = 0;
};
} // namespace

impl::float_licenses impl::licenses_of(float_level level)
{
    return {.keeps_half_at_float = level == float_level::fast,
            .contracts = level == float_level::fast || level == float_level::specials,
            .flushes_denormals = level != float_level::ieee};
}

bool sgl::check::scalar::is_unbounded() const
{
    return is_ranged && least == -infinity() && most == infinity();
}

scalar sgl::check::scalar::unbounded() const
{
    auto s = *this;
    s.is_ranged = true;
    s.least = -infinity();
    s.most = infinity();
    return s;
}

void impl::bound_call(builtins::function_record const& record,
                      cc::span<scalar const> args,
                      float_licenses licenses,
                      cc::span<scalar> results)
{
    // an evaluator that hands an argument on hands on its range too, which is this call's to state
    for (auto& r : results)
        r = {.kind = r.kind, .bits = r.bits, .high = r.high};
    if (record.evaluate == nullptr)
        return;
    auto has_float = false;
    for (auto const& a : args)
    {
        if (a.is_unbounded())
        {
            for (auto& r : results)
                r = r.unbounded();
            return;
        }
        has_float = has_float || is_float_like(a.kind) || a.is_ranged;
    }
    if (!has_float)
        return;

    auto const shape = shape_of(record);
    auto exact = cc::vector<scalar>();
    auto ends = cc::vector<varied>();
    for (auto i = isize(0); i < args.size(); ++i)
    {
        exact.push_back(args[i].widened());
        if (!is_float_like(args[i].kind))
            continue;
        auto const& a = args[i];
        auto is_known = false;
        for (auto& end : ends)
            if (a.is_ranged && a.origin != 0 && end.origin == a.origin)
            {
                end.leaves.push_back(i);
                is_known = true;
            }
        if (is_known)
            continue;
        auto const v = value_of(a);
        auto const least = a.is_ranged ? a.least : v;
        auto const most = a.is_ranged ? a.most : v;
        auto end = varied{.origin = a.is_ranged ? a.origin : 0};
        end.leaves.push_back(i);
        end.values[end.count++] = least;
        if (most != least)
            end.values[end.count++] = most;
        auto const tiny = least_normal_of(a.kind);
        if (licenses.flushes_denormals && v != 0
            && ((least < 0 ? -least : least) < tiny || (most < 0 ? -most : most) < tiny))
            end.values[end.count++] = 0.0f;
        if (end.count > 1)
            ends.push_back(cc::move(end));
    }
    if (ends.empty() && !licenses.is_any())
        return;

    auto hulls = cc::vector<hull>::create_filled(results.size(), hull{});
    auto is_undecided = cc::vector<bool>::create_filled(results.size(), false);
    auto in = exact;
    auto out = cc::vector<scalar>();
    auto const set = [&](varied const& end, f32 value)
    {
        for (auto const leaf : end.leaves)
            in[leaf] = scalar::of(value);
    };
    // One evaluation at `in`, as a hull of what each result may be there.
    auto const evaluate = [&](cc::vector<hull>& into)
    {
        out.clear();
        record.evaluate(in, out);
        for (auto j = isize(0); j < results.size(); ++j)
        {
            if (j >= out.size())
            {
                is_undecided[j] = true;
                continue;
            }
            if (is_float_like(results[j].kind))
            {
                auto const product = shape.is_product ? exact_product(record, in, j, results.size())
                                                      : cc::bit_cast<f64>(0x7ff8'0000'0000'0000ull);
                add_result(into[j], out[j].as_float(), product, results[j].kind, shape, licenses);
            }
            else if (out[j].narrowed_to(results[j].kind) != results[j])
                is_undecided[j] = true;
        }
    };

    auto combinations = isize(1);
    for (auto const& end : ends)
        combinations = combinations > 64 ? combinations : combinations * end.count;
    if (combinations <= 64)
    {
        // every combination of the varied values, the exact arguments among them only where none varies
        for (auto c = isize(0); c < combinations; ++c)
        {
            auto rest = c;
            for (auto const& end : ends)
            {
                set(end, end.values[rest % end.count]);
                rest /= end.count;
            }
            evaluate(hulls);
        }
        if (ends.empty())
            evaluate(hulls);
    }
    else
    {
        // one value at a time, each moving the hull of the exact arguments' results by what it moves them
        evaluate(hulls);
        auto const center = hulls;
        for (auto const& end : ends)
        {
            auto moved = cc::vector<hull>::create_filled(results.size(), hull{});
            for (auto k = 0; k < end.count; ++k)
            {
                set(end, end.values[k]);
                evaluate(moved);
            }
            for (auto const leaf : end.leaves)
                in[leaf] = exact[leaf];
            for (auto j = isize(0); j < results.size(); ++j)
            {
                hulls[j].is_unbounded = hulls[j].is_unbounded || moved[j].is_unbounded;
                if (moved[j].is_empty || center[j].is_empty)
                    continue;
                hulls[j].least += moved[j].least < center[j].least ? moved[j].least - center[j].least : 0;
                hulls[j].most += moved[j].most > center[j].most ? moved[j].most - center[j].most : 0;
            }
        }
    }

    for (auto j = isize(0); j < results.size(); ++j)
    {
        auto& r = results[j];
        if (is_undecided[j] || hulls[j].is_unbounded)
        {
            r = r.unbounded();
            continue;
        }
        if (!is_float_like(r.kind))
            continue;
        auto h = hulls[j];
        h.add(value_of(r));
        if (h.least == h.most)
            continue;
        r.is_ranged = true;
        r.least = h.least;
        r.most = h.most;
        r.origin = origin_of(record, args, j);
    }
}

namespace
{
/// A float or a half leaf's least and most, its value at both where it holds no range.
[[nodiscard]] f32 least_of(scalar s)
{
    return s.is_ranged ? s.least : value_of(s);
}

[[nodiscard]] f32 most_of(scalar s)
{
    return s.is_ranged ? s.most : value_of(s);
}

/// What one leaf may be, given what two ways through a run left in it.
[[nodiscard]] scalar joined(scalar a, scalar b)
{
    if (a == b)
        return a;
    if (a.is_unbounded() || b.is_unbounded() || a.kind != b.kind || !is_float_like(a.kind))
        return a.unbounded();
    auto h = hull();
    h.add(least_of(a));
    h.add(most_of(a));
    h.add(least_of(b));
    h.add(most_of(b));
    if (h.is_unbounded)
        return a.unbounded();
    a.is_ranged = true;
    a.least = h.least;
    a.most = h.most;
    a.origin = 0;
    return a;
}

void join_leaves(cc::span<scalar> into, cc::span<scalar const> other)
{
    for (auto i = isize(0); i < into.size(); ++i)
        into[i] = i < other.size() && into.size() == other.size() ? joined(into[i], other[i]) : into[i].unbounded();
}

/// The one of `all` that `binding`, `member` and `element` name, or null.
template <class Contents>
[[nodiscard]] Contents const* find_contents(cc::span<Contents const> all, Contents const& like)
{
    for (auto const& c : all)
        if (c.binding == like.binding && c.member == like.member && c.element == like.element)
            return &c;
    return nullptr;
}

/// Joins each of `other`'s into `into`'s, and each only one of them holds with what `inputs` held.
template <class Contents>
void join_all(cc::vector<Contents>& into,
              cc::vector<Contents> const& other,
              cc::span<Contents const> inputs,
              cc::vector<scalar> Contents::* leaves)
{
    for (auto const& o : other)
        if (find_contents<Contents>(into, o) == nullptr)
            if (auto const* const held = find_contents(inputs, o))
                into.push_back(*held);
    for (auto& c : into)
    {
        auto const* const o = find_contents<Contents>(other, c);
        auto const* const held = o != nullptr ? o : find_contents(inputs, c);
        join_leaves(c.*leaves, held != nullptr ? cc::span<scalar const>(held->*leaves) : cc::span<scalar const>());
    }
}
} // namespace

void impl::join_into(outcome& into, outcome const& other, run_inputs const& inputs)
{
    join_all(into.buffers, other.buffers, cc::span<buffer_contents const>(inputs.buffers), &buffer_contents::leaves);
    join_all(into.images, other.images, cc::span<texture_contents const>(inputs.textures), &texture_contents::texels);
    join_leaves(into.result.leaves, other.result.leaves);
    for (auto i = isize(0); i < into.lane_results.size(); ++i)
        join_leaves(into.lane_results[i].leaves, i < other.lane_results.size()
                                                     ? cc::span<scalar const>(other.lane_results[i].leaves)
                                                     : cc::span<scalar const>());
    into.undecided += other.undecided;
}
