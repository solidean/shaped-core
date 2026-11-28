#include <clean-core/container/vector.hh>
#include <clean-core/memory/allocation.hh>
#include <clean-core/thread/native_thread.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// The live bytes are the whole process's, so each test runs alone and allocates far more than background threads do:
// what a recorder, a sampler or a thread's own start allocates meanwhile, and what mimalloc has yet to merge, stays within `noise`.
// Without mimalloc the default resource reports nothing, which is all there is to test.

namespace
{
constexpr auto block = i64(64) << 20;
constexpr auto noise = i64(8) << 20; // a thread started takes some 4 MB of its own

[[nodiscard]] i64 live()
{
    return cc::query_default_memory_usage().live_bytes;
}

[[nodiscard]] bool near(i64 a, i64 b)
{
    return a - b <= noise && b - a <= noise;
}
} // namespace

#if CC_HAS_MIMALLOC
TEST("allocation - the live bytes count a block from when it is handed out until it is taken back",
     main_thread,
     exclusive())
{
    auto const before = live();
    {
        auto bytes = cc::vector<byte>();
        bytes.reserve(block);
        auto const during = live() - before;
        CHECK(during >= block); // the block as handed out, which a size class may round up
        CHECK(near(during, block));
    }
    CHECK(near(live(), before));
}

TEST("allocation - a block outlives the thread that allocated it, and counts until another thread frees it",
     main_thread,
     exclusive())
{
    auto const before = live();
    auto bytes = cc::vector<byte>();
    cc::native_thread::start(
        [&]
        {
            auto made = cc::vector<byte>();
            made.reserve(block);
            bytes = cc::move(made);
        })
        .join();
    // the thread has ended, the block still live
    CHECK(near(live() - before, block));
    bytes = {};
    // freed by another thread than the one that allocated it
    CHECK(near(live(), before));
}

#endif

TEST("allocation - what the allocator behind the default resource holds covers what it handed out",
     main_thread,
     exclusive())
{
    auto bytes = cc::vector<byte>();
    bytes.reserve(block);
    auto const usage = cc::query_default_memory_usage();
#if CC_HAS_MIMALLOC
    CHECK(usage.committed_bytes >= block);
    CHECK(usage.live_bytes >= block);
    CHECK(usage.committed_bytes >= usage.live_bytes);
#else
    CHECK(usage.committed_bytes == -1);
    CHECK(usage.live_bytes == -1);
#endif
}
