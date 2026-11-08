#include "metrics.hh"

#include <babel-data/shader/spirv.hh>
#include <clean-core/container/map.hh>
#include <clean-core/record/scope.hh>
#include <clean-core/string/format.hh>

// One walk of the module for its names, types and constants, then one function at a time: its blocks, the values its
// instructions give, liveness over its control flow, and a walk of each block for the live words and the chains.

namespace shan
{
namespace
{
namespace op
{
constexpr u16 ext_inst_import = 11;
constexpr u16 ext_inst = 12;
constexpr u16 name = 5;
constexpr u16 entry_point = 15;
constexpr u16 type_bool = 20;
constexpr u16 type_int = 21;
constexpr u16 type_float = 22;
constexpr u16 type_vector = 23;
constexpr u16 type_matrix = 24;
constexpr u16 type_array = 28;
constexpr u16 type_struct = 30;
constexpr u16 type_pointer = 32;
constexpr u16 constant = 43;
constexpr u16 function = 54;
constexpr u16 function_parameter = 55;
constexpr u16 function_end = 56;
constexpr u16 function_call = 57;
constexpr u16 variable = 59;
constexpr u16 load = 61;
constexpr u16 store = 62;
constexpr u16 vector_shuffle = 79;
constexpr u16 composite_extract = 81;
constexpr u16 composite_insert = 82;
constexpr u16 line = 8;
constexpr u16 no_line = 317;
constexpr u16 phi = 245;
constexpr u16 loop_merge = 246;
constexpr u16 selection_merge = 247;
constexpr u16 label = 248;
constexpr u16 branch = 249;
constexpr u16 branch_conditional = 250;
constexpr u16 switch_ = 251;
constexpr u16 kill = 252;
constexpr u16 return_ = 253;
constexpr u16 return_value = 254;
constexpr u16 unreachable = 255;
constexpr u16 terminate_invocation = 4416;
} // namespace op

[[nodiscard]] bool is_terminator(u16 code)
{
    return code == op::branch || code == op::branch_conditional || code == op::switch_ || code == op::kill
        || code == op::return_ || code == op::return_value || code == op::unreachable
        || code == op::terminate_invocation;
}

/// What the module states once for every function: names, the bits of each type, constants and the debug sets.
struct module_facts
{
    cc::map<u32, cc::string> names;
    cc::map<u32, u32> type_bits;
    cc::map<u32, u32> pointee;
    cc::map<u32, u64> constants;
    /// Extended instruction sets that only describe, `NonSemantic.*`.
    cc::vector<u32> describing_sets;

    [[nodiscard]] i32 words_of(u32 type) const
    {
        auto const* const bits = type_bits.get_ptr(type);
        return bits == nullptr ? 0 : i32((*bits + 31) / 32);
    }
    [[nodiscard]] bool is_describing(u32 set) const
    {
        for (auto const s : describing_sets)
            if (s == set)
                return true;
        return false;
    }
};

[[nodiscard]] module_facts facts_of(babel::spirv::module const& m)
{
    auto f = module_facts();
    auto const bits_of = [&](u32 type) -> u32
    {
        auto const* const bits = f.type_bits.get_ptr(type);
        return bits == nullptr ? 0 : *bits;
    };
    for (auto k = isize(0); k < m.instruction_count(); ++k)
    {
        auto const i = m.at(k);
        auto const w = i.words;
        switch (i.opcode())
        {
        case op::name:
            f.names[w[1]] = cc::string(i.string_at(1));
            break;
        case op::ext_inst_import:
            if (i.string_at(1).starts_with("NonSemantic."))
                f.describing_sets.push_back(w[1]);
            break;
        case op::type_bool:
            f.type_bits[w[1]] = 32;
            break;
        case op::type_int:
        case op::type_float:
            f.type_bits[w[1]] = w[2];
            break;
        case op::type_vector:
        case op::type_matrix:
            f.type_bits[w[1]] = bits_of(w[2]) * w[3];
            break;
        case op::type_array:
        {
            auto const* const length = f.constants.get_ptr(w[3]);
            f.type_bits[w[1]] = bits_of(w[2]) * u32(length == nullptr ? 1 : *length);
            break;
        }
        case op::type_struct:
        {
            auto sum = u32(0);
            for (auto at = isize(2); at < w.size(); ++at)
                sum += bits_of(w[at]);
            f.type_bits[w[1]] = sum;
            break;
        }
        case op::type_pointer:
            // a logical pointer is an address a driver folds; it holds no register of its own
            f.type_bits[w[1]] = 0;
            f.pointee[w[1]] = w[3];
            break;
        case op::constant:
            if (w.size() >= 4)
                f.constants[w[2]] = w[3] | (w.size() >= 5 ? u64(w[4]) << 32 : 0);
            break;
        default:
            break;
        }
    }
    return f;
}

/// The operand words of `i` that may name a value: every operand after the result, but the literals its opcode takes.
void value_operands(babel::spirv::instruction const& i, cc::vector<u32>& out)
{
    out.clear();
    auto const args = i.arguments();
    auto const take = [&](isize from, isize to)
    {
        for (auto k = from; k < to && k < args.size(); ++k)
            out.push_back(args[k]);
    };
    switch (i.opcode())
    {
    case op::composite_extract:
        take(0, 1);
        return;
    case op::composite_insert:
        take(0, 2);
        return;
    case op::vector_shuffle:
        take(0, 2);
        return;
    case op::ext_inst:
        // the set, then the instruction's number, then its operands
        take(2, args.size());
        return;
    case op::load:
        take(0, 1);
        return;
    case op::store:
        take(0, 2);
        return;
    case op::loop_merge:
    case op::selection_merge:
        return;
    case op::switch_:
        take(0, 1);
        return;
    case op::branch_conditional:
        take(0, 1);
        return;
    case op::branch:
        return;
    case op::phi:
        for (auto k = isize(0); k + 1 < args.size(); k += 2)
            out.push_back(args[k]);
        return;
    default:
        take(0, args.size());
        return;
    }
}

/// The blocks a terminator reaches, by label id.
void successors(babel::spirv::instruction const& i, cc::vector<u32>& out)
{
    out.clear();
    auto const args = i.arguments();
    switch (i.opcode())
    {
    case op::branch:
        out.push_back(args[0]);
        return;
    case op::branch_conditional:
        out.push_back(args[1]);
        out.push_back(args[2]);
        return;
    case op::switch_:
        // a 32-bit selector's cases: a literal and a label each
        out.push_back(args[1]);
        for (auto k = isize(2); k + 1 < args.size(); k += 2)
            out.push_back(args[k + 1]);
        return;
    default:
        return;
    }
}

struct block_range
{
    u32 label = 0;
    isize first = 0;
    isize end = 0;
};

class function_walk
{
public:
    function_walk(babel::spirv::module const& m, module_facts const& facts) : _m(m), _facts(facts) {}

    cc::result<function_metrics> run(isize first, isize end)
    {
        auto f = function_metrics();
        auto const head = _m.at(first);
        f.id = head.result_id();
        auto const* const named = _facts.names.get_ptr(f.id);
        f.name = named != nullptr ? *named : cc::format("%{}", f.id);

        // the function's values, numbered densely, and its blocks
        for (auto k = first; k < end; ++k)
        {
            auto const i = _m.at(k);
            if (i.opcode() == op::label)
            {
                if (!_blocks.empty())
                    _blocks.back().end = k;
                _block_of_label[i.result_id()] = i32(_blocks.size());
                _blocks.push_back({.label = i.result_id(), .first = k + 1, .end = end});
                continue;
            }
            if (i.result_id() != 0 && i.result_type() != 0)
            {
                _value_of[i.result_id()] = i32(_widths.size());
                _widths.push_back(_facts.words_of(i.result_type()));
            }
        }
        if (!_blocks.empty())
            _blocks.back().end = end - 1; // OpFunctionEnd
        f.blocks = i32(_blocks.size());
        auto const words = (_widths.size() + 63) / 64;

        // per block: what it defines, what it reads before defining, what it hands each phi of a successor
        auto defs = cc::vector<cc::vector<u64>>();
        auto uses = cc::vector<cc::vector<u64>>();
        auto phi_out = cc::vector<cc::vector<u64>>();
        auto succ = cc::vector<cc::vector<i32>>();
        for (auto const& b : _blocks)
        {
            (void)b;
            defs.push_back(cc::vector<u64>::create_filled(words, 0));
            uses.push_back(cc::vector<u64>::create_filled(words, 0));
            phi_out.push_back(cc::vector<u64>::create_filled(words, 0));
            succ.push_back({});
        }
        auto operands = cc::vector<u32>();
        auto targets = cc::vector<u32>();
        for (auto b = isize(0); b < _blocks.size(); ++b)
        {
            auto const& block = _blocks[b];
            auto has_terminator = false;
            for (auto k = block.first; k < block.end; ++k)
            {
                auto const i = _m.at(k);
                auto const code = i.opcode();
                count(f, i);
                if (code == op::phi)
                {
                    // a phi's value is read at the end of the block it names
                    auto const args = i.arguments();
                    for (auto a = isize(0); a + 1 < args.size(); a += 2)
                        if (auto const* const from = _block_of_label.get_ptr(args[a + 1]))
                            if (auto const* const v = _value_of.get_ptr(args[a]))
                                set(phi_out[*from], *v);
                }
                else
                {
                    value_operands(i, operands);
                    for (auto const o : operands)
                        if (auto const* const v = _value_of.get_ptr(o); v != nullptr && !test(defs[b], *v))
                            set(uses[b], *v);
                }
                if (auto const* const v = _value_of.get_ptr(i.result_id()); v != nullptr && i.result_type() != 0)
                    set(defs[b], *v);
                if (is_terminator(code))
                {
                    has_terminator = true;
                    successors(i, targets);
                    for (auto const t : targets)
                        if (auto const* const to = _block_of_label.get_ptr(t))
                            succ[b].push_back(*to);
                }
            }
            if (!has_terminator)
                return cc::error(cc::format("shan: block %{} of {} has no terminator", block.label, f.name));
        }

        // liveness, to a fixed point: what a block hands on is what its successors read, and its phis' values
        auto live_in = cc::vector<cc::vector<u64>>();
        auto live_out = cc::vector<cc::vector<u64>>();
        for (auto b = isize(0); b < _blocks.size(); ++b)
        {
            live_in.push_back(cc::vector<u64>::create_filled(words, 0));
            live_out.push_back(cc::vector<u64>::create_filled(words, 0));
        }
        for (auto changed = true; changed;)
        {
            changed = false;
            for (auto b = _blocks.size(); b > 0; --b)
            {
                auto const at = b - 1;
                for (auto w = isize(0); w < words; ++w)
                {
                    auto out = phi_out[at][w];
                    for (auto const s : succ[at])
                        out |= live_in[s][w];
                    auto const in = uses[at][w] | (out & ~defs[at][w]);
                    changed = changed || out != live_out[at][w] || in != live_in[at][w];
                    live_out[at][w] = out;
                    live_in[at][w] = in;
                }
            }
        }

        // each block backwards from what it hands on: the live words at every point, and forwards: its chains
        auto live = cc::vector<u64>::create_filled(words, 0);
        auto depth = cc::vector<i32>::create_filled(_widths.size(), 0);
        for (auto b = isize(0); b < _blocks.size(); ++b)
        {
            auto const& block = _blocks[b];
            live = cc::vector<u64>::create_copy_of(cc::span<u64 const>(live_out[b]));
            auto words_live = weight(live);
            f.max_live_words = cc::max(f.max_live_words, words_live);
            for (auto k = block.end; k > block.first; --k)
            {
                auto const i = _m.at(k - 1);
                if (auto const* const v = _value_of.get_ptr(i.result_id()); v != nullptr && test(live, *v))
                {
                    clear(live, *v);
                    words_live -= _widths[*v];
                }
                if (i.opcode() != op::phi)
                {
                    value_operands(i, operands);
                    for (auto const o : operands)
                        if (auto const* const v = _value_of.get_ptr(o); v != nullptr && !test(live, *v))
                        {
                            set(live, *v);
                            words_live += _widths[*v];
                        }
                }
                f.max_live_words = cc::max(f.max_live_words, words_live);
            }
            for (auto k = block.first; k < block.end; ++k)
            {
                auto const i = _m.at(k);
                auto const kind = kind_of(i);
                auto longest = i32(0);
                if (i.opcode() != op::phi)
                {
                    value_operands(i, operands);
                    for (auto const o : operands)
                        if (auto const* const v = _value_of.get_ptr(o); v != nullptr && defined_in(b, o))
                            longest = cc::max(longest, depth[*v]);
                }
                auto const reach = longest + latency_of(kind);
                f.critical_path = cc::max(f.critical_path, reach);
                if (auto const* const v = _value_of.get_ptr(i.result_id()))
                    depth[*v] = reach;
            }
        }
        return f;
    }

private:
    babel::spirv::module const& _m;
    module_facts const& _facts;
    cc::vector<block_range> _blocks;
    cc::map<u32, i32> _block_of_label;
    cc::map<u32, i32> _value_of;
    cc::vector<i32> _widths;

    [[nodiscard]] static bool test(cc::span<u64 const> bits, i32 v) { return (bits[v / 64] >> (v % 64)) & 1u; }
    static void set(cc::span<u64> bits, i32 v) { bits[v / 64] |= u64(1) << (v % 64); }
    static void clear(cc::span<u64> bits, i32 v) { bits[v / 64] &= ~(u64(1) << (v % 64)); }

    [[nodiscard]] i32 weight(cc::span<u64 const> bits) const
    {
        auto sum = i32(0);
        for (auto v = isize(0); v < _widths.size(); ++v)
            if (test(bits, i32(v)))
                sum += _widths[v];
        return sum;
    }

    [[nodiscard]] bool defined_in(isize b, u32 id) const
    {
        auto const& block = _blocks[b];
        for (auto k = block.first; k < block.end; ++k)
            if (_m.at(k).result_id() == id)
                return true;
        return false;
    }

    [[nodiscard]] op_kind kind_of(babel::spirv::instruction const& i) const
    {
        if (i.opcode() == op::ext_inst && i.words.size() > 3 && _facts.is_describing(i.words[3]))
            return op_kind::declaration;
        return spirv_kind_of(i.opcode());
    }

    void count(function_metrics& f, babel::spirv::instruction const& i) const
    {
        auto const code = i.opcode();
        if (code == op::line || code == op::no_line)
            return;
        if (code == op::loop_merge)
            ++f.loops;
        if (code == op::selection_merge)
            ++f.selections;
        if (code == op::function_call)
            ++f.calls;
        if (code == op::variable)
        {
            auto const* const pointee = _facts.pointee.get_ptr(i.result_type());
            f.variable_words += pointee != nullptr ? _facts.words_of(*pointee) : 0;
        }
        auto const kind = kind_of(i);
        if (kind == op_kind::declaration || code == op::loop_merge || code == op::selection_merge)
            return;
        ++f.kinds[isize(kind)];
        ++f.instructions;
    }
};
} // namespace

cc::result<module_metrics> analyze(babel::spirv::module const& m)
{
    CC_RECORD_SCOPE("shan.analyze");
    auto const facts = facts_of(m);
    auto result = module_metrics();
    result.total.name = "total";
    for (auto k = isize(0); k < m.instruction_count(); ++k)
    {
        auto const i = m.at(k);
        if (i.opcode() == op::entry_point)
            result.entry_points.push_back(cc::string(i.string_at(2)));
        if (i.opcode() != op::function)
            continue;
        auto end = k + 1;
        while (end < m.instruction_count() && m.at(end).opcode() != op::function_end)
            ++end;
        if (end == m.instruction_count())
            return cc::error(cc::format("shan: function %{} has no OpFunctionEnd", i.result_id()));
        auto walk = function_walk(m, facts);
        auto f = walk.run(k, end + 1);
        if (f.has_error())
            return cc::error(f.error().to_string());
        auto& t = result.total;
        auto const& g = f.value();
        t.instructions += g.instructions;
        for (auto kind = isize(0); kind < k_op_kind_count; ++kind)
            t.kinds[kind] += g.kinds[kind];
        t.blocks += g.blocks;
        t.loops += g.loops;
        t.selections += g.selections;
        t.calls += g.calls;
        t.variable_words += g.variable_words;
        t.max_live_words = cc::max(t.max_live_words, g.max_live_words);
        t.critical_path = cc::max(t.critical_path, g.critical_path);
        result.functions.push_back(g);
        k = end;
    }
    return result;
}

cc::string to_text(module_metrics const& m)
{
    auto out = cc::string();
    auto const write = [&](function_metrics const& f)
    {
        out.appendf("{}: {} instructions, {} blocks, {} loops, {} selections, {} calls\n", f.name, f.instructions,
                    f.blocks, f.loops, f.selections, f.calls);
        out.appendf("  max live {} words, critical path {}, variables {} words\n", f.max_live_words, f.critical_path,
                    f.variable_words);
        for (auto k = isize(0); k < k_op_kind_count; ++k)
            if (f.kinds[k] != 0)
                out.appendf("  {} {}\n", name_of(op_kind(k)), f.kinds[k]);
    };
    write(m.total);
    for (auto const& f : m.functions)
        write(f);
    return out;
}
} // namespace shan
