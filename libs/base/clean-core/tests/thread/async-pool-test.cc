#include <clean-core/common/macros.hh> // CC_HAS_THREADS

// The concurrent scheduler and its tests need OS threads. Everything thread-free lives in async-test.cc; this
// file is the threaded counterpart, so it compiles to nothing where threads are unavailable (e.g. wasm).
#if CC_HAS_THREADS

#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_thread_pool.hh>
#include <nexus/test.hh>

#include <chrono>
#include <memory>
#include <thread>

using cc::async_context;
using cc::async_result;

namespace
{
// A balanced binary sum-tree: leaves count 1, internal nodes sum both children. Depth d has 2^d leaves, so a
// correct drive returns 2^d — a compact whole-graph workload to run on a pool. The sum alone proves every leaf
// ran exactly once, so there is no leaf-execution counter here (a shared counter would be a data race across
// the pool's worker threads, and redundant).
cc::shared_async<cc::i64> build_sum_tree(int depth)
{
    if (depth == 0)
        return cc::make_async_lazy<cc::i64>([] { return cc::i64(1); });

    auto left = build_sum_tree(depth - 1);
    auto right = build_sum_tree(depth - 1);
    return cc::make_async_lazy([](cc::i64 l, cc::i64 r) { return l + r; }, left, right);
}

// A user-defined affinity class ("render", bit 1) and its route to a dedicated pool. The route is a plain fn
// pointer, so the target pool is reached through a file-static handle the test sets up before use.
constexpr cc::async_affinity render_affinity{2}; // bit 1
cc::async_thread_pool* g_test_render_pool = nullptr;
void route_to_render_pool(std::shared_ptr<cc::async_node_base> node)
{
    g_test_render_pool->submit(cc::move(node));
}
} // namespace

TEST("async - a dependency tree drives correctly on a thread pool")
{
    cc::async_thread_pool pool(4);

    int const depth = 10; // 1024 leaves
    auto root = build_sum_tree(depth);

    CHECK(pool.blocking_get(root) == (cc::i64(1) << depth));
}

TEST("async - many independent asyncs fan out across the pool")
{
    cc::async_thread_pool pool(4);

    // one root that sums 64 independent children via a two-phase frame (creates + requires them, then reads
    // them) — the children fan out across the pool's workers
    int const n = 64;
    auto root = cc::make_async_lazy<cc::i64>(
        [n, step = 0, kids = cc::vector<cc::shared_async<cc::i64>>()](async_context& actx) mutable -> async_result<cc::i64>
        {
            if (step++ == 0)
            {
                for (int i = 0; i < n; ++i)
                {
                    auto k = cc::make_async_lazy([i] { return cc::i64(i); });
                    (void)actx.require(k);
                    kids.push_back(cc::move(k));
                }
                return actx.wait_for_dependencies();
            }
            cc::i64 sum = 0;
            for (auto const& k : kids)
                sum += *k->value_ptr();
            return actx.success(sum);
        });

    CHECK(pool.blocking_get(root) == cc::i64(n) * (n - 1) / 2);
}

TEST("async - external push from a foreign thread wakes a pool-parked dependent")
{
    cc::async_thread_pool pool(2);
    cc::scoped_default_async_pool as_default(pool); // so the foreign push routes the woken dependent back here

    auto ext = cc::make_async_manual<int>();
    auto p = cc::make_async_lazy([](int x) { return x + 1; }, ext);

    std::thread pusher(
        [&]
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2)); // bias toward p parking first
            ext->push_value(41);
        });

    int const v = pool.blocking_get(p);
    pusher.join();

    CHECK(v == 42);
}

TEST("async - affinity routes a task to the pool serving its class, not the general pool")
{
    cc::async_thread_pool general_pool(2, cc::async_affinity::general());
    cc::async_thread_pool render_pool(1, render_affinity);
    g_test_render_pool = &render_pool;

    // identify the render pool's single worker thread
    auto render_worker = std::make_shared<std::thread::id>();
    {
        auto probe = cc::make_async_lazy(
            [render_worker]
            {
                *render_worker = std::this_thread::get_id();
                return 0;
            });
        probe->set_affinity(render_affinity, &route_to_render_pool);
        render_pool.blocking_get(probe);
    }

    // a render-affinity task runs on the render worker
    auto rid = std::make_shared<std::thread::id>();
    auto rtask = cc::make_async_lazy(
        [rid]
        {
            *rid = std::this_thread::get_id();
            return 7;
        });
    rtask->set_affinity(render_affinity, &route_to_render_pool);
    CHECK(render_pool.blocking_get(rtask) == 7);
    CHECK(*rid == *render_worker);

    // a general task never runs on the render worker
    auto gid = std::make_shared<std::thread::id>();
    auto gtask = cc::make_async_lazy(
        [gid]
        {
            *gid = std::this_thread::get_id();
            return 9;
        });
    CHECK(general_pool.blocking_get(gtask) == 9);
    CHECK(*gid != *render_worker);

    g_test_render_pool = nullptr;
}

TEST("async - affinity is frozen once scheduled")
{
    cc::async_thread_pool pool(2);

    auto a = cc::make_async_lazy([] { return 1; });
    CHECK(pool.blocking_get(a) == 1); // now ready, no longer cold

    CHECK_ASSERTS(a->set_affinity(render_affinity, &route_to_render_pool));
}

TEST("async - installing a second default pool asserts")
{
    cc::async_thread_pool pool_a(1);
    cc::async_thread_pool pool_b(1);

    cc::scoped_default_async_pool as_default(pool_a);
    CHECK_ASSERTS(cc::install_default_async_pool(pool_b)); // a default is already installed
}

TEST("async - two pools coexist and drive independent graphs")
{
    cc::async_thread_pool pool_a(2);
    cc::async_thread_pool pool_b(3);

    auto root_a = build_sum_tree(8); // 256 leaves
    auto root_b = build_sum_tree(9); // 512 leaves

    CHECK(pool_a.blocking_get(root_a) == (cc::i64(1) << 8));
    CHECK(pool_b.blocking_get(root_b) == (cc::i64(1) << 9));
}

TEST("async - stress: many small graphs on a pool")
{
    cc::async_thread_pool pool(4);

    for (int iter = 0; iter < 200; ++iter)
    {
        auto root = build_sum_tree(6); // 64 leaves
        CHECK(pool.blocking_get(root) == (cc::i64(1) << 6));
    }
}

#endif // CC_HAS_THREADS
