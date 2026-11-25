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
    /// 1 for an elementwise `+`, -1 for an elementwise `-` of two operands, 0 for anything else.
    i32 sum_sign = 0;
    /// A sum of products a GPU may round step by step and in any order: a `dot`, a matrix product.
    bool is_dot = false;
};

[[nodiscard]] call_shape shape_of(builtins::function_record const& record)
{
    auto const signature = cc::string_view(record.signature);
    auto shape = call_shape{.is_conversion = signature.contains("@operator(\"as\")") || record.takes_literal_at_default};
    // a matrix product sums products, which a contraction may keep unrounded beyond one ulp of each lane
    auto const is_times = signature.contains("@operator(\"*\")");
    auto has_matrix = false;
    for (auto const& p : record.parameters)
        has_matrix = has_matrix || cc::string_view(p).starts_with("mat");
    shape.is_product = is_times && !has_matrix;
    shape.is_dot = record.name == "dot" || (is_times && has_matrix);
    if (record.parameters.size() == 2)
        shape.sum_sign = signature.contains("@operator(\"+\")") ? 1 : signature.contains("@operator(\"-\")") ? -1 : 0;
    return shape;
}

[[nodiscard]] bool is_scalar_type(cc::string_view name)
{
    return name == "float" || name == "half";
}

/// The two operand leaves result `j` of an elementwise operation of two operands takes, of `lanes` lanes; false where
/// `count` leaves are no such pair.
[[nodiscard]] bool operands_of(builtins::function_record const& record, isize count, isize j, isize lanes, isize& a, isize& b)
{
    if (record.parameters.size() != 2)
        return false;
    a = is_scalar_type(record.parameters[0]) ? 0 : j;
    b = (is_scalar_type(record.parameters[0]) ? 1 : lanes) + (is_scalar_type(record.parameters[1]) ? 0 : j);
    return b < count;
}

/// Result `j` of the elementwise product `record` over `in`, exact in a double, as a contraction keeps it; NaN for a
/// product of another shape.
[[nodiscard]] f64 exact_product(builtins::function_record const& record, cc::span<scalar const> in, isize j, isize lanes)
{
    auto a = isize(0);
    auto b = isize(0);
    if (!operands_of(record, in.size(), j, lanes, a, b))
        return cc::bit_cast<f64>(0x7ff8'0000'0000'0000ull);
    return f64(in[a].as_float()) * f64(in[b].as_float());
}

/// A sum a reassociating target may add in any order, as `scalar::terms` and its neighbors keep it, in doubles.
struct sum_chain
{
    i32 terms = 0;
    bool has_products = false;
    f64 magnitude = 0;
    f64 least = 0;
    f64 most = 0;
};

/// `leaf` as a chain's term, or the chain it already is; negated for the right side of a `-`.
[[nodiscard]] sum_chain term_of(scalar leaf, bool is_negated)
{
    auto c = sum_chain{.terms = leaf.terms,
                       .has_products = leaf.has_products,
                       .magnitude = leaf.magnitude,
                       .least = leaf.sum_least,
                       .most = leaf.sum_most};
    if (leaf.terms == 0)
    {
        auto const v = value_of(leaf);
        c.terms = 1;
        c.least = leaf.is_ranged ? leaf.least : v;
        c.most = leaf.is_ranged ? leaf.most : v;
        c.magnitude = cc::max(c.least < 0 ? -c.least : c.least, c.most < 0 ? -c.most : c.most);
    }
    if (is_negated)
        c = {.terms = c.terms, .has_products = c.has_products, .magnitude = c.magnitude, .least = -c.most, .most = -c.least};
    return c;
}

/// Higham's γ(k) = k·u / (1 - k·u): any order of summing k + 1 terms, or a sum of k products, rounds to within
/// γ(k) times the sum of the terms' magnitudes of the exact sum.
[[nodiscard]] f64 gamma_of(i32 k, value_kind kind)
{
    auto const u = kind == value_kind::scalar_half ? 1.0 / 2048.0 : 1.0 / 16777216.0;
    auto const ku = f64(k) * u;
    return ku < 1 ? ku / (1 - ku) : cc::bit_cast<f64>(0x7ff0'0000'0000'0000ull);
}

/// The float at or below `x`, and at or above it.
[[nodiscard]] f32 float_below(f64 x)
{
    auto const nearest = f32(x);
    return f64(nearest) > x ? next_down(nearest) : nearest;
}

[[nodiscard]] f32 float_above(f64 x)
{
    auto const nearest = f32(x);
    return f64(nearest) < x ? next_up(nearest) : nearest;
}

/// Adds to `h` what a GPU may give for a result the evaluator gave as `wide`, a 32-bit leaf, narrowed to `kind`.
/// `product` is the result exact, where the call is a product a contraction may keep unrounded, and NaN elsewhere.
/// `x` rounded to a half toward zero, back as a float: the nearest half, or the one below it in magnitude where the
/// nearest is larger, which is the largest finite half where the nearest overflows.
[[nodiscard]] f32 half_toward_zero(f32 x)
{
    auto const bits = builtins::impl::half_bits_of(x);
    auto const nearest = builtins::impl::float_of_half_bits(bits);
    auto const magnitude = x < 0 ? -x : x;
    auto const nearest_magnitude = nearest < 0 ? -nearest : nearest;
    if (!(nearest_magnitude > magnitude) || (bits & 0x7fffu) == 0)
        return nearest;
    return builtins::impl::float_of_half_bits((bits & 0x8000u) | ((bits & 0x7fffu) - 1));
}

void add_result(hull& h, f32 wide, f64 product, value_kind kind, call_shape shape, impl::float_licenses licenses)
{
    auto const rounded = kind == value_kind::scalar_half
                           ? builtins::impl::float_of_half_bits(builtins::impl::half_bits_of(wide))
                           : wide;
    h.add(rounded);
    if (kind == value_kind::scalar_half && licenses.keeps_half_at_float && !shape.is_conversion)
        h.add(wide);
    // EVAL-96: a conversion to a half rounds to nearest or toward zero, as its target does, at every level
    if (kind == value_kind::scalar_half && shape.is_conversion && is_finite(wide))
        h.add(half_toward_zero(wide));
    if (kind == value_kind::scalar_float && licenses.contracts && product == product && is_finite(wide))
    {
        // the exact product, rounded outward to the floats on either side of it
        h.add(float_below(product));
        h.add(float_above(product));
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
            .flushes_denormals = level != float_level::ieee,
            .reassociates = level == float_level::fast || level == float_level::specials};
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
    if (ends.empty() && !licenses.is_any() && !shape.is_dot && !shape.is_conversion)
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

    // a sum of products' terms and their magnitudes per result: the evaluator over ones, and over the arguments'
    // largest magnitudes, so no builtin's layout needs writing down
    auto counts = cc::vector<scalar>();
    auto magnitudes = cc::vector<scalar>();
    if (shape.is_dot)
    {
        auto ones = exact;
        auto largest = exact;
        for (auto i = isize(0); i < args.size(); ++i)
        {
            if (!is_float_like(args[i].kind))
                continue;
            ones[i] = scalar::of(1.0f);
            auto const least = args[i].is_ranged ? args[i].least : value_of(args[i]);
            auto const most = args[i].is_ranged ? args[i].most : value_of(args[i]);
            largest[i] = scalar::of(cc::max(least < 0 ? -least : least, most < 0 ? -most : most));
        }
        record.evaluate(ones, counts);
        record.evaluate(largest, magnitudes);
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

        // the chain this result sums, and what adding its terms in another order, or rounding each step, may cost
        auto chain = sum_chain();
        auto a = isize(0);
        auto b = isize(0);
        auto rounds = 0;
        if (shape.sum_sign != 0 && licenses.reassociates && operands_of(record, args.size(), j, results.size(), a, b))
        {
            auto const left = term_of(args[a], false);
            auto const right = term_of(args[b], shape.sum_sign < 0);
            chain = {.terms = left.terms + right.terms,
                     .has_products = left.has_products || right.has_products,
                     .magnitude = left.magnitude + right.magnitude,
                     .least = left.least + right.least,
                     .most = left.most + right.most};
            rounds = chain.has_products ? chain.terms : chain.terms - 1;
            // two terms of no products add in one order alone
            rounds = rounds >= 2 ? rounds : 0;
        }
        else if (shape.is_dot && j < counts.size() && j < magnitudes.size())
        {
            auto const n = i32(counts[j].as_float());
            auto const magnitude = f64(magnitudes[j].as_float());
            auto const g = gamma_of(n, r.kind) * magnitude;
            // the exact sum of products is within γ(n) of what the evaluator's order gave
            chain = {.terms = n,
                     .has_products = true,
                     .magnitude = magnitude,
                     .least = f64(h.least) - g,
                     .most = f64(h.most) + g};
            rounds = n;
        }
        if (chain.terms > 0 && rounds > 0)
        {
            auto const slack = gamma_of(rounds, r.kind) * chain.magnitude
                             + (licenses.flushes_denormals ? f64(rounds) * f64(least_normal_of(r.kind)) : 0.0);
            h.add(float_below(chain.least - slack));
            h.add(float_above(chain.most + slack));
        }
        if (chain.terms > 0)
        {
            r.terms = u16(chain.terms < 65535 ? chain.terms : 65535);
            r.has_products = chain.has_products;
            r.magnitude = float_above(chain.magnitude);
            r.sum_least = float_below(chain.least);
            r.sum_most = float_above(chain.most);
        }
        if (h.is_unbounded)
        {
            r = r.unbounded();
            continue;
        }
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
