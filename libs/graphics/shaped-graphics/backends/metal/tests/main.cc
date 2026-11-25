#include <blob-cache/default_cache.hh>
#include <nexus/run.hh>
#include <shaped-graphics/backends/metal/metal_common.hh>

int main(int argc, char** argv)
{
    // Before anything else, because the layer is configured through the environment and Metal reads it when the
    // framework first initializes — see arm_validation_layer.
    // This is the tier-2 suite's only validation oracle: Metal hands a message to stderr and to no callback, so the
    // abort is what stops a violation being a log line nobody reads.
    sg::backend::metal::arm_validation_layer();

    auto const cache = bcache::scoped_test_cache("shaped-graphics-metal-test");
    return nx::run(argc, argv);
}
