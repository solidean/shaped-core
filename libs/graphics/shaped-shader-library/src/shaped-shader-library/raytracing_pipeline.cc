#include <clean-core/common/assertf.hh>
#include <clean-core/common/log.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics-language/driver/describe.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/exceptions.hh>
#include <shaped-shader-library/impl/frozen.hh>
#include <shaped-shader-library/impl/kept_builds.hh>
#include <shaped-shader-library/impl/sgl_library.hh>
#include <shaped-shader-library/raytracing_pipeline.hh>
#include <shaped-shader-library/shader_asset.hh>
#include <shaped-shader-library/shader_library.hh>

using namespace cc::primitive_defines;

namespace
{
/// Every shader handle of the module `d` names, in a fixed order; the host's parts are not among them.
cc::vector<slib::shader_asset_handle const*> module_handles(slib::raytracing_pipeline_definition const& d)
{
    auto result = cc::vector<slib::shader_asset_handle const*>();
    auto const add = [&](slib::shader_asset_handle const* h)
    {
        if (h != nullptr && *h != nullptr)
            result.push_back(h);
    };
    add(d.raygen);
    for (auto const* const miss : d.misses)
        add(miss);
    for (auto const& group : d.hit_groups)
    {
        add(group.intersection);
        for (auto const* const h : group.closest_hits)
            add(h);
        for (auto const* const h : group.any_hits)
            add(h);
        for (auto const* const h : group.metal_traversals)
            add(h);
    }
    for (auto const* const callable : d.callables)
        add(callable);
    add(d.empty_closest_hit);
    return result;
}

/// One declared ray-tracing pipeline's reload state, kept for the life of the process like its definition.
struct live_raytracing_pipeline
{
    slib::raytracing_pipeline_definition const* definition = nullptr;
    /// Parallel to `module_handles`: each generation when the frozen part was last read; empty is the build's, all 0.
    cc::vector<u64> generations;
    /// What moved at that read; empty while the source states the build's frozen part.
    cc::string frozen_moved;

    /// The module's part of the description last built on a context with a set of option values, while the frozen part
    /// still matched the build.
    slib::impl::kept_builds<sg::raytracing_pipeline_description> kept;
};

cc::mutex<cc::vector<cc::unique_ptr<live_raytracing_pipeline>>>& live_raytracing_pipelines()
{
    static auto all = cc::mutex<cc::vector<cc::unique_ptr<live_raytracing_pipeline>>>();
    return all;
}

/// `d`'s record, made on first use; only called under the lock.
live_raytracing_pipeline& live_of(cc::vector<cc::unique_ptr<live_raytracing_pipeline>>& all,
                                  slib::raytracing_pipeline_definition const& d)
{
    for (auto const& live : all)
        if (live->definition == &d)
            return *live;
    all.push_back(cc::make_unique<live_raytracing_pipeline>(live_raytracing_pipeline{.definition = &d}));
    return *all.back();
}

/// Whether `a` and `b` name the same things one by one.
bool is_same(cc::span<cc::string_view const> a, cc::span<cc::string const> b)
{
    if (a.size() != b.size())
        return false;
    for (auto i = isize(0); i < a.size(); ++i)
        if (a[i] != b[i])
            return false;
    return true;
}
} // namespace

cc::string slib::frozen_moved_of(raytracing_pipeline_definition const& d)
{
    auto const handles = module_handles(d);
    auto generations = cc::vector<u64>();
    for (auto const* const h : handles)
        generations.push_back((*h)->generation());

    auto needs_read = false;
    auto current = live_raytracing_pipelines().lock(
        [&](cc::vector<cc::unique_ptr<live_raytracing_pipeline>>& all) -> cc::string
        {
            auto const& live = live_of(all, d);
            for (auto i = isize(0); i < generations.size(); ++i)
                needs_read = needs_read || generations[i] != (i < live.generations.size() ? live.generations[i] : 0);
            return live.frozen_moved;
        });
    if (!needs_read)
        return current;

    // A shader reloaded, so the source may state something new: described outside the lock, since it checks the whole file.
    auto const source = handles.empty() ? cc::optional<cc::string>() : (*handles[0])->read_source();
    auto const modules = handles.empty() ? module_library() : (*handles[0])->read_modules();
    auto const library = impl::sgl_library_of(modules.files());
    auto const described
        = source.has_value()
            ? sgl::describe({.source = source.value(), .source_name = (*handles[0])->virtual_path(), .library = library})
            : cc::result<sgl::module_description, cc::string>(cc::error(cc::string("the source is gone")));
    auto const* found = static_cast<sgl::described_raytracing_pipeline const*>(nullptr);
    if (described.has_value())
        for (auto const& p : described.value().raytracing_pipelines)
            if (p.name == d.name)
                found = &p;

    auto next = cc::string();
    if (found == nullptr)
        next = cc::format("pipeline: the reloaded source does not state it: {}\n",
                          described.has_value() ? cc::string("no ray-tracing pipeline of that name") : described.error());
    else
        next = impl::frozen_moved(d.frozen, found->frozen);
    if (!next.empty() && next != current)
        CC_LOG_WARNING("{}'s ray-tracing pipeline {} keeps what it was last built with: the reloaded source moves "
                       "what the host was built against\n{}",
                       d.file, d.name, next);

    live_raytracing_pipelines().lock(
        [&](cc::vector<cc::unique_ptr<live_raytracing_pipeline>>& all)
        {
            auto& live = live_of(all, d);
            live.frozen_moved = next;
            live.generations = generations;
        });
    return next;
}

cc::shared_async<sg::raytracing_pipeline_description> slib::describe_raytracing_pipeline(
    sg::context* ctx,
    raytracing_pipeline_definition const* definition,
    raytracing_host_parts host)
{
    auto const& d = *definition;
    auto& host_hit_shaders = host.hit_groups;
    CC_ASSERTF(d.has_host_callables || host.callables.empty(),
               "{}'s {}: the module lists every callable, and the host handed it {} more", d.file, d.name,
               host.callables.size());
    CC_ASSERTF(d.raygen != nullptr && *d.raygen != nullptr,
               "{}'s {}: its package was never added to a shader library, so it has no shaders", d.file, d.name);
    CC_ASSERTF(d.has_host_hit_groups || host_hit_shaders.empty(),
               "{}'s {}: the pipeline lists every hit group, and the host handed it {} more hit shaders", d.file,
               d.name, host_hit_shaders.size());
    CC_ASSERTF(host_hit_shaders.size() % d.ray_count == 0,
               "{}'s {}: a hit group is {} hit shaders, one per ray type, and the host handed {}", d.file, d.name,
               d.ray_count, host_hit_shaders.size());

    auto desc = sg::raytracing_pipeline_description();
    desc.layout = d.acquire_layout(*ctx);
    desc.max_recursion_depth = d.max_recursion_depth;
    desc.max_payload_size = d.max_payload_size;
    desc.max_attribute_size = d.max_attribute_size;

    // The module's shaders first: awaiting them is what promotes a reload, which the frozen part is then read against.
    auto const raygen = co_await (*d.raygen)->acquire(*ctx, host.options);
    // metal runs traversal and closest hits otherwise than DXR does: a procedural group's any hit fused into its
    // intersection, and a function in every closest-hit slot
    auto const is_metal = raygen.format == sg::shader_format::msl || raygen.format == sg::shader_format::metal_lib;
    auto empty_closest_hit = cc::optional<sg::compiled_shader>();
    if (is_metal && d.empty_closest_hit != nullptr)
        empty_closest_hit = co_await (*d.empty_closest_hit)->acquire(*ctx, host.options);
    (void)desc.add_raygen_shader(raygen);
    for (auto const* const miss : d.misses)
    {
        CC_ASSERTF(miss != nullptr, "{}'s {}: every ray type has a miss", d.file, d.name);
        (void)desc.add_miss_shader(co_await (*miss)->acquire(*ctx, host.options));
    }
    for (auto const& group : d.hit_groups)
    {
        // metal traces a procedural group through its fused traversal alone, so its plain intersection is never built
        auto const is_fused = is_metal && group.intersection != nullptr && !group.metal_traversals.empty();
        auto intersection = cc::optional<sg::compiled_shader>();
        if (group.intersection != nullptr && !is_fused)
            intersection = co_await (*group.intersection)->acquire(*ctx, host.options);
        for (auto r = 0; r < d.ray_count; ++r)
        {
            auto shader = sg::hit_shader{.intersection = intersection};
            if (auto const* const h = group.closest_hits[r]; h != nullptr)
                shader.closest_hit = co_await (*h)->acquire(*ctx, host.options);
            else if (is_metal)
                shader.closest_hit = empty_closest_hit;
            if (is_fused)
            {
                CC_ASSERTF(r < group.metal_traversals.size() && group.metal_traversals[r] != nullptr,
                           "{}'s {}: a procedural group has a traversal per ray type on metal", d.file, d.name);
                shader.intersection = co_await (*group.metal_traversals[r])->acquire(*ctx, host.options);
            }
            else if (auto const* const h = group.any_hits[r]; h != nullptr)
                shader.any_hit = co_await (*h)->acquire(*ctx, host.options);
            (void)desc.add_hit_shader(cc::move(shader));
        }
    }
    for (auto const* const callable : d.callables)
        (void)desc.add_callable_shader(co_await (*callable)->acquire(*ctx, host.options));

    // Where the frozen part moved, the module's shaders this context last built with these values are what the host's
    // code fits.
    auto const moved = frozen_moved_of(d);
    auto const is_kept = live_raytracing_pipelines().lock(
        [&](cc::vector<cc::unique_ptr<live_raytracing_pipeline>>& all) -> bool
        { return live_of(all, d).kept.keep_or_restore(ctx, host.options, !moved.empty(), desc); });
    if (!is_kept)
        throw sg::pipeline_creation_exception(
            cc::string(d.name),
            cc::any_error(cc::format("{}'s {}: the source moved what the host was built against before the pipeline "
                                     "was ever described on this context, so no build is kept to fall back to\n{}",
                                     d.file, d.name, moved)));

    for (auto& shader : host_hit_shaders)
        (void)desc.add_hit_shader(cc::move(shader));
    for (auto& callable : host.callables)
        (void)desc.add_callable_shader(cc::move(callable));
    co_return desc;
}

sg::raytracing_shader_table_description slib::table_description(raytracing_pipeline_definition const& definition,
                                                                sg::raytracing_pipeline_handle pipeline,
                                                                raytracing_host_parts const& host)
{
    CC_ASSERTF(definition.has_host_callables || host.callables.empty(),
               "{}'s {}: the module lists every callable, and the host handed it {} more", definition.file,
               definition.name, host.callables.size());
    auto table
        = sg::raytracing_shader_table_description{.pipeline = cc::move(pipeline), .ray_count = definition.ray_count};
    (void)table.add_raygen_shader(sg::raygen_shader_handle(0));
    for (auto r = 0; r < definition.ray_count; ++r)
        (void)table.add_miss_shader(sg::miss_shader_handle(r));
    for (auto k = isize(0); k < definition.callables.size() + host.callables.size(); ++k)
        (void)table.add_callable_shader(sg::callable_shader_handle(u32(k)));
    return table;
}

sg::hit_row slib::add_hit_group_row(sg::raytracing_shader_table_description& table, int group)
{
    auto handles = cc::vector<sg::hit_shader_handle>();
    for (auto r = 0; r < table.ray_count; ++r)
        handles.push_back(sg::hit_shader_handle(group * table.ray_count + r));
    return table.add_hit_row(handles);
}

namespace
{
/// The context's first format `library` builds SGL into, as an asset's acquire would pick.
cc::optional<sg::shader_format> sgl_format_for(sg::context const& ctx, slib::shader_library const& library)
{
    for (auto const f : ctx.accepted_shader_formats())
        if (library.can_compile(slib::shader_language::sgl, f))
            return f;
    return {};
}
} // namespace

cc::shared_async<sg::compiled_shader> slib::compile_callable(sg::context* ctx,
                                                             shader_library const* library,
                                                             raytracing_pipeline_definition const* definition,
                                                             cc::string source,
                                                             cc::string entry,
                                                             cc::string label)
{
    auto const& d = *definition;
    auto const fail = [&](cc::string message)
    { return sg::pipeline_creation_exception(cc::string(entry), cc::any_error(cc::move(message))); };

    if (!d.has_host_callables)
        throw fail(cc::format("{}'s {} takes no callable of the host's", d.file, d.name));
    auto const modules = library->read_modules();
    auto const sgl_library = impl::sgl_library_of(modules.files());
    auto const described = sgl::describe({.source = source, .source_name = label, .library = sgl_library});
    if (described.has_error())
        throw fail(cc::format("{} does not compile, so it holds no callable:\n{}", label, described.error()));
    auto const* found = static_cast<sgl::described_entry_point const*>(nullptr);
    for (auto const& e : described.value().entry_points)
        if (e.name == entry && e.stage == sgl::check::stage::callable)
            found = &e;
    if (found == nullptr)
        throw fail(cc::format("{} declares no @callable {}", label, entry));
    // the table calls it with the parameter the module's callables take, so it must match whole
    if (found->payload != d.host_callable_parameter || found->payload_shape != d.host_callable_shape)
        throw fail(cc::format("@callable {} of {} takes {}@{}, and {}'s {} calls the host's with {}@{}", entry, label,
                              found->payload, found->payload_shape, d.file, d.name, d.host_callable_parameter,
                              d.host_callable_shape));

    auto const format = sgl_format_for(*ctx, *library);
    if (!format.has_value())
        throw fail("no registered compiler builds SGL into a format this context accepts");
    co_return co_await library->compile_source(source, sg::shader_stage::callable, entry, format.value(),
                                               {.language = shader_language::sgl, .label = label});
}

cc::shared_async<cc::vector<sg::hit_shader>> slib::compile_hit_group(sg::context* ctx,
                                                                     shader_library const* library,
                                                                     raytracing_pipeline_definition const* definition,
                                                                     cc::string source,
                                                                     cc::string group,
                                                                     cc::string label)
{
    auto const& d = *definition;
    auto const fail = [&](cc::string message)
    { return sg::pipeline_creation_exception(cc::string(group), cc::any_error(cc::move(message))); };

    if (!d.has_host_hit_groups)
        throw fail(cc::format("{}'s {} takes no hit group of the host's", d.file, d.name));
    auto const modules = library->read_modules();
    auto const sgl_library = impl::sgl_library_of(modules.files());
    auto const described = sgl::describe({.source = source, .source_name = label, .library = sgl_library});
    if (described.has_error())
        throw fail(cc::format("{} does not compile, so it holds no hit group:\n{}", label, described.error()));
    auto const& m = described.value();
    auto const* found = static_cast<sgl::described_hit_group const*>(nullptr);
    for (auto const& g : m.hit_groups)
        if (g.name == group)
            found = &g;
    if (found == nullptr)
        throw fail(cc::format("{} declares no hit_group {}", label, group));
    // what a trace hands the group's shaders is the payload, so the ray set must match whole: every ray type's name, and
    // its payload's name, size and shape
    auto const* set = static_cast<sgl::described_ray_set const*>(nullptr);
    for (auto const& s : m.ray_sets)
        if (s.name == found->ray_set)
            set = &s;
    auto is_same_set = found->ray_set == d.ray_set && set != nullptr && is_same(d.rays, set->rays)
                    && is_same(d.payloads, set->payloads) && is_same(d.payload_shapes, set->payload_shapes)
                    && set->payload_sizes.size() == d.payload_sizes.size();
    for (auto i = isize(0); is_same_set && i < d.payload_sizes.size(); ++i)
        is_same_set = set->payload_sizes[i] == d.payload_sizes[i];
    if (!is_same_set)
        throw fail(cc::format("hit_group {} of {} is for the ray set {}, and {}'s {} traces {}, whose ray types, "
                              "payloads and payload layouts it must state alike",
                              group, label, found->ray_set, d.file, d.name, d.ray_set));

    auto const format = sgl_format_for(*ctx, *library);
    if (!format.has_value())
        throw fail("no registered compiler builds SGL into a format this context accepts");

    auto const options = compile_source_options{.language = shader_language::sgl, .label = label};
    auto const is_metal = format.value() == sg::shader_format::msl || format.value() == sg::shader_format::metal_lib;
    auto empty_closest_hit = cc::optional<sg::compiled_shader>();
    if (is_metal)
        empty_closest_hit = co_await library->compile_source(source, sg::shader_stage::closest_hit,
                                                             "sgl_empty_closest_hit", format.value(), options);
    // metal traces a procedural group through its fused traversal alone, so its plain intersection is never built
    auto const is_fused = is_metal && !found->intersection.empty();
    auto intersection = cc::optional<sg::compiled_shader>();
    if (!found->intersection.empty() && !is_fused)
        intersection = co_await library->compile_source(source, sg::shader_stage::intersection, found->intersection,
                                                        format.value(), options);
    auto result = cc::vector<sg::hit_shader>();
    for (auto r = isize(0); r < d.ray_count; ++r)
    {
        auto shader = sg::hit_shader{.intersection = intersection};
        if (!found->closest_hits[r].empty())
            shader.closest_hit = co_await library->compile_source(source, sg::shader_stage::closest_hit,
                                                                  found->closest_hits[r], format.value(), options);
        else if (is_metal)
            shader.closest_hit = empty_closest_hit;
        // metal runs a procedural group's intersection and any hit as one traversal function
        if (is_fused)
            shader.intersection = co_await library->compile_source(source, sg::shader_stage::intersection,
                                                                   found->traversals[r], format.value(), options);
        else if (!found->any_hits[r].empty())
            shader.any_hit = co_await library->compile_source(source, sg::shader_stage::any_hit, found->any_hits[r],
                                                              format.value(), options);
        result.push_back(cc::move(shader));
    }
    co_return result;
}
