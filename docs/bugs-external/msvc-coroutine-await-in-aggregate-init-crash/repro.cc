// Standalone reproduction: MSVC crashes (C1001) on a co_await inside an aggregate's initializer.
// run.py compiles it once per case, and CRASH and ARRAY are the two that crash.
// It is compiled and never linked, because the crash comes first.

#include <coroutine>

struct heavy
{
    ~heavy(); // the bug needs this: without a destructor nothing has to be cleaned up across the suspension
};

struct awaitable
{
    bool await_ready();
    void await_suspend(std::coroutine_handle<>);
    heavy await_resume();
};

struct task
{
    struct promise_type
    {
        task get_return_object();
        std::suspend_never initial_suspend();
        std::suspend_never final_suspend() noexcept;
        void return_void();
        void unhandled_exception();
    };
};

heavy seed();

struct pair
{
    heavy first;
    heavy second;
};

struct with_constructor
{
    with_constructor(heavy, heavy);
    heavy first;
    heavy second;
};

struct int_first
{
    int first;
    heavy second;
};

void take(heavy, heavy);

task reproduce()
{
#if defined(CRASH)
    pair p = {seed(), co_await awaitable{}}; // `first` is built, and alive, while this suspends
#elif defined(ARRAY)
    heavy p[2] = {seed(), co_await awaitable{}};
#elif defined(AWAIT_FIRST)
    pair p = {co_await awaitable{}, seed()}; // nothing is built yet when this suspends
#elif defined(NAMED)
    auto awaited = co_await awaitable{}; // the workaround: `p` does not exist while this suspends
    pair p = {seed(), static_cast<heavy&&>(awaited)};
#elif defined(INT_FIRST)
    int_first p = {1, co_await awaitable{}}; // what is built has no destructor
#elif defined(CONSTRUCTOR)
    with_constructor p = {seed(), co_await awaitable{}}; // these are arguments, and the object is not begun
#elif defined(CALL)
    take(seed(), co_await awaitable{});
#else
#error "run.py defines one case"
#endif
}
