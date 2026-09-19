#include "posix_thread_stacks.hh"

#include <clean-core/common/macros.hh>

#if defined(__linux__) && !defined(__EMSCRIPTEN__)

#include <clean-core/container/span.hh>
#include <clean-core/thread/atomic.hh>
#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <unwind.h>

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

/// How long one thread gets to answer.
/// A thread that is running or blocked answers within microseconds; this only bounds one that cannot answer at all.
constexpr double answer_timeout_secs = 1.0;

/// What one asked thread is doing.
enum class slot_state : int
{
    idle = 0,
    requested, ///< the signal has been sent and the handler has not answered yet
    answered,  ///< frames below are this thread's
};

/// The one slot the collector and the handler share.
///
/// One, because the collector asks a single thread at a time and waits for it: the tid names who may answer, so a
/// late answer from a thread that already timed out finds the slot re-addressed and writes nothing.
struct thread_slot
{
    /// The kernel thread id that may answer, or 0 between requests.
    cc::atomic<i32> tid = {0};

    cc::atomic<int> state = {int(slot_state::idle)};

    void* frames[max_frames] = {};
    cc::atomic<i32> frame_count = {0};
};

thread_slot g_slot;

/// Posted by the handler, waited on by the collector.
sem_t g_answered;

cc::atomic<bool> g_installed = false;

[[nodiscard]] i32 current_tid()
{
    return i32(::syscall(SYS_gettid));
}

/// The frames one unwind has collected so far.
struct unwind_state
{
    cc::span<void*> frames;
    isize count = 0;
};

_Unwind_Reason_Code collect_frame(_Unwind_Context* context, void* user)
{
    auto& state = *static_cast<unwind_state*>(user);
    if (state.count >= state.frames.size())
        return _URC_END_OF_STACK;

    auto const pc = _Unwind_GetIP(context);
    if (pc == 0)
        return _URC_END_OF_STACK;

    state.frames[state.count++] = reinterpret_cast<void*>(pc);
    return _URC_NO_REASON;
}

/// The handler every asked thread runs.
///
/// Unwinds from tables rather than chasing frame pointers: an asked thread is almost always parked inside the C
/// library, which keeps no frame chain, so a chase stops at the first link and reports nothing.
/// The unwinder knows the signal trampoline and walks through it into the interrupted code.
void report_own_stack(int) noexcept
{
    auto const self = current_tid();
    if (g_slot.tid.load(cc::memory_order_acquire) != self)
        return; // a late answer to a request that already timed out
    if (g_slot.state.load(cc::memory_order_acquire) != int(slot_state::requested))
        return;

    auto state = unwind_state{.frames = cc::span<void*>(g_slot.frames)};
    _Unwind_Backtrace(&collect_frame, &state);
    g_slot.frame_count.store(i32(state.count), cc::memory_order_relaxed);

    // Released last, so the collector never reads frames the handler has not finished writing.
    g_slot.state.store(int(slot_state::answered), cc::memory_order_release);
    ::sem_post(&g_answered);
}

/// The layout the kernel writes for getdents64, which glibc declares only under a name that varies by version.
struct linux_dirent64
{
    u64 d_ino;
    i64 d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[1];
};

/// Every thread in this process, from the OS rather than from a registry.
///
/// A registry would only know the threads clean-core created, and the one worth reporting is routinely somebody
/// else's.
/// Read with raw syscalls into a stack buffer, because opendir allocates and this runs inside a fault handler.
[[nodiscard]] isize enumerate_tids(cc::span<i32> out)
{
    auto const fd = int(::syscall(SYS_openat, AT_FDCWD, "/proc/self/task", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (fd < 0)
        return 0;

    alignas(8) char buffer[4096];
    auto count = isize(0);
    while (count < out.size())
    {
        auto const read = ::syscall(SYS_getdents64, fd, buffer, sizeof(buffer));
        if (read <= 0)
            break;

        for (auto offset = 0l; offset < read && count < out.size();)
        {
            auto const* const entry = reinterpret_cast<linux_dirent64 const*>(buffer + offset);
            offset += entry->d_reclen;

            // Parsed by hand rather than with atoi, which is not async-signal-safe; "." and ".." fail the digit test.
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
    }

    ::close(fd);
    return count;
}

/// Writes `value` in decimal, or as `0x…` hex, without formatting through anything that could allocate.
void write_number(u64 value, bool hex)
{
    char digits[24] = {};
    auto n = 0;
    auto const base = hex ? 16u : 10u;
    do
    {
        digits[n++] = "0123456789abcdef"[value % base];
        value /= base;
    } while (value != 0);

    if (hex)
        std::fputs("0x", stderr);
    while (n > 0)
        std::fputc(digits[--n], stderr);
}

/// Asks one thread and waits for it, or gives up.
[[nodiscard]] bool ask(i32 tid)
{
    g_slot.frame_count.store(0, cc::memory_order_relaxed);
    g_slot.state.store(int(slot_state::requested), cc::memory_order_relaxed);
    g_slot.tid.store(tid, cc::memory_order_release);

    // tgkill rather than pthread_kill: the thread list is kernel tids, and translating them back to pthread_t
    // would need a registry this deliberately does without.
    if (::syscall(SYS_tgkill, ::getpid(), tid, g_signal) != 0)
        return false;

    auto deadline = timespec{};
    ::clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += long(answer_timeout_secs * 1e9);
    while (deadline.tv_nsec >= 1'000'000'000)
    {
        deadline.tv_nsec -= 1'000'000'000;
        ++deadline.tv_sec;
    }

    for (;;)
    {
        if (::sem_timedwait(&g_answered, &deadline) != 0)
        {
            // EINTR is this process's own signals arriving, and waiting through them is the point.
            if (errno == EINTR)
                continue;
            return false;
        }

        // A post from a thread that answered after its own timeout is spent here rather than mistaken for this one.
        if (g_slot.state.load(cc::memory_order_acquire) == int(slot_state::answered))
            return true;
    }
}

/// Ends a request, and swallows any answer that is still on its way so the next request starts from zero.
void retire_request()
{
    g_slot.tid.store(0, cc::memory_order_release);
    g_slot.state.store(int(slot_state::idle), cc::memory_order_release);
    while (::sem_trywait(&g_answered) == 0)
    {
    }
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

    // One unwind now, so whatever the unwinder sets up on first use — its lookup caches, a lazily bound symbol — is
    // done before any thread has to do it inside the handler.
    {
        void* warm[4] = {};
        auto state = unwind_state{.frames = cc::span<void*>(warm)};
        _Unwind_Backtrace(&collect_frame, &state);
    }

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

    for (auto i = isize(0); i < count; ++i)
    {
        if (tids[i] == self)
            continue;

        std::fputs("  thread ", stderr);
        write_number(u64(tids[i]), false);
        std::fputs(":\n", stderr);

        if (!ask(tids[i]))
        {
            // **Reported rather than skipped.** A thread that cannot answer is often the one that matters, and
            // saying nothing about it reads as there being nothing to say.
            std::fputs("    <unresponsive; it did not run the handler within the budget>\n", stderr);
            retire_request();
            continue;
        }

        // Raw addresses: resolving them would mean the symbolizer's allocating cache inside a fault handler.
        // They are absolute, so an offline resolver also needs the module bases the recording carries.
        auto const frames = g_slot.frame_count.load(cc::memory_order_relaxed);
        if (frames == 0)
            std::fputs("    <no frames; the unwinder found nothing to walk>\n", stderr);
        for (auto f = i32(0); f < frames; ++f)
        {
            std::fputs("    ", stderr);
            write_number(u64(reinterpret_cast<uintptr_t>(g_slot.frames[f])), true);
            std::fputc('\n', stderr);
        }

        retire_request();
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
