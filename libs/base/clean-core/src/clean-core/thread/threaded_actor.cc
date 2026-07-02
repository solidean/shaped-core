#include <clean-core/string/print.hh>
#include <clean-core/thread/thread.hh>
#include <clean-core/thread/threaded_actor.hh>

#include <chrono>

void cc::threaded_actor_base::start(threaded_actor_mode mode)
{
    CC_ASSERT(!_is_started.load(), "start() must be called exactly once");

    _is_started.store(true);

#if CC_HAS_THREADS
    bool const threaded = mode == threaded_actor_mode::threaded_if_possible;
#else
    bool const threaded = false; // no OS threads on this platform: always run unthreaded
#endif

    if (threaded)
    {
        _thread = std::thread([this] { execute_actor_thread(); });
    }
    else
    {
        // No thread: the caller drives the loop via process_messages_if_unthreaded[_for_ms]().
        // on_thread_init runs here, on the caller; there is no OS thread to name.
        _is_unthreaded = true;
        get_impl().on_thread_init();
    }
}

void cc::threaded_actor_base::begin_shutdown()
{
    CC_ASSERT(_is_started.load(), "actor must be started before begin_shutdown()");
    CC_ASSERT(!_is_shutting_down.load(), "begin_shutdown() must be called at most once");

    _is_shutting_down.store(true);
    _inbox_cond_var.notify_one();
}

void cc::threaded_actor_base::shutdown()
{
    CC_ASSERT(_is_started.load(), "actor must be started before shutdown()");
    CC_ASSERT(!_is_shut_down.load(), "shutdown() must be called at most once");

    if (!_is_shutting_down.load())
        begin_shutdown();

    if (_is_unthreaded)
    {
        // Drain everything queued before shutdown, synchronously on the caller. Loop until a cycle
        // dispatches no messages (mirrors the threaded loop's "exit once the inbox is empty").
        auto& impl = get_impl();
        while (true)
        {
            bool const dispatched = drain_inbox_messages(false);
            impl.on_process();
            if (!dispatched)
                break;
        }
        impl.on_thread_shutdown();
    }
    else
    {
        _thread.join();
    }

    _is_shut_down.store(true);
}

bool cc::threaded_actor_base::process_messages_if_unthreaded()
{
    // No-op unless we own the loop and are still alive; makes it safe to call unconditionally.
    if (!_is_unthreaded || _is_shut_down.load())
        return false;

    bool const dispatched = drain_inbox_messages(false);
    bool const wants_more = get_impl().on_process();
    return dispatched || wants_more;
}

bool cc::threaded_actor_base::process_messages_if_unthreaded_for_ms(double max_ms)
{
    if (max_ms <= 0)
        return process_messages_if_unthreaded();

    auto const deadline = std::chrono::steady_clock::now() + std::chrono::duration<double, std::milli>(max_ms);
    while (true)
    {
        if (!process_messages_if_unthreaded())
            return false; // idle: nothing left to do
        if (std::chrono::steady_clock::now() >= deadline)
            return true; // stopped on the budget with work still pending
    }
}

bool cc::threaded_actor_base::is_shutting_down() const
{
    return _is_shutting_down.load();
}

bool cc::threaded_actor_base::is_shut_down() const
{
    return _is_shut_down.load();
}

bool cc::threaded_actor_base::is_running() const
{
    return _is_started.load() && !_is_shutting_down.load();
}

void cc::threaded_actor_base::report_unhandled_exception(char const* where) noexcept
{
    cc::eprint("threaded_actor: unhandled exception in ");
    cc::eprintln(where);
}

void cc::threaded_actor_base::execute_actor_thread()
{
    try
    {
        auto& impl = get_impl();

        cc::set_current_thread_name(impl.actor_name());
        impl.on_thread_init();

        while (true)
        {
            drain_inbox_messages(false);
            bool const made_progress = impl.on_process();

            // Exit only when shutdown is requested AND the inbox drains empty, so every message
            // enqueued before shutdown() is still processed. If a message arrives between the
            // on_process() call and here, the second drain finds it and we loop again.
            bool const should_exit = is_shutting_down() && !drain_inbox_messages(false);
            if (should_exit)
                break;

            // nothing to do right now: sleep until a message arrives or shutdown begins
            if (!made_progress)
                drain_inbox_messages(true);
        }

        impl.on_thread_shutdown();
    }
    catch (...)
    {
        report_unhandled_exception("actor thread");
    }
}
