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

/// Whether `pointer` reaches into a constant block, whose every element a dispatch reads alike and may read at any time.
[[nodiscard]] bool is_into_constants(program const& p, value_ref pointer)
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
    auto const kind = p.globals[index_of(pointer)].kind;
    return kind == global_kind::constants || kind == global_kind::inline_constants;
}

/// The local `pointer` reaches into, or -1 where it reaches into none.
[[nodiscard]] i32 local_root_of(program const& p, value_ref pointer)
{
    while (is_operation(pointer))
    {
        auto const& i = p.instructions[index_of(pointer)];
        if (i.code != op::access_chain)
            return -1;
        pointer = value_ref(p.operands_of(i)[0]);
    }
    return is_local(pointer) ? i32(index_of(pointer)) : -1;
}

/// Whether `i` gives a value another of its operands gives too; `is_final_read` is a read no store can follow.
[[nodiscard]] bool may_share(program const& p, instruction const& i, bool is_final_read)
{
    if (i.code == op::load)
        return is_final_read || reads_unchanging_memory(p, value_ref(p.operands_of(i)[0]));
    return gives_only_a_value(i.code);
}

/// The blocks that hold anything in their opening order, each one's place among them, which a walk reaches, and who
/// dominates whom by place.
struct walked_graph
{
    cc::vector<block_ref> walked;
    cc::vector<u32> place;
    cc::vector<bool> is_reached;
    cc::vector<u32> edges_to;
    cc::vector<u32> preds;
};

[[nodiscard]] walked_graph walk_of(program const& p)
{
    auto g = walked_graph();
    // a block with nothing in it is one no walk reached, which every table here skips
    auto const order = blocks_in_order(p);
    g.place = cc::vector<u32>::create_filled(p.blocks.size(), 0xffffffffu);
    g.walked = cc::vector<block_ref>::create_with_capacity(order.size());
    for (auto const b : order)
        if (p.blocks[isize(b)].count > 0)
        {
            g.place[isize(b)] = u32(g.walked.size());
            g.walked.push_back(b);
        }

    // the graph, by place: a block's predecessors as one run per block, from what a walk reaches alone
    g.is_reached = cc::vector<bool>::create_filled(g.walked.size(), false);
    auto stack = cc::vector<u32>();
    auto scratch = cc::vector<u32>();
    auto const terminator_of = [&](isize at) -> instruction const&
    {
        auto const& block = p.blocks[isize(g.walked[at])];
        return p.instructions[block.first + block.count - 1];
    };
    g.is_reached[0] = true;
    stack.push_back(0);
    g.edges_to = cc::vector<u32>::create_filled(g.walked.size() + 1, 0u);
    while (!stack.empty())
    {
        auto const at = stack.back();
        stack.pop_back();
        for (auto const to : successors_of(p, terminator_of(at), scratch))
        {
            ++g.edges_to[g.place[to] + 1];
            if (!g.is_reached[g.place[to]])
            {
                g.is_reached[g.place[to]] = true;
                stack.push_back(g.place[to]);
            }
        }
    }
    for (auto i = isize(0); i < g.walked.size(); ++i)
        g.edges_to[i + 1] += g.edges_to[i];
    g.preds = cc::vector<u32>::create_filled(isize(g.edges_to[g.walked.size()]), 0u);
    auto cursor = cc::vector<u32>::create_copy_of(cc::span<u32 const>(g.edges_to));
    for (auto i = isize(0); i < g.walked.size(); ++i)
        if (g.is_reached[i])
            for (auto const to : successors_of(p, terminator_of(i), scratch))
                g.preds[cursor[g.place[to]]++] = u32(i);
    return g;
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
    // a local nothing reads but stores is never observed, so neither is a store into it
    auto is_read = cc::vector<bool>::create_filled(p.locals.size(), false);
    for (auto const& block : p.blocks)
        for (auto i = block.first; i < block.first + block.count; ++i)
        {
            auto const& inst = p.instructions[i];
            auto const operands = p.operands_of(inst);
            for (auto k = isize(0); k < operands.size(); ++k)
            {
                if (operand_role(inst.code, k) != operand::value
                    || (k == 0 && (inst.code == op::store || inst.code == op::access_chain)))
                    continue;
                auto const local = local_root_of(p, value_ref(operands[k]));
                if (local >= 0)
                    is_read[local] = true;
            }
        }
    auto const is_unobserved = [&](instruction const& inst)
    {
        if (inst.code != op::store)
            return false;
        auto const local = local_root_of(p, value_ref(p.operands_of(inst)[0]));
        return local >= 0 && !is_read[local];
    };
    for (auto const& block : p.blocks)
        for (auto i = block.first; i < block.first + block.count; ++i)
            if (has_effect(p.instructions[i].code) && !is_unobserved(p.instructions[i]))
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

namespace
{
/// Each local a pointer reaches only through loads, stores and access chains, so every write to it is a store.
[[nodiscard]] cc::vector<bool> plain_locals(program const& p, walked_graph const& g)
{
    auto plain = cc::vector<bool>::create_filled(p.locals.size(), true);
    for (auto at = isize(0); at < g.walked.size(); ++at)
    {
        if (!g.is_reached[at])
            continue;
        auto const& block = p.blocks[isize(g.walked[at])];
        for (auto i = block.first; i < block.first + block.count; ++i)
        {
            auto const& inst = p.instructions[i];
            auto const operands = p.operands_of(inst);
            for (auto k = isize(0); k < operands.size(); ++k)
            {
                if (operand_role(inst.code, k) != operand::value)
                    continue;
                auto const local = local_root_of(p, value_ref(operands[k]));
                if (local >= 0
                    && !(k == 0 && (inst.code == op::load || inst.code == op::store || inst.code == op::access_chain)))
                    plain[local] = false;
            }
        }
    }
    return plain;
}

/// Each read of a plain local after which no store to that local can run, by instruction.
/// Two such reads of one element give one value, as does any read one of them dominates.
[[nodiscard]] cc::vector<bool> final_reads(program const& p, walked_graph const& g, cc::span<bool const> plain)
{
    auto is_final = cc::vector<bool>::create_filled(p.instructions.size(), false);
    auto const local_of = [&](instruction const& inst) -> i32
    {
        if (inst.code != op::load && inst.code != op::store)
            return -1;
        return local_root_of(p, value_ref(p.operands_of(inst)[0]));
    };
    auto has_load = cc::vector<bool>::create_filled(p.locals.size(), false);
    for (auto at = isize(0); at < g.walked.size(); ++at)
    {
        auto const& block = p.blocks[isize(g.walked[at])];
        for (auto i = block.first; i < block.first + block.count; ++i)
            if (p.instructions[i].code == op::load && local_of(p.instructions[i]) >= 0)
                has_load[local_of(p.instructions[i])] = true;
    }

    // the blocks a walk can reach a store to the local from, a block holding one among them
    auto scratch = cc::vector<u32>();
    auto reaches = cc::vector<bool>::create_filled(g.walked.size(), false);
    auto stack = cc::vector<u32>();
    for (auto local = isize(0); local < p.locals.size(); ++local)
    {
        if (!plain[local] || !has_load[local])
            continue;
        for (auto&& r : reaches)
            r = false;
        stack.clear();
        for (auto at = isize(0); at < g.walked.size(); ++at)
        {
            if (!g.is_reached[at])
                continue;
            auto const& block = p.blocks[isize(g.walked[at])];
            for (auto i = block.first; i < block.first + block.count; ++i)
                if (p.instructions[i].code == op::store && local_of(p.instructions[i]) == i32(local))
                {
                    reaches[at] = true;
                    stack.push_back(u32(at));
                    break;
                }
        }
        while (!stack.empty())
        {
            auto const at = stack.back();
            stack.pop_back();
            for (auto e = g.edges_to[at]; e < g.edges_to[at + 1]; ++e)
                if (!reaches[g.preds[e]])
                {
                    reaches[g.preds[e]] = true;
                    stack.push_back(g.preds[e]);
                }
        }
        for (auto at = isize(0); at < g.walked.size(); ++at)
        {
            if (!g.is_reached[at])
                continue;
            auto const& block = p.blocks[isize(g.walked[at])];
            auto later = false;
            for (auto const to : successors_of(p, p.instructions[block.first + block.count - 1], scratch))
                later = later || reaches[g.place[to]];
            for (auto i = block.first + block.count; i-- > block.first;)
            {
                auto const& inst = p.instructions[i];
                if (local_of(inst) != i32(local))
                    continue;
                if (inst.code == op::store)
                    later = true;
                else
                    is_final[i] = !later;
            }
        }
    }
    return is_final;
}
} // namespace

void sgl::ir::share_common_values(program& p)
{
    auto const g = walk_of(p);
    auto const& walked = g.walked;
    auto const& is_reached = g.is_reached;
    auto const tree = dominance(g.edges_to, g.preds);
    auto const plain = plain_locals(p, g);

    // A local whose one store copies another local whole, read where nothing writes that other one after, reads as it.
    // Its every operand is the other one but the copy's own target, which nothing then reads and remove_unused drops.
    if (!p.locals.empty())
    {
        auto const is_final = final_reads(p, g, plain);
        auto stores = cc::vector<u32>::create_filled(p.locals.size(), 0u);
        auto copied_from = cc::vector<i32>::create_filled(p.locals.size(), -1);
        for (auto at = isize(0); at < walked.size(); ++at)
        {
            if (!is_reached[at])
                continue;
            auto const& block = p.blocks[isize(walked[at])];
            for (auto i = block.first; i < block.first + block.count; ++i)
            {
                auto const& inst = p.instructions[i];
                if (inst.code != op::store)
                    continue;
                auto const operands = p.operands_of(inst);
                auto const target = local_root_of(p, value_ref(operands[0]));
                if (target < 0)
                    continue;
                ++stores[target];
                auto const value = value_ref(operands[1]);
                if (!is_local(value_ref(operands[0])) || !is_operation(value) || !is_final[index_of(value)])
                    continue;
                auto const source = value_ref(p.operands_of(p.instructions[index_of(value)])[0]);
                if (is_local(source) && index_of(source) != u32(target)
                    && p.locals[index_of(source)].type == p.locals[target].type)
                    copied_from[target] = i32(index_of(source));
            }
        }
        auto forward = cc::vector<i32>::create_filled(p.locals.size(), -1);
        auto any = false;
        for (auto l = isize(0); l < p.locals.size(); ++l)
            if (stores[l] == 1 && copied_from[l] >= 0 && plain[l] && plain[copied_from[l]])
            {
                forward[l] = copied_from[l];
                any = true;
            }
        if (any)
            for (auto& inst : p.instructions)
            {
                auto* const operands = p.operands.data() + inst.first;
                for (auto k = isize(0); k < isize(inst.count); ++k)
                {
                    if (operand_role(inst.code, k) != operand::value || !is_local(value_ref(operands[k])))
                        continue;
                    if (inst.code == op::store && k == 0)
                        continue;
                    auto l = i32(index_of(value_ref(operands[k])));
                    for (auto steps = 0; forward[l] >= 0 && steps < 64; ++steps)
                        l = forward[l];
                    operands[k] = u32(local_at(u32(l)));
                }
            }
    }
    auto const is_final = final_reads(p, g, plain);

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
            if (!may_share(p, i, is_final[index]))
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

namespace
{
/// One round of `hoist_common_values`, which reports whether it moved anything.
[[nodiscard]] bool hoist_round(program& p)
{
    auto const g = walk_of(p);
    auto const tree = dominance(g.edges_to, g.preds);
    auto block_of = cc::vector<u32>::create_filled(p.instructions.size(), 0xffffffffu);
    for (auto at = isize(0); at < g.walked.size(); ++at)
    {
        auto const& block = p.blocks[isize(g.walked[at])];
        for (auto i = block.first; i < block.first + block.count; ++i)
            block_of[i] = u32(at);
    }
    auto const is_same = [&](instruction const& a, instruction const& b)
    {
        if (a.code != b.code || a.type != b.type || a.aux != b.aux || a.count != b.count)
            return false;
        for (auto k = u32(0); k < a.count; ++k)
            if (p.operands[a.first + k] != p.operands[b.first + k])
                return false;
        return true;
    };

    // A value every invocation of the dispatch shares: no phi, no per-thread input, and fed by nothing else.
    // Its operands may stand later in the tables than it does, so the marking runs until it settles.
    auto is_invariant = cc::vector<bool>::create_filled(p.instructions.size(), false);
    for (auto changed = true; changed;)
    {
        changed = false;
        for (auto i = isize(0); i < p.instructions.size(); ++i)
        {
            auto const& inst = p.instructions[i];
            if (is_invariant[i] || block_of[i] == 0xffffffffu || inst.code == op::stage_load)
                continue;
            auto const operands = p.operands_of(inst);
            if (inst.code == op::load ? !is_into_constants(p, value_ref(operands[0])) : !may_speculate(inst.code))
                continue;
            auto fed = true;
            for (auto k = isize(0); k < operands.size(); ++k)
                if (operand_role(inst.code, k) == operand::value && is_operation(value_ref(operands[k]))
                    && !is_invariant[index_of(value_ref(operands[k]))])
                    fed = false;
            if (fed)
            {
                is_invariant[i] = true;
                changed = true;
            }
        }
    }

    // each group's twins: the first of each group, and every other its group holds
    auto lead_of = cc::vector<u32>::create_filled(p.instructions.size(), 0xffffffffu);
    auto leads = cc::vector<u32>();
    for (auto i = isize(0); i < p.instructions.size(); ++i)
    {
        auto const& inst = p.instructions[i];
        if ((inst.code != op::access_chain && !is_invariant[i]) || block_of[i] == 0xffffffffu
            || !g.is_reached[block_of[i]])
            continue;
        auto found = false;
        for (auto const l : leads)
            if (is_same(p.instructions[l], inst))
            {
                lead_of[i] = l;
                found = true;
                break;
            }
        if (!found)
        {
            leads.push_back(u32(i));
            lead_of[i] = u32(i);
        }
    }

    // a group's blocks' common dominator, where every operand is already there; -1 where it has no twin to share
    // A twin already in that block dominates the rest, so it stays where it stands and the others read it.
    auto target = cc::vector<i32>::create_filled(p.instructions.size(), -1);
    auto keeper = cc::vector<u32>::create_filled(p.instructions.size(), 0xffffffffu);
    auto any = false;
    for (auto const l : leads)
    {
        auto common = block_of[l];
        auto twins = 0;
        for (auto i = isize(0); i < p.instructions.size(); ++i)
            if (lead_of[i] == l && u32(i) != l)
            {
                ++twins;
                while (!tree.dominates(common, block_of[i]))
                    common = tree.immediate(common);
            }
        if (twins == 0)
            continue;
        auto available = true;
        for (auto const o : p.operands_of(p.instructions[l]))
            if (is_operation(value_ref(o)) && !tree.dominates(block_of[index_of(value_ref(o))], common))
                available = false;
        if (!available)
            continue;
        keeper[l] = l;
        for (auto i = isize(0); i < p.instructions.size(); ++i)
            if (lead_of[i] == l && block_of[i] == common)
            {
                // the first in the block, which every other in it follows
                keeper[l] = u32(i);
                break;
            }
        target[l] = keeper[l] == l && block_of[l] != common ? i32(common) : -2;
        any = true;
    }
    if (!any)
        return false;

    // the tables again: each group's one chain ahead of its common dominator's terminator, and its twins gone
    // what each block takes ahead of its terminator, a chain after any moved chain it reads
    auto moved_into = cc::vector<cc::vector<u32>>::create_defaulted(g.walked.size());
    auto is_placed = cc::vector<bool>::create_filled(p.instructions.size(), false);
    auto const place_moved = [&](auto&& self, u32 l) -> void
    {
        if (is_placed[l])
            return;
        is_placed[l] = true;
        for (auto const o : p.operands_of(p.instructions[l]))
            if (is_operation(value_ref(o)))
            {
                auto const base = lead_of[index_of(value_ref(o))];
                if (base != 0xffffffffu && target[base] == target[l])
                    self(self, base);
            }
        moved_into[target[l]].push_back(l);
    };
    for (auto const l : leads)
        if (target[l] >= 0)
            place_moved(place_moved, l);

    // a twin read through its group's keeper, and a moved keeper written at its common dominator, not here
    auto const is_folded = [&](u32 i)
    {
        auto const l = lead_of[i];
        if (l == 0xffffffffu || target[l] == -1)
            return false;
        return keeper[l] != i || target[l] >= 0;
    };
    auto place = cc::vector<u32>::create_filled(p.instructions.size(), 0xffffffffu);
    auto next = u32(0);
    auto const order = blocks_in_order(p);
    for (auto const b : order)
    {
        auto const& block = p.blocks[isize(b)];
        auto const at = g.place[isize(b)];
        for (auto i = block.first; i < block.first + block.count; ++i)
        {
            if (is_folded(i))
                continue;
            if (i + 1 == block.first + block.count && at != 0xffffffffu)
                for (auto const l : moved_into[at])
                    place[l] = next++;
            place[i] = next++;
        }
    }
    auto const moved = [&](u32 raw) -> u32
    {
        if (!is_operation(value_ref(raw)))
            return raw;
        auto const i = index_of(value_ref(raw));
        auto const l = lead_of[i] != 0xffffffffu && target[lead_of[i]] != -1 ? keeper[lead_of[i]] : i;
        return place[l];
    };
    auto kept = cc::vector<instruction>::create_with_capacity(isize(next));
    auto operands = cc::vector<u32>::create_with_capacity(p.operands.size());
    auto const copy = [&](u32 i)
    {
        auto inst = p.instructions[i];
        auto const held = p.operands_of(inst);
        auto const first = u32(operands.size());
        for (auto k = isize(0); k < held.size(); ++k)
            operands.push_back(operand_role(inst.code, k) == operand::value ? moved(held[k]) : held[k]);
        inst.first = first;
        kept.push_back(inst);
    };
    for (auto const b : order)
    {
        auto& block = p.blocks[isize(b)];
        auto const at = g.place[isize(b)];
        auto const start = u32(kept.size());
        for (auto i = block.first; i < block.first + block.count; ++i)
        {
            if (is_folded(i))
                continue;
            if (i + 1 == block.first + block.count && at != 0xffffffffu)
                for (auto const l : moved_into[at])
                    copy(l);
            copy(i);
        }
        block.first = start;
        block.count = u32(kept.size()) - start;
    }
    p.instructions = cc::move(kept);
    p.operands = cc::move(operands);
    return true;
}
} // namespace

void sgl::ir::hoist_common_values(program& p)
{
    // a round moves one level of a value's operands, so a load of a moved chain moves the round after it
    for (auto round = 0; round < 16 && hoist_round(p); ++round)
    {
    }
}
