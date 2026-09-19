#include "host_loop.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/memory/unique_ptr.hh>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include <emscripten/eventloop.h>

#include <cstdlib> // std::exit: EXIT_RUNTIME turns it into the process's exit code under node and deno
#endif

bool nx::impl::has_host_event_loop()
{
#ifdef __EMSCRIPTEN__
    return true;
#else
    return false;
#endif
}

#ifdef __EMSCRIPTEN__
namespace
{
struct host_run
{
    cc::unique_function<bool()> step;
    cc::unique_function<int()> finish;
};

/// One turn: step, and either end the process or come back after the host has run what is queued.
///
/// A step that stalls is waiting for something only the host can deliver, so the next turn is a timer rather than an immediate call.
/// The one millisecond is a poll interval, not a wait: nothing here sleeps, and a turn with nothing to do returns at once.
void host_turn(void* user)
{
    auto* const run = static_cast<host_run*>(user);
    if (!run->step())
    {
        emscripten_set_timeout(&host_turn, 1, user);
        return;
    }

    auto const code = run->finish();
    delete run;
    std::exit(code);
}
} // namespace

void nx::impl::run_in_host_loop(cc::unique_function<bool()> step, cc::unique_function<int()> finish)
{
    host_turn(new host_run{.step = cc::move(step), .finish = cc::move(finish)});

    // Still running: the thread goes back to the host, and the timer set above carries the run on.
    emscripten_exit_with_live_runtime();
}
#else
void nx::impl::run_in_host_loop(cc::unique_function<bool()>, cc::unique_function<int()>)
{
    CC_UNREACHABLE("run_in_host_loop needs a host event loop; check has_host_event_loop() first");
}
#endif
