// dx12_download_inline_system: the inline READBACK ring buffer plus the actor that performs deferred
// CPU copies. Downloads record a GPU readback copy at record time; at submit the deferred copies are
// stamped with the list's submission token and enqueued on a cc::threaded_actor. The actor blocks on
// the submission fence, memcpys the readback bytes into the destination (skipping it if the future was
// dropped), marks the waiter ready, and frees the ring space.
//
// TODO: the free watermark advances monotonically in actor (== submit) order, which matches the ring
// allocation order only when lists are submitted in the order their space was reserved (true for a
// single recording thread). Robust concurrent multi-list ordering wants per-region completion tracking
// / the split GPU-CPU download watermarks — deferred (see the epochs concept doc).

#include <clean-core/error/optional.hh>
#include <shaped-graphics/backends/dx12/dx12_command_list.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/backends/dx12/dx12_download_inline.hh>
#include <shaped-graphics/backends/dx12/dx12_resource_download.hh>

namespace sg::backend::dx12
{
namespace
{
/// The download system's async agent: one thread that drains readback copies in submit order.
class dx12_download_actor final : public cc::threaded_actor_impl<dx12_download_copy_job>
{
public:
    explicit dx12_download_actor(dx12_download_inline_system& sys) : _sys(sys) {}

    [[nodiscard]] cc::string_view actor_name() const noexcept override { return "sg-dx12-download"; }

protected:
    void on_message(dx12_download_copy_job job) override
    {
        _sys.wait_for_submission(job.token);   // block until the recording list has run on the GPU
        if (auto const alive = job.pin.lock()) // future still wants the data?
            job.deferred_cpu_copy();
        if (job.waiter)
            job.waiter->mark_ready();
        _sys.advance_free_watermark(job.end_pos);
    }

private:
    dx12_download_inline_system& _sys;
};
} // namespace

void dx12_download_inline_system::initialize(ComPtr<ID3D12Resource> buffer,
                                             cc::byte* mapped,
                                             cc::isize capacity,
                                             HANDLE wait_event)
{
    CC_ASSERT(capacity > 0, "download ring capacity must be positive");
    _buffer = cc::move(buffer);
    _mapped = mapped;
    _capacity = capacity;
    _wait_event = wait_event;
    _actor = cc::make_and_start_threaded_actor<dx12_download_actor>(*this);
}

dx12_download_inline_system::reservation dx12_download_inline_system::reserve(cc::isize size)
{
    CC_ASSERT(size > 0, "reserve size must be positive");
    CC_ASSERT(size <= _capacity, "a single inline download exceeds the readback ring capacity");

    for (;;)
    {
        cc::optional<reservation> r = _ring.lock(
            [&](ring_state& s) -> cc::optional<reservation>
            {
                cc::u64 start = s.next_pos;
                cc::isize offset = cc::isize(start % cc::u64(_capacity));
                if (offset + size > _capacity) // would wrap: waste the tail, restart at 0
                {
                    start += cc::u64(_capacity - offset);
                    offset = 0;
                }
                cc::u64 const end = start + cc::u64(size);
                if (end - _freed_pos.load(std::memory_order_acquire) > cc::u64(_capacity))
                    return {}; // space still held by earlier, not-yet-copied downloads
                s.next_pos = end;
                return reservation{offset, end};
            });

        if (r.has_value())
            return r.value();

        // Wait for the actor to free space. This blocks the recording thread; it is only safe because
        // the occupied space belongs to already-submitted lists that the actor is draining. A single
        // list whose own downloads exceed the ring would deadlock here (a documented v1 limitation).
        cc::u64 const seen = _freed_pos.load(std::memory_order_acquire);
        _freed_pos.wait(seen, std::memory_order_acquire);
    }
}

sg::bytes_future dx12_download_inline_system::download_buffer(dx12_command_list& cmd,
                                                              dx12_buffer const& src,
                                                              cc::isize offset,
                                                              cc::isize size)
{
    if (size == 0)
        return sg::bytes_future(cc::span<cc::byte const>(), nullptr, std::make_shared<sg::ready_bytes_waiter>());

    CC_ASSERT(_mapped != nullptr, "download system used before initialization");

    // Destination the readback bytes land in; the pin keeps it alive until the copy runs (or cancels).
    std::shared_ptr<cc::byte[]> dst = std::make_shared<cc::byte[]>(std::size_t(size));
    cc::span<cc::byte> const dst_span(dst.get(), size);
    auto waiter = std::make_shared<dx12_download_waiter>();

    reservation const res = reserve(size);
    dx12_buffer_download download(src, offset, dst_span);
    download.prepare(cmd);
    dx12_download_allocation const alloc{_buffer.Get(), _mapped, res.offset, size};
    dx12_pending_copy pending = download.execute_next_job(*cmd._list.Get(), alloc);
    CC_ASSERT(download.is_finished(), "buffer download did not complete in one job");

    dx12_download_copy_job job;
    job.deferred_cpu_copy = cc::move(pending.deferred_cpu_copy);
    job.pin = std::weak_ptr<void>(dst);
    job.waiter = waiter;
    job.end_pos = res.end_pos;
    cmd._pending_downloads.push_back(cc::move(job));

    return sg::bytes_future(cc::span<cc::byte const>(dst.get(), size), std::shared_ptr<void>(dst), cc::move(waiter));
}

void dx12_download_inline_system::enqueue_submitted(sg::submission_token token, cc::vector<dx12_download_copy_job>& jobs)
{
    for (auto& job : jobs)
    {
        job.token = token;
        if (job.waiter)
            job.waiter->submitted.store(true, std::memory_order_release);
        _actor->enqueue_message(cc::move(job));
    }
    jobs.clear();
}

void dx12_download_inline_system::discard_unsubmitted(cc::vector<dx12_download_copy_job>& jobs)
{
    // The space was reserved in allocation order; freeing to each end_pos reclaims it (single-thread
    // recording keeps these the newest reservations — see the ordering note atop this file).
    for (auto const& job : jobs)
        advance_free_watermark(job.end_pos);
    jobs.clear();
}

void dx12_download_inline_system::wait_for_submission(sg::submission_token token)
{
    if (_ctx.is_submission_complete(token))
        return;
    if (_ctx._submission_fence && _wait_event)
    {
        HRESULT const hr = _ctx._submission_fence->SetEventOnCompletion(cc::u64(token), _wait_event);
        CC_ASSERT(SUCCEEDED(hr), "ID3D12Fence::SetEventOnCompletion failed");
        WaitForSingleObject(_wait_event, INFINITE);
    }
}

void dx12_download_inline_system::advance_free_watermark(cc::u64 end_pos)
{
    cc::u64 prev = _freed_pos.load(std::memory_order_acquire);
    while (end_pos > prev && !_freed_pos.compare_exchange_weak(prev, end_pos, std::memory_order_acq_rel))
        ; // prev is refreshed on failure
    _freed_pos.notify_all();
}

void dx12_download_inline_system::shutdown()
{
    if (_actor)
    {
        _actor->shutdown(); // drains queued copies (their GPU work is already idle by this point)
        _actor = nullptr;
    }
    if (_buffer)
    {
        _buffer->Unmap(0, nullptr);
        _buffer.Reset();
    }
    _mapped = nullptr;
    if (_wait_event)
    {
        CloseHandle(_wait_event);
        _wait_event = nullptr;
    }
}
} // namespace sg::backend::dx12
