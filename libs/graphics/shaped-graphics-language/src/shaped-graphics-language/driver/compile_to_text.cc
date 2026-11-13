#include "compile_to_text.hh"

#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/reserved_bindings.hh>
#include <shaped-graphics-language/check/resources.hh>
#include <shaped-graphics-language/driver/impl/describe_binding.hh>
#include <shaped-graphics-language/driver/impl/front_end.hh>
#include <shaped-graphics-language/emit/impl/dialect.hh>
#include <shaped-graphics-language/legalize/impl/polyfill.hh>
#include <shaped-graphics-language/legalize/legalize.hh>
#include <shaped-graphics-language/test/run_tests.hh>

using namespace sgl;

namespace
{
/// Whether the code of `legal` reaches slot `member` of `binding`: a resource by its position, a block by -1.
/// Every plain member reaches its binding's block, and every member of an `@inline` binding reaches that one block.
bool is_reached(check::checked_module const& m, check::flat_entry_point const& legal, check::symbol_id binding, i32 member)
{
    auto const& info = m.bindings[m.at(binding).info];
    auto const members = m.at(info.members);
    for (auto const& x : legal.exprs)
    {
        auto const* const b = x.node.try_as<check::flat_binding_member>();
        if (b == nullptr || b->binding != binding)
            continue;
        auto const is_resource = !info.is_inline && m.takes_slots(members[b->member].type);
        if ((is_resource ? b->member : -1) == member)
            return true;
    }
    return false;
}

/// Every slot the entry point's text declares, as `sgl describe` numbers and names them.
cc::vector<interface_binding> interface_of(check::checked_module const& m,
                                           check::flat_entry_point const& legal,
                                           cc::span<emit::bound_name const> bound_names)
{
    auto const emitted_of = [&](cc::string_view host) -> cc::string
    {
        for (auto const& b : bound_names)
            if (b.host == host)
                return b.emitted;
        return cc::string(host);
    };

    auto result = cc::vector<interface_binding>();
    auto group = 0;
    for (auto const id : legal.bindings)
    {
        auto const& s = m.at(id);
        // workgroup memory takes no group, and no host binds it
        if (m.bindings[s.info].is_workgroup)
            continue;
        auto const is_inline = m.bindings[s.info].is_inline;
        auto const described = driver::impl::describe_binding(m, s);
        if (is_inline || described.block_slot == 0)
            result.push_back({.name = s.name,
                              .emitted = emitted_of(m.qualified_name_of(id)),
                              .kind = described_member_kind::constant,
                              .is_inline = is_inline,
                              .group = is_inline ? -1 : group,
                              .slot = 0,
                              .is_used = is_reached(m, legal, id, -1),
                              .block_size = described.block_size});
        if (is_inline)
            continue;
        for (auto i = isize(0); i < described.members.size(); ++i)
        {
            auto const& member = described.members[i];
            if (member.kind == described_member_kind::constant)
                continue;
            result.push_back({.name = member.host_name,
                              .emitted = emitted_of(cc::format("{}.{}", m.qualified_name_of(id), member.name)),
                              .kind = member.kind,
                              .group = group,
                              .slot = member.slot,
                              .count = member.count,
                              .is_used = is_reached(m, legal, id, i32(i)),
                              .access = member.access,
                              .texture_dimension = member.texture_dimension,
                              .sample_type = member.sample_type,
                              .image_format = member.image_format,
                              .sampler_type = member.sampler_type});
        }
        ++group;
    }
    for (auto const id : driver::impl::file_samplers_of(legal))
    {
        auto const described = driver::impl::describe_file_sampler(m, id);
        result.push_back({.name = described.name,
                          .emitted = emitted_of(described.name),
                          .kind = described_member_kind::sampler,
                          .is_file_sampler = true,
                          .group = -1,
                          .slot = described.index,
                          .is_used = true,
                          .sampler_type = described.sampler_type});
    }
    return result;
}

/// `level`'s name as `@float` spells it.
cc::string_view float_level_name(check::float_level level)
{
    switch (level)
    {
    case check::float_level::fast:
        return "fast";
    case check::float_level::specials:
        return "specials";
    case check::float_level::precise:
        return "precise";
    case check::float_level::ieee:
        return "ieee";
    }
    return "fast";
}

/// EMIT-185: the strictest float guarantee `target`'s compile keeps today.
check::float_level kept_float_level(emit::target target)
{
    switch (target)
    {
    case emit::target::hlsl_dx12:
        return check::float_level::ieee;
    // safe math keeps the specials and the order; whether it keeps denormals is not known
    case emit::target::msl:
        return check::float_level::precise;
    // DXC's -Gis writes SignedZeroInfNanPreserve and NoContraction on every float operation, and -denorm preserve
    // writes DenormPreserve, each behind the device property it needs
    case emit::target::hlsl_vulkan:
        return check::float_level::ieee;
    // nothing a shader says reaches Tint's or Naga's float handling
    case emit::target::wgsl:
        return check::float_level::fast;
    }
    return check::float_level::fast;
}

/// The back half of `compile_to_text`: one entry point of a checked module, written for one target.
cc::result<emitted_source, cc::string> emit_text(driver::impl::front_end const& front,
                                                 check::flat_entry_point const& e,
                                                 emit::target target,
                                                 bool line_directives,
                                                 bool diagnostics,
                                                 check::undefined_guard guards_undefined,
                                                 bool asks_software_trace,
                                                 bool weakens_float,
                                                 check::feature_set device_features,
                                                 cc::span<check::feature_fill const> fills,
                                                 check::core_fills core,
                                                 emit::native_route native = emit::native_route::text,
                                                 bool native_line_table = false,
                                                 emit::mesh_form mesh = emit::mesh_form::native,
                                                 check::flat_entry_point const* polyfill_vertex = nullptr,
                                                 check::flat_entry_point const* polyfill_task = nullptr,
                                                 check::flat_entry_point const* polyfill_control = nullptr)
{
    // CHK-524: a polyfilled mesh stage that writes values per primitive hands them on in one struct beside its vertex's,
    // which its vertex half returns and its pixel stage takes, and which a copy of the module holds
    auto corner_module = cc::optional<check::checked_module>();
    auto corner = check::type_id::none;
    if (mesh == emit::mesh_form::vertex_fetch
        && (e.entry_stage == check::stage::mesh || e.entry_stage == check::stage::pixel))
        if (auto const parts = check::mesh_corner_parts_of(front.module, e); check::is_valid(parts.primitive))
        {
            corner_module = front.module;
            corner = check::add_mesh_corner(corner_module.value(), parts);
        }
    // CHK-525: a geometry stage's mesh route writes to a `mesh_output` of its stream's vertex, which a copy of the
    // module holds, with the bounds of a workgroup's slots on the geometry function
    auto mesh_route_layout = cc::optional<check::mesh_polyfill_layout>();
    auto mesh_parts = check::geometry_mesh_parts();
    if (mesh == emit::mesh_form::mesh && e.entry_stage == check::stage::geometry)
    {
        auto const layout = check::geometry_polyfill_layout_of(front.module, e);
        if (layout.has_error())
            return cc::error(cc::format("{}: error: unsupported: {}\n", front.source_name, layout.error()));
        mesh_route_layout = layout.value();
        corner_module = front.module;
        mesh_parts = check::add_geometry_mesh_output(corner_module.value(), e, layout.value());
    }
    auto const& m = corner_module.has_value() ? corner_module.value() : front.module;
    auto const source_name = front.source_name;
    // CHK-525: a polyfilled geometry stage's compute half is its pipeline's vertex stage and itself as one compute
    // entry point; each is legalized to be grafted, and what they make is legalized below as any entry point is
    auto polyfill_layout = cc::optional<check::mesh_polyfill_layout>();
    auto fused = cc::optional<check::flat_entry_point>();
    if (mesh_route_layout.has_value())
    {
        if (polyfill_vertex == nullptr)
            return cc::error(cc::format("{}: error: the mesh route of geometry entry point '{}' runs its pipeline's "
                                        "vertex stage, and none was named\n",
                                        source_name, e.name));
        auto const graft = check::legalize_options{.is_emulated = target == emit::target::wgsl || asks_software_trace,
                                                   .lowers_asserts = diagnostics};
        auto made = check::geometry_as_mesh(m, check::legalize(m, *polyfill_vertex, graft),
                                            check::legalize(m, e, graft), mesh_route_layout.value(), mesh_parts);
        if (made.has_error())
            return cc::error(cc::format("{}: error: unsupported: {}\n", source_name, made.error()));
        fused = cc::move(made.value());
        // what comes back is a mesh entry point, which every target with mesh shaders writes as it is
        mesh = emit::mesh_form::native;
    }
    else if (mesh != emit::mesh_form::native && e.entry_stage == check::stage::geometry)
    {
        auto const layout = check::geometry_polyfill_layout_of(m, e);
        if (layout.has_error())
            return cc::error(cc::format("{}: error: unsupported: {}\n", source_name, layout.error()));
        polyfill_layout = layout.value();
        if (mesh == emit::mesh_form::compute)
        {
            if (polyfill_vertex == nullptr)
                return cc::error(cc::format("{}: error: the compute half of geometry entry point '{}' runs its "
                                            "pipeline's "
                                            "vertex stage, and none was named\n",
                                            source_name, e.name));
            auto const graft = check::legalize_options{.is_emulated = target == emit::target::wgsl || asks_software_trace,
                                                       .lowers_asserts = diagnostics};
            auto made = check::geometry_as_compute(m, check::legalize(m, *polyfill_vertex, graft),
                                                   check::legalize(m, e, graft));
            if (made.has_error())
                return cc::error(cc::format("{}: error: unsupported: {}\n", source_name, made.error()));
            fused = cc::move(made.value());
        }
    }
    // CHK-527: a polyfilled tessellation's control stage runs with its pipeline's vertex stage, and its evaluation stage
    // reads what the control stage wrote; both are grafted as the geometry stage is
    auto tessellation_layout = cc::optional<check::tessellation_polyfill_layout>();
    auto const is_tessellation
        = e.entry_stage == check::stage::tessellation_control || e.entry_stage == check::stage::tessellation_evaluation;
    if (mesh != emit::mesh_form::native && is_tessellation)
    {
        auto const* const control = e.entry_stage == check::stage::tessellation_control ? &e : polyfill_control;
        // the evaluation stage's vertex half reads its points back alone, and needs no control stage
        if (control == nullptr && mesh == emit::mesh_form::vertex_fetch)
        {
            auto const leaves = check::impl::leaves_of(m, e.result);
            if (leaves.has_error())
                return cc::error(cc::format("{}: error: unsupported: {}'s result holds a '{}', which no word holds\n",
                                            source_name, e.name, leaves.error()));
            tessellation_layout = check::tessellation_polyfill_layout{.vertex_words = i32(leaves.value().size())};
        }
        else if (control == nullptr)
            return cc::error(cc::format("{}: error: the polyfilled tessellation evaluation entry point '{}' reads what "
                                        "its pipeline's control stage wrote, and none was named\n",
                                        source_name, e.name));
        else
        {
            auto const* const evaluation = e.entry_stage == check::stage::tessellation_evaluation ? &e : nullptr;
            auto const layout = check::tessellation_polyfill_layout_of(m, *control, evaluation);
            if (layout.has_error())
                return cc::error(cc::format("{}: error: unsupported: {}\n", source_name, layout.error()));
            tessellation_layout = layout.value();
        }
        auto const graft = check::legalize_options{.is_emulated = target == emit::target::wgsl || asks_software_trace,
                                                   .lowers_asserts = diagnostics};
        if (mesh == emit::mesh_form::compute && e.entry_stage == check::stage::tessellation_control)
        {
            if (polyfill_vertex == nullptr)
                return cc::error(cc::format("{}: error: the compute half of tessellation control entry point '{}' "
                                            "runs its pipeline's vertex stage, and none was named\n",
                                            source_name, e.name));
            // CHK-304: the factors the evaluation stage reads back are capped, as a native tessellator's are
            auto made = check::tessellation_as_compute(m, check::legalize(m, *polyfill_vertex, graft),
                                                       check::with_capped_factors(m, check::legalize(m, e, graft)));
            if (made.has_error())
                return cc::error(cc::format("{}: error: unsupported: {}\n", source_name, made.error()));
            fused = cc::move(made.value());
        }
        else if (mesh == emit::mesh_form::compute)
        {
            CC_ASSERT(control != nullptr, "an evaluation stage's compute half was refused above without its control "
                                          "stage");
            auto made = check::tessellation_evaluation_as_compute(m, check::legalize(m, *control, graft),
                                                                  check::legalize(m, e, graft));
            if (made.has_error())
                return cc::error(cc::format("{}: error: unsupported: {}\n", source_name, made.error()));
            fused = cc::move(made.value());
        }
    }
    auto const& source_entry = fused.has_value() ? fused.value() : e;
    // EMIT-185: a guarantee the target cannot keep is refused, unless the host weakened it on WebGPU
    auto const raised_by
        = check::is_valid(source_entry.float_raised_by) ? m.at(source_entry.float_raised_by).name : cc::string();
    auto float_guarantee = source_entry.float_guarantee;
    if (u8(float_guarantee) > u8(kept_float_level(target)))
    {
        if (target != emit::target::wgsl || !weakens_float)
            return cc::error(cc::format("{}: error: float-guarantee-not-kept: entry point '{}' needs @float(.{}), "
                                        "which "
                                        "'{}' asks for, and {} keeps no more than .{}\n",
                                        source_name, source_entry.name, float_level_name(float_guarantee), raised_by,
                                        emit::to_string(target), float_level_name(kept_float_level(target))));
        float_guarantee = check::float_level::fast;
    }
    // EMIT-185: vulkan keeps a level through execution modes the device must have
    auto entry = source_entry;
    if (target == emit::target::hlsl_vulkan && float_guarantee != check::float_level::fast)
        entry.features.set(check::feature::float_specials);
    if (target == emit::target::hlsl_vulkan && float_guarantee == check::float_level::ieee)
        entry.features.set(check::feature::float_denormals);
    // The check pass writes the structured form, and a target prints the core form.
    // WebGPU traces through the emulated form of a trace, and every other target through its native query, unless the
    // compile asks for software on HLSL (EMIT-135).
    auto const software_trace = asks_software_trace;
    // LEGAL-63, LEGAL-64: without diagnostics the text holds no print and no assert, and with them both write a record
    auto const files = driver::impl::module_files_of(front);
    auto names = cc::vector<cc::string_view>();
    for (auto i = isize(0); i < files.size(); ++i)
        names.push_back(front.name_of(i));
    // EMIT-183: a stage that cannot write the window ends a failed assert all the same, and drops what it would log
    auto const writes_records = diagnostics
                             && check::reaches_reserved(*check::reserved_binding_named("sg_debug"), target,
                                                        entry.entry_stage, device_features);
    auto const legal = check::legalize(m, entry,
                                       {.is_emulated = target == emit::target::wgsl || software_trace,
                                        .erases_prints = !writes_records,
                                        .lowers_asserts = diagnostics,
                                        .expands_prints = writes_records,
                                        .expands_workgroup = true,
                                        .workgroup_starts_zeroed = target == emit::target::wgsl,
                                        .files = files,
                                        .names = names,
                                        .guards_undefined = guards_undefined});
    // CHK-524: a polyfilled mesh entry point is written, reflected and described as the form it becomes
    auto mesh_layout = polyfill_layout;
    auto form = legal;
    // CHK-304: a native tessellator takes factors up to 64, so a control stage's `max_factor` caps what it returns
    if (legal.entry_stage == check::stage::tessellation_control)
        form = check::with_capped_factors(m, legal);
    if (mesh == emit::mesh_form::vertex_fetch && legal.entry_stage == check::stage::geometry)
        form = check::geometry_as_vertex_fetch(m, legal);
    if (mesh == emit::mesh_form::vertex_fetch && legal.entry_stage == check::stage::tessellation_evaluation)
        form = check::tessellation_as_vertex_fetch(m, legal);
    // CHK-524: the records a polyfilled task stage writes, which its compute form and a launched mesh stage's agree on
    auto task_layout = cc::optional<check::task_polyfill_layout>();
    if (mesh != emit::mesh_form::native && (legal.entry_stage == check::stage::task || polyfill_task != nullptr))
    {
        auto const layout = check::task_polyfill_layout_of(m, polyfill_task != nullptr ? *polyfill_task : e);
        if (layout.has_error())
            return cc::error(cc::format("{}: error: unsupported: {}\n", source_name, layout.error()));
        task_layout = layout.value();
    }
    if (mesh == emit::mesh_form::compute && legal.entry_stage == check::stage::task)
        form = check::task_as_compute(m, legal);
    if (mesh != emit::mesh_form::native && legal.entry_stage == check::stage::mesh)
    {
        // the vertex half reads no payload, so only a task stage's records carry one; with none named, the scan and
        // the launched form number the one record of an indirect draw
        auto const layout
            = check::mesh_polyfill_layout_of(m, legal, mesh == emit::mesh_form::vertex_fetch || task_layout.has_value());
        if (layout.has_error())
            return cc::error(cc::format("{}: error: unsupported: {}\n", source_name, layout.error()));
        if ((mesh == emit::mesh_form::scan || mesh == emit::mesh_form::launched) && !task_layout.has_value())
            task_layout = check::task_polyfill_layout{};
        mesh_layout = layout.value();
        form = mesh == emit::mesh_form::compute || mesh == emit::mesh_form::launched
                 ? check::mesh_as_compute(m, legal, task_layout.has_value() ? &task_layout.value() : nullptr)
             : mesh == emit::mesh_form::scan ? check::mesh_task_scan(m, legal, task_layout.value())
                                             : check::mesh_as_vertex_fetch(m, legal, corner);
    }
    if (mesh == emit::mesh_form::vertex_fetch && legal.entry_stage == check::stage::pixel && check::is_valid(corner))
        form = check::pixel_after_mesh_polyfill(m, legal, corner);
    auto emitted = emit::emit_entry_point(m, form, target,
                                          {.files = files,
                                           .names = names,
                                           .write_directives = line_directives,
                                           .diagnostics = diagnostics,
                                           .device_features = device_features,
                                           .fills = fills,
                                           .core = core,
                                           .software_trace = software_trace,
                                           .native = native,
                                           .native_line_table = native_line_table});
    if (!emitted.has_text())
    {
        auto text = cc::string();
        for (auto const& error : emitted.errors)
            text.appendf("{}: error: {}: {}\n", source_name, emit::to_string(error.kind), error.detail);
        return cc::error(cc::move(text));
    }
    auto result = sgl::emitted_source{
        .text = cc::move(emitted.text),
        .spirv = cc::move(emitted.spirv),
        .dxil = cc::move(emitted.dxil),
        .native_gap = cc::move(emitted.native_gap),
        .signature = cc::move(emitted.signature),
        .stage = form.entry_stage,
        .entry_point = cc::move(emitted.entry_point),
        .bindings = interface_of(m, form, emitted.bound_names),
        .color_targets = emitted.color_targets,
        .target_struct = cc::move(emitted.target_struct),
        .features = mesh_layout.has_value() || task_layout.has_value() || tessellation_layout.has_value()
                      ? form.features
                      : entry.features,
        .options = driver::impl::option_names_of(m, e),
        .footprint = check::footprint_of(m, form),
        .layouts = cc::move(emitted.layouts),
        .lines = cc::move(emitted.lines),
        .debug_sites = cc::move(emitted.debug_sites),
        .mesh_layout = mesh_layout,
        .task_layout = task_layout,
        .tessellation_layout = tessellation_layout};
    for (auto const name : names)
        result.line_files.push_back(cc::string(name));
    // A module's `@pixel struct` is named as the program names it, `m.target`, which tells the host it is that module's.
    if (!result.target_struct.empty() && check::is_valid(e.result) && check::is_valid(m.at(e.result).symbol))
        if (auto const module = m.foreign_module_of(m.at(e.result).symbol); !module.empty())
            result.target_struct = cc::format("{}.{}", module, result.target_struct);
    for (auto axis = 0; axis < 3; ++axis)
        result.workgroup[axis] = form.workgroup[axis];
    result.is_dispatched_by_threads = form.is_auto_workgroup;
    result.preferred_subgroup_size = e.preferred_subgroup_size;
    // EMIT-262: the model is the most any feature needs in its fill, and either core construct written natively
    for (auto i = isize(0); i < check::k_feature_count; ++i)
        if (auto const f = check::feature(i); result.features.has(f))
            if (auto const needs = check::dxil_shader_model_of(f, check::fill_of(fills, f));
                result.dxil_shader_model < needs)
                result.dxil_shader_model = needs;
    if (emitted.reads_draw_start
        && result.dxil_shader_model < check::dxil_shader_model_of(check::core_construct::draw_start, check::fill::native))
        result.dxil_shader_model = check::dxil_shader_model_of(check::core_construct::draw_start, check::fill::native);
    if (emitted.states_wave_size_range
        && result.dxil_shader_model < check::dxil_shader_model_of(check::core_construct::wave_size, check::fill::native))
        result.dxil_shader_model = check::dxil_shader_model_of(check::core_construct::wave_size, check::fill::native);
    if (emitted.states_payload_qualifiers && result.dxil_shader_model < check::dxil_shader_model{.minor = 7})
        result.dxc_args.push_back(cc::string("-enable-payload-qualifiers"));
    // EMIT-263: an nvapi fill talks to NVIDIA's driver through a counter UAV at a slot the host enables, never binds
    if (emitted.writes_nvapi)
    {
        result.needs_nvapi_header = result.dxil.empty();
        result.bindings.push_back({.name = "nvapi_extension",
                                   .emitted = "g_NvidiaExt",
                                   .kind = described_member_kind::buffer,
                                   .is_reserved = true,
                                   .group = check::k_nvapi_space,
                                   .slot = check::k_nvapi_register,
                                   .is_used = true,
                                   .access = "read_write"});
    }
    // EMIT-264: what the entry consulted, each with what this target offers for it
    for (auto i = isize(0); i < check::k_feature_count; ++i)
        if (auto const f = check::feature(i); e.consulted.has(f))
        {
            auto axis = sgl::variant_axis{.f = f};
            for (auto const how : emit::fills_of(target, f))
                axis.fills.push_back(how);
            result.axes.push_back(cc::move(axis));
        }
    result.unshadowed_bytes = legal.unshadowed_bytes;
    result.float_guarantee = float_guarantee;
    if (float_guarantee != check::float_level::fast)
        result.float_raised_by = raised_by;
    return result;
}

/// What `front` says of the text of entry point `e`: the library files the source reached, and the warnings of the
/// program and of `e` (CHK-402).
void add_source_facts(cc::result<emitted_source, cc::string>& text,
                      driver::impl::front_end const& front,
                      check::flat_entry_point const& e)
{
    if (!text.has_value())
        return;
    for (auto const name : front.reached_library_names)
        text.value().library_files.push_back(cc::string(name));
    text.value().warnings = front.warnings_of(e.function);
}
} // namespace

cc::result<sgl::emitted_source, cc::string> sgl::compile_to_text(text_request const& request)
{
    // CHK-426: an inlined body reads the target and the device as this compile's
    auto const front = driver::impl::run_front_end(request.source, request.source_name, request.library, request.options,
                                                   emit::compile_target_of(request.target, request.device_features));
    if (!front.errors.empty())
        return cc::error(front.errors);
    auto const& m = front.module;

    if (request.run_tests)
    {
        // CHK-354: a test runs with every option at its default, so the compile's values need a check of their own
        auto const defaults = request.options.empty()
                                ? cc::optional<driver::impl::front_end>()
                                : cc::optional<driver::impl::front_end>(driver::impl::run_front_end(
                                      request.source, request.source_name, request.library));
        auto const& tested = defaults.has_value() ? defaults.value() : front;
        auto failed = cc::string();
        for (auto const& r :
             test::run_tests(tested.module, driver::impl::module_files_of(tested), {.file = tested.program_file()}))
            if (!r.is_passed())
                failed += driver::impl::format_located(tested, test::diagnostic_of(tested.module, r));
        if (!failed.empty())
            return cc::error(cc::move(failed));
    }

    auto const* found = static_cast<check::flat_entry_point const*>(nullptr);
    for (auto const& candidate : m.entry_points)
        if (candidate.name == request.entry_point)
            found = &candidate;
    // CHK-527: a polyfill's own pass, which the prelude declares and no program lists
    if (found == nullptr)
        for (auto i = isize(0); i < m.internal_entry_points.size(); ++i)
            if (m.internal_entry_points[i].name == request.entry_point)
                found = &m.internal_entry_points[i];

    if (found == nullptr)
    {
        // CHK-402: an entry point whose own body did not check has no flat tree, and its errors say why
        for (auto i = isize(0); i < m.symbols.size(); ++i)
            if (auto const& s = m.symbols[i]; s.kind == check::symbol_kind::function && s.name == request.entry_point
                                              && s.file == front.program_file())
                if (auto own = front.errors_of(check::symbol_id(i)); !own.empty())
                    return cc::error(cc::move(own));
        auto held = cc::string();
        for (auto const& e : m.entry_points)
            held.appendf("{}{} '{}'", held.empty() ? "" : ", ", check::stage_name(e.entry_stage), e.name);
        return cc::error(cc::format("{}: error: no entry point named '{}' (the source holds: {})\n", request.source_name,
                                    request.entry_point, held.empty() ? cc::string_view("none") : cc::string_view(held)));
    }

    auto const& e = *found;
    if (request.stage != check::stage::none && e.entry_stage != request.stage)
        return cc::error(cc::format("{}: error: entry point '{}' is a {} entry point, and a {} one was asked for\n",
                                    request.source_name, e.name, check::stage_name(e.entry_stage),
                                    check::stage_name(request.stage)));
    // CHK-402: what was judged of another entry point's body is no error of this one
    if (auto own = front.errors_of(e.function); !own.empty())
        return cc::error(cc::move(own));
    // CHK-525: the vertex stage a polyfilled geometry stage's compute half runs
    auto const* polyfill_vertex = static_cast<check::flat_entry_point const*>(nullptr);
    if (!request.polyfill_vertex.empty())
    {
        for (auto const& v : m.entry_points)
            if (v.name == request.polyfill_vertex && v.entry_stage == check::stage::vertex)
                polyfill_vertex = &v;
        if (polyfill_vertex == nullptr)
            return cc::error(cc::format("{}: error: no vertex entry point named '{}'\n", request.source_name,
                                        request.polyfill_vertex));
    }
    // CHK-524: the task stage that launches a polyfilled mesh stage, whose records its compute half and scan read
    auto const* polyfill_task = static_cast<check::flat_entry_point const*>(nullptr);
    if (!request.polyfill_task.empty())
    {
        for (auto const& t : m.entry_points)
            if (t.name == request.polyfill_task && t.entry_stage == check::stage::task)
                polyfill_task = &t;
        if (polyfill_task == nullptr)
            return cc::error(
                cc::format("{}: error: no task entry point named '{}'\n", request.source_name, request.polyfill_task));
    }
    // CHK-527: the control stage a polyfilled tessellation evaluation stage reads the patches of
    auto const* polyfill_control = static_cast<check::flat_entry_point const*>(nullptr);
    if (!request.polyfill_control.empty())
    {
        for (auto const& c : m.entry_points)
            if (c.name == request.polyfill_control && c.entry_stage == check::stage::tessellation_control)
                polyfill_control = &c;
        if (polyfill_control == nullptr)
            return cc::error(cc::format("{}: error: no tessellation control entry point named '{}'\n",
                                        request.source_name, request.polyfill_control));
    }
    auto result = emit_text(front, e, request.target, request.line_directives, request.diagnostics,
                            request.guards_undefined, request.software_trace, request.weakens_float,
                            request.device_features, request.fills, request.core, request.native,
                            request.native_line_table, request.mesh, polyfill_vertex, polyfill_task, polyfill_control);
    add_source_facts(result, front, e);
    if (result.has_value() && result.value().float_guarantee != e.float_guarantee)
        result.value().warnings.push_back(cc::format(
            "{}: warning: float-guarantee-weakened: entry point '{}' needs @float(.{}), which '{}' asks for, and runs "
            "at .fast on {}, as the host allowed",
            request.source_name, e.name, float_level_name(e.float_guarantee),
            check::is_valid(e.float_raised_by) ? m.at(e.float_raised_by).name : cc::string(),
            emit::to_string(request.target)));
    if (result.has_value() && result.value().unshadowed_bytes > 0)
        result.value().warnings.push_back(cc::format(
            "{}: warning: workgroup-unshadowed: entry point '{}' holds {} bytes of workgroup memory, past what its "
            "shadow fits beside in 16384, so no read of it is tested for a store before it (LEGAL-69)",
            request.source_name, e.name, result.value().unshadowed_bytes));
    return result;
}

cc::result<cc::vector<sgl::entry_text>, cc::string> sgl::compile_all_to_text(all_text_request const& request)
{
    // CHK-426: each target is a check of its own, since an inlined body reads the target it is written for
    auto fronts = cc::vector<driver::impl::front_end>();
    for (auto const t : request.targets)
    {
        fronts.push_back(driver::impl::run_front_end(request.source, request.source_name, request.library, {},
                                                     emit::compile_target_of(t, request.device_features)));
        if (!fronts.back().errors.empty())
            return cc::error(fronts.back().errors);
    }
    auto result = cc::vector<entry_text>();
    if (fronts.empty())
        return result;
    for (auto i = isize(0); i < fronts.front().module.entry_points.size(); ++i)
        for (auto k = isize(0); k < request.targets.size(); ++k)
        {
            auto const& front = fronts[k];
            auto const& e = front.module.entry_points[i];
            // CHK-402: an entry point's own errors are that element's, and leave the others built
            auto own = front.errors_of(e.function);
            auto text = own.empty() ? emit_text(front, e, request.targets[k], false, false, check::undefined_guard::none,
                                                false, false, request.device_features, request.fills, request.core)
                                    : cc::result<emitted_source, cc::string>(cc::error(own));
            add_source_facts(text, front, e);
            result.push_back({.entry_point = e.name, .target = request.targets[k], .text = cc::move(text)});
        }
    return result;
}

cc::string sgl::variant_key::to_string() const
{
    auto text = cc::string();
    for (auto i = isize(0); i < check::k_feature_count; ++i)
        for (auto const& entry : fills)
            if (entry.f == check::feature(i))
                text.appendf("{}{}={}", text.empty() ? "" : ";", check::name_of(entry.f), check::name_of(entry.how));
    if (core.draw_start != check::fill::native)
        text.appendf("{}draw_start={}", text.empty() ? "" : ";", check::name_of(core.draw_start));
    if (core.wave_size != check::fill::native)
        text.appendf("{}wave_size={}", text.empty() ? "" : ";", check::name_of(core.wave_size));
    return text;
}

sgl::check::feature_set sgl::variant_key::granted() const
{
    auto result = check::feature_set();
    for (auto const& entry : fills)
        result.set(entry.f);
    return result;
}
