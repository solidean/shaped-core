#include "dialect.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/to_string.hh>
#include <shaped-graphics-language/check/reserved_bindings.hh>
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
/// Where metal's `dispatch_rays` binds, per group, the slot-for-slot mirror of each bound TLAS's per-instance hit-group
/// offsets, at the first vertex-buffer slot, which a kernel never has; sg's metal `k_hit_group_offsets_buffer_index`.
constexpr auto k_hit_offsets_buffer = 5;

/// MSL's texture types, parallel to `texture_shape`, and the depth ones where a shape has one.
/// A 1D texture is a 2D one on metal, which sg creates every 1D texture as (EMIT-100).
constexpr cc::string_view k_texture_names[]
    = {"texture2d",          "texture2d_array", "texture2d",   "texture2d_array",  "texture2d_ms",
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
    bool has_line_directives() const override { return true; }

    // a ray-tracing stage is a visible function, which takes no buffer of its own
    // a Metal buffer is a pointer, so the writer reads its length from its group's `sgl_lengths` instead (EMIT-195)
    cc::string buffer_length(cc::string_view, bool) const override { return "0u"; }
    /// MSL's is a function; whether it terminates or demotes the pixel is for the metal backend's tests to pin.
    [[nodiscard]] cc::string_view discard_statement() const override { return "discard_fragment();"; }
    [[nodiscard]] bool fences_image_stores() const override { return true; }

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
        if (member.is_layer)
            return "render_target_array_index";
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
        // EMIT-209: what a mesh stage writes per primitive, its locations past the stage link's
        case struct_role::primitive_link:
            return cc::format("user(sgl{})", member.location);
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

    void write_member(cc::string& out,
                      planned_struct const* owner,
                      planned_member const& member,
                      plan const& p,
                      bool is_packed_tail = false) const
    {
        // EMIT-172: an array of one value per row is one of 4-vectors
        auto const type
            = member.is_row_per_element
                ? cc::format("array<{}, {}>", builtin_spelling(p, row_vector_of(p, p.m.at(member.type).element)),
                             p.m.at(member.type).count)
                : cc::string(type_text(p, *this, member.type));
        out.appendf("{}{}{} {}", k_indent, is_packed_tail ? "packed_" : "", type, member.name);
        if (owner != nullptr)
            if (auto const attribute = attribute_of(*owner, member); !attribute.empty())
                out.appendf(" [[{}]]", attribute);
        out += ";\n";
    }

    /// The alignment MSL gives a value of `type` in memory as this text declares it: a constant block's struct is
    /// `alignas(16)`, and a struct with a packed tail says its own (EMIT-176).
    [[nodiscard]] static i32 msl_alignment_of(plan const& p, type_id type)
    {
        if (auto const* const record = p.m.builtin_type_of(type))
            return record->msl_layout.alignment > 0 ? record->msl_layout.alignment : 4;
        auto const& t = p.m.at(type);
        if (t.kind == type_kind::array)
            return msl_alignment_of(p, t.element);
        if (t.kind != type_kind::structure || p.struct_of_type[index_of(type)] < 0)
            return 4;
        auto const& s = p.structs[p.struct_of_type[index_of(type)]];
        if (s.memory_size > 0 && s.is_constants && !s.is_packed)
            return 16;
        auto result = 4;
        for (auto i = isize(0); i < s.members.size(); ++i)
        {
            if (shares_tail(p, s, i))
                return s.memory_alignment;
            auto const a = msl_alignment_of(p, s.members[i].type);
            result = a > result ? a : result;
        }
        return result;
    }

    /// EMIT-170: the members of a struct or a block in natural memory, a shared tail packed.
    /// Wherever MSL would place a member short of its offset, a padding field keeps it there: a packed 3-vector aligns to
    /// its scalar alone, and a constant block starts a struct, an array and a mat2 on a row MSL does not (EMIT-175).
    void write_memory_members(cc::string& out,
                              planned_struct const* owner,
                              cc::span<planned_member const> members,
                              plan const& p) const
    {
        auto at = 0;
        auto padding = 0;
        for (auto i = isize(0); i < members.size(); ++i)
        {
            auto const& member = members[i];
            auto const is_packed_tail = shares_tail(p, members, i);
            auto const alignment = is_packed_tail ? 4 : msl_alignment_of(p, member.type);
            if (member.offset >= 0 && (at + alignment - 1) / alignment * alignment < member.offset)
                while (at < member.offset)
                {
                    auto const width = member.offset - at >= 4 && at % 4 == 0 ? 4 : 2;
                    out.appendf("{}{} sgl_pad{};\n", k_indent, width == 4 ? "uint" : "ushort", padding++);
                    at += width;
                }
            write_member(out, owner, member, p, is_packed_tail);
            if (member.offset >= 0)
            {
                auto const* const record = p.m.builtin_type_of(member.type);
                auto const is_full_3_vector = record != nullptr && record->leaf_count == 3 && !is_packed_tail;
                // a struct of a constant block is `alignas(16)`, so MSL rounds its size up to a row
                auto const is_row_struct = record == nullptr && p.m.at(member.type).kind == type_kind::structure
                                        && msl_alignment_of(p, member.type) == 16;
                at = member.offset
                   + (is_full_3_vector ? record->msl_layout.size
                      : is_row_struct  ? (member.size + 15) / 16 * 16
                                       : member.size);
            }
        }
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
        case type_kind::bytes:
            return t.is_mut ? "device uint*" : "const device uint*";
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
        // a reserved binding is no group of the argument table, and the function head declares it
        if (!buffers.empty() && buffers[0].is_reserved)
            return;
        for (auto const& b : buffers)
            if (b.element_form.has_value())
                write_form(out, b.element_form.value());
        if (block != nullptr)
            write_block_struct(out, p, *block);

        auto const type_of = [&](planned_resource const& b)
        {
            auto type = b.element_form.has_value()
                          ? cc::format("{}device {}*", b.is_mut ? "" : "const ", b.element_form.value().name)
                          : resource_text(p, b.type);
            // EMIT-150: a buffer's coherence qualifies the memory it points at, and a texture's is its last template
            // argument, since an argument buffer's field takes no qualifier
            if (b.is_coherent && type.starts_with("device "))
                type = cc::format("device coherent(device) {}", type.subview({.start = 7, .end = type.size()}));
            else if (b.is_coherent && type.ends_with(">"))
                type = cc::format("{}, memory_coherence_device>", type.subview({.start = 0, .end = type.size() - 1}));
            return type;
        };
        // EMIT-211: an unbounded array is a table its group sizes, at the address its slot holds, since no member of an
        // argument buffer is unsized: its count, its entries, and their byte lengths behind them
        for (auto const& b : buffers)
            if (b.count == 0)
                out.appendf("struct {}\n{{\n{}uint count;\n{}uint sgl_pad;\n{}{} entries[1];\n}};\n\n", table_of(b),
                            k_indent, k_indent, k_indent, type_of(b));

        auto const group = block != nullptr ? block->group : buffers[0].group;
        out.appendf("struct {}\n{{\n", argument_buffer_of(p, group).struct_name);
        if (block != nullptr)
            out.appendf("{}constant {}* {} [[id({})]];\n", k_indent, block->block_name, block->name, block->slot);
        for (auto const& b : buffers)
        {
            if (b.count == 0)
            {
                out.appendf("{}constant {}* {} [[id({})]];\n", k_indent, table_of(b), b.name, b.slot);
                continue;
            }
            out.appendf("{}{} {} [[id({})]]", k_indent, type_of(b), b.name, b.slot);
            if (b.count > 1)
                out.appendf("[{}]", b.count);
            out += ";\n";
        }
        // EMIT-195: each slot's byte length, which sg writes behind the slots, a buffer's and zero for anything else
        auto slots = block != nullptr ? block->slot + 1 : 0;
        for (auto const& b : buffers)
            slots = cc::max(slots, b.slot + cc::max(b.count, 1));
        out.appendf("{}uint sgl_lengths [[id({})]][{}];\n", k_indent, slots, slots);
        out += "};\n\n";
    }

    /// EMIT-211: the struct of an unbounded array's table.
    [[nodiscard]] static cc::string table_of(planned_resource const& b) { return cc::format("{}_table", b.name); }

    [[nodiscard]] static planned_argument_buffer const& argument_buffer_of(plan const& p, i32 group)
    {
        for (auto const& a : p.argument_buffers)
            if (a.group == group)
                return a;
        CC_UNREACHABLE("every group MSL writes was given an argument buffer by the plan");
    }

    /// EMIT-177: Metal's compiler checks that `name` holds `field` at byte `offset`, as SGL placed it.
    static void write_offset_assert(cc::string& out, cc::string_view name, cc::string_view field, i32 offset)
    {
        out.appendf("static_assert(__builtin_offsetof({}, {}) == {}, \"SGL places {}.{} at byte {}\");\n", name, field,
                    offset, name, field, offset);
    }

    /// EMIT-177: the offset of every member SGL placed, and the struct's size where it is `size` > 0.
    static void write_layout_asserts(cc::string& out, cc::string_view name, cc::span<planned_member const> members, i32 size)
    {
        for (auto const& member : members)
            if (member.offset >= 0)
                write_offset_assert(out, name, member.name, member.offset);
        if (size > 0)
            out.appendf("static_assert(sizeof({}) == {}, \"SGL makes {} {} bytes\");\n", name, size, name, size);
        out += "\n";
    }

    /// A root's memory form: its pieces as fields, each where SGL's layout puts it (memory_form.hh).
    static void write_form(cc::string& out, memory_form const& form)
    {
        out.appendf("struct {}\n{{\n", form.name);
        for (auto const& f : form.fields)
            out.appendf("{}{} {};\n", k_indent,
                        f.count > 0 ? cc::format("array<{}, {}>", f.type, f.count) : cc::string(f.type), f.name);
        out += "};\n\n";
        for (auto const& f : form.fields)
            write_offset_assert(out, form.name, f.name, f.offset);
        out += "\n";
    }

    void write_block_struct(cc::string& out, plan const& p, planned_constants const& block) const
    {
        if (block.form.has_value())
            return write_form(out, block.form.value());
        out.appendf("struct {}\n{{\n", block.block_name);
        write_memory_members(out, nullptr, block.members, p);
        out += "};\n\n";
        // a block's own size is MSL's to round, since the host writes it member by member
        write_layout_asserts(out, block.block_name, block.members, 0);
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

    /// Whether the entry point names `metal::raytracing`: a ray-tracing stage, or any stage that binds an acceleration
    /// structure or traces one inline.
    [[nodiscard]] static bool uses_ray_tracing(plan const& p)
    {
        if (p.e.entry_stage >= stage::raygen)
            return true;
        for (auto const& r : p.resources)
            if (p.m.at(r.type).kind == type_kind::acceleration_structure)
                return true;
        for (auto const& x : p.e.exprs)
            if (auto const* const call = x.node.try_as<flat_call>())
                if (auto const* const record = p.m.builtin_function(call->intrinsic);
                    record != nullptr && record->takes_acceleration_index)
                    return true;
        return false;
    }

    void write_declarations(cc::string& out, plan const& p) const override
    {
        // EMIT-262: the text states its math mode, which the Metal compiler takes over its own options, so an entry point
        // that keeps no float guarantee compiles fast and one that keeps any compiles safe, whatever the compile asked
        out += p.float_guarantee == check::float_level::fast ? "#pragma METAL fp math_mode(fast)\n"
                                                             : "#pragma METAL fp math_mode(safe)\n";
        out += "#include <metal_stdlib>\nusing namespace metal;\n\n";
        if (uses_ray_tracing(p))
        {
            out += "#include <metal_raytracing>\nusing namespace raytracing;\n\n";
            // EMIT-139: what closes every native trace's tags, motion only where a structure that moves can be bound
            out += p.sees_motion ? "#define sgl_motion , instance_motion, primitive_motion\n"
                                   "#define sgl_motion_time(t) , t\n\n"
                                 : "#define sgl_motion\n#define sgl_motion_time(t)\n\n";
        }
        write_enum_constants(out, p, *this);
        // A struct stands ahead of the groups, whose blocks and buffers may hold it.
        for (auto const& s : p.structs)
        {
            // EMIT-170: a packed member lowers the struct's own alignment, which `alignas` restores
            auto is_packing = false;
            for (auto i = isize(0); i < s.members.size(); ++i)
                is_packing = is_packing || shares_tail(p, s, i);
            // a struct in a constant block strides by whole rows, and starts the row after it (EMIT-110)
            auto const in_constants = s.memory_size > 0 && s.is_constants && !s.is_packed;
            if (is_packing || in_constants)
                out.appendf("struct alignas({}) {}\n{{\n", in_constants ? 16 : s.memory_alignment, s.name);
            else
                out.appendf("struct {}\n{{\n", s.name);
            if (s.memory_size > 0 && !s.is_packed)
                write_memory_members(out, &s, s.members, p);
            else
                for (auto const& member : declared_members(s))
                    write_member(out, &s, member, p);
            out += "};\n\n";
            // a struct of a constant block is a whole number of rows, which its `alignas(16)` gives it
            if (s.memory_size > 0 && !s.is_packed)
                write_layout_asserts(out, s.name, s.members,
                                     in_constants ? (s.memory_size + 15) / 16 * 16 : s.memory_size);
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
        // sg mirrors each group's argument buffer slot for slot with the offsets of the structure bound there, and
        // binds one such mirror per group
        // a struct at each level, since a buffer's pointee may hold a pointer only as an argument buffer does
        out += "struct sgl_slot_hit_offsets\n{\n    device uint const* instances;\n};\n\n"
               "struct sgl_hit_offsets_table\n{\n    device sgl_slot_hit_offsets const* groups[3];\n};\n\n";
        out += "struct sgl_context\n{\n"
               "    constant void* groups[3];\n"
               "    constant void* constants;\n"
               "    device void* tables;\n"
               "    constant sgl_hit_offsets_table* hit_offsets;\n"
               "    uint3 launch_id;\n"
               "    uint3 launch_size;\n"
               "    device atomic_uint* debug;\n"
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
        out += "struct sgl_triangle_result\n{\n"
               "    bool accept [[accept_intersection]];\n"
               "    bool continue_search [[continue_search]];\n"
               "};\n\n";
        out += "struct sgl_box_result\n{\n"
               "    bool accept [[accept_intersection]];\n"
               "    float distance [[distance]];\n"
               "    bool continue_search [[continue_search]];\n"
               "};\n\n";
        out += "using sgl_hit_function = void(thread uint4*, thread sgl_hit_record const&, thread sgl_context "
               "const&);\n"
               "using sgl_callable_function = void(thread uint4*, thread sgl_context const&);\n\n";
        // sg's reserved group: ray type 0's intersection table at id 0, then miss, closest hit and callable, then the
        // other ray types' intersection tables from id 4
        out += "struct sgl_tables\n{\n"
               "    intersection_function_table<triangle_data, instancing, world_space_data" SGL_MSL_MOTION_TAGS
               "> hit_0 [[id(0)]];\n"
               "    visible_function_table<sgl_hit_function> miss [[id(1)]];\n"
               "    visible_function_table<sgl_hit_function> closest_hit [[id(2)]];\n"
               "    visible_function_table<sgl_callable_function> callable [[id(3)]];\n";
        for (auto r = isize(1); r < ray_count; ++r)
            out.appendf("    intersection_function_table<triangle_data, instancing, "
                        "world_space_data" SGL_MSL_MOTION_TAGS "> hit_{} "
                        "[[id({})]];\n",
                        r, 3 + r);
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
        // EMIT-183: a stage that prints reaches the window its raygen was handed
        for (auto const& r : p.resources)
            if (r.is_reserved && r.name == "sg_debug")
                out += "    device atomic_uint* sg_debug = sgl_ctx.debug;\n";
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
            parameters.push_back(
                cc::format("constant sgl_hit_offsets_table& sgl_hit_offsets [[buffer({})]]", k_hit_offsets_buffer));
            // EMIT-183: the debug window, which the raygen hands every stage it calls, since none of them is a kernel
            if (p.diagnostics)
                parameters.push_back(
                    cc::format("device atomic_uint* sg_debug [[buffer({})]]",
                               check::reserved_binding_named("sg_debug")->targets[u8(emit::target::msl)].slot));
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
                   "    sgl_ctx.hit_offsets = &sgl_hit_offsets;\n"
                   "    sgl_ctx.launch_id = sgl_launch_id;\n"
                   "    sgl_ctx.launch_size = sgl_launch_size;\n";
            out.appendf("    sgl_ctx.debug = {};\n", p.diagnostics ? "sg_debug" : "nullptr");
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
            out.appendf("[[intersection({}, triangle_data, instancing, world_space_data" SGL_MSL_MOTION_TAGS ")]]\n",
                        is_triangle ? "triangle" : "bounding_box");
            out.appendf("{} {}(", is_triangle ? "sgl_triangle_result" : "sgl_box_result", p.entry_name);
            if (is_triangle)
                out += "float sgl_distance [[distance]], float2 sgl_barycentrics [[barycentric_coord]], "
                       "bool sgl_front [[front_facing]], ";
            out += "float3 sgl_origin [[origin]], float3 sgl_direction [[direction]], "
                   "float sgl_min [[min_distance]], float sgl_max [[max_distance]], uint sgl_primitive "
                   "[[primitive_id]], "
                   "uint sgl_geometry [[geometry_id]], uint sgl_instance [[instance_id]], "
                   "uint sgl_user_instance [[user_instance_id]], "
                   "float4x3 sgl_object_to_world [[object_to_world_transform]], "
                   "float4x3 sgl_world_to_object [[world_to_object_transform]], "
                   "ray_data sgl_ray_data& sgl_data [[payload]])\n{\n";
            out += "    sgl_context const sgl_ctx = sgl_data.context;\n";
            write_context_bindings(out, p);
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
                        "    sgl_hit.object_to_world = sgl_object_to_world;\n"
                        "    sgl_hit.world_to_object = sgl_world_to_object;\n",
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
        // CHK-489: a pixel stage's second struct, what a mesh stage wrote per primitive, arrives in one stage input
        // beside the link, as Metal hands a mesh pipeline's fragment both
        auto per_primitive = isize(-1);
        if (p.e.entry_stage == stage::pixel && check::is_valid(p.e.input))
            for (auto i = isize(1); i < p.e.locals.size(); ++i)
            {
                auto const& local = p.e.locals[i];
                auto is_input = false;
                for (auto const& s : p.e.stage_inputs)
                    is_input = is_input || index_of(s.local) == i;
                if (local.kind == check::local_kind::parameter && !is_input
                    && p.m.at(local.type).kind == type_kind::structure && local.type != p.e.input)
                    per_primitive = i;
            }
        if (per_primitive >= 0)
        {
            out.appendf("struct sgl_stage_in\n{{\n    {} link;\n    {} primitive;\n}};\n\n",
                        type_text(p, *this, p.e.input), type_text(p, *this, p.e.locals[per_primitive].type));
            parameters.push_back("sgl_stage_in sgl_in [[stage_in]]");
        }
        else if (check::is_valid(p.e.input) && p.e.entry_stage != stage::mesh && p.e.entry_stage != stage::task)
            parameters.push_back(cc::format("{} {} [[stage_in]]", type_text(p, *this, p.e.input), p.locals[0]));
        // EMIT-206: a mesh stage's output is Metal's mesh object, and the struct it is handed its object stage's payload
        if (p.e.entry_stage == stage::mesh)
            for (auto i = isize(0); i < p.e.locals.size(); ++i)
            {
                auto const& local = p.e.locals[i];
                auto is_input = false;
                for (auto const& s : p.e.stage_inputs)
                    is_input = is_input || index_of(s.local) == i;
                if (local.kind != check::local_kind::parameter || is_input)
                    continue;
                auto const& t = p.m.at(local.type);
                auto const& mesh = p.m.functions[p.m.at(p.e.function).info];
                if (t.kind == type_kind::stream && t.count == 0)
                    parameters.push_back(cc::format(
                        "metal::mesh<{}, {}, {}, {}, metal::topology::{}> {}", type_text(p, *this, t.element),
                        check::is_valid(t.primitive) ? cc::string(type_text(p, *this, t.primitive)) : cc::string("void"),
                        mesh.max_vertices, mesh.max_primitives, mesh.is_line_mesh ? "line" : "triangle", p.locals[i]));
                else if (t.kind == type_kind::structure)
                    parameters.push_back(cc::format("const object_data {}& {} [[payload]]",
                                                    type_text(p, *this, local.type), p.locals[i]));
            }
        // EMIT-207: an object stage writes its payload and its grid of mesh workgroups from its first invocation
        if (p.e.entry_stage == stage::task)
        {
            parameters.push_back(cc::format("object_data {}& sgl_payload [[payload]]", payload_type(p)));
            parameters.push_back("mesh_grid_properties sgl_grid");
            parameters.push_back("uint sgl_thread [[thread_index_in_threadgroup]]");
        }
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
        // a reserved binding is a parameter at its own slot of the argument table
        for (auto const& r : p.resources)
            if (r.is_reserved)
                parameters.push_back(cc::format("{} {} [[buffer({})]]", resource_text(p, r.type), r.name, r.slot));
        // EMIT-135: an emulated trace reads sg's pool and the dispatch's roots, at their slots of the argument table
        if (traces_in_software(p))
        {
            auto const& pool = check::reserved_binding_named("sg_acceleration_pool")->targets[u8(emit::target::msl)];
            auto const& roots = check::reserved_binding_named("sg_acceleration_roots")->targets[u8(emit::target::msl)];
            parameters.push_back(cc::format("const device uint4* sg_acceleration_pool [[buffer({})]]", pool.slot));
            parameters.push_back(cc::format("constant uint4* sg_acceleration_roots [[buffer({})]]", roots.slot));
        }
        auto list = cc::string();
        for (auto const& parameter : parameters)
            list += cc::format("{}{}", list.empty() ? "" : ", ", parameter);

        // EMIT-59: MSL states no threadgroup shape, so a kernel carries none; SGL's stated one is what reaches sg
        if (p.e.entry_stage == stage::compute)
            out.appendf("kernel void {}({})\n{{\n", p.entry_name, list);
        else if (p.e.entry_stage == stage::mesh || p.e.entry_stage == stage::task)
            out.appendf("[[{}]] void {}({})\n{{\n", p.e.entry_stage == stage::mesh ? "mesh" : "object", p.entry_name,
                        list);
        else
            out.appendf("{} {} {}({})\n{{\n", p.e.entry_stage == stage::vertex ? "vertex" : "fragment",
                        type_text(p, *this, p.e.result), p.entry_name, list);
        write_bound_resources(out, p);
        if (per_primitive >= 0)
        {
            out.appendf("    const {} {} = sgl_in.link;\n", type_text(p, *this, p.e.input), p.locals[0]);
            out.appendf("    const {} {} = sgl_in.primitive;\n", type_text(p, *this, p.e.locals[per_primitive].type),
                        p.locals[per_primitive]);
        }
        for (auto i = isize(0); i < p.e.stage_inputs.size(); ++i)
        {
            if (!p.stage_input_read[i])
                continue;
            auto const local = p.e.stage_inputs[i].local;
            out.appendf("    const {} {} = {};\n", type_text(p, *this, p.e.at(local).type), p.locals[index_of(local)],
                        stage_input_value(p, i));
        }
    }

    /// The payload an object stage hands its mesh workgroups: the struct its `dispatch_mesh` passes (EMIT-207).
    [[nodiscard]] cc::string payload_type(plan const& p) const
    {
        for (auto const& x : p.e.exprs)
            if (auto const* const call = x.node.try_as<check::flat_call>())
                if (auto const* const record = p.m.builtin_function(call->intrinsic);
                    record != nullptr && record->name == "dispatch_mesh" && p.e.at(call->arguments).size() > 3)
                    return cc::string(type_text(p, *this, p.e.at(p.e.at(call->arguments)[3]).type));
        CC_UNREACHABLE("a task stage launches its mesh workgroups once (CHK-485)");
    }

    /// Whether the entry point walks a structure in software, which reads sg's pool and roots (EMIT-135).
    [[nodiscard]] static bool traces_in_software(plan const& p)
    {
        for (auto const& x : p.e.exprs)
            if (auto const* const call = x.node.try_as<check::flat_call>())
                if (auto const* const record = p.m.builtin_function(call->intrinsic);
                    record != nullptr && (record->takes_acceleration_index || record->name == "acceleration_pool_load"))
                    return true;
        return false;
    }

    static void write_bound_resources(cc::string& out, plan const& p)
    {
        for (auto const& block : p.group_blocks)
            out.appendf("    constant auto& {} = *{}.{};\n", block.name, argument_buffer_of(p, block.group).parameter,
                        block.name);
        for (auto const& r : p.resources)
            if (r.is_reserved)
                continue;
            // EMIT-211: an unbounded array is read through its table's entries
            else if (r.count == 0)
                out.appendf("    auto const {} = &{}.{}->entries[0];\n", r.name,
                            argument_buffer_of(p, r.group).parameter, r.name);
            else
                out.appendf("    constant auto& {} = {}.{};\n", r.name, argument_buffer_of(p, r.group).parameter, r.name);
    }
};

constexpr auto k_msl = msl_dialect_t();
} // namespace

sgl::emit::impl::dialect const& sgl::emit::impl::msl_dialect()
{
    return k_msl;
}
