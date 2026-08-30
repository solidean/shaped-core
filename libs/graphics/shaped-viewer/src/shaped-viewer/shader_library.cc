#include "shader_library.hh"

#include <clean-core/platform/leak_annotations.hh> // cc::leak_scope — this library is never freed, by design
#include <clean-core/thread/mutex.hh>
#include <shaped-rendering/shaders.hh> // sr::shader_package (the blit the compositor drives)
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
    // Both formats, so a shader resolves for whichever backend acquires it: shader_asset picks by asking the context
    // what it accepts, so registering both is what makes one library serve a dx12 and a vulkan context alike.
    // Each registration is best-effort — a format this build cannot produce simply is not offered.
    if (auto dxil = slib::create_dxc_compiler(); dxil.has_value())
        lib->add_compiler(cc::move(dxil.value()));
    if (auto spirv = slib::create_dxc_spirv_compiler(); spirv.has_value())
        lib->add_compiler(cc::move(spirv.value()));
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
    //
    // The memoization is under a lock because "give me the process-wide library" is a call anything may make from any thread,
    // and a plain check-then-set is not that.
    // Two first callers both found no library and both built one, and the second construction trips slib's own assertion —
    // so the race did not read as a race, it read as an abort with no message.
    // A parallel test run is what found it, on the one platform whose interleaving happened to hit it.
    static auto cached = cc::mutex<slib::shader_library*>(nullptr);

    return cached.lock(
        [](slib::shader_library*& lib) -> cc::result<slib::shader_library*>
        {
            if (lib != nullptr)
                return lib;

            auto r = g_acquire_shader_library ? g_acquire_shader_library() : impl::acquire_default_shader_library();

            // A failure is deliberately not cached: it leaves a caller free to call `set_acquire_shader_library` and try again.
            if (r.has_error())
                return r;
            if (r.value() == nullptr)
                return cc::error("shaped-viewer: the shader library provider returned no library");

            lib = r.value();
            return lib;
        });
}
} // namespace sv
