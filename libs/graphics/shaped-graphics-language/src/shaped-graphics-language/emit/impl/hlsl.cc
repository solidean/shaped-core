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
/// `ray_flags.force_omm_2_state`, the one ray flag a query must not carry on vulkan without micromaps stated for it.
constexpr auto k_ray_flag_force_omm_2_state = 1024u;

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
    bool has_line_directives() const override { return true; }

    // vulkan sees micromaps without the flag, and DXC takes minutes over it and can write invalid SPIR-V; it takes a
    // ray forced to 2 states only beside it, so such a query keeps it, and so does one whose flags are no constant
    bool flags_query_for_micromaps(cc::optional<u32> flags) const override
    {
        return !_is_vulkan || !flags.has_value() || (flags.value() & k_ray_flag_force_omm_2_state) != 0;
    }

    // vulkan writes no storage from a vertex, geometry or tessellation stage without vertexPipelineStoresAndAtomics,
    // which sg does not ask a device for
    bool writes_debug_in(check::stage s) const override
    {
        return !_is_vulkan
            || (s != check::stage::vertex && s != check::stage::geometry && s != check::stage::tessellation_control
                && s != check::stage::tessellation_evaluation);
    }
    void write_debug_buffer(cc::string& out) const override
    {
        if (_is_vulkan)
            out += "[[vk::binding(19, 3)]] RWByteAddressBuffer sg_debug;\n\n";
        else
            out += "RWByteAddressBuffer sg_debug : register(u0, space11);\n\n";
    }
    cc::string debug_reserve(cc::string_view at, isize count) const override
    {
        return cc::format("sg_debug.InterlockedAdd(0, {}, {});", count, at);
    }
    cc::string debug_declare(cc::string_view at) const override { return cc::format("uint {};", at); }
    cc::string debug_fits(cc::string_view at, isize count) const override
    {
        return cc::format("{} + {} <= sg_debug.Load(4)", at, count);
    }
    cc::string debug_store(cc::string_view at, isize i, cc::string_view word) const override
    {
        return cc::format("sg_debug.Store(12 + ({} + {}) * 4, {});", at, i, word);
    }
    cc::string debug_drop() const override { return "sg_debug.InterlockedAdd(8, 1);"; }
    cc::string debug_word(cc::string_view value, cc::string_view family) const override
    {
        if (family == "uint")
            return cc::string(value);
        if (family == "bool")
            return cc::format("({} ? 1u : 0u)", value);
        if (family == "half")
            return cc::format("asuint(float({}))", value);
        return cc::format("asuint({})", value);
    }
    cc::string_view rolled_loop_hint() const override { return "[loop]"; }

    void write_for_head(cc::string& out, cc::string_view index, cc::string_view first, cc::string_view end) const override
    {
        out.appendf("for (int {} = {}; {} < {}; ++{})", index, first, index, end, index);
    }

    void write_local(cc::string& out, local_declaration const& local) const override
    {
        if (local.value.empty() && local.is_zeroed)
            out.appendf("{} {} = ({})0;", local.type, local.name, local.type);
        else if (local.value.empty())
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
        if (member.is_layer)
            return "SV_RenderTargetArrayIndex";
        switch (s.role)
        {
        case struct_role::vertex_input:
        {
            // EMIT-174: a piece is its member's semantic at its own index, `FRAME0`, which is how dx12 reads one
            auto const at = member.whole >= 0 ? member.whole : &member - s.members.data();
            auto const semantics = vertex_semantics(p.m, p.m.at(s.type));
            for (auto i = isize(0); i < s.member_of.size(); ++i)
                if (s.member_of[i] == at)
                    return member.piece_index >= 0 ? cc::format("{}{}", semantics[i], member.piece_index) : semantics[i];
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
        // EMIT-168: a mat3 in memory is three whole rows, which no packed layout and no function body holds
        auto const in_memory = member.offset >= 0 && (owner == nullptr || (owner->memory_size > 0 && !owner->is_packed));
        auto const is_widened = in_memory && is_mat3(p.m, innermost_of(p.m, member.type));
        // EMIT-169: an array of 3-vectors in memory is one of 4-vectors, which a structured buffer strides by 16 too
        auto const widened_vector = member.is_row_per_element
                                      ? cc::string(builtin_spelling(p, row_vector_of(p, p.m.at(member.type).element)))
                                  : in_memory ? widened_vector_of(p, member.type)
                                              : cc::string();
        // EMIT-175: a mat2 in a constant block is its two columns in one row, which no HLSL matrix there is
        auto const in_constants = owner == nullptr || owner->is_constants;
        auto const is_vector_mat2 = in_memory && in_constants && is_mat2(p.m, member.type);
        auto const type = is_widened              ? cc::string("float4x3")
                        : is_vector_mat2          ? cc::string("float4")
                        : !widened_vector.empty() ? widened_vector
                                                  : cc::string(type_text(p, *this, member.type));
        // Stated on every matrix, so no `-Zpr` and no `#pragma pack_matrix` can turn one around.
        if (type == "float4x4" || type == "float4x3" || type == "float3x3")
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
        out.appendf("{} {}{}", type, member.name, array_dimensions(p, member.type));
        if (owner != nullptr)
            if (auto const semantic = semantic_of(*owner, member, p); !semantic.empty())
                out.appendf(" : {}", semantic);
        // EMIT-137: a payload's field states which shaders read and write it, as every shader of the module does
        if (owner != nullptr && is_payload(p, owner->type))
            out += access_text(payload_access_of(p, owner->type, member.source_name));
        out += ";\n";
    }

    /// EMIT-112: padding fields from `at` up to `offset`, in words, and one 16-bit field where 2 bytes are left.
    /// HLSL's own packing closes every gap a value fits into, so filled gaps land every member at its offset.
    void write_padding(cc::string& out, i32& at, i32 offset, i32& count) const
    {
        while (at < offset)
        {
            auto const width = offset - at >= 4 && at % 4 == 0 ? 4 : 2;
            out += k_indent;
            if (_is_vulkan)
                out.appendf("[[vk::offset({})]] ", at);
            out.appendf("{} sgl_pad{};\n", width == 4 ? "uint" : "uint16_t", count++);
            at += width;
        }
    }

    /// The members of a struct or a block, padded where they stand in GPU memory and as declared everywhere else.
    void write_members(cc::string& out,
                       planned_struct const* owner,
                       cc::span<planned_member const> members,
                       i32 memory_size,
                       plan const& p) const
    {
        auto at = 0;
        auto count = 0;
        for (auto const& member : members)
        {
            if (member.offset >= 0)
            {
                write_padding(out, at, member.offset, count);
                at = member.offset + member.size;
            }
            write_member(out, owner, member, p);
        }
        if (memory_size > 0)
            write_padding(out, at, memory_size, count);
    }

    /// The 4-vector HLSL holds an array of 3-vectors as in GPU memory (EMIT-169), or empty for any other type.
    [[nodiscard]] cc::string widened_vector_of(plan const& p, type_id type) const
    {
        if (p.m.builtin_type_of(type) != nullptr || p.m.at(type).kind != type_kind::array)
            return {};
        auto const* const record = p.m.builtin_type_of(innermost_of(p.m, type));
        if (record == nullptr || record->leaf_count != 3)
            return {};
        auto const family = record->leaf_kind == check::value_kind::scalar_int    ? "int4"
                          : record->leaf_kind == check::value_kind::scalar_uint   ? "uint4"
                          : record->leaf_kind == check::value_kind::scalar_half   ? "half4"
                          : record->leaf_kind == check::value_kind::scalar_short  ? "short4"
                          : record->leaf_kind == check::value_kind::scalar_ushort ? "ushort4"
                                                                                  : "float4";
        return cc::string(builtin_spelling(p, family));
    }

    /// Whether any root holds a mat3 as three whole rows, which a write widens through `sgl_widen_mat3`.
    [[nodiscard]] static bool holds_widened_mat3(plan const& p)
    {
        auto result = false;
        for (auto const& s : p.structs)
            if (s.memory_size > 0 && !s.is_packed)
                for (auto const& member : s.members)
                    result = result || is_mat3(p.m, innermost_of(p.m, member.type));
        for (auto const& r : p.resources)
            if (r.element_form.has_value())
                for (auto const& leaf : r.element_form.value().leaves)
                    result = result || leaf.is_widened;
        return result;
    }

    void write_enum_constant(cc::string& out, cc::string_view name, i32 value) const override
    {
        out.appendf("static const int {} = {};\n", name, value);
    }

    /// A memory form's fields in memory order.
    /// Every field states its offset on vulkan, as the push-constant block's do, so no compiler flag decides the layout.
    /// dx12 states none: each field of a form fits its row and padding fills every gap, so its own packing lands each
    /// at its offset.
    void write_form(cc::string& out, memory_form const& form) const
    {
        out.appendf("struct {}\n{{\n", form.name);
        for (auto const& f : form.fields)
        {
            out += k_indent;
            if (_is_vulkan)
                out.appendf("[[vk::offset({})]] ", f.offset);
            auto const is_matrix
                = f.type == "float4x4" || f.type == "float4x3" || f.type == "float3x3" || f.type == "float2x2";
            out.appendf("{}{} {}{};\n", is_matrix ? "column_major " : "", f.type, f.name,
                        f.count > 0 ? cc::format("[{}]", f.count) : cc::string());
        }
        out += "};\n\n";
    }

    /// A block's struct: its members with their padding, or its memory form where it has one (EMIT-154).
    void write_block_struct(cc::string& out, plan const& p, planned_constants const& block) const
    {
        if (block.form.has_value())
            return write_form(out, block.form.value());
        out.appendf("struct {}\n{{\n", block.block_name);
        write_members(out, nullptr, block.members, 0, p);
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
        for (auto const& b : buffers)
            if (b.element_form.has_value())
                write_form(out, b.element_form.value());
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
        auto const resource = b.element_form.has_value()
                                ? cc::format("{}StructuredBuffer<{}>", t.is_mut ? "RW" : "", b.element_form.value().name)
                                : resource_text(p, b.type);
        auto const type = b.is_coherent ? cc::format("globallycoherent {}", resource) : resource;
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
        for (auto const& access : p.accesses)
            if (access.held == held_as::vector_mat2)
            {
                out += "float2x2 sgl_narrow_mat2(float4 m)\n{\n    return float2x2(m.x, m.z, m.y, m.w);\n}\n\n";
                break;
            }
        if (holds_widened_mat3(p))
            out += "float4x3 sgl_widen_mat3(float3x3 m)\n{\n    return float4x3(m[0], m[1], m[2], float3(0.0, 0.0, "
                   "0.0));\n}\n\n";
        // A struct stands ahead of the groups, whose blocks and buffers may hold it.
        for (auto const& s : p.structs)
        {
            out.appendf("struct {}{}\n{{\n", is_payload(p, s.type) ? "[raypayload] " : "", s.name);
            write_members(out, &s, declared_members(s), s.memory_size, p);
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
            if (!p.stage_input_read[i])
                continue;
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
        // EMIT-184: DXC lowers a query's micromap flags to SPIR-V without declaring the capability they need
        if (_is_vulkan && p.e.features.has(check::feature::opacity_micromap) && flags_some_query_for_micromaps(p))
            out += "[[vk::ext_extension(\"SPV_EXT_opacity_micromap\")]]\n[[vk::ext_capability(5381)]]\n";
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
            // An instance's base is `@first_instance` itself where the entry takes that, since a semantic is taken once.
            auto takes_first_instance = false;
            for (auto const& other : p.e.stage_inputs)
                takes_first_instance = takes_first_instance || other.input == check::stage_input::first_instance;
            auto const base_is_shared
                = p.e.stage_inputs[i].input == check::stage_input::instance_index && takes_first_instance;
            if (has_base(p, p.e.stage_inputs[i].input) && !base_is_shared)
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
            if (!p.stage_input_read[i])
                continue;
            auto const local = p.e.stage_inputs[i].local;
            out.appendf("    const {} {} = {};\n", type_text(p, *this, p.e.at(local).type), p.locals[index_of(local)],
                        stage_input_value(p, i));
        }
    }

private:
    /// Whether any ray query of the entry point is written to see micromaps, which its capability is then declared for.
    /// A query is a local a `declares_only` call declares, as the body writes it.
    [[nodiscard]] bool flags_some_query_for_micromaps(plan const& p) const
    {
        for (auto const& st : p.e.stmts)
        {
            auto local = local_id::none;
            auto value = flat_expr_id::none;
            if (auto const* const let = st.node.try_as<flat_let>())
            {
                local = let->local;
                value = let->value;
            }
            else if (auto const* const var = st.node.try_as<flat_var>())
            {
                local = var->local;
                value = var->value;
            }
            if (!is_valid(value))
                continue;
            auto const* const call = p.e.at(value).node.try_as<flat_call>();
            auto const* const record = call != nullptr ? p.m.builtin_function(call->intrinsic) : nullptr;
            auto const* const type_record = p.m.builtin_type_of(p.e.at(local).type);
            if (record != nullptr && record->declares_only && type_record != nullptr
                && type_record->hlsl_flagged != nullptr && flags_query_for_micromaps(p.local_flags[index_of(local)]))
                return true;
        }
        return false;
    }

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
