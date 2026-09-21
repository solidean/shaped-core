#include "dialect.hh"

#include <clean-core/string/format.hh>

namespace
{
using namespace sgl;
using namespace sgl::check;
using namespace sgl::emit;
using namespace sgl::emit::impl;

/// Where sg's WebGPU backend expects the inline constants: the group it keeps for itself, and that group's first binding.
constexpr auto k_inline_constants_group = 3;
constexpr auto k_inline_constants_binding = 0;

class wgsl_dialect_t final : public dialect
{
public:
    cc::string_view description() const override { return "WGSL"; }

    builtins::language language() const override { return builtins::language::wgsl; }

    bool has_struct_constructor() const override { return true; }

    bool is_c_like() const override { return false; }

    void write_for_head(cc::string& out, cc::string_view index, cc::string_view first, cc::string_view end) const override
    {
        out.appendf("for (var {}: i32 = {}; {} < {}; {}++)", index, first, index, end, index);
    }

    void write_eval(cc::string& out, cc::string_view value) const override { out.appendf("_ = {};", value); }

    void write_local(cc::string& out, local_declaration const& local) const override
    {
        auto const keyword = local.is_mut ? "var" : "let";
        if (local.value.empty())
            out.appendf("var {}: {};", local.name, local.type);
        else
            out.appendf("{} {}: {} = {};", keyword, local.name, local.type, local.value);
    }

    void write_members(cc::string& out, cc::span<planned_member const> members, plan const& p) const
    {
        for (auto const& member : members)
        {
            out += k_indent;
            if (member.is_position)
                out += "@builtin(position) ";
            else if (member.location >= 0)
                out.appendf("@location({}) ", member.location);
            out.appendf("{}: {},\n", member.name, type_text(p, *this, member.type));
        }
    }

    void write_enum_constant(cc::string& out, cc::string_view name, i32 value) const override
    {
        out.appendf("const {}: i32 = {};\n", name, value);
    }

    void write_group(cc::string& out,
                     plan const& p,
                     planned_constants const* block,
                     cc::span<planned_buffer const> buffers) const override
    {
        if (block != nullptr)
        {
            out.appendf("struct {} {{\n", block->block_name);
            write_members(out, block->members, p);
            out += "}\n\n";
            out.appendf("@group({}) @binding({}) var<uniform> {}: {};\n", block->group, block->slot, block->name,
                        block->block_name);
        }
        for (auto const& b : buffers)
            out.appendf("@group({}) @binding({}) var<storage, {}> {}: array<{}>;\n", b.group, b.slot,
                        b.is_mut ? "read_write" : "read", b.name, type_text(p, *this, b.element));
        out += "\n";
    }

    void write_declarations(cc::string& out, plan const& p) const override
    {
        write_enum_constants(out, p, *this);
        write_buffers(out, p, *this);

        for (auto const& s : p.structs)
        {
            out.appendf("struct {} {{\n", s.name);
            write_members(out, s.members, p);
            out += "}\n\n";
        }

        if (!p.constants.has_value())
            return;
        auto const& c = p.constants.value();
        out.appendf("struct {} {{\n", c.block_name);
        write_members(out, c.members, p);
        out += "}\n\n";
        out.appendf("@group({}) @binding({}) var<uniform> {}: {};\n\n", k_inline_constants_group,
                    k_inline_constants_binding, c.name, c.block_name);
    }

    void write_function_head(cc::string& out, plan const& p) const override
    {
        if (p.e.entry_stage == stage::compute)
        {
            out.appendf("@compute @workgroup_size({}, {}, {})\n", p.e.workgroup[0], p.e.workgroup[1], p.e.workgroup[2]);
            out.appendf("fn {}(@builtin(global_invocation_id) {}_in: vec3u) {{\n", p.entry_name, p.locals[0]);
            // WebGPU reports the id unsigned and SGL has one integer type, so the conversion stands at the top.
            out.appendf("    let {}: vec3i = vec3i({}_in);\n", p.locals[0], p.locals[0]);
            return;
        }
        out.appendf("@{}\nfn {}({}: {}) -> {} {{\n", p.e.entry_stage == stage::vertex ? "vertex" : "fragment",
                    p.entry_name, p.locals[0], type_text(p, *this, p.e.input), type_text(p, *this, p.e.result));
    }
};

constexpr auto k_wgsl = wgsl_dialect_t();
} // namespace

sgl::emit::impl::dialect const& sgl::emit::impl::wgsl_dialect()
{
    return k_wgsl;
}
