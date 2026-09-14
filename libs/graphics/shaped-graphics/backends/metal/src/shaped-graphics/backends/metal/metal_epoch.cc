#include "metal_epoch.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>

namespace sg::backend::metal
{
namespace
{
/// How long a single fence wait parks before looking again.
///
/// `waitUntilSignaledValue` takes a timeout rather than blocking forever, so a wait is a loop around it.
/// The value only decides how often a hung GPU re-checks, never how promptly a healthy one wakes: the call returns as
/// soon as the value lands.
constexpr u64 k_wait_slice_ms = 1000;

void wait_for_value(MTL::SharedEvent* event, u64 value)
{
    while (event->signaledValue() < value)
        (void)event->waitUntilSignaledValue(value, k_wait_slice_ms);
}
} // namespace

metal_epoch_system::metal_epoch_system(MTL::Device* device,
                                       MTL4::CommandQueue* queue,
                                       MTL::SharedEvent* epoch_event,
                                       MTL::SharedEvent* submission_event)
  : _device(device), _queue(queue), _epoch_event(epoch_event), _submission_event(submission_event)
{
    CC_ASSERT(_device != nullptr, "epoch system needs a device");
    CC_ASSERT(_queue != nullptr, "epoch system needs a queue");
    CC_ASSERT(_epoch_event != nullptr && _submission_event != nullptr, "epoch system needs both timelines");

    _open.epoch_value = _current.load(std::memory_order_relaxed);
}

metal_epoch_system::~metal_epoch_system()
{
    shutdown();
}

sg::epoch metal_epoch_system::completed() const
{
    // After shutdown the timelines are released and everything they tracked has drained, so the open epoch's
    // predecessor is the true answer and reading a freed event is not.
    if (_is_shut_down)
        return sg::epoch(_current.load(std::memory_order_acquire) - 1);

    auto const value = _epoch_event->signaledValue();
    auto const first_minus_one = u64(sg::epoch::first) - 1;
    return sg::epoch(value < u64(sg::epoch::first) ? first_minus_one : value);
}

void metal_epoch_system::advance()
{
    auto const closing = _current.load(std::memory_order_relaxed);

    // The signal happens under the same lock that parks the payload, and after it.
    //
    // `defer` runs from any thread — a resource's refcount hits zero wherever the last handle is dropped — so signalling
    // outside the lock leaves a window where the fence has already passed `closing` and `_open` is still the payload a
    // concurrent `defer` appends to.
    // That entry would then be parked against an epoch the GPU is already done with, and the next sweep would free a
    // resource still in use.
    _mutex.lock(
        [&](int&)
        {
            _in_flight.push_back(cc::move(_open));
            _open = metal_epoch_payload{.epoch_value = closing + 1};

            // The core invariant: the queue signals the closing epoch's value AFTER everything recorded in it.
            // A commit is already queued ahead of this signal, so reaching the value means that work is done.
            _queue->signalEvent(_epoch_event, closing);
        });

    _current.store(closing + 1, std::memory_order_release);
}

void metal_epoch_system::retire_completed()
{
    // Finalizers run outside the lock: one may release a resource whose destructor stages another deferral, and taking
    // this mutex again from inside it would deadlock.
    auto finalizers = cc::vector<cc::unique_function<void()>>();

    _mutex.lock(
        [&](int&)
        {
            auto const reached = _epoch_event->signaledValue();
            while (!_in_flight.empty() && _in_flight.front().epoch_value <= reached)
            {
                auto payload = _in_flight.pop_front();

                for (auto* allocator : payload.allocators)
                {
                    allocator->reset();
                    _free_allocators.push_back(allocator);
                }
                for (auto& f : payload.finalizers)
                    finalizers.push_back(cc::move(f));
            }
        });

    for (auto& f : finalizers)
        f();
}

int metal_epoch_system::in_flight_count()
{
    return _mutex.lock([&](int&) { return int(_in_flight.size()); });
}

void metal_epoch_system::wait_for(sg::epoch e)
{
    wait_for_value(_epoch_event, u64(e));
    retire_completed();
}

void metal_epoch_system::wait_for_next_inflight()
{
    auto const oldest = _mutex.lock([&](int&) -> u64 { return _in_flight.empty() ? 0 : _in_flight.front().epoch_value; });

    if (oldest == 0)
        return;

    wait_for_value(_epoch_event, oldest);
    retire_completed();
}

void metal_epoch_system::block_until_submissions_complete()
{
    // The submission timeline's newest issued value, not the epoch one: a list committed in the open epoch has no epoch
    // value to wait on yet, and this has to cover it.
    auto const issued = _next_submission.load(std::memory_order_acquire);
    if (issued > u64(sg::submission_token::first))
        wait_for_value(_submission_event, issued - 1);

    retire_completed();
}

sg::submission_token metal_epoch_system::claim_submission_token()
{
    return sg::submission_token(_next_submission.fetch_add(1, std::memory_order_acq_rel));
}

void metal_epoch_system::signal_submission(sg::submission_token token)
{
    _queue->signalEvent(_submission_event, u64(token));
}

bool metal_epoch_system::is_submission_complete(sg::submission_token token) const
{
    if (token == sg::submission_token::invalid)
        return true;
    if (token == sg::submission_token::not_submitted)
        return false;
    // Shutdown drained the queue before releasing the timeline, so every token issued before it is complete.
    if (_is_shut_down)
        return true;
    return _submission_event->signaledValue() >= u64(token);
}

MTL4::CommandAllocator* metal_epoch_system::lease_allocator()
{
    auto* const recycled = _mutex.lock(
        [&](int&) -> MTL4::CommandAllocator*
        {
            if (_free_allocators.empty())
                return nullptr;
            auto* const a = _free_allocators.back();
            _free_allocators.remove_back();
            return a;
        });

    if (recycled != nullptr)
        return recycled;

    auto* const fresh = _device->newCommandAllocator();
    CC_ASSERT(fresh != nullptr, "the device refused a command allocator");
    return fresh;
}

void metal_epoch_system::retire_allocator_with_epoch(MTL4::CommandAllocator* allocator)
{
    CC_ASSERT(allocator != nullptr, "cannot retire a null allocator");
    _mutex.lock([&](int&) { _open.allocators.push_back(allocator); });
}

void metal_epoch_system::defer(cc::unique_function<void()> finalizer)
{
    _mutex.lock([&](int&) { _open.finalizers.push_back(cc::move(finalizer)); });
}

void metal_epoch_system::shutdown()
{
    if (_is_shut_down)
        return;
    _is_shut_down = true;

    // Drain first: an allocator or a resource released below may still be named by a command buffer in flight.
    block_until_submissions_complete();

    auto finalizers = cc::vector<cc::unique_function<void()>>();
    auto allocators = cc::vector<MTL4::CommandAllocator*>();

    _mutex.lock(
        [&](int&)
        {
            auto take = [&](metal_epoch_payload& p)
            {
                for (auto* a : p.allocators)
                    allocators.push_back(a);
                for (auto& f : p.finalizers)
                    finalizers.push_back(cc::move(f));
                p.allocators.clear();
                p.finalizers.clear();
            };

            while (!_in_flight.empty())
            {
                auto payload = _in_flight.pop_front();
                take(payload);
            }
            take(_open);

            for (auto* a : _free_allocators)
                allocators.push_back(a);
            _free_allocators.clear();
        });

    for (auto& f : finalizers)
        f();
    for (auto* a : allocators)
        a->release();

    _epoch_event->release();
    _submission_event->release();
    _epoch_event = nullptr;
    _submission_event = nullptr;
}
} // namespace sg::backend::metal
