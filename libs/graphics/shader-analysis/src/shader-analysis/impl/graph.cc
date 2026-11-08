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

void measure(graph const& g, function_metrics& f)
{
    auto const values = g.widths.size();
    auto const words = (values + 63) / 64;
    auto const blocks = g.blocks.size();
    f.blocks = i32(blocks);

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

    // each block backwards from what it hands on, for the live words; forwards, for its chains
    auto depth = cc::vector<i32>::create_filled(values, 0);
    for (auto b = isize(0); b < blocks; ++b)
    {
        auto const& block = g.blocks[b];
        auto live = cc::vector<u64>::create_copy_of(cc::span<u64 const>(live_out[b]));
        auto live_words = i32(0);
        for (auto v = isize(0); v < values; ++v)
            if (test(live, i32(v)))
                live_words += g.widths[v];
        f.max_live_words = cc::max(f.max_live_words, live_words);
        for (auto k = block.end; k > block.first; --k)
        {
            auto const& i = g.instructions[k - 1];
            if (i.result >= 0 && test(live, i.result))
            {
                clear(live, i.result);
                live_words -= g.widths[i.result];
            }
            for (auto o = i.first; o < i.first + i.count; ++o)
                if (auto const v = g.operands[o]; v >= 0 && !test(live, v))
                {
                    set(live, v);
                    live_words += g.widths[v];
                }
            f.max_live_words = cc::max(f.max_live_words, live_words);
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

i32 back_edges(graph const& g)
{
    // dominators over the blocks in reverse postorder from block 0 (Cooper, Harvey, Kennedy), then each edge to a
    // block that dominates its source
    auto const n = g.blocks.size();
    if (n == 0)
        return 0;
    auto order = cc::vector<i32>();
    auto seen = cc::vector<bool>::create_filled(n, false);
    struct frame
    {
        i32 block;
        isize next;
    };
    auto stack = cc::vector<frame>{{.block = 0, .next = 0}};
    seen[0] = true;
    while (!stack.empty())
    {
        auto& top = stack.back();
        auto const& succ = g.blocks[top.block].successors;
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
        for (auto const s : g.blocks[b].successors)
            if (s >= 0 && s < i32(n))
                predecessors[s].push_back(i32(b));
    auto idom = cc::vector<i32>::create_filled(n, -1);
    idom[0] = 0;
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
            if (b == 0)
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
    total.max_live_words = cc::max(total.max_live_words, f.max_live_words);
    total.critical_path = cc::max(total.critical_path, f.critical_path);
}
} // namespace shan::impl
