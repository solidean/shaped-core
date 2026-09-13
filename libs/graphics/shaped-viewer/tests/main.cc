#include <nexus/run.hh>

#if SV_TEST_HAS_PROBE_SHADERS
#include <shaped-shader-library/shader_library.hh>
#include <shaped-viewer/shader_library.hh>
#include <sv_test_shaders.hh>
#endif

int main(int argc, char** argv)
{
#if SV_TEST_HAS_PROBE_SHADERS
    // The BSDF probe's package joins the one shader library as it is created, before any test can compile through it.
    // `add_package` is not safe beside a concurrent acquire, and the tests sharing that library run in parallel.
    sv::set_acquire_shader_library(
        []
        {
            auto lib = sv::impl::acquire_default_shader_library();
            if (lib.has_value())
                lib.value()->add_package(sv_test::shaders::package());
            return lib;
        });
#endif

    return nx::run(argc, argv);
}
