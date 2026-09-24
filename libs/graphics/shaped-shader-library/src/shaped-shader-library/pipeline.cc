#include "pipeline.hh"

#include <clean-core/common/assertf.hh>
#include <clean-core/common/log.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics-language/driver/describe.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/exceptions.hh>
#include <shaped-shader-library/impl/pipeline_fields.hh>
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

/// One declared pipeline's reload state, kept for the life of the process like the definition it belongs to.
struct live_pipeline
{
    slib::pipeline_definition const* definition = nullptr;
    /// The stages' generations the configuration was last read at; the build's settings are generation 0's.
    u64 vertex_generation = 0;
    u64 pixel_generation = 0;
    slib::pipeline_configuration configuration;

    /// The stages last described on a context while the frozen part still matched the build.
    /// A compiled shader carries its own bytecode, so keeping one keeps what the host's code fits.
    struct kept
    {
        sg::context const* ctx = nullptr;
        sg::compiled_shader vertex;
        cc::optional<sg::compiled_shader> pixel;
    };
    cc::vector<kept> kept_stages;
};

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

slib::pipeline_configuration slib::configuration_of(pipeline_definition const& d)
{
    auto const vertex_generation = d.vertex != nullptr && *d.vertex != nullptr ? (*d.vertex)->generation() : 0;
    auto const pixel_generation = d.pixel != nullptr && *d.pixel != nullptr ? (*d.pixel)->generation() : 0;

    // What is known now, and whether a reload moved a stage since it was read.
    auto needs_read = false;
    auto current = live_pipelines().lock(
        [&](cc::vector<cc::unique_ptr<live_pipeline>>& all) -> pipeline_configuration
        {
            auto const& live = live_of(all, d);
            needs_read = live.vertex_generation != vertex_generation || live.pixel_generation != pixel_generation;
            return live.configuration;
        });
    if (!needs_read)
        return current;

    // A stage reloaded, so the source may say something new: described outside the lock, since it checks the whole file.
    auto next = current;
    auto const source = (*d.vertex)->read_source();
    auto const described
        = source.has_value()
            ? sgl::describe({.source = source.value(), .source_name = d.file})
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
            live.vertex_generation = vertex_generation;
            live.pixel_generation = pixel_generation;
        });
    return next;
}

cc::shared_async<sg::raster_pipeline_description> slib::describe_raster_pipeline(sg::context* ctx,
                                                                                 pipeline_definition const* definition,
                                                                                 cc::vector<open_part> open,
                                                                                 sg::raster_pipeline_customize customize,
                                                                                 bool latest)
{
    auto const& d = *definition;
    CC_ASSERTF(d.vertex != nullptr && *d.vertex != nullptr,
               "{}'s {}: its package was never added to a shader library, so it has no shaders", d.file, d.name);
    // What the host states is the host's to get right: a format it left `undefined`, or a sample count of 0.
    for (auto const& part : open)
        if (part.path.ends_with("sample_count"))
            CC_ASSERTF(part.value >= 1 && part.value <= 64 && (part.value & (part.value - 1)) == 0,
                       "{}'s {}: the sample count is stated as {}, and it is a power of two from 1 to 64", d.file,
                       d.name, part.value);
        else
            CC_ASSERTF(part.value > 0, "{}'s {}: {} is stated as no format", d.file, d.name, part.path);

    // The stages first: awaiting them is what promotes a reload, which the configuration is then read against.
    auto desc = raster_pipeline_description();
    desc.layout = d.acquire_layout(*ctx);
    desc.vertex_shader = co_await (*d.vertex)->acquire(*ctx);
    if (d.pixel != nullptr)
        desc.fragment_shader = co_await (*d.pixel)->acquire(*ctx);
    desc.vertex_input = d.vertex_input();
    desc.target_set = cc::string(d.target_set);
    for (auto i = isize(0); i < d.targets.size(); ++i)
        desc.color_targets.push_back({});

    auto const configuration = configuration_of(d);

    // Where the frozen part moved, the stages this context last built with are what the host's code still fits.
    if (!latest)
    {
        auto const is_moved = !configuration.frozen_moved.empty();
        auto const is_kept = live_pipelines().lock(
            [&](cc::vector<cc::unique_ptr<live_pipeline>>& all) -> bool
            {
                auto& live = live_of(all, d);
                for (auto& k : live.kept_stages)
                    if (k.ctx == ctx)
                    {
                        if (is_moved)
                        {
                            desc.vertex_shader = k.vertex;
                            desc.fragment_shader = k.pixel;
                        }
                        else
                        {
                            k.vertex = desc.vertex_shader;
                            k.pixel = desc.fragment_shader;
                        }
                        return true;
                    }
                if (is_moved)
                    return false;
                live.kept_stages.push_back({.ctx = ctx, .vertex = desc.vertex_shader, .pixel = desc.fragment_shader});
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

    if (customize)
        customize(desc);
    co_return desc;
}
