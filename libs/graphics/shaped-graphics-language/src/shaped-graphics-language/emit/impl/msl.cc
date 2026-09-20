#include "dialect.hh"

#include <clean-core/string/format.hh>

namespace
{
using namespace sgl;
using namespace sgl::check;
using namespace sgl::emit;
using namespace sgl::emit::impl;

/// The buffer index of the inline constants, in every stage that reads them.
///
/// sg's metal backend binds group N at buffer index N, and its argument table has `sg::max_binding_groups + 1` = 4 slots.
/// So 4 is the first index no binding group can take.
/// The backend does not bind inline constants yet, so this number is a proposal it has to adopt, not one it was read from.
constexpr auto k_inline_constants_buffer = 4;

class msl_dialect_t final : public dialect
{
public:
    cc::string_view description() const override { return "MSL"; }

    builtins::language language() const override { return builtins::language::msl; }

    // MSL could brace-initialize, which would drop the member names from the text.
    bool has_struct_constructor() const override { return false; }

    bool is_c_like() const override { return true; }

    void write_for_head(cc::string& out, cc::string_view index, cc::string_view first, cc::string_view end) const override
    {
        out.appendf("for (int {} = {}; {} < {}; ++{})", index, first, index, end, index);
    }

    void write_eval(cc::string& out, cc::string_view value) const override { out.appendf("(void)({});", value); }

    void write_local(cc::string& out, local_declaration const& local) const override
    {
        if (local.value.empty())
            out.appendf("{} {};", local.type, local.name);
        else
            out.appendf("{}{} {} = {};", local.is_mut ? "" : "const ", local.type, local.name, local.value);
    }

    /// Without the brackets; empty for a member that carries no address.
    cc::string attribute_of(planned_struct const& s, planned_member const& member) const
    {
        if (member.is_position)
            return "position";
        switch (s.role)
        {
        case struct_role::vertex_input:
            return cc::format("attribute({})", member.location);
        case struct_role::stage_link:
            return cc::format("user(sgl{})", member.location);
        case struct_role::render_targets:
            return cc::format("color({})", member.location);
        case struct_role::plain:
            break;
        }
        return "";
    }

    void write_member(cc::string& out, planned_struct const* owner, planned_member const& member, plan const& p) const
    {
        out.appendf("{}{} {}", k_indent, type_text(p, *this, member.type), member.name);
        if (owner != nullptr)
            if (auto const attribute = attribute_of(*owner, member); !attribute.empty())
                out.appendf(" [[{}]]", attribute);
        out += ";\n";
    }

    void write_enum_constant(cc::string& out, cc::string_view name, i32 value) const override
    {
        out.appendf("constant int {} = {};\n", name, value);
    }

    void write_declarations(cc::string& out, plan const& p) const override
    {
        out += "#include <metal_stdlib>\nusing namespace metal;\n\n";
        write_enum_constants(out, p, *this);

        for (auto const& s : p.structs)
        {
            out.appendf("struct {}\n{{\n", s.name);
            for (auto const& member : s.members)
                write_member(out, &s, member, p);
            out += "};\n\n";
        }

        if (!p.constants.has_value())
            return;
        auto const& c = p.constants.value();
        out.appendf("struct {}\n{{\n", c.block_name);
        for (auto const& member : c.members)
            write_member(out, nullptr, member, p);
        out += "};\n\n";
    }

    /// MSL has no global resources, so the inline constants are a parameter, and the body reads them as it reads a global.
    void write_function_head(cc::string& out, plan const& p) const override
    {
        out.appendf("{} {} {}({} {} [[stage_in]]", p.e.entry_stage == stage::vertex ? "vertex" : "fragment",
                    type_text(p, *this, p.e.result), p.e.name, type_text(p, *this, p.e.input), p.locals[0]);
        if (p.constants.has_value())
        {
            auto const& c = p.constants.value();
            out.appendf(", constant {}& {} [[buffer({})]]", c.block_name, c.name, k_inline_constants_buffer);
        }
        out += ")\n{\n";
    }
};

constexpr auto k_msl = msl_dialect_t();
} // namespace

sgl::emit::impl::dialect const& sgl::emit::impl::msl_dialect()
{
    return k_msl;
}
