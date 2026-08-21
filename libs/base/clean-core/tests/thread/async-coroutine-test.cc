#include <clean-core/container/vector.hh>
#include <clean-core/error/exception.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <clean-core/thread/async_thread_pool.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// The co_await layer, driven inline on the calling thread like the rest of async-test.cc.
// What is pinned here is the SEMANTICS — short-circuit, failure, laziness, teardown — not the scheduling, which the pool tests own.

// The coroutine's own frame is a single handle, so it never falls back to the boxed cc::unique_function.
// A spilled frame would be an allocation per task on top of the coroutine's own, which is exactly what this layer must not cost.
static_assert(
    cc::async<int>::frame_fits_inline<cc::impl::async_coro_frame<cc::impl::async_promise<int, cc::async_error, true>>>,
    "a coroutine frame must stay inline in the node");
static_assert(sizeof(cc::async<int>) == 64, "the coroutine layer must not grow the node");

namespace
{
cc::shared_async<int> coro_constant(int v)
{
    co_return v;
}

cc::shared_async<int> coro_plus(cc::shared_async<int> dep, int add)
{
    auto const& x = co_await dep;
    co_return x + add;
}

cc::shared_async<int> coro_failing()
{
    co_await cc::async_fail("boom");
    co_return 1; // never reached
}

cc::shared_async<cc::unit> coro_unit(int* counter)
{
    ++*counter;
    co_return;
}

cc::async_scheduled<int> coro_eager(int v)
{
    co_return v;
}

/// Records its own destruction, so a test can tell "the coroutine was destroyed" from "it merely never ran".
struct destruction_probe
{
    int* count = nullptr;

    explicit destruction_probe(int* c) : count(c) {}
    destruction_probe(destruction_probe&& r) noexcept : count(cc::exchange(r.count, nullptr)) {}
    destruction_probe(destruction_probe const&) = delete;
    destruction_probe& operator=(destruction_probe&&) = delete;
    destruction_probe& operator=(destruction_probe const&) = delete;

    ~destruction_probe()
    {
        if (count != nullptr)
            ++*count;
    }
};

cc::shared_async<int> make_failed()
{
    return cc::make_async_from_error<int>(cc::async_error::make_error(cc::any_error("dependency failed")));
}
} // namespace

// ============================================================================
// the basics
// ============================================================================

TEST("async coroutine - a coroutine with no await resolves")
{
    CHECK(cc::async_blocking_get(coro_constant(42)) == 42);
}

TEST("async coroutine - awaiting a ready dependency never suspends")
{
    auto const a = cc::make_async_from_value(20);
    REQUIRE(a->is_ready());
    CHECK(cc::async_blocking_get(coro_plus(a, 22)) == 42);
}

TEST("async coroutine - awaiting a cold dependency drives it")
{
    auto const a = cc::make_async_lazy([] { return 20; });
    CHECK(cc::async_blocking_get(coro_plus(a, 22)) == 42);
}

TEST("async coroutine - a chain of coroutines composes")
{
    auto a = coro_constant(1);
    auto b = coro_plus(cc::move(a), 10);
    auto c = coro_plus(cc::move(b), 100);
    CHECK(cc::async_blocking_get(c) == 111);
}

TEST("async coroutine - composes with lambda nodes in both directions")
{
    // a lambda node depending on a coroutine
    auto const co = coro_constant(4);
    auto const via_lambda = cc::make_async_lazy([](int x) { return x * 10; }, co);
    CHECK(cc::async_blocking_get(via_lambda) == 40);

    // a coroutine depending on a lambda node
    auto const lam = cc::make_async_lazy([] { return 7; });
    CHECK(cc::async_blocking_get(coro_plus(lam, 1)) == 8);
}

TEST("async coroutine - a cc::unit coroutine uses a bare co_return")
{
    auto ran = 0;
    auto const a = coro_unit(&ran);
    auto const r = cc::try_async_blocking_get(a);
    CHECK(r.has_value());
    CHECK(ran == 1);
}

// ============================================================================
// suspension on external work
// ============================================================================

TEST("async coroutine - parks on a manual dependency and resumes after the push")
{
    auto const ext = cc::make_async_manual<int>();
    auto const co = coro_plus(ext, 2);

    cc::singlethreaded_scheduler sched;
    cc::async_worker_scope scope(sched);

    co->schedule();
    sched.drain();
    CHECK(!co->is_ready()); // parked on the unpushed manual node

    ext->push_value(40);
    sched.drain();

    REQUIRE(co->is_ready());
    CHECK(*co->try_value() == 42);
}

TEST("async coroutine - async_yield reschedules and resumes")
{
    auto steps = 0;
    auto const co = [](int* s) -> cc::shared_async<int>
    {
        ++*s;
        co_await cc::async_yield();
        ++*s;
        co_return *s;
    }(&steps);

    CHECK(steps == 0); // cold: creating the coroutine ran none of its body
    CHECK(cc::async_blocking_get(co) == 2);
    CHECK(steps == 2);
}

// ============================================================================
// read-only views as dependencies
// ============================================================================

// A coroutine PRODUCES its value, so it can never return a view — see the static_asserts on std::coroutine_traits.
// Not compilable, pinned as prose:
//   cc::shared_async<int const> bad_cold() { co_return 1; }
//   cc::async_scheduled<int const> bad_eager() { co_return 1; }

TEST("async coroutine - co_await over a read-only view")
{
    auto const src = cc::make_async_lazy([] { return 40; });
    cc::shared_async<int const> const v = src;

    auto const co = [](cc::shared_async<int const> dep) -> cc::shared_async<int>
    {
        auto const& x = co_await dep; // U deduces to int const, so this binds int const&
        static_assert(std::is_same_v<decltype(x), int const&>);
        co_return x + 2;
    }(v);

    CHECK(cc::async_blocking_get(co) == 42);
}

TEST("async coroutine - async_as_result over a read-only view strips the const")
{
    cc::shared_async<int const> const v = cc::make_async_from_value(7);

    auto const co = [](cc::shared_async<int const> dep) -> cc::shared_async<int>
    {
        auto r = co_await cc::async_as_result(dep);
        static_assert(std::is_same_v<decltype(r), cc::result<int, cc::async_error>>);
        co_return r.has_value() ? r.value() : -1;
    }(v);

    CHECK(cc::async_blocking_get(co) == 7);
}

TEST("async coroutine - an eager coroutine's handle converts to a view")
{
    // async_scheduled carries its own operator to the view: reaching it through shared_async would be
    // two user-defined conversions, which copy-initialization does not allow.
    cc::shared_async<int const> const v = coro_eager(5);
    CHECK(cc::async_blocking_get(v) == 5);
}

// ============================================================================
// failure
// ============================================================================

TEST("async coroutine - a failed dependency short-circuits the rest of the body")
{
    auto after_await = 0;
    auto destroyed = 0;

    auto const co = [](cc::shared_async<int> dep, int* after, int* destroyed) -> cc::shared_async<int>
    {
        auto const probe = destruction_probe(destroyed);
        auto const& v = co_await dep; // fails here
        ++*after;
        co_return v;
    }(make_failed(), &after_await, &destroyed);

    auto const r = cc::try_async_blocking_get(co);
    CHECK(r.has_error());

    CHECK(after_await == 0); // the body after the failed await never ran
    CHECK(destroyed == 1);   // but its in-scope local WAS destroyed
}

TEST("async coroutine - a dependency that fails while parked short-circuits too")
{
    auto after_await = 0;
    auto const ext = cc::make_async_manual<int>();

    auto const co = [](cc::shared_async<int> dep, int* after) -> cc::shared_async<int>
    {
        auto const& v = co_await dep;
        ++*after;
        co_return v;
    }(ext, &after_await);

    cc::singlethreaded_scheduler sched;
    cc::async_worker_scope scope(sched);

    co->schedule();
    sched.drain();
    CHECK(!co->is_ready());

    ext->push_error(cc::async_error::make_error(cc::any_error("late failure")));
    sched.drain();

    REQUIRE(co->has_error());
    CHECK(after_await == 0);
}

TEST("async coroutine - async_fail resolves on the failure channel")
{
    auto const r = cc::try_async_blocking_get(coro_failing());
    CHECK(r.has_error());
}

TEST("async coroutine - co_return cc::error resolves on the failure channel")
{
    auto const co = []() -> cc::shared_async<int> { co_return cc::error("returned an error"); }();

    auto const r = cc::try_async_blocking_get(co);
    CHECK(r.has_error());
}

TEST("async coroutine - an escaped exception becomes the node's error")
{
    auto const co = []() -> cc::shared_async<int>
    {
        throw std::runtime_error("thrown from a coroutine");
        co_return 1;
    }();

    auto const r = cc::try_async_blocking_get(co);
    REQUIRE(r.has_error());
    CHECK(!r.error().is_cancelled()); // a throw is a failure, never a cancellation
}

TEST("async coroutine - async_as_result hands the failure to the body instead")
{
    auto const co = [](cc::shared_async<int> dep) -> cc::shared_async<int>
    {
        auto r = co_await cc::async_as_result(dep);
        co_return r.has_error() ? -1 : r.value();
    }(make_failed());

    CHECK(cc::async_blocking_get(co) == -1);
}

TEST("async coroutine - async_settled waits without short-circuiting")
{
    auto const co = [](cc::shared_async<int> dep) -> cc::shared_async<int>
    {
        co_await cc::async_settled(dep);
        co_return dep->has_error() ? -2 : *dep->try_value();
    }(make_failed());

    CHECK(cc::async_blocking_get(co) == -2);
}

// ============================================================================
// fan-out
// ============================================================================

TEST("async coroutine - async_all awaits a pack, then each read is free")
{
    auto const co = []() -> cc::shared_async<int>
    {
        auto const a = cc::make_async_lazy([] { return 1; });
        auto const b = cc::make_async_lazy([] { return 2; });
        auto const c = cc::make_async_lazy([] { return 3; });

        co_await cc::async_all(a, b, c);
        co_return co_await a + co_await b + co_await c;
    }();

    CHECK(cc::async_blocking_get(co) == 6);
}

TEST("async coroutine - async_all short-circuits on a failed member")
{
    auto reached = 0;
    auto const co = [](cc::shared_async<int> bad, int* reached) -> cc::shared_async<int>
    {
        auto const ok = cc::make_async_from_value(1);
        co_await cc::async_all(ok, bad);
        ++*reached;
        co_return 0;
    }(make_failed(), &reached);

    auto const r = cc::try_async_blocking_get(co);
    CHECK(r.has_error());
    CHECK(reached == 0);
}

TEST("async coroutine - async_all over a span")
{
    auto const co = []() -> cc::shared_async<int>
    {
        auto deps = cc::vector<cc::shared_async<int>>();
        for (auto i = 0; i < 5; ++i)
            deps.push_back(cc::make_async_lazy([i] { return i; }));

        co_await cc::async_all(cc::span<cc::shared_async<int> const>(deps));

        auto sum = 0;
        for (auto const& d : deps)
            sum += *d->try_value();
        co_return sum;
    }();

    CHECK(cc::async_blocking_get(co) == 0 + 1 + 2 + 3 + 4);
}

TEST("async coroutine - async_all requires every dependency before parking")
{
    auto const e0 = cc::make_async_manual<int>();
    auto const e1 = cc::make_async_manual<int>();

    auto const co = [](cc::shared_async<int> a, cc::shared_async<int> b) -> cc::shared_async<int>
    {
        co_await cc::async_all(a, b);
        co_return co_await a + co_await b;
    }(e0, e1);

    cc::singlethreaded_scheduler sched;
    cc::async_worker_scope scope(sched);

    co->schedule();
    sched.drain();

    // one park, on BOTH dependencies — not a walk that only ever knows about the first
    CHECK(co->pending_dependency_count() == 2);

    e0->push_value(30);
    sched.drain();
    CHECK(!co->is_ready());

    e1->push_value(12);
    sched.drain();

    REQUIRE(co->is_ready());
    CHECK(*co->try_value() == 42);
}

// ============================================================================
// eager vs lazy, and teardown
// ============================================================================

TEST("async coroutine - the plain return type is cold, async_scheduled is not")
{
    cc::singlethreaded_scheduler sched;
    cc::async_worker_scope scope(sched);

    // calling a coroutine that returns shared_async runs NOTHING — the same contract make_async_lazy has
    auto const lazy = coro_constant(1);
    CHECK(lazy->is_cold());

    auto const eager = coro_eager(2);
    CHECK(!eager->is_cold());

    sched.drain();
    CHECK(lazy->is_cold()); // nothing required it, so nothing ran it
    REQUIRE(eager->is_ready());
    CHECK(*eager->try_value() == 2);

    CHECK(cc::async_blocking_get_on(sched, lazy) == 1);
}

TEST("async coroutine - async_start schedules a cold coroutine and is idempotent")
{
    cc::singlethreaded_scheduler sched;
    cc::async_worker_scope scope(sched);

    auto const a = cc::async_start(coro_constant(7));
    CHECK(!a->is_cold());

    (void)cc::async_start(a); // already scheduled: a no-op, not a second enqueue
    sched.drain();

    REQUIRE(a->is_ready());
    CHECK(*a->try_value() == 7);

    (void)cc::async_start(a); // and on a ready node too
    CHECK(a->is_ready());
}


TEST("async coroutine - a started coroutine runs even after its handle is dropped")
{
    cc::singlethreaded_scheduler sched;
    cc::async_worker_scope scope(sched);

    auto ran = 0;
    {
        auto const dropped = cc::async_start(coro_unit(&ran));
    }
    sched.drain();

    // the schedule queue held the node, so dropping the handle is not a cancellation — we are cooperative
    CHECK(ran == 1);
}

TEST("async coroutine - a cold coroutine whose handle is dropped never runs")
{
    cc::singlethreaded_scheduler sched;
    cc::async_worker_scope scope(sched);

    auto ran = 0;
    {
        auto const dropped = coro_unit(&ran);
    }
    sched.drain();

    CHECK(ran == 0); // nothing required it and nothing started it
}

TEST("async coroutine - destroying a never-run coroutine destroys its parameters")
{
    auto destroyed = 0;
    {
        auto const lazy = [](destruction_probe) -> cc::shared_async<int> { co_return 1; }(destruction_probe(&destroyed));
        CHECK(lazy->is_cold());
        CHECK(destroyed == 0); // the parameter lives in the coroutine frame, which the node owns
    }
    CHECK(destroyed == 1);
}

TEST("async coroutine - a coroutine parked forever is torn down with the node")
{
    auto destroyed = 0;
    {
        auto const ext = cc::make_async_manual<int>();
        auto const co = [](cc::shared_async<int> dep, int* destroyed) -> cc::shared_async<int>
        {
            auto const probe = destruction_probe(destroyed);
            co_return co_await dep;
        }(ext, &destroyed);

        cc::singlethreaded_scheduler sched;
        cc::async_worker_scope scope(sched);
        co->schedule();
        sched.drain();
        CHECK(!co->is_ready()); // suspended on a push that never comes
        CHECK(destroyed == 0);
    }
    CHECK(destroyed == 1); // the frame destroys the suspended coroutine, which destroys its locals
}

// ============================================================================
// on a real pool
// ============================================================================

namespace
{
// A balanced sum tree built out of coroutines: depth d has 2^d leaves, so a correct drive returns 2^d.
// The children are COLD when created, so async_all is what fans them out: it requires both before parking, and the poll loop then publishes one for a peer to steal.
// Awaiting them one after the other instead would run the whole tree on one thread.
cc::shared_async<i64> coro_sum_tree(int depth)
{
    if (depth == 0)
        co_return i64(1);

    auto const left = coro_sum_tree(depth - 1);
    auto const right = coro_sum_tree(depth - 1);

    co_await cc::async_all(left, right);
    co_return co_await left + co_await right;
}
} // namespace

TEST("async coroutine - a coroutine fan-out tree is correct on one thread")
{
    CHECK(cc::async_blocking_get(coro_sum_tree(8)) == 256);
}

// Not gated on CC_HAS_THREADS: the pool exists everywhere and falls back to driving inline, and the answer must be the same either way.
TEST("async coroutine - a coroutine fan-out tree is correct on a pool")
{
    cc::async_thread_pool pool;
    CHECK(cc::async_blocking_get_on(pool, coro_sum_tree(10)) == 1024);
}
