#include <clean-core/common/macros.hh>
#include <clean-core/thread/thread.hh>
#include <nexus/test.hh>

#if CC_HAS_THREADS
#include <thread>
#endif

// Thread naming is best-effort with no readback API, so we just exercise the call paths and confirm
// nothing crashes. The main-thread call is valid everywhere (a no-op where naming is unavailable);
// the spawned-thread part only compiles/runs where real threads exist.
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
