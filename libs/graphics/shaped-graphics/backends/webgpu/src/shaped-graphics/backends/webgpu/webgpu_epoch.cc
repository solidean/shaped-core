// webgpu epoch system: advance and retire, submission completion, and deferred release.
//
// Every signal here comes from `queue.onSubmittedWorkDone`: one registration per submit carries the submission token,
// and one per advance carries the closed epoch.
// The queue calls back in the order the work was queued, which is why a single counter per kind is enough.

#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/webgpu/webgpu_context.hh>

namespace sg::backend::webgpu
{
namespace
{
/// What one work-done callback carries: the anchor it reaches the context through, and what it completes.
struct queue_done_request
{
    std::shared_ptr<webgpu_callback_anchor> anchor;
    u64 submission = 0;
    u64 epoch = 0;
};

void on_submitted_work_done(WGPUQueueWorkDoneStatus status, WGPUStringView, void* userdata1, void*)
{
    auto const request = std::unique_ptr<queue_done_request>(static_cast<queue_done_request*>(userdata1));
    auto* const ctx = request->anchor->ctx;
    if (ctx == nullptr)
        return;

    // A cancelled or failed callback means the device is going away, and a lost device settles every completion as an error anyway.
    // Raising the counters regardless keeps retirement from stalling on work that will never be reported.
    (void)status;
    ctx->on_queue_done(request->submission, request->epoch);
}
} // namespace

void webgpu_context::notify_when_queue_done(u64 submission, u64 epoch)
{
    auto request = std::make_unique<queue_done_request>(queue_done_request{
        .anchor = _anchor,
        .submission = submission,
        .epoch = epoch,
    });
    auto const info = WGPUQueueWorkDoneCallbackInfo{
        .nextInChain = nullptr,
        .mode = WGPUCallbackMode_AllowSpontaneous,
        .callback = on_submitted_work_done,
        .userdata1 = request.release(),
        .userdata2 = nullptr,
    };
    (void)wgpuQueueOnSubmittedWorkDone(queue(), info);
}

void webgpu_context::on_queue_done(u64 submission, u64 epoch)
{
    if (submission > _epochs.completed_submission)
        _epochs.completed_submission = submission;
    if (epoch > _epochs.completed_epoch)
        _epochs.completed_epoch = epoch;
    settle_due_completions();
}

void webgpu_context::advance_epoch()
{
    CC_ASSERT(!_is_shut_down, "cannot advance a shut-down context");
    CC_ASSERT(_open_command_lists == 0, "all command lists opened this epoch must be submitted or dropped before "
                                        "advancing");

    // What other threads dropped since the last advance: their releases and their deferred deletions land in `last`.
    impl::drain_deferred_to_main();

    auto const last = _current_epoch;
    _current_epoch = sg::epoch(u64(last) + 1);

    // The transient resources of `last` expire now: a handle held past its epoch must report itself expired.
    auto const buffers = cc::move(_transient_buffers);
    _transient_buffers = {};
    for (auto const& weak : buffers)
        if (auto const b = weak.lock())
            b->expire();
    auto const textures = cc::move(_transient_textures);
    _transient_textures = {};
    for (auto const& weak : textures)
        if (auto const t = weak.lock())
            t->expire();

    auto data = webgpu_epoch_data{.epoch_id = last, .expiring = cc::move(_epochs.staged)};
    _epochs.staged = {};
    _epochs.in_flight.push_back(cc::move(data));

    // The epoch's fence: everything queued so far belongs to `last` or earlier.
    notify_when_queue_done(0, u64(last));

    apply_pending_transient_budget();
}

void webgpu_context::retire_completed_epochs()
{
    auto finalizers = cc::vector<cc::unique_function<void()>>();
    while (!_epochs.in_flight.empty() && u64(_epochs.in_flight.front().epoch_id) <= _epochs.completed_epoch)
    {
        auto data = _epochs.in_flight.pop_front();
        for (auto& res : data.expiring)
        {
            // Destroy frees the GPU memory now rather than when the last reference goes, which a caller holding an
            // expired handle would otherwise postpone indefinitely.
            if (res.buffer)
                wgpuBufferDestroy(res.buffer.get());
            if (res.texture)
                wgpuTextureDestroy(res.texture.get());
            for (auto& f : res.finalizers)
                finalizers.push_back(cc::move(f));
        }
    }

    // After the objects are gone, so a finalizer never observes a live resource.
    for (auto& f : finalizers)
        f();
}

bool webgpu_context::is_submission_complete(sg::submission_token token) const
{
    if (token == sg::submission_token::not_submitted)
        return false;
    return u64(token) <= _epochs.completed_submission;
}

sg::submission_token webgpu_context::last_issued_submission()
{
    if (_next_submission <= u64(sg::submission_token::first))
        return sg::submission_token::not_submitted;
    return sg::submission_token(_next_submission - 1);
}

void webgpu_context::schedule_deferred_deletion(webgpu_expiring_resource expiring)
{
    // The staging list belongs to the device thread; a resource dropped elsewhere joins it there, at the next advance.
    if (!is_on_device_thread())
    {
        impl::defer_to_main([this, expiring = cc::move(expiring)]() mutable
                            { schedule_deferred_deletion(cc::move(expiring)); });
        return;
    }

    // Past shutdown nothing will retire again, and the device is gone, so the only thing left to do is run the finalizers.
    if (_is_shut_down)
    {
        for (auto& f : expiring.finalizers)
            f();
        return;
    }
    _epochs.staged.push_back(cc::move(expiring));
}
} // namespace sg::backend::webgpu
