#include "dialect.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/resources.hh>

namespace
{
using namespace sgl;
using namespace sgl::check;
using namespace sgl::emit;
using namespace sgl::emit::impl;

/// The buffer index of the inline constants, in every stage that reads them.
/// It must equal sg's metal `k_inline_constants_buffer_index`, which sgl cannot include.
constexpr auto k_inline_constants_buffer = 4;

/// MSL's texture types, parallel to `texture_shape`, and the depth ones where a shape has one.
constexpr cc::string_view k_texture_names[]
    = {"texture1d",          "texture1d_array", "texture2d",   "texture2d_array",  "texture2d_ms",
       "texture2d_ms_array", "texture3d",       "texturecube", "texturecube_array"};
constexpr cc::string_view k_depth_names[]
    = {"", "", "depth2d", "depth2d_array", "depth2d_ms", "depth2d_ms_array", "", "depthcube", "depthcube_array"};
/// Parallel to `access_mode`.
constexpr cc::string_view k_accesses[] = {"read", "read_write", "write"};

/// The scalar a texture of `element` holds, which is what MSL's texture types take: `float` for any `float` width.
cc::string_view scalar_of(cc::string_view element)
{
    return element.starts_with("uint") ? "uint" : element.starts_with("int") ? "int" : "float";
}

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

    [[nodiscard]] cc::string resource_text(plan const& p, type_id type) const override
    {
        auto const& t = p.m.at(type);
        switch (t.kind)
        {
        case type_kind::buffer:
            return cc::format("{}device {}*", t.is_mut ? "" : "const ", type_text(p, *this, t.element));
        case type_kind::texture:
            if (t.is_depth)
                return cc::format("{}<float>", k_depth_names[isize(t.shape)]);
            return cc::format("{}<{}>", k_texture_names[isize(t.shape)], scalar_of(p.m.name_of(t.element)));
        case type_kind::image:
            return cc::format("{}<{}, access::{}>", k_texture_names[isize(t.shape)],
                              scalar_of(builtin_spelling(p, texel_name_of(t.format))), k_accesses[isize(t.access)]);
        case type_kind::sampler:
            return "sampler";
        default:
            return {};
        }
    }

    /// EMIT-89: a group is an argument buffer, a struct whose member `[[id(n)]]` is slot n of the group.
    /// Its constant block is a pointer at slot 0, and a binding array is a C array over consecutive ids.
    void write_group(cc::string& out,
                     plan const& p,
                     planned_constants const* block,
                     cc::span<planned_resource const> buffers) const override
    {
        for (auto const& b : buffers)
            if (b.element_form.has_value())
                write_form(out, b.element_form.value());
        if (block != nullptr)
            write_block_struct(out, p, *block);

        auto const group = block != nullptr ? block->group : buffers[0].group;
        out.appendf("struct {}\n{{\n", argument_buffer_of(p, group).struct_name);
        if (block != nullptr)
            out.appendf("{}constant {}* {} [[id({})]];\n", k_indent, block->block_name, block->name, block->slot);
        for (auto const& b : buffers)
        {
            auto const type = b.element_form.has_value()
                                ? cc::format("{}device {}*", b.is_mut ? "" : "const ", b.element_form.value().name)
                                : resource_text(p, b.type);
            out.appendf("{}{} {} [[id({})]]", k_indent, type, b.name, b.slot);
            if (b.count > 1)
                out.appendf("[{}]", b.count);
            out += ";\n";
        }
        out += "};\n\n";
    }

    [[nodiscard]] static planned_argument_buffer const& argument_buffer_of(plan const& p, i32 group)
    {
        for (auto const& a : p.argument_buffers)
            if (a.group == group)
                return a;
        CC_UNREACHABLE("every group MSL writes was given an argument buffer by the plan");
    }

    /// A root's memory form: its pieces as fields, each where SGL's layout puts it (memory_form.hh).
    static void write_form(cc::string& out, memory_form const& form)
    {
        out.appendf("struct {}\n{{\n", form.name);
        for (auto const& f : form.fields)
            out.appendf("{}{} {};\n", k_indent, f.type, f.name);
        out += "};\n\n";
    }

    void write_block_struct(cc::string& out, plan const& p, planned_constants const& block) const
    {
        if (block.form.has_value())
            return write_form(out, block.form.value());
        out.appendf("struct {}\n{{\n", block.block_name);
        for (auto const& member : block.members)
            write_member(out, nullptr, member, p);
        out += "};\n\n";
    }

    void write_declarations(cc::string& out, plan const& p) const override
    {
        out += "#include <metal_stdlib>\nusing namespace metal;\n\n";
        write_enum_constants(out, p, *this);
        // A struct stands ahead of the groups, whose blocks and buffers may hold it.
        for (auto const& s : p.structs)
        {
            out.appendf("struct {}\n{{\n", s.name);
            for (auto const& member : s.members)
                write_member(out, &s, member, p);
            out += "};\n\n";
        }
        write_buffers(out, p, *this);

        if (!p.constants.has_value())
            return;
        write_block_struct(out, p, p.constants.value());
    }

    /// MSL has no global resources: the inline constants and every group are parameters, and each is bound to a local
    /// at the top of the body under the name the other targets give their global, so the body reads them alike.
    void write_function_head(cc::string& out, plan const& p) const override
    {
        auto parameters = cc::vector<cc::string>();
        if (check::is_valid(p.e.input))
            parameters.push_back(cc::format("{} {} [[stage_in]]", type_text(p, *this, p.e.input), p.locals[0]));
        for (auto i = isize(0); i < p.e.stage_inputs.size(); ++i)
        {
            auto const& spelled = spelling_of(p.e.stage_inputs[i].input);
            parameters.push_back(
                cc::format("{} {} [[{}]]", spelled.msl_type, p.stage_input_names[i], spelled.msl_attribute));
        }
        for (auto const& a : p.argument_buffers)
            parameters.push_back(cc::format("constant {}& {} [[buffer({})]]", a.struct_name, a.parameter, a.group));
        if (p.constants.has_value())
        {
            auto const& c = p.constants.value();
            parameters.push_back(
                cc::format("constant {}& {} [[buffer({})]]", c.block_name, c.name, k_inline_constants_buffer));
        }
        auto list = cc::string();
        for (auto const& parameter : parameters)
            list += cc::format("{}{}", list.empty() ? "" : ", ", parameter);

        if (p.e.entry_stage == stage::compute)
        {
            // EMIT-59: MSL states no threadgroup shape of its own, so the line the Metal compiler reads it from does
            out.appendf("#pragma sc numthreads {} {} {}\n", p.e.workgroup[0], p.e.workgroup[1], p.e.workgroup[2]);
            out.appendf("kernel void {}({})\n{{\n", p.entry_name, list);
        }
        else
            out.appendf("{} {} {}({})\n{{\n", p.e.entry_stage == stage::vertex ? "vertex" : "fragment",
                        type_text(p, *this, p.e.result), p.entry_name, list);
        for (auto const& block : p.group_blocks)
            out.appendf("    constant auto& {} = *{}.{};\n", block.name, argument_buffer_of(p, block.group).parameter,
                        block.name);
        for (auto const& r : p.resources)
            out.appendf("    constant auto& {} = {}.{};\n", r.name, argument_buffer_of(p, r.group).parameter, r.name);
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
