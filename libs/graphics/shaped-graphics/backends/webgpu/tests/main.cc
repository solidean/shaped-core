#include <blob-cache/default_cache.hh>
#include <nexus/run.hh>

int main(int argc, char** argv)
{
    auto const cache = bcache::scoped_test_cache("shaped-graphics-webgpu-test");
    return nx::run(argc, argv);
}
