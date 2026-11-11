#include "ir.hh"

#include <clean-core/string/format.hh>

using namespace sgl;
using namespace sgl::ir;

namespace
{
/// Parallel to `op`.
constexpr cc::string_view k_names[] = {
    "fadd",
    "fsub",
    "fmul",
    "fdiv",
    "frem",
    "fneg",
    "iadd",
    "isub",
    "imul",
    "sdiv",
    "udiv",
    "srem",
    "urem",
    "ineg",
    "bit_and",
    "bit_or",
    "bit_xor",
    "bit_not",
    "bit_reverse",
    "bit_count",
    "shift_left",
    "shift_right_logical",
    "shift_right_arithmetic",
    "feq",
    "fne",
    "flt",
    "fle",
    "fgt",
    "fge",
    "is_nan",
    "is_inf",
    "ieq",
    "ine",
    "slt",
    "sle",
    "sgt",
    "sge",
    "ult",
    "ule",
    "ugt",
    "uge",
    "logical_and",
    "logical_or",
    "logical_not",
    "logical_eq",
    "logical_ne",
    "any",
    "all",
    "select",
    "f_to_s",
    "f_to_u",
    "s_to_f",
    "u_to_f",
    "f_resize",
    "s_resize",
    "u_resize",
    "bitcast",
    "copy_logical",
    "dot",
    "vector_times_scalar",
    "matrix_times_scalar",
    "matrix_times_vector",
    "matrix_times_matrix",
    "transpose",
    "math",
    "derivative",
    "construct",
    "extract",
    "insert",
    "shuffle",
    "load",
    "store",
    "access_chain",
    "texel_pointer",
    "array_length",
    "stage_load",
    "stage_store",
    "atomic_load",
    "atomic_store",
    "atomic_exchange",
    "atomic_compare_exchange",
    "atomic_iadd",
    "atomic_isub",
    "atomic_smin",
    "atomic_umin",
    "atomic_smax",
    "atomic_umax",
    "atomic_and",
    "atomic_or",
    "atomic_xor",
    "atomic_fadd",
    "control_barrier",
    "discard",
    "emit_vertex",
    "end_strip",
    "set_mesh_counts",
    "write_primitive",
    "trace_ray",
    "execute_callable",
    "report_hit",
    "hit_object_trace",
    "trace_reorder_execute",
    "hit_object_reorder",
    "reorder_thread",
    "hit_object_invoke",
    "acceleration_from_address",
    "ray_query_begin",
    "ray_query_trace",
    "ray_query_proceed",
    "ray_query_commit_triangle",
    "ray_query_commit_procedural",
    "ray_query_abort",
    "ray_query_read",
    "hit_object_read",
    "combine_sampler",
    "sample_lod",
    "sample_implicit",
    "sample_bias",
    "sample_grad",
    "sample_compare",
    "sample_compare_zero",
    "image_fetch",
    "image_gather",
    "image_gather_compare",
    "image_read",
    "image_write",
    "image_size",
    "image_levels",
    "image_samples",
    "subgroup",
    "phi",
    "branch",
    "branch_conditional",
    "switch",
    "dispatch_mesh",
    "ignore_hit",
    "accept_and_end_search",
    "return",
    "unreachable",
};
static_assert(sizeof(k_names) / sizeof(k_names[0]) == sizeof(char[isize(op::unreachable) + 1]));

void append_type(cc::string& out, program const& p, type_ref t)
{
    if (!is_valid(t))
        return;
    auto const& info = p.at(t);
    switch (info.kind)
    {
    case type_kind::void_:
        out += "void";
        break;
    case type_kind::bool_:
        out += "bool";
        break;
    case type_kind::int_:
        out.appendf("{}{}", info.is_signed ? "i" : "u", int(info.width));
        break;
    case type_kind::float_:
        out.appendf("f{}", int(info.width));
        break;
    case type_kind::vector:
        append_type(out, p, info.element);
        out.appendf("x{}", info.count);
        break;
    case type_kind::matrix:
        append_type(out, p, info.element);
        out.appendf("[{}]", info.count);
        break;
    case type_kind::array:
        append_type(out, p, info.element);
        out.appendf("[{}]", info.count);
        break;
    case type_kind::runtime_array:
        append_type(out, p, info.element);
        out += "[]";
        break;
    case type_kind::structure:
        out += p.name_at(info.name_first, info.name_size);
        break;
    case type_kind::pointer:
        out += "*";
        append_type(out, p, info.element);
        break;
    case type_kind::image:
        out += "image";
        break;
    case type_kind::sampler:
        out += "sampler";
        break;
    case type_kind::sampled_image:
        out += "sampled_image";
        break;
    case type_kind::acceleration_structure:
        out += "acceleration_structure";
        break;
    case type_kind::ray_query:
        out += "ray_query";
        break;
    case type_kind::hit_object:
        out += "hit_object";
        break;
    }
}

void append_value(cc::string& out, program const& p, value_ref v)
{
    if (!is_valid(v))
        out += "?";
    else if (is_constant(v))
    {
        auto const& c = p.constants[index_of(v)];
        if (c.is_null)
            out += "null";
        else if (c.is_undefined)
            out += "undef";
        else
            out.appendf("{:#x}", u64(c.low) | u64(c.high) << 32);
    }
    else if (is_global(v))
    {
        auto const& g = p.globals[index_of(v)];
        out.appendf("@{}", p.name_at(g.name_first, g.name_size));
    }
    else if (is_local(v))
    {
        auto const& l = p.locals[index_of(v)];
        out.appendf("${}", p.name_at(l.name_first, l.name_size));
    }
    else
        out.appendf("%{}", u32(v));
}
} // namespace

cc::string_view sgl::ir::op_name(op code)
{
    return k_names[isize(code)];
}

cc::string sgl::ir::type_name(program const& p, type_ref t)
{
    auto out = cc::string();
    append_type(out, p, t);
    return out;
}

sgl::ir::operand sgl::ir::operand_role(op code, isize at)
{
    switch (code)
    {
    case op::extract:
    case op::array_length:
        return at < 1 ? operand::value : operand::literal;
    case op::insert:
    case op::shuffle:
        return at < 2 ? operand::value : operand::literal;
    case op::stage_load:
    case op::stage_store:
        return at == 0 ? operand::literal : operand::value;
    case op::phi:
        return at % 2 == 0 ? operand::value : operand::block;
    case op::branch:
        return operand::block;
    case op::branch_conditional:
        return at == 0 ? operand::value : operand::block;
    case op::switch_:
        return at == 0 ? operand::value : at == 1 || at % 2 == 1 ? operand::block : operand::literal;
    default:
        return operand::value;
    }
}

bool sgl::ir::has_effect(op code)
{
    if (u16(code) >= u16(op::atomic_load) && u16(code) <= u16(op::ray_query_abort))
        return true;
    if (u16(code) >= u16(op::branch))
        return true;
    return code == op::store || code == op::stage_store || code == op::image_write;
}

bool sgl::ir::gives_only_a_value(op code)
{
    // everything from the first operation to `shuffle` is arithmetic, comparison, conversion or composite
    return u16(code) <= u16(op::shuffle) || code == op::access_chain || code == op::texel_pointer
        || code == op::stage_load;
}

bool sgl::ir::may_speculate(op code)
{
    // an integer division can trap, where a float's gives a value
    if (code == op::sdiv || code == op::udiv || code == op::srem || code == op::urem)
        return false;
    return gives_only_a_value(code);
}

cc::span<u32 const> sgl::ir::successors_of(program const& p, instruction const& terminator, cc::vector<u32>& scratch)
{
    scratch.clear();
    auto const operands = p.operands_of(terminator);
    for (auto k = isize(0); k < operands.size(); ++k)
        if (operand_role(terminator.code, k) == operand::block)
            scratch.push_back(operands[k]);
    return cc::span<u32 const>(scratch);
}

cc::vector<sgl::ir::block_ref> sgl::ir::blocks_in_order(program const& p)
{
    auto order = cc::vector<block_ref>();
    for (auto i = isize(0); i < p.blocks.size(); ++i)
        order.push_back(block_ref(i));
    // nearly sorted, since most blocks open in the order they were made
    for (auto i = isize(1); i < order.size(); ++i)
    {
        auto const moved = order[i];
        auto at = i;
        while (at > 0 && p.blocks[isize(order[at - 1])].first > p.blocks[isize(moved)].first)
        {
            order[at] = order[at - 1];
            --at;
        }
        order[at] = moved;
    }
    return order;
}

cc::string sgl::ir::dump(program const& p)
{
    auto out = cc::string();
    out.appendf("entry {} [{} {} {}]\n", p.entry_name, p.workgroup[0], p.workgroup[1], p.workgroup[2]);
    for (auto e = isize(0); e < p.stage_elements.size(); ++e)
    {
        auto const& element = p.stage_elements[e];
        out.appendf("element e{}: ", e);
        append_type(out, p, element.type);
        out.appendf(" {}\n", p.name_at(element.name_first, element.name_size));
    }
    for (auto b = isize(0); b < p.blocks.size(); ++b)
    {
        auto const& block = p.blocks[b];
        out.appendf("b{}:", b);
        if (block.merges == merge_kind::selection)
            out.appendf(" merge b{}", u32(block.merge));
        if (block.merges == merge_kind::loop)
            out.appendf(" merge b{} continue b{}", u32(block.merge), u32(block.continue_to));
        out += "\n";
        for (auto i = block.first; i < block.first + block.count; ++i)
        {
            auto const& instruction = p.instructions[i];
            out += "    ";
            if (is_valid(instruction.type) && instruction.type != type_ref(0))
            {
                out.appendf("%{}: ", i);
                append_type(out, p, instruction.type);
                out += " = ";
            }
            out += k_names[isize(instruction.code)];
            if (instruction.code == op::math || instruction.code == op::subgroup
                || instruction.code == op::control_barrier)
                out.appendf(".{}", int(instruction.sub));
            auto const operands = p.operands_of(instruction);
            for (auto k = isize(0); k < operands.size(); ++k)
            {
                out += " ";
                if (k == 0 && (instruction.code == op::stage_load || instruction.code == op::stage_store))
                    out.appendf("e{}", operands[k]);
                else
                    append_value(out, p, value_ref(operands[k]));
            }
            out += "\n";
        }
    }
    return out;
}
