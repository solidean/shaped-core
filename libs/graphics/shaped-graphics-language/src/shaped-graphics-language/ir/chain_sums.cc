#include "passes.hh"

#include <clean-core/container/vector.hh>
#include <shaped-graphics-language/ir/ir.hh>

using namespace sgl;
using namespace sgl::ir;

void sgl::ir::chain_sums(program& p)
{
    auto uses = cc::vector<u32>::create_filled(p.instructions.size(), 0u);
    for (auto const& block : p.blocks)
        for (auto i = block.first; i < block.first + block.count; ++i)
        {
            auto const& inst = p.instructions[i];
            auto const operands = p.operands_of(inst);
            for (auto k = isize(0); k < operands.size(); ++k)
                if (operand_role(inst.code, k) == operand::value && is_operation(value_ref(operands[k])))
                    ++uses[index_of(value_ref(operands[k]))];
        }
    auto is_taken = cc::vector<bool>::create_filled(p.instructions.size(), false);
    auto const is_sum
        = [&](instruction const& i) { return i.code == op::fadd && i.guarantee == check::float_level::fast; };

    struct leaf
    {
        u32 value = 0;
        /// Where it is defined; what stands outside the block comes first.
        u32 position = 0;
    };
    auto sums = cc::vector<u32>();
    auto leaves = cc::vector<leaf>();
    auto pending = cc::vector<u32>();
    for (auto const& block : p.blocks)
    {
        for (auto i = block.first + block.count; i > block.first; --i)
        {
            auto const root = i - 1;
            if (!is_sum(p.instructions[root]) || is_taken[root])
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
                auto const operands = p.operands_of(p.instructions[at]);
                for (auto k = isize(0); k < 2; ++k)
                {
                    auto const v = value_ref(operands[k]);
                    auto const is_here
                        = is_operation(v) && index_of(v) >= block.first && index_of(v) < block.first + block.count;
                    if (is_here && uses[index_of(v)] == 1 && is_sum(p.instructions[index_of(v)]))
                        pending.push_back(index_of(v));
                    else
                        leaves.push_back({.value = u32(v), .position = is_here ? index_of(v) - block.first + 1 : 0u});
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
                auto const first = p.instructions[sums[a]].first;
                p.operands[first] = a == 0 ? leaves[0].value : sums[a - 1];
                p.operands[first + 1] = leaves[a + 1].value;
            }
        }
    }
}
