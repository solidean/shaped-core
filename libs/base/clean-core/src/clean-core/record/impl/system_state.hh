#pragma once

#include <clean-core/function/function_ref.hh>
#include <clean-core/record/fwd.hh>
#include <clean-core/thread/atomic.hh>

// The handful of globals the write path and the consumer share.
//
// Internal: everything here is either owned by system.cc or written once at initialize().
// The write path reads exactly two of them on its cold path and none on its hot one.

namespace cc::rec::impl
{
/// The live chunk pool, or null while the system is down.
/// **This is what makes recording before cc::rec::initialize() inert** rather than a crash: the cold path finds no
/// pool, counts a drop, and the first gap event afterwards says how many were lost.
extern cc::atomic<rec::chunk_pool*> g_pool;

/// Bumped by every initialize() and every shutdown().
/// The write path compares it against its thread-local copy and forgets everything on a mismatch.
extern cc::atomic<u64> g_system_generation;

/// How long the cold path waits before asking an exhausted pool again.
/// Without it a drop storm would take the pool lock once per dropped event, which is the opposite of what dropping is for.
extern cc::atomic<u64> g_drop_retry_cycles;

/// Registers a thread with the consumer.
/// Takes the registry lock; called once per recording thread.
void register_thread_state(thread_state* s);

/// Clears a thread_state's pointer back into its owner's thread-local storage.
/// Called from the thread-exit handshake: the state outlives the thread so its chunks stay drainable, and that pointer
/// must not, because the storage it names goes away with the thread.
void detach_thread_state_tls(thread_state* s);

/// Unlinks and frees a drained, dead thread state.
/// Consumer-only.
void reclaim_thread_state(thread_state* s);

/// Calls `f` for every registered thread, alive or not, under the registry lock.
void for_each_thread_state(cc::function_ref<void(thread_state&)> f);

/// The same, but gives up instead of blocking when the registry is busy; false means `f` never ran.
///
/// **For the crash handler, which must not block on a lock a dead thread may be holding.**
/// A thread that crashed inside registration or reclamation still holds this one, and a dump that waited for it would
/// hang exactly where a dump is most wanted — so failing to write is the better answer, and it names the reason.
[[nodiscard]] bool try_for_each_thread_state(cc::function_ref<void(thread_state&)> f);

/// Runs `f` on one registered thread state, chosen as `n` modulo however many there are, and reports that count.
///
/// The callback runs UNDER the registry lock, which is the whole point: a state is reaped the moment its owner has
/// died and been drained, and a sampler that picked one outside the lock would be holding freed memory.
/// Returns 0 without calling `f` when nothing is registered.
isize with_nth_thread_state(isize n, cc::function_ref<void(thread_state&)> f);

/// How many threads have ever recorded.
[[nodiscard]] isize thread_state_count();
} // namespace cc::rec::impl
