#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/thread/async.hh>
#include <nexus/test.hh>

#include <memory>

// These tests drive the graph inline on the calling thread (cc::async_blocking_get or an explicit
// inline_scheduler + async_worker_scope) — deterministic and thread-free, matching the threaded_actor test
// philosophy. The concurrent work-stealing scheduler and its tests live in async-pool-test.cc (threads only).

using cc::async_context;
using cc::async_result;

// ============================================================================
// basics
// ============================================================================

TEST("async - basic scheduled async and zero-copy try_value")
{
    auto a = cc::make_async_lazy([] { return 42; });

    CHECK(cc::async_blocking_get(a) == 42);

    auto v = a->try_value();
    REQUIRE(v != nullptr);
    CHECK(*v == 42);

    // the aliasing shared_ptr keeps the node alive after the original handle is dropped
    a.reset();
    CHECK(*v == 42);
}

TEST("async - try_value is empty before completion")
{
    auto a = cc::make_async_lazy([] { return 1; });
    CHECK(a->try_value() == nullptr);
    CHECK(!a->is_ready());
}

TEST("async - success via context helper")
{
    // f keeps its async_context to exercise success(); T is deduced from the success tag
    auto a = cc::make_async_lazy([](async_context& actx) { return actx.success(cc::string("hi")); });
    CHECK(cc::async_blocking_get(a) == "hi");
}

// ============================================================================
// map — dataflow transform (never `then`)
// ============================================================================

TEST("async - member map_lazy transforms a value")
{
    auto a = cc::make_async_lazy([] { return 20; });
    auto b = a->map_lazy([](int x) { return x + 22; });
    CHECK(cc::async_blocking_get(b) == 42);
}

TEST("async - chained member map_lazy")
{
    auto a = cc::make_async_lazy([] { return 1; });
    auto b = a->map_lazy([](int x) { return x + 1; });
    auto c = b->map_lazy([](int x) { return x * 10; });
    CHECK(cc::async_blocking_get(c) == 20);
}

TEST("async - variadic dependency form unwraps async args")
{
    auto a = cc::make_async_lazy([] { return 3; });
    auto b = cc::make_async_lazy([] { return 4; });

    // c depends on a and b; its function receives plain ints, and runs only once both are ready
    auto c = cc::make_async_lazy([](int x, int y) { return x * y; }, a, b);

    CHECK(cc::async_blocking_get(c) == 12);
}

TEST("async - variadic dependency form short-circuits on a dependency error")
{
    auto a = cc::make_async_lazy([] { return 3; });
    auto bad = cc::make_async_lazy<int>([](async_context& actx) -> async_result<int>
                                        { return actx.error(cc::any_error("boom")); });

    bool ran = false;
    auto c = cc::make_async_lazy(
        [&](int x, int y)
        {
            ran = true;
            return x + y;
        },
        a, bad);

    auto r = cc::try_async_blocking_get(c);
    CHECK(r.has_error());
    CHECK(!ran);
}

TEST("async - frames may omit the async_context parameter")
{
    // no context, no deps
    auto a = cc::make_async_lazy([] { return 41; });
    CHECK(cc::async_blocking_get(a) == 41);

    // no context, with a dependency (f gets the plain value)
    auto b = cc::make_async_lazy([](int x) { return x + 1; }, a);
    CHECK(cc::async_blocking_get(b) == 42);
}

TEST("async - dependency frame may still take a leading async_context")
{
    auto a = cc::make_async_lazy([] { return 10; });

    // f receives the context plus the unwrapped dependency value
    auto b = cc::make_async_lazy([](async_context& actx, int x) { return actx.success(x * 2); }, a);
    CHECK(cc::async_blocking_get(b) == 20);
}

// ============================================================================
// dynamic dependencies
// ============================================================================

TEST("async - dynamic dependency added during compute, removed once ready")
{
    // step 0 creates a dependency mid-compute, requires it, and waits; step 1 reads its value. The dependency
    // must be gone from the pending list by the time the parent completes.
    auto p = cc::make_async_lazy<int>(
        [step = 0, child = cc::shared_async<int>()](async_context& actx) mutable -> async_result<int>
        {
            switch (step++)
            {
            case 0:
                child = cc::make_async_lazy([] { return 10; }); // a regular dependency, created on the fly
                (void)actx.require(child);
                return actx.wait_for_dependencies();
            default:
                return actx.success(*child->value_ptr() + 5);
            }
        });

    CHECK(cc::async_blocking_get(p) == 15);
    CHECK(p->pending_dependency_count() == 0);
}

TEST("async - already-ready dependency completes without parking")
{
    auto dep = cc::make_async_manual<int>();
    dep->push_value(7); // ready before anyone requires it

    auto p = cc::make_async_lazy<int>(
        [dep](async_context& actx) -> async_result<int>
        {
            bool const ready = actx.require(dep);
            CHECK(ready); // require on a ready dep returns true immediately
            return actx.success(*dep->value_ptr() + 1);
        });

    CHECK(cc::async_blocking_get(p) == 8);
    CHECK(p->pending_dependency_count() == 0);
    CHECK(dep->continuation_count() == 0); // never subscribed — the dep was already ready
}

TEST("async - required cold dependency is scheduled and driven to completion")
{
    // The dependency is a separate cold async captured by the parent. Requiring it schedules it, and the
    // scheduler drives it to completion, waking the parent — all within one async_blocking_get.
    auto dep = cc::make_async_lazy([] { return 100; });
    auto p = cc::make_async_lazy<int>(
        [dep](async_context& actx) -> async_result<int>
        {
            if (!actx.require(dep))
                return actx.wait_for_dependencies();
            return actx.success(*dep->value_ptr() + 1);
        });

    CHECK(cc::async_blocking_get(p) == 101);
}

// ============================================================================
// frame-invocation invariants
// ============================================================================

TEST("async - a frame is never invoked again after it produces a value")
{
    auto calls = std::make_shared<int>(0);
    auto a = cc::make_async_lazy(
        [calls]
        {
            ++*calls;
            return 7;
        });

    CHECK(cc::async_blocking_get(a) == 7);
    CHECK(*calls == 1);

    // extra scheduling of a completed node must not resurrect and re-invoke the (destroyed) frame
    {
        cc::inline_scheduler s;
        cc::async_worker_scope sc(s);
        a->schedule();
        s.run_until([] { return true; });
    }
    CHECK(*calls == 1);
}

TEST("async - a two-phase frame runs exactly twice (register deps, then compute)")
{
    // First poll registers a dependency and waits; the second (and last) poll computes. The frame must be
    // entered exactly twice — never again after it returns success.
    auto calls = std::make_shared<int>(0);
    auto p = cc::make_async_lazy<int>(
        [calls, step = 0, child = cc::shared_async<int>()](async_context& actx) mutable -> async_result<int>
        {
            ++*calls;
            switch (step++)
            {
            case 0:
                child = cc::make_async_lazy([] { return 1; });
                (void)actx.require(child);
                return actx.wait_for_dependencies();
            default:
                return actx.success(*child->value_ptr());
            }
        });

    CHECK(cc::async_blocking_get(p) == 1);
    CHECK(*calls == 2);
}

// ============================================================================
// late subscription / external completion
// ============================================================================

TEST("async - blocking on external dep subscribes late, completion wakes it")
{
    cc::inline_scheduler sched;
    cc::async_worker_scope scope(sched);

    auto ext = cc::make_async_manual<int>();
    auto p = cc::make_async_lazy<int>(
        [ext](async_context& actx) -> async_result<int>
        {
            if (!actx.require(ext))
                return actx.wait_for_dependencies();
            return actx.success(*ext->value_ptr() + 1);
        });

    p->schedule();
    sched.run_until([&] { return p->is_ready(); });

    // the graph cannot progress until ext is pushed: p parked and subscribed exactly once
    CHECK(!p->is_ready());
    CHECK(ext->continuation_count() == 1);

    ext->push_value(41); // wakes p
    sched.run_until([&] { return p->is_ready(); });

    REQUIRE(p->is_ready());
    CHECK(*p->try_value() == 42);
    CHECK(ext->continuation_count() == 0); // detached on completion
}

// ============================================================================
// error propagation
// ============================================================================

TEST("async - error short-circuits member map, f never runs")
{
    auto a = cc::make_async_lazy<int>([](async_context& actx) -> async_result<int>
                                      { return actx.error(cc::any_error("boom")); });

    bool ran = false;
    auto b = a->map_lazy(
        [&](int x)
        {
            ran = true;
            return x + 1;
        });

    auto r = cc::try_async_blocking_get(b);
    CHECK(r.has_error());
    CHECK(!ran);
}

TEST("async - try_async_blocking_get surfaces a value")
{
    auto a = cc::make_async_lazy([] { return 3; });
    auto r = cc::try_async_blocking_get(a);
    REQUIRE(r.has_value());
    CHECK(r.value() == 3);
}

TEST("async - cancellation propagates as a value")
{
    auto a = cc::make_async_lazy<int>([](async_context& actx) -> async_result<int>
                                      { return actx.error(cc::async_error::make_cancelled()); });

    auto r = cc::try_async_blocking_get(a);
    REQUIRE(r.has_error());
    CHECK(r.error().is_cancelled());
}

// ============================================================================
// scale — a many-node dependency tree
// ============================================================================

namespace
{
// Build a balanced binary sum-tree of the given depth. Leaves count 1; internal nodes require both children
// and sum them. leaf_exec counts leaf-frame runs, so we can assert no completed node is recomputed and that
// undemanded work stays cold. (Internal frames legitimately run twice — once to register deps and return
// wait, once to compute after they are ready — which is inherent to the re-entrant poll model.)
cc::shared_async<cc::i64> build_sum_tree(int depth, std::shared_ptr<cc::i64> leaf_exec)
{
    if (depth == 0)
        return cc::make_async_lazy<cc::i64>(
            [leaf_exec]
            {
                ++*leaf_exec;
                return cc::i64(1);
            });

    auto left = build_sum_tree(depth - 1, leaf_exec);
    auto right = build_sum_tree(depth - 1, leaf_exec);
    // internal nodes use the variadic dependency form: both children awaited + unwrapped, then summed
    return cc::make_async_lazy([](cc::i64 l, cc::i64 r) { return l + r; }, left, right);
}
} // namespace

TEST("async - large dependency tree drives correctly without computing undemanded branches")
{
    auto leaf_exec = std::make_shared<cc::i64>(0);

    int const depth = 13; // 2^13 leaves -> 16383 nodes total
    auto root = build_sum_tree(depth, leaf_exec);

    // an undemanded sibling subtree: never required, must stay cold
    auto orphan_ran = std::make_shared<bool>(false);
    auto orphan = cc::make_async_lazy<cc::i64>(
        [orphan_ran]
        {
            *orphan_ran = true;
            return cc::i64(7);
        });

    CHECK(cc::async_blocking_get(root) == (cc::i64(1) << depth)); // sum of all leaves == leaf count == 8192

    // each demanded leaf ran exactly once (no completed node recomputed); the orphan never did
    CHECK(*leaf_exec == (cc::i64(1) << depth));
    CHECK(!*orphan_ran);
    CHECK(!orphan->is_ready());
}
