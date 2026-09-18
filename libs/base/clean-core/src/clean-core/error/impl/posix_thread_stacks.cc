#include "posix_thread_stacks.hh"

#include <clean-core/common/macros.hh>

#if defined(__linux__) && !defined(__EMSCRIPTEN__)

#include <clean-core/platform/stack_capture.hh>
#include <clean-core/platform/symbolize.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/thread/atomic.hh>
#include <dirent.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

using namespace cc::primitive_defines;

namespace
{
/// The signal a thread is asked to report on.
///
/// A real-time signal rather than SIGUSR1/2, which applications and libraries claim routinely, or SIGPROF, which a
/// profiler owns.
/// SIGRTMIN is a function on glibc rather than a constant, so this is resolved at install time.
int g_signal = 0;

/// The deepest stack one thread reports.
/// Fixed, because the slot is reserved before the process is in trouble and may not grow afterwards.
constexpr isize max_frames = 64;

/// How many threads a report covers.
/// A process with more is reported for the first of them, which loses a name rather than growing a table inside a
/// crash handler.
constexpr isize max_threads = 128;

/// What one asked thread is doing.
enum class slot_state : int
{
    idle = 0,
    requested, ///< the signal has been sent and the handler has not answered yet
    answered,  ///< frames below are this thread's
};

struct thread_slot
{
    /// The kernel thread id this slot belongs to while a request is outstanding.
    cc::atomic<i32> tid = {0};

    cc::atomic<int> state = {int(slot_state::idle)};

    void* frames[max_frames] = {};
    cc::atomic<i32> frame_count = {0};
};

thread_slot g_slots[max_threads];

/// Posted by the handler, waited on by the collector.
/// One rather than one per slot: the collector asks a single thread at a time, which keeps the handler's work to a
/// slot write and a post.
sem_t g_answered;

cc::atomic<bool> g_installed = false;

[[nodiscard]] i32 current_tid()
{
    return i32(::syscall(SYS_gettid));
}

/// The handler every asked thread runs.
///
/// **Async-signal-safe, and that is a requirement rather than a preference.**
/// cc::capture_stack is documented allocation-free and lock-free; sem_post is on the POSIX list; nothing else here
/// touches anything a signal could have interrupted.
void report_own_stack(int) noexcept
{
    auto const self = current_tid();

    for (auto& slot : g_slots)
    {
        if (slot.state.load(cc::memory_order_acquire) != int(slot_state::requested))
            continue;
        if (slot.tid.load(cc::memory_order_relaxed) != self)
            continue;

        auto const captured = cc::capture_stack(cc::span<void*>(slot.frames, max_frames), 1);
        slot.frame_count.store(i32(captured.count), cc::memory_order_relaxed);

        // Released last, so the collector never reads frames the handler has not finished writing.
        slot.state.store(int(slot_state::answered), cc::memory_order_release);
        ::sem_post(&g_answered);
        return;
    }
}

/// Every thread in this process, from the OS rather than from a registry.
///
/// A registry would only know the threads clean-core created, and the one worth reporting is routinely somebody
/// else's.
[[nodiscard]] isize enumerate_tids(cc::span<i32> out)
{
    auto* const dir = ::opendir("/proc/self/task");
    if (dir == nullptr)
        return 0;

    auto count = isize(0);
    while (count < out.size())
    {
        auto const* const entry = ::readdir(dir);
        if (entry == nullptr)
            break;
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9')
            continue;

        // Parsed by hand rather than with atoi, which is not async-signal-safe.
        // Note that opendir/readdir above are not either — see the hazard list in the header.
        auto value = i32(0);
        for (auto const* c = entry->d_name; *c != '\0'; ++c)
        {
            if (*c < '0' || *c > '9')
            {
                value = 0;
                break;
            }
            value = value * 10 + (*c - '0');
        }

        if (value != 0)
            out[count++] = value;
    }

    ::closedir(dir);
    return count;
}

/// Asks one thread and waits for it, or gives up.
[[nodiscard]] bool ask(thread_slot& slot, i32 tid, double timeout_secs)
{
    slot.frame_count.store(0, cc::memory_order_relaxed);
    slot.tid.store(tid, cc::memory_order_relaxed);
    slot.state.store(int(slot_state::requested), cc::memory_order_release);

    // tgkill rather than pthread_kill: the thread list is kernel tids, and translating them back to pthread_t
    // would need a registry this deliberately does without.
    if (::syscall(SYS_tgkill, ::getpid(), tid, g_signal) != 0)
    {
        slot.state.store(int(slot_state::idle), cc::memory_order_release);
        return false;
    }

    auto deadline = timespec{};
    ::clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += time_t(timeout_secs);
    deadline.tv_nsec += long((timeout_secs - double(time_t(timeout_secs))) * 1e9);
    if (deadline.tv_nsec >= 1'000'000'000)
    {
        deadline.tv_nsec -= 1'000'000'000;
        ++deadline.tv_sec;
    }

    while (::sem_timedwait(&g_answered, &deadline) != 0)
    {
        // EINTR is this process's own signals arriving, and waiting through them is the point.
        if (errno == EINTR)
            continue;
        return false;
    }

    return slot.state.load(cc::memory_order_acquire) == int(slot_state::answered);
}
} // namespace

bool cc::impl::posix_thread_stacks_available()
{
    return g_installed.load(cc::memory_order_acquire);
}

void cc::impl::install_posix_thread_stack_reporter()
{
    if (g_installed.load(cc::memory_order_acquire))
        return;

    g_signal = SIGRTMIN + 3;
    if (::sem_init(&g_answered, 0, 0) != 0)
        return;

    struct sigaction action;
    ::memset(&action, 0, sizeof(action));
    action.sa_handler = &report_own_stack;
    ::sigfillset(&action.sa_mask);

    // SA_RESTART so an interrupted syscall in the target resumes rather than failing: this is a diagnostic, and it
    // must not change the behaviour of the program it is diagnosing.
    action.sa_flags = SA_RESTART;

    if (::sigaction(g_signal, &action, nullptr) != 0)
        return;

    g_installed.store(true, cc::memory_order_release);
}

bool cc::impl::report_posix_thread_stacks() noexcept
{
    if (!g_installed.load(cc::memory_order_acquire))
        return false;

    i32 tids[max_threads] = {};
    auto const count = enumerate_tids(cc::span<i32>(tids, max_threads));
    if (count == 0)
        return false;

    auto const self = current_tid();

    // Outside the loop, so one thread's debug info is not opened and closed per frame.
    auto symbols = cc::symbolizer();

    for (auto i = isize(0); i < count; ++i)
    {
        if (tids[i] == self)
            continue;

        auto& slot = g_slots[i];

        std::fputs("  thread ", stderr);
        char buffer[32] = {};
        auto written = isize(0);
        for (auto v = tids[i]; v > 0; v /= 10)
            buffer[written++] = char('0' + (v % 10));
        for (auto j = written; j > 0; --j)
            std::fputc(buffer[j - 1], stderr);
        std::fputs(":\n", stderr);

        // One second, and the number is a guess that has never been tested against a real stuck thread.
        if (!ask(slot, tids[i], 1.0))
        {
            // **Reported rather than skipped.** A thread that cannot answer is often the one that matters, and
            // saying nothing about it reads as there being nothing to say.
            std::fputs("    <unresponsive; it did not run the handler within the budget>\n", stderr);
            slot.state.store(int(slot_state::idle), cc::memory_order_release);
            continue;
        }

        auto const frames = slot.frame_count.load(cc::memory_order_relaxed);
        for (auto f = i32(0); f < frames; ++f)
        {
            auto const& info = symbols.resolve(slot.frames[f]);
            auto const line = info.to_string();
            std::fputs("    ", stderr);
            std::fwrite(line.data(), 1, size_t(line.size()), stderr);
            std::fputc('\n', stderr);
        }

        slot.state.store(int(slot_state::idle), cc::memory_order_release);
    }

    return true;
}

#else // not Linux

bool cc::impl::posix_thread_stacks_available()
{
    return false;
}

void cc::impl::install_posix_thread_stack_reporter()
{
}

bool cc::impl::report_posix_thread_stacks() noexcept
{
    return false;
}

#endif
