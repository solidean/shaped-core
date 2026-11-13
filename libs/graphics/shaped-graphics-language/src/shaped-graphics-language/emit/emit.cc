#include "emit.hh"

#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/check.hh>
#include <shaped-graphics-language/check/source_lines.hh>
#include <shaped-graphics-language/emit/impl/dialect.hh>
#include <shaped-graphics-language/emit/impl/plan.hh>
#include <shaped-graphics-language/ir/ir.hh>
#include <shaped-graphics-language/ir/pipeline.hh>
#include <shaped-graphics-language/legalize/legalize.hh>

namespace
{
constexpr sgl::emit::target k_all_targets[] = {
    sgl::emit::target::hlsl_dx12,
    sgl::emit::target::hlsl_vulkan,
    sgl::emit::target::wgsl,
    sgl::emit::target::msl,
};
} // namespace

cc::span<sgl::emit::target const> sgl::emit::all_targets()
{
    return k_all_targets;
}

cc::string_view sgl::emit::to_string(target t)
{
    switch (t)
    {
    case target::hlsl_dx12:
        return "hlsl-dx12";
    case target::hlsl_vulkan:
        return "hlsl-vulkan";
    case target::wgsl:
        return "wgsl";
    case target::msl:
        return "msl";
    }
    return "";
}

sgl::check::compile_target sgl::emit::compile_target_of(target t, check::feature_set device_features)
{
    auto result = check::compile_target{.device_features = device_features};
    switch (t)
    {
    case target::hlsl_dx12:
        result.api = check::graphics_api::dx12;
        result.language = check::shading_language::hlsl;
        break;
    case target::hlsl_vulkan:
        result.api = check::graphics_api::vulkan;
        result.language = check::shading_language::hlsl;
        break;
    case target::wgsl:
        result.api = check::graphics_api::webgpu;
        result.language = check::shading_language::wgsl;
        break;
    case target::msl:
        result.api = check::graphics_api::metal;
        result.language = check::shading_language::msl;
        break;
    }
    return result;
}

cc::string_view sgl::emit::to_string(error_kind kind)
{
    switch (kind)
    {
    case error_kind::module_has_errors:
        return "module-has-errors";
    case error_kind::unknown_entry_point:
        return "unknown-entry-point";
    case error_kind::reserved_entry_point_name:
        return "reserved-entry-point-name";
    case error_kind::unsupported:
        return "unsupported";
    case error_kind::system_value_semantic:
        return "system-value-semantic";
    case error_kind::layout_mismatch:
        return "layout-mismatch";
    case error_kind::non_finite_literal:
        return "non-finite-literal";
    case error_kind::malformed_tree:
        return "malformed-tree";
    case error_kind::not_core:
        return "not-core";
    case error_kind::too_many_groups:
        return "too-many-groups";
    case error_kind::target_lacks_feature:
        return "target-lacks-feature";
    case error_kind::layout_conflict:
        return "layout-conflict";
    case error_kind::padding_forbidden:
        return "padding-forbidden";
    case error_kind::too_many_samplers:
        return "too-many-samplers";
    case error_kind::too_many_acceleration_structures:
        return "too-many-acceleration-structures";
    case error_kind::ray_data_conflict:
        return "ray-data-conflict";
    }
    return "";
}

// The one place that knows every target; a new one is a case here and a file under impl/.
sgl::emit::impl::dialect const& sgl::emit::impl::dialect_of(target t)
{
    switch (t)
    {
    case target::hlsl_dx12:
        return hlsl_dx12_dialect();
    case target::hlsl_vulkan:
        return hlsl_vulkan_dialect();
    case target::wgsl:
        return wgsl_dialect();
    case target::msl:
        return msl_dialect();
    }
    return hlsl_dx12_dialect();
}

sgl::emit::emitted_text sgl::emit::emit(check::checked_module const& m,
                                        sgl::isize entry_point,
                                        target t,
                                        options const& sources)
{
    auto result = emitted_text();

    auto error_count = 0;
    for (auto const& d : m.diagnostics)
        if (d.what.level != severity::warning)
            ++error_count;
    if (error_count != 0)
    {
        result.errors.push_back({.kind = error_kind::module_has_errors, .detail = cc::format("{} errors", error_count)});
        return result;
    }
    if (entry_point < 0 || entry_point >= m.entry_points.size())
    {
        result.errors.push_back({.kind = error_kind::unknown_entry_point,
                                 .detail = cc::format("{} of {}", entry_point, m.entry_points.size())});
        return result;
    }

    // WebGPU traces through the emulated form of a trace, and every other target through its native query
    return emit_entry_point(
        m, check::legalize(m, m.entry_points[entry_point], {.is_emulated = t == target::wgsl, .erases_prints = true}),
        t, sources);
}

namespace sgl::emit
{
namespace
{
/// EMIT-257: each statement's line and its calls, as `source_lines` reads them, in the order the statements stand.
[[nodiscard]] ir::line_table line_table_of(check::checked_module const& m,
                                           check::flat_entry_point const& e,
                                           options const& sources)
{
    auto lines = check::source_lines{.m = m, .files = sources.files, .names = sources.names};
    auto table = ir::line_table();
    for (auto f = isize(0); f < sources.files.size(); ++f)
    {
        table.names.push_back(cc::string(lines.name_of(i32(f))));
        table.texts.push_back(cc::string(sources.files[f].file.source));
    }
    table.statements.reserve(e.stmts.size());
    for (auto const& s : e.stmts)
    {
        auto const origin = lines.origin_of(e, s);
        auto place = ir::source_place{.file = origin.at.file,
                                      .line = origin.at.line,
                                      .calls_first = u32(table.calls.size()),
                                      .calls_count = u32(origin.inlined_from.size())};
        for (auto const& call : origin.inlined_from)
            table.calls.push_back({.file = call.file, .line = call.line});
        table.statements.push_back(place);
    }
    return table;
}
} // namespace
} // namespace sgl::emit

sgl::emit::emitted_text sgl::emit::emit_entry_point(check::checked_module const& m,
                                                    check::flat_entry_point const& e,
                                                    target t,
                                                    options const& sources)
{
    // CHK-491: dx12 and vulkan read an unbounded array's length from the block sg writes per dispatch or draw
    if ((t == target::hlsl_dx12 || t == target::hlsl_vulkan) && check::reads_array_lengths(m, e))
    {
        auto const reading = check::with_reserved_array_lengths(m, e);
        if (!check::reads_array_lengths(m, reading))
            return emit_entry_point(m, reading, t, sources);
    }
    // CHK-520: a bare @compute's threads past the grid sg writes return; metal dispatches the grid's threads alone
    if (e.is_auto_workgroup && t != target::msl && !check::guards_dispatch_grid(m, e))
    {
        auto const guarded = check::with_dispatch_grid_guard(m, e);
        if (check::guards_dispatch_grid(m, guarded))
            return emit_entry_point(m, guarded, t, sources);
    }

    // CHK-524: a polyfilled mesh entry point is written as a compute or a vertex entry point, from the same slots
    // A task entry point is written as the compute entry point of its records; a mesh stage it launches takes it by
    // name, which only a compile of the whole source has (`text_request::polyfill_task`).
    if (e.entry_stage == check::stage::task && sources.mesh == mesh_form::compute)
    {
        auto const layout = check::task_polyfill_layout_of(m, e);
        if (layout.has_error())
        {
            auto refused = emitted_text();
            refused.errors.push_back({.kind = error_kind::unsupported, .symbol = e.function, .detail = layout.error()});
            return refused;
        }
        auto native = sources;
        native.mesh = mesh_form::native;
        auto result = emit_entry_point(m, check::task_as_compute(m, e), t, native);
        result.task_layout = layout.value();
        return result;
    }
    if (e.entry_stage == check::stage::mesh && sources.mesh != mesh_form::native)
    {
        // a task stage is named by a compile of the whole source alone, so the scan and the launched form here are an
        // indirect draw's, whose one record carries no payload
        auto const layout = check::mesh_polyfill_layout_of(m, e, sources.mesh == mesh_form::vertex_fetch);
        if (layout.has_error())
        {
            auto refused = emitted_text();
            refused.errors.push_back({.kind = error_kind::unsupported, .symbol = e.function, .detail = layout.error()});
            return refused;
        }
        auto native = sources;
        native.mesh = mesh_form::native;
        auto const indirect = check::task_polyfill_layout{};
        if (sources.mesh == mesh_form::compute || sources.mesh == mesh_form::launched)
        {
            auto const is_launched = sources.mesh == mesh_form::launched;
            auto result = emit_entry_point(m, check::mesh_as_compute(m, e, is_launched ? &indirect : nullptr), t, native);
            result.mesh_layout = layout.value();
            if (is_launched)
                result.task_layout = indirect;
            return result;
        }
        if (sources.mesh == mesh_form::scan)
        {
            auto result = emit_entry_point(m, check::mesh_task_scan(m, e, indirect), t, native);
            result.mesh_layout = layout.value();
            result.task_layout = indirect;
            return result;
        }
        // a vertex half that hands values per primitive on returns a struct of both, which a copy of the module holds
        auto const parts = check::mesh_corner_parts_of(m, e);
        auto extended = cc::optional<check::checked_module>();
        auto corner = check::type_id::none;
        if (check::is_valid(parts.primitive))
        {
            extended = m;
            corner = check::add_mesh_corner(extended.value(), parts);
        }
        auto const& written = extended.has_value() ? extended.value() : m;
        auto result = emit_entry_point(written, check::mesh_as_vertex_fetch(written, e, corner), t, native);
        result.mesh_layout = layout.value();
        return result;
    }
    // CHK-524: the pixel stage of a polyfilled mesh pipeline takes that struct in place of its two
    if (e.entry_stage == check::stage::pixel && sources.mesh == mesh_form::vertex_fetch)
    {
        auto native = sources;
        native.mesh = mesh_form::native;
        auto const parts = check::mesh_corner_parts_of(m, e);
        if (!check::is_valid(parts.primitive))
            return emit_entry_point(m, e, t, native);
        auto extended = m;
        auto const corner = check::add_mesh_corner(extended, parts);
        return emit_entry_point(extended, check::pixel_after_mesh_polyfill(extended, e, corner), t, native);
    }

    auto result = emitted_text();
    impl::validate(m, e, result.errors, sources.diagnostics);
    if (!result.errors.empty())
        return result;

    // EMIT-109: WebGPU has none of these on any device, and the text would spell a type WGSL does not have.
    if (t == target::wgsl)
    {
        auto const never
            = check::feature_set(check::feature::binding_arrays) | check::feature::multisampled_array_textures
            | check::feature::raytracing_pipeline | check::feature::geometry_shader
            | check::feature::tessellation_shader | check::feature::mesh_shader | check::feature::shader_int16
            | check::feature::device_coherence | check::feature::image_atomics | check::feature::first_instance
            | check::feature::helper_lane_query | check::feature::vertex_layer_output | check::feature::view_instancing
            | check::feature::unbounded_binding_arrays | check::feature::shader_int64 | check::feature::shader_f64
            | check::feature::shader_int64_atomics | check::feature::shader_shared_int64_atomics
            | check::feature::shader_int64_min_max_atomics;
        for (auto i = isize(0); i < check::k_feature_count; ++i)
            if (e.features.has(check::feature(i)) && never.has(check::feature(i)))
                result.errors.push_back({.kind = error_kind::target_lacks_feature,
                                         .symbol = e.function,
                                         .detail = cc::format("{} needs {}, which WebGPU does not have", e.name,
                                                              check::k_feature_names[i])});
        // EMIT-261: only a polyfilled mesh stage feeds WebGPU's pixel stage, which takes its values in another form
        if (e.entry_stage == check::stage::pixel && check::is_valid(check::mesh_corner_parts_of(m, e).primitive))
            result.errors.push_back(
                {.kind = error_kind::target_lacks_feature,
                 .symbol = e.function,
                 .detail = cc::format("{} takes what a mesh stage wrote per primitive, which needs mesh_shader, which "
                                      "WebGPU does not have; the pixel stage of a polyfilled mesh pipeline is written "
                                      "with `--mesh vertex` instead (CHK-524)",
                                      e.name)});

        if (!result.errors.empty())
            return result;
    }

    // EMIT-135: sg binds the roots as 16 words of one uniform, and the position is the call's last argument
    if (t == target::wgsl || sources.software_trace)
    {
        constexpr auto max_roots = 16;
        for (auto const& x : e.exprs)
        {
            auto const* const call = x.node.try_as<check::flat_call>();
            auto const* const record = call == nullptr ? nullptr : m.builtin_function(call->intrinsic);
            if (record == nullptr || !record->takes_acceleration_index)
                continue;
            auto const arguments = e.at(call->arguments);
            auto const* const k = e.at(arguments[arguments.size() - 1]).node.try_as<check::flat_int_literal>();
            auto const* const member = e.at(arguments[0]).node.try_as<check::flat_binding_member>();
            if (k == nullptr || k->value < max_roots)
                continue;
            result.errors.push_back({.kind = error_kind::too_many_acceleration_structures,
                                     .symbol = member != nullptr ? member->binding : e.function,
                                     .detail = cc::format("{} traces the acceleration member at position {}, and "
                                                          "sg binds {} roots",
                                                          e.name, k->value, max_roots)});
            return result;
        }
    }

    // EMIT-122: Metal has no geometry stage, and its tessellation is shaped otherwise: a compute kernel writes the factors
    if (t == target::msl
        && (e.features.has(check::feature::geometry_shader) || e.features.has(check::feature::tessellation_shader)))
    {
        auto const f = e.entry_stage == check::stage::geometry ? check::feature::geometry_shader
                                                               : check::feature::tessellation_shader;
        result.errors.push_back({.kind = error_kind::target_lacks_feature,
                                 .symbol = e.function,
                                 .detail = cc::format("{} is a {} stage and needs {}, which Metal does not have",
                                                      e.name, check::stage_name(e.entry_stage), check::name_of(f))});
        return result;
    }

    // EMIT-192: Metal has no double, and its 64-bit atomics are a ulong's min and max alone, in a buffer
    if (t == target::msl)
        for (auto const f : {check::feature::shader_f64, check::feature::shader_int64_atomics,
                             check::feature::shader_shared_int64_atomics})
            if (e.features.has(f))
            {
                result.errors.push_back(
                    {.kind = error_kind::target_lacks_feature,
                     .symbol = e.function,
                     .detail = cc::format("{} needs {}, which Metal does not have", e.name, check::name_of(f))});
                return result;
            }

    // EMIT-218: MSL's ulong min and max give nothing back, so one whose value the program reads has no text
    if (t == target::msl && e.features.has(check::feature::shader_int64_min_max_atomics))
    {
        auto standing = cc::vector<bool>::create_filled(e.exprs.size(), false);
        for (auto const& st : e.stmts)
            if (auto const* const eval = st.node.try_as<check::flat_eval>();
                eval != nullptr && check::is_valid(eval->value))
                standing[index_of(eval->value)] = true;
        for (auto i = isize(0); i < e.exprs.size(); ++i)
        {
            auto const* const call = e.exprs[i].node.try_as<check::flat_call>();
            auto const* const record = call != nullptr ? m.builtin_function(call->intrinsic) : nullptr;
            if (record == nullptr || !record->is_atomic
                || !record->features.has(check::feature::shader_int64_min_max_atomics) || standing[i])
                continue;
            result.errors.push_back({.kind = error_kind::target_lacks_feature,
                                     .symbol = e.function,
                                     .detail = cc::format("{} reads what an atomic ulong {} gave back, which Metal's "
                                                          "does not "
                                                          "give",
                                                          e.name, record->name)});
            return result;
        }
    }

    // EMIT-139: a stage that traces or stands in traversal agrees on the ray data with every shader it is linked with
    auto const uses_ray_data = !e.traced_rays.empty() || e.entry_stage == check::stage::any_hit
                            || e.entry_stage == check::stage::intersection;
    if (t == target::msl && uses_ray_data)
    {
        auto const payloads_of = [&](check::symbol_id set)
        {
            auto types = cc::vector<check::type_id>();
            for (auto const& ray : m.at(m.at(m.at(set).type).members))
            {
                auto at = isize(0);
                while (at < types.size() && index_of(types[at]) < index_of(ray.type))
                    ++at;
                if (at == types.size() || types[at] != ray.type)
                    types.insert_at(at, ray.type);
            }
            return types;
        };
        auto const owners = impl::owning_ray_sets(m, e);
        for (auto i = isize(1); i < owners.size(); ++i)
            if (!ast::impl::is_equal(payloads_of(owners[i]), payloads_of(owners[0])))
            {
                result.errors.push_back({.kind = error_kind::ray_data_conflict,
                                         .symbol = e.function,
                                         .detail = cc::format("{} is linked with ray sets '{}' and '{}', whose "
                                                              "payloads size Metal's ray "
                                                              "data differently",
                                                              e.name, m.at(owners[0]).name, m.at(owners[i]).name)});
                return result;
            }
    }

    auto plan = impl::make_plan(m, e, t, sources.device_features.has(check::feature::float_atomic_add));
    plan.diagnostics = sources.diagnostics;
    plan.sees_motion = sources.software_trace;
    plan.compiled_for = compile_target_of(t, sources.device_features);
    // EMIT-133: a ray-tracing stage's file sampler is a `constexpr sampler` on metal, which takes no mip bias
    if (t == target::msl && e.entry_stage >= check::stage::raygen)
        for (auto const& s : plan.samplers)
            if (m.samplers[m.at(s.symbol).info].mip_lod_bias != 0.0f)
                result.errors.push_back({.kind = error_kind::unsupported,
                                         .symbol = s.symbol,
                                         .detail = cc::format("{} samples through '{}', whose mip_lod_bias a "
                                                              "ray-tracing stage on Metal "
                                                              "does not carry yet",
                                                              e.name, s.host_name)});
    // EMIT-174: a link's array or matrix is declared as its pieces, which is no layout GPU memory has
    for (auto const& s : plan.structs)
        if (s.memory_size > 0)
            for (auto const& member : s.members)
                if (member.pieces > 0)
                {
                    result.errors.push_back({.kind = error_kind::unsupported,
                                             .symbol = m.at(s.type).symbol,
                                             .detail = cc::format("'{}' both crossing a stage edge and standing in GPU "
                                                                  "memory, where "
                                                                  "its '{}' crosses as its pieces",
                                                                  m.name_of(s.type), member.source_name)});
                    break;
                }
    if (!result.errors.empty())
        return result;
    // The plan decided every name and address already, so a binary answers to the host as the text would.
    auto is_native = false;
    if (sources.native != native_route::text)
    {
        auto program = ir::program();
        if (sources.native_line_table && !sources.files.empty())
            program.lines = line_table_of(m, plan.e, sources);
        auto const is_dxil = t == target::hlsl_dx12;
        auto const gap
            = is_dxil ? ir::compile_dxil(plan, program, result.dxil) : ir::compile_spirv(plan, program, result.spirv);
        if (is_dxil)
            result.signature = ir::dxil::pack_signature(program);
        if (!gap.has_value())
        {
            result.text = ir::dump(program);
            is_native = true;
        }
        else if (sources.native == native_route::prefer)
            result.native_gap = gap.value();
        else
        {
            result.errors.push_back({.kind = error_kind::unsupported,
                                     .symbol = e.function,
                                     .detail = cc::format("{} holds {}, which the {} route does not lower yet", e.name,
                                                          gap.value(), is_dxil ? "DXIL" : "SPIR-V")});
            return result;
        }
    }
    if (!is_native)
    {
        auto written = impl::write_text(plan, impl::dialect_of(t), sources);
        result.text = cc::move(written.text);
        result.lines = cc::move(written.lines);
    }
    result.debug_sites = e.debug_sites;
    result.entry_point = plan.entry_name;
    if (plan.constants.has_value())
        result.bound_names.push_back({.emitted = plan.constants.value().name, .host = plan.constants.value().host_name});
    for (auto const& block : plan.group_blocks)
        if (!block.is_reserved)
            result.bound_names.push_back({.emitted = block.name, .host = block.host_name});
    for (auto const& buffer : plan.resources)
        if (!buffer.is_reserved)
            result.bound_names.push_back({.emitted = buffer.name, .host = buffer.host_name});
    for (auto const& s : plan.samplers)
        result.bound_names.push_back({.emitted = s.name, .host = s.host_name});
    result.layouts = impl::layouts_of(plan);
    // a pixel stage that writes images alone has no target struct at all (CHK-418)
    if (e.entry_stage == check::stage::pixel && e.result != check::type_id::none
        && e.result != check::checked_module::void_type)
    {
        // a depth or a sample mask is an output of its own and no target (EMIT-130)
        result.color_targets = 0;
        for (auto const& member : m.at(m.at(e.result).members))
            result.color_targets += member.output == check::pixel_output::color ? 1 : 0;
        result.target_struct = cc::string(m.name_of(e.result));
    }
    return result;
}

cc::string sgl::emit::dump_errors(emitted_text const& e)
{
    auto out = cc::string();
    for (auto const& error : e.errors)
        out.appendf("{} {}\n", to_string(error.kind), error.detail);
    return out;
}
