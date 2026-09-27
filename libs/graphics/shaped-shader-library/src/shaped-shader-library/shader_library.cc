#include <clean-core/common/assert.hh>
#include <clean-core/common/log.hh>
#include <clean-core/container/set.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/scope.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <clean-core/thread/thread_pump.hh>
#include <shaped-graphics/routine/reload_generation.hh>
#include <shaped-shader-library/binding/binding_groups.hh>
#include <shaped-shader-library/filesystem/embedded_filesystem.hh>
#include <shaped-shader-library/filesystem/impl/path.hh>
#include <shaped-shader-library/filesystem/real_filesystem.hh>
#include <shaped-shader-library/shader_asset.hh>
#include <shaped-shader-library/shader_library.hh>

using namespace cc::primitive_defines;

namespace slib
{
CC_REC_DEFINE_DOMAIN(g_rec_domain, "slib");
} // namespace slib

namespace
{
// The generated package symbols are process-wide globals, so two libraries would fight over who owns the assets they point at.
bool g_library_alive = false;

/// The name a compiler reflects `b` under: the identifier the text spells it with.
cc::string_view emitted_name_of(sg::binding const& b)
{
    return b.reflected_name.empty() ? cc::string_view(b.name) : cc::string_view(b.reflected_name);
}

/// Where `reflected`, a compiler's reading of a binding, disagrees with the `declared` one the text was written from.
/// Only what every compiler reads off the text is compared: DXC declares every image read-write, so an image's access
/// is compared as whether it writes, and slib's WGSL reader assumes a sampler's and a texture's filtering.
void compare_binding(sg::binding const& reflected, sg::binding const& declared, sg::shader_format format, cc::string& out)
{
    auto const name = cc::string_view(declared.name);
    auto const differs = [&](cc::string_view what, auto const& a, auto const& b)
    { out += cc::format("'{}' reflects {} {}, and SGL states {}\n", name, what, a, b); };

    if (reflected.type != declared.type)
        differs("kind", int(reflected.type), int(declared.type));
    if (reflected.index != declared.index)
        differs("index", reflected.index, declared.index);
    if (reflected.count != declared.count)
        differs("count", reflected.count, declared.count);
    if (reflected.group_index.has_value() && reflected.group_index != declared.group_index)
        differs("set", reflected.group_index.value(), declared.group_index.value_or(u32(-1)));
    if (reflected.space.has_value() && reflected.space != declared.space)
        differs("space", reflected.space.value(), declared.space.value_or(u32(-1)));
    if (reflected.is_writable() != declared.is_writable())
        differs("writable", reflected.is_writable(), declared.is_writable());
    auto const rows = [](isize size) { return (size + 15) / 16 * 16; };
    if (reflected.block_size.has_value() && declared.block_size.has_value()
        && rows(reflected.block_size.value()) != rows(declared.block_size.value()))
        differs("block size", reflected.block_size.value(), declared.block_size.value());
    if (reflected.texture_dimension.has_value() && reflected.texture_dimension != declared.texture_dimension)
        differs("dimension", int(reflected.texture_dimension.value()), int(declared.texture_dimension.value_or({})));
    if (reflected.image_format.has_value() && reflected.image_format != declared.image_format)
        differs("image format", int(reflected.image_format.value()), int(declared.image_format.value_or({})));
    if (format != sg::shader_format::wgsl)
    {
        if (reflected.sample_type.has_value() && reflected.sample_type != declared.sample_type)
            differs("sample type", int(reflected.sample_type.value()), int(declared.sample_type.value_or({})));
        if (reflected.sampler_type.has_value() && reflected.sampler_type != declared.sampler_type)
            differs("sampler type", int(reflected.sampler_type.value()), int(declared.sampler_type.value_or({})));
    }
}

/// What keeps `reflected`, the compiler's own reading of the text, from confirming what SGL `stated` of it; empty where it does.
/// A compiler reports every binding the text declares (WGSL) or only those the code keeps (DXC), so each reflected
/// binding is looked up among `declared`, and SGL's used ones are a subset of those by construction.
cc::string reflection_mismatch(sg::compiled_shader const& reflected,
                               sg::compiled_shader const& stated,
                               cc::span<sg::binding const> declared)
{
    auto out = cc::string();
    for (auto const& r : reflected.bindings)
    {
        sg::binding const* match = nullptr;
        for (auto const& d : declared)
            if (emitted_name_of(d) == r.name)
                match = &d;
        if (match == nullptr)
            out += cc::format("'{}' is reflected, and SGL declares no binding the text spells so\n", r.name);
        else
            compare_binding(r, *match, stated.format, out);
    }

    if (reflected.workgroup_size.has_value() && stated.workgroup_size.has_value())
    {
        auto const& a = reflected.workgroup_size.value();
        auto const& b = stated.workgroup_size.value();
        if (a.x != b.x || a.y != b.y || a.z != b.z)
            out += cc::format("the workgroup reflects as {}x{}x{}, and SGL states {}x{}x{}\n", a.x, a.y, a.z, b.x, b.y,
                              b.z);
    }
    if (reflected.color_output_count.has_value() && stated.color_output_count.has_value()
        && reflected.color_output_count != stated.color_output_count)
        out += cc::format("{} render targets reflect, and SGL states {}\n", reflected.color_output_count.value(),
                          stated.color_output_count.value());

    // A written slot cannot be dropped by a compiler, so it has to reflect as writable wherever it reflects at all.
    for (auto const& slot : stated.footprint.slots)
    {
        if (!slot.access.has(sg::access_flag::shader_write))
            continue;
        for (auto const& d : declared)
            if (d.name == slot.name)
                for (auto const& r : reflected.bindings)
                    if (r.name == emitted_name_of(d) && !r.is_writable())
                        out += cc::format("'{}' is written, and reflects as read-only\n", slot.name);
    }
    return out;
}

/// What keeps the compiler's layout of each block and buffer element from being the one SGL `stated`; empty where it is.
/// A block the compiler dropped is not compared, and neither is one whose fields it names in another way than SGL.
cc::string layout_mismatch(cc::span<slib::block_layout const> reflected, cc::span<slib::block_layout const> stated)
{
    auto out = cc::string();
    for (auto const& s : stated)
    {
        slib::block_layout const* r = nullptr;
        for (auto const& candidate : reflected)
            if (candidate.global == s.global)
                r = &candidate;
        if (r == nullptr)
            continue;
        if (s.stride > 0 && r->stride > 0 && s.stride != r->stride)
            out += cc::format("'{}' strides by {} bytes, and SGL states {}\n", s.global, r->stride, s.stride);
        for (auto const& f : s.fields)
            for (auto const& g : r->fields)
                if (g.name == f.name && g.offset != f.offset)
                    out += cc::format("'{}.{}' sits at byte {}, and SGL states {}\n", s.global, f.name, g.offset,
                                      f.offset);
    }
    return out;
}

/// SGL's `interface`, with the bytecode and the compiler of `compiled`, which is all a compile contributes to it.
/// `compiled`'s own reflection is compared against the interface on the way, and a difference is an SGL bug that is
/// logged, never used: SGL wrote the text, so it is what the text means.
sg::compiled_shader assembled(sg::compiled_shader compiled,
                              sg::compiled_shader interface,
                              cc::span<sg::binding const> declared,
                              cc::span<slib::block_layout const> layouts,
                              slib::shader_compiler const* compiler,
                              cc::string_view label)
{
    {
        CC_RECORD_SCOPE("slib.reflection_check");
        auto mismatch = reflection_mismatch(compiled, interface, declared);
        if (auto const reflected = compiler->reflect_layouts(compiled); reflected.has_value())
            mismatch += layout_mismatch(reflected.value(), layouts);
        if (!mismatch.empty())
            CC_LOG_ERROR("the compiler's reflection of '{}' ({}) disagrees with SGL, whose interface is used:\n{}",
                         label, interface.entry_point, mismatch);
    }
    interface.bytecode = cc::move(compiled.bytecode);
    interface.compiler = cc::move(compiled.compiler);
    interface.compiler.signature = cc::format("{} sgl", interface.compiler.signature);
    return interface;
}

struct sgl_interface
{
    sg::compiled_shader shader;
    cc::vector<sg::binding> declared;
    cc::vector<slib::block_layout> layouts;
    /// The edge that compiled it, whose reflection is compared; the library owns it for as long as it compiles.
    slib::shader_compiler const* compiler = nullptr;
    cc::string label;
};

sg::async_compiled_shader assembled_once_settled(sg::async_compiled_shader built, sgl_interface interface)
{
    auto shader = co_await built;
    co_return assembled(cc::move(shader), cc::move(interface.shader), interface.declared, interface.layouts,
                        interface.compiler, interface.label);
}

/// `built` as the shader SGL's interface describes, once it settles.
/// Assembled after the compile rather than inside it, so a compiler's cache holds only what the compiler reflected.
/// A compile that settled already stays settled, as a WGSL one does.
sg::async_compiled_shader with_interface(sg::async_compiled_shader built, sgl_interface interface)
{
    if (!built->has_value())
        return assembled_once_settled(cc::move(built), cc::move(interface));
    return cc::make_async_from_value(assembled(*built->try_value(), cc::move(interface.shader), interface.declared,
                                               interface.layouts, interface.compiler, interface.label));
}

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
    _watcher_poll_now = std::make_shared<cc::atomic<bool>>(false);
    _wake = std::make_shared<impl::reload_wake>();
    _watcher = cc::make_threaded_actor<impl::reload_watcher>(*this, config.interval_ms, threaded, config.force_polling,
                                                             _watcher_stopping, _watcher_poll_now, _wake);

    // Arm before start: the constructor's scan had no actor to wake, and nothing runs until start(), so there is no gap to race.
    _wake->arm(_watcher.get());
    _watcher->start(threaded ? cc::threaded_actor_mode::threaded_if_possible : cc::threaded_actor_mode::unthreaded);
}

void slib::shader_library::poll_hot_reload()
{
    // Deliberately NOT cc::thread_pump_all(): the watcher IS registered there, but a sweep runs on every blocking wait
    // in the process and may only rescan on the watcher's own interval.
    // This is the caller asking outright, on its own cadence, so it says so and then drives the watcher.
    if (_watcher == nullptr)
        return;

    _watcher_poll_now->store(true);
    (void)_watcher->process_messages_if_unthreaded(); // a no-op while the watcher has its own thread
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
    // Embedded first, then the real source dir over it; a missing directory simply finds nothing.
    add_package(package, nullptr);
}

void slib::shader_library::add_package(shader_package const& package, filesystem_handle fs)
{
    CC_ASSERT(!package.name.empty(), "a shader package must be named");
    CC_ASSERT(_watcher == nullptr, "add every package before start_hot_reload — the watcher walks the asset "
                                   "list from its own thread, so it must not grow underneath it");

    for (auto const& existing : _packages)
        CC_ASSERT(existing.name != package.name, "this shader package was already added");

    _packages.push_back(package_entry{.name = cc::string::create_copy_of(package.name),
                                      .host_namespace = cc::string::create_copy_of(package.host_namespace),
                                      .language = package.language});

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

u64 slib::shader_library::generation() const
{
    return sg::reload_generation();
}

u64 slib::current_reload_generation()
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

    auto source = _mounts.read_text(virtual_path);
    if (!source.has_value())
    {
        outcome.shader = make_failed_shader(cc::format("shader source not found: '{}'", virtual_path));
        return outcome;
    }

    // Where an `#include "..."` is looked for, most specific first: the shader's own directory, then the package's own root, then the mount root.
    _compile_text(outcome, cc::move(source.value()), virtual_path, impl::parent_path(virtual_path), package.name,
                  package.host_namespace, package.language, stage, entry_point, format);
    return outcome;
}

sg::async_compiled_shader slib::shader_library::compile_source(cc::string_view source,
                                                               sg::shader_stage stage,
                                                               cc::string_view entry_point,
                                                               sg::shader_format format,
                                                               compile_source_options const& opts) const
{
    compile_outcome outcome;
    _compile_text(outcome, cc::string::create_copy_of(source), opts.label, opts.include_dir, cc::string_view(),
                  cc::string_view(), opts.language, stage, entry_point, format);
    return cc::move(outcome.shader);
}

void slib::shader_library::_compile_text(compile_outcome& outcome,
                                         cc::string source,
                                         cc::string_view label,
                                         cc::string_view source_dir,
                                         cc::string_view package_root,
                                         cc::string_view host_namespace,
                                         shader_language language,
                                         sg::shader_stage stage,
                                         cc::string_view entry_point,
                                         sg::shader_format format) const
{
    auto const* const compiler = find_compiler(language, format);
    if (compiler == nullptr)
    {
        outcome.shader = make_failed_shader(cc::format("no compiler registered to build '{}' into this format", label));
        return;
    }

    // Every path the resolver hands back becomes a dependency, so an edit to any include reloads the shaders that pulled it in.
    // Resolving the same file twice yields empty text (pragma-once semantics) rather than a duplicate expansion.
    cc::set<cc::string> seen;
    seen.insert(cc::string::create_copy_of(label));

    // Fixed from the shader being compiled, so an include pulled in by another include resolves from here too, not from its includer.
    auto const search_roots = {source_dir, package_root, cc::string_view()};

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

    shader_source_description desc = {.source = cc::move(source),
                                      .entry_point = cc::string::create_copy_of(entry_point),
                                      .stage = stage,
                                      .label = cc::string::create_copy_of(label)};

    auto preprocessed = compiler->preprocess(desc, resolve);
    if (preprocessed.has_error())
    {
        outcome.shader
            = make_failed_shader(cc::format("preprocessing '{}' failed: {}", label, preprocessed.error().to_string()));
        return;
    }

    desc.source = cc::move(preprocessed.value().source);
    auto interface = cc::optional<sgl_interface>();
    if (preprocessed.value().interface.has_value())
    {
        interface = sgl_interface{.shader = cc::move(preprocessed.value().interface.value()),
                                  .declared = cc::move(preprocessed.value().declared_bindings),
                                  .layouts = cc::move(preprocessed.value().layouts),
                                  .compiler = compiler,
                                  .label = cc::string::create_copy_of(label)};
        auto& target_set = interface.value().shader.target_set;
        if (!target_set.empty())
            target_set = host_namespace.empty() ? cc::string() : cc::format("{}::{}", host_namespace, target_set);
    }
    // A preprocessor that renamed the entry point says so, and the compile has to ask for the name the text declares.
    if (!preprocessed.value().entry_point.empty())
        desc.entry_point = cc::move(preprocessed.value().entry_point);

    // Between the flatten and the compile, because a group's numbering is defined over one flattened translation
    // unit, and because a decorating compiler could be displaced by any later add_compiler for the same edge.
    // It also keeps the compiler's cache key honest: everything the rewrite depends on is folded into the source
    // it hashes.
    // The pass reads HLSL's binding attributes, so a WGSL module, which states its own addresses, never goes through it.
    // Neither does SGL's text, which carries its final addresses on every target.
    // Skipped, DXC numbers the registers itself and puts every group in space 0 and set 0, which a one-group shader
    // cannot tell apart from the right answer.
    auto const is_hlsl_text = compiler->source_language() == shader_language::hlsl;
    if (is_hlsl_text)
    {
        auto rewritten = rewrite_binding_groups(desc.source, format);
        if (rewritten.has_error())
        {
            outcome.shader = make_failed_shader(
                cc::format("rewriting the bindings of '{}' failed: {}", label, rewritten.error().to_string()));
            return;
        }
        desc.source = cc::move(rewritten.value());
    }
    outcome.shader = compiler->compile(desc);
    if (interface.has_value())
        outcome.shader = with_interface(cc::move(outcome.shader), cc::move(interface.value()));
    _backlog.track(outcome.shader);
}
