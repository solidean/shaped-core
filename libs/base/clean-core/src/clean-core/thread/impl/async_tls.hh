#pragma once

#include <clean-core/fwd.hh>

// The async runtime's per-thread state, in ONE block.
// Three separate thread_locals used to answer three questions a single poll asks together — the bound scheduler, the inline-drive depth, and the running worker.
//
// Consolidating them measured NEUTRAL on the single-thread drive benchmark, inside its run-to-run spread.
// It is here so further per-thread state costs no further TLS resolution, not as an optimization in its own right.

namespace cc::impl
{
/// Per-thread state of the async runtime.
/// POD and zero-initialized, so it needs no lazy-init guard and registers no TLS destructor.
///
/// `current_worker` is a void* because async_thread_pool::worker is a private nested type; the pool casts at its own use sites.
/// It is declared unconditionally: a build without threads simply leaves it null rather than reshaping this struct.
struct async_tls_block
{
    cc::async_scheduler* scheduler = nullptr; // the scheduler bound by async_worker_scope; null => no worker scope
    void* ambient = nullptr;        // head of the ambient context chain (async_ambient.hh); null => no scope active
    void* current_worker = nullptr; // async_thread_pool::worker running this thread's loop; null on foreign threads
    int inline_depth = 0;           // recursion depth of the eager depth-first dep drive
};

/// Defined in async.cc.
/// extern rather than a function-local static so the accessor inlines to a direct TLS read.
extern thread_local async_tls_block s_async_tls;

[[nodiscard]] inline async_tls_block& async_tls()
{
    return s_async_tls;
}
} // namespace cc::impl
