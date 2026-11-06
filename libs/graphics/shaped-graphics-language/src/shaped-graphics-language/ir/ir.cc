#include "ir.hh"

#include <clean-core/string/format.hh>

using namespace sgl;
using namespace sgl::ir;

namespace
{
/// Parallel to `op`.
constexpr cc::string_view k_names[] = {
    "fadd", "fsub", "fmul", "fdiv", "frem", "fneg",
    "iadd", "isub", "imul", "sdiv", "udiv", "srem",
    "urem", "ineg", "bit_and", "bit_or", "bit_xor", "bit_not",
    "bit_reverse", "bit_count", "shift_left", "shift_right_logical", "shift_right_arithmetic", "feq",
    "fne", "flt", "fle", "fgt", "fge", "is_nan",
    "is_inf", "ieq", "ine", "slt", "sle", "sgt",
    "sge", "ult", "ule", "ugt", "uge", "logical_and",
    "logical_or", "logical_not", "logical_eq", "logical_ne", "any", "all",
    "select", "f_to_s", "f_to_u", "s_to_f", "u_to_f", "f_resize",
    "s_resize", "u_resize", "bitcast", "copy_logical", "dot", "vector_times_scalar",
    "matrix_times_scalar", "matrix_times_vector", "matrix_times_matrix", "transpose", "math", "construct",
    "extract", "insert", "shuffle", "load", "store", "access_chain",
    "array_length", "atomic_load", "atomic_store", "atomic_exchange", "atomic_compare_exchange", "atomic_iadd",
    "atomic_isub", "atomic_smin", "atomic_umin", "atomic_smax", "atomic_umax", "atomic_and",
    "atomic_or", "atomic_xor", "atomic_fadd", "control_barrier", "combine_sampler", "sample_lod",
    "image_fetch", "image_gather", "image_read", "image_write", "image_size", "image_levels",
    "image_samples", "subgroup", "phi", "branch", "branch_conditional", "switch",
    "return", "unreachable",
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

cc::string sgl::ir::dump(program const& p)
{
    auto out = cc::string();
    out.appendf("entry {} [{} {} {}]\n", p.entry_name, p.workgroup[0], p.workgroup[1], p.workgroup[2]);
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
            if (instruction.code == op::math || instruction.code == op::subgroup || instruction.code == op::control_barrier)
                out.appendf(".{}", int(instruction.sub));
            for (auto const raw : p.operands_of(instruction))
            {
                out += " ";
                append_value(out, p, value_ref(raw));
            }
            out += "\n";
        }
    }
    return out;
}
