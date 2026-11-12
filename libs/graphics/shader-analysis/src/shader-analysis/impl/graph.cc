#include "graph.hh"

namespace shan::impl
{
namespace
{
[[nodiscard]] bool test(cc::span<u64 const> bits, i32 v)
{
    return ((bits[v / 64] >> (v % 64)) & 1u) != 0;
}
void set(cc::span<u64> bits, i32 v)
{
    bits[v / 64] |= u64(1) << (v % 64);
}
void clear(cc::span<u64> bits, i32 v)
{
    bits[v / 64] &= ~(u64(1) << (v % 64));
}
} // namespace

cost_class class_of(op_kind kind)
{
    switch (kind)
    {
    case op_kind::float_arithmetic:
    case op_kind::int_arithmetic:
    case op_kind::extended:
    case op_kind::compare:
    case op_kind::logic:
    case op_kind::subgroup:
    case op_kind::derivative:
        return cost_class::alu;
    case op_kind::conversion:
        return cost_class::conversion;
    case op_kind::load:
    case op_kind::store:
    case op_kind::image:
    case op_kind::atomic:
    case op_kind::barrier:
    case op_kind::ray:
        return cost_class::memory;
    case op_kind::control:
        return cost_class::control;
    default:
        return cost_class::free;
    }
}

void measure(graph const& g, function_metrics& f)
{
    auto const values = g.widths.size();
    auto const words = (values + 63) / 64;
    auto const blocks = g.blocks.size();
    f.blocks = i32(blocks);
    for (auto const& b : g.blocks)
        f.counted_loops += b.trips > 0 ? 1 : 0;

    auto const bitset = [&] { return cc::vector<u64>::create_filled(words, 0); };
    auto defs = cc::vector<cc::vector<u64>>();
    auto uses = cc::vector<cc::vector<u64>>();
    auto phi_out = cc::vector<cc::vector<u64>>();
    auto live_in = cc::vector<cc::vector<u64>>();
    auto live_out = cc::vector<cc::vector<u64>>();
    for (auto b = isize(0); b < blocks; ++b)
    {
        defs.push_back(bitset());
        uses.push_back(bitset());
        phi_out.push_back(bitset());
        live_in.push_back(bitset());
        live_out.push_back(bitset());
    }
    auto block_of_value = cc::vector<i32>::create_filled(values, -1);

    auto const varying = varying_values(g);
    auto const frequency = frequencies(g, varying, k_assumed_loop_trips);
    // per block: what it defines, what it reads before defining, what it hands the phis of its successors
    for (auto b = isize(0); b < blocks; ++b)
    {
        auto const& block = g.blocks[b];
        for (auto k = block.first; k < block.end; ++k)
        {
            auto const& i = g.instructions[k];
            if (i.counts)
            {
                ++f.kinds[isize(i.kind)];
                ++f.instructions;
                // a float3's add is three lanes, which a GPU that works on scalars runs as three
                auto const lanes = f64(cc::max(1, i.result >= 0 ? g.widths[i.result] : 1));
                auto is_uniform = i.result >= 0 ? !varying[i.result] : true;
                for (auto o = i.first; o < i.first + i.count && i.result < 0; ++o)
                    is_uniform = is_uniform && !(g.operands[o] >= 0 && varying[g.operands[o]]);
                auto const k = isize(i.kind);
                f.lanes[k] += lanes;
                f.executed_lanes[k] += lanes * frequency[b];
                if (is_uniform)
                {
                    f.uniform_lanes[k] += lanes;
                    f.executed_uniform_lanes[k] += lanes * frequency[b];
                }
                auto const spend = [&](cost_class c, f64 factor)
                {
                    f.executed_class_lanes[isize(c)] += lanes * factor * frequency[b];
                    if (is_uniform)
                        f.executed_uniform_class_lanes[isize(c)] += lanes * factor * frequency[b];
                };
                spend(i.has_cost ? i.cost : class_of(i.kind), i.has_cost ? f64(i.cost_factor) : 1.0);
                if (i.has_cost && i.extra_factor > 0)
                    spend(i.extra, f64(i.extra_factor));
            }
            for (auto p = i.phi_first; p < i.phi_first + i.phi_count; ++p)
                if (g.phi_values[p] >= 0 && g.phi_blocks[p] >= 0 && g.phi_blocks[p] < i32(blocks))
                    set(phi_out[g.phi_blocks[p]], g.phi_values[p]);
            for (auto o = i.first; o < i.first + i.count; ++o)
                if (auto const v = g.operands[o]; v >= 0 && !test(defs[b], v))
                    set(uses[b], v);
            if (i.result >= 0)
            {
                set(defs[b], i.result);
                block_of_value[i.result] = i32(b);
            }
        }
    }

    // liveness to a fixed point: what a block hands on is what its successors read, and its phis' values
    for (auto changed = true; changed;)
    {
        changed = false;
        for (auto b = blocks; b > 0; --b)
        {
            auto const at = b - 1;
            for (auto w = isize(0); w < words; ++w)
            {
                auto out = phi_out[at][w];
                for (auto const s : g.blocks[at].successors)
                    if (s >= 0 && s < i32(blocks))
                        out |= live_in[s][w];
                auto const in = uses[at][w] | (out & ~defs[at][w]);
                changed = changed || out != live_out[at][w] || in != live_in[at][w];
                live_out[at][w] = out;
                live_in[at][w] = in;
            }
        }
    }

    // each block backwards from what it hands on, for the live words, apart by whether a wave shares them; forwards,
    // for its chains
    auto depth = cc::vector<i32>::create_filled(values, 0);
    for (auto b = isize(0); b < blocks; ++b)
    {
        auto const& block = g.blocks[b];
        auto live = cc::vector<u64>::create_copy_of(cc::span<u64 const>(live_out[b]));
        auto live_vector = i32(0);
        auto live_scalar = i32(0);
        auto const add = [&](i32 v, i32 sign)
        {
            if (varying[v])
                live_vector += sign * g.widths[v];
            else
                live_scalar += sign * g.widths[v];
        };
        auto const note = [&]
        {
            f.max_live_words = cc::max(f.max_live_words, live_vector + live_scalar);
            f.max_live_vector_words = cc::max(f.max_live_vector_words, live_vector);
            f.max_live_scalar_words = cc::max(f.max_live_scalar_words, live_scalar);
        };
        for (auto v = isize(0); v < values; ++v)
            if (test(live, i32(v)))
                add(i32(v), 1);
        note();
        for (auto k = block.end; k > block.first; --k)
        {
            auto const& i = g.instructions[k - 1];
            if (i.result >= 0 && test(live, i.result))
            {
                clear(live, i.result);
                add(i.result, -1);
            }
            for (auto o = i.first; o < i.first + i.count; ++o)
                if (auto const v = g.operands[o]; v >= 0 && !test(live, v))
                {
                    set(live, v);
                    add(v, 1);
                }
            note();
        }
        for (auto k = block.first; k < block.end; ++k)
        {
            auto const& i = g.instructions[k];
            auto longest = i32(0);
            for (auto o = i.first; o < i.first + i.count; ++o)
                if (auto const v = g.operands[o]; v >= 0 && block_of_value[v] == i32(b))
                    longest = cc::max(longest, depth[v]);
            auto const reach = longest + latency_of(i.kind);
            f.critical_path = cc::max(f.critical_path, reach);
            if (i.result >= 0)
                depth[i.result] = reach;
        }
    }
}

namespace
{
/// Each node's immediate dominator over `successors` from `entry`, the entry its own and what no walk reaches -1.
[[nodiscard]] cc::vector<i32> immediate_dominators(cc::span<cc::vector<i32> const> successors, i32 entry)
{
    // over the blocks in reverse postorder from block 0 (Cooper, Harvey, Kennedy)
    auto const n = successors.size();
    if (n == 0)
        return {};
    auto order = cc::vector<i32>();
    auto seen = cc::vector<bool>::create_filled(n, false);
    struct frame
    {
        i32 block;
        isize next;
    };
    auto stack = cc::vector<frame>{{.block = entry, .next = 0}};
    seen[entry] = true;
    while (!stack.empty())
    {
        auto& top = stack.back();
        auto const& succ = successors[top.block];
        if (top.next < succ.size())
        {
            auto const s = succ[top.next++];
            if (s >= 0 && s < i32(n) && !seen[s])
            {
                seen[s] = true;
                stack.push_back({.block = s, .next = 0});
            }
            continue;
        }
        order.push_back(top.block);
        stack.pop_back();
    }
    auto rank = cc::vector<i32>::create_filled(n, -1);
    for (auto k = isize(0); k < order.size(); ++k)
        rank[order[k]] = i32(order.size() - 1 - k);
    auto predecessors = cc::vector<cc::vector<i32>>::create_defaulted(n);
    for (auto b = isize(0); b < n; ++b)
        for (auto const s : successors[b])
            if (s >= 0 && s < i32(n))
                predecessors[s].push_back(i32(b));
    auto idom = cc::vector<i32>::create_filled(n, -1);
    idom[entry] = entry;
    auto const intersect = [&](i32 a, i32 b)
    {
        while (a != b)
        {
            while (rank[a] > rank[b])
                a = idom[a];
            while (rank[b] > rank[a])
                b = idom[b];
        }
        return a;
    };
    for (auto changed = true; changed;)
    {
        changed = false;
        for (auto k = order.size(); k > 0; --k)
        {
            auto const b = order[k - 1];
            if (b == entry)
                continue;
            auto chosen = -1;
            for (auto const p : predecessors[b])
                if (idom[p] >= 0)
                    chosen = chosen < 0 ? p : intersect(p, chosen);
            if (chosen >= 0 && idom[b] != chosen)
            {
                idom[b] = chosen;
                changed = true;
            }
        }
    }
    return idom;
}

/// The blocks' successors, as lists.
[[nodiscard]] cc::vector<cc::vector<i32>> successor_lists(graph const& g)
{
    auto lists = cc::vector<cc::vector<i32>>();
    auto const n = i32(g.blocks.size());
    for (auto const& b : g.blocks)
    {
        auto& list = lists.emplace_back();
        for (auto const s : b.successors)
            if (s >= 0 && s < n)
                list.push_back(s);
    }
    return lists;
}
} // namespace

cc::vector<i32> dominators(graph const& g)
{
    if (g.blocks.empty())
        return {};
    return immediate_dominators(successor_lists(g), 0);
}

cc::vector<i32> post_dominators(graph const& g)
{
    // the edges reversed, from one exit every block that leaves the function reaches
    auto const n = i32(g.blocks.size());
    if (n == 0)
        return {};
    auto reversed = cc::vector<cc::vector<i32>>::create_defaulted(isize(n) + 1);
    for (auto b = 0; b < n; ++b)
    {
        auto leaves = true;
        for (auto const s : g.blocks[b].successors)
            if (s >= 0 && s < n)
            {
                reversed[s].push_back(b);
                leaves = false;
            }
        if (leaves)
            reversed[n].push_back(b);
    }
    auto ipdom = immediate_dominators(reversed, n);
    ipdom.pop_back();
    for (auto& d : ipdom)
        d = d == n ? -1 : d;
    return ipdom;
}

cc::vector<natural_loop> natural_loops(graph const& g)
{
    auto result = cc::vector<natural_loop>();
    auto const n = i32(g.blocks.size());
    if (n == 0)
        return result;
    auto const idom = dominators(g);
    auto predecessors = cc::vector<cc::vector<i32>>::create_defaulted(n);
    auto latches = cc::vector<cc::vector<i32>>::create_defaulted(n);
    for (auto t = 0; t < n; ++t)
        for (auto const h : g.blocks[t].successors)
        {
            if (h < 0 || h >= n)
                continue;
            predecessors[h].push_back(t);
            if (idom[t] < 0)
                continue;
            for (auto at = t; at >= 0; at = at == idom[at] ? -1 : idom[at])
                if (at == h)
                {
                    latches[h].push_back(t);
                    break;
                }
        }
    for (auto h = 0; h < n; ++h)
    {
        if (latches[h].empty())
            continue;
        auto& loop = result.emplace_back();
        loop.header = h;
        loop.body = cc::vector<bool>::create_filled(n, false);
        loop.body[h] = true;
        auto stack = cc::vector<i32>::create_copy_of(cc::span<i32 const>(latches[h]));
        while (!stack.empty())
        {
            auto const b = stack.back();
            stack.pop_back();
            if (loop.body[b])
                continue;
            loop.body[b] = true;
            for (auto const p : predecessors[b])
                if (!loop.body[p])
                    stack.push_back(p);
        }
    }
    return result;
}


cc::vector<f64> frequencies(graph const& g, cc::span<bool const> varying, f64 loop_trips)
{
    auto const n = i32(g.blocks.size());
    auto frequency = cc::vector<f64>::create_filled(n, 0.0);
    if (n == 0)
        return frequency;
    auto const idom = dominators(g);
    auto const ipdom = post_dominators(g);
    auto const post_dominates = [&](i32 a, i32 b)
    {
        for (auto at = b; at >= 0; at = ipdom[at])
            if (at == a)
                return true;
        return false;
    };
    // each block in the order a walk dominates it, as often as its dominator, but for an arm of a branch
    auto order = cc::vector<i32>();
    auto children = cc::vector<cc::vector<i32>>::create_defaulted(n);
    for (auto b = 1; b < n; ++b)
        if (idom[b] >= 0)
            children[idom[b]].push_back(b);
    order.push_back(0);
    for (auto k = isize(0); k < order.size(); ++k)
        order.push_back_range(children[order[k]]);
    auto const loops = natural_loops(g);
    auto leads_to_loop = cc::vector<bool>::create_filled(n, false);
    for (auto const& loop : loops)
        for (auto at = loop.header; at >= 0; at = at == idom[at] ? -1 : idom[at])
            leads_to_loop[at] = true;
    // the way a loop's exit test stays in the loop runs as often as its trips say, which the loop multiplies in below
    auto stays_in_loop = cc::vector<bool>::create_filled(n, false);
    for (auto const& loop : loops)
        for (auto b = 0; b < n; ++b)
        {
            auto const d = idom[b];
            if (!loop.body[b] || d < 0 || !loop.body[d])
                continue;
            for (auto const s : g.blocks[d].successors)
                if (s >= 0 && s < n && !loop.body[s])
                    stays_in_loop[b] = true;
        }
    frequency[0] = 1.0;
    for (auto const b : order)
    {
        if (b == 0)
            continue;
        auto const d = idom[b];
        auto share = 1.0;
        // an arm that leads into a loop is a guard on that loop, which is all but always taken
        if (!post_dominates(b, d) && !leads_to_loop[b] && !stays_in_loop[b])
        {
            // a branch the wave shares takes one way; one it does not takes them all, one after the other
            auto const condition = g.blocks[d].condition;
            auto const ways = g.blocks[d].successors.size();
            share = condition >= 0 && varying[condition] ? 1.0 : ways > 1 ? 1.0 / f64(ways) : 0.5;
        }
        frequency[b] = frequency[d] * share;
    }
    // a natural loop's body runs its trips as often again
    for (auto const& loop : loops)
    {
        auto const trips = g.blocks[loop.header].trips > 0 ? g.blocks[loop.header].trips : loop_trips;
        for (auto b = 0; b < n; ++b)
            if (loop.body[b])
                frequency[b] *= trips;
    }
    return frequency;
}

i32 back_edges(graph const& g)
{
    auto const idom = dominators(g);
    auto const n = g.blocks.size();
    auto const dominates = [&](i32 a, i32 b)
    {
        for (auto at = b; at >= 0; at = idom[at])
        {
            if (at == a)
                return true;
            if (at == 0)
                break;
        }
        return false;
    };
    auto edges = i32(0);
    for (auto b = isize(0); b < n; ++b)
        if (idom[b] >= 0)
            for (auto const s : g.blocks[b].successors)
                if (s >= 0 && s < i32(n) && dominates(s, i32(b)))
                    ++edges;
    return edges;
}

cc::vector<bool> varying_values(graph const& g)
{
    auto varying = cc::vector<bool>::create_filled(g.widths.size(), false);
    auto const idom = dominators(g);
    auto block_of = cc::vector<i32>::create_filled(g.instructions.size(), -1);
    for (auto b = isize(0); b < g.blocks.size(); ++b)
        for (auto k = g.blocks[b].first; k < g.blocks[b].end; ++k)
            block_of[k] = i32(b);
    auto const parted = [&](i32 at, graph_instruction const& phi)
    {
        auto const top = idom[at];
        for (auto p = phi.phi_first; p < phi.phi_first + phi.phi_count; ++p)
        {
            auto b = g.phi_blocks[p];
            if (b < 0 || b >= i32(g.blocks.size()) || idom[b] < 0)
                continue;
            for (auto steps = isize(0); steps <= g.blocks.size(); ++steps)
            {
                if (auto const c = g.blocks[b].condition; c >= 0 && varying[c])
                    return true;
                if (b == top || b == 0)
                    break;
                b = idom[b];
            }
        }
        return false;
    };
    // grows only, so it ends: a phi of a loop may turn varying once a value of its back edge has
    for (auto changed = true; changed;)
    {
        changed = false;
        for (auto k = isize(0); k < g.instructions.size(); ++k)
        {
            auto const& i = g.instructions[k];
            if (i.result < 0 || varying[i.result] || i.source == uniformity_source::uniform)
                continue;
            auto is_varying = i.source == uniformity_source::varying;
            for (auto o = i.first; o < i.first + i.count && !is_varying; ++o)
                is_varying = g.operands[o] >= 0 && varying[g.operands[o]];
            for (auto p = i.phi_first; p < i.phi_first + i.phi_count && !is_varying; ++p)
                is_varying = g.phi_values[p] >= 0 && varying[g.phi_values[p]];
            // a phi where lanes a varying branch parted meet again: one between an incoming block and the phi's
            // immediate dominator, which is the branch of an if and a loop's exit test alike
            if (!is_varying && i.phi_count > 0 && block_of[k] >= 0 && !idom.empty())
                is_varying = parted(block_of[k], i);
            if (is_varying)
            {
                varying[i.result] = true;
                changed = true;
            }
        }
    }
    return varying;
}

void accumulate(function_metrics const& f, function_metrics& total)
{
    total.instructions += f.instructions;
    for (auto k = isize(0); k < k_op_kind_count; ++k)
        total.kinds[k] += f.kinds[k];
    total.blocks += f.blocks;
    total.loops += f.loops;
    total.selections += f.selections;
    total.calls += f.calls;
    total.variable_words += f.variable_words;
    for (auto k = isize(0); k < k_op_kind_count; ++k)
    {
        total.lanes[k] += f.lanes[k];
        total.uniform_lanes[k] += f.uniform_lanes[k];
        total.executed_lanes[k] += f.executed_lanes[k];
        total.executed_uniform_lanes[k] += f.executed_uniform_lanes[k];
    }
    for (auto c = isize(0); c < k_cost_class_count; ++c)
    {
        total.executed_class_lanes[c] += f.executed_class_lanes[c];
        total.executed_uniform_class_lanes[c] += f.executed_uniform_class_lanes[c];
    }
    total.counted_loops += f.counted_loops;
    total.max_live_words = cc::max(total.max_live_words, f.max_live_words);
    total.max_live_vector_words = cc::max(total.max_live_vector_words, f.max_live_vector_words);
    total.max_live_scalar_words = cc::max(total.max_live_scalar_words, f.max_live_scalar_words);
    total.critical_path = cc::max(total.critical_path, f.critical_path);
}
} // namespace shan::impl
