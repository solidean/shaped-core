#include "llvm.hh"

#include <shaped-graphics-language/ir/dominance.hh>

using namespace sgl;
using namespace sgl::ir::dxil::impl;

// What a module goes through once its function is written: only what the writer itself made, since `ir/`'s passes ran
// on the program it was written from.
// Each value of the program becomes several here, so this is where a row of constants or an address two of them
// reach becomes one, and where what no lane of a value read goes away.

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
    remove_unused_declarations();
}

void module_builder::remove_unused_declarations()
{
    auto is_called = cc::vector<bool>::create_filled(_globals.size(), false);
    for (auto const& inst : _instructions)
        if (inst.code == inst_code::call)
            is_called[ll_index(ll_value(_args[inst.first]))] = true;

    // the validator refuses an operation declared and never called, which a call the passes removed leaves behind
    auto moved_to = cc::vector<u32>::create_filled(_globals.size(), 0u);
    auto kept = cc::vector<global_entry>::create_with_capacity(_globals.size());
    for (auto g = isize(0); g < _globals.size(); ++g)
    {
        moved_to[g] = u32(kept.size());
        if (!_globals[g].is_declaration || is_called[g])
            kept.push_back(_globals[g]);
    }
    if (kept.size() == _globals.size())
        return;
    _globals = cc::move(kept);
    auto const moved = [&](u32& raw)
    {
        auto const v = ll_value(raw);
        if (ll_is_global(v))
            raw = moved_to[ll_index(v)] | k_ll_global_bit;
    };
    for (auto& inst : _instructions)
        for_each_value(inst, moved);
}
