#include <nexus/run.hh>

#ifdef SR_TEST_HAS_METAL
#include <shaped-graphics/backends/metal/metal_common.hh>
#endif

int main(int argc, char** argv)
{
#ifdef SR_TEST_HAS_METAL
    // Metal's validation layer is switched on through the environment, and only before the framework initializes,
    // so it cannot wait for the entry driver; see arm_validation_layer.
    sg::backend::metal::arm_validation_layer();
#endif

    return nx::run(argc, argv);
}
