#include "simplify.hh"

#include <clean-core/math/bit.hh>
#include <shaped-graphics-language/ir/ir.hh>

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
    explicit simplifier(program& p) : _p(p) {}

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
                continue;
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
                auto const kind = fold_kind_of(i.code);
                if (kind != fold_kind::none)
                    fold_binary(index, i, kind, resolved(value_ref(operands[0])), resolved(value_ref(operands[1])));
            }

            auto const& last = _p.instructions[block.first + block.count - 1];
            auto const operands = _p.operands_of(last);
            if (last.code == op::branch_conditional)
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
                continue;
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
            if (!_visited[isize(b)])
            {
                blocks[isize(b)].first = u32(instructions.size());
                blocks[isize(b)].count = 0;
                continue;
            }
            auto const& block = _p.blocks[isize(b)];
            auto const start = u32(instructions.size());
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
        _p.instructions = cc::move(instructions);
        _p.operands = cc::move(operands);
        // a block no walk reached stands at the end with nothing in it, which a writer's reachability skips
        _p.blocks = cc::move(blocks);
    }

    [[nodiscard]] bool phi_edge_stays(block_ref from, block_ref to) const
    {
        return _visited[isize(from)] && (_target[isize(from)] == k_no_target || _target[isize(from)] == u32(to));
    }

    program& _p;
    cc::vector<block_ref> _order;
    cc::vector<value_ref> _alias;
    cc::vector<bool> _reached;
    cc::vector<bool> _visited;
    /// Parallel to the blocks: the one block a folded branch goes to.
    cc::vector<u32> _target;
    cc::vector<u32> _scratch;
    bool _changed = false;
};
} // namespace

void sgl::ir::simplify(program& p)
{
    simplifier(p).run();
}
