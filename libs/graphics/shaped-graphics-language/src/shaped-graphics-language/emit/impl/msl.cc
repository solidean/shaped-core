#include "dialect.hh"

#include <clean-core/string/format.hh>

namespace
{
using namespace sgl;
using namespace sgl::check;
using namespace sgl::emit;
using namespace sgl::emit::impl;

/// The buffer index of the inline constants, in every stage that reads them.
/// It must equal sg's metal `k_inline_constants_buffer_index`, which sgl cannot include.
constexpr auto k_inline_constants_buffer = 4;

class msl_dialect_t final : public dialect
{
public:
    cc::string_view description() const override { return "MSL"; }

    builtins::language language() const override { return builtins::language::msl; }

    // MSL could brace-initialize, which would drop the member names from the text.
    bool has_struct_constructor() const override { return false; }

    bool is_c_like() const override { return true; }
    /// MSL's is a function; whether it terminates or demotes the pixel is for the metal backend's tests to pin.
    [[nodiscard]] cc::string_view discard_statement() const override { return "discard_fragment();"; }

    void write_for_head(cc::string& out, cc::string_view index, cc::string_view first, cc::string_view end) const override
    {
        out.appendf("for (int {} = {}; {} < {}; ++{})", index, first, index, end, index);
    }

    void write_eval(cc::string& out, cc::string_view value) const override { out.appendf("(void)({});", value); }

    /// MSL has threadgroup memory only in a kernel's own scope, and every function is inlined into the kernel.
    void write_workgroup(cc::string& out, planned_workgroup const& w, plan const& p) const override
    {
        out.appendf("threadgroup {} {};", type_text(p, *this, w.type), w.name);
    }
    bool declares_workgroup_in_function() const override { return true; }

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
        {
            // EMIT-129: MSL names each combination as one attribute
            using kind = check::interpolation::kind_t;
            using sampling = check::interpolation::sampling_t;
            auto const& i = member.interpolate;
            if (i.kind == kind::flat)
                return cc::format("user(sgl{}), flat", member.location);
            if (i.kind == kind::perspective && i.sampling == sampling::center)
                return cc::format("user(sgl{})", member.location);
            return cc::format("user(sgl{}), {}_{}", member.location,
                              i.sampling == sampling::centroid ? "centroid"
                              : i.sampling == sampling::sample ? "sample"
                                                               : "center",
                              i.kind == kind::linear ? "no_perspective" : "perspective");
        }
        case struct_role::render_targets:
            switch (member.output)
            {
            case check::pixel_output::depth:
                return "depth(any)";
            case check::pixel_output::depth_greater_equal:
                return "depth(greater)";
            case check::pixel_output::depth_less_equal:
                return "depth(less)";
            case check::pixel_output::sample_mask:
                return "sample_mask";
            case check::pixel_output::color:
                break;
            }
            return cc::format("color({})", member.location);
        // MSL refuses the tessellation stages (EMIT-122), so no struct of theirs reaches it
        case struct_role::patch_constants:
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

    /// MSL declines every group (EMIT-89), so nothing asks for a resource's spelling.
    [[nodiscard]] cc::string resource_text(plan const&, type_id) const override { return {}; }

    /// A Metal buffer is a parameter of the entry point rather than a global, so MSL declines every group (EMIT-89).
    /// Nothing reaches here.
    void write_group(cc::string&, plan const&, planned_constants const*, cc::span<planned_resource const>) const override
    {
    }

    /// A Metal sampler is a parameter of the entry point, which `write_function_head` writes.
    void write_file_sampler(cc::string&, plan const&, planned_sampler const&) const override {}

    void write_declarations(cc::string& out, plan const& p) const override
    {
        out += "#include <metal_stdlib>\nusing namespace metal;\n\n";
        write_enum_constants(out, p, *this);
        write_buffers(out, p, *this);
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
        // Its memory form where MSL's own rule would place a member elsewhere than SGL (memory_form.hh).
        if (c.form.has_value())
            for (auto const& f : c.form.value().fields)
                out.appendf("{}{} {};\n", k_indent, f.type, f.name);
        else
            for (auto const& member : c.members)
                write_member(out, nullptr, member, p);
        out += "};\n\n";
    }

    /// MSL has no global resources, so the inline constants are a parameter, and the body reads them as it reads a global.
    void write_function_head(cc::string& out, plan const& p) const override
    {
        auto list = cc::string();
        if (check::is_valid(p.e.input))
            list = cc::format("{} {} [[stage_in]]", type_text(p, *this, p.e.input), p.locals[0]);
        for (auto i = isize(0); i < p.e.stage_inputs.size(); ++i)
        {
            auto const& spelled = spelling_of(p.e.stage_inputs[i].input);
            list += cc::format("{}{} {} [[{}]]", list.empty() ? "" : ", ", spelled.msl_type, p.stage_input_names[i],
                               spelled.msl_attribute);
        }
        if (p.constants.has_value())
        {
            auto const& c = p.constants.value();
            list += cc::format("{}constant {}& {} [[buffer({})]]", list.empty() ? "" : ", ", c.block_name, c.name,
                               k_inline_constants_buffer);
        }
        // EMIT-133: a static sampler's slot of the argument table is its index, and MSL has one sampler type
        for (auto const& s : p.samplers)
            list += cc::format("{}sampler {} [[sampler({})]]", list.empty() ? "" : ", ", s.name, s.index);
        out.appendf("{} {} {}({})\n{{\n", p.e.entry_stage == stage::vertex ? "vertex" : "fragment",
                    type_text(p, *this, p.e.result), p.entry_name, list);
        for (auto i = isize(0); i < p.e.stage_inputs.size(); ++i)
        {
            auto const local = p.e.stage_inputs[i].local;
            out.appendf("    const {} {} = {};\n", type_text(p, *this, p.e.at(local).type), p.locals[index_of(local)],
                        stage_input_value(p, i));
        }
    }
};

constexpr auto k_msl = msl_dialect_t();
} // namespace

sgl::emit::impl::dialect const& sgl::emit::impl::msl_dialect()
{
    return k_msl;
}
