#include <clean-core/thread/thread.hh>
#include <nexus/test.hh>

#include <thread>

// Thread naming is best-effort with no readback API, so we just exercise the call paths (current
// thread, a spawned thread, and a name past the Linux 15-byte cap) and confirm nothing crashes.
TEST("thread - set_current_thread_name is callable")
{
    cc::set_current_thread_name("nexus-main");

    std::thread t(
        []
        {
            cc::set_current_thread_name("worker");
            cc::set_current_thread_name("a-very-long-thread-name-past-the-limit");
        });
    t.join();

    CHECK(true);
}
