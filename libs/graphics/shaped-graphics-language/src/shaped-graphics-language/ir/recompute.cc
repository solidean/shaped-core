#include "passes.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <shaped-graphics-language/ir/dominance.hh>
#include <shaped-graphics-language/ir/ir.hh>

using namespace sgl;
using namespace sgl::ir;

namespace
{
constexpr auto k_none = 0xffffffffu;

/// The global `pointer` reaches into through access chains, or -1 where it reaches into none.
[[nodiscard]] i32 global_root_of(program const& p, value_ref pointer)
{
    while (is_operation(pointer))
    {
        auto const& i = p.instructions[index_of(pointer)];
        if (i.code != op::access_chain)
            return -1;
        pointer = value_ref(p.operands_of(i)[0]);
    }
    return is_global(pointer) ? i32(index_of(pointer)) : -1;
}

/// Whether an access chain from global `g` names an element of memory, not a resource of a binding array.
[[nodiscard]] bool is_plain_memory(program const& p, u32 g)
{
    auto const& global = p.globals[g];
    switch (global.kind)
    {
    case global_kind::constants:
    case global_kind::inline_constants:
    case global_kind::buffer:
    case global_kind::bytes:
    {
        auto const& outer = p.at(p.at(global.type).element);
        return outer.kind != type_kind::array && outer.kind != type_kind::runtime_array;
    }
    default:
        return false;
    }
}

/// Whether `i`, its operands aside, gives the same value wherever in the invocation it runs: a read of a constant
/// block, the launch's index or size, an address into memory, or arithmetic.
/// A read of memory the shader or a called shader can write is none, and neither is a resource handle.
[[nodiscard]] bool gives_a_lasting_value(program const& p, instruction const& i)
{
    auto const operands = p.operands_of(i);
    switch (i.code)
    {
    case op::derivative:
        return false;
    case op::stage_load:
    {
        if (operands.size() != 1)
            return false;
        auto const system = p.stage_elements[operands[0]].system;
        return system == built_in::launch_id || system == built_in::launch_size;
    }
    case op::load:
    {
        auto const root = global_root_of(p, value_ref(operands[0]));
        if (root < 0 || !is_plain_memory(p, u32(root)))
            return false;
        auto const kind = p.globals[root].kind;
        return kind == global_kind::constants || kind == global_kind::inline_constants;
    }
    case op::access_chain:
    {
        auto const root = global_root_of(p, value_ref(i.count > 0 ? operands[0] : u32(value_ref::none)));
        return root >= 0 && is_plain_memory(p, u32(root));
    }
    default:
        return u16(i.code) <= u16(op::shuffle);
    }
}

/// Where each instruction stands, and the call points of each block by position.
struct layout
{
    cc::vector<u32> block_of;
    /// Each block's call points, as one run per block, by position in `program::instructions`.
    cc::vector<u32> calls_first;
    cc::vector<u32> calls;
};
} // namespace

void sgl::ir::recompute_after_calls(program& p)
{
    auto any_call = false;
    for (auto const& i : p.instructions)
        any_call = any_call || is_call_point(i.code);
    if (!any_call)
        return;

    auto const block_count = p.blocks.size();
    auto lay = layout();
    lay.block_of = cc::vector<u32>::create_filled(p.instructions.size(), k_none);
    lay.calls_first = cc::vector<u32>::create_filled(block_count + 1, 0u);
    for (auto b = isize(0); b < block_count; ++b)
    {
        auto const& block = p.blocks[b];
        for (auto i = block.first; i < block.first + block.count; ++i)
        {
            lay.block_of[i] = u32(b);
            if (is_call_point(p.instructions[i].code))
                ++lay.calls_first[b + 1];
        }
    }
    for (auto b = isize(0); b < block_count; ++b)
        lay.calls_first[b + 1] += lay.calls_first[b];
    lay.calls = cc::vector<u32>::create_filled(isize(lay.calls_first[block_count]), 0u);
    auto is_marked = cc::vector<bool>::create_filled(block_count, false);
    for (auto b = isize(0); b < block_count; ++b)
    {
        auto at = lay.calls_first[b];
        auto const& block = p.blocks[b];
        for (auto i = block.first; i < block.first + block.count; ++i)
            if (is_call_point(p.instructions[i].code))
            {
                lay.calls[at++] = i;
                is_marked[b] = true;
            }
    }

    // the graph by block, a block nothing fills holding no edge
    auto scratch = cc::vector<u32>();
    auto succ_first = cc::vector<u32>::create_filled(block_count + 1, 0u);
    auto succs = cc::vector<u32>();
    for (auto b = isize(0); b < block_count; ++b)
    {
        auto const& block = p.blocks[b];
        if (block.count > 0)
            for (auto const to : successors_of(p, p.instructions[block.first + block.count - 1], scratch))
                succs.push_back(to);
        succ_first[b + 1] = u32(succs.size());
    }
    auto walks = marked_walks(succ_first, succs, is_marked);

    // the calls of block `b` standing between positions `after` and `before`, both excluded
    auto const calls_between = [&](u32 b, u32 after, u32 before)
    {
        for (auto k = lay.calls_first[b]; k < lay.calls_first[b + 1]; ++k)
            if (lay.calls[k] > after && lay.calls[k] < before)
                return true;
        return false;
    };
    auto const calls_before = [&](u32 b, u32 before)
    { return lay.calls_first[b] < lay.calls_first[b + 1] && lay.calls[lay.calls_first[b]] < before; };
    // whether a call can run between operation `def` and position `use` of block `b`, which `def` dominates
    auto const crosses = [&](u32 def, u32 b, u32 use)
    {
        auto const from = lay.block_of[def];
        if (from == b)
            return calls_between(b, def, use);
        return calls_between(from, def, k_none) || calls_before(b, use) || walks.passes_mark(from, b);
    };

    // the values a dispatch computes alike at any point of an invocation, from their operands on
    auto is_lasting = cc::vector<bool>::create_filled(p.instructions.size(), false);
    for (auto const b : blocks_in_order(p))
    {
        auto const& block = p.blocks[isize(b)];
        for (auto i = block.first; i < block.first + block.count; ++i)
        {
            auto const& inst = p.instructions[i];
            if (!gives_a_lasting_value(p, inst))
                continue;
            auto fed = true;
            auto const operands = p.operands_of(inst);
            for (auto k = isize(0); k < operands.size() && fed; ++k)
                if (operand_role(inst.code, k) == operand::value && is_operation(value_ref(operands[k])))
                    fed = is_lasting[index_of(value_ref(operands[k]))];
            is_lasting[i] = fed;
        }
    }

    // A stretch of a block between two call points is a segment; a copy serves every use of its segment.
    // Copies are added to the tables' end, each ahead of the position its segment starts at.
    auto const original_count = u32(p.instructions.size());
    auto ahead = cc::vector<cc::vector<u32>>::create_defaulted(original_count);
    // where the segment holding position `at` of block `b` starts: after the call before it, or after the block's phis
    auto const segment_start = [&](u32 b, u32 at)
    {
        auto start = k_none;
        for (auto k = lay.calls_first[b]; k < lay.calls_first[b + 1]; ++k)
            if (lay.calls[k] < at)
                start = lay.calls[k] + 1;
        if (start != k_none)
            return start;
        auto const& block = p.blocks[b];
        start = block.first;
        while (start + 1 < block.first + block.count && p.instructions[start].code == op::phi)
            ++start;
        return start;
    };
    // the operation each copy was made from, by its position past `original_count`; none for a restated variable
    auto origin = cc::vector<u32>();
    auto const copied_from = [&](u32 c) { return origin[c - original_count]; };

    // EMIT-267: each variable a debug value states with a value, which a copy of the value states again where the
    // variable holds it on every way from that debug value to the copy
    struct statement
    {
        u32 at = 0;
        u32 variable = 0;
        u32 leaf_first = 0;
        u32 leaf_end = 0;
    };
    auto statements = cc::vector<statement>();
    auto named_by = cc::vector<cc::vector<u32>>();
    if (p.describes_variables)
    {
        named_by.resize_to_defaulted(original_count);
        for (auto i = u32(0); i < original_count; ++i)
        {
            auto const& inst = p.instructions[i];
            if (inst.code != op::debug_value || lay.block_of[i] == k_none)
                continue;
            auto const operands = p.operands_of(inst);
            auto const value = value_ref(operands[1]);
            auto const leaf = operands[2];
            if (is_operation(value) && index_of(value) < original_count && is_lasting[index_of(value)])
                named_by[index_of(value)].push_back(u32(statements.size()));
            statements.push_back({.at = i,
                                  .variable = operands[0],
                                  .leaf_first = leaf,
                                  .leaf_end = leaf + (is_valid(value) ? p.leaves_of(p.type_of(value)) : 1u)});
        }
    }
    // who dominates whom, by each filled block's place in the blocks' order, which a dominator stands ahead in
    auto place_of = cc::vector<u32>::create_filled(block_count, k_none);
    auto order_first = cc::vector<u32>();
    auto order_preds = cc::vector<u32>();
    auto tree = cc::optional<dominance>();
    auto const dominates = [&](u32 a, u32 b)
    {
        if (!tree.has_value())
        {
            auto filled = cc::vector<u32>();
            for (auto const bb : blocks_in_order(p))
                if (p.blocks[isize(bb)].count > 0)
                {
                    place_of[isize(bb)] = u32(filled.size());
                    filled.push_back(u32(bb));
                }
            order_first.resize_to_filled(filled.size() + 1, 0u);
            for (auto const from : filled)
                for (auto k = succ_first[from]; k < succ_first[from + 1]; ++k)
                    ++order_first[place_of[succs[k]] + 1];
            for (auto k = isize(0); k < filled.size(); ++k)
                order_first[k + 1] += order_first[k];
            order_preds.resize_to_filled(isize(order_first[filled.size()]), 0u);
            auto cursor = cc::vector<u32>::create_copy_of(cc::span<u32 const>(order_first));
            for (auto const from : filled)
                for (auto k = succ_first[from]; k < succ_first[from + 1]; ++k)
                    order_preds[cursor[place_of[succs[k]]]++] = place_of[from];
            tree = dominance(order_first, order_preds);
        }
        return tree.value().dominates(place_of[a], place_of[b]);
    };
    // whether the variable of statement `s` still holds its value at position `start` of block `b`
    auto const holds = [&](statement const& st, u32 b, u32 start)
    {
        auto const from = lay.block_of[st.at];
        auto const restates = [&](u32 i)
        {
            if (i == st.at || p.instructions[i].code != op::debug_value)
                return false;
            auto const operands = p.operands_of(p.instructions[i]);
            if (operands[0] != st.variable)
                return false;
            auto const value = value_ref(operands[1]);
            auto const first = operands[2];
            auto const end = first + (is_valid(value) ? p.leaves_of(p.type_of(value)) : 1u);
            return first < st.leaf_end && st.leaf_first < end;
        };
        // a debug value of the variable standing in `block` from position `first` on, and ahead of `end`
        auto const restated_in = [&](u32 block, u32 first, u32 end)
        {
            auto const& range = p.blocks[block];
            for (auto i = cc::max(range.first, first); i < cc::min(range.first + range.count, end); ++i)
                if (restates(i))
                    return true;
            return false;
        };
        if (from == b)
            return st.at < start && !restated_in(b, st.at + 1, start);
        if (!dominates(from, b) || restated_in(from, st.at + 1, k_none) || restated_in(b, 0, start))
            return false;
        // nor in any block a walk from the one to the other passes
        auto is_restating = cc::vector<bool>::create_filled(block_count, false);
        for (auto bb = u32(0); bb < u32(block_count); ++bb)
            is_restating[bb] = restated_in(bb, 0, k_none);
        auto walk = marked_walks(succ_first, succs, is_restating);
        return !walk.passes_mark(from, b);
    };
    auto const copy_of = [&](auto const& self, u32 v, u32 b, u32 start) -> u32
    {
        for (auto const c : ahead[start])
            if (copied_from(c) == v)
                return c;
        // the operands a copy reads, each a copy of its own where it too would be kept across a call
        auto const source = p.instructions[v];
        auto mapped = cc::vector<u32>::create_with_capacity(source.count);
        for (auto k = u32(0); k < source.count; ++k)
        {
            auto o = p.operands[source.first + k];
            if (operand_role(source.code, k) == operand::value && is_operation(value_ref(o)))
            {
                // an operand an earlier use already moved to a copy is asked of again by the value it copies
                auto from = index_of(value_ref(o));
                if (from >= original_count)
                    from = copied_from(from);
                if (is_lasting[from] && crosses(from, b, start))
                    o = self(self, from, b, start);
            }
            mapped.push_back(o);
        }
        auto copy = source;
        copy.first = u32(p.operands.size());
        for (auto const o : mapped)
            p.operands.push_back(o);
        auto const at = u32(p.instructions.size());
        p.instructions.push_back(copy);
        origin.push_back(v);
        ahead[start].push_back(at);
        if (!named_by.empty())
            for (auto const named : named_by[v])
            {
                auto const& st = statements[named];
                if (!holds(st, b, start))
                    continue;
                auto restated = p.instructions[st.at];
                auto const first = u32(p.operands.size());
                for (auto k = u32(0); k < restated.count; ++k)
                    p.operands.push_back(k == 1 ? at : p.operands[restated.first + k]);
                restated.first = first;
                p.instructions.push_back(restated);
                origin.push_back(k_none);
                ahead[start].push_back(u32(p.instructions.size()) - 1);
            }
        return at;
    };

    for (auto b = u32(0); b < u32(block_count); ++b)
    {
        auto const& block = p.blocks[b];
        auto const first = block.first;
        auto const end = block.first + block.count;
        for (auto i = first; i < end; ++i)
        {
            auto const code = p.instructions[i].code;
            auto const count = p.instructions[i].count;
            for (auto k = u32(0); k < count; ++k)
            {
                auto const raw = p.operands[p.instructions[i].first + k];
                if (operand_role(code, k) != operand::value || !is_operation(value_ref(raw)))
                    continue;
                auto const v = index_of(value_ref(raw));
                if (v >= original_count || !is_lasting[v])
                    continue;
                // a phi reads its value at the end of the way it comes in by
                auto use_block = b;
                auto use = i;
                if (code == op::phi)
                {
                    use_block = p.operands[p.instructions[i].first + k + 1];
                    auto const& from = p.blocks[use_block];
                    use = from.first + from.count - 1;
                }
                if (!crosses(v, use_block, use))
                    continue;
                auto const start = segment_start(use_block, use);
                auto const copy = copy_of(copy_of, v, use_block, start);
                p.operands[p.instructions[i].first + k] = copy;
            }
        }
    }
    if (origin.empty())
        return;

    // the tables again: each copy ahead of the position its segment starts at, in the order the copies were made
    auto place = cc::vector<u32>::create_filled(p.instructions.size(), k_none);
    auto next = u32(0);
    auto const order = blocks_in_order(p);
    for (auto const b : order)
    {
        auto const& block = p.blocks[isize(b)];
        for (auto i = block.first; i < block.first + block.count; ++i)
        {
            for (auto const c : ahead[i])
                place[c] = next++;
            place[i] = next++;
        }
    }
    auto kept = cc::vector<instruction>::create_with_capacity(isize(next));
    auto operands = cc::vector<u32>::create_with_capacity(p.operands.size());
    auto const take = [&](u32 i)
    {
        auto inst = p.instructions[i];
        auto const held = p.operands_of(inst);
        auto const first = u32(operands.size());
        for (auto k = isize(0); k < held.size(); ++k)
        {
            auto o = held[k];
            if (names_a_value(operand_role(inst.code, k)) && is_operation(value_ref(o)))
                o = place[index_of(value_ref(o))];
            operands.push_back(o);
        }
        inst.first = first;
        kept.push_back(inst);
    };
    for (auto const b : order)
    {
        auto& block = p.blocks[isize(b)];
        auto const start = u32(kept.size());
        for (auto i = block.first; i < block.first + block.count; ++i)
        {
            for (auto const c : ahead[i])
                take(c);
            take(i);
        }
        block.first = start;
        block.count = u32(kept.size()) - start;
    }
    p.instructions = cc::move(kept);
    p.operands = cc::move(operands);
}
