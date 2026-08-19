#pragma once

#include <clean-core/function/unique_function.hh>
#include <clean-core/fwd.hh>

// Cooperative concurrency for the semantic threads that have no OS thread.
//
// Anything that would block has to let the rest of the program progress, and without threads there is no rest of the
// program unless somebody runs it.
// So a semantic thread with no thread of its own registers a pump here, and every blocking wait — cc::async_blocking_get,
// a frame loop, a shutdown drain — runs the whole registry rather than only the actors it happens to know about.
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
/// It runs on whichever thread is blocking, not on one of its own, and it is never re-entered: a sweep that finds it
/// already running skips it, exactly as a busy thread takes no new work.
///
/// A pump MUST NOT block on progress another registration has to make.
/// It holds the only thread there is, so the pump it is waiting for never runs — the wait that looks like a stall is a
/// deadlock, and the one place a simulated thread differs from a real one.
/// Sweep instead: calling thread_pump_all() from inside a pump is safe (this one is skipped, the others run) and is how
/// a handler waits for a sibling.
/// Blocking on something OUTSIDE the registry — a GPU fence, an OS handle — stays fine, because nothing here has to run
/// for it to be signalled.
/// (declared above cc::thread_pump_registration, which befriends it)

/// Runs one cycle of every registered pump; true if any reported progress or more work.
/// Safe to call unconditionally: with nothing registered it is one atomic load, which is the normal threaded build.
bool thread_pump_all();

/// Repeats thread_pump_all() until nothing progresses or `max_ms` of wall-clock elapses; max_ms <= 0 runs a single cycle.
/// Returns true if it stopped on the budget with work still pending.
bool thread_pump_all_for(double max_ms);

/// How many pumps are registered right now.
/// For a leak check at the end of a run: a registration outliving its semantic thread is a bug, and a silent one.
[[nodiscard]] isize registered_thread_pump_count();
} // namespace cc
