#include <nexus/run.hh>

#if SSC_MSL_TEST_HAS_METAL
#include <shaped-graphics/backends/metal/metal_common.hh>
#endif

int main(int argc, char** argv)
{
#if SSC_MSL_TEST_HAS_METAL
    // Metal has no validation-message callback, so the gate is an abort — and the framework reads these variables when
    // it first initializes, which makes main the only moment that works.
    // See libs/graphics/shaped-graphics/backends/metal/readme.md.
    sg::backend::metal::arm_validation_layer();
#endif

    return nx::run(argc, argv);
}
