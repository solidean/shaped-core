#include "metal_stream.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/map.hh>
#include <clean-core/record/log.hh>
#include <shaped-graphics/backends/metal/metal_buffer.hh>
#include <shaped-graphics/backends/metal/metal_context.hh>
#include <shaped-graphics/backends/metal/metal_texture.hh>

namespace sg::backend::metal
{
namespace
{
/// Shared storage, because the CPU writes or reads it; untracked, like every other resource here.
constexpr MTL::ResourceOptions k_stream_staging_options
    = MTL::ResourceStorageModeShared | MTL::ResourceHazardTrackingModeUntracked;

/// How many bytes one cycle stages before it commits and starts another.
///
/// A budget rather than a real window: Metal needs no packing, since a batch allocates exactly the staging it uses.
/// What it bounds is how much a single cycle can hold resident at once, and how coarse the scheduler's accounting is.
constexpr isize k_cycle_budget_bytes = 8 * 1024 * 1024;

/// Sub-allocations inside a batch's staging buffer are aligned to this.
/// A texture copy's source offset has alignment rules of its own, and 256 satisfies every format sg exposes.
constexpr isize k_staging_alignment = 256;

[[nodiscard]] isize align_up(isize value, isize alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

/// Settle `job` one way, exactly once, and release everything it held.
///
/// Every teardown path goes through here: a manual node nobody pushes parks its dependents for the process's life,
/// and a stream value nobody signals parks every command list that ever waited on it.
void settle(metal_context& ctx, metal_stream_job& job, bool ok)
{
    if (job.stream_value != 0)
    {
        auto const* const resource
            = job.is_texture ? static_cast<void const*>(job.texture.get()) : static_cast<void const*>(job.buffer.get());
        ctx.transfers().signal_stream_value(resource, job.stream_value);
        job.stream_value = 0;
    }

    if (job.control != nullptr && job.control->completion != nullptr && !job.control->completion->is_ready())
    {
        if (ok)
            job.control->completion->push_value(cc::unit{});
        else
            job.control->completion->push_error(cc::async_error::make_cancelled());
    }

    if (job.bytes_completion != nullptr && !job.bytes_completion->is_ready())
    {
        if (ok)
            job.bytes_completion->push_value(cc::unit{});
        else
            job.bytes_completion->push_error(cc::async_error::make_cancelled());
    }

    job.source.reset();
    job.sink = {};
    job.buffer = nullptr;
    job.texture = nullptr;
    job.drain = {};
}
} // namespace

/// The actor every streaming transfer runs on.
///
/// Two message types: a job to admit, and a batch id the GPU has finished.
/// Both are state changes rather than work, so the cycle in `on_process` is the only place that stages, commits or
/// delivers — which is what keeps every job's state actor-local and lock-free.
class metal_stream_system::actor_impl final : public cc::threaded_actor_impl<metal_stream_job, u64>
{
public:
    explicit actor_impl(metal_context& ctx) : _ctx(&ctx)
    {
        _upload_scheduler.set_window_bytes(k_cycle_budget_bytes);
        _download_scheduler.set_window_bytes(k_cycle_budget_bytes);
    }

    [[nodiscard]] cc::string_view actor_name() const noexcept override { return "sg metal stream"; }

    /// Wakes the actor from any thread: a source whose data arrived, or a commit that completed.
    void wake() { _self->enqueue_message(u64(0)); }

    void set_self(cc::threaded_actor<metal_stream_job, u64>* self) { _self = self; }

    sg::impl::transfer_scheduler& upload_scheduler() { return _upload_scheduler; }
    sg::impl::transfer_scheduler& download_scheduler() { return _download_scheduler; }

protected:
    void on_message(metal_stream_job job) override;
    void on_message(u64 batch_id) override;
    bool on_process() override;
    void on_thread_shutdown() override;

private:
    /// One cycle's worth of staged copies, and the staging they read or write.
    struct batch
    {
        u64 id = 0;
        MTL::Buffer* staging = nullptr;
        MTL4::CommandBuffer* command_buffer = nullptr;

        /// Downloads only: where each staged run sits, and which job it belongs to.
        struct run
        {
            u64 job_sequence = 0;
            isize staging_offset = 0;
            isize extent_offset = 0;
            isize size_in_bytes = 0;
        };
        cc::vector<run> runs;
        cc::vector<u64> jobs; ///< every job with bytes in this batch, upload and download alike
    };

    [[nodiscard]] metal_stream_job* find_job(u64 sequence);
    void reap_finished();
    [[nodiscard]] bool run_cycle();
    void deliver_completed_batches();
    void install_waker(metal_stream_job& job);

    metal_context* _ctx = nullptr;
    cc::threaded_actor<metal_stream_job, u64>* _self = nullptr;

    cc::vector<metal_stream_job> _jobs;
    sg::impl::transfer_scheduler _upload_scheduler;
    sg::impl::transfer_scheduler _download_scheduler;

    cc::vector<batch> _in_flight;              ///< committed, waiting on the GPU
    cc::vector<u64> _completed;                ///< batch ids the handler reported, to deliver next cycle
    callback_mutex<cc::vector<u64>> _reported; ///< what the commit handler hands over, from its own thread
    u64 _next_batch = 1;
};

void metal_stream_system::actor_impl::on_message(metal_stream_job job)
{
    install_waker(job);
    _jobs.push_back(cc::move(job));
}

void metal_stream_system::actor_impl::on_message(u64)
{
    // A bare wake: the work it refers to is found by the cycle below, so the message carries nothing.
}

void metal_stream_system::actor_impl::install_waker(metal_stream_job& job)
{
    if (job.source == nullptr)
        return;

    // The waker outlives the job by design — a source may call it after the transfer ended — so it names the actor
    // rather than anything per job.
    auto* const self = this;
    job.source->set_waker([self] { self->wake(); });
}

metal_stream_job* metal_stream_system::actor_impl::find_job(u64 sequence)
{
    for (auto& job : _jobs)
        if (job.sequence == sequence)
            return &job;
    return nullptr;
}

bool metal_stream_system::actor_impl::on_process()
{
    // What the commit handler reported since the last cycle, taken under its lock and processed without it.
    _reported.lock(
        [&](cc::vector<u64>& reported)
        {
            for (auto const id : reported)
                _completed.push_back(id);
            reported.clear();
        });

    deliver_completed_batches();
    auto const staged = run_cycle();
    reap_finished();

    // Another cycle only if this one moved something: a cycle that staged nothing has nothing new to find, and the
    // next wake is what a stalled source or an in-flight commit owes us.
    return staged;
}

void metal_stream_system::actor_impl::deliver_completed_batches()
{
    if (_completed.empty())
        return;

    for (auto const id : _completed)
    {
        auto const found = [&]() -> isize
        {
            for (auto i = isize(0); i < _in_flight.size(); ++i)
                if (_in_flight[i].id == id)
                    return i;
            return -1;
        }();
        if (found < 0)
            continue;

        auto& b = _in_flight[found];

        // Downloads first: the bytes are in the staging buffer and this is the only thread that may hand them over.
        for (auto const& r : b.runs)
        {
            auto* const job = find_job(r.job_sequence);
            if (job == nullptr || job->failed)
                continue;

            auto const bytes = cc::span<byte const>(static_cast<byte const*>(b.staging->contents()) + r.staging_offset,
                                                    r.size_in_bytes);

            if (job->sink)
            {
                // In cursor order, which is what lets a sink append rather than seek.
                // Runs of one job are staged and committed in order on one queue, so `delivered` always matches here.
                CC_ASSERT(r.extent_offset == job->delivered, "a streaming download delivered a run out of order");
                if (!job->sink(bytes, r.extent_offset))
                    job->failed = true;
            }
            else if (auto const alive = job->weak_destination.lock(); alive != nullptr)
            {
                cc::memcpy(job->destination.data() + r.extent_offset, bytes.data(), size_t(r.size_in_bytes));
            }
            else
            {
                // The caller dropped the future, which is what cancellation means on this channel.
                job->failed = true;
            }

            job->delivered += r.size_in_bytes;
        }

        for (auto const sequence : b.jobs)
            if (auto* const job = find_job(sequence); job != nullptr)
                --job->chunks_in_flight;

        _ctx->residency().remove(b.staging);
        b.staging->release();
        b.command_buffer->release();
        _in_flight.remove_at(found);
    }

    _completed.clear();
}

bool metal_stream_system::actor_impl::run_cycle()
{
    if (_jobs.empty())
        return false;

    // Eligibility is recomputed every cycle: a cancelled job stops being picked, and a finished one has nothing left.
    auto candidates = cc::vector<sg::impl::transfer_candidate>::create_with_capacity(_jobs.size());
    for (auto& job : _jobs)
    {
        auto const cancelled = job.control != nullptr && job.control->cancelled.load(std::memory_order_relaxed);
        candidates.push_back({
            .flavor = sg::impl::transfer_flavor::streaming,
            .priority = job.control != nullptr ? job.control->priority.load(std::memory_order_relaxed) : 0,
            .family = job.family,
            .sequence = job.sequence,
            // Eligibility is per cycle and never sticky: a source that said `not_yet` last time is asked again, which
            // is what makes its waker mean something.
            // Re-polling costs nothing, because a cycle that stages nothing sleeps until the next wake.
            .eligible = !job.failed && !job.source_exhausted && !cancelled,
        });
    }

    // One staging buffer for the whole batch, so a cycle costs one allocation rather than one per chunk.
    // The chunks are collected first because the size is not known until the sources have been polled.
    struct staged_chunk
    {
        u64 job_sequence = 0;
        cc::pinned_data<byte const> data; ///< uploads: the bytes to copy in
        isize extent_offset = 0;
        isize size_in_bytes = 0;
        isize staging_offset = 0;
        bool is_download = false;
    };

    auto chunks = cc::vector<staged_chunk>();
    auto total = isize(0);

    _upload_scheduler.begin_window();
    _download_scheduler.begin_window();

    while (total < k_cycle_budget_bytes)
    {
        auto const picked = _upload_scheduler.pick_next(candidates);
        if (!picked.has_value())
            break;

        auto const index = picked.value();
        auto& job = _jobs[index];
        candidates[index].eligible = false; // one chunk per job per cycle, so no job can monopolize a cycle

        if (job.is_download)
        {
            auto const remaining = job.size_in_bytes - job.cursor;
            if (remaining <= 0)
            {
                job.source_exhausted = true;
                continue;
            }

            auto take = cc::min(remaining, k_cycle_budget_bytes - total);
            if (job.is_texture && job.layout.bytes_per_row > 0)
            {
                // A sink is handed whole tightly-packed rows, and a row is the smallest unit a texture copy places.
                take = take / job.layout.bytes_per_row * job.layout.bytes_per_row;
                if (take == 0)
                    break; // no room for a row this cycle; the next one starts empty
            }

            chunks.push_back({.job_sequence = job.sequence,
                              .extent_offset = job.cursor,
                              .size_in_bytes = take,
                              .staging_offset = align_up(total, k_staging_alignment),
                              .is_download = true});
            total = chunks.back().staging_offset + take;
            job.cursor += take;
            if (job.cursor >= job.size_in_bytes)
                job.source_exhausted = true;
            continue;
        }

        auto poll = job.source->try_next_chunk();
        switch (poll.status)
        {
        case sg::stream_source_status::ready:
            break;
        case sg::stream_source_status::not_yet:
            // Passed over rather than waited on: the cycle is filled with other work and its waker brings it back.
            continue;
        case sg::stream_source_status::done:
            job.source_exhausted = true;
            continue;
        case sg::stream_source_status::failed:
            job.failed = true;
            continue;
        }

        if (poll.chunk.data.empty())
            continue;

        auto const size = isize(poll.chunk.data.size());
        chunks.push_back({.job_sequence = job.sequence,
                          .data = cc::move(poll.chunk.data),
                          .extent_offset = poll.chunk.offset,
                          .size_in_bytes = size,
                          .staging_offset = align_up(total, k_staging_alignment)});
        total = chunks.back().staging_offset + size;
    }

    if (chunks.empty())
        return false;

    auto const scope = autorelease_scope();

    auto b = batch{.id = _next_batch++};
    b.staging = _ctx->device()->newBuffer(NS::UInteger(total), k_stream_staging_options);

    // **A refused allocation fails this cycle's jobs rather than the process.**
    // Each of them settles through the same path a cancelled or errored transfer takes, so a caller waiting on the
    // handle is told rather than left parked.
    auto* const allocator = b.staging != nullptr ? _ctx->epochs().lease_allocator() : nullptr;
    b.command_buffer = allocator != nullptr ? _ctx->device()->newCommandBuffer() : nullptr;
    if (b.command_buffer == nullptr)
    {
        if (allocator != nullptr)
            _ctx->epochs().retire_allocator_with_epoch(allocator);
        if (b.staging != nullptr)
            b.staging->release();

        for (auto const& chunk : chunks)
            if (auto* const job = find_job(chunk.job_sequence); job != nullptr)
                job->failed = true;

        _ctx->report_feedback_error(sg::device_error_kind::creation_failed, "the metal device refused a streaming "
                                                                            "staging allocation");
        return false;
    }

    b.staging->setLabel(ns_string("sg stream staging"));
    _ctx->residency().add(b.staging);

    b.command_buffer->beginCommandBuffer(allocator);
    auto* const encoder = b.command_buffer->computeCommandEncoder();

    auto staged_this_cycle = isize(0);
    for (auto& chunk : chunks)
    {
        auto* const job = find_job(chunk.job_sequence);
        CC_ASSERT(job != nullptr, "a staged chunk outlived its job");

        if (!chunk.is_download)
            cc::memcpy(static_cast<byte*>(b.staging->contents()) + chunk.staging_offset, chunk.data.data(),
                       size_t(chunk.size_in_bytes));

        _ctx->transfers().order_stream_copy(*job);

        if (job->is_texture)
        {
            auto const& mtl_texture = static_cast<metal_texture const&>(*job->texture);
            auto const row = job->layout.bytes_per_row;
            auto const image = job->layout.bytes_per_image;

            // **A staged row is a row of BLOCKS, and a chunk may span slices.**
            //
            // `staging_layout_of` counts rows in blocks, so a BC row covers `block_extent` texel rows and treating the
            // two as one fills the top quarter of a BC texture and overruns a 3D one.
            // And the extent is slice-major, so a chunk that crosses `bytes_per_image` continues on the next z — which
            // one copy with a depth of 1 cannot express.
            // So the chunk is walked as one copy per slice, which is also the shape dx12's row packer produces.
            //
            // sg requires a texture chunk to start and end on a row boundary (see stream_source.hh), which is what
            // makes the walk whole rows throughout.
            auto const block = isize(sg::format_block_extent(job->texture->description().format));
            auto const rows_per_image = row > 0 ? image / row : 0;
            CC_ASSERT(row > 0 && rows_per_image > 0, "a texture stream chunk needs a row and an image stride");
            CC_ASSERT(chunk.extent_offset % row == 0 && chunk.size_in_bytes % row == 0,
                      "a texture stream chunk must start and end on a row boundary");

            auto remaining_rows = chunk.size_in_bytes / row;
            auto staged = chunk.staging_offset;
            auto cursor_row = chunk.extent_offset / row; // block rows into the region, slice-major

            while (remaining_rows > 0)
            {
                auto const slice = cursor_row / rows_per_image;
                auto const row_in_slice = cursor_row % rows_per_image;
                auto const rows = cc::min(remaining_rows, rows_per_image - row_in_slice);

                // The last block row of a slice is partial whenever the height is not a multiple of the block extent.
                auto const top = row_in_slice * block;
                auto const height = cc::min(rows * block, isize(job->region.size[1]) - top);

                auto const origin
                    = MTL::Origin(NS::UInteger(job->region.offset[0]), NS::UInteger(isize(job->region.offset[1]) + top),
                                  NS::UInteger(isize(job->region.offset[2]) + slice));
                auto const size = MTL::Size(NS::UInteger(job->region.size[0]), NS::UInteger(height), NS::UInteger(1));

                if (chunk.is_download)
                    encoder->copyFromTexture(mtl_texture.texture(), NS::UInteger(job->subresource.array_layer),
                                             NS::UInteger(job->subresource.mip_level), origin, size, b.staging,
                                             NS::UInteger(staged), NS::UInteger(row), NS::UInteger(row * rows));
                else
                    encoder->copyFromBuffer(b.staging, NS::UInteger(staged), NS::UInteger(row), NS::UInteger(row * rows),
                                            size, mtl_texture.texture(), NS::UInteger(job->subresource.array_layer),
                                            NS::UInteger(job->subresource.mip_level), origin);

                staged += rows * row;
                cursor_row += rows;
                remaining_rows -= rows;
            }
        }
        else
        {
            auto const& mtl_buffer = static_cast<metal_buffer const&>(*job->buffer);
            auto const resource_offset = job->offset_in_bytes + chunk.extent_offset;
            if (chunk.is_download)
                encoder->copyFromBuffer(mtl_buffer.buffer(), NS::UInteger(resource_offset), b.staging,
                                        NS::UInteger(chunk.staging_offset), NS::UInteger(chunk.size_in_bytes));
            else
                encoder->copyFromBuffer(b.staging, NS::UInteger(chunk.staging_offset), mtl_buffer.buffer(),
                                        NS::UInteger(resource_offset), NS::UInteger(chunk.size_in_bytes));
        }

        if (chunk.is_download)
            b.runs.push_back({.job_sequence = job->sequence,
                              .staging_offset = chunk.staging_offset,
                              .extent_offset = chunk.extent_offset,
                              .size_in_bytes = chunk.size_in_bytes});

        b.jobs.push_back(job->sequence);
        ++job->chunks_in_flight;
        job->staged_bytes += chunk.size_in_bytes;
        staged_this_cycle += chunk.size_in_bytes;
        if (job->control != nullptr)
            job->control->bytes_done.store(i64(job->staged_bytes), std::memory_order_relaxed);
    }

    encoder->endEncoding();
    b.command_buffer->endCommandBuffer();

    // The handler does no work beyond naming the batch: delivery belongs on the actor, where order and threading are
    // ours rather than the dispatch queue's.
    auto* const reported = &_reported;
    auto* const self = this;
    auto const id = b.id;

    _ctx->transfers().commit_stream_batch(b.command_buffer, allocator,
                                          [reported, self, id]
                                          {
                                              reported->lock([&](cc::vector<u64>& v) { v.push_back(id); });
                                              self->wake();
                                          });

    _upload_scheduler.on_window_submitted(0, staged_this_cycle);
    _download_scheduler.on_window_submitted(0, staged_this_cycle);
    _in_flight.push_back(cc::move(b));
    return true;
}

void metal_stream_system::actor_impl::reap_finished()
{
    for (auto i = _jobs.size(); i > 0; --i)
    {
        auto& job = _jobs[i - 1];
        auto const cancelled = job.control != nullptr && job.control->cancelled.load(std::memory_order_relaxed);

        // A cancelled job stops being picked, and its chunks already recorded still run — cancellation bounds future
        // work rather than undoing past work.
        auto const finished = job.failed || cancelled || job.source_exhausted;
        if (!finished || job.chunks_in_flight > 0)
            continue;

        settle(*_ctx, job, !job.failed && !cancelled);
        _jobs.remove_at(i - 1);
    }
}

void metal_stream_system::actor_impl::on_thread_shutdown()
{
    // Nothing may be left unsettled, and nothing may still name the device.
    for (auto& b : _in_flight)
    {
        _ctx->residency().remove(b.staging);
        b.staging->release();
        b.command_buffer->release();
    }
    _in_flight.clear();

    for (auto& job : _jobs)
        settle(*_ctx, job, false);
    _jobs.clear();
}

void metal_stream_system::create(metal_context& ctx)
{
    CC_ASSERT(_actor == nullptr, "the stream system is created once");
    _ctx = &ctx;

    // The completion machinery requires every drain `are_transfers_drained` reads to report reaching zero.
    // Without this the waiter is never told, which the busy-spin used to hide.
    _drain.notify_on_drained(&ctx);

    // The impl is built here rather than through make_threaded_actor, because the handle exposes it only after
    // shutdown and the ratio knobs need it while it runs.
    auto impl = std::make_unique<actor_impl>(ctx);
    _impl = impl.get();

    auto actor = cc::make_unique<cc::threaded_actor<metal_stream_job, u64>>(cc::move(impl));
    _impl->set_self(actor.get());
    actor->start();
    _actor = cc::move(actor);
}

void metal_stream_system::shutdown()
{
    if (_actor == nullptr)
        return;

    _actor->shutdown();
    _actor = nullptr;
    _impl = nullptr;
    _ctx = nullptr;
}

void metal_stream_system::admit(metal_stream_job job)
{
    // An empty extent has nothing to copy and settles here rather than costing the actor a cycle.
    if (job.size_in_bytes == 0 && job.source == nullptr)
    {
        settle(*_ctx, job, true);
        return;
    }

    job.drain = _drain.start();
    job.direct_wait = job.is_texture ? static_cast<metal_texture const&>(*job.texture).submission().get()
                                     : static_cast<metal_buffer const&>(*job.buffer).submission().get();
    job.transfer_wait = job.is_texture ? _ctx->transfers().pending_value_for(*job.texture)
                                       : _ctx->transfers().pending_value_for(*job.buffer);
    if (job.control != nullptr)
        job.control->total_hint.store(job.source != nullptr ? job.source->total_size_hint() : i64(job.size_in_bytes),
                                      std::memory_order_relaxed);

    _actor->enqueue_message(cc::move(job));
}

namespace
{
/// The control block and its promotion hook, shared by all four entry points.
///
/// `on_promote` is the backend's half of `promote_to_async`, and on Metal it is only the *statement of intent*:
/// every stream already makes a later list wait, so what promotion removes is the warning rather than adding a wait.
[[nodiscard]] std::shared_ptr<sg::impl::stream_control> make_control(cc::unique_function<void()> on_promote)
{
    auto control = std::make_shared<sg::impl::stream_control>();
    control->completion = cc::make_async_manual<cc::unit>();
    control->on_promote = cc::move(on_promote);
    return control;
}
} // namespace

sg::stream_upload_handle metal_stream_system::upload_to_buffer(sg::raw_buffer_handle buffer,
                                                               std::unique_ptr<sg::stream_source> source,
                                                               isize offset_in_bytes)
{
    CC_ASSERT(buffer != nullptr && source != nullptr, "a streaming upload needs a target and a source");
    CC_ASSERT(!buffer->is_expired(), "streaming upload target is a transient resource used past its epoch");
    CC_ASSERT(buffer->usage().has(sg::buffer_usage::copy_dst), "streaming upload target lacks copy_dst usage");

    auto* const resource = buffer.get();
    auto const value = _ctx->transfers().reserve_stream_value(resource);
    auto const weak = std::weak_ptr<sg::raw_buffer const>(buffer);

    auto job = metal_stream_job{};
    job.sequence = _next_sequence.fetch_add(1, cc::memory_order_relaxed);
    job.family = u64(reinterpret_cast<uintptr_t>(resource));
    job.control = make_control(
        [weak, value]
        {
            if (auto const alive = weak.lock(); alive != nullptr)
                alive->suppress_stream_wait_warning(value);
        });
    job.buffer = cc::move(buffer);
    job.source = cc::move(source);
    job.offset_in_bytes = offset_in_bytes;
    job.stream_value = value;

    auto handle = sg::stream_upload_handle(job.control);
    admit(cc::move(job));
    return handle;
}

sg::stream_upload_handle metal_stream_system::upload_to_texture(sg::raw_texture_handle texture,
                                                                std::unique_ptr<sg::stream_source> source,
                                                                sg::subresource_index const& subresource,
                                                                sg::texture_region const& region)
{
    CC_ASSERT(texture != nullptr && source != nullptr, "a streaming upload needs a target and a source");
    CC_ASSERT(!texture->is_expired(), "streaming upload target is a transient texture used past its epoch");
    CC_ASSERT(texture->usage().has(sg::texture_usage::copy_dst), "streaming upload target texture lacks copy_dst "
                                                                 "usage");

    auto* const resource = texture.get();
    auto const value = _ctx->transfers().reserve_stream_value(resource);
    auto const weak = std::weak_ptr<sg::raw_texture const>(texture);

    auto job = metal_stream_job{};
    job.sequence = _next_sequence.fetch_add(1, cc::memory_order_relaxed);
    job.family = u64(reinterpret_cast<uintptr_t>(resource));
    job.is_texture = true;
    job.control = make_control(
        [weak, value]
        {
            if (auto const alive = weak.lock(); alive != nullptr)
                alive->suppress_stream_wait_warning(value);
        });
    job.layout = staging_layout_of(texture->description().format, region);
    job.size_in_bytes = job.layout.size_in_bytes;
    job.texture = cc::move(texture);
    job.source = cc::move(source);
    job.subresource = subresource;
    job.region = region;
    job.stream_value = value;

    auto handle = sg::stream_upload_handle(job.control);
    admit(cc::move(job));
    return handle;
}

sg::stream_download_handle metal_stream_system::download_from_buffer(sg::raw_buffer_handle buffer,
                                                                     sg::stream_sink sink,
                                                                     isize offset_in_bytes,
                                                                     isize size_in_bytes)
{
    CC_ASSERT(buffer != nullptr, "a streaming download needs a source");
    CC_ASSERT(!buffer->is_expired(), "streaming download source is a transient resource used past its epoch");
    CC_ASSERT(buffer->usage().has(sg::buffer_usage::copy_src), "streaming download source lacks copy_src usage");

    auto* const resource = buffer.get();
    auto const value = _ctx->transfers().reserve_stream_value(resource);
    auto const weak = std::weak_ptr<sg::raw_buffer const>(buffer);

    auto job = metal_stream_job{};
    job.sequence = _next_sequence.fetch_add(1, cc::memory_order_relaxed);
    job.family = u64(reinterpret_cast<uintptr_t>(resource));
    job.is_download = true;
    job.control = make_control(
        [weak, value]
        {
            if (auto const alive = weak.lock(); alive != nullptr)
                alive->suppress_stream_wait_warning(value);
        });
    job.buffer = cc::move(buffer);
    job.sink = cc::move(sink);
    job.offset_in_bytes = offset_in_bytes;
    job.size_in_bytes = size_in_bytes;
    job.stream_value = value;

    return finish_download(cc::move(job));
}

sg::stream_download_handle metal_stream_system::download_from_texture(sg::raw_texture_handle texture,
                                                                      sg::stream_sink sink,
                                                                      sg::subresource_index const& subresource,
                                                                      sg::texture_region const& region)
{
    CC_ASSERT(texture != nullptr, "a streaming download needs a source");
    CC_ASSERT(!texture->is_expired(), "streaming download source is a transient texture used past its epoch");
    CC_ASSERT(texture->usage().has(sg::texture_usage::copy_src), "streaming download source texture lacks copy_src "
                                                                 "usage");

    auto* const resource = texture.get();
    auto const value = _ctx->transfers().reserve_stream_value(resource);
    auto const weak = std::weak_ptr<sg::raw_texture const>(texture);

    auto job = metal_stream_job{};
    job.sequence = _next_sequence.fetch_add(1, cc::memory_order_relaxed);
    job.family = u64(reinterpret_cast<uintptr_t>(resource));
    job.is_download = true;
    job.is_texture = true;
    job.control = make_control(
        [weak, value]
        {
            if (auto const alive = weak.lock(); alive != nullptr)
                alive->suppress_stream_wait_warning(value);
        });
    job.layout = staging_layout_of(texture->description().format, region);
    job.size_in_bytes = job.layout.size_in_bytes;
    job.texture = cc::move(texture);
    job.sink = cc::move(sink);
    job.subresource = subresource;
    job.region = region;
    job.stream_value = value;

    return finish_download(cc::move(job));
}

sg::stream_download_handle metal_stream_system::finish_download(metal_stream_job job)
{
    auto control = job.control;

    // A sink-driven download carries no future: the sink IS the delivery channel, and handing back an empty future
    // beside it would only invite someone to wait on bytes that were never going to land anywhere.
    if (job.sink)
    {
        admit(cc::move(job));
        return sg::stream_download_handle(cc::move(control), sg::bytes_future());
    }

    auto destination = cc::pinned_data<byte>::create_uninitialized(job.size_in_bytes);
    auto completion = cc::make_async_manual<cc::unit>();

    // Weak, because dropping the future cancels the transfer — the same channel meaning an async download has.
    job.weak_destination = std::weak_ptr<void const>(destination.pin());
    job.bytes_completion = completion;
    job.destination = destination.span();

    auto future = sg::bytes_future(cc::pinned_data<byte const>(cc::move(destination)), cc::move(completion));
    admit(cc::move(job));
    return sg::stream_download_handle(cc::move(control), cc::move(future));
}

void metal_stream_system::set_upload_ratio(float ratio)
{
    if (_impl != nullptr)
        _impl->upload_scheduler().set_stream_ratio(ratio);
}

void metal_stream_system::set_download_ratio(float ratio)
{
    if (_impl != nullptr)
        _impl->download_scheduler().set_stream_ratio(ratio);
}

void metal_stream_system::set_upload_aging(float per_second)
{
    if (_impl != nullptr)
        _impl->upload_scheduler().set_aging_factor(per_second);
}

void metal_stream_system::set_download_aging(float per_second)
{
    if (_impl != nullptr)
        _impl->download_scheduler().set_aging_factor(per_second);
}
} // namespace sg::backend::metal
