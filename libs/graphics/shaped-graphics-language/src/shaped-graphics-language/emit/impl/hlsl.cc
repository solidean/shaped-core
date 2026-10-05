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
    cc::string semantic_of(planned_struct const& s, planned_member const& member, plan const& p) const
    {
        if (member.is_position)
            return "SV_Position";
        switch (s.role)
        {
        case struct_role::vertex_input:
        {
            auto const at = &member - s.members.data();
            auto const semantics = vertex_semantics(p.m, p.m.at(s.type));
            for (auto i = isize(0); i < s.member_of.size(); ++i)
                if (s.member_of[i] == at)
                    return semantics[i];
            return "";
        }
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

    /// Which shaders read and write one payload field: the caller, then closesthit, miss and anyhit.
    struct payload_access
    {
        bool reads[4] = {};
        bool writes[4] = {};
    };

    /// `: read(…) : write(…)` for `a`; a field nobody reads or writes is still the caller's, since HLSL qualifies each.
    static cc::string access_text(payload_access const& a)
    {
        cc::string_view const names[] = {"caller", "closesthit", "miss", "anyhit"};
        auto const list = [&](bool const(&on)[4])
        {
            auto text = cc::string();
            for (auto i = 0; i < 4; ++i)
                if (on[i])
                    text.appendf("{}{}", text.empty() ? "" : ", ", names[i]);
            return text.empty() ? cc::string("caller") : text;
        };
        return cc::format(" : read({}) : write({})", list(a.reads), list(a.writes));
    }

    /// EMIT-137: field `name` of payload `type`, as every entry point of the module touches it.
    /// A stage is its payload parameter's reader and writer, and a caller whatever local of the type it traces with.
    /// A partial write reads the field too, since what it leaves must arrive intact; and a whole-struct access is one
    /// of every field.
    /// Every shader of one SGL file then states the same qualifiers, which is what one pipeline's shaders must agree on.
    static payload_access payload_access_of(plan const& p, check::type_id type, cc::string_view name)
    {
        auto result = payload_access();
        // Every shader of a pipeline must state the same qualifiers, and a host's hit group is compiled apart from the
        // pipeline it joins: so a type only closed pipelines of this module trace is inferred, and any other is widest.
        auto is_traced = false;
        auto is_open = false;
        for (auto const& pipeline : p.m.pipelines)
        {
            if (pipeline.kind != check::pipeline_kind::raytracing)
                continue;
            auto is_its = false;
            for (auto const& ray : p.m.at(p.m.at(p.m.at(pipeline.ray_set).type).members))
                is_its = is_its || ray.type == type;
            is_traced = is_traced || is_its;
            is_open = is_open || (is_its && pipeline.has_host_hit_groups);
        }
        if (!is_traced || is_open)
        {
            for (auto i = 0; i < 4; ++i)
                result.reads[i] = result.writes[i] = true;
            return result;
        }
        auto field = isize(-1);
        auto const members = p.m.at(p.m.at(type).members);
        for (auto i = isize(0); i < members.size(); ++i)
            if (members[i].name == name)
                field = i;
        for (auto const& e : p.m.entry_points)
        {
            auto const stage_slot = e.entry_stage == stage::closest_hit ? 1
                                  : e.entry_stage == stage::miss        ? 2
                                  : e.entry_stage == stage::any_hit     ? 3
                                                                        : -1;
            auto is_caller = false;
            for (auto const& r : e.traced_rays)
                is_caller = is_caller || p.m.at(p.m.at(p.m.at(r.set).type).members)[r.ray].type == type;
            auto const slot_of = [&](check::local_id local) -> int
            {
                if (stage_slot >= 0 && e.input == type && local == check::local_id(0))
                    return stage_slot;
                if (is_caller && e.at(local).type == type)
                    return 0;
                return -1;
            };
            // what stands as the object of a member, a place, or a trace's payload is no whole-struct read
            auto is_object = cc::vector<bool>::create_filled(e.exprs.size(), false);
            auto is_place = cc::vector<bool>::create_filled(e.exprs.size(), false);
            for (auto const& x : e.exprs)
            {
                if (auto const* const m = x.node.try_as<check::flat_member>(); m != nullptr && check::is_valid(m->object))
                    is_object[index_of(m->object)] = true;
                auto const* const c = x.node.try_as<check::flat_call>();
                auto const* const record = c == nullptr ? nullptr : p.m.builtin_function(c->intrinsic);
                if (record == nullptr || !record->takes_element || e.at(c->arguments).empty())
                    continue;
                auto const handed = e.at(c->arguments).back();
                is_place[index_of(handed)] = true;
                // a payload handed on to a nested trace or a callable comes back written, whatever the stage itself
                // does with it: what the nested shaders wrote must survive this stage's exit
                if (auto const* const ref = e.at(handed).node.try_as<check::flat_local_ref>();
                    ref != nullptr && slot_of(ref->local) >= 0)
                    result.reads[slot_of(ref->local)] = result.writes[slot_of(ref->local)] = true;
            }
            for (auto const& st : e.stmts)
            {
                // a local's value where it is declared is a write of every field
                if (auto const* const l = st.node.try_as<check::flat_let>(); l != nullptr && slot_of(l->local) >= 0)
                    result.writes[slot_of(l->local)] = true;
                if (auto const* const v = st.node.try_as<check::flat_var>();
                    v != nullptr && check::is_valid(v->value) && slot_of(v->local) >= 0)
                    result.writes[slot_of(v->local)] = true;
                auto const* const a = st.node.try_as<check::flat_assign>();
                if (a == nullptr || !check::is_valid(a->place))
                    continue;
                // down the member chain to the field right below the local
                auto at = a->place;
                auto top = flat_expr_id::none;
                auto depth = 0;
                while (auto const* const m = e.at(at).node.try_as<check::flat_member>())
                {
                    is_place[index_of(at)] = true;
                    top = at;
                    at = m->object;
                    ++depth;
                }
                auto const* const ref = e.at(at).node.try_as<check::flat_local_ref>();
                if (ref == nullptr)
                    continue;
                is_place[index_of(at)] = true;
                auto const slot = slot_of(ref->local);
                if (slot < 0)
                    continue;
                if (!check::is_valid(top))
                    result.writes[slot] = true;
                else if (e.at(top).node.as<check::flat_member>().member == field)
                {
                    result.writes[slot] = true;
                    result.reads[slot] = result.reads[slot] || depth > 1;
                }
            }
            for (auto i = isize(0); i < e.exprs.size(); ++i)
            {
                auto const& x = e.exprs[i];
                if (auto const* const m = x.node.try_as<check::flat_member>(); m != nullptr && !is_place[i])
                    if (auto const* const ref = e.at(m->object).node.try_as<check::flat_local_ref>();
                        ref != nullptr && m->member == field && slot_of(ref->local) >= 0)
                        result.reads[slot_of(ref->local)] = true;
                if (auto const* const ref = x.node.try_as<check::flat_local_ref>();
                    ref != nullptr && !is_object[i] && !is_place[i] && slot_of(ref->local) >= 0)
                    result.reads[slot_of(ref->local)] = true;
            }
        }
        // a stage that writes a field reads it too: a write it makes only on some paths keeps what came in on the others
        for (auto i = 1; i < 4; ++i)
        {
            result.reads[i] = result.reads[i] || result.writes[i];
            // what a stage writes the caller reads, since it alone consumes the trace; and what a stage reads the caller
            // writes, so it arrives defined
            result.reads[0] = result.reads[0] || result.writes[i];
            result.writes[0] = result.writes[0] || result.reads[i];
        }
        return result;
    }

    /// Whether `type` is a ray payload of the entry point: the one it is handed, or one it traces with.
    [[nodiscard]] static bool is_payload(plan const& p, check::type_id type)
    {
        if (p.e.entry_stage >= stage::raygen && p.e.entry_stage != stage::callable && type == p.e.input)
            return true;
        for (auto const& r : p.e.traced_rays)
            if (p.m.at(p.m.at(p.m.at(r.set).type).members)[r.ray].type == type)
                return true;
        return false;
    }

    /// Whether the entry point is a pixel stage writing `@depth(.greater_equal)` or `@depth(.less_equal)`.
    [[nodiscard]] static bool writes_conservative_depth(plan const& p)
    {
        if (p.e.entry_stage != stage::pixel || !check::is_valid(p.e.result))
            return false;
        for (auto const& m : p.m.at(p.m.at(p.e.result).members))
            if (m.output == check::pixel_output::depth_greater_equal || m.output == check::pixel_output::depth_less_equal)
                return true;
        return false;
    }

    void write_member(cc::string& out, planned_struct const* owner, planned_member const& member, plan const& p) const
    {
        out += k_indent;
        // EMIT-130: DXIL takes a conservative depth only from a pixel stage whose position is interpolated at the centroid
        if (!_is_vulkan && member.is_position && writes_conservative_depth(p))
            out += "noperspective centroid ";
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
            if (auto const semantic = semantic_of(*owner, member, p); !semantic.empty())
                out.appendf(" : {}", semantic);
        // EMIT-137: a payload's field states which shaders read and write it, as every shader of the module does
        if (owner != nullptr && is_payload(p, owner->type))
            out += access_text(payload_access_of(p, owner->type, member.source_name));
        out += ";\n";
    }

    void write_enum_constant(cc::string& out, cc::string_view name, i32 value) const override
    {
        out.appendf("static const int {} = {};\n", name, value);
    }

    /// A block's struct: its members, or its memory form where it has one (EMIT-154).
    /// Every field states its offset on vulkan, as the push-constant block's do, so no compiler flag decides the layout.
    /// dx12 states none: each field of a form fits its row and padding fills every gap, so its own packing lands each
    /// at its offset.
    void write_block_struct(cc::string& out, plan const& p, planned_constants const& block) const
    {
        out.appendf("struct {}\n{{\n", block.block_name);
        if (!block.form.has_value())
            for (auto const& member : block.members)
                write_member(out, nullptr, member, p);
        else
            for (auto const& f : block.form.value().fields)
            {
                out += k_indent;
                if (_is_vulkan)
                    out.appendf("[[vk::offset({})]] ", f.offset);
                out.appendf("{}{} {};\n", f.type == "float4x4" ? "column_major " : "", f.type, f.name);
            }
        out += "};\n\n";
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
            write_block_struct(out, p, *block);
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
        // EMIT-150: DXC writes it as SPIR-V's `Coherent` for vulkan
        auto const type
            = b.is_coherent ? cc::format("globallycoherent {}", resource_text(p, b.type)) : resource_text(p, b.type);
        write_addressed(out, type, name, register_class_of(t), b.group, b.slot, format);
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
        if (t.kind == type_kind::image || ((t.kind == type_kind::buffer || t.kind == type_kind::bytes) && t.is_mut))
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
        case type_kind::bytes:
            return t.is_mut ? "RWByteAddressBuffer" : "ByteAddressBuffer";
        case type_kind::texture:
            // A depth texture samples to one float, which is how HLSL declares it.
            return cc::format("{}<{}>", k_texture_names[isize(t.shape)],
                              t.is_depth ? cc::string_view("float") : type_text(p, *this, t.element));
        case type_kind::image:
            return cc::format("{}<{}>", k_image_names[isize(t.shape)], builtin_spelling(p, texel_name_of(t.format)));
        case type_kind::sampler:
            return t.is_comparison ? "SamplerComparisonState" : "SamplerState";
        case type_kind::acceleration_structure:
            return "RaytracingAccelerationStructure";
        default:
            return {};
        }
    }

    void write_declarations(cc::string& out, plan const& p) const override
    {
        // EMIT-137: a payload the host's shaders may touch states the widest access, which DXC takes for a missed
        // optimization rather than an error
        if (p.e.entry_stage >= stage::raygen)
            out += "#pragma dxc diagnostic ignored \"-Wpayload-access-perf\"\n\n";
        write_enum_constants(out, p, *this);
        // A struct stands ahead of the groups, whose blocks and buffers may hold it.
        for (auto const& s : p.structs)
        {
            out.appendf("struct {}{}\n{{\n", is_payload(p, s.type) ? "[raypayload] " : "", s.name);
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
        write_block_struct(out, p, c);
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
        // EMIT-134: HLSL names the winding in its domain's own orientation, which mirrors the patch its points weigh
        auto const topology = domain == "isoline" ? cc::string_view("line")
                            : info.is_clockwise   ? cc::string_view("triangle_ccw")
                                                  : cc::string_view("triangle_cw");
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
            if (!spelled.hlsl_read.empty())
                continue;
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

        // EMIT-136: a ray-tracing stage is a library export, named by its kind, whose payload is `inout`
        if (p.e.entry_stage >= stage::raygen)
        {
            auto const kind = p.e.entry_stage == stage::raygen       ? "raygeneration"
                            : p.e.entry_stage == stage::miss         ? "miss"
                            : p.e.entry_stage == stage::closest_hit  ? "closesthit"
                            : p.e.entry_stage == stage::any_hit      ? "anyhit"
                            : p.e.entry_stage == stage::intersection ? "intersection"
                                                                     : "callable";
            auto rt = cc::string();
            if (check::is_valid(p.e.input))
                rt = cc::format("inout {} {}", type_text(p, *this, p.e.input), p.locals[0]);
            // EMIT-137: a triangle's attributes are its barycentrics, and a procedural primitive's what it reported
            if (check::is_valid(p.e.attributes))
                rt += cc::format(", in {} {}", type_text(p, *this, p.e.at(p.e.attributes).type),
                                 p.locals[index_of(p.e.attributes)]);
            else if (p.e.entry_stage == stage::closest_hit || p.e.entry_stage == stage::any_hit)
                rt += ", in BuiltInTriangleIntersectionAttributes sgl_attributes";
            out.appendf("[shader(\"{}\")]\nvoid {}({})\n{{\n", kind, p.entry_name, rt);
            return;
        }

        if (p.e.entry_stage == stage::compute)
        {
            out.appendf("[numthreads({}, {}, {})]\n", p.e.workgroup[0], p.e.workgroup[1], p.e.workgroup[2]);
            // EMIT-148: the range form, which every dx12 device runs, prefers the size where the device has it
            if (!_is_vulkan && p.e.preferred_subgroup_size > 0)
                out.appendf("[WaveSize(4, 128, {})]\n", p.e.preferred_subgroup_size);
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
