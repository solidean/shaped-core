#include "describe.hh"

#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/resources.hh>
#include <shaped-graphics-language/check/structural_hash.hh>
#include <shaped-graphics-language/driver/impl/describe_binding.hh>
#include <shaped-graphics-language/driver/impl/front_end.hh>
#include <shaped-graphics-language/emit/impl/layout.hh>
#include <shaped-graphics-language/emit/impl/plan.hh>
#include <shaped-graphics-language/legalize/legalize.hh>

using namespace sgl;

namespace
{
namespace emit_impl = sgl::emit::impl;

/// What kind of sampler `member` is to a layout: a static one says so by its settings, a bound one by its declaration.
cc::string_view sampler_type_of(check::checked_module const& m, check::member_info const& member)
{
    auto const& t = m.at(member.type);
    if (t.is_comparison)
        return "comparison";
    if (member.is_non_filtering)
        return "non_filtering";
    if (member.static_sampler >= 0)
    {
        auto const& state = m.samplers[member.static_sampler];
        if (state.min_filter == 0 && state.mag_filter == 0 && state.mip_filter == 0)
            return "non_filtering";
    }
    return "filtering";
}

/// A texture's `sg::texture_sample_type`, which the declaration states whole (the spec's bindings file, "Sample types").
cc::string_view sample_type_of(check::checked_module const& m, check::member_info const& member)
{
    auto const& t = m.at(member.type);
    if (t.is_depth)
        return "depth";
    auto const element = m.name_of(t.element);
    if (element.starts_with("uint"))
        return "uint";
    if (element.starts_with("int"))
        return "sint";
    return member.is_unfilterable || t.shape == check::texture_shape::d2_ms ? "unfilterable_float" : "filterable_float";
}

// An `sg::access_mode` name; SGL's own enum orders its members differently, so it is never cast across.
cc::string_view access_name(check::access_mode access)
{
    switch (access)
    {
    case check::access_mode::read:
        return "read";
    case check::access_mode::read_write:
        return "read_write";
    case check::access_mode::write:
        return "write";
    }
    return "read";
}

described_sampler describe_settings(check::sampler_state const& state)
{
    return {
        .min_filter = cc::string(check::k_sampler_filters[state.min_filter]),
        .mag_filter = cc::string(check::k_sampler_filters[state.mag_filter]),
        .mip_filter = cc::string(check::k_sampler_filters[state.mip_filter]),
        .address_u = cc::string(check::k_sampler_addresses[state.address_u]),
        .address_v = cc::string(check::k_sampler_addresses[state.address_v]),
        .address_w = cc::string(check::k_sampler_addresses[state.address_w]),
        .compare = state.compare >= 0 ? cc::string(check::k_compare_ops[state.compare]) : cc::string(),
        .max_anisotropy = state.max_anisotropy,
        .min_lod = state.min_lod,
        .max_lod = state.max_lod,
        .mip_lod_bias = state.mip_lod_bias,
    };
}

described_binding_member describe_resource(check::checked_module const& m,
                                           check::member_info const& member,
                                           i32 slot,
                                           cc::string host_name)
{
    auto const& t = m.at(member.type);
    auto result = described_binding_member{.name = member.name,
                                           .slot = slot,
                                           .host_name = cc::move(host_name),
                                           .access = cc::string("read")};
    switch (t.kind)
    {
    case check::type_kind::texture:
        result.kind = described_member_kind::texture;
        result.type = cc::string(m.name_of(member.type));
        result.texture_dimension = cc::string(check::info_of(t.shape).sg_name);
        result.sample_type = cc::string(sample_type_of(m, member));
        break;
    case check::type_kind::image:
        result.kind = described_member_kind::image;
        result.type = cc::string(m.name_of(member.type));
        result.texture_dimension = cc::string(check::info_of(t.shape).sg_name);
        result.image_format = cc::string(check::k_image_formats[t.format].name);
        result.access = cc::string(access_name(t.access));
        break;
    case check::type_kind::acceleration_structure:
        result.kind = described_member_kind::acceleration_structure;
        result.type = cc::string(m.name_of(member.type));
        break;
    default:
        result.kind = described_member_kind::sampler;
        result.type = cc::string(m.name_of(member.type));
        result.sampler_type = cc::string(sampler_type_of(m, member));
        if (member.static_sampler >= 0)
            result.static_sampler = describe_settings(m.samplers[member.static_sampler]);
        break;
    }
    return result;
}


described_struct describe_struct(check::checked_module const& m, check::type_info const& t)
{
    auto result = described_struct{.name = m.at(t.symbol).name,
                                   .edge = t.edge,
                                   .shape = check::hex_of(check::structural_hash(m, m.at(t.members)))};
    auto location = 0;
    auto const semantics = t.edge == check::stage::vertex ? emit_impl::vertex_semantics(m, t) : cc::vector<cc::string>();
    auto index = isize(0);
    for (auto const& member : m.at(t.members))
        result.members.push_back(
            {.name = member.name,
             .type = cc::string(m.name_of(member.type)),
             // the position, the depth and the sample mask take no location
             .location = member.is_position || member.output != check::pixel_output::color ? -1 : location++,
             .stream = t.edge == check::stage::vertex ? emit_impl::stream_of(member) : cc::string(),
             .format = member.vertex_format,
             .output = member.output == check::pixel_output::sample_mask ? cc::string("sample_mask")
                     : member.output != check::pixel_output::color       ? cc::string("depth")
                                                                         : cc::string(),
             .semantic = t.edge == check::stage::vertex ? semantics[index++] : cc::string(),
             .is_per_instance = member.is_per_instance});
    return result;
}

described_memory_struct describe_memory_struct(check::checked_module const& m,
                                               check::type_id type,
                                               emit_impl::address_space space)
{
    auto const placed = emit_impl::place_struct(m, type, space);
    auto result = described_memory_struct{
        .name = cc::string(m.name_of(type)),
        .space = cc::string(space == emit_impl::address_space::constants ? "constants" : "storage"),
        .size = placed.size};
    auto const members = m.at(m.at(type).members);
    for (auto i = isize(0); i < members.size(); ++i)
        if (members[i].type != check::checked_module::void_type)
            result.members.push_back({.name = members[i].name,
                                      .type = cc::string(m.name_of(members[i].type)),
                                      .offset = placed.offsets[i],
                                      .size = placed.sizes[i]});
    return result;
}

cc::vector<cc::string> feature_names(check::feature_set features)
{
    auto result = cc::vector<cc::string>();
    for (auto i = isize(0); i < check::k_feature_count; ++i)
        if (features.has(check::feature(i)))
            result.push_back(cc::string(check::k_feature_names[i]));
    return result;
}

/// `legal` is `e` legalized, which is the tree the footprint is read from.
described_entry_point describe_entry_point(check::checked_module const& m,
                                           check::flat_entry_point const& e,
                                           check::flat_entry_point const& legal)
{
    auto result = described_entry_point{.name = e.name, .stage = e.entry_stage};
    for (auto axis = 0; axis < 3; ++axis)
        result.workgroup[axis] = e.workgroup[axis];
    for (auto const id : e.bindings)
        if (!m.bindings[m.at(id).info].is_workgroup)
            result.bindings.push_back(m.at(id).name);
    result.features = feature_names(e.features);
    result.footprint = check::footprint_of(m, legal);
    for (auto const id : driver::impl::file_samplers_of(legal))
        result.samplers.push_back(m.at(id).name);
    return result;
}

/// `legal` holds each entry point of the module legalized, parallel to `m.entry_points`.
described_pipeline describe_pipeline(check::checked_module const& m,
                                     check::pipeline_info const& p,
                                     cc::span<check::flat_entry_point const> legal)
{
    auto result = described_pipeline{.name = m.at(p.symbol).name};
    auto features = check::feature_set();
    struct named_stage
    {
        check::symbol_id entry;
        cc::string* name;
    };
    // Parallel to `m.symbols`: whether a stage reaches it, a file-scope sampler being the only symbol that can.
    auto is_reached = cc::vector<u8>::create_filled(m.symbols.size(), 0);
    for (auto const s :
         {named_stage{p.vertex, &result.vertex}, named_stage{p.pixel, &result.pixel},
          named_stage{p.geometry, &result.geometry}, named_stage{p.tessellation_control, &result.tessellation_control},
          named_stage{p.tessellation_evaluation, &result.tessellation_evaluation}})
        if (check::is_valid(s.entry))
        {
            *s.name = m.at(s.entry).name;
            features |= m.functions[m.at(s.entry).info].features;
            for (auto i = isize(0); i < m.entry_points.size(); ++i)
                if (m.entry_points[i].function == s.entry)
                    for (auto const id : driver::impl::file_samplers_of(legal[i]))
                        is_reached[index_of(id)] = 1;
        }
    // One layout serves every stage, so it carries what any of them reaches, in index order.
    for (auto i = isize(0); i < m.symbols.size(); ++i)
        if (is_reached[i] != 0)
            result.samplers.push_back(m.symbols[i].name);
    for (auto const b : m.at(p.layout))
        result.layout.push_back(m.at(b).name);
    if (check::is_valid(p.inline_constants))
        result.inline_constants = m.at(p.inline_constants).name;
    // empty for a vertex stage that draws from no vertex buffer
    if (check::is_valid(p.vertex_input))
        result.vertex_input = m.name_of(p.vertex_input);
    if (check::is_valid(p.target_set))
    {
        result.target_set = m.name_of(p.target_set);
        for (auto const& member : m.at(m.at(p.target_set).members))
            if (member.output == check::pixel_output::color)
                result.targets.push_back(member.name);
    }
    result.features = feature_names(features);

    auto const settings = m.at(p.settings);
    for (auto const& s : settings)
        result.settings.push_back({.path = s.path,
                                   .kind = s.kind,
                                   .integer = s.integer,
                                   .real = s.real,
                                   .enum_case = s.enum_case,
                                   .enum_name = s.enum_name});

    // The frozen part, which a reload compares line by line: a declaration by its name and its shape.
    auto const shaped = [&](check::type_id type)
    { return cc::format("{}@{}", m.name_of(type), check::hex_of(check::structural_hash(m, type))); };
    auto const bound = [&](check::symbol_id b)
    {
        return cc::format("{}@{}", m.at(b).name,
                          check::hex_of(check::structural_hash(m, m.at(m.bindings[m.at(b).info].members))));
    };
    auto layout = cc::string();
    for (auto const b : m.at(p.layout))
        layout += cc::format("{}{}", layout.empty() ? "" : ", ", bound(b));
    result.frozen.push_back(cc::format("layout = {}", layout));
    result.frozen.push_back(
        cc::format("inline constants = {}", check::is_valid(p.inline_constants) ? bound(p.inline_constants) : ""));
    result.frozen.push_back(
        cc::format("vertex input = {}", check::is_valid(p.vertex_input) ? shaped(p.vertex_input) : cc::string()));
    result.frozen.push_back(
        cc::format("target set = {}", check::is_valid(p.target_set) ? shaped(p.target_set) : cc::string()));
    // The samplers are baked into the layout too, each at its index among all the file's samplers.
    auto baked = cc::string();
    auto sampler_index = 0;
    for (auto i = isize(0); i < m.symbols.size(); ++i)
    {
        if (is_reached[i] != 0)
            baked += cc::format("{}{}#{}@{}", baked.empty() ? "" : ", ", m.symbols[i].name, sampler_index,
                                check::hex_of(check::structural_hash(m.samplers[m.symbols[i].info])));
        sampler_index += m.symbols[i].kind == check::symbol_kind::sampler ? 1 : 0;
    }
    result.frozen.push_back(cc::format("samplers = {}", baked));
    // The host's code holds a shader per stage, so a reload that adds or drops one has nothing to build it with.
    auto stages = cc::string();
    for (auto const* name : {&result.vertex, &result.tessellation_control, &result.tessellation_evaluation,
                             &result.geometry, &result.pixel})
        if (!name->empty())
            stages += cc::format("{}{}", stages.empty() ? "" : ", ", *name);
    result.frozen.push_back(cc::format("stages = {}", stages));
    // A device lacking a feature a reload now needs would refuse the pipeline, so the build's needs are frozen too.
    auto needs = cc::string();
    for (auto const& name : result.features)
        needs += cc::format("{}{}", needs.empty() ? "" : ", ", name);
    result.frozen.push_back(cc::format("features = {}", needs));
    for (auto i = isize(0); i < settings.size(); ++i)
    {
        auto const& s = settings[i];
        auto const is_frozen
            = s.path.ends_with(".format") || s.path == "depth_stencil_format" || s.path == "sample_count";
        auto is_last = true;
        for (auto j = i + 1; j < settings.size(); ++j)
            is_last = is_last && settings[j].path != s.path;
        if (!is_frozen || !is_last)
            continue;
        auto const value = s.kind == check::setting_kind::host      ? cc::string(".host")
                         : s.kind == check::setting_kind::enum_case ? cc::format(".{}", s.enum_case)
                                                                    : cc::format("{}", s.integer);
        result.frozen.push_back(cc::format("{} = {}", s.path, value));
    }

    // Open is where the last word is `.host`: a later setting of that field takes it back.
    for (auto i = isize(0); i < settings.size(); ++i)
    {
        auto is_last = true;
        for (auto j = i + 1; j < settings.size(); ++j)
            is_last = is_last && settings[j].path != settings[i].path;
        if (is_last && settings[i].kind == check::setting_kind::host)
            result.open.push_back(settings[i].path);
    }
    return result;
}
} // namespace

sgl::described_binding sgl::driver::impl::describe_binding(check::checked_module const& m, check::symbol const& s)
{
    auto const& b = m.bindings[s.info];
    auto const members = m.at(b.members);
    auto result = described_binding{.name = s.name,
                                    .is_inline = b.is_inline,
                                    .shape = check::hex_of(check::structural_hash(m, members))};

    if (b.is_inline)
    {
        auto const placed = sgl::emit::impl::place_block(m, members);
        for (auto i = isize(0); i < members.size(); ++i)
            result.members.push_back({.name = members[i].name,
                                      .kind = described_member_kind::constant,
                                      .type = cc::string(m.name_of(members[i].type)),
                                      .offset = placed.offsets[i],
                                      .size = placed.sizes[i]});
        result.block_size = placed.size;
        return result;
    }

    // Numbered as the emitter numbers them: the constant block first when there is one, then the resources in
    // declaration order, each the next slot of its group.
    auto const plain = sgl::emit::impl::plain_members_of(m, b);
    auto const placed = sgl::emit::impl::place_block(m, plain);
    if (!plain.empty())
    {
        result.block_size = placed.size;
        result.block_slot = 0;
        result.block_host_name = s.name;
    }
    auto slot = sgl::emit::impl::first_resource_slot(m, b);
    auto next_constant = isize(0);
    for (auto const& whole : members)
    {
        // a binding array is described as its element, taking one slot per element
        auto member = whole;
        auto count = 1;
        if (auto const& t = m.at(whole.type);
            t.kind == check::type_kind::array && check::is_resource(m.at(t.element).kind))
        {
            member.type = t.element;
            count = t.count;
        }
        auto const& t = m.at(member.type);
        if (t.kind == check::type_kind::buffer)
        {
            result.members.push_back({.name = member.name,
                                      .kind = described_member_kind::buffer,
                                      .type = cc::string(m.name_of(t.element)),
                                      .slot = slot,
                                      .count = count,
                                      .stride = sgl::emit::impl::element_stride(m, t.element),
                                      .host_name = cc::format("{}.{}", s.name, member.name),
                                      .access = cc::string(t.is_mut ? "read_write" : "read")});
            slot += count;
            continue;
        }
        if (check::is_resource(t.kind))
        {
            auto described = describe_resource(m, member, slot, cc::format("{}.{}", s.name, member.name));
            described.count = count;
            result.members.push_back(cc::move(described));
            slot += count;
            continue;
        }
        result.members.push_back({.name = member.name,
                                  .kind = described_member_kind::constant,
                                  .type = cc::string(m.name_of(member.type)),
                                  .offset = placed.offsets[next_constant],
                                  .size = placed.sizes[next_constant]});
        ++next_constant;
    }
    return result;
}

sgl::described_file_sampler sgl::driver::impl::describe_file_sampler(check::checked_module const& m, check::symbol_id id)
{
    auto const& s = m.at(id);
    auto const& state = m.samplers[s.info];
    return {.name = s.name,
            .index = emit_impl::file_sampler_index(m, id),
            .sampler_type = cc::string(sampler_type_of(m, {.type = s.type, .static_sampler = s.info})),
            .settings = describe_settings(state),
            .shape = check::hex_of(check::structural_hash(state))};
}

cc::vector<sgl::check::symbol_id> sgl::driver::impl::file_samplers_of(check::flat_entry_point const& legal)
{
    auto result = cc::vector<check::symbol_id>();
    for (auto const& x : legal.exprs)
        if (auto const* const smp = x.node.try_as<check::flat_file_sampler>(); smp != nullptr)
        {
            auto at = isize(0);
            while (at < result.size() && index_of(result[at]) < index_of(smp->sampler))
                ++at;
            if (at == result.size() || result[at] != smp->sampler)
                result.insert_at(at, smp->sampler);
        }
    return result;
}

cc::result<sgl::module_description, cc::string> sgl::describe(describe_request const& request)
{
    auto const front = driver::impl::run_front_end(request.source, request.source_name);
    if (!front.errors.empty())
        return cc::error(front.errors);

    auto const& m = front.module;
    auto errors = cc::vector<emit::error>();
    auto result = module_description();
    auto described = cc::vector<check::symbol_id>();

    // Only the program's own declarations: the prelude describes nothing, and an imported module describes itself.
    for (auto i = isize(0); i < m.symbols.size(); ++i)
    {
        auto const id = check::symbol_id(i);
        auto const& s = m.at(id);
        if (s.file != front.program_file() || s.state != check::symbol_state::checked)
            continue;

        // workgroup memory has no host side, so the host is told nothing of it
        if (s.kind == check::symbol_kind::binding && m.bindings[s.info].is_workgroup)
            continue;
        if (s.kind == check::symbol_kind::binding)
        {
            auto const before = errors.size();
            emit_impl::validate_binding(m, id, errors);
            if (errors.size() == before)
            {
                result.bindings.push_back(driver::impl::describe_binding(m, s));
                described.push_back(id);
            }
        }
        else if (s.kind == check::symbol_kind::sampler)
            result.samplers.push_back(driver::impl::describe_file_sampler(m, id));
        else if (s.kind == check::symbol_kind::structure && check::is_valid(s.type))
        {
            auto const& t = m.at(s.type);
            if (t.edge == check::stage::none)
                continue;
            auto const role = t.edge == check::stage::vertex ? emit_impl::struct_role::vertex_input
                                                             : emit_impl::struct_role::render_targets;
            auto const before = errors.size();
            emit_impl::validate_edge_struct(m, s.type, role, errors);
            if (errors.size() == before)
                result.structs.push_back(describe_struct(m, t));
        }
    }

    // The structs the described bindings place in memory, each once and after what it holds.
    for (auto const id : described)
    {
        for (auto const space : {emit_impl::address_space::constants, emit_impl::address_space::storage})
        {
            auto structs = cc::vector<check::type_id>();
            emit_impl::collect_placed_structs(m, id, space, structs);
            for (auto const type : structs)
            {
                auto const name = m.name_of(type);
                auto is_known = false;
                for (auto const& known : result.memory_structs)
                    is_known = is_known || known.name == name;
                if (!is_known)
                    result.memory_structs.push_back(describe_memory_struct(m, type, space));
            }
        }
    }

    // What only an entry point can get wrong: its list, its signature and its body.
    auto legal = cc::vector<check::flat_entry_point>();
    for (auto const& e : m.entry_points)
    {
        auto const before = errors.size();
        legal.push_back(check::legalize(m, e));
        emit_impl::validate(m, legal.back(), errors);
        if (errors.size() == before)
            result.entry_points.push_back(describe_entry_point(m, e, legal.back()));
    }

    for (auto const& p : m.pipelines)
        if (m.at(p.symbol).file == front.program_file())
            result.pipelines.push_back(describe_pipeline(m, p, legal));

    if (!errors.empty())
    {
        // A binding is judged on its own and again under every entry point that lists it, so its errors are said once.
        auto text = cc::string();
        for (auto i = isize(0); i < errors.size(); ++i)
        {
            auto const& error = errors[i];
            auto is_repeat = false;
            for (auto j = isize(0); j < i; ++j)
                is_repeat = is_repeat || errors[j] == error;
            if (!is_repeat)
                text.appendf("{}: error: {}: {}\n", request.source_name, emit::to_string(error.kind), error.detail);
        }
        return cc::error(cc::move(text));
    }
    return result;
}
