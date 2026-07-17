#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/async.hh> // sg::async_compiled_shader is a cc::shared_async
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/threaded_actor.hh>
#include <shaped-graphics/compiled_shader.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-shader-library/compiler/shader_compiler.hh>
#include <shaped-shader-library/filesystem/mount_table.hh>
#include <shaped-shader-library/fwd.hh>
#include <shaped-shader-library/impl/reload_watcher.hh>
#include <shaped-shader-library/shader_package.hh>

#include <memory>

namespace slib
{
/// How the reload watcher runs. See shader_library::start_hot_reload.
struct reload_config
{
    /// How long between rescans while polling. Ignored where the filesystem can notify: there is no
    /// interval then, the change itself is the trigger.
    double interval_ms = 200;

    /// Run no thread; nothing is scanned until you call poll_hot_reload(). Forced on where the platform
    /// has no threads. Tests use it to make a reload deterministic — write a file, pump, acquire, done.
    bool unthreaded = false;

    /// Rescan on interval_ms even where the filesystem could notify. For exercising the fallback path
    /// deliberately; a real app has no reason to ask for it.
    bool force_polling = false;
};

/// Owns the shader packages a program uses: their filesystems, the compilers that build them, and the
/// assets the generated symbols point at.
///
/// Not a singleton and not something call sites touch — build one at startup, register what you need,
/// and forget it. Everything after that goes through the generated package symbols:
///
///   slib::shader_library lib;
///   lib.add_compiler(slib::create_dxc_compiler().value());
///   lib.add_package(sg::test::shaders::package());
///   ...
///   auto cs = sg::test::shaders::invert.compute.main->acquire(ctx);
///
/// The generated symbols are process-wide globals, so at most one library may exist at a time and a
/// package may only be added once — both assert. Those globals can outlive any library, so an asset
/// only weakly references the one that made it: acquiring through a stale global reports an error
/// rather than reaching a dead library.
class shader_library
{
public:
    shader_library();
    ~shader_library();

    shader_library(shader_library const&) = delete;
    shader_library& operator=(shader_library const&) = delete;

    /// Registers a compilation edge (language -> format). Adding a second compiler for the same edge
    /// replaces the first.
    void add_compiler(std::unique_ptr<shader_compiler> compiler);

    /// Mounts `package` at its own name and writes its generated asset handles back.
    ///
    /// Mounts the embedded files, then the package's source_dir over them when that directory exists.
    /// That single line is the whole dev-vs-ship split: a dev build reads and watches the source tree,
    /// a shipped build reads what the generator baked in, and neither needs a mode flag.
    void add_package(shader_package const& package);

    /// Mounts `fs` for the package instead of the default embedded+source pair. Tests pass a
    /// memory_filesystem, which is what makes a reload a write() rather than a sleep.
    void add_package(shader_package const& package, filesystem_handle fs);

    /// Mounts a filesystem at a virtual path — for shader sources that belong to no single package,
    /// like a shared include library.
    void mount(cc::string_view virtual_dir, filesystem_handle fs);

    /// Starts watching every file the assets are built from, staging a recompile whenever one changes.
    /// Call after every add_package: the watcher walks the asset list, which registration grows.
    ///
    /// The filesystem notifies where it can, and the watcher is otherwise asleep — no interval, no periodic
    /// wakeup. Where it cannot (a platform with no watch backend, SC_THREADS=OFF, force_polling), it falls
    /// back to rescanning every `interval_ms`.
    ///
    /// Recompiles run on the watcher, not on whoever acquires — so a reload costs a consumer nothing but
    /// the lock around a pointer swap, and it keeps the last good shader until the new one is ready.
    void start_hot_reload(reload_config config = {});

    /// Drives the watcher on the calling thread. A no-op unless hot reload was started unthreaded, so it is
    /// safe to call unconditionally every frame.
    ///
    /// Unthreaded, this *is* the recompile: it blocks for as long as the changed shaders take to build.
    /// That is the trade for having no thread, and it is what makes a reload deterministic. Where the
    /// filesystem notifies, a poll with nothing pending does no work at all.
    void poll_hot_reload();

    /// Whether the watcher is running.
    [[nodiscard]] bool is_hot_reloading() const { return _watcher != nullptr; }

    /// Whether a registered compiler connects `language` to `format`.
    [[nodiscard]] bool can_compile(shader_language language, sg::shader_format format) const;

    /// Every format some registered compiler can produce from `language`.
    [[nodiscard]] cc::vector<sg::shader_format> supported_formats(shader_language language) const;

    /// The assets registered so far, in package-declaration order. Fixed once hot reload has started —
    /// add_package asserts after that, so the watcher may hold on to this.
    [[nodiscard]] cc::span<shader_asset_handle const> assets() const { return _assets; }

    /// Everything mounted: the packages' sources plus any shared mounts. What shader paths resolve against.
    [[nodiscard]] mount_table const& filesystem() const { return _mounts; }

    /// Bumped whenever any asset's shader is replaced by a reload — the coarse "something changed"
    /// check, for a consumer that would rebuild everything anyway. Prefer an asset's own generation()
    /// when you can: this one moves for shaders you never use.
    [[nodiscard]] u64 generation() const;

    // internal — the compile path an asset drives
public:
    /// What a compile produced: the async shader, plus every virtual path it was built from.
    struct compile_outcome
    {
        sg::async_compiled_shader shader;
        cc::vector<cc::string> dependencies; ///< the source itself, then each resolved include
    };

    /// Reads, preprocesses and compiles one shader for `format`. A missing file, a missing compiler, or
    /// a compile error all come back as an async error on `shader` — never a throw.
    [[nodiscard]] compile_outcome compile_shader(cc::string_view virtual_path,
                                                 sg::shader_stage stage,
                                                 cc::string_view entry_point,
                                                 sg::shader_format format) const;

    /// The language of the package that owns `virtual_path`.
    [[nodiscard]] shader_language language_of(cc::string_view virtual_path) const;

    void note_reload();

    /// Tells the watcher an asset's dependency set moved, so it can seed the new paths and re-arm its
    /// watches. A first acquire records what a shader is built from on a *consumer* thread — which the
    /// watcher, parked on its mailbox, would otherwise never hear about. Cheap and safe to over-call.
    void note_dependencies_changed();

private:
    /// One registered package: its mount point and the language its sources are in.
    struct package_entry
    {
        cc::string name;
        shader_language language = shader_language::hlsl;
    };

    [[nodiscard]] shader_compiler const* find_compiler(shader_language language, sg::shader_format format) const;

    /// The package that owns `virtual_path`. Every asset path lies under exactly one.
    [[nodiscard]] package_entry const& package_of(cc::string_view virtual_path) const;

    /// Alive-token handed to every asset as a weak reference, cleared first thing on destruction. The
    /// generated globals are statics, so an asset can easily outlive its library; this is what turns
    /// "acquire through a stale global" into a reported error instead of a dangling back-reference.
    /// Aliasing with a no-op deleter — it owns nothing, it only tracks whether we are still here.
    std::shared_ptr<shader_library> _alive;

    mount_table _mounts;
    cc::vector<std::unique_ptr<shader_compiler>> _compilers;
    cc::vector<shader_asset_handle> _assets;

    cc::vector<package_entry> _packages;

    cc::atomic<u64> _generation{0};

    /// The reload watcher's actor, the flag that tells a sleeping poll loop to give up, and the wake a
    /// filesystem notification comes in through. Both are shared because the actor owns the impl and only
    /// hands it back once it has stopped.
    cc::unique_ptr<cc::threaded_actor<impl::check_now>> _watcher;
    std::shared_ptr<cc::atomic<bool>> _watcher_stopping;
    std::shared_ptr<impl::reload_wake> _wake;
};
} // namespace slib
