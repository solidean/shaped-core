#include <clean-core/common/assert.hh>
#include <clean-core/common/assertf.hh>
#include <clean-core/common/utility.hh> // cc::memcpy
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/thread/native_thread.hh>
#include <clean-core/thread/thread.hh>

#if CC_HAS_THREADS
#if defined(_WIN32)
#include <clean-core/platform/win32_sanitized.hh>
#include <process.h> // _beginthreadex: the CRT's thread start, which sets up its per-thread state as CreateThread does not promise to

#include <cerrno>
#else
#include <limits.h> // PTHREAD_STACK_MIN
#include <pthread.h>
#include <unistd.h> // sysconf
#endif
#endif

using namespace cc::primitive_defines;

namespace
{
template <class StartData>
void run_body(StartData& start) noexcept
{
    if (!start.name.empty())
        cc::set_current_thread_name(start.name);
    start.body();
    // Released here, so what the body captured dies on the thread that ran it, as it would under std::thread.
    start.body = {};
}

#if CC_HAS_THREADS
#if defined(_WIN32)
template <class StartData>
unsigned __stdcall thread_entry(void* start)
{
    run_body(*static_cast<StartData*>(start));
    return 0;
}
#else
template <class StartData>
void* thread_entry(void* start)
{
    run_body(*static_cast<StartData*>(start));
    return nullptr;
}

/// pthread_attr_setstacksize rejects a size below PTHREAD_STACK_MIN, and macOS one that is not a whole number of pages.
size_t posix_stack_size(isize bytes)
{
    auto const page = size_t(sysconf(_SC_PAGESIZE));
    auto size = size_t(bytes) < size_t(PTHREAD_STACK_MIN) ? size_t(PTHREAD_STACK_MIN) : size_t(bytes);
    return (size + page - 1) / page * page;
}

// A byte copy rather than a cast: pthread_t is an integer on Linux, a pointer on macOS, and 4 bytes on wasm32.
static_assert(sizeof(pthread_t) <= sizeof(u64), "a pthread_t must fit the u64 handle");

u64 to_handle(pthread_t thread)
{
    auto handle = u64(0);
    cc::memcpy(&handle, &thread, sizeof(thread));
    return handle;
}

pthread_t from_handle(u64 handle)
{
    auto thread = pthread_t();
    cc::memcpy(&thread, &handle, sizeof(thread));
    return thread;
}
#endif
#endif
} // namespace

cc::native_thread cc::native_thread::start(cc::unique_function<void()> body, native_thread_config const& config)
{
    CC_ASSERT(body.is_valid(), "a native_thread needs a body");
    CC_ASSERT(config.stack_bytes >= 0, "stack_bytes must be >= 0");

    auto thread = native_thread();
    thread._start = cc::make_unique<start_data>(start_data{.body = cc::move(body), .name = cc::string(config.name)});

#if !CC_HAS_THREADS
    run_body(*thread._start);
#elif defined(_WIN32)
    // A reservation, not a commit: the stack grows into it page by page, as the main thread's does.
    auto const handle = ::_beginthreadex(nullptr, unsigned(config.stack_bytes), thread_entry<start_data>,
                                         thread._start.get(), STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    CC_ASSERTF_ALWAYS(handle != 0, "_beginthreadex failed (errno {})", errno);
    thread._handle = u64(handle);
#else
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (config.stack_bytes > 0)
    {
        auto const set = pthread_attr_setstacksize(&attr, posix_stack_size(config.stack_bytes));
        CC_ASSERTF_ALWAYS(set == 0, "pthread_attr_setstacksize failed ({}) for {} bytes", set, config.stack_bytes);
    }
    auto handle = pthread_t();
    auto const created = pthread_create(&handle, &attr, thread_entry<start_data>, thread._start.get());
    pthread_attr_destroy(&attr);
    CC_ASSERTF_ALWAYS(created == 0, "pthread_create failed ({})", created);
    thread._handle = to_handle(handle);
#endif

    return thread;
}

void cc::native_thread::join()
{
    CC_ASSERT(joinable(), "join needs a started thread that was not joined yet");

#if CC_HAS_THREADS
#if defined(_WIN32)
    auto const handle = reinterpret_cast<HANDLE>(_handle);
    ::WaitForSingleObject(handle, INFINITE);
    ::CloseHandle(handle);
#else
    auto const joined = pthread_join(from_handle(_handle), nullptr);
    CC_ASSERTF_ALWAYS(joined == 0, "pthread_join failed ({})", joined);
#endif
#endif

    _handle = 0;
    _start = {};
}

cc::native_thread& cc::native_thread::operator=(native_thread&& rhs) noexcept
{
    CC_ASSERT(!joinable(), "a native_thread must be joined before it is assigned over");
    _start = cc::move(rhs._start);
    _handle = cc::exchange(rhs._handle, 0);
    return *this;
}

cc::native_thread::~native_thread()
{
    CC_ASSERT(!joinable(), "a native_thread must be joined before it is destroyed");
}
