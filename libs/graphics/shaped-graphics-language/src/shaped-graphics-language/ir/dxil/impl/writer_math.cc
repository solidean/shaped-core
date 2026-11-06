#include "writer.hh"

#include <clean-core/math/bit.hh>
#include <clean-core/string/format.hh>

using namespace sgl;
using namespace sgl::ir;
using namespace sgl::ir::dxil::impl;

namespace
{
constexpr f64 k_pi = 3.14159265358979323846;
/// log2(e) and ln(2): DXIL's exponential and logarithm are base 2.
constexpr f64 k_log2_e = 1.4426950408889634;
constexpr f64 k_ln_2 = 0.6931471805599453;

[[nodiscard]] u64 bits_of_float(f64 value, u32 width)
{
    if (width == 64)
        return cc::bit_cast<u64>(value);
    auto const single = cc::bit_cast<u32>(f32(value));
    if (width == 32)
        return single;
    // the few constants a lowering writes are exact in a half
    auto const sign = (single >> 16) & 0x8000u;
    auto const exponent = i32((single >> 23) & 0xffu) - 127 + 15;
    if (exponent <= 0)
        return sign;
    return sign | (u32(exponent) << 10) | ((single >> 13) & 0x3ffu);
}
} // namespace

ll_value writer::dx_unary(dx_op op, ll_value a)
{
    ll_value const args[] = {_m.constant_i32(k_dx_ops[isize(op)].number), a};
    return call(op, _m.type_of(a), args);
}

ll_value writer::dx_binary(dx_op op, ll_value a, ll_value b)
{
    ll_value const args[] = {_m.constant_i32(k_dx_ops[isize(op)].number), a, b};
    return call(op, _m.type_of(a), args);
}

ll_value writer::dx_tertiary(dx_op op, ll_value a, ll_value b, ll_value c)
{
    ll_value const args[] = {_m.constant_i32(k_dx_ops[isize(op)].number), a, b, c};
    return call(op, _m.type_of(a), args);
}

ll_value writer::float_like(ll_value like, f64 value)
{
    auto const type = _m.type_of(like);
    return _m.constant_float(type, bits_of_float(value, _m.bits_of(type)));
}

ll_value writer::dot_of(cc::span<ll_value const> a, cc::span<ll_value const> b, u8 flags)
{
    auto const type = _m.type_of(a[0]);
    auto const lanes = a.size();
    if (lanes >= 2 && lanes <= 4 && _m.is_float(type) && _m.bits_of(type) != 64)
    {
        auto const op = lanes == 2 ? dx_op::dot2 : lanes == 3 ? dx_op::dot3 : dx_op::dot4;
        ll_value args[9];
        args[0] = _m.constant_i32(k_dx_ops[isize(op)].number);
        for (auto l = isize(0); l < lanes; ++l)
        {
            args[1 + l] = a[l];
            args[1 + lanes + l] = b[l];
        }
        return call(op, type, cc::span<ll_value const>(args, 1 + lanes * 2));
    }
    auto sum = _m.binary(binary_op::mul, a[0], b[0], flags);
    for (auto l = isize(1); l < lanes; ++l)
        sum = _m.binary(binary_op::add, sum, _m.binary(binary_op::mul, a[l], b[l], flags), flags);
    return sum;
}

// ---- each one lane of a `math` with no operation of its own

ll_value writer::lane_fsign(cc::span<ll_value const> a, u8 flags)
{
    auto const positive = _m.compare(predicate::f_ogt, a[0], float_like(a[0], 0.0), flags);
    auto const negative = _m.compare(predicate::f_olt, a[0], float_like(a[0], 0.0), flags);
    return _m.select(positive, float_like(a[0], 1.0), _m.select(negative, float_like(a[0], -1.0), float_like(a[0], 0.0)));
}

ll_value writer::lane_ssign(cc::span<ll_value const> a, u8)
{
    auto const type = _m.type_of(a[0]);
    auto const zero = _m.constant_int(type, 0);
    auto const positive = _m.compare(predicate::i_sgt, a[0], zero);
    auto const negative = _m.compare(predicate::i_slt, a[0], zero);
    return _m.select(positive, _m.constant_int(type, 1), _m.select(negative, _m.constant_int(type, ~u64(0)), zero));
}

ll_value writer::lane_sabs(cc::span<ll_value const> a, u8)
{
    auto const negated = _m.binary(binary_op::sub, _m.constant_int(_m.type_of(a[0]), 0), a[0]);
    return dx_binary(dx_op::imax, a[0], negated);
}

ll_value writer::lane_atan2(cc::span<ll_value const> a, u8 flags)
{
    auto const y = a[0];
    auto const x = a[1];
    auto const zero = float_like(x, 0.0);
    auto const tangent = dx_unary(dx_op::atan, _m.binary(binary_op::sdiv, y, x, flags));
    auto const x_negative = _m.compare(predicate::f_olt, x, zero, flags);
    auto const x_zero = _m.compare(predicate::f_oeq, x, zero, flags);
    auto const y_negative = _m.compare(predicate::f_olt, y, zero, flags);
    auto const y_positive = _m.compare(predicate::f_ogt, y, zero, flags);
    auto const y_not_negative = _m.compare(predicate::f_oge, y, zero, flags);
    auto const above = _m.binary(binary_op::add, tangent, float_like(x, k_pi), flags);
    auto const below = _m.binary(binary_op::sub, tangent, float_like(x, k_pi), flags);
    auto result = tangent;
    result = _m.select(_m.binary(binary_op::bit_and, x_negative, y_not_negative), above, result);
    result = _m.select(_m.binary(binary_op::bit_and, x_negative, y_negative), below, result);
    result = _m.select(_m.binary(binary_op::bit_and, x_zero, y_positive), float_like(x, k_pi / 2.0), result);
    return _m.select(_m.binary(binary_op::bit_and, x_zero, y_negative), float_like(x, -k_pi / 2.0), result);
}

ll_value writer::lane_pow(cc::span<ll_value const> a, u8 flags)
{
    return dx_unary(dx_op::exp, _m.binary(binary_op::mul, dx_unary(dx_op::log, a[0]), a[1], flags));
}

ll_value writer::lane_exp(cc::span<ll_value const> a, u8 flags)
{
    return dx_unary(dx_op::exp, _m.binary(binary_op::mul, a[0], float_like(a[0], k_log2_e), flags));
}

ll_value writer::lane_log(cc::span<ll_value const> a, u8 flags)
{
    return _m.binary(binary_op::mul, dx_unary(dx_op::log, a[0]), float_like(a[0], k_ln_2), flags);
}

ll_value writer::lane_fclamp(cc::span<ll_value const> a, u8)
{
    auto low = u64(0);
    auto high = u64(0);
    // `saturate` lowers to this form, and DXIL has it as one operation
    if (_m.constant_bits(a[1], low) && _m.constant_bits(a[2], high) && low == 0
        && high == bits_of_float(1.0, _m.bits_of(_m.type_of(a[0]))))
        return dx_unary(dx_op::saturate, a[0]);
    return dx_binary(dx_op::fmin, dx_binary(dx_op::fmax, a[0], a[1]), a[2]);
}

ll_value writer::lane_sclamp(cc::span<ll_value const> a, u8)
{
    return dx_binary(dx_op::imin, dx_binary(dx_op::imax, a[0], a[1]), a[2]);
}

ll_value writer::lane_uclamp(cc::span<ll_value const> a, u8)
{
    return dx_binary(dx_op::umin, dx_binary(dx_op::umax, a[0], a[1]), a[2]);
}

ll_value writer::lane_fmix(cc::span<ll_value const> a, u8 flags)
{
    auto const difference = _m.binary(binary_op::sub, a[1], a[0], flags);
    return _m.binary(binary_op::add, a[0], _m.binary(binary_op::mul, difference, a[2], flags), flags);
}

ll_value writer::lane_step(cc::span<ll_value const> a, u8 flags)
{
    auto const below = _m.compare(predicate::f_olt, a[1], a[0], flags);
    return _m.select(below, float_like(a[0], 0.0), float_like(a[0], 1.0));
}

ll_value writer::lane_smooth_step(cc::span<ll_value const> a, u8 flags)
{
    auto const span = _m.binary(binary_op::sub, a[1], a[0], flags);
    auto const raw = _m.binary(binary_op::sdiv, _m.binary(binary_op::sub, a[2], a[0], flags), span, flags);
    auto const t = dx_unary(dx_op::saturate, raw);
    auto const shaped
        = _m.binary(binary_op::sub, float_like(t, 3.0), _m.binary(binary_op::mul, float_like(t, 2.0), t, flags), flags);
    return _m.binary(binary_op::mul, _m.binary(binary_op::mul, t, t, flags), shaped, flags);
}

ll_value writer::lane_fma(cc::span<ll_value const> a, u8)
{
    auto const is_double = _m.bits_of(_m.type_of(a[0])) == 64;
    _uses.double_extensions = _uses.double_extensions || is_double;
    return dx_tertiary(is_double ? dx_op::fma : dx_op::fmad, a[0], a[1], a[2]);
}

ll_value writer::lane_find_msb(ll_value counted, u32 width)
{
    // DXIL counts from the top bit, and -1 says no bit stands
    auto const type = _m.type_of(counted);
    auto const none = _m.constant_int(type, ~u64(0));
    auto const position = _m.binary(binary_op::sub, _m.constant_int(type, width - 1), counted);
    return _m.select(_m.compare(predicate::i_eq, counted, none), none, position);
}

ll_value writer::lane_umsb(cc::span<ll_value const> a, u8)
{
    return lane_find_msb(dx_bits(dx_op::firstbit_hi, a[0]), _m.bits_of(_m.type_of(a[0])));
}

ll_value writer::lane_smsb(cc::span<ll_value const> a, u8)
{
    return lane_find_msb(dx_bits(dx_op::firstbit_shi, a[0]), _m.bits_of(_m.type_of(a[0])));
}

ll_value writer::lane_ilsb(cc::span<ll_value const> a, u8)
{
    return dx_bits(dx_op::firstbit_lo, a[0]);
}

ll_value writer::dx_bits(dx_op op, ll_value a)
{
    ll_value const args[] = {_m.constant_i32(k_dx_ops[isize(op)].number), a};
    return call(op, _m.type_of(a), args);
}

// ---- those whose lanes depend on each other

void writer::whole_length(instruction const& i, u8 flags)
{
    auto const v = value_ref(_p.operands_of(i)[0]);
    auto const lanes = count_of(v);
    if (lanes == 1)
    {
        _scratch.push_back(dx_unary(dx_op::fabs, leaf_at(v)));
        return;
    }
    ll_value parts[4];
    for (auto l = u32(0); l < lanes; ++l)
        parts[l] = leaf_at(v, l);
    auto const span = cc::span<ll_value const>(parts, lanes);
    _scratch.push_back(dx_unary(dx_op::sqrt, dot_of(span, span, flags)));
}

void writer::whole_distance(instruction const& i, u8 flags)
{
    auto const operands = _p.operands_of(i);
    auto const a = value_ref(operands[0]);
    auto const b = value_ref(operands[1]);
    auto const lanes = count_of(a);
    ll_value parts[4];
    for (auto l = u32(0); l < lanes; ++l)
        parts[l] = _m.binary(binary_op::sub, leaf_at(a, l), leaf_at(b, l), flags);
    if (lanes == 1)
    {
        _scratch.push_back(dx_unary(dx_op::fabs, parts[0]));
        return;
    }
    auto const span = cc::span<ll_value const>(parts, lanes);
    _scratch.push_back(dx_unary(dx_op::sqrt, dot_of(span, span, flags)));
}

void writer::whole_cross(instruction const& i, u8 flags)
{
    auto const operands = _p.operands_of(i);
    auto const a = value_ref(operands[0]);
    auto const b = value_ref(operands[1]);
    auto const minor = [&](u32 p, u32 q)
    {
        return _m.binary(binary_op::sub, _m.binary(binary_op::mul, leaf_at(a, p), leaf_at(b, q), flags),
                         _m.binary(binary_op::mul, leaf_at(a, q), leaf_at(b, p), flags), flags);
    };
    _scratch.push_back(minor(1, 2));
    _scratch.push_back(minor(2, 0));
    _scratch.push_back(minor(0, 1));
}

void writer::whole_normalize(instruction const& i, u8 flags)
{
    auto const v = value_ref(_p.operands_of(i)[0]);
    auto const lanes = count_of(v);
    if (lanes == 1)
    {
        _scratch.push_back(_m.binary(binary_op::sdiv, leaf_at(v), dx_unary(dx_op::fabs, leaf_at(v)), flags));
        return;
    }
    ll_value parts[4];
    for (auto l = u32(0); l < lanes; ++l)
        parts[l] = leaf_at(v, l);
    auto const span = cc::span<ll_value const>(parts, lanes);
    auto const inverse = dx_unary(dx_op::rsqrt, dot_of(span, span, flags));
    for (auto l = u32(0); l < lanes; ++l)
        _scratch.push_back(_m.binary(binary_op::mul, parts[l], inverse, flags));
}

void writer::whole_reflect(instruction const& i, u8 flags)
{
    auto const operands = _p.operands_of(i);
    auto const v = value_ref(operands[0]);
    auto const n = value_ref(operands[1]);
    auto const lanes = count_of(v);
    ll_value left[4];
    ll_value right[4];
    for (auto l = u32(0); l < lanes; ++l)
    {
        left[l] = leaf_at(n, l);
        right[l] = leaf_at(v, l);
    }
    auto const d = dot_of(cc::span<ll_value const>(left, lanes), cc::span<ll_value const>(right, lanes), flags);
    auto const twice = _m.binary(binary_op::mul, float_like(d, 2.0), d, flags);
    for (auto l = u32(0); l < lanes; ++l)
        _scratch.push_back(_m.binary(binary_op::sub, right[l], _m.binary(binary_op::mul, twice, left[l], flags), flags));
}

void writer::whole_refract(instruction const& i, u8 flags)
{
    auto const operands = _p.operands_of(i);
    auto const v = value_ref(operands[0]);
    auto const n = value_ref(operands[1]);
    auto const eta = leaf_at(value_ref(operands[2]));
    auto const lanes = count_of(v);
    ll_value left[4];
    ll_value right[4];
    for (auto l = u32(0); l < lanes; ++l)
    {
        left[l] = leaf_at(n, l);
        right[l] = leaf_at(v, l);
    }
    auto const d = dot_of(cc::span<ll_value const>(left, lanes), cc::span<ll_value const>(right, lanes), flags);
    auto const one = float_like(d, 1.0);
    auto const sine = _m.binary(binary_op::sub, one, _m.binary(binary_op::mul, d, d, flags), flags);
    auto const k = _m.binary(binary_op::sub, one,
                             _m.binary(binary_op::mul, _m.binary(binary_op::mul, eta, eta, flags), sine, flags), flags);
    auto const is_total = _m.compare(predicate::f_olt, k, float_like(d, 0.0), flags);
    auto const term
        = _m.binary(binary_op::add, _m.binary(binary_op::mul, eta, d, flags), dx_unary(dx_op::sqrt, k), flags);
    for (auto l = u32(0); l < lanes; ++l)
    {
        auto const bent = _m.binary(binary_op::sub, _m.binary(binary_op::mul, eta, right[l], flags),
                                    _m.binary(binary_op::mul, term, left[l], flags), flags);
        _scratch.push_back(_m.select(is_total, float_like(d, 0.0), bent));
    }
}

void writer::math_op(u32 index, instruction const& i)
{
    using lane_fn = ll_value (writer::*)(cc::span<ll_value const>, u8);
    using whole_fn = void (writer::*)(instruction const&, u8);
    struct rule
    {
        math fn;
        /// DXIL's own operation, or -1 where a function below writes it.
        i32 op = -1;
        u8 operands = 1;
        lane_fn lane = nullptr;
        whole_fn whole = nullptr;
    };
    // a new `math` is a row here; one with no row stays a named gap
    static constexpr rule k_rules[] = {
        {math::round_even, i32(dx_op::round_ne)},
        {math::trunc, i32(dx_op::round_z)},
        {math::fabs, i32(dx_op::fabs)},
        {math::sabs, -1, 1, &writer::lane_sabs},
        {math::fsign, -1, 1, &writer::lane_fsign},
        {math::ssign, -1, 1, &writer::lane_ssign},
        {math::floor, i32(dx_op::round_ni)},
        {math::ceil, i32(dx_op::round_pi)},
        {math::fract, i32(dx_op::frc)},
        {math::sin, i32(dx_op::sin)},
        {math::cos, i32(dx_op::cos)},
        {math::tan, i32(dx_op::tan)},
        {math::asin, i32(dx_op::asin)},
        {math::acos, i32(dx_op::acos)},
        {math::atan, i32(dx_op::atan)},
        {math::sinh, i32(dx_op::hsin)},
        {math::cosh, i32(dx_op::hcos)},
        {math::tanh, i32(dx_op::htan)},
        {math::atan2, -1, 2, &writer::lane_atan2},
        {math::pow, -1, 2, &writer::lane_pow},
        {math::exp, -1, 1, &writer::lane_exp},
        {math::log, -1, 1, &writer::lane_log},
        {math::exp2, i32(dx_op::exp)},
        {math::log2, i32(dx_op::log)},
        {math::sqrt, i32(dx_op::sqrt)},
        {math::inverse_sqrt, i32(dx_op::rsqrt)},
        {math::fmin, i32(dx_op::fmin), 2},
        {math::umin, i32(dx_op::umin), 2},
        {math::smin, i32(dx_op::imin), 2},
        {math::fmax, i32(dx_op::fmax), 2},
        {math::umax, i32(dx_op::umax), 2},
        {math::smax, i32(dx_op::imax), 2},
        {math::fclamp, -1, 3, &writer::lane_fclamp},
        {math::uclamp, -1, 3, &writer::lane_uclamp},
        {math::sclamp, -1, 3, &writer::lane_sclamp},
        {math::fmix, -1, 3, &writer::lane_fmix},
        {math::step, -1, 2, &writer::lane_step},
        {math::smooth_step, -1, 3, &writer::lane_smooth_step},
        {math::fma, -1, 3, &writer::lane_fma},
        {math::find_ilsb, -1, 1, &writer::lane_ilsb},
        {math::find_smsb, -1, 1, &writer::lane_smsb},
        {math::find_umsb, -1, 1, &writer::lane_umsb},
        {math::length, -1, 1, nullptr, &writer::whole_length},
        {math::distance, -1, 2, nullptr, &writer::whole_distance},
        {math::cross, -1, 2, nullptr, &writer::whole_cross},
        {math::normalize, -1, 1, nullptr, &writer::whole_normalize},
        {math::reflect, -1, 2, nullptr, &writer::whole_reflect},
        {math::refract, -1, 3, nullptr, &writer::whole_refract},
    };

    auto const fn = math(i.sub);
    rule const* found = nullptr;
    for (auto const& r : k_rules)
        if (r.fn == fn)
            found = &r;
    if (found == nullptr)
        return unsupported(cc::format("the math function {} on DXIL", i.sub));

    auto const flags = flags_of(i);
    auto const operands = _p.operands_of(i);
    _scratch.clear();
    if (found->whole != nullptr)
    {
        (this->*found->whole)(i, flags);
        return define(index);
    }
    auto const lanes = count_of(value_ref(operands[0]));
    for (auto l = u32(0); l < lanes; ++l)
    {
        ll_value args[3];
        for (auto k = isize(0); k < operands.size() && k < 3; ++k)
        {
            auto const v = value_ref(operands[k]);
            args[k] = leaf_at(v, count_of(v) == 1 ? 0 : l);
        }
        if (found->lane != nullptr)
        {
            _scratch.push_back((this->*found->lane)(cc::span<ll_value const>(args, operands.size()), flags));
            continue;
        }
        auto const op = dx_op(found->op);
        _scratch.push_back(found->operands == 2 ? dx_binary(op, args[0], args[1]) : dx_unary(op, args[0]));
    }
    define(index);
}
