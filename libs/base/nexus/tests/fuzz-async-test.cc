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
