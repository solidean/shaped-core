#pragma once

#include <clean-core/function/unique_function.hh>
#include <clean-core/fwd.hh>

// Cooperative concurrency for the semantic threads that have no OS thread.
//
// Anything that would block has to let the rest of the program progress, and without threads there is no rest of the
// program unless somebody runs it.
// So a semantic thread with no thread of its own registers a pump here, and every loop that drives such threads — a frame
// loop, a blocking drive on a scheduler without threads, a shutdown drain — runs the whole registry rather than only the
// actors it happens to know about.
// A thread parked in a pool is not such a loop, and never sweeps; see register_thread_pump.
//
// That is the point: individual pumping is a deadlock waiting to happen.
// A wait can only drain what its own library can name, so the next actor added below it, or beside it, deadlocks a build
// with no threads and passes every threaded test.
//
// Registration follows the LIFETIME of the semantic thread, so it is the same rule everywhere:
// cc::threaded_actor registers itself when started unthreaded, and a hand-rolled thread registers where it would have
// spawned and deregisters where it would have joined.
// An actor that owns a real thread registers nothing, which is why a normal threaded build finds the registry empty and
// a sweep costs one atomic load.

namespace cc
{
// Declared ahead of the handle, which befriends it: only registration may build one.
[[nodiscard]] thread_pump_registration register_thread_pump(cc::unique_function<bool()> pump);
} // namespace cc

/// Keeps one pump registered; it runs until this handle dies.
/// Move-only, and the destructor outlasts a sweep already inside that pump on another thread — the semantic thread it
/// belongs to is about to go away, so outranking the call is not enough.
struct cc::thread_pump_registration
{
    thread_pump_registration() = default;
    thread_pump_registration(thread_pump_registration&& rhs) noexcept;
    thread_pump_registration& operator=(thread_pump_registration&& rhs) noexcept;
    ~thread_pump_registration();

    thread_pump_registration(thread_pump_registration const&) = delete;
    thread_pump_registration& operator=(thread_pump_registration const&) = delete;

    /// Deregisters early; a moved-from or default-constructed handle is a no-op.
    void reset();

    [[nodiscard]] bool is_registered() const { return _entry != nullptr; }

private:
    friend cc::thread_pump_registration cc::register_thread_pump(cc::unique_function<bool()> pump);
    explicit thread_pump_registration(cc::impl::thread_pump_entry* entry) : _entry(entry) {}

    cc::impl::thread_pump_entry* _entry = nullptr;
};

namespace cc
{
/// Registers `pump` for the lifetime of the returned handle.
///
/// `pump` must return true only when it made progress or knows of more work.
/// One that always returns true turns every blocking wait into a busy loop, because a driver treats "no progress
/// anywhere" as its cue to sleep.
///
/// It must also be CHEAP when it has nothing due.
/// A sweep runs on every blocking wait in the process, which is orders of magnitude more often than any one caller's
/// cadence, so work a real thread would have slept between belongs behind the same interval here.
///
/// It runs on whichever thread sweeps, not on one of its own, and it is never re-entered: a sweep that finds it already
/// running on another thread asks that thread to run it once more, rather than taking the work itself.
///
/// **Who sweeps is the thread that owns the loop driving it** — a frame loop, a test's own pump loop, a blocking drive
/// on a scheduler without threads, the main thread's loop.
/// A thread parked in a cc::async_thread_pool never sweeps.
/// That is deliberate, and it is what makes an unthreaded component deterministic: were parked pool threads to sweep, every
/// post to a component would hand its handlers to whichever unrelated thread happened to be parked, racing the loop that
/// owns it — see "Who drives a pump" in docs/systems/async.md.
/// So a coroutine awaiting an unthreaded component must run where such a loop is: homed to the main thread, or on a
/// thread that pumps.
///
/// **A pump that gains work while nobody runs it must raise cc::thread_pump_notify**, so a loop parked on that signal wakes.
///
/// A pump MUST NOT block on progress another registration has to make.
/// It holds the only thread there is, so the pump it is waiting for never runs — the wait that looks like a stall is a
/// deadlock, and the one place a simulated thread differs from a real one.
/// Sweep instead: calling thread_pump_all() from inside a pump is safe (this one is skipped, the others run) and is how
/// a handler waits for a sibling.
/// Blocking on something OUTSIDE the registry — a GPU fence, an OS handle — stays fine, because nothing here has to run
/// for it to be signalled.
/// (declared above cc::thread_pump_registration, which befriends it)

/// Says a registered pump has work it did not have when it last returned — a post landed in its mailbox.
///
/// **This is what lets a loop that drives pumps sleep instead of polling.** Every such loop parked on the signal wakes
/// and sweeps once, so a delivery is never waiting for a clock.
/// Raised after the work is visible to the pump, from any thread; costs a lock over the parked loops, of which there are few.
/// Registration raises it too, since a new pump may already hold work.
void thread_pump_notify();

/// Runs one cycle of the calling thread's home, if it owns one, and of every registered pump; true if any reported progress or more work.
/// Safe to call unconditionally: with nothing registered and no home it is a TLS read and one atomic load, which is the normal threaded build.
bool thread_pump_all();

/// Repeats thread_pump_all() until nothing progresses or `max_ms` of wall-clock elapses; max_ms <= 0 runs a single cycle.
/// Returns true if it stopped on the budget with work still pending.
bool thread_pump_all_for(double max_ms);

/// How many pumps are registered right now.
/// For a leak check at the end of a run: a registration outliving its semantic thread is a bug, and a silent one.
[[nodiscard]] isize registered_thread_pump_count();
} // namespace cc

namespace cc::impl
{
/// thread_pump_all without the calling thread's home: one sweep of the registry.
/// For cc::pump_main_thread, which pumps the main home itself under its own budget, and for a loop about to park.
bool thread_pump_registry();

/// Calls `wake(ctx)` on every cc::thread_pump_notify for as long as it lives.
///
/// For a loop that drives pumps and is about to park.
/// The wait has to sweep AFTER the listener exists and park only if that sweep found nothing, which is what makes a
/// notify racing the park impossible to lose: the work was either visible to the sweep, or its notify reaches the listener.
/// `wake` runs on the notifying thread with the listener list locked, so it may take its own lock and notify, and nothing more.
struct thread_pump_listener
{
    thread_pump_listener(void (*wake)(void*), void* ctx);
    ~thread_pump_listener();

    thread_pump_listener(thread_pump_listener const&) = delete;
    thread_pump_listener& operator=(thread_pump_listener const&) = delete;

    void (*wake)(void*) = nullptr;
    void* ctx = nullptr;
};
} // namespace cc::impl
