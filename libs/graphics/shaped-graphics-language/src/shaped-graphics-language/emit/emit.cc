#include "emit.hh"

#include <clean-core/string/format.hh>
#include <shaped-graphics-language/emit/impl/dialect.hh>
#include <shaped-graphics-language/emit/impl/plan.hh>
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

sgl::emit::emitted_text sgl::emit::emit(check::checked_module const& m, sgl::isize entry_point, target t)
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
    return emit_entry_point(m, check::legalize(m, m.entry_points[entry_point], {.is_emulated = t == target::wgsl}), t);
}

sgl::emit::emitted_text sgl::emit::emit_entry_point(check::checked_module const& m,
                                                    check::flat_entry_point const& e,
                                                    target t)
{
    auto result = emitted_text();
    impl::validate(m, e, result.errors);
    if (!result.errors.empty())
        return result;

    // EMIT-109: WebGPU has none of these on any device, and the text would spell a type WGSL does not have.
    if (t == target::wgsl)
    {
        auto const never
            = check::feature_set(check::feature::binding_arrays) | check::feature::multisampled_array_textures
            | check::feature::raytracing_pipeline | check::feature::geometry_shader | check::feature::tessellation_shader
            | check::feature::shader_int16 | check::feature::device_coherence | check::feature::image_atomics;
        for (auto i = isize(0); i < check::k_feature_count; ++i)
            if (e.features.has(check::feature(i)) && never.has(check::feature(i)))
                result.errors.push_back({.kind = error_kind::target_lacks_feature,
                                         .symbol = e.function,
                                         .detail = cc::format("{} needs {}, which WebGPU does not have", e.name,
                                                              check::k_feature_names[i])});

        // EMIT-135: sg binds the roots as 16 words of one uniform, and the position is the call's last argument
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
                                                          "WebGPU binds {} roots",
                                                          e.name, k->value, max_roots)});
            break;
        }
        if (!result.errors.empty())
            return result;
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

    auto plan = impl::make_plan(m, e, t);
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
    if (!result.errors.empty())
        return result;
    result.text = impl::write_text(plan, impl::dialect_of(t));
    result.entry_point = plan.entry_name;
    if (plan.constants.has_value())
        result.bound_names.push_back({.emitted = plan.constants.value().name, .host = plan.constants.value().host_name});
    for (auto const& block : plan.group_blocks)
        result.bound_names.push_back({.emitted = block.name, .host = block.host_name});
    for (auto const& buffer : plan.resources)
        result.bound_names.push_back({.emitted = buffer.name, .host = buffer.host_name});
    for (auto const& s : plan.samplers)
        result.bound_names.push_back({.emitted = s.name, .host = s.host_name});
    result.layouts = impl::layouts_of(plan);
    if (e.entry_stage == check::stage::pixel && e.result != check::type_id::none)
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
