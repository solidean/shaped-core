#include "simplify.hh"

#include <clean-core/math/bit.hh>
#include <clean-core/math/float.hh>
#include <shaped-graphics-language/builtins/impl/soft_math.hh>
#include <shaped-graphics-language/ir/ir.hh>
#include <shaped-graphics-language/ir/verify.hh>

using namespace sgl;
using namespace sgl::ir;

// A walk in the order blocks opened, a dominance order, which rewrites the tables.
// It repeats while a folded branch makes a phi trivial, which may fold another branch.

namespace
{
constexpr u32 k_no_target = 0xffffffffu;
constexpr int k_most_fold_rounds = 4;

/// What an operation with both operands known gives; a row is what adds a case.
enum class fold_kind : u8
{
    none,
    integer,
    compare,
    logical,
    real,
    real_compare,
    negate,
};

struct fold_rule
{
    op code = op::unreachable;
    fold_kind kind = fold_kind::none;
};

constexpr fold_rule k_folds[] = {
    {op::iadd, fold_kind::integer},
    {op::isub, fold_kind::integer},
    {op::imul, fold_kind::integer},
    {op::bit_and, fold_kind::integer},
    {op::bit_or, fold_kind::integer},
    {op::bit_xor, fold_kind::integer},
    {op::shift_left, fold_kind::integer},
    {op::shift_right_logical, fold_kind::integer},
    {op::shift_right_arithmetic, fold_kind::integer},
    {op::sdiv, fold_kind::integer},
    {op::udiv, fold_kind::integer},
    {op::srem, fold_kind::integer},
    {op::urem, fold_kind::integer},
    {op::ieq, fold_kind::compare},
    {op::ine, fold_kind::compare},
    {op::slt, fold_kind::compare},
    {op::sle, fold_kind::compare},
    {op::sgt, fold_kind::compare},
    {op::sge, fold_kind::compare},
    {op::ult, fold_kind::compare},
    {op::ule, fold_kind::compare},
    {op::ugt, fold_kind::compare},
    {op::uge, fold_kind::compare},
    {op::logical_and, fold_kind::logical},
    {op::logical_or, fold_kind::logical},
    {op::logical_eq, fold_kind::logical},
    {op::logical_ne, fold_kind::logical},
    {op::fadd, fold_kind::real},
    {op::fsub, fold_kind::real},
    {op::fmul, fold_kind::real},
    {op::fdiv, fold_kind::real},
    {op::feq, fold_kind::real_compare},
    {op::fne, fold_kind::real_compare},
    {op::flt, fold_kind::real_compare},
    {op::fle, fold_kind::real_compare},
    {op::fgt, fold_kind::real_compare},
    {op::fge, fold_kind::real_compare},
};

[[nodiscard]] constexpr fold_kind fold_kind_of(op code)
{
    for (auto const& r : k_folds)
        if (r.code == code)
            return r.kind;
    return fold_kind::none;
}

[[nodiscard]] constexpr u64 mask_of(u32 width)
{
    return width >= 64 ? ~u64(0) : (u64(1) << width) - 1;
}

[[nodiscard]] constexpr i64 signed_of(u64 bits, u32 width)
{
    return width >= 64 ? i64(bits) : i64(bits << (64 - width)) >> (64 - width);
}

class simplifier
{
public:
    simplifier(program& p, control_flow flow) : _p(p), _keeps_structure(flow == control_flow::structured) {}

    void run()
    {
        for (auto round = 0; round < k_most_fold_rounds; ++round)
        {
            prepare();
            fold();
            auto const changed = _changed;
            rebuild();
            if (!changed)
                break;
        }
    }

private:
    void prepare()
    {
        _order.clear();
        for (auto const b : blocks_in_order(_p))
            if (_p.blocks[isize(b)].count > 0)
                _order.push_back(b);
        _alias = cc::vector<value_ref>::create_filled(_p.instructions.size(), value_ref::none);
        _reached = cc::vector<bool>::create_filled(_p.blocks.size(), false);
        _visited = cc::vector<bool>::create_filled(_p.blocks.size(), false);
        _target = cc::vector<u32>::create_filled(_p.blocks.size(), k_no_target);
        _skipped = cc::vector<bool>::create_filled(_p.blocks.size(), false);
        _changed = false;
    }

    [[nodiscard]] value_ref resolved(value_ref v) const
    {
        while (is_valid(v) && !is_constant(v) && !is_global(v) && !is_local(v) && is_valid(_alias[index_of(v)]))
            v = _alias[index_of(v)];
        return v;
    }

    [[nodiscard]] bool scalar_bits(value_ref v, u64& bits, u32& width, bool& is_float) const
    {
        if (!is_constant(v))
            return false;
        auto const& c = _p.constants[index_of(v)];
        if (c.is_undefined)
            return false;
        auto const& t = _p.at(c.type);
        if (t.kind != type_kind::int_ && t.kind != type_kind::float_ && t.kind != type_kind::bool_)
            return false;
        bits = c.is_null ? 0 : (u64(c.high) << 32) | c.low;
        width = t.kind == type_kind::bool_ ? 1 : t.width;
        is_float = t.kind == type_kind::float_;
        return true;
    }

    [[nodiscard]] value_ref constant_of(type_ref type, u64 bits)
    {
        for (auto i = _p.constants.size(); i > cc::max(isize(0), _p.constants.size() - 64); --i)
        {
            auto const& c = _p.constants[i - 1];
            if (c.type == type && !c.is_null && !c.is_undefined && c.low == u32(bits) && c.high == u32(bits >> 32))
                return constant_at(u32(i - 1));
        }
        _p.constants.push_back({.type = type, .low = u32(bits), .high = u32(bits >> 32)});
        return constant_at(u32(_p.constants.size() - 1));
    }

    void set_alias(u32 index, value_ref v)
    {
        _alias[index] = v;
        _changed = true;
    }

    /// The one value every lane of `v` holds, where `v` is a float known here.
    [[nodiscard]] bool known_float(value_ref v, f64& value, int depth = 0) const
    {
        if (is_constant(v))
        {
            auto const& c = _p.constants[index_of(v)];
            if (c.is_undefined)
                return false;
            if (c.is_null)
            {
                value = 0.0;
                return true;
            }
            auto const& t = _p.at(c.type);
            if (t.kind != type_kind::float_)
                return false;
            if (t.width == 32)
                value = f64(cc::bit_cast<f32>(c.low));
            else if (t.width == 64)
                value = cc::bit_cast<f64>((u64(c.high) << 32) | c.low);
            else if (t.width == 16 && (c.low & 0x7fffu) == 0)
                value = 0.0;
            else if (t.width == 16 && c.low == 0x3c00u)
                value = 1.0;
            else
                return false;
            return true;
        }
        if (!is_operation(v) || depth > 2)
            return false;
        auto const& i = _p.instructions[index_of(v)];
        if (i.code != op::construct)
            return false;
        auto const parts = _p.operands_of(i);
        for (auto k = isize(0); k < parts.size(); ++k)
        {
            auto lane = 0.0;
            if (!known_float(resolved(value_ref(parts[k])), lane, depth + 1) || (k > 0 && lane != value))
                return false;
            value = lane;
        }
        return !parts.empty();
    }

    /// The lanes of a 32-bit float or a vector of up to four of them, where every one is known here.
    struct lanes
    {
        u32 count = 0;
        f32 at[4] = {};
    };

    [[nodiscard]] bool is_float32_shape(type_ref t, u32& count) const
    {
        auto const& info = _p.at(t);
        if (info.kind == type_kind::float_ && info.width == 32)
        {
            count = 1;
            return true;
        }
        if (info.kind != type_kind::vector || info.count < 2 || info.count > 4)
            return false;
        auto const& element = _p.at(info.element);
        count = info.count;
        return element.kind == type_kind::float_ && element.width == 32;
    }

    [[nodiscard]] bool known_lanes(value_ref v, lanes& out) const
    {
        auto count = u32(0);
        if (is_constant(v))
        {
            auto const& c = _p.constants[index_of(v)];
            if (c.is_undefined || !is_float32_shape(c.type, count))
                return false;
            out.count = count;
            for (auto k = u32(0); k < count; ++k)
                out.at[k] = c.is_null ? 0.0f : cc::bit_cast<f32>(c.low);
            return true;
        }
        if (!is_operation(v))
            return false;
        auto const& i = _p.instructions[index_of(v)];
        if (i.code != op::construct || !is_float32_shape(i.type, count))
            return false;
        auto const parts = _p.operands_of(i);
        if (isize(count) != parts.size())
            return false;
        out.count = count;
        for (auto k = u32(0); k < count; ++k)
        {
            auto part = lanes();
            if (!known_lanes(resolved(value_ref(parts[k])), part) || part.count != 1)
                return false;
            out.at[k] = part.at[0];
        }
        return true;
    }

    /// `index` as the value `known` holds: a constant for a scalar, a construct of constants rewritten in place for a vector.
    void set_known(u32 index, lanes const& known)
    {
        for (auto k = u32(0); k < known.count; ++k)
            if (!cc::is_finite(known.at[k]))
                return;
        auto const type = _p.instructions[index].type;
        if (known.count == 1)
            return set_alias(index, constant_of(type, cc::bit_cast<u32>(known.at[0])));
        auto const element = _p.at(type).element;
        auto parts = cc::vector<u32>();
        for (auto k = u32(0); k < known.count; ++k)
            parts.push_back(u32(constant_of(element, cc::bit_cast<u32>(known.at[k]))));
        auto const first = u32(_p.operands.size());
        _p.operands.push_back_range(parts);
        auto& made = _p.instructions[index];
        made.code = op::construct;
        made.sub = 0;
        made.aux = 0;
        made.first = first;
        made.count = known.count;
        _changed = true;
    }

    /// An operation of float lanes all known here, worked out as the target would: arithmetic at every level, as the
    /// scalar fold does, and a builtin only at `.fast`, where a builtin is as precise as its target states (EVAL-6).
    [[nodiscard]] bool fold_lanes(u32 index, instruction const& i, cc::span<u32 const> operands)
    {
        auto count = u32(0);
        if (!is_float32_shape(i.type, count) || operands.size() > 3 || operands.empty())
            return false;
        lanes args[3];
        for (auto k = isize(0); k < operands.size(); ++k)
            if (!known_lanes(resolved(value_ref(operands[k])), args[k]))
                return false;
        // a scalar beside vectors is the same in every lane
        auto const lane = [&](isize k, u32 l) { return args[k].count == 1 ? args[k].at[0] : args[k].at[l]; };
        auto result = lanes{.count = count};
        auto const each = [&](auto&& f)
        {
            for (auto l = u32(0); l < count; ++l)
                result.at[l] = f(l);
            return true;
        };
        auto const is_fast = i.guarantee == check::float_level::fast;
        auto done = false;
        switch (i.code)
        {
        case op::fadd:
            done = operands.size() == 2 && each([&](u32 l) { return lane(0, l) + lane(1, l); });
            break;
        case op::fsub:
            done = operands.size() == 2 && each([&](u32 l) { return lane(0, l) - lane(1, l); });
            break;
        case op::fmul:
            done = operands.size() == 2 && each([&](u32 l) { return lane(0, l) * lane(1, l); });
            break;
        case op::fdiv:
            done = operands.size() == 2 && each([&](u32 l) { return lane(0, l) / lane(1, l); });
            break;
        case op::fneg:
            done = operands.size() == 1 && each([&](u32 l) { return -lane(0, l); });
            break;
        case op::dot:
            if (is_fast && operands.size() == 2 && count == 1 && args[0].count == args[1].count)
            {
                auto sum = 0.0f;
                for (auto l = u32(0); l < args[0].count; ++l)
                    sum += args[0].at[l] * args[1].at[l];
                result.at[0] = sum;
                done = true;
            }
            break;
        case op::math:
            if (is_fast)
                done = fold_math(math(i.sub), operands.size(), args, count, result);
            break;
        default:
            break;
        }
        if (!done)
            return false;
        set_known(index, result);
        return true;
    }

    /// Through soft_math rather than a libm, so every host folds a builtin to the same bits, as the interpreter does.
    /// A NaN out stops the fold, so what a builtin gives outside its domain stays the target's to say.
    [[nodiscard]] static bool fold_math(math m, isize n, lanes const (&args)[3], u32 count, lanes& result)
    {
        namespace soft = sgl::builtins::impl;
        auto const lane = [&](isize k, u32 l) { return f64(args[k].count == 1 ? args[k].at[0] : args[k].at[l]); };
        auto const each = [&](isize needs, auto&& f)
        {
            if (n != needs)
                return false;
            for (auto l = u32(0); l < count; ++l)
                result.at[l] = f32(f(l));
            return true;
        };
        constexpr auto ln2 = 0.693147180559945309417;
        auto const nan = cc::bit_cast<f64>(u64(0x7ff8000000000000ull));
        auto const positive = [&](f64 x, auto&& f) { return x > 0.0 ? f(x) : nan; };
        switch (m)
        {
        case math::fabs:
            return each(1, [&](u32 l) { return lane(0, l) < 0.0 ? -lane(0, l) : lane(0, l); });
        case math::floor:
            return each(1, [&](u32 l) { return soft::soft_floor(lane(0, l)); });
        case math::ceil:
            return each(1, [&](u32 l) { return -soft::soft_floor(-lane(0, l)); });
        case math::trunc:
            return each(1, [&](u32 l) { return soft::soft_trunc(lane(0, l)); });
        case math::fract:
            return each(1, [&](u32 l) { return lane(0, l) - soft::soft_floor(lane(0, l)); });
        case math::sqrt:
            return each(1, [&](u32 l) { return lane(0, l) < 0.0 ? nan : soft::soft_sqrt(lane(0, l)); });
        case math::inverse_sqrt:
            return each(1, [&](u32 l) { return positive(lane(0, l), [](f64 x) { return 1.0 / soft::soft_sqrt(x); }); });
        case math::exp:
            return each(1, [&](u32 l) { return soft::soft_exp(lane(0, l)); });
        case math::exp2:
            return each(1, [&](u32 l) { return soft::soft_exp(lane(0, l) * ln2); });
        case math::log:
            return each(1, [&](u32 l) { return positive(lane(0, l), [](f64 x) { return soft::soft_log(x); }); });
        case math::log2:
            return each(1, [&](u32 l) { return positive(lane(0, l), [&](f64 x) { return soft::soft_log(x) / ln2; }); });
        case math::sin:
            return each(1, [&](u32 l) { return soft::soft_sin(lane(0, l)); });
        case math::cos:
            return each(1, [&](u32 l) { return soft::soft_cos(lane(0, l)); });
        case math::fsign:
            return each(1, [&](u32 l) { return lane(0, l) > 0.0 ? 1.0 : lane(0, l) < 0.0 ? -1.0 : 0.0; });
        case math::fmin:
            return each(2, [&](u32 l) { return lane(1, l) < lane(0, l) ? lane(1, l) : lane(0, l); });
        case math::fmax:
            return each(2, [&](u32 l) { return lane(1, l) > lane(0, l) ? lane(1, l) : lane(0, l); });
        case math::pow:
            return each(2,
                        [&](u32 l)
                        {
                            auto const y = lane(1, l);
                            return positive(lane(0, l), [&](f64 x) { return soft::soft_exp(y * soft::soft_log(x)); });
                        });
        case math::step:
            return each(2, [&](u32 l) { return lane(1, l) < lane(0, l) ? 0.0 : 1.0; });
        case math::fclamp:
            return each(3,
                        [&](u32 l)
                        {
                            auto const raised = lane(0, l) < lane(1, l) ? lane(1, l) : lane(0, l);
                            return raised > lane(2, l) ? lane(2, l) : raised;
                        });
        case math::fmix:
            return each(3, [&](u32 l) { return lane(0, l) + (lane(1, l) - lane(0, l)) * lane(2, l); });
        case math::fma:
            return each(3, [&](u32 l) { return lane(0, l) * lane(1, l) + lane(2, l); });
        case math::length:
        {
            if (n != 1 || count != 1)
                return false;
            auto sum = 0.0;
            for (auto l = u32(0); l < args[0].count; ++l)
                sum += f64(args[0].at[l]) * f64(args[0].at[l]);
            result.at[0] = f32(soft::soft_sqrt(sum));
            return true;
        }
        default:
            return false;
        }
    }

    /// The zero of `type`, a scalar or a vector.
    [[nodiscard]] value_ref zero_of(type_ref type)
    {
        for (auto i = _p.constants.size(); i > 0; --i)
            if (_p.constants[i - 1].type == type && _p.constants[i - 1].is_null)
                return constant_at(u32(i - 1));
        _p.constants.push_back({.type = type, .is_null = true});
        return constant_at(u32(_p.constants.size() - 1));
    }

    /// EVAL-6's `.fast`: no NaN or infinity arises and a zero's sign is not kept, so a known zero or one decides an
    /// operation of the other operand alone, which DXC's fast flags let it do and a lobe a material leaves at zero needs.
    [[nodiscard]] bool fold_fast(u32 index, instruction const& i, value_ref a, value_ref b)
    {
        auto x = 0.0;
        auto y = 0.0;
        auto const has_a = known_float(a, x);
        auto const has_b = known_float(b, y);
        auto const same = _p.type_of(a) == i.type && _p.type_of(b) == i.type;
        if (!same || (has_a && has_b))
            return false;
        switch (i.code)
        {
        case op::fmul:
            if ((has_a && x == 0.0) || (has_b && y == 0.0))
                return set_alias(index, zero_of(i.type)), true;
            if (has_a && x == 1.0)
                return set_alias(index, b), true;
            if (has_b && y == 1.0)
                return set_alias(index, a), true;
            if (has_b)
                return reassociate(index, i, a, y);
            return has_a && reassociate(index, i, b, x);
        case op::fadd:
            if (has_a && x == 0.0)
                return set_alias(index, b), true;
            if (has_b && y == 0.0)
                return set_alias(index, a), true;
            return false;
        case op::fsub:
            if (has_b && y == 0.0)
                return set_alias(index, a), true;
            if (a == b)
                return set_alias(index, zero_of(i.type)), true;
            return false;
        case op::fdiv:
            if (has_b && y == 1.0)
                return set_alias(index, a), true;
            if (has_a && x == 0.0)
                return set_alias(index, zero_of(i.type)), true;
            return has_b && y != 0.0 && reassociate(index, i, a, 1.0 / y);
        default:
            return false;
        }
    }

    /// `.fast` reassociates, so `(v * c) / d` is `v * (c / d)`, and `v` itself where those cancel: bc7's `x * 255 / 255`.
    /// `by` is what the operation multiplies `a` by; a factor that is no splat of one leaves a scalar alone, since only
    /// a scalar's constant is made here.
    [[nodiscard]] bool reassociate(u32 index, instruction const& i, value_ref a, f64 by)
    {
        if (!is_operation(a))
            return false;
        auto const& inner = _p.instructions[index_of(a)];
        if ((inner.code != op::fmul && inner.code != op::fdiv) || inner.guarantee != check::float_level::fast
            || inner.type != i.type)
            return false;
        auto const parts = _p.operands_of(inner);
        auto const left = resolved(value_ref(parts[0]));
        auto const right = resolved(value_ref(parts[1]));
        auto c = 0.0;
        auto kept = value_ref::none;
        if (known_float(right, c) && c != 0.0)
            kept = left;
        else if (inner.code == op::fmul && known_float(left, c))
            kept = right;
        else
            return false;
        auto const factor = (inner.code == op::fmul ? c : 1.0 / c) * by;
        if (factor == 1.0)
            return set_alias(index, kept), true;
        auto const& t = _p.at(i.type);
        if (t.kind != type_kind::float_ || t.width != 32 || !cc::is_finite(factor) || factor == 0.0)
            return false;
        // this operation becomes the one product, and the inner one is left for remove_unused
        auto& self = _p.instructions[index];
        self.code = op::fmul;
        _p.operands[self.first] = u32(kept);
        _p.operands[self.first + 1] = u32(constant_of(i.type, cc::bit_cast<u32>(f32(factor))));
        _changed = true;
        return true;
    }

    void fold_binary(u32 index, instruction const& i, fold_kind kind, value_ref a, value_ref b)
    {
        auto x = u64(0);
        auto y = u64(0);
        auto width = u32(0);
        auto other_width = u32(0);
        auto is_float = false;
        auto is_other_float = false;
        auto const has_a = scalar_bits(a, x, width, is_float);
        auto const has_b = scalar_bits(b, y, other_width, is_other_float);

        // one operand known is enough for what leaves the other as it is
        if (has_b && !has_a && !is_other_float)
        {
            auto const is_zero = y == 0;
            if ((i.code == op::iadd || i.code == op::isub || i.code == op::bit_or || i.code == op::bit_xor
                 || i.code == op::shift_left || i.code == op::shift_right_logical || i.code == op::shift_right_arithmetic)
                && is_zero)
                return set_alias(index, a);
            if (i.code == op::imul && y == 1)
                return set_alias(index, a);
        }
        if (has_a && !has_b && !is_float)
        {
            if ((i.code == op::iadd || i.code == op::bit_or || i.code == op::bit_xor) && x == 0)
                return set_alias(index, b);
            if (i.code == op::imul && x == 1)
                return set_alias(index, b);
        }
        if (i.code == op::fmul && has_b && !has_a && is_other_float && other_width == 32 && y == 0x3f800000u)
            return set_alias(index, a);
        if (i.code == op::fmul && has_a && !has_b && is_float && width == 32 && x == 0x3f800000u)
            return set_alias(index, b);
        if ((i.code == op::logical_and || i.code == op::logical_or) && (has_a != has_b))
        {
            auto const known = has_a ? x : y;
            auto const rest = has_a ? b : a;
            auto const decides = i.code == op::logical_and ? known == 0 : known != 0;
            return set_alias(index, decides ? (has_a ? a : b) : rest);
        }
        if (!has_a || !has_b)
            return;

        auto const type = i.type;
        switch (kind)
        {
        case fold_kind::integer:
        {
            auto const sx = signed_of(x, width);
            auto const sy = signed_of(y, width);
            auto const count = y & (width - 1);
            auto result = u64(0);
            switch (i.code)
            {
            case op::iadd:
                result = x + y;
                break;
            case op::isub:
                result = x - y;
                break;
            case op::imul:
                result = x * y;
                break;
            case op::bit_and:
                result = x & y;
                break;
            case op::bit_or:
                result = x | y;
                break;
            case op::bit_xor:
                result = x ^ y;
                break;
            case op::shift_left:
                result = x << count;
                break;
            case op::shift_right_logical:
                result = (x & mask_of(width)) >> count;
                break;
            case op::shift_right_arithmetic:
                result = u64(sx >> count);
                break;
            case op::sdiv:
                // a division with no value stays for the target to answer
                if (sy == 0 || (sy == -1 && u64(sx) == (u64(1) << (width - 1))))
                    return;
                result = u64(sx / sy);
                break;
            case op::srem:
                if (sy == 0 || sy == -1)
                    return;
                result = u64(sx % sy);
                break;
            case op::udiv:
                if ((y & mask_of(width)) == 0)
                    return;
                result = (x & mask_of(width)) / (y & mask_of(width));
                break;
            case op::urem:
                if ((y & mask_of(width)) == 0)
                    return;
                result = (x & mask_of(width)) % (y & mask_of(width));
                break;
            default:
                return;
            }
            return set_alias(index, constant_of(type, result & mask_of(width)));
        }
        case fold_kind::compare:
        {
            auto const sx = signed_of(x, width);
            auto const sy = signed_of(y, width);
            auto const ux = x & mask_of(width);
            auto const uy = y & mask_of(width);
            auto result = false;
            switch (i.code)
            {
            case op::ieq:
                result = ux == uy;
                break;
            case op::ine:
                result = ux != uy;
                break;
            case op::slt:
                result = sx < sy;
                break;
            case op::sle:
                result = sx <= sy;
                break;
            case op::sgt:
                result = sx > sy;
                break;
            case op::sge:
                result = sx >= sy;
                break;
            case op::ult:
                result = ux < uy;
                break;
            case op::ule:
                result = ux <= uy;
                break;
            case op::ugt:
                result = ux > uy;
                break;
            case op::uge:
                result = ux >= uy;
                break;
            default:
                return;
            }
            return set_alias(index, constant_of(type, result ? 1 : 0));
        }
        case fold_kind::logical:
        {
            auto const p = x != 0;
            auto const q = y != 0;
            auto const result = i.code == op::logical_and ? p && q
                              : i.code == op::logical_or  ? p || q
                              : i.code == op::logical_eq  ? p == q
                                                          : p != q;
            return set_alias(index, constant_of(type, result ? 1 : 0));
        }
        case fold_kind::real:
        case fold_kind::real_compare:
        {
            if (width != 32 || !is_float)
                return;
            auto const p = cc::bit_cast<f32>(u32(x));
            auto const q = cc::bit_cast<f32>(u32(y));
            if (kind == fold_kind::real)
            {
                // a NaN's payload and a division's zero are the target's to say
                if (i.code == op::fdiv && q == 0.0f)
                    return;
                auto const value = i.code == op::fadd ? p + q
                                 : i.code == op::fsub ? p - q
                                 : i.code == op::fmul ? p * q
                                                      : p / q;
                if (value != value)
                    return;
                return set_alias(index, constant_of(type, cc::bit_cast<u32>(value)));
            }
            auto const result = i.code == op::feq ? p == q
                              : i.code == op::fne ? p != q
                              : i.code == op::flt ? p < q
                              : i.code == op::fle ? p <= q
                              : i.code == op::fgt ? p > q
                                                  : p >= q;
            return set_alias(index, constant_of(type, result ? 1 : 0));
        }
        default:
            return;
        }
    }

    /// The value `extract` lands on when what it reads was built here of parts one level down.
    void fold_extract(u32 index, value_ref source, cc::span<u32 const> operands)
    {
        if (is_constant(source) && _p.constants[index_of(source)].is_null)
            return set_alias(index, zero_of(_p.instructions[index].type));
        if (operands.size() != 2 || is_constant(source) || is_global(source) || is_local(source))
            return;
        auto const& built = _p.instructions[index_of(source)];
        if (built.code != op::construct)
            return;
        auto const parts = _p.operands_of(built);
        // one part per position, which a vector built of a smaller vector is not
        auto const& whole = _p.at(built.type);
        auto const expected = whole.kind == type_kind::structure ? whole.member_count : whole.count;
        if (isize(expected) != parts.size())
            return;
        set_alias(index, resolved(value_ref(parts[operands[1]])));
    }

    /// The vector a `construct` rebuilds when each part is the lane of one value at its own position.
    void fold_construct(u32 index, instruction const& i, cc::span<u32 const> operands)
    {
        auto const& whole = _p.at(i.type);
        if (whole.kind != type_kind::vector || isize(whole.count) != operands.size())
            return;
        auto source = value_ref::none;
        for (auto k = isize(0); k < operands.size(); ++k)
        {
            auto const part = resolved(value_ref(operands[k]));
            if (!is_operation(part))
                return;
            auto const& read = _p.instructions[index_of(part)];
            auto const from = _p.operands_of(read);
            if (read.code != op::extract || from.size() != 2 || from[1] != u32(k))
                return;
            auto const of = resolved(value_ref(from[0]));
            if (is_valid(source) && of != source)
                return;
            source = of;
        }
        auto const type = is_operation(source) ? _p.instructions[index_of(source)].type
                        : is_constant(source)  ? _p.constants[index_of(source)].type
                                               : type_ref::none;
        if (type == i.type)
            set_alias(index, source);
    }

    void fold_conversion(u32 index, instruction const& i, value_ref a)
    {
        auto x = u64(0);
        auto width = u32(0);
        auto is_float = false;
        if (!scalar_bits(a, x, width, is_float))
            return;
        auto const& target = _p.at(i.type);
        if (target.kind != type_kind::float_ || target.width != 32 || width > 32)
            return;
        if (i.code == op::s_to_f)
            set_alias(index, constant_of(i.type, cc::bit_cast<u32>(f32(signed_of(x, width)))));
        else if (i.code == op::u_to_f)
            set_alias(index, constant_of(i.type, cc::bit_cast<u32>(f32(x & mask_of(width)))));
    }

    void fold_select(u32 index, value_ref condition, value_ref yes, value_ref no)
    {
        if (yes == no)
            return set_alias(index, yes);
        auto x = u64(0);
        auto width = u32(0);
        auto is_float = false;
        if (scalar_bits(condition, x, width, is_float))
            set_alias(index, x != 0 ? yes : no);
    }

    void fold_phi(u32 index, instruction const& i, block_ref here)
    {
        auto const operands = _p.operands_of(i);
        auto unique = value_ref::none;
        auto const self = value_ref(index);
        for (auto k = isize(0); k < operands.size(); k += 2)
        {
            auto const from = block_ref(operands[k + 1]);
            if (!edge_is_live(from, here))
                continue;
            // a back edge's value is not known yet, so this phi stays
            if (!_visited[isize(from)])
                return;
            auto const v = resolved(value_ref(operands[k]));
            if (v == self || v == unique)
                continue;
            if (is_valid(unique))
                return;
            unique = v;
        }
        if (is_valid(unique))
            set_alias(index, unique);
    }

    [[nodiscard]] bool edge_is_live(block_ref from, block_ref to) const
    {
        // a block the walk passed without reaching it never runs, whatever its terminator says
        if (_skipped[isize(from)])
            return false;
        // a block not walked yet may be a back edge, and one that never is goes in a later round
        if (!_visited[isize(from)])
            return true;
        return _target[isize(from)] == k_no_target || _target[isize(from)] == u32(to);
    }

    void fold()
    {
        _reached[isize(_order[0])] = true;
        for (auto const b : _order)
        {
            if (!_reached[isize(b)])
            {
                _skipped[isize(b)] = true;
                continue;
            }
            auto const& block = _p.blocks[isize(b)];
            for (auto index = block.first; index + 1 < block.first + block.count; ++index)
            {
                auto const& i = _p.instructions[index];
                auto const operands = _p.operands_of(i);
                if (i.code == op::phi)
                {
                    fold_phi(index, i, b);
                    continue;
                }
                if (i.code == op::extract)
                {
                    fold_extract(index, resolved(value_ref(operands[0])), operands);
                    continue;
                }
                if (i.code == op::construct)
                {
                    fold_construct(index, i, operands);
                    continue;
                }
                if (i.code == op::select)
                {
                    fold_select(index, resolved(value_ref(operands[0])), resolved(value_ref(operands[1])),
                                resolved(value_ref(operands[2])));
                    continue;
                }
                if (i.code == op::s_to_f || i.code == op::u_to_f)
                {
                    fold_conversion(index, i, resolved(value_ref(operands[0])));
                    continue;
                }
                if ((i.code == op::fadd || i.code == op::fsub || i.code == op::fmul || i.code == op::fdiv
                     || i.code == op::fneg || i.code == op::dot || i.code == op::math)
                    && fold_lanes(index, i, operands))
                    continue;
                auto const kind = fold_kind_of(i.code);
                if (kind == fold_kind::real && i.guarantee == check::float_level::fast
                    && fold_fast(index, i, resolved(value_ref(operands[0])), resolved(value_ref(operands[1]))))
                    continue;
                if (kind != fold_kind::none)
                    fold_binary(index, i, kind, resolved(value_ref(operands[0])), resolved(value_ref(operands[1])));
            }

            auto const& last = _p.instructions[block.first + block.count - 1];
            auto const operands = _p.operands_of(last);
            // a selection's header may go to one side alone and state no merge, which leaves the other side unreachable
            // as SPIR-V allows; a loop's construct must keep its edges
            if (last.code == op::branch_conditional && (!_keeps_structure || block.merges == merge_kind::selection))
            {
                auto x = u64(0);
                auto width = u32(0);
                auto is_float = false;
                if (scalar_bits(resolved(value_ref(operands[0])), x, width, is_float))
                {
                    _target[isize(b)] = x != 0 ? operands[1] : operands[2];
                    _changed = true;
                }
                else if (operands[1] == operands[2])
                    _target[isize(b)] = operands[1];
            }
            _visited[isize(b)] = true;
            if (_target[isize(b)] != k_no_target)
                _reached[_target[isize(b)]] = true;
            else
                for (auto const to : successors_of(_p, last, _scratch))
                    _reached[to] = true;
        }
    }

    /// The tables again, without what a walk found dropped and with every operand as it resolved.
    void rebuild()
    {
        auto place = cc::vector<u32>::create_filled(_p.instructions.size(), 0u);
        auto next = u32(0);
        for (auto const b : _order)
        {
            if (!_visited[isize(b)])
            {
                // a kept block no walk reached is its terminator alone, `unreachable`
                next += is_kept(b) ? 1 : 0;
                continue;
            }
            auto const& block = _p.blocks[isize(b)];
            for (auto i = block.first; i < block.first + block.count; ++i)
                if (!is_valid(_alias[i]))
                    place[i] = next++;
        }

        auto const moved = [&](u32 raw) -> u32
        {
            auto const v = resolved(value_ref(raw));
            if (!is_valid(v) || is_constant(v) || is_global(v) || is_local(v))
                return u32(v);
            return place[index_of(v)];
        };

        auto instructions = cc::vector<instruction>::create_with_capacity(isize(next));
        auto operands = cc::vector<u32>::create_with_capacity(_p.operands.size());
        auto blocks = cc::vector<ir::block>::create_copy_of(cc::span<ir::block const>(_p.blocks));
        for (auto const b : _order)
        {
            if (!is_kept(b))
            {
                blocks[isize(b)].first = u32(instructions.size());
                blocks[isize(b)].count = 0;
                continue;
            }
            auto const& block = _p.blocks[isize(b)];
            auto const start = u32(instructions.size());
            // SPIR-V's merge and continue targets still name it, so it stays, as a label and `OpUnreachable`
            if (!_visited[isize(b)])
            {
                auto const& last = _p.instructions[block.first + block.count - 1];
                instructions.push_back(
                    {.code = op::unreachable, .first = u32(operands.size()), .count = 0, .statement = last.statement});
                blocks[isize(b)].first = start;
                blocks[isize(b)].count = 1;
                blocks[isize(b)].merges = merge_kind::none;
                continue;
            }
            for (auto i = block.first; i < block.first + block.count; ++i)
            {
                if (is_valid(_alias[i]))
                    continue;
                auto copy = _p.instructions[i];
                auto const held = _p.operands_of(copy);
                auto const first = u32(operands.size());
                auto const is_last = i + 1 == block.first + block.count;
                if (is_last && _target[isize(b)] != k_no_target)
                {
                    copy.code = op::branch;
                    operands.push_back(_target[isize(b)]);
                    copy.count = 1;
                    blocks[isize(b)].merges = merge_kind::none;
                }
                else if (copy.code == op::phi)
                {
                    copy.count = 0;
                    for (auto k = isize(0); k < held.size(); k += 2)
                        if (phi_edge_stays(block_ref(held[k + 1]), b))
                        {
                            operands.push_back(moved(held[k]));
                            operands.push_back(held[k + 1]);
                            copy.count += 2;
                        }
                }
                else
                    for (auto k = isize(0); k < held.size(); ++k)
                        operands.push_back(operand_role(copy.code, k) == operand::value ? moved(held[k]) : held[k]);
                copy.first = first;
                instructions.push_back(copy);
            }
            blocks[isize(b)].first = start;
            blocks[isize(b)].count = u32(instructions.size()) - start;
        }
        // an earlier round's dead block is in no order, and it must not point past what this one kept
        for (auto& block : blocks)
            if (block.count == 0)
                block.first = u32(instructions.size());
        _p.instructions = cc::move(instructions);
        _p.operands = cc::move(operands);
        // a block no walk reached stands at the end with nothing in it, which a writer's reachability skips
        _p.blocks = cc::move(blocks);
    }

    [[nodiscard]] bool phi_edge_stays(block_ref from, block_ref to) const
    {
        return _visited[isize(from)] && (_target[isize(from)] == k_no_target || _target[isize(from)] == u32(to));
    }

    /// A block no walk reached is dropped, unless SPIR-V's merge and continue targets must keep naming it.
    [[nodiscard]] bool is_kept(block_ref b) const { return _visited[isize(b)] || _keeps_structure; }

    program& _p;
    bool _keeps_structure = false;
    cc::vector<block_ref> _order;
    cc::vector<value_ref> _alias;
    cc::vector<bool> _reached;
    cc::vector<bool> _visited;
    /// Passed by the walk unreached: no edge out of it is taken.
    cc::vector<bool> _skipped;
    /// Parallel to the blocks: the one block a folded branch goes to.
    cc::vector<u32> _target;
    cc::vector<u32> _scratch;
    bool _changed = false;
};
} // namespace

void sgl::ir::simplify(program& p, control_flow flow)
{
    simplifier(p, flow).run();
}

void sgl::ir::simplify(program& p)
{
    simplify(p, control_flow::unstructured);
}
