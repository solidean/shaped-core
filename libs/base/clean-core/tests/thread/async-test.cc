#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/async.hh>
#include <nexus/test.hh>

#include <memory>

// These tests drive the graph inline on the calling thread (cc::async_blocking_get_singlethreaded or an explicit
// singlethreaded_scheduler + async_worker_scope) — deterministic and thread-free, matching the threaded_actor test
// philosophy. The concurrent work-stealing scheduler and its tests live in async-pool-test.cc (threads only).

using cc::async_context;

// Node size guards. The node is a 16 B header (intrusive refcount + tagged state/ops word) followed by the
// payload slot: max(scratch, sizeof(T), sizeof(E)), aligned so the whole node is a 64 B line for a value up to
// ~48 B. The value/error shares the payload with the unresolved scratch (frame + deps + continuations) and
// grows the node naturally for a larger T/E (no inline cap — it is built straight into the payload at resolution).
static_assert(sizeof(cc::async<int>) == 64, "async<int> should be exactly one cache line");
static_assert(sizeof(cc::async<cc::vector<int>>) == 64, "async<vector> should stay one cache line");
static_assert(sizeof(cc::async<cc::string>) == 64, "async<string> should stay one cache line");
namespace
{
struct big_value // 96 B: intentionally larger than one line's payload — the node must grow, not fail to compile
{
    cc::i64 data[12] = {};
};
struct big_error // 64 B: a custom failure type larger than the 32 B unresolved scratch — grows the error arm
{
    cc::i64 data[8] = {};
};
} // namespace
static_assert(sizeof(cc::async<big_value>) > 64, "a large value must grow the node onto further cache lines");
// a custom E defaults to async_error; the default async<int> (== async<int, async_error>) stays one cache line
static_assert(sizeof(cc::async<int, cc::async_error>) == 64, "async<int, async_error> should be exactly one cache line");
static_assert(sizeof(cc::async<int, big_error>) > 64, "a large error type must grow the node onto further lines");

// async_type_ops collapse: trivially-destructible payloads of the same node size class share ONE descriptor.
// int and float are both trivial and land in the same 64 B class -> identical ops pointer.
static_assert(&cc::impl::async_type_ops_for<int, cc::async_error> == &cc::impl::async_type_ops_for<float, cc::async_error>,
              "trivially-destructible values of the same size class must share one async_type_ops");
// a non-trivially-destructible value keeps its own value-teardown, so it does NOT collapse with int
static_assert(&cc::impl::async_type_ops_for<int, cc::async_error>
                  != &cc::impl::async_type_ops_for<cc::string, cc::async_error>,
              "a non-trivially-destructible value type must keep its own teardown");

// ============================================================================
// basics
// ============================================================================

TEST("async - basic scheduled async and zero-copy try_value")
{
    auto a = cc::make_async_lazy([] { return 42; });

    CHECK(cc::async_blocking_get_singlethreaded(a) == 42);

    // try_value() is a non-owning pointer into the node (null unless ready with a value); the handle keeps
    // the node alive.
    int const* v = a->try_value();
    REQUIRE(v != nullptr);
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
    // f keeps its async_context to exercise success(); a raw ctx-resolving frame gives its result type explicitly
    auto a = cc::make_async_lazy<cc::string>([](async_context<cc::string>& actx)
                                             { return actx.success(cc::string("hi")); });
    CHECK(cc::async_blocking_get_singlethreaded(a) == "hi");
}

// ============================================================================
// single-dependency transform (the one-argument variadic form)
// ============================================================================

TEST("async - single-dependency transform via make_async_lazy")
{
    auto a = cc::make_async_lazy([] { return 20; });
    auto b = cc::make_async_lazy([](int x) { return x + 22; }, a);
    CHECK(cc::async_blocking_get_singlethreaded(b) == 42);
}

TEST("async - chained single-dependency transforms")
{
    auto a = cc::make_async_lazy([] { return 1; });
    auto b = cc::make_async_lazy([](int x) { return x + 1; }, a);
    auto c = cc::make_async_lazy([](int x) { return x * 10; }, b);
    CHECK(cc::async_blocking_get_singlethreaded(c) == 20);
}

TEST("async - variadic dependency form unwraps async args")
{
    auto a = cc::make_async_lazy([] { return 3; });
    auto b = cc::make_async_lazy([] { return 4; });

    // c depends on a and b; its function receives plain ints, and runs only once both are ready
    auto c = cc::make_async_lazy([](int x, int y) { return x * y; }, a, b);

    CHECK(cc::async_blocking_get_singlethreaded(c) == 12);
}

TEST("async - variadic dependency form short-circuits on a dependency error")
{
    auto a = cc::make_async_lazy([] { return 3; });
    auto bad = cc::make_async_lazy<int>([](async_context<int>& actx) -> cc::async_step_status
                                        { return actx.error(cc::any_error("boom")); });

    bool ran = false;
    auto c = cc::make_async_lazy(
        [&](int x, int y)
        {
            ran = true;
            return x + y;
        },
        a, bad);

    auto r = cc::try_async_blocking_get_singlethreaded(c);
    CHECK(r.has_error());
    CHECK(!ran);
}

TEST("async - frames may omit the async_context parameter")
{
    // no context, no deps
    auto a = cc::make_async_lazy([] { return 41; });
    CHECK(cc::async_blocking_get_singlethreaded(a) == 41);

    // no context, with a dependency (f gets the plain value)
    auto b = cc::make_async_lazy([](int x) { return x + 1; }, a);
    CHECK(cc::async_blocking_get_singlethreaded(b) == 42);
}

TEST("async - dependency frame may still take a leading async_context")
{
    auto a = cc::make_async_lazy([] { return 10; });

    // f receives the context plus the unwrapped dependency value; a ctx-resolving frame gives its result type
    auto b = cc::make_async_lazy<int>([](async_context<int>& actx, int x) { return actx.success(x * 2); }, a);
    CHECK(cc::async_blocking_get_singlethreaded(b) == 20);
}

// ============================================================================
// dynamic dependencies
// ============================================================================

TEST("async - dynamic dependency added during compute, removed once ready")
{
    // step 0 creates a dependency mid-compute, requires it, and waits; step 1 reads its value. The dependency
    // must be gone from the pending list by the time the parent completes.
    auto p = cc::make_async_lazy<int>(
        [step = 0, child = cc::shared_async<int>()](async_context<int>& actx) mutable -> cc::async_step_status
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

    CHECK(cc::async_blocking_get_singlethreaded(p) == 15);
    CHECK(p->pending_dependency_count() == 0);
}

TEST("async - already-ready dependency completes without parking")
{
    auto dep = cc::make_async_manual<int>();
    dep->push_value(7); // ready before anyone requires it

    auto p = cc::make_async_lazy<int>(
        [dep](async_context<int>& actx) -> cc::async_step_status
        {
            bool const ready = actx.require(dep);
            CHECK(ready); // require on a ready dep returns true immediately
            return actx.success(*dep->value_ptr() + 1);
        });

    CHECK(cc::async_blocking_get_singlethreaded(p) == 8);
    CHECK(p->pending_dependency_count() == 0);
    CHECK(dep->continuation_count() == 0); // never subscribed — the dep was already ready
}

TEST("async - required cold dependency is driven to completion")
{
    // The dependency is a separate cold async captured by the parent. require() neither schedules nor
    // subscribes: the parent's poll loop drives the cold dep inline on its own stack, all within one
    // async_blocking_get_singlethreaded.
    auto dep = cc::make_async_lazy([] { return 100; });
    auto p = cc::make_async_lazy<int>(
        [dep](async_context<int>& actx) -> cc::async_step_status
        {
            if (!actx.require(dep))
                return actx.wait_for_dependencies();
            return actx.success(*dep->value_ptr() + 1);
        });

    CHECK(cc::async_blocking_get_singlethreaded(p) == 101);
}

TEST("async - a reused singlethreaded_scheduler settles empty after each graph")
{
    // A singlethreaded_scheduler has no steal-capable peers, so the poll loop never publishes a dependency it
    // is about to drive inline. Nothing is left queued once the root is ready.
    //
    // This pins a real lifetime invariant, not just churn: a queued entry is a STRONG node handle, so anything
    // abandoned here would pin its whole graph alive for as long as the scheduler lives. When require() still
    // scheduled eagerly, every drive leaked its deps this way — unbounded across a long-lived scheduler, and
    // worth 13-82x on the drive benchmark once the slab stopped recycling hot nodes.
    cc::singlethreaded_scheduler sched;
    cc::async_worker_scope scope(sched);

    for (int i = 0; i < 4; ++i)
    {
        // a chain (single dep per node) and a fan-in (two deps) — the loop drives one dep inline and, with no
        // peers, must not publish the sibling either
        auto leaf = cc::make_async_lazy<int>([i] { return i; });
        auto mid = cc::make_async_lazy([](int x) { return x + 1; }, cc::move(leaf));
        auto other = cc::make_async_lazy<int>([i] { return i * 10; });
        auto root = cc::make_async_lazy([](int a, int b) { return a + b; }, cc::move(mid), cc::move(other));

        root->schedule();
        sched.run_until([&] { return root->is_ready(); });

        CHECK(*root->try_value() == i + 1 + i * 10);
        CHECK(sched.empty()); // nothing abandoned: no node is pinned alive past its graph
    }
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

    CHECK(cc::async_blocking_get_singlethreaded(a) == 7);
    CHECK(*calls == 1);

    // extra scheduling of a completed node must not resurrect and re-invoke the (destroyed) frame
    {
        cc::singlethreaded_scheduler s;
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
        [calls, step = 0, child = cc::shared_async<int>()](async_context<int>& actx) mutable -> cc::async_step_status
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

    CHECK(cc::async_blocking_get_singlethreaded(p) == 1);
    CHECK(*calls == 2);
}

// ============================================================================
// late subscription / external completion
// ============================================================================

TEST("async - blocking on external dep subscribes late, completion wakes it")
{
    cc::singlethreaded_scheduler sched;
    cc::async_worker_scope scope(sched);

    auto ext = cc::make_async_manual<int>();
    auto p = cc::make_async_lazy<int>(
        [ext](async_context<int>& actx) -> cc::async_step_status
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

TEST("async - many dependents park on one node (inline + spill), all woken on completion")
{
    cc::singlethreaded_scheduler sched;
    cc::async_worker_scope scope(sched);

    auto ext = cc::make_async_manual<int>();

    // 5 dependents: fills the 3 inline continuation slots + spills 2 into the slab list
    constexpr int n = 5;
    cc::vector<cc::shared_async<int>> deps;
    for (int i = 0; i < n; ++i)
        deps.push_back(cc::make_async_lazy<int>(
            [ext, i](async_context<int>& actx) -> cc::async_step_status
            {
                if (!actx.require(ext))
                    return actx.wait_for_dependencies();
                return actx.success(*ext->value_ptr() + i);
            }));

    for (auto const& d : deps)
        d->schedule();
    sched.run_until([&] { return false; }); // drain: every dependent parks + subscribes on ext

    CHECK(ext->continuation_count() == n); // 3 inline + 2 spill

    ext->push_value(100); // wakes all n dependents in one completion
    sched.run_until([&] { return false; });

    for (int i = 0; i < n; ++i)
    {
        REQUIRE(deps[i]->is_ready());
        CHECK(*deps[i]->try_value() == 100 + i);
    }
    CHECK(ext->continuation_count() == 0); // continuation head stolen at completion
}

TEST("async - a dependent that expires is pruned from a subscribed-to node")
{
    cc::singlethreaded_scheduler sched;
    cc::async_worker_scope scope(sched);

    auto ext = cc::make_async_manual<int>();
    {
        auto p = cc::make_async_lazy<int>(
            [ext](async_context<int>& actx) -> cc::async_step_status
            {
                if (!actx.require(ext))
                    return actx.wait_for_dependencies();
                return actx.success(*ext->value_ptr());
            });
        p->schedule();
        sched.run_until([&] { return false; }); // p parks + subscribes on ext
        CHECK(ext->continuation_count() == 1);
    } // p's last handle drops here: its node is torn down while still subscribed on ext (weak continuation)

    // completing ext must not touch the torn-down dependent (weak lock fails) — no crash, count sees it gone
    ext->push_value(7);
    CHECK(ext->continuation_count() == 0);
}

// ============================================================================
// error propagation
// ============================================================================

TEST("async - error short-circuits a dependent transform, f never runs")
{
    auto a = cc::make_async_lazy<int>([](async_context<int>& actx) -> cc::async_step_status
                                      { return actx.error(cc::any_error("boom")); });

    bool ran = false;
    auto b = cc::make_async_lazy(
        [&](int x)
        {
            ran = true;
            return x + 1;
        },
        a);

    auto r = cc::try_async_blocking_get_singlethreaded(b);
    CHECK(r.has_error());
    CHECK(!ran);
}

TEST("async - try_async_blocking_get_singlethreaded surfaces a value")
{
    auto a = cc::make_async_lazy([] { return 3; });
    auto r = cc::try_async_blocking_get_singlethreaded(a);
    REQUIRE(r.has_value());
    CHECK(r.value() == 3);
}

TEST("async - cancellation propagates as a value")
{
    auto a = cc::make_async_lazy<int>([](async_context<int>& actx) -> cc::async_step_status
                                      { return actx.error(cc::async_error::make_cancelled()); });

    auto r = cc::try_async_blocking_get_singlethreaded(a);
    REQUIRE(r.has_error());
    CHECK(r.error().is_cancelled());
}

// ============================================================================
// large values (node grows past one cache line; value built in place)
// ============================================================================

TEST("async - a large value grows the node but round-trips through value + dependency paths")
{
    // big_value (96 B) exceeds one line's payload, so the node spans multiple lines. The value is built
    // straight into the payload at resolution (over the moved-out frame's slot) — verify it survives the value
    // read, a manual push, and being unwrapped as a dependency.
    auto a = cc::make_async_lazy(
        []
        {
            big_value v;
            v.data[0] = 7;
            v.data[11] = 42;
            return v;
        });
    auto va = cc::async_blocking_get_singlethreaded(a);
    CHECK(va.data[0] == 7);
    CHECK(va.data[11] == 42);
    REQUIRE(a->try_value() != nullptr);
    CHECK(a->try_value()->data[11] == 42); // zero-copy read of the in-payload value

    // unwrapped as a dependency (by value)
    auto b = cc::make_async_lazy([](big_value x) { return x.data[0] + x.data[11]; }, a);
    CHECK(cc::async_blocking_get_singlethreaded(b) == 49);

    // manual/push path with a large value
    auto m = cc::make_async_manual<big_value>();
    big_value pushed;
    pushed.data[5] = 99;
    m->push_value(pushed);
    REQUIRE(m->try_value() != nullptr);
    CHECK(m->try_value()->data[5] == 99);
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

    CHECK(cc::async_blocking_get_singlethreaded(root) == (cc::i64(1) << depth)); // sum of all leaves == leaf count == 8192

    // each demanded leaf ran exactly once (no completed node recomputed); the orphan never did
    CHECK(*leaf_exec == (cc::i64(1) << depth));
    CHECK(!*orphan_ran);
    CHECK(!orphan->is_ready());
}

TEST("async - deep cold chain completes across the inline depth cap")
{
    // A lazy chain far longer than the eager-drive recursion cap: the first ~cap levels are driven inline
    // depth-first on one stack, then the poll loop falls back to subscribe+park (driven via the scheduler
    // queue) for the rest. The result must be identical either way — this pins that the cap fallback is
    // correct and that the native stack stays bounded regardless of graph depth.
    int const n = 400; // > the internal inline depth cap
    auto node = cc::make_async_lazy<cc::i64>([] { return cc::i64(0); });
    for (int i = 1; i < n; ++i)
        node = cc::make_async_lazy([](cc::i64 x) { return x + 1; }, cc::move(node));

    CHECK(cc::async_blocking_get_singlethreaded(node) == cc::i64(n - 1));
}

// ============================================================================
// born-ready factories (no frame, no scheduling)
// ============================================================================

TEST("async - make_async_from_value is immediately ready and readable")
{
    auto a = cc::make_async_from_value(42);
    REQUIRE(a->is_ready());
    CHECK(a->has_value());
    REQUIRE(a->try_value() != nullptr);
    CHECK(*a->try_value() == 42);
}

TEST("async - make_async_from_error is immediately ready on the failure channel")
{
    auto a = cc::make_async_from_error<int>(cc::async_error::make_error(cc::any_error("boom")));
    REQUIRE(a->is_ready());
    CHECK(a->has_error());
    REQUIRE(a->try_error() != nullptr);
    CHECK(!a->try_error()->is_cancelled());
}

TEST("async - a born-ready value drives a dependent without a scheduler round-trip")
{
    auto a = cc::make_async_from_value(20);
    auto b = cc::make_async_lazy([](int x) { return x + 22; }, a);
    CHECK(cc::async_blocking_get_singlethreaded(b) == 42);
}

TEST("async - a born-ready error short-circuits a dependent transform")
{
    auto a = cc::make_async_from_error<int>(cc::async_error::make_error(cc::any_error("nope")));
    bool ran = false;
    auto b = cc::make_async_lazy(
        [&](int x)
        {
            ran = true;
            return x + 1;
        },
        a);
    auto r = cc::try_async_blocking_get_singlethreaded(b);
    CHECK(r.has_error());
    CHECK(!ran);
}

// ============================================================================
// emplace resolves + immovable T (constructed in place, never moved)
// ============================================================================

namespace
{
// move- and copy-deleted, but constructible: only the emplace paths can produce an async of this
struct immovable
{
    int v;
    explicit immovable(int x) : v(x) {}
    immovable(immovable&&) = delete;
    immovable(immovable const&) = delete;
    immovable& operator=(immovable&&) = delete;
    immovable& operator=(immovable const&) = delete;
};
} // namespace

TEST("async - make_async_from_value_emplace constructs an immovable T in place")
{
    auto a = cc::make_async_from_value_emplace<immovable>(7);
    REQUIRE(a->is_ready());
    REQUIRE(a->try_value() != nullptr);
    CHECK(a->try_value()->v == 7);
}

TEST("async - resolve_to_value_emplace builds an immovable T inside a compute frame")
{
    auto a = cc::make_async_lazy<immovable>([](cc::async_context<immovable>& ctx) -> cc::async_step_status
                                            { return ctx.resolve_to_value_emplace(9); });

    // immovable T cannot be copied out by async_blocking_get_singlethreaded — drive inline and read in place
    cc::singlethreaded_scheduler sched;
    cc::async_worker_scope scope(sched);
    a->schedule();
    sched.run_until([&] { return a->is_ready(); });

    REQUIRE(a->is_ready());
    REQUIRE(a->try_value() != nullptr);
    CHECK(a->try_value()->v == 9);
}

// ============================================================================
// into_result — move the outcome out
// ============================================================================

TEST("async - into_result moves the value out")
{
    auto a = cc::make_async_from_value(cc::string("hello"));
    cc::result<cc::string, cc::async_error> r = cc::into_result(cc::move(a));
    REQUIRE(r.has_value());
    CHECK(r.value() == "hello");
}

TEST("async - into_result surfaces the error out")
{
    auto a = cc::make_async_from_error<int>(cc::async_error::make_cancelled());
    auto r = cc::into_result(cc::move(a));
    REQUIRE(r.has_error());
    CHECK(r.error().is_cancelled());
}

TEST("async - into_result after driving a graph")
{
    auto a = cc::make_async_lazy([] { return 20; });
    auto b = cc::make_async_lazy([](int x) { return x + 22; }, a);
    (void)cc::async_blocking_get_singlethreaded(b); // drive to ready (keeps b alive)
    auto r = cc::into_result(cc::move(b));
    REQUIRE(r.has_value());
    CHECK(r.value() == 42);
}

// ============================================================================
// custom error type E
// ============================================================================

namespace
{
enum class my_err
{
    boom,
    nope,
};

struct str_err // a copyable custom failure type — propagation copies it (async_error is the only re-materialize)
{
    cc::string msg;
};
} // namespace

TEST("async - custom enum error round-trips through resolve / try_error / into_result")
{
    auto a = cc::make_async_lazy<int, my_err>([](cc::async_context<int, my_err>& ctx) -> cc::async_step_status
                                              { return ctx.resolve_to_error(my_err::boom); });

    auto r = cc::try_async_blocking_get_singlethreaded(a);
    REQUIRE(r.has_error());
    CHECK(r.error() == my_err::boom);

    REQUIRE(a->try_error() != nullptr);
    CHECK(*a->try_error() == my_err::boom);
}

TEST("async - custom-E value path returns cc::result<T, E>")
{
    auto a = cc::make_async_lazy<int, my_err>([](cc::async_context<int, my_err>& ctx) -> cc::async_step_status
                                              { return ctx.resolve_to_value(5); });
    cc::result<int, my_err> r = cc::try_async_blocking_get_singlethreaded(a);
    REQUIRE(r.has_value());
    CHECK(r.value() == 5);
}

TEST("async - custom copyable E auto-propagates (copied) through a dependency chain")
{
    auto a = cc::make_async_lazy<int, str_err>([](cc::async_context<int, str_err>& ctx) -> cc::async_step_status
                                               { return ctx.resolve_to_error(str_err{"root failed"}); });

    bool ran = false;
    auto b = cc::make_async_lazy<int, str_err>(
        [&](int x)
        {
            ran = true;
            return x + 1;
        },
        a);

    auto r = cc::try_async_blocking_get_singlethreaded(b);
    REQUIRE(r.has_error());
    CHECK(r.error().msg == "root failed"); // propagated by copy (not re-materialized)
    CHECK(!ran);                           // f was skipped by the auto-propagation short-circuit
}

// ============================================================================
// low-level error handling — the raw frame decides (NO auto-propagation)
// ============================================================================

TEST("async - a raw frame that transforms a dependency error does NOT auto-propagate")
{
    // dep fails; the dependent requires it manually (raw frame), sees try_error(), and RESOLVES TO A VALUE
    // instead of propagating — proving the low-level path leaves the decision to the frame.
    auto dep = cc::make_async_from_error<int>(cc::async_error::make_error(cc::any_error("dep failed")));

    auto p = cc::make_async_lazy<int>(
        [dep](cc::async_context<int>& ctx) -> cc::async_step_status
        {
            if (!ctx.require(dep))
                return ctx.wait_for_dependencies();
            if (dep->try_error() != nullptr)
                return ctx.resolve_to_value(-1); // transform the error into a sentinel value
            return ctx.resolve_to_value(*dep->try_value());
        });

    auto r = cc::try_async_blocking_get_singlethreaded(p);
    REQUIRE(r.has_value());
    CHECK(r.value() == -1); // the frame chose a value; no error propagated
}
