#include "shader_fixtures.hh"

#include <shaped-graphics/context/context.hh>
#include <shaped-shader-library/compiler/available_compilers.hh>
#include <shaped-shader-library/shader_library.hh>

// The packages this target declares (sc_add_shader_package in shaped-graphics' CMakeLists), generated into the build
// dir and private to this binary.
#include <sg_test_sgl_shaders.hh>

#if SG_TEST_HAS_HLSL_SHADERS
#include <sg_test_shaders.hh>
#endif

namespace
{
slib::shader_library& create_library()
{
    static slib::shader_library lib;

    // Every edge this build can make: a shader_asset picks between them by asking the context what it accepts,
    // which is what lets the tests name no backend.
    slib::add_available_compilers(lib);

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
