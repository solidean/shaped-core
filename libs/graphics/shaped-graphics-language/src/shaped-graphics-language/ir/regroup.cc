#include <shaped-graphics-language/ir/ir.hh>
#include <shaped-graphics-language/ir/passes.hh>

using namespace sgl;
using namespace sgl::ir;

// SSA construction promotes a local scalar by scalar, so a vector crosses a merge as one phi per lane.
// That is DXIL's own shape, and a SPIR-V module would rebuild the vector after every merge and take it apart before.
// A group here is the phis a `construct` reads lane by lane, and each way in must hand over one vector whole.
// A way that hands over the lanes of another merge's phis is that merge's group first, so nested merges regroup as one.

namespace
{
constexpr u32 k_nothing = 0xffffffffu;

class regrouper
{
public:
    explicit regrouper(program& p) : _p(p) {}

    void run()
    {
        auto const count = _p.instructions.size();
        _replaced = cc::vector<u32>::create_filled(count, k_nothing);
        _block_of = cc::vector<u32>::create_filled(count, k_nothing);
        _in_progress = cc::vector<bool>::create_filled(count, false);
        _heads = cc::vector<cc::vector<u32>>::create_defaulted(_p.blocks.size());
        _tails = cc::vector<cc::vector<u32>>::create_defaulted(_p.blocks.size());
        _lanes = cc::vector<cc::vector<u32>>::create_defaulted(_p.blocks.size());
        for (auto b = isize(0); b < _p.blocks.size(); ++b)
            for (auto i = _p.blocks[b].first; i < _p.blocks[b].first + _p.blocks[b].count; ++i)
                _block_of[i] = u32(b);
        auto any = false;
        for (auto i = isize(0); i < count; ++i)
            if (_p.instructions[i].code == op::construct)
                any = regroup(u32(i)) || any;
        if (any)
            rebuild();
    }

private:
    /// What way in `edge` of a group hands over as one vector: the phi itself, a vector of the right type, or its lanes,
    /// which a `construct` ahead of that way's branch builds.
    struct handed
    {
        bool is_self = false;
        bool is_built = false;
        u32 value = k_nothing;
    };

    [[nodiscard]] handed whole_of(cc::span<u32 const> phis, isize edge, type_ref vector) const
    {
        auto self = true;
        auto source = k_nothing;
        auto from_one = true;
        for (auto k = isize(0); k < phis.size(); ++k)
        {
            auto const lane = current(_p.operands_of(_p.instructions[phis[k]])[2 * edge]);
            self = self && lane == phis[k];
            if (!is_operation(value_ref(lane)) || _p.instructions[lane].code != op::extract)
            {
                from_one = false;
                continue;
            }
            auto const read = _p.operands_of(_p.instructions[lane]);
            auto const whole = read[0];
            auto const is_vector = is_operation(value_ref(whole)) ? _p.instructions[whole].type == vector
                                 : is_constant(value_ref(whole)) ? _p.constants[index_of(value_ref(whole))].type == vector
                                                                 : false;
            if (read.size() != 2 || read[1] != u32(k) || !is_vector || (source != k_nothing && source != whole))
                from_one = false;
            source = whole;
        }
        if (self)
            return {.is_self = true};
        if (from_one && source != k_nothing)
            return {.value = source};
        return {.is_built = true};
    }

    [[nodiscard]] bool regroup(u32 built)
    {
        auto const c = _p.instructions[built];
        return regroup_phis(cc::vector<u32>::create_copy_of(_p.operands_of(c)), c.type);
    }

    /// Makes `phis` one phi of `vector_type`, where each is a phi of one block, a lane in order.
    [[nodiscard]] bool regroup_phis(cc::vector<u32> const& phis, type_ref vector_type)
    {
        auto const whole = _p.at(vector_type);
        if (whole.kind != type_kind::vector || isize(whole.count) != phis.size() || phis.size() < 2)
            return false;
        auto block = k_nothing;
        for (auto k = isize(0); k < phis.size(); ++k)
        {
            auto const v = value_ref(phis[k]);
            if (!is_operation(v) || phis[k] >= _replaced.size() || _p.instructions[phis[k]].code != op::phi
                || _replaced[phis[k]] != k_nothing || _in_progress[phis[k]])
                return false;
            if (_p.instructions[phis[k]].type != whole.element)
                return false;
            for (auto j = isize(0); j < k; ++j)
                if (phis[j] == phis[k])
                    return false;
            if (block != k_nothing && _block_of[phis[k]] != block)
                return false;
            block = _block_of[phis[k]];
        }
        // the ways in stand in one order in every lane's phi, since SSA made the lanes' phis together
        auto const first = cc::vector<u32>::create_copy_of(_p.operands_of(_p.instructions[phis[0]]));
        for (auto k = isize(1); k < phis.size(); ++k)
        {
            auto const other = _p.operands_of(_p.instructions[phis[k]]);
            if (other.size() != first.size())
                return false;
            for (auto e = isize(1); e < first.size(); e += 2)
                if (other[e] != first[e])
                    return false;
        }
        // a way whose lanes are another merge's phis hands that merge's vector over, once those are a group too
        for (auto const k : phis)
            _in_progress[k] = true;
        for (auto e = isize(0); e < first.size() / 2; ++e)
        {
            auto inner = cc::vector<u32>();
            for (auto const k : phis)
                inner.push_back(current(_p.operands_of(_p.instructions[k])[2 * e]));
            auto is_self = true;
            for (auto k = isize(0); k < phis.size(); ++k)
                is_self = is_self && inner[k] == phis[k];
            if (!is_self)
                (void)regroup_phis(inner, vector_type);
        }
        for (auto const k : phis)
            _in_progress[k] = false;

        // a way that hands a vector over pays for building it on the others; lanes alone everywhere are better left lanes
        auto ways = cc::vector<handed>();
        auto is_whole_once = false;
        for (auto e = isize(0); e < first.size() / 2; ++e)
        {
            ways.push_back(whole_of(phis, e, vector_type));
            is_whole_once = is_whole_once || !ways.back().is_built;
        }
        if (!is_whole_once)
            return false;

        auto const lead = _p.instructions[phis[0]];
        auto const phi_at = u32(_p.instructions.size());
        // made first, so the index of a way's construct below is known when the phi's operands are written
        auto const operands_first = u32(_p.operands.size());
        _p.instructions.push_back({.code = op::phi,
                                   .guarantee = lead.guarantee,
                                   .type = vector_type,
                                   .first = operands_first,
                                   .count = u32(2 * ways.size()),
                                   .statement = lead.statement});
        _p.operands.resize_to_filled(_p.operands.size() + 2 * ways.size(), 0u);
        for (auto e = isize(0); e < ways.size(); ++e)
        {
            auto const from = first[2 * e + 1];
            auto value = ways[e].value;
            if (ways[e].is_self)
                value = phi_at;
            else if (ways[e].is_built)
            {
                value = u32(_p.instructions.size());
                auto const parts = u32(_p.operands.size());
                for (auto k = isize(0); k < phis.size(); ++k)
                {
                    auto const lane = _p.operands[_p.instructions[phis[k]].first + 2 * e];
                    _p.operands.push_back(lane);
                }
                _p.instructions.push_back({.code = op::construct,
                                           .guarantee = lead.guarantee,
                                           .type = vector_type,
                                           .first = parts,
                                           .count = u32(phis.size()),
                                           .statement = lead.statement});
                _tails[from].push_back(value);
            }
            _p.operands[operands_first + 2 * e] = value;
            _p.operands[operands_first + 2 * e + 1] = from;
        }
        _heads[block].push_back(phi_at);
        for (auto k = isize(0); k < phis.size(); ++k)
        {
            auto const lane = _p.instructions[phis[k]];
            auto const parts = u32(_p.operands.size());
            _p.operands.push_back(phi_at);
            _p.operands.push_back(u32(k));
            _replaced[phis[k]] = u32(_p.instructions.size());
            _lanes[block].push_back(u32(_p.instructions.size()));
            _p.instructions.push_back({.code = op::extract,
                                       .guarantee = lane.guarantee,
                                       .type = lane.type,
                                       .first = parts,
                                       .count = 2,
                                       .statement = lane.statement});
        }
        return true;
    }

    [[nodiscard]] u32 current(u32 v) const
    {
        while (is_operation(value_ref(v)) && v < _replaced.size() && _replaced[v] != k_nothing)
            v = _replaced[v];
        return v;
    }

    /// The tables again, each group's phi after its block's phis and its lanes after every phi.
    void rebuild()
    {
        auto order = cc::vector<u32>();
        auto const emit_block = [&](u32 b)
        {
            auto const& block = _p.blocks[b];
            auto i = block.first;
            auto const end = block.first + block.count;
            for (; i < end && _p.instructions[i].code == op::phi; ++i)
                order.push_back(i);
            order.push_back_range(_heads[b]);
            order.push_back_range(_lanes[b]);
            for (; i + 1 < end; ++i)
                order.push_back(i);
            order.push_back_range(_tails[b]);
            if (block.count > 0)
                order.push_back(end - 1);
        };
        auto const in_order = blocks_in_order(_p);
        auto first_of = cc::vector<u32>::create_filled(_p.blocks.size(), 0u);
        auto count_of = cc::vector<u32>::create_filled(_p.blocks.size(), 0u);
        for (auto const b : in_order)
        {
            first_of[isize(b)] = u32(order.size());
            emit_block(u32(b));
            count_of[isize(b)] = u32(order.size()) - first_of[isize(b)];
        }
        auto place = cc::vector<u32>::create_filled(_p.instructions.size(), k_nothing);
        for (auto n = isize(0); n < order.size(); ++n)
            place[order[n]] = u32(n);

        auto instructions = cc::vector<instruction>::create_with_capacity(order.size());
        auto operands = cc::vector<u32>::create_with_capacity(_p.operands.size());
        for (auto const at : order)
        {
            auto copy = _p.instructions[at];
            auto const held = _p.operands_of(copy);
            auto const start = u32(operands.size());
            for (auto k = isize(0); k < held.size(); ++k)
            {
                if (operand_role(copy.code, k) != operand::value)
                {
                    operands.push_back(held[k]);
                    continue;
                }
                auto const v = current(held[k]);
                operands.push_back(is_operation(value_ref(v)) ? place[v] : v);
            }
            copy.first = start;
            instructions.push_back(copy);
        }
        for (auto b = isize(0); b < _p.blocks.size(); ++b)
        {
            _p.blocks[b].first = first_of[b];
            _p.blocks[b].count = count_of[b];
        }
        _p.instructions = cc::move(instructions);
        _p.operands = cc::move(operands);
    }

    program& _p;
    /// Parallel to the instructions this pass found: the lane that replaces a phi now part of a group.
    cc::vector<u32> _replaced;
    cc::vector<u32> _block_of;
    /// The phis of a group being made, which a group it reaches through its ways may not take.
    cc::vector<bool> _in_progress;
    /// Per block: the groups' phis it opens with, and the constants' constructs its ways out need.
    cc::vector<cc::vector<u32>> _heads;
    cc::vector<cc::vector<u32>> _tails;
    /// Per block: each group's lanes, which follow every phi.
    cc::vector<cc::vector<u32>> _lanes;
};
} // namespace

void sgl::ir::regroup_lanes(program& p)
{
    regrouper(p).run();
}
