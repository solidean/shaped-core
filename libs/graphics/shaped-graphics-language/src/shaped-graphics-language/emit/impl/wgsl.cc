#include "dialect.hh"

#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/resources.hh>

namespace
{
/// WGSL's texture and image types, parallel to `texture_shape`.
/// A 1D texture is a 2D one on WebGPU, which sg creates every 1D texture as (the spec's bindings file, "Shapes").
constexpr cc::string_view k_texture_names[]
    = {"texture_2d", "texture_2d_array", "texture_2d",   "texture_2d_array",  "texture_multisampled_2d",
       "",           "texture_3d",       "texture_cube", "texture_cube_array"};
constexpr cc::string_view k_depth_names[]
    = {"", "", "texture_depth_2d",   "texture_depth_2d_array",  "texture_depth_multisampled_2d",
       "", "", "texture_depth_cube", "texture_depth_cube_array"};
constexpr cc::string_view k_image_names[] = {"texture_storage_2d",
                                             "texture_storage_2d_array",
                                             "texture_storage_2d",
                                             "texture_storage_2d_array",
                                             "",
                                             "",
                                             "texture_storage_3d",
                                             "",
                                             ""};
/// Parallel to `image_access`.
constexpr cc::string_view k_accesses[] = {"read", "read_write", "write"};

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
                     cc::span<planned_resource const> buffers) const override
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
            write_resource(out, p, b);
        out += "\n";
    }

    void write_resource(cc::string& out, plan const& p, planned_resource const& b) const
    {
        auto const& t = p.m.at(b.type);
        auto const address = cc::format("@group({}) @binding({})", b.group, b.slot);
        // WGSL has no static sampler: the layout carries it, and the group binds it (slib's WGSL notes).
        if (t.kind == type_kind::buffer)
            out.appendf("{} var<storage, {}> {}: {};\n", address, b.is_mut ? "read_write" : "read", b.name,
                        resource_text(p, b.type));
        else
            out.appendf("{} var {}: {};\n", address, b.name, resource_text(p, b.type));
    }

    [[nodiscard]] cc::string resource_text(plan const& p, type_id type) const override
    {
        auto const& t = p.m.at(type);
        switch (t.kind)
        {
        case type_kind::buffer:
            return cc::format("array<{}>", type_text(p, *this, t.element));
        case type_kind::texture:
            if (t.is_depth)
                return cc::string(k_depth_names[isize(t.shape)]);
            return cc::format("{}<{}>", k_texture_names[isize(t.shape)], scalar_of(p, t.element));
        case type_kind::image:
            return cc::format("{}<{}, {}>", k_image_names[isize(t.shape)], k_image_formats[t.format].wgsl,
                              k_accesses[isize(t.access)]);
        case type_kind::sampler:
            return t.is_comparison ? "sampler_comparison" : "sampler";
        default:
            return {};
        }
    }

    /// The scalar a texture of `element` samples to: `f32` for any `float` width.
    static cc::string_view scalar_of(plan const& p, type_id element)
    {
        auto const name = p.m.name_of(element);
        return name.starts_with("uint") ? "u32" : name.starts_with("int") ? "i32" : "f32";
    }

    void write_declarations(cc::string& out, plan const& p) const override
    {
        // EMIT-103: a directive, so it stands ahead of every declaration.
        if (uses_derivatives(p))
            out += "diagnostic(off, derivative_uniformity);\n\n";
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
            out.appendf("fn {}(@builtin(global_invocation_id) {}: vec3u) {{\n", p.entry_name, p.dispatch_name);
            // WebGPU reports the id unsigned and SGL has one integer type, so the conversion stands at the top.
            out.appendf("    let {}: vec3i = vec3i({});\n", p.locals[0], p.dispatch_name);
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
