#include "pipeline.hh"

#include <clean-core/common/assertf.hh>
#include <clean-core/common/log.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/fixed_array.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics-language/driver/describe.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/exceptions.hh>
#include <shaped-shader-library/impl/frozen.hh>
#include <shaped-shader-library/impl/kept_builds.hh>
#include <shaped-shader-library/impl/pipeline_fields.hh>
#include <shaped-shader-library/impl/sgl_emit_cache.hh>
#include <shaped-shader-library/impl/sgl_library.hh>
#include <shaped-shader-library/shader_asset.hh>

using namespace cc::primitive_defines;

namespace
{
using sg::raster_pipeline_description;
using slib::pipeline_setting;
using slib::setting_kind;

constexpr cc::string_view k_color_targets = "color_targets.";

/// The generated field `path` names, `color_targets.<target>` read as `color_targets.*`, and the target's index.
struct found_field
{
    slib::impl::field const* field = nullptr;
    int target = -1;
    cc::string error;
};

found_field field_of(cc::string_view path, cc::span<cc::string_view const> targets, isize target_count)
{
    auto key = cc::string(path);
    auto target = -1;
    if (path.starts_with(k_color_targets))
    {
        auto const rest = path.subview({.offset = k_color_targets.size(), .size = path.size() - k_color_targets.size()});
        auto const dot = rest.find('.');
        auto const name = dot < 0 ? rest : rest.subview({.offset = 0, .size = dot});
        for (auto i = isize(0); i < targets.size(); ++i)
            if (targets[i] == name)
                target = int(i);
        if (target < 0 || target >= target_count)
            return {.error = cc::format("{}: the pipeline has no target {}", path, name)};
        key = cc::format("{}*{}", k_color_targets,
                         dot < 0 ? cc::string_view() : rest.subview({.offset = dot, .size = rest.size() - dot}));
    }
    for (auto const& f : slib::impl::fields::all)
        if (f.path == key)
            return {.field = &f, .target = target};
    return {.error = cc::format("{}: sg's raster_pipeline_description has no such field", path)};
}

/// The value `s` gives field `f`, or nothing where the field cannot take it.
cc::optional<slib::impl::field_value> value_for(slib::impl::field const& f, pipeline_setting const& s)
{
    using slib::impl::field_kind;
    switch (f.kind)
    {
    case field_kind::boolean:
        if (s.kind == setting_kind::boolean)
            return slib::impl::field_value{.integer = s.integer};
        return {};
    case field_kind::integer:
        if (s.kind == setting_kind::integer)
            return slib::impl::field_value{.integer = s.integer};
        return {};
    case field_kind::real:
        if (s.kind == setting_kind::real)
            return slib::impl::field_value{.real = s.real};
        if (s.kind == setting_kind::integer)
            return slib::impl::field_value{.real = f64(s.integer)};
        return {};
    case field_kind::enum_case:
        if (s.kind == setting_kind::enum_case)
            for (auto const& c : f.cases)
                if (c.name == s.enum_case)
                    return slib::impl::field_value{.integer = c.value};
        return {};
    case field_kind::none:
        if (s.kind == setting_kind::none)
            return slib::impl::field_value{};
        return {};
    }
    return {};
}
} // namespace

cc::result<cc::unit, cc::string> slib::apply_settings(sg::raster_pipeline_description& desc,
                                                      cc::span<pipeline_setting const> settings,
                                                      cc::span<cc::string_view const> targets)
{
    for (auto const& s : settings)
    {
        // What the host states arrives as an open part, after the settings.
        if (s.kind == setting_kind::host)
            continue;
        auto const found = field_of(s.path, targets, desc.color_targets.size());
        if (found.field == nullptr)
            return cc::error(found.error);
        auto const value = value_for(*found.field, s);
        if (!value.has_value())
            return cc::error(cc::format("{}: the field cannot take this value", s.path));
        found.field->set(desc, found.target, value.value());
    }
    return cc::unit{};
}

namespace
{
/// The paths `settings` leaves to the host: those whose last setting is `.host`.
cc::vector<cc::string_view> open_paths_of(cc::span<pipeline_setting const> settings)
{
    auto result = cc::vector<cc::string_view>();
    for (auto i = isize(0); i < settings.size(); ++i)
    {
        auto is_last = true;
        for (auto j = i + 1; j < settings.size(); ++j)
            is_last = is_last && settings[j].path != settings[i].path;
        if (is_last && settings[i].kind == setting_kind::host)
            result.push_back(settings[i].path);
    }
    return result;
}

/// A string that lives as long as the process, so a setting read from a reloaded source is viewed like a baked one.
/// A reload adds a handful and most repeat, so they are kept rather than freed.
cc::string_view interned(cc::string_view text)
{
    static auto strings = cc::mutex<cc::vector<cc::unique_ptr<cc::string>>>();
    return strings.lock(
        [&](cc::vector<cc::unique_ptr<cc::string>>& all) -> cc::string_view
        {
            for (auto const& s : all)
                if (*s == text)
                    return *s;
            all.push_back(cc::make_unique<cc::string>(text));
            return *all.back();
        });
}

/// `key = value` split at its first ` = `; a line without one is all key.
struct frozen_line
{
    cc::string_view key;
    cc::string_view value;
};

frozen_line split(cc::string_view line)
{
    auto const at = line.find(" = ");
    if (at < 0)
        return {.key = line};
    return {.key = line.subview({.offset = 0, .size = at}),
            .value = line.subview({.offset = at + 3, .size = line.size() - at - 3})};
}

/// What moved from `built` to `now`, one line each; empty where nothing did.
template <class Built, class Now>
cc::string moved(Built const& built, Now const& now)
{
    auto const value_in = [](auto const& lines, cc::string_view key) -> cc::string_view
    {
        for (auto const& line : lines)
            if (auto const l = split(line); l.key == key)
                return l.value;
        return "<unset>";
    };
    auto out = cc::string();
    auto const add = [&](auto const& lines)
    {
        for (auto const& line : lines)
        {
            auto const key = split(line).key;
            auto const was = value_in(built, key);
            auto const is = value_in(now, key);
            if (was != is && !out.contains(cc::format("{}:", key)))
                out.appendf("{}: {} -> {}\n", key, was, is);
        }
    };
    add(built);
    add(now);
    return out;
}

/// `d`'s stage handles, each null without its stage.
cc::fixed_array<slib::shader_asset_handle const*, 7> stages_of(slib::pipeline_definition const& d)
{
    return {d.vertex, d.pixel, d.geometry, d.tessellation_control, d.tessellation_evaluation, d.mesh, d.task};
}

/// The stage whose source holds the pipeline declaration's file: the vertex stage, or the mesh stage in its place.
slib::shader_asset_handle const* first_stage_of(slib::pipeline_definition const& d)
{
    return d.mesh != nullptr ? d.mesh : d.vertex;
}

/// One declared pipeline's reload state, kept for the life of the process like the definition it belongs to.
struct live_pipeline
{
    slib::pipeline_definition const* definition = nullptr;
    /// The stages' generations the configuration was last read at, in `stages_of` order; the build's settings are
    /// generation 0's.
    cc::fixed_array<u64, 7> generations = {};
    slib::pipeline_configuration configuration;

    /// The stages last described on a context with a set of option values, while the frozen part still matched.
    /// A compiled shader carries its own bytecode, so keeping one keeps what the host's code fits.
    struct stages
    {
        sg::compiled_shader vertex;
        cc::optional<sg::compiled_shader> pixel;
        cc::optional<sg::compiled_shader> geometry;
        cc::optional<sg::compiled_shader> tessellation_control;
        cc::optional<sg::compiled_shader> tessellation_evaluation;
        cc::optional<sg::compiled_shader> mesh;
        cc::optional<sg::compiled_shader> task;
        cc::optional<sg::mesh_polyfill_description> mesh_polyfill;
    };
    slib::impl::kept_builds<stages> kept;
};

/// The stages `desc` was described with, to fall back to after a reload moves the frozen part.
live_pipeline::stages stages_in(sg::raster_pipeline_description const& desc)
{
    return {.vertex = desc.vertex_shader,
            .pixel = desc.fragment_shader,
            .geometry = desc.geometry_shader,
            .tessellation_control = desc.tessellation_control_shader,
            .tessellation_evaluation = desc.tessellation_evaluation_shader,
            .mesh = desc.mesh_shader,
            .task = desc.task_shader,
            .mesh_polyfill = desc.mesh_polyfill};
}

void use_stages(sg::raster_pipeline_description& desc, live_pipeline::stages const& s)
{
    desc.vertex_shader = s.vertex;
    desc.fragment_shader = s.pixel;
    desc.geometry_shader = s.geometry;
    desc.tessellation_control_shader = s.tessellation_control;
    desc.tessellation_evaluation_shader = s.tessellation_evaluation;
    desc.mesh_shader = s.mesh;
    desc.task_shader = s.task;
    desc.mesh_polyfill = s.mesh_polyfill;
}

cc::mutex<cc::vector<cc::unique_ptr<live_pipeline>>>& live_pipelines()
{
    static auto all = cc::mutex<cc::vector<cc::unique_ptr<live_pipeline>>>();
    return all;
}

/// `d`'s record, made on first use with the build's configuration at generation 0.
/// Only called under the lock.
live_pipeline& live_of(cc::vector<cc::unique_ptr<live_pipeline>>& all, slib::pipeline_definition const& d)
{
    for (auto const& live : all)
        if (live->definition == &d)
            return *live;
    all.push_back(cc::make_unique<live_pipeline>(live_pipeline{.definition = &d}));
    return *all.back();
}
} // namespace

cc::string slib::impl::frozen_moved(cc::span<cc::string_view const> built, cc::span<cc::string const> now)
{
    return moved(built, now);
}

slib::pipeline_configuration slib::configuration_of(pipeline_definition const& d)
{
    auto generations = cc::fixed_array<u64, 7>{};
    auto const stages = stages_of(d);
    for (auto i = isize(0); i < stages.size(); ++i)
        generations[i] = stages[i] != nullptr && *stages[i] != nullptr ? (*stages[i])->generation() : 0;

    // What is known now, and whether a reload moved a stage since it was read.
    auto needs_read = false;
    auto current = live_pipelines().lock(
        [&](cc::vector<cc::unique_ptr<live_pipeline>>& all) -> pipeline_configuration
        {
            auto const& live = live_of(all, d);
            for (auto i = isize(0); i < generations.size(); ++i)
                needs_read = needs_read || live.generations[i] != generations[i];
            return live.configuration;
        });
    if (!needs_read)
        return current;

    // A stage reloaded, so the source may say something new: described outside the lock, since it checks the whole file.
    auto next = current;
    auto const& first = *first_stage_of(d);
    auto const source = first->read_source();
    auto const modules = first->read_modules();
    auto const library = impl::sgl_library_of(modules.files());
    auto const described
        = source.has_value()
            ? slib::impl::cached_describe(
                  {.source = source.value(), .source_name = first->virtual_path(), .library = library})
            : cc::result<sgl::module_description, cc::string>(cc::error(cc::string("the source is gone")));
    auto const* found = static_cast<sgl::described_pipeline const*>(nullptr);
    if (described.has_value())
        for (auto const& p : described.value().pipelines)
            if (p.name == d.name)
                found = &p;

    if (found == nullptr)
        CC_LOG_WARNING("{}'s pipeline {} keeps its configuration, since the reloaded source does not state it: {}",
                       d.file, d.name,
                       described.has_value() ? cc::string("no pipeline of that name") : described.error());
    else
    {
        auto latest = cc::vector<pipeline_setting>();
        for (auto const& s : found->settings)
            latest.push_back({.path = interned(s.path),
                              .kind = setting_kind(s.kind),
                              .integer = s.integer,
                              .real = s.real,
                              .enum_case = interned(s.enum_case)});
        next.latest = cc::move(latest);

        next.frozen_moved = moved(d.frozen, found->frozen);
        if (next.frozen_moved.empty())
            next.settings = next.latest;
        else
            CC_LOG_WARNING("{}'s pipeline {} keeps what it was last built with: the reloaded source moves what the "
                           "host "
                           "was built against\n{}",
                           d.file, d.name, next.frozen_moved);
    }

    live_pipelines().lock(
        [&](cc::vector<cc::unique_ptr<live_pipeline>>& all)
        {
            auto& live = live_of(all, d);
            live.configuration = next;
            live.generations = generations;
        });
    return next;
}

cc::shared_async<sg::raster_pipeline_description> slib::describe_raster_pipeline(sg::context* ctx,
                                                                                 pipeline_definition const* definition,
                                                                                 cc::vector<open_part> open,
                                                                                 sg::raster_pipeline_customize customize,
                                                                                 bool latest,
                                                                                 cc::vector<shader_option> options)
{
    auto const& d = *definition;
    CC_ASSERTF(first_stage_of(d) != nullptr && *first_stage_of(d) != nullptr,
               "{}'s {}: its package was never added to a shader library, so it has no shaders", d.file, d.name);
    // What the host states is the host's to get right: a format it left `undefined`, or a sample count of 0.
    for (auto const& part : open)
        if (part.path.ends_with("sample_count"))
            CC_ASSERTF(part.value >= 1 && part.value <= 64 && (part.value & (part.value - 1)) == 0,
                       "{}'s {}: the sample count is stated as {}, and it is a power of two from 1 to 64", d.file,
                       d.name, part.value);
        else
            CC_ASSERTF(part.value > 0, "{}'s {}: {} is stated as no format", d.file, d.name, part.path);

    // A stage the device lacks is refused by its feature before any stage compiles, since a target without the stage
    // can fail an earlier one first: Metal rejects a vertex stage that feeds tessellation and writes no position.
    // The tessellation stages likewise, opt-in until their benchmark against the native stages has run (SGL's CHK-527).
    auto const tessellation_use = ctx->tessellation_polyfill();
    auto const polyfills_tessellation
        = d.tessellation_control != nullptr && d.tessellation_evaluation != nullptr && d.vertex != nullptr
       && (tessellation_use == sg::polyfill_use::always
           || (tessellation_use == sg::polyfill_use::where_missing && !ctx->supports(sg::feature::tessellation_shader)));
    if ((d.tessellation_control != nullptr || d.tessellation_evaluation != nullptr) && !polyfills_tessellation
        && !ctx->supports(sg::feature::tessellation_shader))
        throw sg::pipeline_creation_exception(cc::string(d.name),
                                              cc::any_error(cc::format("{}'s {}: the pipeline has tessellation stages, "
                                                                       "and this device "
                                                                       "lacks sg::feature::tessellation_shader",
                                                                       d.file, d.name)));
    // A mesh stage is drawn as its polyfill where the context asks for one, and its task stage with it.
    auto const polyfill_use = ctx->mesh_polyfill();
    auto const polyfills
        = d.mesh != nullptr
       && (polyfill_use == sg::polyfill_use::always
           || (polyfill_use == sg::polyfill_use::where_missing && !ctx->supports(sg::feature::mesh_shader)));
    if (d.mesh != nullptr && !polyfills && !ctx->supports(sg::feature::mesh_shader))
        throw sg::pipeline_creation_exception(
            cc::string(d.name), cc::any_error(cc::format("{}'s {}: the pipeline has a mesh stage, and this device "
                                                         "lacks sg::feature::mesh_shader",
                                                         d.file, d.name)));
    // A geometry stage likewise, opt-in until its benchmark against the native stage has run (SGL's CHK-525).
    auto const geometry_use = ctx->geometry_polyfill();
    auto const polyfills_geometry
        = d.geometry != nullptr && d.vertex != nullptr
       && (geometry_use == sg::polyfill_use::always
           || (geometry_use == sg::polyfill_use::where_missing && !ctx->supports(sg::feature::geometry_shader)));
    if (d.geometry != nullptr && !polyfills_geometry && !ctx->supports(sg::feature::geometry_shader))
        throw sg::pipeline_creation_exception(
            cc::string(d.name), cc::any_error(cc::format("{}'s {}: the pipeline has a geometry stage, and this device "
                                                         "lacks sg::feature::geometry_shader",
                                                         d.file, d.name)));

    // A polyfilled geometry stage's mesh route, where the context asks for it and the device has mesh shaders.
    auto const meshes_geometry = polyfills_geometry && ctx->geometry_polyfill_route() == sg::geometry_polyfill_route::mesh
                              && ctx->supports(sg::feature::mesh_shader) && ctx->draws_geometry_mesh_route();

    // The stages first: awaiting them is what promotes a reload, which the configuration is then read against.
    auto desc = raster_pipeline_description();
    desc.layout = d.acquire_layout(*ctx);
    // a polyfilled tessellation's vertex stage runs inside its control pass alone, and need not draw on its own; nor
    // does a geometry stage's mesh route's, inside its mesh stage
    if (d.vertex != nullptr && !polyfills_tessellation && !meshes_geometry)
        desc.vertex_shader = co_await (*d.vertex)->acquire(*ctx, options);
    if (d.mesh != nullptr && polyfills)
    {
        // the compute half and the scan read the task stage's records, so they name it; with no task stage, an
        // indirect draw launches a half of its own by one record, which the scan numbers too
        auto const half = [&](cc::string_view form)
        {
            auto with_form = options;
            with_form.push_back({.name = cc::string(slib::k_mesh_form_option), .value = cc::string(form)});
            if (d.task != nullptr && form != "vertex")
                with_form.push_back(
                    {.name = cc::string(slib::k_polyfill_task_option), .value = cc::string((*d.task)->entry_point())});
            return (*d.mesh)->acquire(*ctx, with_form);
        };
        auto compute = co_await half("compute");
        desc.vertex_shader = co_await half("vertex");
        desc.mesh_polyfill = sg::mesh_polyfill_description{.corners = compute.mesh_polyfill_corners,
                                                           .primitives_per_group = compute.mesh_polyfill_primitives,
                                                           .words_per_group = compute.mesh_polyfill_words};
        auto& p = desc.mesh_polyfill.value();
        if (d.task != nullptr)
        {
            auto with_form = options;
            with_form.push_back({.name = cc::string(slib::k_mesh_form_option), .value = cc::string("compute")});
            auto task = co_await (*d.task)->acquire(*ctx, with_form);
            p.record_words = task.task_polyfill_record_words;
            p.task_meshes = task.task_polyfill_meshes;
            p.task_shader = cc::move(task);
        }
        else
        {
            auto launched = co_await half("launched");
            p.record_words = launched.task_polyfill_record_words;
            p.launched_shader = cc::move(launched);
        }
        p.scan_shader = co_await half("scan");
        desc.mesh_polyfill.value().compute_shader = cc::move(compute);
    }
    else if (d.mesh != nullptr)
        desc.mesh_shader = co_await (*d.mesh)->acquire(*ctx, options);
    if (d.task != nullptr && !polyfills)
        desc.task_shader = co_await (*d.task)->acquire(*ctx, options);
    // the pixel stage of a polyfilled mesh stage takes what it wrote per primitive from the vertex half (SGL's CHK-524)
    if (d.pixel != nullptr && polyfills)
    {
        auto with_form = options;
        with_form.push_back({.name = cc::string(slib::k_mesh_form_option), .value = cc::string("vertex")});
        desc.fragment_shader = co_await (*d.pixel)->acquire(*ctx, with_form);
    }
    else if (d.pixel != nullptr)
        desc.fragment_shader = co_await (*d.pixel)->acquire(*ctx, options);
    if (d.geometry != nullptr && meshes_geometry)
    {
        // one mesh stage runs the pipeline's vertex stage and the geometry stage, so it names the vertex stage
        auto with_form = options;
        with_form.push_back({.name = cc::string(slib::k_mesh_form_option), .value = cc::string("mesh")});
        with_form.push_back(
            {.name = cc::string(slib::k_polyfill_vertex_option), .value = cc::string((*d.vertex)->entry_point())});
        desc.mesh_shader = co_await (*d.geometry)->acquire(*ctx, with_form);
    }
    else if (d.geometry != nullptr && polyfills_geometry)
    {
        // the compute half runs the pipeline's vertex stage, so it names it; the vertex half draws the slots back
        auto with_form = options;
        with_form.push_back({.name = cc::string(slib::k_mesh_form_option), .value = cc::string("vertex")});
        desc.vertex_shader = co_await (*d.geometry)->acquire(*ctx, with_form);
        with_form.back().value = "compute";
        with_form.push_back(
            {.name = cc::string(slib::k_polyfill_vertex_option), .value = cc::string((*d.vertex)->entry_point())});
        auto compute = co_await (*d.geometry)->acquire(*ctx, with_form);
        desc.mesh_polyfill = sg::mesh_polyfill_description{.corners = compute.mesh_polyfill_corners,
                                                           .primitives_per_group = compute.mesh_polyfill_primitives,
                                                           .words_per_group = compute.mesh_polyfill_words};
        desc.mesh_polyfill.value().compute_shader = cc::move(compute);
    }
    else if (d.geometry != nullptr)
        desc.geometry_shader = co_await (*d.geometry)->acquire(*ctx, options);
    if (polyfills_tessellation)
    {
        // the vertex half draws the points back; the control stage's compute half runs the pipeline's vertex stage, and
        // the evaluation stage's reads the control stage's patches, so each names the other
        auto with_form = options;
        with_form.push_back({.name = cc::string(slib::k_mesh_form_option), .value = cc::string("vertex")});
        desc.vertex_shader = co_await (*d.tessellation_evaluation)->acquire(*ctx, with_form);
        with_form.back().value = "compute";
        auto control_options = with_form;
        control_options.push_back(
            {.name = cc::string(slib::k_polyfill_vertex_option), .value = cc::string((*d.vertex)->entry_point())});
        auto control = co_await (*d.tessellation_control)->acquire(*ctx, control_options);
        auto evaluation_options = with_form;
        evaluation_options.push_back({.name = cc::string(slib::k_polyfill_control_option),
                                      .value = cc::string((*d.tessellation_control)->entry_point())});
        auto evaluation = co_await (*d.tessellation_evaluation)->acquire(*ctx, evaluation_options);
        // the prelude's passes, compiled through the control stage's source
        auto const pass = [&](cc::string_view name)
        {
            auto with_pass = options;
            with_pass.push_back({.name = cc::string(slib::k_polyfill_pass_option), .value = cc::string(name)});
            return (*d.tessellation_control)->acquire(*ctx, with_pass);
        };
        auto polyfill = sg::tessellation_polyfill_description{.domain = control.tessellation_domain,
                                                              .control_points = control.tessellation_control_points,
                                                              .patch_words = control.tessellation_patch_words,
                                                              .vertex_words = evaluation.tessellation_vertex_words,
                                                              .partitioning = control.tessellation_partitioning,
                                                              .output = control.tessellation_output,
                                                              .max_factor = float(control.tessellation_max_factor)};
        polyfill.count_shader = co_await pass("sg_tessellation_count");
        polyfill.scan_shader = co_await pass("sg_tessellation_scan");
        polyfill.generate_shader = co_await pass("sg_tessellation_generate");
        polyfill.control_shader = cc::move(control);
        polyfill.evaluation_shader = cc::move(evaluation);
        desc.tessellation_polyfill = cc::move(polyfill);
    }
    else
    {
        if (d.tessellation_control != nullptr)
            desc.tessellation_control_shader = co_await (*d.tessellation_control)->acquire(*ctx, options);
        if (d.tessellation_evaluation != nullptr)
            desc.tessellation_evaluation_shader = co_await (*d.tessellation_evaluation)->acquire(*ctx, options);
    }
    // a vertex stage that draws from no vertex buffer, reading `@vertex_index` alone, has no layout; nor has a polyfill's
    // vertex half, whose compute half reads the buffers instead
    if (d.vertex_input != nullptr && !polyfills_geometry && !polyfills_tessellation)
        desc.vertex_input = d.vertex_input();
    desc.target_set = cc::string(d.target_set);
    for (auto i = isize(0); i < d.targets.size(); ++i)
        desc.color_targets.push_back({});

    auto const configuration = configuration_of(d);

    // Where the frozen part moved, the stages this context last built with these values are what the host's code fits.
    if (!latest)
    {
        auto const is_moved = !configuration.frozen_moved.empty();
        auto const is_kept = live_pipelines().lock(
            [&](cc::vector<cc::unique_ptr<live_pipeline>>& all) -> bool
            {
                auto stages = stages_in(desc);
                if (!live_of(all, d).kept.keep_or_restore(ctx, options, is_moved, stages))
                    return false;
                use_stages(desc, stages);
                return true;
            });
        if (!is_kept)
            throw sg::pipeline_creation_exception(
                cc::string(d.name), cc::any_error(cc::format(
                                        "{}'s {}: the source moved what the host was built against before the pipeline "
                                        "was ever described on this context, so no build is kept to fall back to\n{}",
                                        d.file, d.name, configuration.frozen_moved)));
    }

    auto const& settings = latest ? configuration.latest : configuration.settings;
    if (!settings.has_value())
        d.apply(desc, open);
    else
    {
        auto const applied = slib::apply_settings(desc, settings.value(), d.targets);
        CC_ASSERTF(applied.has_value(), "{}'s {}: {}", d.file, d.name,
                   applied.has_value() ? cc::string() : applied.error());

        // Each open part as the setting it stands for, with the host's value in place of `.host`.
        for (auto const path : open_paths_of(settings.value()))
        {
            auto const* part = static_cast<slib::open_part const*>(nullptr);
            for (auto const& p : open)
                if (p.path == path)
                    part = &p;
            CC_ASSERTF(part != nullptr, "{}'s {} leaves {} to the host, and the acquire does not state it", d.file,
                       d.name, path);
            auto const found = field_of(path, d.targets, desc.color_targets.size());
            CC_ASSERTF(found.field != nullptr, "{}'s {}: {}", d.file, d.name, found.error);
            found.field->set(desc, found.target, {.integer = part->value});
        }
    }

    // a polyfilled geometry stage assembles the primitives its topology states, a list's alone so far
    if (polyfills_geometry)
    {
        auto const t = desc.topology;
        auto const corners = t == sg::primitive_topology::point_list    ? 1
                           : t == sg::primitive_topology::line_list     ? 2
                           : t == sg::primitive_topology::triangle_list ? 3
                                                                        : 0;
        if (corners == 0)
            throw sg::pipeline_creation_exception(cc::string(d.name),
                                                  cc::any_error(cc::format("{}'s {}: the pipeline polyfills its "
                                                                           "geometry stage over a strip, and the "
                                                                           "polyfill assembles lists alone so far",
                                                                           d.file, d.name)));
        if (meshes_geometry)
        {
            // the mesh stage draws the list of the kind the geometry stage writes, which its compile states
            desc.geometry_mesh_corners = corners;
            desc.topology = desc.mesh_shader.has_value() && desc.mesh_shader.value().mesh_polyfill_corners == 2
                              ? sg::primitive_topology::line_list
                              : sg::primitive_topology::triangle_list;
        }
        else
            desc.mesh_polyfill.value().input_corners = corners;
    }
    // a polyfilled stage draws its primitives as a list of the kind it writes
    if (desc.mesh_polyfill.has_value())
        desc.topology = desc.mesh_polyfill.value().corners == 2 ? sg::primitive_topology::line_list
                                                                : sg::primitive_topology::triangle_list;
    // polyfilled tessellation draws the list its output topology states
    if (desc.tessellation_polyfill.has_value())
    {
        desc.topology = desc.tessellation_polyfill.value().output == 2 ? sg::primitive_topology::line_list
                                                                       : sg::primitive_topology::triangle_list;
        desc.patch_control_points = 0;
    }
    if (customize)
        customize(desc);
    co_return desc;
}
