#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/assert.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh> // acquire(ctx) asks it which formats it accepts
#include <shaped-shader-library/shader_asset.hh>
#include <shaped-shader-library/shader_library.hh>

using namespace cc::primitive_defines;

namespace
{
sg::async_compiled_shader make_failed_shader(cc::string message)
{
    return cc::make_async_from_error<sg::compiled_shader>(cc::async_error::make_error(cc::any_error(cc::move(message))));
}

bool is_same(cc::span<slib::shader_option const> a, cc::span<slib::shader_option const> b)
{
    if (a.size() != b.size())
        return false;
    for (auto i = isize(0); i < a.size(); ++i)
        if (!(a[i] == b[i]))
            return false;
    return true;
}
} // namespace

slib::shader_asset::shader_asset(std::weak_ptr<shader_library> library,
                                 cc::string virtual_path,
                                 sg::shader_stage stage,
                                 cc::string entry_point,
                                 cc::vector<cc::string> options)
  : _library(cc::move(library)),
    _virtual_path(cc::move(virtual_path)),
    _stage(stage),
    _entry_point(cc::move(entry_point)),
    _options(cc::move(options))
{
}

void slib::shader_asset::promote_pending(shader_library& library, state& s, format_entry& entry) const
{
    if (entry.pending == nullptr)
        return;

    // Only promote once the compile has finished, so a consumer never waits on the compiler here.
    if (!entry.pending->is_ready())
        return;

    if (entry.pending->has_value())
    {
        entry.current = entry.pending;
        s.last_error = cc::nullopt;
        ++s.generation;
        library.note_reload();
    }
    else
    {
        auto const* const error = entry.pending->try_error();
        if (error == nullptr)
            s.last_error = cc::string("shader compilation failed");
        else if (error->is_cancelled())
            s.last_error = cc::string("shader compilation was cancelled");
        else
            s.last_error = error->underlying().to_string();
    }

    entry.pending = nullptr;
}

sg::async_compiled_shader slib::shader_asset::acquire(sg::shader_format format, cc::span<shader_option const> options) const
{
    auto const library = _library.lock();
    if (library == nullptr)
        return make_failed_shader(cc::format("the shader library that owns '{}' is gone", _virtual_path));

    // The key: only the options the entry point reaches, by name, so an order or an option of another stage splits nothing.
    auto key = cc::vector<shader_option>();
    for (auto const& name : _options)
        for (auto const& o : options)
            if (o.name == name)
            {
                key.push_back(o);
                break;
            }
    cc::sort(key, [](shader_option const& a, shader_option const& b) { return a.name < b.name; });

    bool recorded_dependencies = false;
    auto shader = _state.lock(
        [&](state& s) -> sg::async_compiled_shader
        {
            format_entry* entry = nullptr;
            for (auto& e : s.formats)
                if (e.format == format && is_same(e.options, key))
                    entry = &e;

            if (entry == nullptr)
            {
                s.formats.push_back(format_entry{.format = format, .options = key});
                entry = &s.formats.back();
            }

            promote_pending(*library, s, *entry);

            if (entry->current == nullptr) // first acquire for this format and option set
            {
                auto outcome = library->compile_shader(_virtual_path, _stage, _entry_point, format, key);
                entry->current = cc::move(outcome.shader);
                entry->dependencies = cc::move(outcome.dependencies);
                recorded_dependencies = true;
            }

            return entry->current;
        });

    // A first compile is what resolves the includes, so the dependency set only becomes real here, on a consumer's thread.
    // Off the lock: what this wakes turns straight around and reads us back.
    if (recorded_dependencies)
        library->note_dependencies_changed();

    return shader;
}

bool slib::shader_asset::can_build(sg::shader_format format) const
{
    auto const library = _library.lock();
    if (library == nullptr)
        return false;

    return library->can_compile(library->language_of(_virtual_path), format);
}

bool slib::shader_asset::can_acquire(sg::context const& ctx) const
{
    // The same loop acquire(ctx) runs, which is what makes this a prediction of it rather than a second opinion.
    for (auto const format : ctx.accepted_shader_formats())
        if (this->can_build(format))
            return true;

    return false;
}

sg::async_compiled_shader slib::shader_asset::acquire(sg::context const& ctx, cc::span<shader_option const> options) const
{
    // The context lists what it takes in preference order, so the first one we can actually build wins.
    for (auto const format : ctx.accepted_shader_formats())
        if (this->can_build(format))
            return acquire(format, options);

    if (_library.expired())
        return make_failed_shader(cc::format("the shader library that owns '{}' is gone", _virtual_path));

    return make_failed_shader(
        cc::format("no compiler registered to build '{}' into a format this context accepts", _virtual_path));
}

u64 slib::shader_asset::generation() const
{
    return _state.lock([](state const& s) { return s.generation; });
}

cc::optional<cc::string> slib::shader_asset::read_source() const
{
    auto const library = _library.lock();
    if (library == nullptr)
        return cc::nullopt;
    return library->filesystem().read_text(_virtual_path);
}

cc::optional<cc::string> slib::shader_asset::last_error() const
{
    return _state.lock([](state const& s) { return s.last_error; });
}

cc::vector<cc::string> slib::shader_asset::dependencies() const
{
    return _state.lock(
        [](state const& s)
        {
            cc::vector<cc::string> all;
            for (auto const& entry : s.formats)
                for (auto const& dependency : entry.dependencies)
                {
                    // A file included by several formats' builds only needs watching once.
                    bool known = false;
                    for (auto const& existing : all)
                        if (existing == dependency)
                            known = true;
                    if (!known)
                        all.push_back(dependency);
                }
            return all;
        });
}

void slib::shader_asset::stage_reload()
{
    auto const library = _library.lock();
    if (library == nullptr)
        return;

    // Only pairs someone has actually asked for: staging a compile for one nobody acquired would
    // burn the compiler on a shader that is never used.
    struct acquired
    {
        sg::shader_format format;
        cc::vector<shader_option> options;
    };
    cc::small_vector<acquired, 2> pairs;
    _state.lock(
        [&](state const& s)
        {
            for (auto const& entry : s.formats)
                if (entry.current != nullptr)
                    pairs.push_back({.format = entry.format, .options = entry.options});
        });

    for (auto const& [format, options] : pairs)
    {
        // Read, preprocess and compile off the lock, so a consumer's acquire only ever waits for the swap below.
        auto outcome = library->compile_shader(_virtual_path, _stage, _entry_point, format, options);

        // Drive the compile here, on the watcher's own thread.
        // A cc::async node is cold until a scheduler runs it, and nothing else ever looks at this one — a consumer only sees it after promotion, and promotion needs it ready.
        // With a compute async scheduler installed this returns nullopt because the pool already owns the node and finishes it; without one, this call is what runs the compile.
        (void)cc::try_async_blocking_get(outcome.shader);

        _state.lock(
            [&](state& s)
            {
                for (auto& entry : s.formats)
                {
                    if (entry.format != format || !is_same(entry.options, options))
                        continue;
                    entry.pending = cc::move(outcome.shader);
                    entry.dependencies = cc::move(outcome.dependencies);
                }
            });
    }
}

sg::async_compute_pipeline slib::acquire_compute_pipeline(sg::context* ctx,
                                                          shader_asset_handle asset,
                                                          sg::pipeline_layout_handle layout,
                                                          cc::vector<shader_option> options)
{
    auto const shader = co_await asset->acquire(*ctx, options);
    co_return co_await ctx->cached.acquire_compute_pipeline({.shader = shader, .layout = layout});
}
