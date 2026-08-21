#include <clean-core/common/assert.hh>
#include <clean-core/common/macros.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/thread.hh>

#if CC_HAS_THREADS
// <chrono> is expensive and normally confined to time.cc; this is the one other place that needs it, because
// std::this_thread::sleep_for takes a duration and there is no other portable way to spell one.
#include <chrono>
#include <thread>

// The OS's own thread id, which cc::thread_id deliberately is not.
#if defined(_WIN32)
#include <clean-core/platform/win32_sanitized.hh>
#elif defined(__APPLE__)
#include <pthread.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

int cc::num_hardware_threads()
{
    unsigned const n = std::thread::hardware_concurrency();
    return n == 0 ? 1 : int(n);
}
void cc::this_thread_yield()
{
    std::this_thread::yield();
}

void cc::this_thread_sleep_secs(double secs)
{
    if (secs <= 0)
        return;
    std::this_thread::sleep_for(std::chrono::duration<double>(secs));
}
#else
int cc::num_hardware_threads()
{
    return 1;
}

void cc::this_thread_yield()
{
    // Nothing else can be runnable, so there is nobody to yield to.
}

void cc::this_thread_sleep_secs(double)
{
    // Nothing else can be runnable, so sleeping could only ever waste the wait.
}
#endif

namespace
{
// A counter rather than GetCurrentThreadId / pthread_self: one portable branch, distinct by construction, and equality is all any caller wants.
// Starts past thread_id::main, which is reserved for whoever claims it.
cc::atomic<cc::u64> g_next_thread_id = {cc::u64(cc::thread_id::main) + 1};

// Relaxed throughout: the counter hands out identities and publishes nothing else.
thread_local cc::thread_id tl_thread_id = cc::thread_id::invalid;

cc::atomic<bool> g_main_claimed = {false};
} // namespace

cc::u64 cc::native_thread_id()
{
#if !CC_HAS_THREADS
    return 0;
#elif defined(_WIN32)
    return u64(::GetCurrentThreadId());
#elif defined(__APPLE__)
    u64 id = 0;
    return pthread_threadid_np(nullptr, &id) == 0 ? id : 0;
#elif defined(__linux__)
    return u64(::gettid());
#else
    return 0;
#endif
}

cc::thread_id cc::current_thread_id()
{
    if (tl_thread_id == thread_id::invalid)
        tl_thread_id = thread_id(g_next_thread_id.fetch_add(1, cc::memory_order_relaxed));
    return tl_thread_id;
}

void cc::mark_current_thread_as_main()
{
    CC_ASSERT(tl_thread_id == thread_id::invalid || tl_thread_id == thread_id::main,
              "this thread was already handed an id; mark_current_thread_as_main must come before anything asks for "
              "cc::current_thread_id()");
    CC_ASSERT(!g_main_claimed.exchange(true, cc::memory_order_relaxed) || tl_thread_id == thread_id::main,
              "another thread already claimed cc::thread_id::main");
    tl_thread_id = thread_id::main;
}

#if CC_HAS_THREADS

#if defined(CC_OS_WINDOWS)

#include <clean-core/string/conversion.hh>

// Declared here (not via <windows.h>) to keep this TU light, mirroring how assert.cc imports IsDebuggerPresent.
// char16_t and Windows wchar_t are both 16-bit, so the wide buffer maps directly.
extern "C" __declspec(dllimport) void* __stdcall GetCurrentThread() noexcept;
extern "C" __declspec(dllimport) long __stdcall SetThreadDescription(void*, wchar_t const*) noexcept;

void cc::set_current_thread_name(string_view name)
{
    static_assert(sizeof(wchar_t) == sizeof(char16_t), "Windows wchar_t must be 16-bit");

    auto wide = cc::utf8_to_utf16(name);
    wide.push_back(u'\0');
    SetThreadDescription(GetCurrentThread(), reinterpret_cast<wchar_t const*>(wide.data()));
}

#elif defined(CC_OS_LINUX) || defined(CC_OS_ANDROID)

#include <pthread.h>

void cc::set_current_thread_name(string_view name)
{
    // Linux caps the thread name at 16 bytes including the NUL terminator.
    char buf[16];
    isize const n = name.size() < 15 ? name.size() : 15;
    for (isize i = 0; i < n; ++i)
        buf[i] = name.data()[i];
    buf[n] = '\0';

    pthread_setname_np(pthread_self(), buf);
}

#elif defined(CC_OS_MACOS) || defined(CC_OS_IOS) || defined(CC_OS_TVOS)

#include <pthread.h>

void cc::set_current_thread_name(string_view name)
{
    char buf[64];
    isize const n = name.size() < 63 ? name.size() : 63;
    for (isize i = 0; i < n; ++i)
        buf[i] = name.data()[i];
    buf[n] = '\0';

    pthread_setname_np(buf); // Darwin names only the current thread
}

#else // threads exist but naming is unsupported on this OS

void cc::set_current_thread_name(string_view)
{
}

#endif

#else // CC_HAS_THREADS == 0

void cc::set_current_thread_name(string_view)
{
}

#endif
