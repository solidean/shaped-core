#include "metal_context.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/record/log.hh>
#include <clean-core/thread/thread_pump.hh>
#include <shaped-graphics/backends/metal/metal_acceleration_structure.hh>
#include <shaped-graphics/backends/metal/metal_buffer.hh>
#include <shaped-graphics/exceptions.hh>

#include <thread>

// Seams the milestone order has not reached; see libs/graphics/shaped-graphics/docs/writing-a-backend.md.
#define SG_METAL_UNIMPLEMENTED(what) CC_UNREACHABLE(what " is not implemented in the metal backend yet")

namespace sg::backend::metal
{
CC_REC_DEFINE_DOMAIN(g_rec_domain, "sg.metal");

metal_context::metal_context(MTL::Device* device,
                             MTL4::CommandQueue* queue,
                             MTL4::Compiler* compiler,
                             MTL::SharedEvent* epoch_event,
                             MTL::SharedEvent* submission_event)
  : sg::context(sg::backend_kind::metal, sg::thread_model::multi_threaded, k_accepted_shader_formats),
    _device(device),
    _queue(queue),
    _compiler(compiler),
    _epochs(device, queue, epoch_event, submission_event)
{
    CC_ASSERT(_device != nullptr && _queue != nullptr, "a metal context needs a device and a queue");
    CC_ASSERT(_compiler != nullptr, "a metal context needs a compiler");
    _feedback = std::make_shared<metal_feedback_sink>(*this);

    // The listener the completion-signal waiter arms its notifications on.
    // Its default queue is Apple's, which is the point: a handler must run even in a build with no threads of ours.
    _completion.listener = MTL::SharedEventListener::alloc()->init();
}

cc::result<cc::unit> metal_context::create_systems(isize upload_bytes, isize download_bytes)
{
    // Before the rings, so their own buffers can declare themselves resident as they are made.
    CC_RETURN_IF_ERROR(_residency.create(_device, _queue));
    CC_RETURN_IF_ERROR(_transfers.create(*this));
    _streams.create(*this);

    CC_RETURN_IF_ERROR(_upload_ring.create(_device, upload_bytes, "sg inline upload ring"));
    CC_RETURN_IF_ERROR(_download_ring.create(_device, download_bytes, "sg inline download ring"));

    _residency.add(_upload_ring.buffer());
    _residency.add(_download_ring.buffer());
    return cc::unit{};
}

void metal_context::report_feedback_error(sg::device_error_kind kind, cc::string_view message)
{
    if (kind == sg::device_error_kind::device_lost)
        mark_device_lost(cc::string(message));
    else
        report_device_error({.kind = kind, .message = cc::string(message)});
}

metal_context::~metal_context()
{
    shutdown_no_throw();
}

bool metal_context::supports(sg::feature f) const
{
    switch (f)
    {
    case sg::feature::raytracing:
        // Every device above this backend's Metal 4 floor can ray trace, so there is nothing to probe.
        // Acceleration structures build, a tlas binds for an inline RayQuery trace, and the DXR-shaped pipeline path
        // maps onto a compute pipeline per raygen plus Metal's function tables.
        return true;
    case sg::feature::headless_present:
        // Always: the chain is emulated with ordinary render targets, so there is no surface extension to be missing.
        return true;
    case sg::feature::timestamp_query:
        return false;
    case sg::feature::geometry_shader:
    case sg::feature::tessellation_shader:
        // Metal has never had either stage; a caller asking gets a permanent answer rather than a temporary one.
        return false;
    }
    return false;
}

void metal_context::advance_epoch()
{
    // Before any state change, so a caller catching this still has a usable context.
    CC_ASSERT(_slots.live_count() == 0, "all command lists opened this epoch must be submitted or dropped before "
                                        "advancing");

    // Auto-expire the closing epoch's transient resources before the epoch is packaged.
    //
    // The transient bump heap resets its head every epoch, so the next epoch's first allocation aliases these bytes.
    // A handle held across the advance therefore has to report itself expired rather than name storage that now
    // belongs to something else.
    auto const expiring = _transient_expiring.lock(
        [](cc::vector<std::weak_ptr<sg::raw_buffer const>>& v)
        {
            auto out = cc::move(v);
            v.clear();
            return out;
        });

    auto const expiring_textures = _transient_expiring_textures.lock(
        [](cc::vector<std::weak_ptr<sg::raw_texture const>>& v)
        {
            auto out = cc::move(v);
            v.clear();
            return out;
        });

    // Outside the lock: expire() runs the resource's finalizers, which stage a deferred release and take the epoch
    // system's own lock.
    for (auto const& weak : expiring)
        if (auto const buffer = weak.lock())
            buffer->expire();
    for (auto const& weak : expiring_textures)
        if (auto const texture = weak.lock())
            texture->expire();

    // The rings' bytes were read (or written) by copies recorded in the closing epoch, so they are only reclaimable
    // once that epoch retires.
    // Each ring records its own checkpoint here and frees it when told the epoch completed, so nothing outside carries
    // a boundary that could go stale — see metal_staging_ring.
    auto const closing = current_epoch();
    _upload_ring.on_epoch_advance(closing);
    _download_ring.on_epoch_advance(closing);
    _epochs.defer(
        [this, closing]
        {
            _upload_ring.on_epochs_completed(closing);
            _download_ring.on_epochs_completed(closing);
        });

    _epochs.advance();
    apply_pending_transient_budget();
}

sg::submission_token metal_context::submit_command_list(std::unique_ptr<sg::command_list> cmd)
{
    CC_ASSERT(cmd != nullptr, "cannot submit a null command list");
    auto& list = static_cast<metal_command_list&>(*cmd);
    CC_ASSERT(list.created_in_epoch() == current_epoch(), "a command list must be submitted in the epoch it was opened "
                                                          "in");

    auto const scope = autorelease_scope();

    list.end_recording();

    // **Wait, finalize, claim, commit and signal are one step in a single global order.**
    //
    // The queue is free-threaded, and that is the hazard: two threads may claim tokens 5 and 6 and reach the commit in
    // the other order, which either releases a waiter on 5 before list 5 has run, or drives the shared event backwards
    // from 6 to 5 and breaks is_submission_complete.
    // The waits are inside for a sharper reason — an MTL4 queue wait applies to what is committed after it, so a wait
    // separated from its own commit by another thread's is a wait that list never gets.
    // Finalize is inside for a third — finalize order must equal execute order, which is what makes a resource's
    // `current` mean "after everything submitted so far".
    // Work that neither orders against another submit nor names a token runs after the lock.
    auto* const buffer = list.buffer();
    auto* const allocator = list.allocator();
    auto* const argument_table = list.take_argument_table();

    auto const token = _submission.lock(
        [&](int&)
        {
            finalize_touched_buffers(list);

            // **A list waits for the submissions that last named what it touches, on the submission timeline.**
            //
            // The queue barrier pair an encoder opens and closes with is not what orders two command buffers, however
            // much it reads like it: a queue wait between the two commits — which is exactly what the transfer wait
            // below is — leaves the later encoder's `barrierAfterQueueStages` with no earlier work to find, and its
            // read of a buffer the earlier list wrote returns the bytes from before.
            // tests/barrier/cross-list-ordering-test.cc is that sequence, and it fails without this wait.
            //
            // Per-resource rather than a blanket serialization: two lists touching nothing in common still run
            // concurrently, and the stamp is read here, before `stamp_touched_resources` raises it to this submission.
            if (auto const wait = highest_prior_submission(list); wait > 0)
                _queue->wait(_epochs.submission_timeline(), wait);

            // Order this list after every off-frame transfer of a resource it touches.
            //
            // The two queues are otherwise independent: an async upload committed to the transfer queue has no
            // relationship to a command list committed to the direct one, so a list reading a buffer an upload is
            // still filling would read whatever was there.
            // One wait covers the whole list, on the highest value any of its resources claimed.
            if (auto const wait = highest_pending_transfer(list); wait > 0)
                _queue->wait(_transfers.timeline(), wait);

            wait_for_streams(list);

            // Claimed inside, so the stamp lands before submit returns: a caller that issues an async transfer on the
            // very next line must find this list already named.
            auto const claimed = _epochs.claim_submission_token();
            stamp_touched_resources(list, claimed);
            list.release_ownership();

            // Every commit carries a feedback handler, because it is the only channel Metal has for a failure that
            // arrives after the call that caused it — the validation layer speaks only to stderr.
            // The handler captures the sink rather than this context: it runs on a dispatch queue at a time nothing
            // here controls, which can be after shutdown.
            // See metal_feedback.hh.
            auto sink = _feedback;

            // The list's downloads: their bytes are in the staging ring and become readable when this commit
            // completes, which is precisely when the feedback handler runs.
            auto downloads
                = std::make_shared<cc::vector<metal_command_list::pending_download>>(list.take_pending_downloads());
            auto const has_downloads = !downloads->empty();
            auto* const pending_counter = &_pending_downloads;
            if (has_downloads)
                pending_counter->fetch_add(1, std::memory_order_acq_rel);

            auto* const options = MTL4::CommitOptions::alloc()->init();
            options->addFeedbackHandler(^void(MTL4::CommitFeedback* feedback) {
              auto* const error = feedback->error();
              if (error != nullptr)
                  sink->report(device_error_kind_of(NS::UInteger(error->code())),
                               describe_error(error, "a metal command buffer failed"));

              for (auto& download : *downloads)
              {
                  download.copy_out();
                  download.settle_staging(); // the ring's span and any overflow buffer go back here, not on the epoch
              }
              downloads->clear();

              // Reaching zero is what `are_transfers_drained` reports, so the completion machinery has to be told.
              // Routed through the sink because this runs on Apple's queue, possibly after shutdown — and only with
              // threads, since the singlethreaded pump polls `settle_due_completions` itself.
              if (has_downloads && pending_counter->fetch_sub(1, std::memory_order_acq_rel) == 1)
              {
#if CC_HAS_THREADS
                  sink->notify_drained();
#endif
              }
            });

            MTL4::CommandBuffer const* const buffers[] = {buffer};
            _queue->commit(buffers, 1, options);
            options->release();

            _epochs.signal_submission(claimed);
            return claimed;
        });

    // The allocator rides the epoch rather than going back to the pool here: resetting it while the buffer just
    // committed is still executing is exactly what MTL4 forbids.
    _epochs.retire_allocator_with_epoch(allocator);
    _epochs.defer([buffer] { buffer->release(); });

    // The table holds the addresses the encoded commands read as they run, so it outlives the recording by exactly as
    // long as the buffer does.
    if (argument_table != nullptr)
        _epochs.defer([argument_table] { argument_table->release(); });

    _slots.release(list.slot());

    return token;
}

void metal_context::drop_command_list(std::unique_ptr<sg::command_list> cmd)
{
    CC_ASSERT(cmd != nullptr, "cannot drop a null command list");
    auto& list = static_cast<metal_command_list&>(*cmd);

    auto const scope = autorelease_scope();

    list.end_recording();

    auto* const buffer = list.buffer();
    auto* const allocator = list.allocator();
    list.release_ownership();

    list.abandon_recording();

    // Nothing was committed, so the GPU never saw any of these and they all go back immediately.
    if (auto* const table = list.take_argument_table(); table != nullptr)
        table->release();
    buffer->release();
    allocator->reset();
    _epochs.retire_allocator_with_epoch(allocator);

    _slots.release(list.slot());
}

u64 metal_context::highest_pending_transfer(metal_command_list& list) const
{
    u64 highest = 0;
    for (auto const& touched : list.touched_buffers())
        highest = cc::max(highest, _transfers.pending_value_for(*touched));
    for (auto const& touched : list.touched_textures())
        highest = cc::max(highest, _transfers.pending_value_for(*touched));
    return highest;
}

void metal_context::finalize_touched_buffers(metal_command_list& list)
{
    // Every touched buffer's state moves into its `current`, in submission order — that ordering is what makes
    // `current` mean "after everything submitted so far".
    //
    // The entry barrier each finalize computes is discarded rather than emitted: the list has already ordered its own
    // encoder against the queue (see metal_command_list::flush_barriers), and there is no way to prepend a barrier to
    // a command buffer that is already recorded.
    // Keeping the finalize is still what carries a write across lists, which is the half Metal genuinely needs.
    for (auto const& touched : list.touched_buffers())
    {
        auto const& mtl_buffer = static_cast<metal_buffer const&>(*touched);
        (void)mtl_buffer.access().lock([&](metal_resource_access& a) { return a.finalize(list.slot()); });
    }
    for (auto const& touched : list.touched_textures())
    {
        auto const& mtl_texture = static_cast<metal_texture const&>(*touched);
        (void)mtl_texture.access().lock([&](metal_resource_access& a) { return a.finalize(list.slot()); });
    }
    for (auto const& touched : list.touched_accels())
        (void)touched.storage->access().lock([&](metal_resource_access& a) { return a.finalize(list.slot()); });
}

void metal_context::wait_for_streams(metal_command_list& list)
{
    auto const wait_on = [&](metal_transfer_system::stream_wait const& wait, auto const& resource)
    {
        if (wait.event == nullptr)
            return;

        if (resource->claim_stream_wait_warning(wait.value))
            CC_LOG_WARNING("a command list is waiting on an in-flight streaming transfer, which stalls it until the "
                           "whole transfer lands. Wait on the stream handle yourself before using the resource, or "
                           "call promote_to_async on it if the wait is what you want");

        _queue->wait(wait.event, wait.value);
    };

    for (auto const& touched : list.touched_buffers())
        wait_on(_transfers.pending_stream_wait(touched.get()), touched);
    for (auto const& touched : list.touched_textures())
        wait_on(_transfers.pending_stream_wait(touched.get()), touched);
}

u64 metal_context::highest_prior_submission(metal_command_list& list) const
{
    u64 highest = 0;
    for (auto const& touched : list.touched_buffers())
        highest = cc::max(highest, static_cast<metal_buffer const&>(*touched).submission().get());
    for (auto const& touched : list.touched_textures())
        highest = cc::max(highest, static_cast<metal_texture const&>(*touched).submission().get());
    for (auto const& touched : list.touched_accels())
        highest = cc::max(highest, touched.storage->submission().get());
    return highest;
}

void metal_context::stamp_touched_resources(metal_command_list& list, sg::submission_token token)
{
    for (auto const& touched : list.touched_buffers())
        static_cast<metal_buffer const&>(*touched).submission().raise(u64(token));
    for (auto const& touched : list.touched_textures())
        static_cast<metal_texture const&>(*touched).submission().raise(u64(token));
    for (auto const& touched : list.touched_accels())
        touched.storage->submission().raise(u64(token));
}

cc::result<std::unique_ptr<sg::command_list>> metal_context::try_create_command_list()
{
    if (is_device_lost())
        return cc::error("the metal device has been lost");

    auto const scope = autorelease_scope();

    auto* const allocator = _epochs.lease_allocator();
    if (allocator == nullptr)
        return cc::error("the metal device refused a command allocator");

    auto* const buffer = _device->newCommandBuffer();
    if (buffer == nullptr)
    {
        _epochs.retire_allocator_with_epoch(allocator);
        return cc::error("the metal device refused a command buffer");
    }

    return std::unique_ptr<sg::command_list>(new metal_command_list(*this, current_epoch(), allocator, buffer));
}

void metal_context::shutdown()
{
    if (is_shut_down())
        return;

    auto const scope = autorelease_scope();

    // Routines first: they may cache epoch-managed resources that must be freed before the systems below go.
    routines.clear();

    // Both of these are device memory the context caches for its own lifetime, and Metal's reference counting does not
    // order them against the device — which is the category the vulkan build-out found the hard way, one validation
    // message at a time.
    release_transient_heap();
    release_cached_pipelines();

    // Close the final epoch and drain, so every deferred release runs while the device is still alive.
    advance_epoch();
    block_until_idle();

    // Fails every completion still outstanding, so nothing parks on a timeline that is about to go.
    stop_completion_signals();

    // Before the device and the queue: a handler still in flight would otherwise report into a context being torn down.
    _feedback->detach();

    // **The two transfer tiers go down before the epoch system, not after it.**
    // Both settle their in-flight work by handing resources to `defer`, and `_epochs.shutdown()` is the only thing
    // that ever sweeps those — a job another thread admitted during shutdown would drop its buffers into a payload
    // nothing runs again, so they leak and their finalizers never fire.
    //
    // Within the pair: the stream actor commits onto the transfer system's queue, so a job still in flight would name
    // a queue that is already gone.
    _streams.shutdown();
    _transfers.shutdown();

    _epochs.shutdown();

    // After the epoch shutdown drained the queue, so no notification handler is still due to run.
    if (_completion.listener != nullptr)
    {
        _completion.listener->release();
        _completion.listener = nullptr;
    }

    // After the drain above, so nothing in flight still names these bytes.
    _upload_ring.shutdown();
    _download_ring.shutdown();
    _samplers.shutdown();
    _texture_views.shutdown();

    if (_compiler != nullptr)
    {
        _compiler->release();
        _compiler = nullptr;
    }

    _residency.shutdown();

    _queue->release();
    _queue = nullptr;

    _device->release();
    _device = nullptr;

    // The flag directly rather than sg::context::shutdown(), which would re-run the three releases above — after the
    // device is gone, which is the wrong order even where they are idempotent.
    _is_shut_down = true;
}

void metal_context::shutdown_no_throw() noexcept
{
    // The teardown still has to run, and nothing above a destructor can act on a failure, so the only honest outcome is
    // to finish and say what happened.
    try
    {
        shutdown();
    }
    catch (sg::device_lost_exception const& e)
    {
        CC_LOG_ERROR("context shutdown on a lost device: {}", e.reason());
    }
    catch (sg::exception const& e)
    {
        CC_LOG_ERROR("context shutdown failed: {}", e.message());
    }
    catch (...)
    {
        CC_LOG_ERROR("context shutdown failed with an unknown exception");
    }
}

void metal_context::block_until_transfers_drained()
{
    // Every loop here pumps, because an unthreaded build runs the streaming actor on whoever waits — so a wait that
    // only yields is a wait for something nothing will ever do.
    //
    // A condition rather than a duration throughout: the handlers run on a dispatch queue we do not own, so there is
    // nothing to join and nothing whose timing is ours to predict.
    auto const spin = [](auto&& still_pending)
    {
        while (still_pending())
        {
            if (!cc::thread_pump_all())
                std::this_thread::yield();
        }
    };

    // Streaming first: its jobs commit onto the transfer queue, so draining it can add to what the next loop waits on.
    spin([&] { return _streams.has_pending(); });
    spin([&] { return _transfers.has_pending(); });
    spin([&] { return _pending_downloads.load(std::memory_order_acquire) > 0; });
}

bool metal_context::are_transfers_drained() const
{
    // block_until_transfers_drained's loop condition, without the loop — the three the drain spins on, in the same
    // order and for the same reasons.
    return !_streams.has_pending() && !_transfers.has_pending()
        && _pending_downloads.load(std::memory_order_acquire) == 0;
}

sg::submission_token metal_context::last_issued_submission()
{
    return _epochs.last_issued_submission();
}

void metal_context::wait_for_completion_signal(u64 submission, u64 epoch, u64 wake_generation)
{
    // **Two GPU timelines and a host wake, and Metal cannot wait on several at once.**
    // Vulkan parks one `vkWaitSemaphores` with WAIT_ANY over all three; `MTL::SharedEvent::waitUntilSignaledValue`
    // takes a single event, so a waiter built on it would miss whichever of the others fired first.
    // What Metal has instead is `notifyListener`, which calls back when a timeline reaches a value — so every source
    // raises one generation under one condition, and the waiter parks on that.
    auto* const submission_event = _epochs.submission_timeline();
    auto* const epoch_event = _epochs.epoch_timeline();
    if (submission_event == nullptr || epoch_event == nullptr || _completion.listener == nullptr)
        return;

    auto const reached = [&]
    {
        return (submission != 0 && submission_event->signaledValue() >= submission)
            || (epoch != 0 && epoch_event->signaledValue() >= epoch);
    };

    // Already satisfied, so there is nothing to arm and nothing to park on.
    if (reached())
        return;

    // Arm each timeline at most once per target: a notification already pending for this value fires either way, and
    // re-arming would pile up handlers that outlive the wait.
    // The completion waiter is this seam's only caller, which is what lets the armed values live without a lock.
    // Armed whenever the target *differs* from what is armed, not only when it grows.
    // The portable layer wakes this waiter precisely when a new target is below the armed one, so a `>=` test would
    // leave that lower target unarmed and park on the higher one — `epoch_completion(4)` after `epoch_completion(5)`
    // would wait for 5.
    // A duplicate notification is harmless: it raises the generation, and one spurious wake costs one re-check.
    //
    // The block captures the detachable sink rather than `this`, for the reason every commit handler does: it runs on
    // a queue Apple owns, at a time that can be after shutdown released the listener.
    auto sink = _feedback;
    auto const arm = [this, &sink](MTL::SharedEvent* event, u64 value, u64& armed)
    {
        if (value == 0 || armed == value)
            return;
        armed = value;
        event->notifyListener(_completion.listener, value,
                              [sink](MTL::SharedEvent*, u64) { sink->notify_completion_signal(); });
    };

    arm(submission_event, submission, _completion.armed_submission);
    arm(epoch_event, epoch, _completion.armed_epoch);

    // A GPU signal or a host wake, never a timeout.
    //
    // **Each source releases exactly one wait.**
    // A GPU notification raises `gpu_generation` past what the last wait consumed; a host wake raises
    // `host_generation` past the counter the caller read before parking.
    // Comparing a single counter against the caller's instead is what made this spin: after one GPU notification the
    // predicate stayed true forever, and `run_completion_signal_waiter` loops whether or not anything is pending.
    // A spurious return is harmless by contract, so re-checking the timelines beside the generations costs nothing and
    // closes the window between the check above and the park.
    auto guard = std::unique_lock(_completion.mutex);
    _completion.condition.wait(guard,
                               [&]
                               {
                                   return _completion.gpu_generation != _completion.consumed_gpu_generation
                                       || _completion.host_generation > wake_generation || reached();
                               });
    _completion.consumed_gpu_generation = _completion.gpu_generation;
}

void metal_context::notify_completion_signal()
{
    // On a dispatch queue Apple owns, which SC_THREADS=OFF does not reach — so this mutex and condition are the real
    // ones rather than cc::mutex's compiled-away lock.
    auto const guard = std::lock_guard(_completion.mutex);
    ++_completion.gpu_generation;
    _completion.condition.notify_all();
}

void metal_context::wake_completion_signal(u64 generation)
{
    // The host source, kept apart from the GPU one so neither masks the other.
    // Raised rather than assigned: generations are handed out strictly increasing, and a later wake may already have
    // raised it further.
    auto const guard = std::lock_guard(_completion.mutex);
    _completion.host_generation = cc::max(_completion.host_generation, generation + 1);
    _completion.condition.notify_all();
}

cc::result<sg::swapchain_handle> metal_context::try_create_swapchain(swapchain_description const& desc)
{
    return create_metal_swapchain(desc);
}

sg::texture_layout metal_context::async_ready_layout(async_direction) const
{
    // Metal textures have no layouts at all, so `general` is not a placeholder here the way it is on the other two
    // backends — it is the only thing a metal texture is ever in.
    return sg::texture_layout::general;
}

sg::texture_layout metal_context::current_texture_layout(raw_texture_handle const&, subresource_range const&) const
{
    // A Metal texture has no layout, so `general` is the true and only answer rather than a placeholder — see the
    // barrier translation, where a pure layout transition emits nothing at all.
    return sg::texture_layout::general;
}

void metal_context::async_upload_bytes_to_buffer(raw_buffer_handle buffer,
                                                 cc::pinned_data<byte const> data,
                                                 isize offset_in_bytes)
{
    _transfers.upload_to_buffer(cc::move(buffer), data, offset_in_bytes);
}

void metal_context::async_upload_bytes_to_texture(raw_texture_handle texture,
                                                  cc::pinned_data<byte const> data,
                                                  subresource_index const& subresource,
                                                  texture_region const& region)
{
    _transfers.upload_to_texture(cc::move(texture), data, subresource, region);
}

sg::bytes_future metal_context::async_download_bytes_from_buffer(raw_buffer_handle buffer,
                                                                 isize offset_in_bytes,
                                                                 isize size_in_bytes)
{
    return _transfers.download_from_buffer(cc::move(buffer), offset_in_bytes, size_in_bytes);
}

sg::bytes_future metal_context::async_download_bytes_from_texture(raw_texture_handle texture,
                                                                  subresource_index const& subresource,
                                                                  texture_region const& region)
{
    return _transfers.download_from_texture(cc::move(texture), subresource, region);
}

// The resident forms build a source of one always-ready chunk, so there is one implementation underneath rather than
// a second thing to keep correct.
// `stream_scope` needs nothing here: the narrow scopes are a creation-time property on the other two backends, and a
// Metal resource has no equivalent to declare — sg has already checked the usage flags by this point.

sg::stream_upload_handle metal_context::stream_bytes_to_buffer(raw_buffer_handle buffer,
                                                               cc::pinned_data<byte const> data,
                                                               isize offset_in_bytes,
                                                               stream_scope)
{
    return _streams.upload_to_buffer(cc::move(buffer), sg::make_pinned_stream_source(cc::move(data), 0), offset_in_bytes);
}

sg::stream_upload_handle metal_context::stream_bytes_to_texture(raw_texture_handle texture,
                                                                cc::pinned_data<byte const> data,
                                                                subresource_index const& subresource,
                                                                texture_region const& region,
                                                                stream_scope)
{
    return _streams.upload_to_texture(cc::move(texture), sg::make_pinned_stream_source(cc::move(data), 0), subresource,
                                      region);
}

sg::stream_upload_handle metal_context::stream_source_to_buffer(raw_buffer_handle buffer,
                                                                std::unique_ptr<stream_source> source,
                                                                isize offset_in_bytes,
                                                                stream_scope)
{
    return _streams.upload_to_buffer(cc::move(buffer), cc::move(source), offset_in_bytes);
}

sg::stream_upload_handle metal_context::stream_source_to_texture(raw_texture_handle texture,
                                                                 std::unique_ptr<stream_source> source,
                                                                 subresource_index const& subresource,
                                                                 texture_region const& region,
                                                                 stream_scope)
{
    return _streams.upload_to_texture(cc::move(texture), cc::move(source), subresource, region);
}

sg::stream_download_handle metal_context::stream_bytes_from_buffer(raw_buffer_handle buffer,
                                                                   isize offset_in_bytes,
                                                                   isize size_in_bytes,
                                                                   stream_scope)
{
    return _streams.download_from_buffer(cc::move(buffer), {}, offset_in_bytes, size_in_bytes);
}

sg::stream_download_handle metal_context::stream_bytes_from_texture(raw_texture_handle texture,
                                                                    subresource_index const& subresource,
                                                                    texture_region const& region,
                                                                    stream_scope)
{
    return _streams.download_from_texture(cc::move(texture), {}, subresource, region);
}

sg::stream_download_handle metal_context::stream_to_sink_from_buffer(raw_buffer_handle buffer,
                                                                     stream_sink sink,
                                                                     isize offset_in_bytes,
                                                                     isize size_in_bytes,
                                                                     stream_scope)
{
    return _streams.download_from_buffer(cc::move(buffer), cc::move(sink), offset_in_bytes, size_in_bytes);
}

sg::stream_download_handle metal_context::stream_to_sink_from_texture(raw_texture_handle texture,
                                                                      stream_sink sink,
                                                                      subresource_index const& subresource,
                                                                      texture_region const& region,
                                                                      stream_scope)
{
    return _streams.download_from_texture(cc::move(texture), cc::move(sink), subresource, region);
}

void metal_context::set_stream_upload_ratio(float ratio)
{
    _streams.set_upload_ratio(ratio);
}

void metal_context::set_stream_download_ratio(float ratio)
{
    _streams.set_download_ratio(ratio);
}

void metal_context::set_stream_upload_aging(float per_second)
{
    _streams.set_upload_aging(per_second);
}

void metal_context::set_stream_download_aging(float per_second)
{
    _streams.set_download_aging(per_second);
}

cc::result<sg::raw_buffer_handle> metal_context::try_create_raw_buffer(isize size_in_bytes,
                                                                       buffer_usages usage,
                                                                       allocation_info const& alloc)
{
    return cc::result<sg::raw_buffer_handle>(create_metal_buffer(size_in_bytes, usage, alloc));
}

cc::result<metal_buffer_handle> metal_context::create_metal_buffer(isize size_in_bytes,
                                                                   sg::buffer_usages usage,
                                                                   sg::allocation_info const& alloc)
{
    CC_ASSERT(size_in_bytes >= 0, "buffer size must be non-negative");

    if (is_device_lost())
        return cc::error("the metal device has been lost");

    auto const scope = autorelease_scope();

    // An empty buffer allocates nothing: Metal refuses a zero length, and with validation armed the attempt aborts
    // rather than returning null.
    // A null MTLBuffer is the representation, and size 0 is a legal sg buffer.
    MTL::Buffer* buffer = nullptr;
    if (size_in_bytes > 0)
    {
        if (alloc.is_placed())
        {
            auto const& heap = static_cast<metal_memory_heap const&>(*alloc.heap);
            buffer = heap.heap()->newBuffer(NS::UInteger(size_in_bytes), k_buffer_options, NS::UInteger(alloc.offset));
            if (buffer == nullptr)
                return cc::error("the metal heap refused a placed buffer — check the offset's alignment and room");
        }
        else
        {
            buffer = _device->newBuffer(NS::UInteger(size_in_bytes), k_buffer_options);
            if (buffer == nullptr)
                return cc::error("the metal device refused a buffer allocation");
        }
    }

    // MTL4 names no resources at record time, so a buffer outside the residency set is simply absent when the GPU
    // runs — a copy from it reads zeroes, with nothing reported anywhere.
    _residency.add(buffer);

    auto handle = std::make_shared<metal_buffer const>(*this, size_in_bytes, usage, buffer, alloc.heap);

    if (alloc.scope == sg::lifetime_scope::transient)
        _transient_expiring.lock([&](cc::vector<std::weak_ptr<sg::raw_buffer const>>& v)
                                 { v.push_back(std::weak_ptr<sg::raw_buffer const>(handle)); });

    return handle;
}

cc::result<metal_memory_heap_handle> metal_context::create_metal_memory_heap(isize size_in_bytes)
{
    CC_ASSERT(size_in_bytes > 0, "heap size must be positive");

    if (is_device_lost())
        return cc::error("the metal device has been lost");

    auto const scope = autorelease_scope();

    auto* const descriptor = MTL::HeapDescriptor::alloc()->init();
    descriptor->setSize(NS::UInteger(size_in_bytes));
    descriptor->setStorageMode(MTL::StorageModePrivate);
    // Placement is the one heap type that lets the caller choose the offset, which is sg's whole model: an external
    // allocator sub-allocates and the heap only validates and mints.
    descriptor->setType(MTL::HeapTypePlacement);
    // Same reasoning as k_buffer_options: sg emits the barriers, so the driver must not infer its own.
    descriptor->setHazardTrackingMode(MTL::HazardTrackingModeUntracked);

    auto* const heap = _device->newHeap(descriptor);
    descriptor->release();

    if (heap == nullptr)
        return cc::error("the metal device refused a heap allocation");

    // A placement heap is the allocation; the buffers placed into it are not separately resident.
    _residency.add(heap);

    return std::make_shared<metal_memory_heap const>(*this, size_in_bytes, heap);
}

cc::result<sg::raw_texture_handle> metal_context::try_create_raw_texture(texture_description const& desc,
                                                                         allocation_info const& alloc)
{
    return cc::result<sg::raw_texture_handle>(create_metal_texture(desc, alloc));
}

cc::result<sg::memory_heap_handle> metal_context::try_create_memory_heap(isize size_in_bytes)
{
    return cc::result<sg::memory_heap_handle>(create_metal_memory_heap(size_in_bytes));
}

cc::result<sg::binding_group_layout_handle> metal_context::try_create_binding_group_layout(
    cc::span<binding const> bindings,
    cc::span<named_sampler const> static_samplers,
    lifetime_scope scope)
{
    return cc::result<sg::binding_group_layout_handle>(
        create_metal_binding_group_layout(bindings, static_samplers, scope));
}

cc::result<sg::pipeline_layout_handle> metal_context::try_create_pipeline_layout(pipeline_layout_description const& desc,
                                                                                 lifetime_scope scope)
{
    return cc::result<sg::pipeline_layout_handle>(create_metal_pipeline_layout(desc, scope));
}

cc::result<sg::compute_pipeline_handle> metal_context::try_create_compute_pipeline(compute_pipeline_description const& desc,
                                                                                   lifetime_scope scope)
{
    return cc::result<sg::compute_pipeline_handle>(create_metal_compute_pipeline(desc, scope));
}

cc::result<sg::raster_pipeline_handle> metal_context::try_create_raster_pipeline(raster_pipeline_description const& desc,
                                                                                 lifetime_scope scope)
{
    return cc::result<sg::raster_pipeline_handle>(create_metal_raster_pipeline(desc, scope));
}

cc::result<sg::raytracing_pipeline_handle> metal_context::try_create_raytracing_pipeline(
    raytracing_pipeline_description const& desc,
    lifetime_scope scope)
{
    return create_metal_raytracing_pipeline(desc, scope);
}

cc::result<sg::raytracing_shader_table_handle> metal_context::try_create_raytracing_shader_table(
    raytracing_shader_table_description const& desc,
    lifetime_scope scope)
{
    return create_metal_raytracing_shader_table(desc, scope);
}

cc::result<sg::binding_group_handle> metal_context::try_create_binding_group(binding_group_layout_handle layout,
                                                                             cc::span<named_view const> views,
                                                                             cc::span<named_sampler const> samplers,
                                                                             lifetime_scope scope)
{
    return cc::result<sg::binding_group_handle>(create_metal_binding_group(layout, views, samplers, scope));
}

cc::result<sg::binding_group_handle> metal_context::try_create_binding_group(binding_group_layout_handle layout,
                                                                             cc::span<slotted_view const> views,
                                                                             cc::span<named_sampler const> samplers,
                                                                             lifetime_scope scope)
{
    // Resolved to names and handed to the one create, rather than a second write path.
    // What the slot form saves is a string compare per binding, which is real and small; a second encoder for the
    // argument buffer would be neither.
    if (layout == nullptr)
        return cc::error("binding_group: the description names no layout");

    auto const bindings = layout->bindings();
    auto named = cc::vector<sg::named_view>();
    named.reserve(views.size());

    for (auto const& v : views)
    {
        auto const slot = isize(u32(v.slot));
        if (v.slot == sg::binding_slot::invalid || slot >= bindings.size())
            return cc::error(cc::format("binding_group: slot {} is not a slot of this layout, which has {} bindings",
                                        slot, bindings.size()));
        named.push_back({.name = bindings[slot].name, .view = v.view});
    }

    return cc::result<sg::binding_group_handle>(create_metal_binding_group(layout, named, samplers, scope));
}

cc::result<sg::staging_binding_group_handle> metal_context::try_create_staging_binding_group(binding_group_layout_handle layout,
                                                                                             lifetime_scope scope)
{
    return create_metal_staging_binding_group(cc::move(layout), scope);
}
} // namespace sg::backend::metal
