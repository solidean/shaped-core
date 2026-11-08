#include "passes.hh"

#include <clean-core/container/vector.hh>
#include <shaped-graphics-language/ir/dominance.hh>
#include <shaped-graphics-language/ir/ir.hh>

using namespace sgl;
using namespace sgl::ir;

namespace
{
/// Whether reading through `pointer` always gives the same value, since nothing in the shader can write there.
[[nodiscard]] bool reads_unchanging_memory(program const& p, value_ref pointer)
{
    while (is_operation(pointer))
    {
        auto const& i = p.instructions[index_of(pointer)];
        if (i.code != op::access_chain)
            return false;
        pointer = value_ref(p.operands_of(i)[0]);
    }
    if (!is_global(pointer))
        return false;
    auto const& g = p.globals[index_of(pointer)];
    if (p.at(g.type).space == storage::table)
        return true;
    switch (g.kind)
    {
    case global_kind::constants:
    case global_kind::inline_constants:
        return true;
    case global_kind::buffer:
    case global_kind::bytes:
        return g.is_read_only;
    default:
        return false;
    }
}

[[nodiscard]] bool may_share(program const& p, instruction const& i)
{
    if (i.code == op::load)
        return reads_unchanging_memory(p, value_ref(p.operands_of(i)[0]));
    return gives_only_a_value(i.code);
}
} // namespace

void sgl::ir::remove_unused(program& p)
{
    auto is_live = cc::vector<bool>::create_filled(p.instructions.size(), false);
    auto stack = cc::vector<u32>();
    auto const keep = [&](u32 at)
    {
        if (!is_live[at])
        {
            is_live[at] = true;
            stack.push_back(at);
        }
    };
    for (auto const& block : p.blocks)
        for (auto i = block.first; i < block.first + block.count; ++i)
            if (has_effect(p.instructions[i].code))
                keep(i);
    while (!stack.empty())
    {
        auto const& i = p.instructions[stack.back()];
        stack.pop_back();
        auto const operands = p.operands_of(i);
        for (auto k = isize(0); k < operands.size(); ++k)
            if (operand_role(i.code, k) == operand::value && is_operation(value_ref(operands[k])))
                keep(index_of(value_ref(operands[k])));
    }

    auto const order = blocks_in_order(p);
    auto place = cc::vector<u32>::create_filled(p.instructions.size(), 0u);
    auto next = u32(0);
    for (auto const b : order)
    {
        auto const& block = p.blocks[isize(b)];
        for (auto i = block.first; i < block.first + block.count; ++i)
            if (is_live[i])
                place[i] = next++;
    }
    auto kept = cc::vector<instruction>::create_with_capacity(isize(next));
    for (auto const b : order)
    {
        auto& block = p.blocks[isize(b)];
        auto const start = u32(kept.size());
        for (auto i = block.first; i < block.first + block.count; ++i)
        {
            if (!is_live[i])
                continue;
            auto const& inst = p.instructions[i];
            auto* const operands = p.operands.data() + inst.first;
            for (auto k = isize(0); k < isize(inst.count); ++k)
                if (operand_role(inst.code, k) == operand::value && is_operation(value_ref(operands[k])))
                    operands[k] = place[index_of(value_ref(operands[k]))];
            kept.push_back(inst);
        }
        block.first = start;
        block.count = u32(kept.size()) - start;
    }
    p.instructions = cc::move(kept);
}

void sgl::ir::share_common_values(program& p)
{
    // a block with nothing in it is one no walk reached, which every table here skips
    auto const order = blocks_in_order(p);
    auto place = cc::vector<u32>::create_filled(p.blocks.size(), 0xffffffffu);
    auto walked = cc::vector<block_ref>::create_with_capacity(order.size());
    for (auto const b : order)
        if (p.blocks[isize(b)].count > 0)
        {
            place[isize(b)] = u32(walked.size());
            walked.push_back(b);
        }

    // the graph, by place: a block's predecessors as one run per block, from what a walk reaches alone
    auto is_reached = cc::vector<bool>::create_filled(walked.size(), false);
    auto stack = cc::vector<u32>();
    auto scratch = cc::vector<u32>();
    auto const terminator_of = [&](isize at) -> instruction const&
    {
        auto const& block = p.blocks[isize(walked[at])];
        return p.instructions[block.first + block.count - 1];
    };
    is_reached[0] = true;
    stack.push_back(0);
    auto edges_to = cc::vector<u32>::create_filled(walked.size() + 1, 0u);
    while (!stack.empty())
    {
        auto const at = stack.back();
        stack.pop_back();
        for (auto const to : successors_of(p, terminator_of(at), scratch))
        {
            ++edges_to[place[to] + 1];
            if (!is_reached[place[to]])
            {
                is_reached[place[to]] = true;
                stack.push_back(place[to]);
            }
        }
    }
    for (auto i = isize(0); i < walked.size(); ++i)
        edges_to[i + 1] += edges_to[i];
    auto preds = cc::vector<u32>::create_filled(isize(edges_to[walked.size()]), 0u);
    auto cursor = cc::vector<u32>::create_copy_of(cc::span<u32 const>(edges_to));
    for (auto i = isize(0); i < walked.size(); ++i)
        if (is_reached[i])
            for (auto const to : successors_of(p, terminator_of(i), scratch))
                preds[cursor[place[to]]++] = u32(i);
    auto const tree = dominance(edges_to, preds);

    struct entry
    {
        u32 hash = 0;
        u32 index = 0;
        u32 block = 0;
    };
    auto size = isize(64);
    while (size < p.instructions.size() * 2)
        size *= 2;
    auto table = cc::vector<entry>::create_filled(size, {});
    auto const wrap = size - 1;
    auto replaced = cc::vector<u32>::create_filled(p.instructions.size(), 0xffffffffu);
    auto const current = [&](u32 raw) -> u32
    {
        if (!is_operation(value_ref(raw)))
            return raw;
        return replaced[index_of(value_ref(raw))] != 0xffffffffu ? replaced[index_of(value_ref(raw))] : raw;
    };

    for (auto at = isize(0); at < walked.size(); ++at)
    {
        if (!is_reached[at])
            continue;
        auto const& block = p.blocks[isize(walked[at])];
        for (auto index = block.first; index < block.first + block.count; ++index)
        {
            auto const& i = p.instructions[index];
            auto* const operands = p.operands.data() + i.first;
            for (auto k = isize(0); k < isize(i.count); ++k)
                if (operand_role(i.code, k) == operand::value)
                    operands[k] = current(operands[k]);
            if (!may_share(p, i))
                continue;
            auto hash = u32(i.code) * 0x9e3779b1u + u32(i.type) * 0x85ebca6bu + (u32(i.sub) << 16) + (u32(i.aux) << 8)
                      + u32(i.guarantee);
            for (auto k = u32(0); k < i.count; ++k)
                hash = (hash ^ operands[k]) * 0x01000193u;
            hash |= 1u;
            auto slot = isize(hash) & wrap;
            auto found = false;
            while (table[slot].hash != 0 && !found)
            {
                if (table[slot].hash == hash && tree.dominates(table[slot].block, u32(at)))
                {
                    auto const& other = p.instructions[table[slot].index];
                    auto is_same = other.code == i.code && other.sub == i.sub && other.aux == i.aux
                                && other.guarantee == i.guarantee && other.type == i.type && other.count == i.count;
                    for (auto k = u32(0); k < i.count && is_same; ++k)
                        is_same = p.operands[other.first + k] == operands[k];
                    if (is_same)
                    {
                        replaced[index] = table[slot].index;
                        found = true;
                        break;
                    }
                }
                slot = (slot + 1) & wrap;
            }
            if (!found)
                table[slot] = {.hash = hash, .index = index, .block = u32(at)};
        }
    }
    // a phi reaches back to values written after it
    for (auto const& i : p.instructions)
        if (i.code == op::phi)
            for (auto k = u32(0); k < i.count; k += 2)
                p.operands[i.first + k] = current(p.operands[i.first + k]);
}
