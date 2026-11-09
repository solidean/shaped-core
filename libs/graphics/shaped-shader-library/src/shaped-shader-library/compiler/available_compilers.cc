#include <clean-core/record/log.hh>
#include <shaped-shader-library/compiler/available_compilers.hh>
#include <shaped-shader-library/compiler/sgl_compiler.hh>
#include <shaped-shader-library/compiler/wgsl_compiler.hh>
#include <shaped-shader-library/shader_library.hh>

#if SLIB_HAS_DXC
#include <shaped-shader-library/compiler/dxc_compiler.hh>
#endif

#if SLIB_HAS_METAL
#include <shaped-shader-library/compiler/metal_compiler.hh>
#endif

namespace
{
// SC_SHADER_DIAGNOSTICS: SGL writes print and assert records exactly where sg binds a window for them.
// SC_SHADER_UNDEFINED: what an operation with no value does, which without a window to report into is at most abort.
constexpr auto k_guard = slib::sgl_undefined_guard(SLIB_SHADER_UNDEFINED);
constexpr auto k_sgl_options
    = slib::sgl_compiler_options{.diagnostics = SG_SHADER_DIAGNOSTICS != 0, .guards_undefined = k_guard};
} // namespace

void slib::add_available_compilers(shader_library& lib)
{
    lib.add_compiler(create_wgsl_compiler());
    lib.add_compiler(create_sgl_compiler(create_wgsl_compiler(), k_sgl_options));

#if SLIB_HAS_DXC
    // This build fetched DXC, so a DXC that does not create is a broken install, and its reason is the one diagnostic.
    // Both edges fail on that one condition, so they report together rather than twice.
    // Each edge needs its own inner compiler, since the SGL one takes ownership of it.
    auto dxil = create_dxc_compiler();
    auto spirv = create_dxc_spirv_compiler();
    if (dxil.has_error())
        CC_LOG_WARNING("DXC did not load, so no HLSL or SGL shader compiles to DXIL or to SPIR-V: {}",
                       dxil.error().to_string());

    if (dxil.has_value())
    {
        lib.add_compiler(cc::move(dxil.value()));
        if (auto sgl_dxil = create_dxc_compiler(); sgl_dxil.has_value())
            lib.add_compiler(create_sgl_compiler(cc::move(sgl_dxil.value()), k_sgl_options));
    }

    if (spirv.has_value())
    {
        lib.add_compiler(cc::move(spirv.value()));
        if (auto sgl_spirv = create_dxc_spirv_compiler(); sgl_spirv.has_value())
            lib.add_compiler(create_sgl_compiler(cc::move(sgl_spirv.value()), k_sgl_options));
    }
#else
    // no DXC, so vulkan's SGL shaders are what SGL can write itself
    lib.add_compiler(create_sgl_spirv_compiler(k_sgl_options));
#endif

#if SLIB_HAS_METAL
    lib.add_compiler(create_metal_compiler());
    lib.add_compiler(create_sgl_compiler(create_metal_compiler(), k_sgl_options));
#endif
}
