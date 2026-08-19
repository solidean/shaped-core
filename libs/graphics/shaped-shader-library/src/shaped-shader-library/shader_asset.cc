#include <clean-core/common/assert.hh>
#include <clean-core/string/format.hh>
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
} // namespace

slib::shader_asset::shader_asset(std::weak_ptr<shader_library> library,
                                 cc::string virtual_path,
                                 sg::shader_stage stage,
                                 cc::string entry_point)
  : _library(cc::move(library)), _virtual_path(cc::move(virtual_path)), _stage(stage), _entry_point(cc::move(entry_point))
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

sg::async_compiled_shader slib::shader_asset::acquire(sg::shader_format format) const
{
    auto const library = _library.lock();
    if (library == nullptr)
        return make_failed_shader(cc::format("the shader library that owns '{}' is gone", _virtual_path));

    bool recorded_dependencies = false;
    auto shader = _state.lock(
        [&](state& s) -> sg::async_compiled_shader
        {
            format_entry* entry = nullptr;
            for (auto& e : s.formats)
                if (e.format == format)
                    entry = &e;

            if (entry == nullptr)
            {
                s.formats.push_back(format_entry{.format = format});
                entry = &s.formats.back();
            }

            promote_pending(*library, s, *entry);

            if (entry->current == nullptr) // first acquire for this format
            {
                auto outcome = library->compile_shader(_virtual_path, _stage, _entry_point, format);
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

sg::async_compiled_shader slib::shader_asset::acquire(sg::context const& ctx) const
{
    auto const library = _library.lock();
    if (library == nullptr)
        return make_failed_shader(cc::format("the shader library that owns '{}' is gone", _virtual_path));

    auto const language = library->language_of(_virtual_path);

    // The context lists what it takes in preference order, so the first one we can actually build wins.
    for (auto const format : ctx.accepted_shader_formats())
        if (library->can_compile(language, format))
            return acquire(format);

    return make_failed_shader(
        cc::format("no compiler registered to build '{}' into a format this context accepts", _virtual_path));
}

u64 slib::shader_asset::generation() const
{
    return _state.lock([](state const& s) { return s.generation; });
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

    // Only formats someone has actually asked for: staging a compile for a format nobody acquired would
    // burn the compiler on a shader that is never used.
    cc::small_vector<sg::shader_format, 2> formats;
    _state.lock(
        [&](state const& s)
        {
            for (auto const& entry : s.formats)
                if (entry.current != nullptr)
                    formats.push_back(entry.format);
        });

    for (auto const format : formats)
    {
        // Read, preprocess and compile off the lock, so a consumer's acquire only ever waits for the swap below.
        auto outcome = library->compile_shader(_virtual_path, _stage, _entry_point, format);

        // Drive the compile here, on the watcher's own thread.
        // A cc::async node is cold until a scheduler runs it, and nothing else ever looks at this one — a consumer only sees it after promotion, and promotion needs it ready.
        // With a default async pool installed this returns nullopt because the pool already owns the node and finishes it; without one, this call is what runs the compile.
        (void)cc::try_async_blocking_get(outcome.shader);

        _state.lock(
            [&](state& s)
            {
                for (auto& entry : s.formats)
                {
                    if (entry.format != format)
                        continue;
                    entry.pending = cc::move(outcome.shader);
                    entry.dependencies = cc::move(outcome.dependencies);
                }
            });
    }
}
