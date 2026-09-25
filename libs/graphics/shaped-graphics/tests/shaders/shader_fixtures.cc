#include "shader_fixtures.hh"

#include <shaped-graphics/context/context.hh>
#include <shaped-shader-library/compiler/sgl_compiler.hh>
#include <shaped-shader-library/compiler/wgsl_compiler.hh>
#include <shaped-shader-library/shader_library.hh>

// The packages this target declares (sc_add_shader_package in shaped-graphics' CMakeLists), generated into the build
// dir and private to this binary.
#include <sg_test_sgl_shaders.hh>

#if SLIB_HAS_DXC
#include <shaped-shader-library/compiler/dxc_compiler.hh>
#endif

#if SG_TEST_HAS_HLSL_SHADERS
#include <sg_test_shaders.hh>
#endif

namespace
{
/// Every edge this build can make.
/// A shader_asset picks between them by asking the context what it accepts, which is what lets the tests name no backend.
void add_compilers(slib::shader_library& lib)
{
    // Native to slib and present on every platform, so the SGL fixtures reach a WebGPU context with no toolchain at all.
    lib.add_compiler(slib::create_sgl_compiler(slib::create_wgsl_compiler()));

#if SLIB_HAS_DXC
    if (auto dxil = slib::create_dxc_compiler(); dxil.has_value())
        lib.add_compiler(cc::move(dxil.value()));
    if (auto spirv = slib::create_dxc_spirv_compiler(); spirv.has_value())
        lib.add_compiler(cc::move(spirv.value()));

    // The same two compilers again, behind SGL's preprocessing: sgl -> dxil and sgl -> spirv are their own edges.
    if (auto dxil = slib::create_dxc_compiler(); dxil.has_value())
        lib.add_compiler(slib::create_sgl_compiler(cc::move(dxil.value())));
    if (auto spirv = slib::create_dxc_spirv_compiler(); spirv.has_value())
        lib.add_compiler(slib::create_sgl_compiler(cc::move(spirv.value())));
#endif
}

slib::shader_library& create_library()
{
    static slib::shader_library lib;
    add_compilers(lib);

    lib.add_package(sg::test::sgl_shaders::package());
#if SG_TEST_HAS_HLSL_SHADERS
    lib.add_package(sg::test::shaders::package());
#endif

    return lib;
}
} // namespace

slib::shader_library& sg_test::shader_fixtures()
{
    // The inner static is only ever reached through this one, so the guard here is what makes the fill run once.
    static slib::shader_library& lib = create_library();
    return lib;
}

bool sg_test::shaders_reach(sg::context const& ctx)
{
    auto& lib = shader_fixtures();

    // Both fixture languages, because which packages this build has is a build property: the HLSL one is absent
    // without DXC, and `supported_formats` then simply answers with nothing.
    for (auto const language : {slib::shader_language::sgl, slib::shader_language::hlsl})
        for (auto const format : lib.supported_formats(language))
            if (ctx.accepts_shader_format(format))
                return true;

    return false;
}
