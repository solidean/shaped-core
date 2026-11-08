#include "passes.hh"

#include <clean-core/container/vector.hh>
#include <shaped-graphics-language/ir/ir.hh>
#include <shaped-graphics-language/ir/verify.hh>

using namespace sgl;
using namespace sgl::ir;

namespace
{
constexpr u32 k_nothing = 0xffffffffu;
/// The most scalar operations one side of a branch may hold and still run whichever way the branch would have gone.
constexpr u32 k_most_speculated = 4;
constexpr int k_most_flatten_rounds = 16;

class flattener
{
public:
    flattener(program& p, control_flow flow) : _p(p), _structured(flow == control_flow::structured) {}

    void run()
    {
        number_blocks();
        link_lists();
        for (auto round = 0; round < k_most_flatten_rounds; ++round)
        {
            start_round();
            auto changed = false;
            for (auto b = isize(0); b < _block_count; ++b)
            {
                if (_is_dead[b])
                    continue;
                auto const step = visit(b);
                changed = changed || step != outcome::nothing;
                // what a joined block reached may have just lost a way in
                if (step == outcome::again)
                    --b;
            }
            if (!changed)
                break;
        }
        rebuild();
    }

private:
    enum class outcome : u8
    {
        nothing,
        changed,
        again,
    };

    /// Blocks numbered by where they open from 0, the entry first; what no walk reaches is gone from here on.
    void number_blocks()
    {
        auto const order = blocks_in_order(_p);
        // a structured program keeps what no walk reaches, which may be a construct's merge block, but for a block a
        // pass already dropped
        auto is_reached = cc::vector<bool>::create_filled(_p.blocks.size(), false);
        if (_structured)
            for (auto b = isize(0); b < _p.blocks.size(); ++b)
                is_reached[b] = _p.blocks[b].count > 0;
        auto stack = cc::vector<u32>();
        auto scratch = cc::vector<u32>();
        is_reached[isize(order[0])] = true;
        stack.push_back(u32(order[0]));
        while (!stack.empty())
        {
            auto const& block = _p.blocks[stack.back()];
            stack.pop_back();
            for (auto const to : successors_of(_p, _p.instructions[block.first + block.count - 1], scratch))
                if (!is_reached[to])
                {
                    is_reached[to] = true;
                    stack.push_back(to);
                }
        }
        auto place = cc::vector<u32>::create_filled(_p.blocks.size(), k_nothing);
        for (auto const b : order)
            if (is_reached[isize(b)])
            {
                place[isize(b)] = u32(_first.size());
                _first.push_back(_p.blocks[isize(b)].first);
                _count.push_back(_p.blocks[isize(b)].count);
            }
        _block_count = _first.size();
        _merges = cc::vector<merge_kind>::create_filled(_block_count, merge_kind::none);
        _merge = cc::vector<u32>::create_filled(_block_count, k_nothing);
        _continue_to = cc::vector<u32>::create_filled(_block_count, k_nothing);
        _named = cc::vector<u32>::create_filled(_block_count, 0u);
        if (_structured)
            for (auto const b : order)
            {
                auto const& block = _p.blocks[isize(b)];
                auto const at = place[isize(b)];
                if (at == k_nothing)
                    continue;
                _merges[at] = block.merges;
                if (block.merges == merge_kind::none)
                    continue;
                _merge[at] = place[isize(block.merge)];
                ++_named[_merge[at]];
                if (block.merges == merge_kind::loop)
                {
                    _continue_to[at] = place[isize(block.continue_to)];
                    ++_named[_continue_to[at]];
                }
            }
        for (auto b = isize(0); b < _block_count; ++b)
            for (auto i = _first[b]; i < _first[b] + _count[b]; ++i)
            {
                auto& inst = _p.instructions[i];
                auto* const operands = _p.operands.data() + inst.first;
                if (inst.code == op::phi)
                {
                    // an edge from what no walk reaches is no edge
                    auto kept = u32(0);
                    for (auto k = u32(0); k < inst.count; k += 2)
                        if (place[operands[k + 1]] != k_nothing)
                        {
                            operands[kept] = operands[k];
                            operands[kept + 1] = place[operands[k + 1]];
                            kept += 2;
                        }
                    inst.count = kept;
                    continue;
                }
                for (auto k = isize(0); k < isize(inst.count); ++k)
                    if (operand_role(inst.code, k) == operand::block)
                        operands[k] = place[operands[k]];
            }
    }

    /// Each block as a list, so a run moves between blocks whole.
    void link_lists()
    {
        _next = cc::vector<u32>::create_filled(_p.instructions.size(), k_nothing);
        _previous = cc::vector<u32>::create_filled(_p.instructions.size(), k_nothing);
        _replaced = cc::vector<u32>::create_filled(_p.instructions.size(), k_nothing);
        _head = cc::vector<u32>::create_filled(_block_count, k_nothing);
        _tail = cc::vector<u32>::create_filled(_block_count, k_nothing);
        _is_dead = cc::vector<bool>::create_filled(_block_count, false);
        _is_touched = cc::vector<bool>::create_filled(_block_count, false);
        _incoming = cc::vector<u32>::create_filled(_block_count, 0u);
        _first_way = cc::vector<u32>::create_filled(_block_count + 1, 0u);
        for (auto b = isize(0); b < _block_count; ++b)
        {
            _head[b] = _first[b];
            _tail[b] = _first[b] + _count[b] - 1;
            for (auto i = _first[b]; i < _first[b] + _count[b]; ++i)
            {
                _next[i] = i + 1 < _first[b] + _count[b] ? i + 1 : k_nothing;
                _previous[i] = i > _first[b] ? i - 1 : k_nothing;
            }
        }
    }

    [[nodiscard]] u32 current(u32 raw) const
    {
        auto v = raw;
        while (is_operation(value_ref(v)) && _replaced[index_of(value_ref(v))] != k_nothing)
            v = _replaced[index_of(value_ref(v))];
        return v;
    }

    template <class Visit>
    void each_successor(isize b, Visit&& visit_block)
    {
        auto const& last = _p.instructions[_tail[b]];
        auto* const operands = _p.operands.data() + last.first;
        for (auto k = isize(0); k < isize(last.count); ++k)
            if (operand_role(last.code, k) == operand::block)
                visit_block(operands[k]);
    }

    [[nodiscard]] bool is_phi(u32 i) const { return i != k_nothing && _p.instructions[i].code == op::phi; }

    [[nodiscard]] u32 ends_in_branch_to(isize b) const
    {
        auto const& last = _p.instructions[_tail[b]];
        return last.code == op::branch ? _p.operands[last.first] : k_nothing;
    }

    /// Lanes of a value, roughly what a target that works on scalars writes for one operation that gives it.
    [[nodiscard]] u32 lanes_of(type_ref t) const
    {
        auto const& info = _p.at(t);
        if (info.kind == type_kind::vector || info.kind == type_kind::array)
            return info.count * lanes_of(info.element);
        if (info.kind == type_kind::matrix)
            return info.count * lanes_of(info.element);
        return 1;
    }

    /// What `i` costs when it runs on a path that did not ask for it.
    [[nodiscard]] u32 cost_of(instruction const& i) const
    {
        switch (i.code)
        {
        case op::construct:
        case op::extract:
        case op::insert:
        case op::shuffle:
        case op::copy_logical:
            return 0;
        case op::math:
            switch (math(i.sub))
            {
            case math::fsign:
            case math::ssign:
            case math::fmix:
            case math::fclamp:
            case math::uclamp:
            case math::sclamp:
            case math::sabs:
            case math::pow:
            case math::exp:
            case math::log:
                return 2 * lanes_of(i.type);
            case math::atan2:
            case math::smooth_step:
            case math::length:
            case math::distance:
            case math::cross:
            case math::reflect:
            case math::refract:
                return k_most_speculated + 1;
            case math::normalize:
                // its dot is so often at hand already, from the length a caller tested
                return lanes_of(i.type) + 1;
            default:
                return lanes_of(i.type);
            }
        default:
            return lanes_of(i.type);
        }
    }

    /// What SPIR-V's logical addressing lets a `select` choose: data, never a pointer or a handle.
    [[nodiscard]] bool is_selectable(type_ref t) const
    {
        auto const& info = _p.at(t);
        switch (info.kind)
        {
        case type_kind::bool_:
        case type_kind::int_:
        case type_kind::float_:
            return true;
        case type_kind::vector:
        case type_kind::matrix:
        case type_kind::array:
            return is_selectable(info.element);
        case type_kind::structure:
            for (auto m = info.first_member; m < info.first_member + info.member_count; ++m)
                if (!is_selectable(_p.members[m].type))
                    return false;
            return true;
        default:
            return false;
        }
    }

    [[nodiscard]] bool body_is_small(isize b) const
    {
        auto cost = u32(0);
        for (auto i = _head[b]; i != _tail[b]; i = _next[i])
        {
            auto const& inst = _p.instructions[i];
            if (inst.code == op::phi || !may_speculate(inst.code))
                return false;
            cost += cost_of(inst);
            if (cost > k_most_speculated)
                return false;
        }
        return true;
    }

    /// A new `select` at the end of `b`, ahead of its terminator, standing for the phi at `phi_at`.
    [[nodiscard]] u32 select_at_end(isize b, u32 phi_at, u32 condition, u32 yes, u32 no)
    {
        auto const type = _p.instructions[phi_at].type;
        auto const guarantee = _p.instructions[phi_at].guarantee;
        auto const statement = _p.instructions[phi_at].statement;
        auto const index = u32(_p.instructions.size());
        auto const first = u32(_p.operands.size());
        _p.operands.push_back(condition);
        _p.operands.push_back(yes);
        _p.operands.push_back(no);
        _p.instructions.push_back(
            {.code = op::select, .guarantee = guarantee, .type = type, .first = first, .count = 3, .statement = statement});
        _next.push_back(_tail[b]);
        _previous.push_back(_previous[_tail[b]]);
        if (_previous[_tail[b]] != k_nothing)
            _next[_previous[_tail[b]]] = index;
        else
            _head[b] = index;
        _previous[_tail[b]] = index;
        _replaced.push_back(k_nothing);
        return index;
    }

    /// The body of `from`, all but its terminator, ahead of the terminator of `into`.
    void splice_body(isize from, isize into)
    {
        if (_head[from] == _tail[from])
            return;
        auto const last = _previous[_tail[from]];
        auto const before = _previous[_tail[into]];
        if (before != k_nothing)
            _next[before] = _head[from];
        else
            _head[into] = _head[from];
        _previous[_head[from]] = before;
        _next[last] = _tail[into];
        _previous[_tail[into]] = last;
    }

    /// Phis of what `of_block` reaches, which named `from` for this block's edge, name `to`.
    void rename_edges(isize of_block, u32 from, u32 to)
    {
        each_successor(of_block,
                       [&](u32 successor)
                       {
                           for (auto i = _head[successor]; is_phi(i); i = _next[i])
                               for (auto k = u32(1); k < _p.instructions[i].count; k += 2)
                                   if (_p.operands[_p.instructions[i].first + k] == from)
                                       _p.operands[_p.instructions[i].first + k] = to;
                       });
    }

    void start_round()
    {
        for (auto& count : _incoming)
            count = 0;
        for (auto& touched : _is_touched)
            touched = false;
        for (auto& first : _first_way)
            first = 0;
        for (auto b = isize(0); b < _block_count; ++b)
            if (!_is_dead[b])
                each_successor(b,
                               [&](u32 to)
                               {
                                   ++_incoming[to];
                                   ++_first_way[to + 1];
                               });
        for (auto b = isize(0); b < _block_count; ++b)
            _first_way[b + 1] += _first_way[b];
        _ways_in.resize_to_filled(isize(_first_way[_block_count]), 0u);
        auto cursor = cc::vector<u32>::create_copy_of(cc::span<u32 const>(_first_way));
        for (auto b = isize(0); b < _block_count; ++b)
            if (!_is_dead[b])
                each_successor(b, [&](u32 to) { _ways_in[cursor[to]++] = u32(b); });
        _uses.resize_to_filled(_p.instructions.size(), 0u);
        for (auto& count : _uses)
            count = 0;
        for (auto b = isize(0); b < _block_count; ++b)
            if (!_is_dead[b])
                for (auto i = _head[b]; i != k_nothing; i = _next[i])
                {
                    auto const& inst = _p.instructions[i];
                    auto const operands = _p.operands_of(inst);
                    for (auto k = isize(0); k < operands.size(); ++k)
                        if (operand_role(inst.code, k) == operand::value)
                        {
                            auto const v = current(operands[k]);
                            if (is_operation(value_ref(v)))
                                ++_uses[index_of(value_ref(v))];
                        }
                }
    }

    /// Sends what reached `b`, a block of phis and a branch alone, straight to where `b` goes.
    [[nodiscard]] bool thread_through(isize b, u32 merge)
    {
        if (_is_touched[b] || _is_touched[merge] || i64(merge) == b || b == 0)
            return false;
        auto const preds = cc::span<u32 const>(_ways_in.data() + _first_way[b], isize(_first_way[b + 1] - _first_way[b]));
        if (preds.empty())
            return false;
        for (auto const p : preds)
        {
            if (_is_touched[p])
                return false;
            auto reaches_merge = false;
            each_successor(isize(p), [&](u32 to) { reaches_merge = reaches_merge || to == merge; });
            if (reaches_merge)
                return false;
        }
        // a phi of `b` may be read by the phis of `merge` on this edge alone
        for (auto i = _head[b]; i != _tail[b]; i = _next[i])
        {
            auto seen = u32(0);
            for (auto m = _head[merge]; is_phi(m); m = _next[m])
                for (auto k = u32(0); k < _p.instructions[m].count; k += 2)
                    if (_p.operands[_p.instructions[m].first + k + 1] == u32(b)
                        && current(_p.operands[_p.instructions[m].first + k]) == i)
                        ++seen;
            if (seen != _uses[i])
                return false;
        }

        for (auto m = _head[merge]; is_phi(m); m = _next[m])
        {
            auto const first = u32(_p.operands.size());
            auto const held_first = _p.instructions[m].first;
            auto const held_count = _p.instructions[m].count;
            auto value_from_b = k_nothing;
            for (auto k = u32(0); k < held_count; k += 2)
            {
                if (_p.operands[held_first + k + 1] == u32(b))
                {
                    value_from_b = current(_p.operands[held_first + k]);
                    continue;
                }
                auto const value = _p.operands[held_first + k];
                auto const from = _p.operands[held_first + k + 1];
                _p.operands.push_back(value);
                _p.operands.push_back(from);
            }
            for (auto const p : preds)
            {
                auto value = value_from_b;
                for (auto i = _head[b]; i != _tail[b]; i = _next[i])
                    if (i == value_from_b)
                    {
                        auto const& held = _p.instructions[i];
                        for (auto k = u32(0); k < held.count; k += 2)
                            if (_p.operands[held.first + k + 1] == p)
                                value = _p.operands[held.first + k];
                    }
                _p.operands.push_back(value);
                _p.operands.push_back(p);
            }
            _p.instructions[m].first = first;
            _p.instructions[m].count = u32(_p.operands.size()) - first;
        }
        for (auto const p : preds)
        {
            auto const& last = _p.instructions[_tail[p]];
            auto* const operands = _p.operands.data() + last.first;
            for (auto k = isize(0); k < isize(last.count); ++k)
                if (operand_role(last.code, k) == operand::block && operands[k] == u32(b))
                    operands[k] = merge;
            _is_touched[p] = true;
        }
        _incoming[merge] += u32(preds.size()) - 1;
        _is_dead[b] = true;
        _is_touched[b] = true;
        _is_touched[merge] = true;
        return true;
    }

    /// A block with this one for its only way in is this block.
    [[nodiscard]] outcome join(isize b, u32 to)
    {
        // the construct `to` heads becomes this block's, and a block a construct names stays a block of its own
        if (_structured)
        {
            if (_named[to] != 0 || _merges[b] != merge_kind::none)
                return outcome::nothing;
            _merges[b] = _merges[to];
            _merge[b] = _merge[to];
            _continue_to[b] = _continue_to[to];
            _merges[to] = merge_kind::none;
        }
        while (is_phi(_head[to]))
        {
            auto const phi = _head[to];
            _replaced[phi] = _p.operands[_p.instructions[phi].first];
            _head[to] = _next[phi];
            _previous[_head[to]] = k_nothing;
        }
        // all of `to`, its terminator too, in place of this block's terminator
        auto const before = _previous[_tail[b]];
        if (before != k_nothing)
            _next[before] = _head[to];
        else
            _head[b] = _head[to];
        _previous[_head[to]] = before;
        _tail[b] = _tail[to];
        _is_dead[to] = true;
        rename_edges(b, to, u32(b));
        // what this block now reaches has a way in the lists of this round do not name
        each_successor(b, [&](u32 successor) { _is_touched[successor] = true; });
        return outcome::again;
    }

    [[nodiscard]] outcome to_selects(isize b, u32 yes, u32 no, u32 condition)
    {
        auto const can_move = [&](u32 side, u32 past)
        { return _incoming[side] == 1 && ends_in_branch_to(isize(side)) == past && body_is_small(isize(side)); };
        auto merge = k_nothing;
        auto is_yes = false;
        auto is_no = false;
        if (can_move(yes, no))
        {
            merge = no;
            is_yes = true;
        }
        else if (can_move(no, yes))
        {
            merge = yes;
            is_no = true;
        }
        else if (ends_in_branch_to(isize(yes)) != k_nothing
                 && ends_in_branch_to(isize(yes)) == ends_in_branch_to(isize(no))
                 && can_move(yes, ends_in_branch_to(isize(yes))) && can_move(no, ends_in_branch_to(isize(no))))
        {
            merge = ends_in_branch_to(isize(yes));
            is_yes = true;
            is_no = true;
        }
        if (merge == k_nothing || i64(merge) == b)
            return outcome::nothing;
        if (_structured)
        {
            // a selection of its own, whose arms head nothing and are no construct's target
            if (_merges[b] != merge_kind::selection || _merge[b] != merge)
                return outcome::nothing;
            if ((is_yes && (_named[yes] != 0 || _merges[yes] != merge_kind::none))
                || (is_no && (_named[no] != 0 || _merges[no] != merge_kind::none)))
                return outcome::nothing;
            for (auto i = _head[merge]; is_phi(i); i = _next[i])
                if (!is_selectable(_p.instructions[i].type))
                    return outcome::nothing;
            --_named[merge];
            _merges[b] = merge_kind::none;
            _merge[b] = k_nothing;
        }

        if (is_yes)
            splice_body(isize(yes), b);
        if (is_no)
            splice_body(isize(no), b);
        // each phi of the merge: what either way gave is now one value, from this block
        auto const from_yes = is_yes ? yes : u32(b);
        auto const from_no = is_no ? no : u32(b);
        for (auto i = _head[merge], next = k_nothing; is_phi(i); i = next)
        {
            next = _next[i];
            auto const first = _p.instructions[i].first;
            auto const count = _p.instructions[i].count;
            auto yes_value = k_nothing;
            auto no_value = k_nothing;
            auto kept = u32(0);
            for (auto k = u32(0); k < count; k += 2)
            {
                auto const value = _p.operands[first + k];
                auto const block = _p.operands[first + k + 1];
                if (block == from_yes)
                    yes_value = value;
                else if (block == from_no)
                    no_value = value;
                else
                {
                    _p.operands[first + kept] = value;
                    _p.operands[first + kept + 1] = block;
                    kept += 2;
                }
            }
            // what is undefined may be anything, so it may be the other way's value, which DXIL requires of a select
            auto const is_undefined
                = [&](u32 v) { return is_constant(value_ref(v)) && _p.constants[index_of(value_ref(v))].is_undefined; };
            auto const chosen = is_undefined(no_value)  ? yes_value
                              : is_undefined(yes_value) ? no_value
                                                        : select_at_end(b, i, condition, yes_value, no_value);
            _p.operands[first + kept] = chosen;
            _p.operands[first + kept + 1] = u32(b);
            _p.instructions[i].count = kept + 2;
            // a phi of this block's way in alone is its value, where a merge a construct names stays a block of its own
            if (kept == 0)
            {
                _replaced[i] = chosen;
                if (_previous[i] != k_nothing)
                    _next[_previous[i]] = next;
                else
                    _head[merge] = next;
                _previous[next] = _previous[i];
            }
        }
        if (is_yes)
            _is_dead[yes] = true;
        if (is_no)
            _is_dead[no] = true;
        // the branch is one way now
        auto& terminator = _p.instructions[_tail[b]];
        terminator.code = op::branch;
        terminator.count = 1;
        _p.operands[terminator.first] = merge;
        // the two ways into the merge are one now
        --_incoming[merge];
        _is_touched[merge] = true;
        return outcome::again;
    }

    [[nodiscard]] outcome visit(isize b)
    {
        auto const last = _p.instructions[_tail[b]];
        if (last.code == op::branch)
        {
            auto const to = _p.operands[last.first];
            // nothing but phis on the way to a block that has them
            auto only_phis = true;
            for (auto i = _head[b]; i != _tail[b] && only_phis; i = _next[i])
                only_phis = _p.instructions[i].code == op::phi;
            if (!_structured && only_phis && thread_through(b, to))
                return outcome::changed;
            if (i64(to) == b || _incoming[to] != 1 || to == 0)
                return outcome::nothing;
            return join(b, to);
        }
        if (last.code != op::branch_conditional)
            return outcome::nothing;
        return to_selects(b, _p.operands[last.first + 1], _p.operands[last.first + 2], _p.operands[last.first]);
    }

    /// The lists as tables again, in reverse postorder: joined blocks may stand ahead of what they now dominate.
    void rebuild()
    {
        struct walk_entry
        {
            u32 block = 0;
            u32 first = 0;
            u32 count = 0;
        };
        auto successors = cc::vector<u32>();
        auto walk = cc::vector<walk_entry>();
        auto postorder = cc::vector<u32>();
        auto is_seen = cc::vector<bool>::create_filled(_block_count, false);
        auto const enter = [&](u32 b)
        {
            is_seen[b] = true;
            auto const first = u32(successors.size());
            each_successor(isize(b), [&](u32 to) { successors.push_back(to); });
            walk.push_back({.block = b, .first = first, .count = u32(successors.size()) - first});
        };
        enter(0);
        while (!walk.empty())
        {
            auto& top = walk.back();
            if (top.count == 0)
            {
                postorder.push_back(top.block);
                walk.pop_back();
                continue;
            }
            // the last successor first, so the first one stands right after this block
            --top.count;
            auto const to = successors[top.first + top.count];
            if (!is_seen[to])
                enter(to);
        }
        auto order = cc::vector<u32>::create_with_capacity(postorder.size());
        if (_structured)
        {
            // the order blocks opened in, what no walk reaches too: a header still opens ahead of its construct
            for (auto b = isize(0); b < _block_count; ++b)
                if (!_is_dead[b])
                    order.push_back(u32(b));
        }
        else
            for (auto i = postorder.size(); i > 0; --i)
                order.push_back(postorder[i - 1]);

        auto block_place = cc::vector<u32>::create_filled(_block_count, k_nothing);
        for (auto i = isize(0); i < order.size(); ++i)
            block_place[order[i]] = u32(i);
        auto place = cc::vector<u32>::create_filled(_p.instructions.size(), k_nothing);
        auto next_place = u32(0);
        for (auto const b : order)
            for (auto i = _head[b]; i != k_nothing; i = _next[i])
                place[i] = next_place++;

        auto kept = cc::vector<instruction>::create_with_capacity(isize(next_place));
        auto operands = cc::vector<u32>::create_with_capacity(_p.operands.size());
        auto blocks = cc::vector<ir::block>::create_with_capacity(order.size());
        for (auto const b : order)
        {
            auto const start = u32(kept.size());
            for (auto i = _head[b]; i != k_nothing; i = _next[i])
            {
                auto copy = _p.instructions[i];
                auto const held = _p.operands_of(copy);
                auto const first = u32(operands.size());
                for (auto k = isize(0); k < held.size(); ++k)
                {
                    auto const role = operand_role(copy.code, k);
                    if (role == operand::block)
                        operands.push_back(block_place[held[k]]);
                    else if (role == operand::literal)
                        operands.push_back(held[k]);
                    else
                    {
                        auto const v = current(held[k]);
                        operands.push_back(is_operation(value_ref(v)) ? place[index_of(value_ref(v))] : v);
                    }
                }
                copy.first = first;
                kept.push_back(copy);
            }
            auto rebuilt = ir::block{.first = start, .count = u32(kept.size()) - start};
            if (_merges[b] != merge_kind::none)
            {
                rebuilt.merges = _merges[b];
                rebuilt.merge = block_ref(block_place[_merge[b]]);
                if (_merges[b] == merge_kind::loop)
                    rebuilt.continue_to = block_ref(block_place[_continue_to[b]]);
            }
            blocks.push_back(rebuilt);
        }
        _p.instructions = cc::move(kept);
        _p.operands = cc::move(operands);
        _p.blocks = cc::move(blocks);
    }

    program& _p;
    bool _structured = false;
    isize _block_count = 0;
    /// Parallel to the blocks, where structured: each one's construct, and how many constructs name it.
    cc::vector<merge_kind> _merges;
    cc::vector<u32> _merge;
    cc::vector<u32> _continue_to;
    cc::vector<u32> _named;
    cc::vector<u32> _first;
    cc::vector<u32> _count;
    cc::vector<u32> _next;
    cc::vector<u32> _previous;
    cc::vector<u32> _head;
    cc::vector<u32> _tail;
    cc::vector<bool> _is_dead;
    /// What a value became, where a join took its phi away.
    cc::vector<u32> _replaced;
    cc::vector<u32> _incoming;
    cc::vector<u32> _uses;
    cc::vector<bool> _is_touched;
    cc::vector<u32> _ways_in;
    cc::vector<u32> _first_way;
};
} // namespace

void sgl::ir::flatten_branches(program& p, control_flow flow)
{
    flattener(p, flow).run();
}

void sgl::ir::flatten_branches(program& p)
{
    flatten_branches(p, control_flow::unstructured);
}
