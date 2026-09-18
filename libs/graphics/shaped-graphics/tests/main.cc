#include <nexus/run.hh>

#ifdef SG_TEST_HAS_METAL
#include <shaped-graphics/backends/metal/metal_common.hh>
#endif

int main(int argc, char** argv)
{
#ifdef SG_TEST_HAS_METAL
    // Metal's validation layer is switched on through the environment rather than through an API, and only before the
    // framework initializes — so it has to happen here rather than at context creation.
    // The other two backends take a flag on their config instead, which is why this is the one backend with a line in
    // main; see arm_validation_layer.
    sg::backend::metal::arm_validation_layer();
#endif

    return nx::run(argc, argv);
}
