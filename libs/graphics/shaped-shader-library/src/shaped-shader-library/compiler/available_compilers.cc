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

void slib::add_available_compilers(shader_library& lib)
{
    lib.add_compiler(create_wgsl_compiler());
    lib.add_compiler(create_sgl_compiler(create_wgsl_compiler()));

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
        // EMIT-4: SGL writes a half as `float16_t`, which DXC compiles only with 16-bit types enabled
        if (auto sgl_dxil = create_dxc_compiler(sgl_dxc_options()); sgl_dxil.has_value())
            lib.add_compiler(create_sgl_compiler(cc::move(sgl_dxil.value())));
    }

    if (spirv.has_value())
    {
        lib.add_compiler(cc::move(spirv.value()));
        if (auto sgl_spirv = create_dxc_spirv_compiler(sgl_dxc_options()); sgl_spirv.has_value())
            lib.add_compiler(create_sgl_compiler(cc::move(sgl_spirv.value())));
    }
#endif

#if SLIB_HAS_METAL
    lib.add_compiler(create_metal_compiler());
    // EMIT-4: SGL writes `coherent(device)` and texture atomics, which a metallib compiles from MSL 3.2 on
    lib.add_compiler(create_sgl_compiler(create_metal_compiler("metal3.2")));
#endif
}
