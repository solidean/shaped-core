#include <clean-core/common/assert.hh>
#include <clean-core/container/set.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <shaped-graphics/reload_generation.hh>
#include <shaped-shader-library/filesystem/embedded_filesystem.hh>
#include <shaped-shader-library/filesystem/impl/path.hh>
#include <shaped-shader-library/filesystem/real_filesystem.hh>
#include <shaped-shader-library/shader_asset.hh>
#include <shaped-shader-library/shader_library.hh>

namespace
{
// The generated package symbols are process-wide globals, so two libraries would fight over who owns
// the assets they point at. One at a time, enforced rather than documented.
bool g_library_alive = false;

sg::async_compiled_shader make_failed_shader(cc::string message)
{
    return cc::make_async_from_error<sg::compiled_shader>(cc::async_error::make_error(cc::any_error(cc::move(message))));
}
} // namespace

slib::shader_library::shader_library() : _alive(this, [](shader_library*) {}) // tracks liveness, owns nothing
{
    CC_ASSERT(!g_library_alive, "only one slib::shader_library may exist at a time — the generated package "
                                "symbols they write into are process-wide globals");
    g_library_alive = true;
}

slib::shader_library::~shader_library()
{
    // Stop the watcher before anything it reads goes away — it holds a back-reference to us.
    if (_watcher != nullptr)
    {
        _watcher_stopping->store(true); // cuts a sleeping poll loop short instead of waiting it out
        _watcher->shutdown();           // drops the watches, so no filesystem can wake it after this

        _wake->disarm(); // nothing may reach the actor we are about to destroy
        _watcher = nullptr;
        _wake = nullptr;
    }

    // Drop the token: any asset still reachable through a generated global now reports that its library
    // is gone, rather than dereferencing one that is half torn down.
    _alive.reset();
    g_library_alive = false;
}

void slib::shader_library::start_hot_reload(reload_config config)
{
    CC_ASSERT(_watcher == nullptr, "hot reload is already running");

    // start() forces unthreaded where the platform has no threads, so decide it here rather than let the
    // watcher believe it has a thread and sleep on whoever pumps it.
    bool const threaded = CC_HAS_THREADS && !config.unthreaded;

    _watcher_stopping = std::make_shared<cc::atomic<bool>>(false);
    _wake = std::make_shared<impl::reload_wake>();
    _watcher = cc::make_threaded_actor<impl::reload_watcher>(*this, config.interval_ms, threaded, config.force_polling,
                                                             _watcher_stopping, _wake);

    // Between the two: the watcher's constructor scanned with no actor to wake yet, and arming asks for the
    // one scan that pass could not. Nothing is running until start(), so there is no gap to race.
    _wake->arm(_watcher.get());
    _watcher->start(threaded ? cc::threaded_actor_mode::threaded_if_possible : cc::threaded_actor_mode::unthreaded);
}

void slib::shader_library::poll_hot_reload()
{
    if (_watcher != nullptr)
        (void)_watcher->process_messages_if_unthreaded(); // no-op when the watcher has its own thread
}

void slib::shader_library::add_compiler(std::unique_ptr<shader_compiler> compiler)
{
    CC_ASSERT(compiler != nullptr, "cannot add a null compiler");

    // A later compiler for the same edge replaces the earlier one.
    auto const language = compiler->source_language();
    auto const format = compiler->target_format();
    for (auto& existing : _compilers)
    {
        if (existing->source_language() == language && existing->target_format() == format)
        {
            existing = cc::move(compiler);
            return;
        }
    }
    _compilers.push_back(cc::move(compiler));
}

slib::shader_compiler const* slib::shader_library::find_compiler(shader_language language, sg::shader_format format) const
{
    for (auto const& compiler : _compilers)
        if (compiler->source_language() == language && compiler->target_format() == format)
            return compiler.get();
    return nullptr;
}

bool slib::shader_library::can_compile(shader_language language, sg::shader_format format) const
{
    return find_compiler(language, format) != nullptr;
}

cc::vector<sg::shader_format> slib::shader_library::supported_formats(shader_language language) const
{
    cc::vector<sg::shader_format> formats;
    for (auto const& compiler : _compilers)
        if (compiler->source_language() == language)
            formats.push_back(compiler->target_format());
    return formats;
}

void slib::shader_library::mount(cc::string_view virtual_dir, filesystem_handle fs)
{
    _mounts.mount(virtual_dir, cc::move(fs));
}

void slib::shader_library::add_package(shader_package const& package)
{
    // Embedded first, then the real source dir over it. real_filesystem over a missing directory simply
    // finds nothing, so a shipped build lands on the embedded copy without asking which mode it is in.
    add_package(package, nullptr);
}

void slib::shader_library::add_package(shader_package const& package, filesystem_handle fs)
{
    CC_ASSERT(!package.name.empty(), "a shader package must be named");
    CC_ASSERT(_watcher == nullptr, "add every package before start_hot_reload — the watcher walks the asset "
                                   "list from its own thread, so it must not grow underneath it");

    for (auto const& existing : _packages)
        CC_ASSERT(existing.name != package.name, "this shader package was already added");

    _packages.push_back(package_entry{.name = cc::string::create_copy_of(package.name), .language = package.language});

    if (fs != nullptr)
    {
        _mounts.mount(package.name, cc::move(fs));
    }
    else
    {
        _mounts.mount(package.name, std::make_shared<embedded_filesystem>(package.embedded_files));
        if (!package.source_dir.empty())
            _mounts.mount(package.name,
                          std::make_shared<real_filesystem>(cc::string::create_copy_of(package.source_dir)));
    }

    for (auto const& definition : package.definitions)
    {
        CC_ASSERT(definition.asset != nullptr, "a shader definition must name the global to fill in");

        auto virtual_path = impl::join_path(package.name, definition.path);
        CC_ASSERT(virtual_path.has_value(), "a shader path must not escape its package");

        auto asset = std::make_shared<shader_asset>(_alive, cc::move(virtual_path.value()), definition.stage,
                                                    cc::string::create_copy_of(definition.entry_point));
        *definition.asset = asset;
        _assets.push_back(cc::move(asset));
    }
}

slib::shader_language slib::shader_library::language_of(cc::string_view virtual_path) const
{
    return package_of(virtual_path).language;
}

slib::shader_library::package_entry const& slib::shader_library::package_of(cc::string_view virtual_path) const
{
    for (auto const& package : _packages)
        if (impl::is_path_under(virtual_path, package.name))
            return package;

    CC_UNREACHABLE("every asset's path lies under the package that registered it");
}

cc::u64 slib::shader_library::generation() const
{
    return sg::reload_generation();
}

cc::u64 slib::current_reload_generation()
{
    return sg::reload_generation();
}

void slib::shader_library::note_reload()
{
    sg::signal_reload();
}

void slib::shader_library::note_dependencies_changed()
{
    if (_wake != nullptr)
        _wake->fire();
}

slib::shader_library::compile_outcome slib::shader_library::compile_shader(cc::string_view virtual_path,
                                                                           sg::shader_stage stage,
                                                                           cc::string_view entry_point,
                                                                           sg::shader_format format) const
{
    compile_outcome outcome;
    outcome.dependencies.push_back(cc::string::create_copy_of(virtual_path));

    auto const& package = package_of(virtual_path);
    auto const* const compiler = find_compiler(package.language, format);
    if (compiler == nullptr)
    {
        outcome.shader
            = make_failed_shader(cc::format("no compiler registered to build '{}' into this format", virtual_path));
        return outcome;
    }

    auto source = _mounts.read_text(virtual_path);
    if (!source.has_value())
    {
        outcome.shader = make_failed_shader(cc::format("shader source not found: '{}'", virtual_path));
        return outcome;
    }

    auto const source_dir = impl::parent_path(virtual_path);

    // Every path the resolver hands back becomes a dependency, so an edit to any include reloads the
    // shaders that pulled it in. Resolving the same file twice yields empty text (pragma-once semantics)
    // rather than a duplicate expansion.
    cc::set<cc::string> seen;
    seen.insert(cc::string::create_copy_of(virtual_path));

    // Where an `#include "..."` is looked for, most specific first: next to the including file, then at
    // the package's own root, then at the mount root — which is what reaches a shared library mounted
    // outside any package (`#include "common/brdf.hlsli"`).
    auto const search_roots = {source_dir, cc::string_view(package.name), cc::string_view()};

    // Non-const: cc::function_ref binds a mutable lvalue.
    auto resolve = [&](cc::string_view include_path) -> cc::optional<cc::string>
    {
        cc::optional<cc::string> resolved;
        for (auto const& root : search_roots)
        {
            auto candidate = impl::join_path(root, include_path);
            if (candidate.has_value() && _mounts.exists(candidate.value()))
            {
                resolved = cc::move(candidate);
                break;
            }
        }
        if (!resolved.has_value())
            return cc::nullopt;

        auto text = _mounts.read_text(resolved.value());
        if (!text.has_value())
            return cc::nullopt;

        if (!seen.insert(resolved.value()))
            return cc::string(); // already included

        outcome.dependencies.push_back(cc::move(resolved.value()));
        return text;
    };

    shader_source_description desc{.source = cc::move(source.value()),
                                   .entry_point = cc::string::create_copy_of(entry_point),
                                   .stage = stage};

    auto preprocessed = compiler->preprocess(desc, resolve);
    if (preprocessed.has_error())
    {
        outcome.shader = make_failed_shader(
            cc::format("preprocessing '{}' failed: {}", virtual_path, preprocessed.error().to_string()));
        return outcome;
    }

    desc.source = cc::move(preprocessed.value());
    outcome.shader = compiler->compile(desc);
    return outcome;
}
