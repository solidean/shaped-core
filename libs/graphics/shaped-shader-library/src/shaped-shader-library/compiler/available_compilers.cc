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
    // Each edge needs its own inner compiler, since the SGL one takes ownership of it.
    if (auto dxil = create_dxc_compiler(); dxil.has_value())
        lib.add_compiler(cc::move(dxil.value()));
    if (auto spirv = create_dxc_spirv_compiler(); spirv.has_value())
        lib.add_compiler(cc::move(spirv.value()));
    if (auto dxil = create_dxc_compiler(); dxil.has_value())
        lib.add_compiler(create_sgl_compiler(cc::move(dxil.value())));
    if (auto spirv = create_dxc_spirv_compiler(); spirv.has_value())
        lib.add_compiler(create_sgl_compiler(cc::move(spirv.value())));
#endif

#if SLIB_HAS_METAL
    lib.add_compiler(create_metal_compiler());
    lib.add_compiler(create_sgl_compiler(create_metal_compiler()));
#endif
}
