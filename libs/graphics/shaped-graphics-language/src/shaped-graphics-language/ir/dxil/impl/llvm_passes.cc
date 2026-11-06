#include "llvm.hh"

#include <shaped-graphics-language/ir/dominance.hh>

using namespace sgl;
using namespace sgl::ir::dxil::impl;

// What a module goes through once its function is written: the work DXC's optimizer does for the text route.
// Each works on scalars, which is why these stand here and not on `ir::program`, whose values are vectors.

bool module_builder::has_effect(inst_entry const& inst) const
{
    switch (inst.code)
    {
    case inst_code::store:
    case inst_code::atomic_rmw:
    case inst_code::atomic_exchange:
    case inst_code::branch:
    case inst_code::branch_if:
    case inst_code::switch_:
    case inst_code::return_void:
    case inst_code::unreachable:
        return true;
    case inst_code::call:
    {
        auto const& callee = _globals[ll_index(ll_value(_args[inst.first]))];
        auto const& list = _attribute_lists[callee.attributes];
        for (auto k = u32(0); k < list.count; ++k)
        {
            auto const kind = _attribute_items[list.first + k].kind;
            if (kind == k_attr_readnone || kind == k_attr_readonly)
                return false;
        }
        return true;
    }
    default:
        return false;
    }
}

template <class Visit>
void module_builder::for_each_value(inst_entry const& inst, Visit&& visit)
{
    auto* const args = _args.data() + inst.first;
    switch (inst.code)
    {
    case inst_code::cast:
    case inst_code::extract_value:
    case inst_code::load:
        visit(args[0]);
        break;
    case inst_code::alloca_:
    case inst_code::branch:
    case inst_code::return_void:
    case inst_code::unreachable:
        break;
    case inst_code::element_pointer:
        for (auto k = u32(1); k < inst.count; ++k)
            visit(args[k]);
        break;
    case inst_code::phi:
        for (auto k = u32(0); k < inst.count; k += 2)
            visit(args[k]);
        break;
    case inst_code::branch_if:
        visit(args[2]);
        break;
    case inst_code::switch_:
        visit(args[0]);
        break;
    default:
        for (auto k = u32(0); k < inst.count; ++k)
            visit(args[k]);
        break;
    }
}

bool module_builder::may_share(inst_entry const& inst) const
{
    switch (inst.code)
    {
    case inst_code::binary:
    case inst_code::cast:
    case inst_code::compare:
    case inst_code::select:
    case inst_code::extract_value:
    case inst_code::element_pointer:
        return true;
    case inst_code::call:
    {
        if (inst.flags != 0)
            return true;
        auto const& callee = _globals[ll_index(ll_value(_args[inst.first]))];
        auto const& list = _attribute_lists[callee.attributes];
        for (auto k = u32(0); k < list.count; ++k)
            if (_attribute_items[list.first + k].kind == k_attr_readnone)
                return true;
        return false;
    }
    default:
        return false;
    }
}

void module_builder::share_common_values()
{
    // the graph first: a block's predecessors, as one run per block
    auto edges_to = cc::vector<u32>::create_filled(_blocks.size() + 1, 0u);
    auto const each_successor = [&](block_entry const& b, auto&& visit)
    {
        auto const& last = _instructions[b.first + b.count - 1];
        auto const* const args = _args.data() + last.first;
        if (last.code == inst_code::branch)
            visit(args[0]);
        else if (last.code == inst_code::branch_if)
        {
            visit(args[0]);
            visit(args[1]);
        }
        else if (last.code == inst_code::switch_)
        {
            visit(args[1]);
            for (auto k = u32(2); k < last.count; k += 2)
                visit(args[k + 1]);
        }
    };
    for (auto const& b : _blocks)
        each_successor(b, [&](u32 to) { ++edges_to[to + 1]; });
    for (auto i = isize(0); i < _blocks.size(); ++i)
        edges_to[i + 1] += edges_to[i];
    auto preds = cc::vector<u32>::create_filled(isize(edges_to[_blocks.size()]), 0u);
    auto cursor = cc::vector<u32>::create_copy_of(cc::span<u32 const>(edges_to));
    for (auto i = isize(0); i < _blocks.size(); ++i)
        each_successor(_blocks[i], [&](u32 to) { preds[cursor[to]++] = u32(i); });
    auto const tree = ir::dominance(edges_to, preds);

    struct entry
    {
        u32 hash = 0;
        u32 result = 0;
        u32 block = 0;
    };
    auto size = isize(1024);
    while (size < _results.size() * 2)
        size *= 2;
    auto table = cc::vector<entry>::create_filled(size, {});
    auto const wrap = size - 1;
    auto replaced = cc::vector<u32>::create_filled(_results.size(), 0xffffffffu);
    auto const current = [&](u32 raw) -> u32
    {
        auto const v = ll_value(raw);
        if (ll_is_constant(v) || ll_is_global(v))
            return raw;
        return replaced[raw] != 0xffffffffu ? replaced[raw] : raw;
    };

    auto result = u32(0);
    for (auto b = isize(0); b < _blocks.size(); ++b)
    {
        auto const& block = _blocks[b];
        for (auto i = block.first; i < block.first + block.count; ++i)
        {
            auto& inst = _instructions[i];
            for_each_value(inst, [&](u32& raw) { raw = current(raw); });
            if (inst.type == _void)
                continue;
            auto const own = result++;
            if (!may_share(inst))
                continue;
            auto hash = u32(inst.code) * 0x9e3779b1u + u32(inst.type) * 0x85ebca6bu + (u32(inst.sub) << 8) + inst.flags;
            for (auto k = u32(0); k < inst.count; ++k)
                hash = (hash ^ _args[inst.first + k]) * 0x01000193u;
            hash |= 1u;
            auto at = isize(hash) & wrap;
            auto found = false;
            while (table[at].hash != 0 && !found)
            {
                if (table[at].hash == hash && tree.dominates(table[at].block, u32(b)))
                {
                    auto const& other = _instructions[_results[table[at].result]];
                    auto is_same = other.code == inst.code && other.sub == inst.sub && other.flags == inst.flags
                                && other.type == inst.type && other.count == inst.count;
                    for (auto k = u32(0); k < inst.count && is_same; ++k)
                        is_same = _args[other.first + k] == _args[inst.first + k];
                    if (is_same)
                    {
                        replaced[own] = table[at].result;
                        found = true;
                        break;
                    }
                }
                at = (at + 1) & wrap;
            }
            if (!found)
                table[at] = {.hash = hash, .result = own, .block = u32(b)};
        }
    }
    // a phi reaches back to values written after it
    for (auto const& inst : _instructions)
        if (inst.code == inst_code::phi)
            for (auto k = u32(0); k < inst.count; k += 2)
                _args[inst.first + k] = current(_args[inst.first + k]);
}

namespace
{
constexpr u32 k_nothing = 0xffffffffu;
/// The most operations one side of a branch may hold and still run whichever way the branch would have gone.
constexpr u32 k_most_speculated = 4;
constexpr int k_most_flatten_rounds = 16;
} // namespace

bool module_builder::is_speculable(inst_entry const& inst) const
{
    switch (inst.code)
    {
    case inst_code::cast:
    case inst_code::compare:
    case inst_code::select:
    case inst_code::extract_value:
    case inst_code::element_pointer:
        return true;
    case inst_code::binary:
    {
        // an integer division can trap, where a float's gives a value
        auto const op = binary_op(inst.sub);
        auto const is_division
            = op == binary_op::udiv || op == binary_op::sdiv || op == binary_op::urem || op == binary_op::srem;
        return !is_division || is_float(inst.type);
    }
    case inst_code::call:
        return may_share(inst) && inst.flags == 0;
    default:
        return false;
    }
}

void module_builder::flatten_branches()
{
    auto const block_count = _blocks.size();
    // each block as a list, so a run moves between blocks whole
    auto next = cc::vector<u32>::create_filled(_instructions.size(), k_nothing);
    auto previous = cc::vector<u32>::create_filled(_instructions.size(), k_nothing);
    auto head = cc::vector<u32>::create_filled(block_count, k_nothing);
    auto tail = cc::vector<u32>::create_filled(block_count, k_nothing);
    for (auto b = isize(0); b < block_count; ++b)
    {
        auto const& block = _blocks[b];
        head[b] = block.first;
        tail[b] = block.first + block.count - 1;
        for (auto i = block.first; i < block.first + block.count; ++i)
        {
            next[i] = i + 1 < block.first + block.count ? i + 1 : k_nothing;
            previous[i] = i > block.first ? i - 1 : k_nothing;
        }
    }
    auto is_dead = cc::vector<bool>::create_filled(block_count, false);
    auto replaced = cc::vector<u32>::create_filled(_results.size(), k_nothing);
    // the result each instruction gives, for one found by its place
    auto result_of = cc::vector<u32>::create_filled(_instructions.size(), k_nothing);
    for (auto r = isize(0); r < _results.size(); ++r)
        result_of[_results[r]] = u32(r);
    auto const current = [&](u32 raw) -> u32
    {
        auto v = raw;
        while (!ll_is_constant(ll_value(v)) && !ll_is_global(ll_value(v)) && replaced[v] != k_nothing)
            v = replaced[v];
        return v;
    };

    auto const each_successor = [&](isize b, auto&& visit)
    {
        auto const& last = _instructions[tail[b]];
        auto const first = last.first;
        if (last.code == inst_code::branch)
            visit(_args[first]);
        else if (last.code == inst_code::branch_if)
        {
            visit(_args[first]);
            visit(_args[first + 1]);
        }
        else if (last.code == inst_code::switch_)
        {
            visit(_args[first + 1]);
            for (auto k = u32(2); k < last.count; k += 2)
                visit(_args[first + k + 1]);
        }
    };
    /// A new `select` at the end of `b`, ahead of its terminator.
    auto const select_at_end = [&](isize b, u32 condition, u32 yes, u32 no)
    {
        auto const type = type_of(ll_value(current(yes)));
        auto const index = u32(_instructions.size());
        auto const first = u32(_args.size());
        _args.push_back(yes);
        _args.push_back(no);
        _args.push_back(condition);
        _instructions.push_back({.code = inst_code::select, .type = type, .first = first, .count = 3});
        result_of.push_back(u32(_results.size()));
        next.push_back(tail[b]);
        previous.push_back(previous[tail[b]]);
        if (previous[tail[b]] != k_nothing)
            next[previous[tail[b]]] = index;
        else
            head[b] = index;
        previous[tail[b]] = index;
        _results.push_back(index);
        replaced.push_back(k_nothing);
        return u32(_results.size() - 1);
    };
    /// The body of `from`, all but its terminator, ahead of the terminator of `into`.
    auto const splice_body = [&](isize from, isize into)
    {
        if (head[from] == tail[from])
            return;
        auto const last = previous[tail[from]];
        auto const before = previous[tail[into]];
        if (before != k_nothing)
            next[before] = head[from];
        else
            head[into] = head[from];
        previous[head[from]] = before;
        next[last] = tail[into];
        previous[tail[into]] = last;
    };
    auto const rename_edges = [&](isize of_block, u32 from, u32 to)
    {
        each_successor(of_block,
                       [&](u32 successor)
                       {
                           for (auto i = head[successor]; i != k_nothing && _instructions[i].code == inst_code::phi;
                                i = next[i])
                               for (auto k = u32(1); k < _instructions[i].count; k += 2)
                                   if (_args[_instructions[i].first + k] == from)
                                       _args[_instructions[i].first + k] = to;
                       });
    };
    auto const body_is_small = [&](isize b)
    {
        auto count = u32(0);
        for (auto i = head[b]; i != tail[b]; i = next[i])
        {
            auto const& inst = _instructions[i];
            if (inst.code == inst_code::phi || !is_speculable(inst) || ++count > k_most_speculated)
                return false;
        }
        return true;
    };
    auto const ends_in_branch_to = [&](isize b)
    {
        auto const& last = _instructions[tail[b]];
        return last.code == inst_code::branch ? _args[last.first] : k_nothing;
    };

    auto incoming = cc::vector<u32>::create_filled(block_count, 0u);
    auto uses = cc::vector<u32>();
    auto is_touched = cc::vector<bool>::create_filled(block_count, false);
    auto ways_in = cc::vector<u32>();
    auto first_way = cc::vector<u32>::create_filled(block_count + 1, 0u);

    /// Sends what reached `b`, a block of phis and a branch alone, straight to where `b` goes.
    auto const thread_through = [&](isize b, u32 merge) -> bool
    {
        if (is_touched[b] || is_touched[merge] || i64(merge) == b || b == 0)
            return false;
        auto const preds = cc::span<u32 const>(ways_in.data() + first_way[b], isize(first_way[b + 1] - first_way[b]));
        if (preds.empty())
            return false;
        for (auto const p : preds)
        {
            if (is_touched[p])
                return false;
            auto reaches_merge = false;
            each_successor(p, [&](u32 to) { reaches_merge = reaches_merge || to == merge; });
            if (reaches_merge)
                return false;
        }
        // a phi of `b` may be read by the phis of `merge` on this edge alone
        for (auto i = head[b]; i != tail[b]; i = next[i])
        {
            auto seen = u32(0);
            for (auto m = head[merge]; m != k_nothing && _instructions[m].code == inst_code::phi; m = next[m])
                for (auto k = u32(0); k < _instructions[m].count; k += 2)
                    if (_args[_instructions[m].first + k + 1] == u32(b)
                        && current(_args[_instructions[m].first + k]) == result_of[i])
                        ++seen;
            if (seen != uses[result_of[i]])
                return false;
        }

        for (auto m = head[merge]; m != k_nothing && _instructions[m].code == inst_code::phi; m = next[m])
        {
            auto& phi = _instructions[m];
            auto const first = u32(_args.size());
            auto value_from_b = k_nothing;
            for (auto k = u32(0); k < phi.count; k += 2)
            {
                if (_args[phi.first + k + 1] == u32(b))
                {
                    value_from_b = current(_args[phi.first + k]);
                    continue;
                }
                _args.push_back(_args[phi.first + k]);
                _args.push_back(_args[phi.first + k + 1]);
            }
            for (auto const p : preds)
            {
                auto value = value_from_b;
                for (auto i = head[b]; i != tail[b]; i = next[i])
                    if (result_of[i] == value_from_b)
                    {
                        auto const& held = _instructions[i];
                        for (auto k = u32(0); k < held.count; k += 2)
                            if (_args[held.first + k + 1] == p)
                                value = _args[held.first + k];
                    }
                _args.push_back(value);
                _args.push_back(p);
            }
            phi.first = first;
            phi.count = u32(_args.size()) - first;
        }
        for (auto const p : preds)
        {
            auto const& last = _instructions[tail[p]];
            auto const redirect = [&](u32& target)
            {
                if (target == u32(b))
                    target = merge;
            };
            if (last.code == inst_code::branch)
                redirect(_args[last.first]);
            else if (last.code == inst_code::branch_if)
            {
                redirect(_args[last.first]);
                redirect(_args[last.first + 1]);
            }
            else if (last.code == inst_code::switch_)
            {
                redirect(_args[last.first + 1]);
                for (auto k = u32(2); k < last.count; k += 2)
                    redirect(_args[last.first + k + 1]);
            }
            is_touched[p] = true;
        }
        incoming[merge] += u32(preds.size()) - 1;
        is_dead[b] = true;
        is_touched[b] = true;
        is_touched[merge] = true;
        return true;
    };

    for (auto round = 0; round < k_most_flatten_rounds; ++round)
    {
        auto changed = false;
        for (auto& count : incoming)
            count = 0;
        for (auto& touched : is_touched)
            touched = false;
        for (auto& first : first_way)
            first = 0;
        for (auto b = isize(0); b < block_count; ++b)
            if (!is_dead[b])
                each_successor(b,
                               [&](u32 to)
                               {
                                   ++incoming[to];
                                   ++first_way[to + 1];
                               });
        for (auto b = isize(0); b < block_count; ++b)
            first_way[b + 1] += first_way[b];
        ways_in.resize_to_filled(isize(first_way[block_count]), 0u);
        {
            auto cursor = cc::vector<u32>::create_copy_of(cc::span<u32 const>(first_way));
            for (auto b = isize(0); b < block_count; ++b)
                if (!is_dead[b])
                    each_successor(b, [&](u32 to) { ways_in[cursor[to]++] = u32(b); });
        }
        uses.resize_to_filled(_results.size(), 0u);
        for (auto& count : uses)
            count = 0;
        for (auto b = isize(0); b < block_count; ++b)
            if (!is_dead[b])
                for (auto i = head[b]; i != k_nothing; i = next[i])
                    for_each_value(_instructions[i],
                                   [&](u32& raw)
                                   {
                                       auto const v = current(raw);
                                       if (!ll_is_constant(ll_value(v)) && !ll_is_global(ll_value(v)))
                                           ++uses[v];
                                   });

        for (auto b = isize(0); b < block_count; ++b)
        {
            if (is_dead[b])
                continue;
            auto const last = _instructions[tail[b]];

            if (last.code == inst_code::branch)
            {
                // nothing but phis on the way to a block that has them
                auto only_phis = true;
                for (auto i = head[b]; i != tail[b] && only_phis; i = next[i])
                    only_phis = _instructions[i].code == inst_code::phi;
                if (only_phis && thread_through(b, _args[last.first]))
                {
                    changed = true;
                    continue;
                }
                // a block with this one for its only way in is this block
                auto const to = _args[last.first];
                if (i64(to) == b || incoming[to] != 1 || to == 0)
                    continue;
                while (head[to] != k_nothing && _instructions[head[to]].code == inst_code::phi)
                {
                    auto const phi = head[to];
                    replaced[result_of[phi]] = _args[_instructions[phi].first];
                    head[to] = next[phi];
                    previous[head[to]] = k_nothing;
                }
                // all of `to`, its terminator too, in place of this block's terminator
                auto const before = previous[tail[b]];
                if (before != k_nothing)
                    next[before] = head[to];
                else
                    head[b] = head[to];
                previous[head[to]] = before;
                tail[b] = tail[to];
                is_dead[to] = true;
                rename_edges(b, u32(to), u32(b));
                // what this block now reaches has a way in the lists of this round do not name
                each_successor(b, [&](u32 successor) { is_touched[successor] = true; });
                changed = true;
                // what `to` reached may have just lost a way in
                --b;
                continue;
            }
            if (last.code != inst_code::branch_if)
                continue;

            auto const yes = _args[last.first];
            auto const no = _args[last.first + 1];
            auto const condition = _args[last.first + 2];
            auto const can_move = [&](u32 side, u32 past)
            { return incoming[side] == 1 && ends_in_branch_to(side) == past && body_is_small(side); };
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
            else if (ends_in_branch_to(yes) != k_nothing && ends_in_branch_to(yes) == ends_in_branch_to(no)
                     && can_move(yes, ends_in_branch_to(yes)) && can_move(no, ends_in_branch_to(no)))
            {
                merge = ends_in_branch_to(yes);
                is_yes = true;
                is_no = true;
            }
            if (merge == k_nothing || i64(merge) == b)
                continue;

            if (is_yes)
                splice_body(yes, b);
            if (is_no)
                splice_body(no, b);
            // each phi of the merge: what either way gave is now one value, from this block
            auto const from_yes = is_yes ? yes : u32(b);
            auto const from_no = is_no ? no : u32(b);
            for (auto i = head[merge]; i != k_nothing && _instructions[i].code == inst_code::phi; i = next[i])
            {
                auto const count = _instructions[i].count;
                auto yes_value = k_nothing;
                auto no_value = k_nothing;
                auto kept = u32(0);
                for (auto k = u32(0); k < count; k += 2)
                {
                    auto const value = _args[_instructions[i].first + k];
                    auto const block = _args[_instructions[i].first + k + 1];
                    if (block == from_yes)
                        yes_value = value;
                    else if (block == from_no)
                        no_value = value;
                    else
                    {
                        _args[_instructions[i].first + kept] = value;
                        _args[_instructions[i].first + kept + 1] = block;
                        kept += 2;
                    }
                }
                auto const chosen = select_at_end(b, condition, yes_value, no_value);
                _args[_instructions[i].first + kept] = chosen;
                _args[_instructions[i].first + kept + 1] = u32(b);
                _instructions[i].count = kept + 2;
            }
            if (is_yes)
                is_dead[yes] = true;
            if (is_no)
                is_dead[no] = true;
            // the branch is one way now
            auto& terminator = _instructions[tail[b]];
            terminator.code = inst_code::branch;
            terminator.count = 1;
            _args[terminator.first] = merge;
            // the two ways into the merge are one now
            --incoming[merge];
            is_touched[merge] = true;
            changed = true;
            --b;
        }
        if (!changed)
            break;
    }

    // ---- the lists as tables again, in reverse postorder: joined blocks may stand ahead of what they now dominate
    struct visit
    {
        u32 block = 0;
        u32 first = 0;
        u32 count = 0;
    };
    auto successors = cc::vector<u32>();
    auto walk = cc::vector<visit>();
    auto postorder = cc::vector<u32>();
    auto is_seen = cc::vector<bool>::create_filled(block_count, false);
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
    for (auto i = postorder.size(); i > 0; --i)
        order.push_back(postorder[i - 1]);

    auto block_place = cc::vector<u32>::create_filled(block_count, k_nothing);
    for (auto i = isize(0); i < order.size(); ++i)
        block_place[order[i]] = u32(i);

    auto renumbered = cc::vector<u32>::create_filled(_results.size(), k_nothing);
    auto next_result = u32(0);
    for (auto const b : order)
        for (auto i = head[b]; i != k_nothing; i = next[i])
            if (_instructions[i].type != _void)
                renumbered[result_of[i]] = next_result++;

    auto kept = cc::vector<inst_entry>::create_with_capacity(_instructions.size());
    auto kept_results = cc::vector<u32>::create_with_capacity(isize(next_result));
    auto kept_blocks = cc::vector<block_entry>();
    auto const moved_value = [&](u32& raw)
    {
        raw = current(raw);
        if (!ll_is_constant(ll_value(raw)) && !ll_is_global(ll_value(raw)))
            raw = renumbered[raw];
    };
    for (auto const b : order)
    {
        auto const start = u32(kept.size());
        for (auto i = head[b]; i != k_nothing; i = next[i])
        {
            auto const& inst = _instructions[i];
            for_each_value(inst, moved_value);
            auto* const args = _args.data() + inst.first;
            if (inst.code == inst_code::phi)
                for (auto k = u32(1); k < inst.count; k += 2)
                    args[k] = block_place[args[k]];
            else if (inst.code == inst_code::branch)
                args[0] = block_place[args[0]];
            else if (inst.code == inst_code::branch_if)
            {
                args[0] = block_place[args[0]];
                args[1] = block_place[args[1]];
            }
            else if (inst.code == inst_code::switch_)
            {
                args[1] = block_place[args[1]];
                for (auto k = u32(2); k < inst.count; k += 2)
                    args[k + 1] = block_place[args[k + 1]];
            }
            if (inst.type != _void)
                kept_results.push_back(u32(kept.size()));
            kept.push_back(inst);
        }
        kept_blocks.push_back({.first = start, .count = u32(kept.size()) - start});
    }
    _instructions = cc::move(kept);
    _results = cc::move(kept_results);
    _blocks = cc::move(kept_blocks);
}

void module_builder::chain_sums()
{
    constexpr u8 k_reorders = fp_flags::unsafe_algebra;
    auto uses = cc::vector<u32>::create_filled(_results.size(), 0u);
    for (auto const& inst : _instructions)
        for_each_value(inst,
                       [&](u32& raw)
                       {
                           if (!ll_is_constant(ll_value(raw)) && !ll_is_global(ll_value(raw)))
                               ++uses[raw];
                       });
    auto result_of = cc::vector<u32>::create_filled(_instructions.size(), 0xffffffffu);
    for (auto r = isize(0); r < _results.size(); ++r)
        result_of[_results[r]] = u32(r);
    auto is_taken = cc::vector<bool>::create_filled(_instructions.size(), false);

    auto const is_sum = [&](inst_entry const& inst)
    {
        return inst.code == inst_code::binary && binary_op(inst.sub) == binary_op::add && is_float(inst.type)
            && (inst.flags & k_reorders) != 0;
    };

    struct leaf
    {
        u32 value = 0;
        /// Where it is defined; what stands outside the block comes first.
        u32 position = 0;
    };
    auto sums = cc::vector<u32>();
    auto leaves = cc::vector<leaf>();
    auto pending = cc::vector<u32>();
    for (auto const& block : _blocks)
    {
        for (auto i = block.first + block.count; i > block.first; --i)
        {
            auto const root = i - 1;
            if (!is_sum(_instructions[root]) || is_taken[root])
                continue;
            sums.clear();
            leaves.clear();
            pending.clear();
            pending.push_back(root);
            while (!pending.empty())
            {
                auto const at = pending.back();
                pending.pop_back();
                sums.push_back(at);
                is_taken[at] = true;
                auto const& inst = _instructions[at];
                for (auto k = u32(0); k < 2; ++k)
                {
                    auto const raw = _args[inst.first + k];
                    auto const is_instruction = !ll_is_constant(ll_value(raw)) && !ll_is_global(ll_value(raw));
                    auto const defined = is_instruction ? _results[raw] : 0u;
                    auto const is_here = is_instruction && defined >= block.first && defined < block.first + block.count;
                    if (is_here && uses[raw] == 1 && is_sum(_instructions[defined])
                        && _instructions[defined].flags == inst.flags)
                        pending.push_back(defined);
                    else
                        leaves.push_back({.value = raw, .position = is_here ? defined - block.first + 1 : 0u});
                }
            }
            if (sums.size() < 2)
                continue;

            // by place, both: the n-th sum has n + 1 leaves ahead of it, so each leaf is defined where it lands
            for (auto a = isize(1); a < leaves.size(); ++a)
            {
                auto const held = leaves[a];
                auto at = a;
                while (at > 0 && leaves[at - 1].position > held.position)
                {
                    leaves[at] = leaves[at - 1];
                    --at;
                }
                leaves[at] = held;
            }
            for (auto a = isize(1); a < sums.size(); ++a)
            {
                auto const held = sums[a];
                auto at = a;
                while (at > 0 && sums[at - 1] > held)
                {
                    sums[at] = sums[at - 1];
                    --at;
                }
                sums[at] = held;
            }
            for (auto a = isize(0); a < sums.size(); ++a)
            {
                auto const& inst = _instructions[sums[a]];
                _args[inst.first] = a == 0 ? leaves[0].value : result_of[sums[a - 1]];
                _args[inst.first + 1] = leaves[a + 1].value;
            }
        }
    }
}

void module_builder::remove_unused()
{
    auto is_live = cc::vector<bool>::create_filled(_instructions.size(), false);
    auto stack = cc::vector<u32>();
    auto const mark = [&](u32& raw)
    {
        auto const v = ll_value(raw);
        if (ll_is_constant(v) || ll_is_global(v))
            return;
        auto const at = _results[raw];
        if (!is_live[at])
        {
            is_live[at] = true;
            stack.push_back(at);
        }
    };
    for (auto i = isize(0); i < _instructions.size(); ++i)
        if (has_effect(_instructions[i]))
        {
            is_live[i] = true;
            stack.push_back(u32(i));
        }
    while (!stack.empty())
    {
        auto const at = stack.back();
        stack.pop_back();
        for_each_value(_instructions[at], mark);
    }

    // the results that are left, numbered as they come
    auto renumbered = cc::vector<u32>::create_filled(_results.size(), 0u);
    auto kept_results = cc::vector<u32>::create_with_capacity(_results.size());
    auto kept = cc::vector<inst_entry>::create_with_capacity(_instructions.size());
    auto next_result = u32(0);
    for (auto r = isize(0); r < _results.size(); ++r)
        if (is_live[_results[r]])
            renumbered[r] = next_result++;

    auto const moved = [&](u32& raw)
    {
        auto const v = ll_value(raw);
        if (!ll_is_constant(v) && !ll_is_global(v))
            raw = renumbered[raw];
    };
    _call_count = 0;
    for (auto& b : _blocks)
    {
        auto const start = u32(kept.size());
        for (auto i = b.first; i < b.first + b.count; ++i)
        {
            if (!is_live[i])
                continue;
            auto const& inst = _instructions[i];
            for_each_value(inst, moved);
            if (inst.type != _void)
                kept_results.push_back(u32(kept.size()));
            _call_count += inst.code == inst_code::call ? 1 : 0;
            kept.push_back(inst);
        }
        b.first = start;
        b.count = u32(kept.size()) - start;
    }
    _instructions = cc::move(kept);
    _results = cc::move(kept_results);
}
