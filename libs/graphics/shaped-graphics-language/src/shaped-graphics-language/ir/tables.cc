#include "passes.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <shaped-graphics-language/ir/dominance.hh>
#include <shaped-graphics-language/ir/ir.hh>

using namespace sgl;
using namespace sgl::ir;

// A local SSA left in memory, since an index reaches it at run time, may still be a value the program states in full:
// written once, whole, with constants, before any read, and never again.
// Such a local is a table every invocation shares, which a target states once, rather than memory each invocation
// fills with a store per element first.

namespace
{
/// More leaves than this are a table a target would rather build than state.
constexpr isize k_most_leaves = 4096;
constexpr u32 k_none = 0xffffffffu;

class tabler
{
public:
    explicit tabler(program& p) : _p(p) {}

    void run()
    {
        index_blocks();
        auto is_dropped = cc::vector<bool>::create_filled(_p.instructions.size(), false);
        auto replaced = cc::vector<value_ref>::create_filled(_p.locals.size(), value_ref::none);
        auto any = false;
        for (auto l = isize(0); l < _p.locals.size(); ++l)
        {
            auto store = k_none;
            auto leaves = cc::vector<u32>();
            if (!qualifies(local_at(u32(l)), store, leaves))
                continue;
            replaced[l] = table_of(_p.at(_p.locals[l].type).element, leaves, _p.locals[l]);
            is_dropped[store] = true;
            any = true;
        }
        if (!any)
            return;
        rewrite(replaced, is_dropped);
    }

private:
    program& _p;
    cc::vector<u32> _block_of;
    /// Each block's position in the order blocks open, which dominance numbers its nodes by.
    cc::vector<u32> _node_of;
    cc::optional<dominance> _dominance;

    void index_blocks()
    {
        _block_of = cc::vector<u32>::create_filled(_p.instructions.size(), k_none);
        _node_of = cc::vector<u32>::create_filled(_p.blocks.size(), k_none);
        auto const order = blocks_in_order(_p);
        auto nodes = u32(0);
        for (auto const b : order)
        {
            auto const& block = _p.blocks[isize(b)];
            if (block.count == 0)
                continue;
            _node_of[isize(b)] = nodes++;
            for (auto i = block.first; i < block.first + block.count; ++i)
                _block_of[i] = u32(b);
        }
        auto predecessors = cc::vector<cc::vector<u32>>::create_defaulted(isize(nodes));
        auto scratch = cc::vector<u32>();
        for (auto const b : order)
        {
            auto const& block = _p.blocks[isize(b)];
            if (block.count == 0)
                continue;
            for (auto const to : successors_of(_p, _p.instructions[block.first + block.count - 1], scratch))
                if (_node_of[isize(to)] != k_none)
                    predecessors[isize(_node_of[isize(to)])].push_back(_node_of[isize(b)]);
        }
        auto first = cc::vector<u32>();
        auto preds = cc::vector<u32>();
        for (auto const& from : predecessors)
        {
            first.push_back(u32(preds.size()));
            for (auto const f : from)
                preds.push_back(f);
        }
        first.push_back(u32(preds.size()));
        _dominance = dominance(first, preds);
    }

    /// Whether `at` runs before `use` on every path: its block dominates, or the two share a block and it comes first.
    [[nodiscard]] bool runs_before(u32 at, u32 use) const
    {
        auto const a = _block_of[at];
        auto const b = _block_of[use];
        if (a == b)
            return at < use;
        return _dominance.value().dominates(_node_of[isize(a)], _node_of[isize(b)]);
    }

    /// The pointer `v` is reached from: a local, a global, or an operation that is no access chain.
    [[nodiscard]] value_ref root_of(value_ref v) const
    {
        while (is_operation(v) && _p.instructions[index_of(v)].code == op::access_chain)
            v = value_ref(_p.operands_of(_p.instructions[index_of(v)])[0]);
        return v;
    }

    [[nodiscard]] bool qualifies(value_ref local, u32& store, cc::vector<u32>& leaves)
    {
        auto reads = cc::vector<u32>();
        for (auto i = u32(0); i < u32(_p.instructions.size()); ++i)
        {
            if (_block_of[i] == k_none)
                continue;
            auto const& inst = _p.instructions[i];
            auto const operands = _p.operands_of(inst);
            for (auto k = isize(0); k < operands.size(); ++k)
            {
                if (operand_role(inst.code, k) != operand::value)
                    continue;
                auto const operand = value_ref(operands[k]);
                if (root_of(operand) != local)
                    continue;
                // the one whole store, a read, or an access chain on the way to one; nothing else may reach it
                if (inst.code == op::store && k == 0 && operand == local && store == k_none)
                    store = i;
                else if ((inst.code == op::load && k == 0) || (inst.code == op::access_chain && k == 0))
                    reads.push_back(i);
                else
                    return false;
            }
        }
        if (store == k_none)
            return false;
        for (auto const r : reads)
            if (!runs_before(store, r))
                return false;
        return constant_leaves(value_ref(_p.operands_of(_p.instructions[store])[1]), leaves) && !leaves.empty()
            && leaves.size() <= k_most_leaves;
    }

    [[nodiscard]] bool constant_leaves(value_ref value, cc::vector<u32>& out)
    {
        if (is_constant(value))
        {
            auto const& c = _p.constants[index_of(value)];
            if (c.is_undefined)
                return false;
            auto const kind = _p.at(c.type).kind;
            if (kind == type_kind::bool_ || kind == type_kind::int_ || kind == type_kind::float_)
            {
                out.push_back(u32(value));
                return true;
            }
            if (!c.is_null)
                return false;
            zero_leaves(c.type, out);
            return true;
        }
        if (!is_operation(value))
            return false;
        auto const& made = _p.instructions[index_of(value)];
        if (made.code != op::construct)
            return false;
        // a construct's leaves are its operands' in order, whichever composite it builds
        for (auto const part : _p.operands_of(made))
            if (!constant_leaves(value_ref(part), out))
                return false;
        return true;
    }

    void zero_leaves(type_ref type, cc::vector<u32>& out)
    {
        auto const& info = _p.at(type);
        switch (info.kind)
        {
        case type_kind::vector:
        case type_kind::matrix:
        case type_kind::array:
            for (auto k = u32(0); k < info.count; ++k)
                zero_leaves(info.element, out);
            return;
        case type_kind::structure:
            for (auto m = u32(0); m < info.member_count; ++m)
                zero_leaves(_p.members[info.first_member + m].type, out);
            return;
        default:
            _p.constants.push_back({.type = type});
            out.push_back(u32(constant_at(u32(_p.constants.size() - 1))));
            return;
        }
    }

    [[nodiscard]] type_ref table_pointer(type_ref element)
    {
        for (auto t = isize(0); t < _p.types.size(); ++t)
        {
            auto const& known = _p.types[t];
            if (known.kind == type_kind::pointer && known.space == storage::table && known.element == element)
                return type_ref(t);
        }
        _p.types.push_back({.kind = type_kind::pointer, .space = storage::table, .element = element});
        return type_ref(_p.types.size() - 1);
    }

    /// Whether two leaves are the same constant, whichever entry of the table states it.
    [[nodiscard]] bool same_leaf(u32 a, u32 b) const
    {
        if (a == b)
            return true;
        auto const& x = _p.constants[index_of(value_ref(a))];
        auto const& y = _p.constants[index_of(value_ref(b))];
        return x.type == y.type && (x.is_null ? 0u : x.low) == (y.is_null ? 0u : y.low)
            && (x.is_null ? 0u : x.high) == (y.is_null ? 0u : y.high);
    }

    /// The table of `element` holding `leaves`, shared with any that holds the same.
    [[nodiscard]] value_ref table_of(type_ref element, cc::span<u32 const> leaves, local const& named)
    {
        auto const pointer = table_pointer(element);
        for (auto g = isize(0); g < _p.globals.size(); ++g)
        {
            auto const& known = _p.globals[g];
            if (known.type != pointer || known.initializer_count != u32(leaves.size()))
                continue;
            auto is_same = true;
            for (auto k = isize(0); k < leaves.size() && is_same; ++k)
                is_same = same_leaf(_p.initializers[isize(known.initializer_first) + k], leaves[k]);
            if (is_same)
                return global_at(u32(g));
        }
        auto const first = u32(_p.initializers.size());
        for (auto const leaf : leaves)
            _p.initializers.push_back(leaf);
        _p.globals.push_back({.type = pointer,
                              .is_read_only = true,
                              .name_first = named.name_first,
                              .name_size = named.name_size,
                              .initializer_first = first,
                              .initializer_count = u32(leaves.size())});
        return global_at(u32(_p.globals.size() - 1));
    }

    /// Reads every replaced local as its table, drops its store, and drops the local, renumbering what follows.
    void rewrite(cc::span<value_ref const> replaced, cc::span<bool const> is_dropped)
    {
        auto local_place = cc::vector<u32>::create_filled(_p.locals.size(), k_none);
        auto kept_locals = cc::vector<local>();
        for (auto l = isize(0); l < _p.locals.size(); ++l)
            if (!is_valid(replaced[l]))
            {
                local_place[l] = u32(kept_locals.size());
                kept_locals.push_back(_p.locals[l]);
            }
        auto const order = blocks_in_order(_p);
        auto place = cc::vector<u32>::create_filled(_p.instructions.size(), 0u);
        auto next = u32(0);
        for (auto const b : order)
        {
            auto const& block = _p.blocks[isize(b)];
            for (auto i = block.first; i < block.first + block.count; ++i)
                if (!is_dropped[i])
                    place[i] = next++;
        }
        auto kept = cc::vector<instruction>::create_with_capacity(isize(next));
        for (auto const b : order)
        {
            auto& block = _p.blocks[isize(b)];
            auto const start = u32(kept.size());
            for (auto i = block.first; i < block.first + block.count; ++i)
            {
                if (is_dropped[i])
                    continue;
                auto inst = _p.instructions[i];
                auto* const operands = _p.operands.data() + inst.first;
                for (auto k = isize(0); k < isize(inst.count); ++k)
                {
                    if (operand_role(inst.code, k) != operand::value)
                        continue;
                    auto const v = value_ref(operands[k]);
                    if (is_operation(v))
                        operands[k] = place[index_of(v)];
                    else if (is_local(v))
                        operands[k] = is_valid(replaced[index_of(v)]) ? u32(replaced[index_of(v)])
                                                                      : u32(local_at(local_place[index_of(v)]));
                }
                // an access chain into a table points into the table's storage
                if (inst.code == op::access_chain)
                {
                    auto const root = value_ref(operands[0]);
                    auto const is_table
                        = is_global(root) && _p.at(_p.globals[index_of(root)].type).space == storage::table;
                    auto const is_chain_into_table
                        = is_operation(root) && _p.at(kept[isize(index_of(root))].type).space == storage::table;
                    if (is_table || is_chain_into_table)
                        inst.type = table_pointer(_p.at(inst.type).element);
                }
                kept.push_back(inst);
            }
            block.first = start;
            block.count = u32(kept.size()) - start;
        }
        _p.instructions = cc::move(kept);
        _p.locals = cc::move(kept_locals);
    }
};
} // namespace

void sgl::ir::state_constant_tables(program& p)
{
    auto t = tabler(p);
    t.run();
}
