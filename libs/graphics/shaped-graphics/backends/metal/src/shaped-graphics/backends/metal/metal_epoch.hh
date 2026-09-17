#pragma once

#include <clean-core/container/ringbuffer.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/backends/metal/fwd.hh>
#include <shaped-graphics/backends/metal/metal_common.hh>
#include <shaped-graphics/fwd.hh>


namespace sg::backend::metal
{
struct metal_epoch_payload;
}

// Per-epoch bookkeeping for the metal backend's epoch system.
// The epoch *concept* lives in sg:: — fwd.hh plus the sg::context contract — and this is metal's concrete realization on
// a pair of MTLSharedEvents, which are timeline semaphores under another name.
// See libs/graphics/shaped-graphics/docs/concepts/epochs.md.

/// Everything one closed epoch owns and reclaims once its GPU work finishes.
struct sg::backend::metal::metal_epoch_payload
{
    /// The epoch fence value that has to be reached before any of this may be touched.
    u64 epoch_value = 0;

    /// Command allocators the lists of this epoch recorded into.
    ///
    /// An MTL4 allocator cannot be reset while a command buffer built from it is still executing, which is the whole
    /// reason they ride the epoch rather than going straight back to the pool at submit.
    cc::vector<MTL4::CommandAllocator*> allocators;

    /// Ran after the epoch retires — releasing a resource's Metal objects is one of these.
    cc::vector<cc::unique_function<void()>> finalizers;
};

/// The metal backend's epoch and submission timelines.
///
/// Two MTLSharedEvents on one MTL4 queue: the epoch event is signalled with the closing epoch's value at `advance`, and
/// the submission event with a per-list value at every commit.
/// Both are monotonic, so "has N finished" is one integer compare against `signaledValue()`.
///
/// Every method is safe to call from any thread except `advance` and `shutdown`, which sg already requires the caller
/// to serialize — see libs/graphics/shaped-graphics/docs/concepts/threading.md.
class sg::backend::metal::metal_epoch_system
{
public:
    /// Takes ownership of `epoch_event` and `submission_event`; `device` and `queue` are the context's and outlive this.
    metal_epoch_system(MTL::Device* device,
                       MTL4::CommandQueue* queue,
                       MTL::SharedEvent* epoch_event,
                       MTL::SharedEvent* submission_event);
    ~metal_epoch_system();

    metal_epoch_system(metal_epoch_system const&) = delete;
    metal_epoch_system& operator=(metal_epoch_system const&) = delete;

    [[nodiscard]] sg::epoch current() const { return sg::epoch(_current.load(std::memory_order_acquire)); }

    /// The newest epoch whose GPU work the fence says is finished.
    ///
    /// A fresh event sits at 0 while epochs start at `epoch::first`, so a value below that reads as `first - 1`:
    /// "nothing has finished", expressed as a value every ordinary `<=` comparison already handles.
    [[nodiscard]] sg::epoch completed() const;

    /// Closes the current epoch and opens the next; never waits.
    void advance();

    /// Reclaims every in-flight epoch the fence has passed, oldest first.
    void retire_completed();

    [[nodiscard]] int in_flight_count();

    /// Blocks until the fence reaches `e`, then retires.
    void wait_for(sg::epoch e);

    /// Blocks until the oldest in-flight epoch retires; a no-op when none is.
    /// The standard back-pressure primitive when a pool is exhausted.
    void wait_for_next_inflight();

    /// Blocks until every committed command buffer has finished, then retires.
    void block_until_submissions_complete();

    /// The token the next commit signals, handed out before the commit so the caller can pass it on.
    [[nodiscard]] sg::submission_token claim_submission_token();

    /// Signals `token`'s value on the queue; called immediately after the commit it belongs to.
    void signal_submission(sg::submission_token token);

    [[nodiscard]] bool is_submission_complete(sg::submission_token token) const;

    /// The newest submission token handed out, or `not_submitted` before anything has been submitted.
    /// `_next_submission` is what the NEXT commit will take, so the newest issued is one below it.
    [[nodiscard]] sg::submission_token last_issued_submission() const;

    /// The epoch timeline, for a waiter that parks on it directly.
    [[nodiscard]] MTL::SharedEvent* epoch_timeline() const { return _epoch_event; }

    /// The submission timeline itself, for a second queue that has to order itself behind the direct one.
    /// `metal_transfer_system` is the only caller; every other question about it is `is_submission_complete`.
    [[nodiscard]] MTL::SharedEvent* submission_timeline() const { return _submission_event; }

    /// An allocator to record into, recycled from the pool or newly created.
    /// Null when the device refused a fresh one, which every caller reports rather than asserts on.
    [[nodiscard]] MTL4::CommandAllocator* lease_allocator();

    /// Hands `allocator` to the open epoch, which resets it and returns it to the pool once the GPU is done with it.
    void retire_allocator_with_epoch(MTL4::CommandAllocator* allocator);

    /// Hands an allocator back from the commit's own feedback handler, which is the only thing that knows that
    /// commit has finished.
    ///
    /// **A transfer or stream allocator must not ride the epoch.**
    /// `retire_allocator_with_epoch` resets when the *direct* queue's epoch fence is reached, and nothing ties that to
    /// the transfer or stream queue — so an upload ordered behind a stream, or behind a busy submission, still has a
    /// command buffer pending when the frame advances, and its allocator is reset and leased to the next list.
    /// That is what `_free_allocators`' contract forbids, and it corrupts whatever the next list records.
    ///
    /// Called on a queue Apple owns, hence the callback mutex rather than `_mutex`.
    void return_allocator_from_callback(MTL4::CommandAllocator* allocator);

    /// Runs `finalizer` once the open epoch's GPU work has finished.
    void defer(cc::unique_function<void()> finalizer);

    /// Drains the GPU, runs every outstanding finalizer, and releases the events and the allocator pool.
    /// Idempotent; the context calls it before it releases the device.
    void shutdown();

private:
    MTL::Device* _device = nullptr;
    MTL4::CommandQueue* _queue = nullptr;
    MTL::SharedEvent* _epoch_event = nullptr;
    MTL::SharedEvent* _submission_event = nullptr;

    /// The epoch new work records into, and the value the next commit signals.
    cc::atomic<u64> _current = {u64(sg::epoch::first)};
    cc::atomic<u64> _next_submission = {u64(sg::submission_token::first)};

    /// Guards the in-flight FIFO, the open epoch's payload and the allocator pool.
    cc::mutex<int> _mutex;
    cc::ringbuffer<metal_epoch_payload> _in_flight;
    metal_epoch_payload _open;
    cc::vector<MTL4::CommandAllocator*> _free_allocators;

    /// Allocators handed back from a commit feedback handler, drained by `lease_allocator`.
    /// Separate from `_free_allocators` because it is written from Apple's threads, which `_mutex` does not cover with
    /// SC_THREADS off.
    callback_mutex<cc::vector<MTL4::CommandAllocator*>> _callback_free_allocators;

    bool _is_shut_down = false;
};
