#include <clean-core/common/utility.hh>
#include <clean-core/error/exception.hh>
#include <clean-core/math/random.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <clean-core/thread/thread.hh>
#include <nexus/async-test.hh>
#include <nexus/fuzz/async.hh>
#include <nexus/test.hh>

#include <typeindex>

// Async operations: an op returning cc::shared_async<T> is awaited by the engine, and T is what reaches its slot.

TEST("fuzz async - an op returning shared_async<T> is async and produces T")
{
    auto t = nx::fuzz::test::create();
    auto* const produce = t->add_op("produce", []() -> cc::shared_async<int> { co_return 7; });
    auto* const settle = t->add_op("settle", [](int) -> cc::shared_async<cc::unit> { co_return; });
    auto* const plain = t->add_op("plain", [](int a) { return a + 1; });

    CHECK(produce->is_async());
    CHECK(produce->return_type() == std::type_index(typeid(int)));
    CHECK(!produce->returns_void());

    // shared_async<cc::unit> is the async spelling of a void op: nothing reaches a slot.
    CHECK(settle->is_async());
    CHECK(settle->returns_void());

    CHECK(!plain->is_async());
}

TEST("fuzz async - the synchronous entry points refuse a machine holding an async op")
{
    auto t = nx::fuzz::test::create();
    t->add_value("x", 1);
    t->add_op("inc", [](int a) { return a + 1; });
    t->add_op("wait", [](int) -> cc::shared_async<cc::unit> { co_return; });
    t->add_op("load", [](int a) -> cc::shared_async<int> { co_return a; });

    auto const res = t->execute_fuzzer(1);
    CHECK(!res.is_ok);
    CHECK(!res.failing_run.has_value()); // a setup error, not a finding
    CHECK(res.error_message.contains("'wait' and 'load' are async"));
    CHECK(res.error_message.contains("execute_fuzz_test_async"));
    CHECK(!t->execute_fuzz_test());
}

// The divert behind async steps: it follows the test rather than the thread, so work on a pool worker is caught too.

ASYNC_TEST("fuzz async - a divert takes a failing CHECK on a pool worker off the test")
{
    auto sink = nx::impl::async_check_capture_sink();
    {
        auto const divert = nx::impl::scoped_test_check_divert(sink);
        auto const one = 1;
        auto const child = cc::make_async_scheduled_on(cc::compute_scheduler(),
                                                       [one]
                                                       {
                                                           CHECK(one == 2);
                                                           return cc::unit{};
                                                       });
        co_await cc::async_settled(child);
        CHECK(child->has_value()); // diverted too, so it cannot fail this test either way
    }

    // The failure reached the sink and nowhere else: this test passes.
    CHECK(sink.failed.load() == 1);
    CHECK(sink.executed.load() == 2);
    CHECK(!sink.require_failed.load());
    sink.first_message.lock([](cc::string& m) { CHECK(m.contains("one == 2")); });
}

ASYNC_TEST("fuzz async - a divert tallies strands checking at once")
{
    auto sink = nx::impl::async_check_capture_sink();
    {
        auto const divert = nx::impl::scoped_test_check_divert(sink);
        auto const strand = [](int v)
        {
            return cc::make_async_scheduled_on(cc::compute_scheduler(),
                                               [v]
                                               {
                                                   for (auto i = 0; i < 100; ++i)
                                                       CHECK(v == i);
                                                   return cc::unit{};
                                               });
        };
        co_await cc::async_all(strand(3), strand(5));
    }

    CHECK(sink.executed.load() == 200);
    CHECK(sink.failed.load() == 198);
}

#if CC_ASSERT_ENABLED
ASYNC_TEST("fuzz async - a diverted CC_ASSERT fails the node that asserted, with its message")
{
    auto sink = nx::impl::async_check_capture_sink();
    auto const child = cc::make_async_lazy_on(cc::compute_scheduler(),
                                              []
                                              {
                                                  CC_ASSERT(false, "the divert catches this");
                                                  return cc::unit{};
                                              });
    {
        auto const divert = nx::impl::scoped_test_check_divert(sink);
        co_await cc::async_settled(child);
    }

    REQUIRE(child->has_error());
    CHECK(child->try_error()->underlying().to_string().contains("the divert catches this"));
    CHECK(sink.require_failed.load());
    CHECK(sink.failed.load() == 1);
}
#endif

// The awaited driver: generation, invariants, shrinking and placement over a mix of sync and async ops.

namespace
{
// Finds the first seed whose program fails, awaiting async ops.
cc::shared_async<nx::fuzz::test::fuzz_result> find_failing_async(nx::fuzz::test* t, int max_seed)
{
    for (auto s = 1; s <= max_seed; ++s)
    {
        auto res = co_await cc::async_take(t->execute_fuzzer_async(s));
        if (!res.is_ok && res.failing_run.has_value())
            co_return res;
    }
    co_return nx::fuzz::test::fuzz_result{};
}
} // namespace

ASYNC_TEST("fuzz async - a mix of sync and async ops passes")
{
    auto test = nx::fuzz::test::create();
    test->add_value("0", 0);
    test->add_op("inc", [](int a) { return a % 1000 + 1; });
    test->add_op("double elsewhere", [](int a) -> cc::shared_async<int>
                 { co_return co_await cc::async_run_on(cc::compute_scheduler(), [a] { return a % 1000 * 2; }); });
    test->add_op("settle",
                 [](int& a) -> cc::shared_async<cc::unit>
                 {
                     co_await cc::async_yield();
                     a = a % 1000;
                     co_return;
                 });
    test->add_invariant("non-negative", [](int i) { return i >= 0; });
    test->add_invariant("non-negative, awaited", [](int i) -> cc::shared_async<bool> { co_return i >= 0; });
    test->cap_seed_count(8);

    CHECK(co_await test->execute_fuzz_test_async());
}

ASYNC_TEST("fuzz async - a failing async op is found, shrunk and replayed")
{
    auto t = nx::fuzz::test::create();
    t->add_value("3", 3);
    t->add_op("add1 later", [](int a) -> cc::shared_async<int> { co_return a + 1; });
    t->add_invariant("is-not-7", [](int i) { return i != 7; });

    auto res = co_await cc::async_take(find_failing_async(t.get(), 64));
    REQUIRE(res.failing_run.has_value());
    CHECK(res.error_message.contains("invariant violated"));

    auto rng = cc::random(1u);
    auto const minimized = co_await cc::async_take(res.failing_run.value().minimize_async(rng, nullptr));

    // value "3" + 4x add1 (3->4->5->6->7) + the failing is-not-7 check, as in the synchronous engine
    CHECK(int(minimized.operations.size()) == 6);
    auto const replay = co_await minimized.replay_async(nullptr);
    CHECK(replay.is_failing());
}

ASYNC_TEST("fuzz async - a CHECK failing on a pool worker fails its step, not the test")
{
    auto t = nx::fuzz::test::create();
    t->add_value("0", 0);
    t->add_op("inc", [](int a) { return a + 1; });
    t->add_op("check elsewhere",
              [](int a) -> cc::shared_async<cc::unit>
              {
                  co_await cc::async_run_on(cc::compute_scheduler(),
                                            [a]
                                            {
                                                CHECK(a != 3);
                                                return cc::unit{};
                                            });
              });

    auto res = co_await cc::async_take(find_failing_async(t.get(), 64));
    REQUIRE(res.failing_run.has_value());
    CHECK(res.error_message.contains("a != 3"));

    // Shrinking replays the failing program over and over; none of it may reach this test, which passes.
    auto rng = cc::random(2u);
    auto const minimized = co_await cc::async_take(res.failing_run.value().minimize_async(rng, nullptr));
    CHECK(minimized.operations.size() >= 2);
}

ASYNC_TEST("fuzz async - an async op that throws fails its step with the message")
{
    auto t = nx::fuzz::test::create();
    t->add_value("x", 0);
    t->add_op("boom later",
              [](int) -> cc::shared_async<cc::unit>
              {
                  co_await cc::async_yield();
                  throw std::runtime_error("boom from a coroutine");
              });

    auto const res = co_await cc::async_take(t->execute_fuzzer_async(1));
    CHECK(!res.is_ok);
    REQUIRE(res.failing_run.has_value());
    CHECK(res.error_message.contains("boom from a coroutine"));
}

ASYNC_TEST("fuzz async - the awaited entry point runs an all-sync machine too")
{
    auto t = nx::fuzz::test::create();
    t->add_value("0", 0);
    t->add_op("inc", [](int a) { return a % 100 + 1; });
    t->add_invariant("positive-or-zero", [](int i) { return i >= 0; });
    t->cap_seed_count(4);

    CHECK(co_await t->execute_fuzz_test_async());
}

ASYNC_TEST("fuzz async - set_inherit_home runs every op on the caller's home", main_thread)
{
    auto const home_thread = cc::current_thread_id();
    auto sync_elsewhere = 0;
    auto async_elsewhere = 0;

    auto t = nx::fuzz::test::create();
    t->set_inherit_home(true);
    t->add_value("0", 0);
    t->add_op("sync",
              [&](int a)
              {
                  sync_elsewhere += cc::current_thread_id() != home_thread ? 1 : 0;
                  return a % 100 + 1;
              });
    t->add_op("async",
              [&](int a) -> cc::shared_async<int>
              {
                  async_elsewhere += cc::current_thread_id() != home_thread ? 1 : 0;
                  co_await cc::async_yield();
                  async_elsewhere += cc::current_thread_id() != home_thread ? 1 : 0;
                  co_return a;
              });
    t->cap_seed_count(4);

    CHECK(co_await t->execute_fuzz_test_async());
    CHECK(sync_elsewhere == 0);
    CHECK(async_elsewhere == 0);
}

ASYNC_TEST("fuzz async - set_inherit_home does nothing for a caller in no home")
{
    auto t = nx::fuzz::test::create();
    t->set_inherit_home(true);
    t->add_value("0", 0);
    t->add_op("async", [](int a) -> cc::shared_async<int> { co_return a % 100 + 1; });
    t->cap_seed_count(4);

    CHECK(co_await t->execute_fuzz_test_async());
}
