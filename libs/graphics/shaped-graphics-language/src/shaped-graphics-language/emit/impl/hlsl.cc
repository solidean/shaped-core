#include "dialect.hh"

#include <clean-core/string/format.hh>

namespace
{
using namespace sgl;
using namespace sgl::check;
using namespace sgl::emit;
using namespace sgl::emit::impl;

/// The space slib's binding pass gives the inline constants of a dx12 pipeline, `slib::inline_constants_space`.
/// sgl does not link slib, so the number is repeated here, and the pipeline layout sg builds is what it has to match.
constexpr auto k_dx12_inline_constants_space = 9;

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

    cc::string_view type_name(builtin b) const override
    {
        switch (b)
        {
        case builtin::scalar_float:
            return "float";
        case builtin::float3:
        case builtin::vec3:
        case builtin::pos3:
            return "float3";
        case builtin::float4:
        case builtin::hpos4:
            return "float4";
        case builtin::mat4:
            return "float4x4";
        case builtin::scalar_int:
            return "int";
        case builtin::boolean:
            return "bool";
        default:
            return "";
        }
    }

    cc::string_view function_name(builtin b) const override { return to_string(b); }

    bool has_mul_function() const override { return true; }
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
        if (builtin_of_type(p.m, member.type) == builtin::mat4)
            out += "column_major ";
        out.appendf("{} {}", type_text(p, *this, member.type), member.name);
        if (owner != nullptr)
            if (auto const semantic = semantic_of(*owner, member); !semantic.empty())
                out.appendf(" : {}", semantic);
        out += ";\n";
    }

    void write_declarations(cc::string& out, plan const& p) const override
    {
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
                        k_dx12_inline_constants_space);
    }

    void write_function_head(cc::string& out, plan const& p) const override
    {
        out.appendf("{} {}({} {})\n{{\n", type_text(p, *this, p.e.result), p.e.name, type_text(p, *this, p.e.input),
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
