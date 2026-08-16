#include <clean-core/common/macros.hh>
#include <clean-core/thread/thread.hh>
#include <nexus/test.hh>

#if CC_HAS_THREADS
#include <thread>
#endif

using namespace cc::primitive_defines;

// Thread naming is best-effort with no readback API, so we just exercise the call paths and confirm nothing crashes.
// The main-thread call is valid everywhere, a no-op where naming is unavailable.
// The spawned-thread part only compiles and runs where real threads exist.
TEST("thread - set_current_thread_name is callable")
{
    cc::set_current_thread_name("nexus-main");

#if CC_HAS_THREADS
    std::thread t(
        []
        {
            cc::set_current_thread_name("worker");
            cc::set_current_thread_name("a-very-long-thread-name-past-the-limit");
        });
    t.join();
#endif

    CHECK(true);
}

TEST("thread - current_thread_id is stable per thread and distinct across them")
{
    // nexus claims cc::thread_id::main from nx::run, so this test body — which runs on it — sees exactly that.
    auto const mine = cc::current_thread_id();
    CHECK(mine != cc::thread_id::invalid);
    CHECK(cc::current_thread_id() == mine);

#if CC_HAS_THREADS
    auto other = cc::thread_id::invalid;
    auto other_again = cc::thread_id::invalid;
    std::thread t(
        [&]
        {
            other = cc::current_thread_id();
            other_again = cc::current_thread_id();
        });
    t.join();

    CHECK(other != cc::thread_id::invalid);
    CHECK(other != cc::thread_id::main); // main is reserved for whoever claimed it
    CHECK(other == other_again);
    CHECK(other != mine);
#endif
}
