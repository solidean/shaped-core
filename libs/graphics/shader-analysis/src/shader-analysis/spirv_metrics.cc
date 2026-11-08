#include "cost.hh"
#include "impl/graph.hh"
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
constexpr u16 decorate = 71;
constexpr u16 iadd = 128;
constexpr u16 isub = 130;
constexpr u16 iequal = 170;
constexpr u16 inot_equal = 171;
constexpr u16 ugreater_than = 172;
constexpr u16 sgreater_than = 173;
constexpr u16 ugreater_than_equal = 174;
constexpr u16 sgreater_than_equal = 175;
constexpr u16 uless_than = 176;
constexpr u16 sless_than = 177;
constexpr u16 uless_than_equal = 178;
constexpr u16 sless_than_equal = 179;
constexpr u16 access_chain = 65;
constexpr u16 in_bounds_access_chain = 66;
constexpr u16 ptr_access_chain = 67;
constexpr u16 in_bounds_ptr_access_chain = 70;
constexpr u16 group_non_uniform_elect = 333;
constexpr u16 group_non_uniform_all = 334;
constexpr u16 group_non_uniform_ballot = 339;
constexpr u16 group_non_uniform_inverse_ballot = 340;
constexpr u16 group_non_uniform_ballot_bit_count = 342;
constexpr u16 group_non_uniform_shuffle = 345;
constexpr u16 group_non_uniform_shuffle_relative_down = 348;
constexpr u16 group_non_uniform_iadd = 349;
constexpr u16 group_non_uniform_logical_xor = 364;
constexpr u16 subgroup_first_invocation_khr = 4422;
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
    /// The module's variables whose loads differ across a wave: its inputs but the builtins a workgroup shares.
    cc::vector<u32> varying_variables;

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
    [[nodiscard]] bool is_varying_variable(u32 id) const
    {
        for (auto const v : varying_variables)
            if (v == id)
                return true;
        return false;
    }
};

[[nodiscard]] module_facts facts_of(babel::spirv::module const& m)
{
    auto f = module_facts();
    auto builtins = cc::map<u32, u32>();
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
        case op::decorate:
            // BuiltIn
            if (w.size() >= 4 && w[2] == 11)
                builtins[w[1]] = w[3];
            break;
        case op::variable:
        {
            // a module's Input variable, unless it is a builtin a wave shares: NumWorkgroups, WorkgroupSize,
            // WorkgroupId, SubgroupSize, NumSubgroups, SubgroupId
            if (w.size() < 4 || w[3] != 1)
                break;
            auto const* const builtin = builtins.get_ptr(w[2]);
            auto const shared = builtin != nullptr
                             && (*builtin == 24 || *builtin == 25 || *builtin == 26 || *builtin == 36 || *builtin == 38
                                 || *builtin == 40);
            if (!shared)
                f.varying_variables.push_back(w[2]);
            break;
        }
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

/// Whether an instruction's result differs across a wave by what it is rather than by what it reads.
[[nodiscard]] impl::uniformity_source source_of(babel::spirv::instruction const& i)
{
    auto const code = i.opcode();
    auto const kind = spirv_kind_of(code);
    if (code == op::group_non_uniform_elect || code == op::group_non_uniform_inverse_ballot
        || (code >= op::group_non_uniform_shuffle && code <= op::group_non_uniform_shuffle_relative_down)
        || kind == op_kind::atomic || kind == op_kind::derivative)
        return impl::uniformity_source::varying;
    // all, any, all-equal, broadcast, broadcast-first and ballot answer for the whole wave
    if ((code >= op::group_non_uniform_all && code <= op::group_non_uniform_ballot)
        || code == op::subgroup_first_invocation_khr)
        return impl::uniformity_source::uniform;
    // a bit count or an arithmetic group operation: a reduction is the wave's, a scan each lane's
    if (code == op::group_non_uniform_ballot_bit_count
        || (code >= op::group_non_uniform_iadd && code <= op::group_non_uniform_logical_xor))
    {
        auto const args = i.arguments();
        return args.size() >= 2 && args[1] == 0 ? impl::uniformity_source::uniform : impl::uniformity_source::varying;
    }
    return impl::uniformity_source::operands;
}

/// Whether the integer comparison `code` holds of `a` and `b`; nullopt where `code` is no integer comparison.
[[nodiscard]] cc::optional<bool> compares(u16 code, i64 a, i64 b)
{
    switch (code)
    {
    case op::iequal:
        return a == b;
    case op::inot_equal:
        return a != b;
    case op::ugreater_than:
    case op::sgreater_than:
        return a > b;
    case op::ugreater_than_equal:
    case op::sgreater_than_equal:
        return a >= b;
    case op::uless_than:
    case op::sless_than:
        return a < b;
    case op::uless_than_equal:
    case op::sless_than_equal:
        return a <= b;
    default:
        return {};
    }
}

/// The trips of the loop a header at `header` heads, where a counter of its phis starts at a constant, steps by one,
/// and a branch leaves the loop on comparing it, or its next value, with a constant; 0 where it is anything else.
[[nodiscard]] f64 trips_of(babel::spirv::module const& m,
                           module_facts const& facts,
                           cc::map<u32, isize> const& defined_at,
                           isize header,
                           u32 merge_label)
{
    auto const constant = [&](u32 id, i64& out)
    {
        auto const* const c = facts.constants.get_ptr(id);
        if (c == nullptr)
            return false;
        out = i64(i32(u32(*c)));
        return true;
    };
    auto const at = [&](u32 id) -> cc::optional<babel::spirv::instruction>
    {
        auto const* const k = defined_at.get_ptr(id);
        if (k == nullptr)
            return {};
        return m.at(*k);
    };
    constexpr auto k_most_trips = 4096;
    for (auto k = header + 1; k < m.instruction_count() && m.at(k).opcode() == op::phi; ++k)
    {
        auto const phi = m.at(k);
        auto const args = phi.arguments();
        if (args.size() != 4)
            continue;
        // the start, a constant, and the next value, the phi plus or minus a constant
        for (auto way = 0; way < 2; ++way)
        {
            auto start = i64(0);
            if (!constant(args[2 * way], start))
                continue;
            auto const next = at(args[2 * (1 - way)]);
            if (!next.has_value() || (next.value().opcode() != op::iadd && next.value().opcode() != op::isub))
                continue;
            auto const step_args = next.value().arguments();
            auto step = i64(0);
            if (step_args[0] != phi.result_id() || !constant(step_args[1], step) || step == 0)
                continue;
            if (next.value().opcode() == op::isub)
                step = -step;
            // a compare of the phi or its next value with a constant, which a branch leaving the loop decides on
            for (auto c = header; c < m.instruction_count() && m.at(c).opcode() != op::function_end; ++c)
            {
                auto const branch = m.at(c);
                if (branch.opcode() != op::branch_conditional)
                    continue;
                auto const b = branch.arguments();
                auto const leaves_on = b[1] == merge_label ? 1 : b[2] == merge_label ? 0 : -1;
                if (leaves_on < 0)
                    continue;
                auto const test = at(b[0]);
                if (!test.has_value())
                    continue;
                auto const t = test.value().arguments();
                auto bound = i64(0);
                auto const of_next = t[0] == next.value().result_id();
                if ((t[0] != phi.result_id() && !of_next) || !constant(t[1], bound))
                    continue;
                auto trips = 0;
                for (auto i = start; trips < k_most_trips; i += step)
                {
                    auto const holds = compares(test.value().opcode(), of_next ? i + step : i, bound);
                    if (!holds.has_value())
                        return 0;
                    // a test of the next value runs after the body, so the body has run once more
                    if (holds.value() == (leaves_on == 1))
                        return f64(of_next ? trips + 1 : trips);
                    ++trips;
                }
                return 0;
            }
        }
    }
    return 0;
}

/// What one lane of a GLSL.std.450 function's result spends, beside its operands' lanes where it reduces them.
struct spend
{
    cost_class cost = cost_class::alu;
    f32 factor = 1;
    cost_class extra = cost_class::free;
    f32 extra_factor = 0;
};

/// A GLSL.std.450 instruction's spend per lane of its result; `width` is its first operand's lanes, which a length or a
/// distance reduces to one.
[[nodiscard]] spend glsl_spend(u32 number, f32 width)
{
    using enum cost_class;
    switch (number)
    {
    case 13: // Sin
    case 14: // Cos
        return {.cost = transcendental, .extra = alu, .extra_factor = 2};
    case 29: // Exp2
    case 30: // Log2
    case 31: // Sqrt
    case 32: // InverseSqrt
        return {.cost = transcendental};
    case 27: // Exp
    case 28: // Log
        return {.cost = transcendental, .extra = alu, .extra_factor = 1};
    case 26: // Pow
        return {.cost = transcendental, .factor = 2, .extra = alu, .extra_factor = 1};
    case 15: // Tan
        return {.cost = transcendental, .factor = 3, .extra = alu, .extra_factor = 1};
    case 16: // Asin
    case 17: // Acos
    case 18: // Atan
    case 25: // Atan2
        return {.cost = alu, .factor = 12, .extra = transcendental, .extra_factor = 1};
    case 19: // Sinh
    case 20: // Cosh
    case 21: // Tanh
    case 22: // Asinh
    case 23: // Acosh
    case 24: // Atanh
        return {.cost = transcendental, .factor = 2, .extra = alu, .extra_factor = 4};
    case 33: // Determinant
    case 34: // MatrixInverse
        return {.cost = alu, .factor = 10};
    case 43: // FClamp
    case 44: // UClamp
    case 45: // SClamp
    case 46: // FMix
    case 47: // IMix
    case 51: // Frexp
    case 53: // Ldexp
    case 68: // Cross
    case 70: // FaceForward
    case 81: // NClamp
        return {.cost = alu, .factor = 2};
    case 49: // SmoothStep
        return {.cost = alu, .factor = 4};
    case 66: // Length: a dot of the operand, and a square root
        return {.cost = alu, .factor = width, .extra = transcendental, .extra_factor = 1};
    case 67: // Distance: a difference, its dot, a square root
        return {.cost = alu, .factor = 2 * width, .extra = transcendental, .extra_factor = 1};
    case 69: // Normalize: a dot, its reciprocal square root once, a multiply
        return {.cost = alu, .factor = 2, .extra = transcendental, .extra_factor = 1 / width};
    case 71: // Reflect
        return {.cost = alu, .factor = 3};
    case 72: // Refract
        return {.cost = alu, .factor = 5, .extra = transcendental, .extra_factor = 1 / width};
    default:
        return {};
    }
}

/// What `code` spends per lane of its result, where its kind's own class says too little; nullopt where that is all.
/// `operand_width` is its first operand's lanes, which a dot and a matrix product reduce.
[[nodiscard]] cc::optional<spend> spend_of(babel::spirv::instruction const& i, f32 operand_width, f32 result_width)
{
    using enum cost_class;
    switch (i.opcode())
    {
    case 132: // IMul
        return spend{.cost = int_multiply};
    case 134: // UDiv
    case 135: // SDiv
    case 137: // UMod
    case 138: // SRem
    case 139: // SMod
        return spend{.cost = int_divide};
    case 136: // FDiv: one transcendental, as both GPUs measured it
        return spend{.cost = transcendental};
    case 140: // FRem
    case 141: // FMod
        return spend{.cost = alu, .factor = 3, .extra = transcendental, .extra_factor = 1};
    case 148: // Dot
    case 145: // MatrixTimesVector
    case 144: // VectorTimesMatrix
    case 146: // MatrixTimesMatrix
        return spend{.cost = alu, .factor = result_width > 0 ? operand_width / result_width : operand_width};
    case op::ext_inst:
        return glsl_spend(i.words.size() > 4 ? i.words[4] : 0, operand_width);
    default:
        return {};
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

/// One function as a graph: its blocks, its values numbered densely, each operand that names one of them.
cc::result<function_metrics> walk(babel::spirv::module const& m, module_facts const& facts, isize first, isize end)
{
    auto f = function_metrics();
    auto const head = m.at(first);
    f.id = head.result_id();
    auto const* const named = facts.names.get_ptr(f.id);
    f.name = named != nullptr ? *named : cc::format("%{}", f.id);

    auto g = impl::graph();
    auto value_of = cc::map<u32, i32>();
    auto block_of_label = cc::map<u32, i32>();
    for (auto k = first; k < end; ++k)
    {
        auto const i = m.at(k);
        if (i.opcode() == op::label)
            block_of_label[i.result_id()] = i32(block_of_label.size());
        else if (i.result_id() != 0 && i.result_type() != 0)
        {
            value_of[i.result_id()] = i32(g.widths.size());
            g.widths.push_back(facts.words_of(i.result_type()));
        }
    }
    auto const value = [&](u32 id) -> i32
    {
        auto const* const v = value_of.get_ptr(id);
        return v == nullptr ? -1 : *v;
    };

    // a pointer's variable through its access chains, and the function's own variables
    auto root_of = cc::map<u32, u32>();
    auto own_variables = cc::vector<u32>();
    auto const varying_load = [&](u32 pointer)
    {
        auto const* const root = root_of.get_ptr(pointer);
        auto const at = root == nullptr ? pointer : *root;
        if (facts.is_varying_variable(at))
            return true;
        // what a function's own variable holds is not followed through its stores
        for (auto const v : own_variables)
            if (v == at)
                return true;
        return false;
    };
    auto defined_at = cc::map<u32, isize>();
    for (auto k = first; k < end; ++k)
        if (auto const id = m.at(k).result_id(); id != 0)
            defined_at[id] = k;
    auto block_start = isize(0);
    auto operands = cc::vector<u32>();
    auto targets = cc::vector<u32>();
    auto in_block = false;
    for (auto k = first; k < end; ++k)
    {
        auto const i = m.at(k);
        auto const code = i.opcode();
        if (code == op::label)
        {
            if (in_block)
                return cc::error(cc::format("shan: a block of {} has no terminator", f.name));
            g.blocks.push_back({.first = u32(g.instructions.size()), .end = u32(g.instructions.size())});
            in_block = true;
            block_start = k;
            continue;
        }
        if (!in_block || code == op::line || code == op::no_line)
            continue;
        if (code == op::loop_merge)
        {
            ++f.loops;
            g.blocks.back().trips = trips_of(m, facts, defined_at, block_start, i.arguments()[0]);
        }
        if (code == op::selection_merge)
            ++f.selections;
        if (code == op::function_call)
            ++f.calls;
        if (code == op::variable)
        {
            own_variables.push_back(i.result_id());
            if (auto const* const pointee = facts.pointee.get_ptr(i.result_type()))
                f.variable_words += facts.words_of(*pointee);
        }
        if ((code == op::access_chain || code == op::in_bounds_access_chain || code == op::ptr_access_chain
             || code == op::in_bounds_ptr_access_chain)
            && !i.arguments().empty())
        {
            auto const base = i.arguments()[0];
            auto const* const root = root_of.get_ptr(base);
            root_of[i.result_id()] = root == nullptr ? base : *root;
        }

        auto kind = spirv_kind_of(code);
        if (code == op::ext_inst && i.words.size() > 3 && facts.is_describing(i.words[3]))
            kind = op_kind::declaration;
        auto gi = impl::graph_instruction{
            .kind = kind,
            .result = i.result_type() != 0 ? value(i.result_id()) : -1,
            .first = u32(g.operands.size()),
            .phi_first = u32(g.phi_values.size()),
            .counts = kind != op_kind::declaration && code != op::loop_merge && code != op::selection_merge,
            .source = source_of(i),
        };
        if (kind != op_kind::declaration)
        {
            // the first operand's lanes, which a dot or a length reduces
            auto const args = i.arguments();
            auto const first_arg = code == op::ext_inst ? 2 : 0;
            auto operand_width = 1.0f;
            if (isize(first_arg) < args.size())
                if (auto const v = value(args[first_arg]); v >= 0)
                    operand_width = f32(cc::max(1, g.widths[v]));
            auto const result_width = gi.result >= 0 ? f32(cc::max(1, g.widths[gi.result])) : 1.0f;
            auto s = spend_of(i, operand_width, result_width);
            // a multiply by a power of two is a shift to every driver
            if (code == 132 && s.has_value())
                for (auto const arg : args)
                    if (auto const* const c = facts.constants.get_ptr(arg);
                        c != nullptr && *c != 0 && (*c & (*c - 1)) == 0)
                        s = spend{.cost = cost_class::alu};
            if (s.has_value())
            {
                gi.has_cost = true;
                gi.cost = s.value().cost;
                gi.cost_factor = s.value().factor;
                gi.extra = s.value().extra;
                gi.extra_factor = s.value().extra_factor;
            }
        }
        if (code == op::load && !i.arguments().empty() && varying_load(i.arguments()[0]))
            gi.source = impl::uniformity_source::varying;
        if ((code == op::branch_conditional || code == op::switch_) && !i.arguments().empty())
            g.blocks.back().condition = value(i.arguments()[0]);
        if (code == op::phi)
        {
            auto const args = i.arguments();
            for (auto a = isize(0); a + 1 < args.size(); a += 2)
            {
                g.phi_values.push_back(value(args[a]));
                auto const* const from = block_of_label.get_ptr(args[a + 1]);
                g.phi_blocks.push_back(from == nullptr ? -1 : *from);
            }
            gi.phi_count = u32(g.phi_values.size()) - gi.phi_first;
        }
        else
        {
            value_operands(i, operands);
            for (auto const o : operands)
                if (auto const v = value(o); v >= 0)
                    g.operands.push_back(v);
        }
        gi.count = u32(g.operands.size()) - gi.first;
        g.instructions.push_back(gi);
        g.blocks.back().end = u32(g.instructions.size());
        if (is_terminator(code))
        {
            successors(i, targets);
            for (auto const t : targets)
                if (auto const* const to = block_of_label.get_ptr(t))
                    g.blocks.back().successors.push_back(*to);
            in_block = false;
        }
    }
    if (in_block)
        return cc::error(cc::format("shan: a block of {} has no terminator", f.name));
    impl::measure(g, f);
    return f;
}
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
        auto f = walk(m, facts, k, end + 1);
        if (f.has_error())
            return cc::error(f.error().to_string());
        auto const& g = f.value();
        impl::accumulate(g, result.total);
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
        out.appendf("{}: {} instructions, {} blocks, {} loops ({} counted), {} selections, {} calls\n", f.name,
                    f.instructions, f.blocks, f.loops, f.counted_loops, f.selections, f.calls);
        out.appendf("  max live {} words ({} vector, {} scalar), critical path {}, variables {} words\n",
                    f.max_live_words, f.max_live_vector_words, f.max_live_scalar_words, f.critical_path,
                    f.variable_words);
        out.appendf("  compute cost {:.0f}:", compute_cost(f, default_cost_model()));
        for (auto c = isize(0); c < k_cost_class_count; ++c)
            if (f.executed_class_lanes[c] > 0)
                out.appendf(" {} {:.0f}", name_of(cost_class(c)), f.executed_class_lanes[c]);
        out.append("\n");
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
