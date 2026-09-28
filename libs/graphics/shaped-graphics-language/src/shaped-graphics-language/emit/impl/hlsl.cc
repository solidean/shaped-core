#include "dialect.hh"

#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/resources.hh>

namespace
{
/// The space slib's binding pass gives the inline constants of a dx12 pipeline, `slib::inline_constants_space`.
/// sgl does not link slib, so the number is repeated here, and the pipeline layout sg builds is what it has to match.
constexpr auto k_inline_constants_space = 9;
/// The space slib gives a pipeline layout's static samplers on dx12, `slib::bound_samplers_space`, repeated for the same reason.
constexpr auto k_bound_samplers_space = 10;
/// The descriptor set sg keeps for itself on vulkan, `sg::reserved_binding_group`, whose binding 0 no static sampler takes.
constexpr auto k_reserved_set = 3;

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
            out.appendf("{} {}{};", local.type, local.name, local.dimensions);
        else
            out.appendf("{}{} {}{} = {};", local.is_mut ? "" : "const ", local.type, local.name, local.dimensions,
                        local.value);
    }

    void write_eval(cc::string& out, cc::string_view value) const override { out.appendf("{};", value); }

    void write_workgroup(cc::string& out, planned_workgroup const& w, plan const& p) const override
    {
        out.appendf("groupshared {} {}{};\n", type_text(p, *this, w.type), w.name, array_dimensions(p, w.type));
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
        case struct_role::patch_constants:
            // EMIT-123: the factors are the tessellator's, and any other member reaches the evaluation stage as a link
            if (member.factor == check::tessellation_factor::edge)
                return "SV_TessFactor";
            if (member.factor == check::tessellation_factor::inside)
                return "SV_InsideTessFactor";
            return cc::format("SGL{}", member.location);
        case struct_role::render_targets:
            // EMIT-130: the depth and the sample mask are outputs of their own
            switch (member.output)
            {
            case check::pixel_output::depth:
                return "SV_Depth";
            case check::pixel_output::depth_greater_equal:
                return "SV_DepthGreaterEqual";
            case check::pixel_output::depth_less_equal:
                return "SV_DepthLessEqual";
            case check::pixel_output::sample_mask:
                return "SV_Coverage";
            case check::pixel_output::color:
                break;
            }
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
        // EMIT-129: a stage link's interpolation, as HLSL's qualifiers spell it
        if (owner != nullptr && owner->role == struct_role::stage_link)
        {
            using kind = check::interpolation::kind_t;
            using sampling = check::interpolation::sampling_t;
            if (member.interpolate.kind == kind::flat)
                out += "nointerpolation ";
            else if (member.interpolate.kind == kind::linear)
                out += "noperspective ";
            if (member.interpolate.sampling == sampling::centroid)
                out += "centroid ";
            else if (member.interpolate.sampling == sampling::sample)
                out += "sample ";
        }
        out.appendf("{} {}{}", type_text(p, *this, member.type), member.name, array_dimensions(p, member.type));
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
        auto const format = t.kind == type_kind::image ? k_image_formats[t.format].spirv : cc::string_view();
        // a binding array takes `count` consecutive registers from its first
        auto const name = b.count > 1 ? cc::format("{}[{}]", b.name, b.count) : cc::string(b.name);
        write_addressed(out, resource_text(p, b.type), name, register_class_of(t), b.group, b.slot, format);
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

    void write_file_sampler(cc::string& out, plan const& p, planned_sampler const& s) const override
    {
        if (_is_vulkan)
            out.appendf("[[vk::binding({}, {})]] {} {};\n", s.index + 1, k_reserved_set, resource_text(p, s.type),
                        s.name);
        else
            out.appendf("{} {} : register(s{}, space{});\n", resource_text(p, s.type), s.name, s.index,
                        k_bound_samplers_space);
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
        // A struct stands ahead of the groups, whose blocks and buffers may hold it.
        for (auto const& s : p.structs)
        {
            out.appendf("struct {}\n{{\n", s.name);
            for (auto const& member : s.members)
                write_member(out, &s, member, p);
            out += "};\n\n";
        }
        write_buffers(out, p, *this);

        for (auto const& w : p.workgroup)
            write_workgroup(out, w, p);
        if (!p.workgroup.empty())
            out += "\n";

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

    /// The domain a tessellation stage's factors struct says, as HLSL names it (CHK-305).
    [[nodiscard]] static cc::string_view domain_of(plan const& p, type_id factors)
    {
        for (auto const& m : p.m.at(p.m.at(factors).members))
            if (m.factor == check::tessellation_factor::edge)
                return p.m.at(m.type).count == 2 ? "isoline" : p.m.at(m.type).count == 3 ? "tri" : "quad";
        return "tri";
    }

    /// EMIT-123: a geometry or a tessellation entry point, its parameters as HLSL takes each, in the order written.
    void write_primitive_head(cc::string& out, plan const& p) const
    {
        auto parameters = cc::vector<cc::string>();
        auto factors = type_id::none;
        for (auto i = isize(0); i < p.e.locals.size(); ++i)
        {
            auto const& local = p.e.locals[i];
            if (local.kind != check::local_kind::parameter)
                continue;
            auto const& name = p.locals[i];
            auto const& t = p.m.at(local.type);
            auto input = isize(-1);
            for (auto k = isize(0); k < p.e.stage_inputs.size(); ++k)
                if (index_of(p.e.stage_inputs[k].local) == i)
                    input = k;
            if (input >= 0)
            {
                auto const& spelled = spelling_of(p.e.stage_inputs[input].input);
                // a domain location is the float3 or the float2 its parameter is
                auto const type = p.e.stage_inputs[input].input == check::stage_input::domain_location
                                    ? type_text(p, *this, local.type)
                                    : spelled.hlsl_type;
                parameters.push_back(cc::format("{} {} : {}", type, p.stage_input_names[input], spelled.hlsl_semantic));
                continue;
            }
            if (t.kind == type_kind::array)
            {
                auto const element = type_text(p, *this, t.element);
                if (p.e.entry_stage == stage::geometry)
                {
                    constexpr cc::string_view primitives[]
                        = {"", "point", "line", "triangle", "lineadj", "", "triangleadj"};
                    parameters.push_back(cc::format("{} {} {}[{}]", primitives[t.count], element, name, t.count));
                }
                else if (p.e.entry_stage == stage::tessellation_control)
                    parameters.push_back(cc::format("InputPatch<{}, {}> {}", element, t.count, name));
                else
                    parameters.push_back(cc::format("const OutputPatch<{}, {}> {}", element, t.count, name));
                continue;
            }
            if (t.kind == type_kind::stream)
            {
                auto const stream = t.count == 1 ? "PointStream" : t.count == 2 ? "LineStream" : "TriangleStream";
                parameters.push_back(cc::format("inout {}<{}> {}", stream, type_text(p, *this, t.element), name));
                continue;
            }
            factors = local.type;
            parameters.push_back(cc::format("{} {}", type_text(p, *this, local.type), name));
        }
        auto list = cc::string();
        for (auto const& parameter : parameters)
            list += cc::format("{}{}", list.empty() ? "" : ", ", parameter);

        auto const& info = p.m.functions[p.m.at(p.e.function).info];
        if (p.e.entry_stage == stage::geometry)
        {
            out.appendf("[maxvertexcount({})]\n", info.max_vertices);
            out.appendf("void {}({})\n{{\n", p.entry_name, list);
        }
        else if (p.e.entry_stage == stage::tessellation_control)
            out.appendf("{} {}({})\n{{\n", type_text(p, *this, p.e.result), p.patch_function, list);
        else
        {
            out.appendf("[domain(\"{}\")]\n", domain_of(p, factors));
            out.appendf("{} {}({})\n{{\n", type_text(p, *this, p.e.result), p.entry_name, list);
        }
        for (auto i = isize(0); i < p.e.stage_inputs.size(); ++i)
        {
            auto const local = p.e.stage_inputs[i].local;
            out.appendf("    const {} {} = {};\n", type_text(p, *this, p.e.at(local).type), p.locals[index_of(local)],
                        stage_input_value(p, i));
        }
    }

    /// A control stage's control points pass through unchanged, which is a hull function that hands each on.
    void write_function_tail(cc::string& out, plan const& p) const override
    {
        if (p.e.entry_stage != stage::tessellation_control)
            return;
        auto const& info = p.m.functions[p.m.at(p.e.function).info];
        auto const& patch = p.m.at(p.e.input);
        auto const point = type_text(p, *this, patch.element);
        auto const domain = domain_of(p, p.e.result);
        constexpr cc::string_view partitionings[] = {"integer", "fractional_even", "fractional_odd"};
        auto const topology = domain == "isoline" ? cc::string_view("line")
                            : info.is_clockwise   ? cc::string_view("triangle_cw")
                                                  : cc::string_view("triangle_ccw");
        out.appendf("\n[domain(\"{}\")]\n", domain);
        out.appendf("[partitioning(\"{}\")]\n", partitionings[isize(info.partitioning)]);
        out.appendf("[outputtopology(\"{}\")]\n", topology);
        out.appendf("[outputcontrolpoints({})]\n", patch.count);
        out.appendf("[patchconstantfunc(\"{}\")]\n", p.patch_function);
        out.appendf("{} {}(InputPatch<{}, {}> {}, uint {} : SV_OutputControlPointID)\n{{\n", point, p.entry_name, point,
                    patch.count, p.locals[0], p.point_index);
        out.appendf("    return {}[{}];\n}}\n", p.locals[0], p.point_index);
    }

    void write_function_head(cc::string& out, plan const& p) const override
    {
        if (p.e.entry_stage == stage::geometry || p.e.entry_stage == stage::tessellation_control
            || p.e.entry_stage == stage::tessellation_evaluation)
            return write_primitive_head(out, p);
        auto parameters = cc::vector<cc::string>();
        if (check::is_valid(p.e.input))
            parameters.push_back(cc::format("{} {}", type_text(p, *this, p.e.input), p.locals[0]));
        for (auto i = isize(0); i < p.e.stage_inputs.size(); ++i)
        {
            auto const& spelled = spelling_of(p.e.stage_inputs[i].input);
            parameters.push_back(
                cc::format("{} {} : {}", spelled.hlsl_type, p.stage_input_names[i], spelled.hlsl_semantic));
            // EMIT-128: HLSL counts from the draw's base, and shader model 6.8 says where the draw started
            if (has_base(p, p.e.stage_inputs[i].input))
                parameters.push_back(cc::format("uint {} : {}", p.stage_input_bases[i],
                                                p.e.stage_inputs[i].input == check::stage_input::vertex_index
                                                    ? "SV_StartVertexLocation"
                                                    : "SV_StartInstanceLocation"));
        }
        auto list = cc::string();
        for (auto const& parameter : parameters)
            list += cc::format("{}{}", list.empty() ? "" : ", ", parameter);

        if (p.e.entry_stage == stage::compute)
        {
            out.appendf("[numthreads({}, {}, {})]\n", p.e.workgroup[0], p.e.workgroup[1], p.e.workgroup[2]);
            out.appendf("void {}({})\n{{\n", p.entry_name, list);
        }
        else
            out.appendf("{} {}({})\n{{\n", type_text(p, *this, p.e.result), p.entry_name, list);
        // A target hands an index over unsigned and SGL counts in int, so each conversion stands at the top.
        for (auto i = isize(0); i < p.e.stage_inputs.size(); ++i)
        {
            auto const local = p.e.stage_inputs[i].local;
            out.appendf("    const {} {} = {};\n", type_text(p, *this, p.e.at(local).type), p.locals[index_of(local)],
                        stage_input_value(p, i));
        }
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
