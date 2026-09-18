#include <clean-core/common/utility.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
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
