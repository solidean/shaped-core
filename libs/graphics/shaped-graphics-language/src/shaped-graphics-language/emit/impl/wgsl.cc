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

    void write_declarations(cc::string& out, plan const& p) const override
    {
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
        out.appendf("@{}\nfn {}({}: {}) -> {} {{\n", p.e.entry_stage == stage::vertex ? "vertex" : "fragment", p.e.name,
                    p.locals[0], type_text(p, *this, p.e.input), type_text(p, *this, p.e.result));
    }
};

constexpr auto k_wgsl = wgsl_dialect_t();
} // namespace

sgl::emit::impl::dialect const& sgl::emit::impl::wgsl_dialect()
{
    return k_wgsl;
}
