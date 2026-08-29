#include "shader_library.hh"

#include <clean-core/platform/leak_annotations.hh> // cc::leak_scope — this library is never freed, by design
#include <shaped-rendering/shaders.hh>             // sr::shader_package (the blit the compositor drives)
#include <shaped-shader-library/compiler/dxc_compiler.hh>
#include <shaped-shader-library/shader_library.hh>
#include <shaped-viewer/rendering/shaders.hh>

namespace sv
{
namespace
{
/// The caller's provider, unset until `set_acquire_shader_library` is called.
/// It lives here rather than in the header so the only way to reach it is the setter — nothing can read it, and no translation
/// unit can race the others to initialize it.
shader_library_provider g_acquire_shader_library;
} // namespace

void set_acquire_shader_library(shader_library_provider provider)
{
    g_acquire_shader_library = cc::move(provider);
}

cc::result<slib::shader_library*> impl::acquire_default_shader_library()
{
    // Deliberately leaked: the generated package symbols an asset is reached through are process-wide globals that outlive any
    // owner, so destroying the library at exit would leave them naming freed assets.
    // The whole construction is scoped rather than the library pointer annotated, because the filesystems the packages mount are
    // owned through a cc:: container and LeakSanitizer cannot reach them from `lib` — cc::leak_scope's header says why.
    // libs/graphics/shaped-viewer/docs/TODO.md carries the case for a shape that leaks nothing and needs no annotation at all.
    auto const leak_guard = cc::leak_scope();

    auto* const lib = new slib::shader_library();

#if SLIB_HAS_DXC
    auto compiler = slib::create_dxc_compiler();
    if (compiler.has_value())
        lib->add_compiler(cc::move(compiler.value()));
#endif

    // Both packages, always: sv's routines trace with theirs, and the compositor places every view with sr's blit.
    lib->add_package(sv::shader_package());
    lib->add_package(sr::shader_package());
    return lib;
}

cc::result<slib::shader_library*> acquire_shader_library()
{
    // One library for the process, which is not a preference but slib's rule: a second would fight the first over the package
    // globals both write into.
    static slib::shader_library* cached = nullptr;
    if (cached != nullptr)
        return cached;

    auto r = g_acquire_shader_library ? g_acquire_shader_library() : impl::acquire_default_shader_library();

    // A failure is deliberately not cached: it leaves a caller free to call `set_acquire_shader_library` and try again.
    if (r.has_error())
        return r;
    if (r.value() == nullptr)
        return cc::error("shaped-viewer: the shader library provider returned no library");

    cached = r.value();
    return cached;
}
} // namespace sv
