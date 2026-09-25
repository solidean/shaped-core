#include "dialect.hh"

#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/resources.hh>

namespace
{
/// The space slib's binding pass gives the inline constants of a dx12 pipeline, `slib::inline_constants_space`.
/// sgl does not link slib, so the number is repeated here, and the pipeline layout sg builds is what it has to match.
constexpr auto k_inline_constants_space = 9;

using namespace sgl;
using namespace sgl::check;
using namespace sgl::emit;
using namespace sgl::emit::impl;

/// HLSL's texture and image types, parallel to `texture_shape`; an image has no multisampled or cube form.
constexpr cc::string_view k_texture_names[]
    = {"Texture1D",        "Texture1DArray", "Texture2D",   "Texture2DArray",  "Texture2DMS",
       "Texture2DMSArray", "Texture3D",      "TextureCube", "TextureCubeArray"};
constexpr cc::string_view k_image_names[]
    = {"RWTexture1D", "RWTexture1DArray", "RWTexture2D", "RWTexture2DArray", "", "", "RWTexture3D", "", ""};

/// `position` -> `POSITION`, which is how a dx12 input layout names a vertex attribute.
cc::string upper_cased(cc::string_view name)
{
    auto result = cc::string(name);
    for (auto& c : result.as_mutable_span())
        if (c >= 'a' && c <= 'z')
            c = char(c - 'a' + 'A');
    return result;
}

/// Both HLSL targets: one language, and two ways of saying where a thing lives.
class hlsl_dialect final : public dialect
{
public:
    explicit constexpr hlsl_dialect(bool is_vulkan) : _is_vulkan(is_vulkan) {}

    cc::string_view description() const override { return _is_vulkan ? "HLSL for vulkan" : "HLSL for dx12"; }

    builtins::language language() const override { return builtins::language::hlsl; }

    bool has_struct_constructor() const override { return false; }

    bool is_c_like() const override { return true; }

    void write_for_head(cc::string& out, cc::string_view index, cc::string_view first, cc::string_view end) const override
    {
        out.appendf("for (int {} = {}; {} < {}; ++{})", index, first, index, end, index);
    }

    void write_local(cc::string& out, local_declaration const& local) const override
    {
        if (local.value.empty())
            out.appendf("{} {};", local.type, local.name);
        else
            out.appendf("{}{} {} = {};", local.is_mut ? "" : "const ", local.type, local.name, local.value);
    }

    void write_eval(cc::string& out, cc::string_view value) const override { out.appendf("{};", value); }

    /// SPIR-V has no semantics, so vulkan takes the location as an attribute and keeps the semantic HLSL's grammar asks for.
    cc::string semantic_of(planned_struct const& s, planned_member const& member) const
    {
        if (member.is_position)
            return "SV_Position";
        switch (s.role)
        {
        case struct_role::vertex_input:
            return upper_cased(member.source_name);
        case struct_role::stage_link:
            return cc::format("SGL{}", member.location);
        case struct_role::render_targets:
            return cc::format("SV_Target{}", member.location);
        case struct_role::plain:
            break;
        }
        return "";
    }

    void write_member(cc::string& out, planned_struct const* owner, planned_member const& member, plan const& p) const
    {
        out += k_indent;
        auto const has_location = owner != nullptr && member.location >= 0 && owner->role != struct_role::render_targets;
        if (_is_vulkan && has_location)
            out.appendf("[[vk::location({})]] ", member.location);
        if (_is_vulkan && member.offset >= 0)
            out.appendf("[[vk::offset({})]] ", member.offset);
        // Stated on every matrix, so no `-Zpr` and no `#pragma pack_matrix` can turn one around.
        if (type_text(p, *this, member.type) == "float4x4")
            out += "column_major ";
        out.appendf("{} {}", type_text(p, *this, member.type), member.name);
        if (owner != nullptr)
            if (auto const semantic = semantic_of(*owner, member); !semantic.empty())
                out.appendf(" : {}", semantic);
        out += ";\n";
    }

    void write_enum_constant(cc::string& out, cc::string_view name, i32 value) const override
    {
        out.appendf("static const int {} = {};\n", name, value);
    }

    /// Each resource of a group carries its final address: `space` is the group and the register is the slot on
    /// dx12, `[[vk::binding(slot, group)]]` on vulkan.
    /// A group's block is a `ConstantBuffer` of a struct declared ahead of it.
    void write_group(cc::string& out,
                     plan const& p,
                     planned_constants const* block,
                     cc::span<planned_resource const> buffers) const override
    {
        if (block != nullptr)
        {
            // Every member states its offset on vulkan, as the push-constant block's do, so no compiler flag decides
            // the layout.
            out.appendf("struct {}\n{{\n", block->block_name);
            for (auto const& member : block->members)
                write_member(out, nullptr, member, p);
            out += "};\n\n";
        }
        if (block != nullptr)
            write_addressed(out, cc::format("ConstantBuffer<{}>", block->block_name), block->name, 'b', block->group,
                            block->slot, {});
        for (auto const& b : buffers)
            write_resource(out, p, b);
        out += "\n";
    }

    void write_resource(cc::string& out, plan const& p, planned_resource const& b) const
    {
        auto const& t = p.m.at(b.type);
        auto const format = t.kind == type_kind::image ? k_storage_formats[t.format].spirv : cc::string_view();
        write_addressed(out, resource_text(p, b.type), b.name, register_class_of(t), b.group, b.slot, format);
    }

    /// One declaration of a group with its address; `format` is an image's `[[vk::image_format]]`, which vulkan's
    /// SPIR-V wants and dx12 leaves to the view.
    void write_addressed(cc::string& out,
                         cc::string_view type,
                         cc::string_view name,
                         char register_class,
                         i32 group,
                         i32 slot,
                         cc::string_view format) const
    {
        if (_is_vulkan)
        {
            out.appendf("[[vk::binding({}, {})]] ", slot, group);
            if (!format.empty())
                out.appendf("[[vk::image_format(\"{}\")]] ", format);
            out.appendf("{} {};\n", type, name);
        }
        else
            out.appendf("{} {} : register({}{}, space{});\n", type, name, register_class, slot, group);
    }

    /// dx12's register class: `u` for what the shader writes, `s` for a sampler, `t` for every other resource.
    [[nodiscard]] static char register_class_of(check::type_info const& t)
    {
        if (t.kind == type_kind::sampler)
            return 's';
        if (t.kind == type_kind::image || (t.kind == type_kind::buffer && t.is_mut))
            return 'u';
        return 't';
    }

    [[nodiscard]] cc::string resource_text(plan const& p, type_id type) const override
    {
        auto const& t = p.m.at(type);
        switch (t.kind)
        {
        case type_kind::buffer:
            return cc::format("{}StructuredBuffer<{}>", t.is_mut ? "RW" : "", type_text(p, *this, t.element));
        case type_kind::texture:
            // A depth texture samples to one float, which is how HLSL declares it.
            return cc::format("{}<{}>", k_texture_names[isize(t.shape)],
                              t.is_depth ? cc::string_view("float") : type_text(p, *this, t.element));
        case type_kind::image:
            return cc::format("{}<{}>", k_image_names[isize(t.shape)], builtin_spelling(p, texel_name_of(t.format)));
        case type_kind::sampler:
            return t.is_comparison ? "SamplerComparisonState" : "SamplerState";
        default:
            return {};
        }
    }

    void write_declarations(cc::string& out, plan const& p) const override
    {
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
        for (auto const& member : c.members)
            write_member(out, nullptr, member, p);
        out += "};\n\n";
        if (_is_vulkan)
            out.appendf("[[vk::push_constant]] ConstantBuffer<{}> {};\n\n", c.block_name, c.name);
        else
            out.appendf("ConstantBuffer<{}> {} : register(b0, space{});\n\n", c.block_name, c.name,
                        k_inline_constants_space);
    }

    void write_function_head(cc::string& out, plan const& p) const override
    {
        if (p.e.entry_stage == stage::compute)
        {
            out.appendf("[numthreads({}, {}, {})]\n", p.e.workgroup[0], p.e.workgroup[1], p.e.workgroup[2]);
            out.appendf("void {}(uint3 {} : SV_DispatchThreadID)\n{{\n", p.entry_name, p.dispatch_name);
            // The dispatch reports the id unsigned and SGL has one integer type, so the conversion stands at the top.
            out.appendf("    const int3 {} = int3({});\n", p.locals[0], p.dispatch_name);
            return;
        }
        out.appendf("{} {}({} {})\n{{\n", type_text(p, *this, p.e.result), p.entry_name, type_text(p, *this, p.e.input),
                    p.locals[0]);
    }

private:
    bool _is_vulkan = false;
};

constexpr auto k_dx12 = hlsl_dialect(false);
constexpr auto k_vulkan = hlsl_dialect(true);
} // namespace

sgl::emit::impl::dialect const& sgl::emit::impl::hlsl_dx12_dialect()
{
    return k_dx12;
}

sgl::emit::impl::dialect const& sgl::emit::impl::hlsl_vulkan_dialect()
{
    return k_vulkan;
}
