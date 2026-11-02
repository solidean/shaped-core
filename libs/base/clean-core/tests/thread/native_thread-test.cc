#include <clean-core/common/macros.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/native_thread.hh>
#include <clean-core/thread/thread.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
/// Touches `frames` frames of `frame_bytes` each, so it needs about frames * frame_bytes of stack below its caller.
/// The buffer is volatile and summed on the way back up, so no frame can be folded away or turned into a loop.
[[gnu::noinline]] int use_stack(int frames)
{
    constexpr auto frame_bytes = 64 * 1024;
    char volatile buffer[frame_bytes];
    buffer[0] = char(frames);
    buffer[frame_bytes - 1] = char(frames);
    if (frames <= 1)
        return buffer[0];
    return use_stack(frames - 1) + buffer[frame_bytes - 1];
}
} // namespace

TEST("native_thread - start runs the body, and join waits for it")
{
    auto ran = cc::atomic<int>(0);
    auto t = cc::native_thread::start([&] { ran.fetch_add(1); });
    CHECK(t.joinable());
    t.join();
    CHECK(!t.joinable());
    CHECK(ran.load() == 1);
}

TEST("native_thread - a default-constructed or moved-from thread is not joinable")
{
    auto empty = cc::native_thread();
    CHECK(!empty.joinable());

    auto t = cc::native_thread::start([] {});
    auto moved = cc::move(t);
    CHECK(!t.joinable()); // NOLINT(bugprone-use-after-move): moved-from is the state under test
    CHECK(moved.joinable());

    // Assigning into a joined (or never-started) thread is fine; it is a joinable one that must not be overwritten.
    empty = cc::move(moved);
    CHECK(empty.joinable());
    empty.join();
}

TEST("native_thread - the body runs on a thread of its own")
{
    auto body_thread = cc::thread_id::invalid;
    auto t = cc::native_thread::start([&] { body_thread = cc::current_thread_id(); });
    t.join();
    CHECK(body_thread != cc::thread_id::invalid);
    if (CC_HAS_THREADS)
        CHECK(body_thread != cc::current_thread_id());
    else
        CHECK(body_thread == cc::current_thread_id()); // without threads the body runs inline, on this thread
}

// The point of cc::native_thread: the default stack is 8 MiB everywhere, not the platform's.
// About 2 MiB of frames here overflows a std::thread on macOS (512 KiB) and on Windows (1 MiB), and fits on Linux.
// ASan widens every frame, which the margin below 8 MiB absorbs.
TEST("native_thread - the default stack holds a recursion past macOS's and Windows' own defaults")
{
#ifdef __EMSCRIPTEN__
    SKIP("Emscripten keeps its own default stack: a pthread's stack is carved from linear memory");
#endif
    auto sum = 0;
    auto t = cc::native_thread::start([&] { sum = use_stack(32); });
    t.join();
    CHECK(sum != 0);
}

TEST("native_thread - an explicit stack size is honoured, including one below the platform minimum")
{
    auto big = 0;
    auto t
        = cc::native_thread::start([&] { big = use_stack(32); }, {.name = "big stack", .stack_bytes = isize(16) << 20});
    t.join();
    CHECK(big != 0);

    // A size below PTHREAD_STACK_MIN is rounded up rather than refused.
    auto ran = false;
    auto small = cc::native_thread::start([&] { ran = true; }, {.stack_bytes = 1});
    small.join();
    CHECK(ran);
}
