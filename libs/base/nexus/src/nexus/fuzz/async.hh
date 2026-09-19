#pragma once

// Async operations for nx::fuzz: an op returning cc::shared_async<T> is awaited by the engine, one step at a time.
//
// A separate header because it is the one fuzz header that needs the coroutine machinery, and nexus/fuzz/fuzz.hh stays free of it.
// Include it wherever an async op is registered.
//
//   #include <nexus/async-test.hh>
//   #include <nexus/fuzz/async.hh>
//
//   ASYNC_TEST("cache - survives random sequences")
//   {
//       auto test = nx::fuzz::test::create();
//       test->add_op("mk", [] { return cache(); });
//       test->add_op("load", [](cache& c) -> cc::shared_async<entry> { co_return co_await c.load("a"); });
//       SECTION("fuzz") { CHECK(co_await test->execute_fuzz_test_async()); }
//   }
//
// See libs/base/nexus/docs/fuzz-testing.md, "Async operations".

#include <clean-core/common/utility.hh> // cc::unit
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/fuzz/fuzz.hh>

#include <type_traits>

namespace nx::fuzz::impl
{
// Unboxes a resolved value as T; T must be copyable, as for eval_to.
template <class T>
cc::shared_async<T> unbox_async(cc::shared_async<typed_value> boxed)
{
    co_return (co_await cc::async_take(cc::move(boxed))).template get<T>();
}
} // namespace nx::fuzz::impl

template <class... Args>
auto nx::fuzz::test::eval_op_async(cc::string_view op, Args&&... args) const
{
    auto* const home = inherited_home();
    return cc::async_take(impl::place(async_op_or_fail(op)->eval_async(home, cc::forward<Args>(args)...), home));
}

template <class T, class... Args>
auto nx::fuzz::test::eval_op_to_async(cc::string_view op, Args&&... args) const
{
    auto* const home = inherited_home();
    auto boxed = impl::place(async_op_or_fail(op)->eval_async(home, cc::forward<Args>(args)...), home);
    return cc::async_take(impl::place(impl::unbox_async<T>(cc::move(boxed)), home));
}

template <class... Args>
auto nx::fuzz::test::eval_op_bool_async(cc::string_view op, Args&&... args) const
{
    return eval_op_to_async<bool>(op, cc::forward<Args>(args)...);
}

/// Boxes an async op's value once it resolves.
/// The value is taken out of the op's node, so a handle the op kept for itself afterwards reads a moved-from value.
template <class T>
struct nx::fuzz::impl::async_op_glue
{
    static cc::shared_async<typed_value> box(cc::shared_async<T> op)
    {
        if constexpr (std::is_same_v<T, cc::unit>)
        {
            (void)co_await op;
            co_return typed_value();
        }
        else
        {
            co_return typed_value::create(co_await cc::async_take(cc::move(op)));
        }
    }
};
