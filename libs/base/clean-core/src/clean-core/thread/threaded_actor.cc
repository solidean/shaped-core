#include <clean-core/string/print.hh>
#include <clean-core/thread/thread.hh>
#include <clean-core/thread/threaded_actor.hh>

void cc::threaded_actor_base::start()
{
    CC_ASSERT(!_is_started.load(), "start() must be called exactly once");

    _is_started.store(true);
    _thread = std::thread([this] { execute_actor_thread(); });
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

    _thread.join();
    _is_shut_down.store(true);
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
