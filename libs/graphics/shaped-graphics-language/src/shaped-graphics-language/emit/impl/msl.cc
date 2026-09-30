#include "dialect.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/to_string.hh>
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
/// sg's reserved group, where metal's ray-tracing tables arrive; sg's `sg::reserved_binding_group`.
constexpr auto k_reserved_group = 3;
/// Where metal's `dispatch_rays` binds each TLAS instance's hit-group offset, the first vertex-buffer slot, which a
/// kernel never has; sg's metal `k_hit_group_offsets_buffer_index`.
constexpr auto k_hit_offsets_buffer = 5;

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
        case type_kind::acceleration_structure:
            return "instance_acceleration_structure";
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

    /// A Metal sampler is a parameter of a raster or compute entry point, which `write_function_head` writes.
    /// A ray-tracing stage declares it `constexpr` at file scope instead (EMIT-133).
    /// Its visible and intersection functions have no sampler slot of the argument table, a struct holding a sampler
    /// must be passed by value, and an intersection function reads its context out of the ray data.
    void write_file_sampler(cc::string& out, plan const& p, planned_sampler const& s) const override
    {
        if (p.e.entry_stage < stage::raygen)
            return;
        auto const& state = p.m.samplers[p.m.at(s.symbol).info];
        constexpr cc::string_view addresses[] = {"repeat", "mirrored_repeat", "clamp_to_edge"};
        constexpr cc::string_view filters[] = {"nearest", "linear"};
        auto settings = cc::format("s_address::{}, t_address::{}, r_address::{}, mag_filter::{}, min_filter::{}, "
                                   "mip_filter::{}",
                                   addresses[state.address_u], addresses[state.address_v], addresses[state.address_w],
                                   filters[state.mag_filter], filters[state.min_filter], filters[state.mip_filter]);
        if (state.compare >= 0)
            settings.appendf(", compare_func::{}", check::k_compare_ops[state.compare]);
        if (state.max_anisotropy > 1)
            settings.appendf(", max_anisotropy({})", state.max_anisotropy);
        if (state.min_lod != 0.0f || state.max_lod != check::sampler_state().max_lod)
            settings.appendf(
                ", lod_clamp({}, {})", float_text(state.min_lod),
                state.max_lod == check::sampler_state().max_lod ? cc::string("FLT_MAX") : float_text(state.max_lod));
        out.appendf("constexpr sampler {}({});\n", s.name, settings);
    }

    /// A float as MSL reads it, always with a decimal point.
    [[nodiscard]] static cc::string float_text(f32 value)
    {
        auto text = cc::to_string(value);
        return text.contains('.') || text.contains('e') ? text : text + ".0";
    }

    void write_declarations(cc::string& out, plan const& p) const override
    {
        out += "#include <metal_stdlib>\nusing namespace metal;\n\n";
        if (p.e.entry_stage >= stage::raygen)
            out += "#include <metal_raytracing>\nusing namespace raytracing;\n\n";
        write_enum_constants(out, p, *this);
        // A struct stands ahead of the groups, whose blocks and buffers may hold it.
        for (auto const& s : p.structs)
        {
            out.appendf("struct {}\n{{\n", s.name);
            for (auto const& member : s.members)
                write_member(out, &s, member, p);
            out += "};\n\n";
        }
        if (p.e.entry_stage >= stage::raygen)
            write_ray_tracing_types(out, p);
        write_buffers(out, p, *this);

        if (!p.constants.has_value())
            return;
        write_block_struct(out, p, p.constants.value());
    }

    /// EMIT-139: what every ray-tracing function of a pipeline agrees on, since each is compiled apart and linked.
    /// A visible function takes the payload as words and its bindings through `sgl_context`, whatever its ray type, since
    /// one table holds the miss and closest-hit functions of every ray type; the ray data carries the payload, the
    /// attributes a procedural hit reports and the context into traversal.
    static void write_ray_tracing_types(cc::string& out, plan const& p)
    {
        auto const set = ray_set_of(p.m, p.e);
        auto payload_size = cc::string("16");
        auto ray_count = isize(1);
        if (check::is_valid(set))
        {
            auto const rays = p.m.at(p.m.at(p.m.at(set).type).members);
            ray_count = rays.size();
            payload_size = cc::format("sizeof({})", struct_name(p, rays[0].type));
            for (auto i = isize(1); i < rays.size(); ++i)
            {
                auto const next = cc::format("sizeof({})", struct_name(p, rays[i].type));
                payload_size = cc::format("({0} > {1} ? {0} : {1})", payload_size, next);
            }
        }
        out += "struct sgl_context\n{\n"
               "    constant void* groups[3];\n"
               "    constant void* constants;\n"
               "    device void* tables;\n"
               "    device uint const* hit_offsets;\n"
               "    uint3 launch_id;\n"
               "    uint3 launch_size;\n"
               "};\n\n";
        out += "struct sgl_hit_record\n{\n"
               "    float t;\n"
               "    float t_min;\n"
               "    float3 origin;\n"
               "    float3 direction;\n"
               "    float3 object_origin;\n"
               "    float3 object_direction;\n"
               "    uint instance_id;\n"
               "    uint instance_index;\n"
               "    uint geometry_index;\n"
               "    uint primitive_index;\n"
               "    float2 barycentrics;\n"
               "    uint front_face;\n"
               "    float4x3 object_to_world;\n"
               "    float4x3 world_to_object;\n"
               "    uint4 attributes[2];\n"
               "};\n\n";
        out.appendf("struct sgl_ray_data\n{{\n"
                    "    uint4 payload[({} + 15) / 16];\n"
                    "    uint4 attributes[2];\n"
                    "    float3 world_origin;\n"
                    "    float3 world_direction;\n"
                    "    sgl_context context;\n"
                    "}};\n\n",
                    payload_size);
        out += "struct sgl_box_result\n{\n"
               "    bool accept [[accept_intersection]];\n"
               "    float distance [[distance]];\n"
               "};\n\n";
        out += "using sgl_hit_function = void(thread uint4*, thread sgl_hit_record const&, thread sgl_context "
               "const&);\n"
               "using sgl_callable_function = void(thread uint4*, thread sgl_context const&);\n\n";
        // sg's reserved group: ray type 0's intersection table at id 0, then miss, closest hit and callable, then the
        // other ray types' intersection tables from id 4
        out += "struct sgl_tables\n{\n"
               "    intersection_function_table<triangle_data, instancing> hit_0 [[id(0)]];\n"
               "    visible_function_table<sgl_hit_function> miss [[id(1)]];\n"
               "    visible_function_table<sgl_hit_function> closest_hit [[id(2)]];\n"
               "    visible_function_table<sgl_callable_function> callable [[id(3)]];\n";
        for (auto r = isize(1); r < ray_count; ++r)
            out.appendf("    intersection_function_table<triangle_data, instancing> hit_{} [[id({})]];\n", r, 3 + r);
        out += "};\n\n";
        out += "template <typename T>\nvoid sgl_store(thread uint4* words, T value)\n{\n"
               "    *reinterpret_cast<thread T*>(words) = value;\n}\n\n"
               "template <typename T>\nvoid sgl_load(thread T& value, thread uint4* words)\n{\n"
               "    value = *reinterpret_cast<thread T*>(words);\n}\n\n";
    }

    [[nodiscard]] static cc::string struct_name(plan const& p, type_id type)
    {
        auto const k = p.struct_of_type[index_of(type)];
        return k >= 0 ? p.structs[k].name : cc::string(p.m.name_of(type));
    }

    /// The groups and the inline constants as locals of a function that is handed them through `sgl_context`.
    static void write_context_bindings(cc::string& out, plan const& p)
    {
        for (auto const& a : p.argument_buffers)
            out.appendf("    constant {0}& {1} = *static_cast<constant {0}*>(sgl_ctx.groups[{2}]);\n", a.struct_name,
                        a.parameter, a.group);
        if (p.constants.has_value())
            out.appendf("    constant {0}& {1} = *static_cast<constant {0}*>(sgl_ctx.constants);\n",
                        p.constants.value().block_name, p.constants.value().name);
        out += "    device sgl_tables& sgl_t = *static_cast<device sgl_tables*>(sgl_ctx.tables);\n";
    }

    /// EMIT-139: a ray-tracing stage as metal runs it: the raygen a kernel, a miss, closest hit or callable a visible
    /// function, and an any hit or a procedural group's traversal an intersection function.
    void write_ray_tracing_head(cc::string& out, plan const& p) const
    {
        auto const stage_of = p.e.entry_stage;
        auto const payload_type = check::is_valid(p.e.input) ? cc::string(type_text(p, *this, p.e.input)) : cc::string();
        auto const payload = check::is_valid(p.e.input) ? cc::string(p.locals[0]) : cc::string();
        if (stage_of == stage::raygen)
        {
            auto parameters = cc::vector<cc::string>();
            for (auto const& a : p.argument_buffers)
                parameters.push_back(cc::format("constant {}& {} [[buffer({})]]", a.struct_name, a.parameter, a.group));
            if (p.constants.has_value())
                parameters.push_back(cc::format("constant {}& {} [[buffer({})]]", p.constants.value().block_name,
                                                p.constants.value().name, k_inline_constants_buffer));
            parameters.push_back(cc::format("device sgl_tables& sgl_t [[buffer({})]]", k_reserved_group));
            parameters.push_back(cc::format("device uint const* sgl_hit_offsets [[buffer({})]]", k_hit_offsets_buffer));
            parameters.push_back("uint3 sgl_launch_id [[thread_position_in_grid]]");
            parameters.push_back("uint3 sgl_launch_size [[threads_per_grid]]");
            auto list = cc::string();
            for (auto const& parameter : parameters)
                list += cc::format("{}{}", list.empty() ? "" : ", ", parameter);
            out.appendf("kernel void {}({})\n{{\n", p.entry_name, list);
            out += "    sgl_context sgl_ctx;\n";
            for (auto g = 0; g < 3; ++g)
                out.appendf("    sgl_ctx.groups[{}] = nullptr;\n", g);
            for (auto const& a : p.argument_buffers)
                if (a.group < 3)
                    out.appendf("    sgl_ctx.groups[{}] = &{};\n", a.group, a.parameter);
            out.appendf("    sgl_ctx.constants = {};\n",
                        p.constants.has_value() ? cc::format("&{}", p.constants.value().name) : cc::string("nullptr"));
            out += "    sgl_ctx.tables = &sgl_t;\n"
                   "    sgl_ctx.hit_offsets = sgl_hit_offsets;\n"
                   "    sgl_ctx.launch_id = sgl_launch_id;\n"
                   "    sgl_ctx.launch_size = sgl_launch_size;\n";
        }
        else if (stage_of == stage::miss || stage_of == stage::closest_hit || stage_of == stage::callable)
        {
            auto const hit = stage_of == stage::callable ? "" : "thread sgl_hit_record const& sgl_hit, ";
            out.appendf("[[visible]] void {}(thread uint4* sgl_payload, {}thread sgl_context const& sgl_ctx)\n{{\n",
                        p.entry_name, hit);
            write_context_bindings(out, p);
            if (!payload.empty())
                out.appendf("    thread {0}& {1} = *reinterpret_cast<thread {0}*>(sgl_payload);\n", payload_type,
                            payload);
            else
                out += "    (void)sgl_payload;\n";
            if (check::is_valid(p.e.attributes))
                out.appendf("    thread {0} const& {1} = *reinterpret_cast<thread {0} const*>(sgl_hit.attributes);\n",
                            type_text(p, *this, p.e.at(p.e.attributes).type), p.locals[index_of(p.e.attributes)]);
        }
        else
        {
            // an any hit decides a triangle, and a procedural group's traversal reports a box's primitive
            auto const is_triangle = stage_of == stage::any_hit;
            out.appendf("[[intersection({}, triangle_data, instancing)]]\n", is_triangle ? "triangle" : "bounding_box");
            out.appendf("{} {}(", is_triangle ? "bool" : "sgl_box_result", p.entry_name);
            if (is_triangle)
                out += "float sgl_distance [[distance]], float2 sgl_barycentrics [[barycentric_coord]], "
                       "bool sgl_front [[front_facing]], ";
            out += "float3 sgl_origin [[origin]], float3 sgl_direction [[direction]], "
                   "float sgl_min [[min_distance]], float sgl_max [[max_distance]], uint sgl_primitive "
                   "[[primitive_id]], "
                   "uint sgl_geometry [[geometry_id]], uint sgl_instance [[instance_id]], "
                   "uint sgl_user_instance [[user_instance_id]], ray_data sgl_ray_data& sgl_data [[payload]])\n{\n";
            out += "    sgl_context const sgl_ctx = sgl_data.context;\n";
            write_context_bindings(out, p);
            // TODO: metal hands a traversal function its instance's transforms only under intersection tags sg's
            // tables do not declare, so a hit's transforms are the identity here
            out.appendf("    sgl_hit_record sgl_hit;\n"
                        "    sgl_hit.t = {};\n"
                        "    sgl_hit.t_min = sgl_min;\n"
                        "    sgl_hit.origin = sgl_data.world_origin;\n"
                        "    sgl_hit.direction = sgl_data.world_direction;\n"
                        "    sgl_hit.object_origin = sgl_origin;\n"
                        "    sgl_hit.object_direction = sgl_direction;\n"
                        "    sgl_hit.instance_id = sgl_user_instance;\n"
                        "    sgl_hit.instance_index = sgl_instance;\n"
                        "    sgl_hit.geometry_index = sgl_geometry;\n"
                        "    sgl_hit.primitive_index = sgl_primitive;\n"
                        "    sgl_hit.barycentrics = {};\n"
                        "    sgl_hit.front_face = {};\n"
                        "    sgl_hit.object_to_world = float4x3(float3(1, 0, 0), float3(0, 1, 0), float3(0, 0, 1), "
                        "float3(0));\n"
                        "    sgl_hit.world_to_object = sgl_hit.object_to_world;\n",
                        is_triangle ? "sgl_distance" : "sgl_max", is_triangle ? "sgl_barycentrics" : "float2(0)",
                        is_triangle ? "sgl_front ? 1u : 0u" : "0u");
            if (!payload.empty())
                out.appendf("    {0} {1} = *reinterpret_cast<ray_data {0}*>(sgl_data.payload);\n", payload_type, payload);
            if (check::is_valid(p.e.attributes))
                out.appendf("    {0} const {1} = *reinterpret_cast<ray_data {0}*>(sgl_data.attributes);\n",
                            type_text(p, *this, p.e.at(p.e.attributes).type), p.locals[index_of(p.e.attributes)]);
        }
    }

    /// MSL has no global resources: the inline constants and every group are parameters, and each is bound to a local
    /// at the top of the body under the name the other targets give their global, so the body reads them alike.
    void write_function_head(cc::string& out, plan const& p) const override
    {
        if (p.e.entry_stage >= stage::raygen)
        {
            write_ray_tracing_head(out, p);
            write_bound_resources(out, p);
            return;
        }
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
        // EMIT-133: a static sampler's slot of the argument table is its index, and MSL has one sampler type
        for (auto const& s : p.samplers)
            parameters.push_back(cc::format("sampler {} [[sampler({})]]", s.name, s.index));
        auto list = cc::string();
        for (auto const& parameter : parameters)
            list += cc::format("{}{}", list.empty() ? "" : ", ", parameter);

        // EMIT-59: MSL states no threadgroup shape, so a kernel carries none; SGL's stated one is what reaches sg
        if (p.e.entry_stage == stage::compute)
            out.appendf("kernel void {}({})\n{{\n", p.entry_name, list);
        else
            out.appendf("{} {} {}({})\n{{\n", p.e.entry_stage == stage::vertex ? "vertex" : "fragment",
                        type_text(p, *this, p.e.result), p.entry_name, list);
        write_bound_resources(out, p);
        for (auto i = isize(0); i < p.e.stage_inputs.size(); ++i)
        {
            auto const local = p.e.stage_inputs[i].local;
            out.appendf("    const {} {} = {};\n", type_text(p, *this, p.e.at(local).type), p.locals[index_of(local)],
                        stage_input_value(p, i));
        }
    }

    static void write_bound_resources(cc::string& out, plan const& p)
    {
        for (auto const& block : p.group_blocks)
            out.appendf("    constant auto& {} = *{}.{};\n", block.name, argument_buffer_of(p, block.group).parameter,
                        block.name);
        for (auto const& r : p.resources)
            out.appendf("    constant auto& {} = {}.{};\n", r.name, argument_buffer_of(p, r.group).parameter, r.name);
    }
};

constexpr auto k_msl = msl_dialect_t();
} // namespace

sgl::emit::impl::dialect const& sgl::emit::impl::msl_dialect()
{
    return k_msl;
}
