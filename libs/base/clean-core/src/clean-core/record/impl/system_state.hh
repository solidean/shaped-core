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

/// Unlinks and frees a drained, dead thread state.
/// Consumer-only.
/// Clears a thread_state's pointer back into its owner's thread-local storage.
/// Called from the thread-exit handshake: the state outlives the thread so its chunks stay drainable, and that pointer
/// must not, because the storage it names goes away with the thread.
void detach_thread_state_tls(thread_state* s);

void reclaim_thread_state(thread_state* s);

/// Calls `f` for every registered thread, alive or not, under the registry lock.
void for_each_thread_state(cc::function_ref<void(thread_state&)> f);

/// How many threads have ever recorded.
[[nodiscard]] isize thread_state_count();
} // namespace cc::rec::impl
