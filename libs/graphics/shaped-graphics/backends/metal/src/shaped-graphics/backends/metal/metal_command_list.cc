#include "metal_command_list.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/record/log.hh>
#include <shaped-graphics/backends/metal/metal_buffer.hh>
#include <shaped-graphics/backends/metal/metal_context.hh>
#include <shaped-graphics/backends/metal/metal_staging_ring.hh>

// Everything below the constructor is a seam the milestone order has not reached; see
// libs/graphics/shaped-graphics/docs/writing-a-backend.md.
//
// A CC_UNREACHABLE stub *satisfies* the CHECK_ASSERTS tests written against these contracts, so those tests pass while
// the seam is unimplemented and start failing the moment it is filled in.
// That is a known trap rather than a surprise: copy the reference backend's assert list when implementing one, rather
// than inferring it from what the tests currently accept.
#define SG_METAL_UNIMPLEMENTED(what) CC_UNREACHABLE(what " is not implemented in the metal backend yet")

namespace sg::backend::metal
{
metal_command_list::metal_command_list(metal_context& ctx,
                                       sg::epoch created_in,
                                       MTL4::CommandAllocator* allocator,
                                       MTL4::CommandBuffer* buffer)
  : sg::command_list(ctx, created_in), _metal_context(ctx), _allocator(allocator), _buffer(buffer)
{
    CC_ASSERT(_allocator != nullptr && _buffer != nullptr, "a metal command list needs both an allocator and a buffer");
    _buffer->beginCommandBuffer(_allocator);
    _slot = ctx.slots().acquire();
}

metal_command_list::~metal_command_list()
{
    // Ownership normally leaves through release_ownership at submit or drop.
    // A list destroyed still holding it was never handed to the context, so nothing else will free these — release them
    // here rather than leak, and say so, the way the vulkan backend does.
    if (_buffer == nullptr && _allocator == nullptr)
        return;

    CC_LOG_WARNING("command list destroyed without submit or drop — releasing it. Submit or drop every list you open.");

    end_recording();
    if (_buffer != nullptr)
        _buffer->release();
    if (_allocator != nullptr)
        _allocator->release();
}

MTL4::ComputeCommandEncoder* metal_command_list::compute_encoder()
{
    if (_encoder != nullptr)
        return _encoder;

    _encoder = _buffer->computeCommandEncoder();

    // Every encoder opens by waiting on everything already committed to this queue, and closes by publishing its own
    // work — the two halves of MTL4's queue barrier pair.
    //
    // **Unconditional, and that is the correction rather than the conservatism.**
    // Emitting it only where an intra-list barrier was needed is wrong: a list whose first op has no *local* hazard —
    // a copy reading a buffer this list has not touched before — then never waits for the list that wrote it.
    // That reads correctly in isolation, because the queue usually drains between two submits, and fails under load.
    // The tier-1 suite running its invocables concurrently is what exposed it.
    //
    // Narrowing this to the resources a list actually reads is an optimization the per-buffer tracking already has the
    // information for; it is not a correctness gap.
    _encoder->barrierAfterQueueStages(k_compute_encoder_stages, k_compute_encoder_stages, MTL4::VisibilityOptionDevice);
    return _encoder;
}

void metal_command_list::end_encoder()
{
    if (_encoder == nullptr)
        return;
    _encoder->endEncoding();
    _encoder = nullptr;
}

void metal_command_list::declare_buffer(raw_buffer_handle const& buffer, pipeline_stage_flags stages, access_flags access)
{
    CC_ASSERT(buffer != nullptr, "cannot declare access on a null buffer");
    CC_ASSERT(!buffer->is_expired(), "a transient resource was used past its epoch");

    auto const& mtl_buffer = static_cast<metal_buffer const&>(*buffer);

    auto const [newly_pending, newly_recorded] = mtl_buffer.access().lock(
        [&](metal_buffer_access& a)
        {
            a.declare(_slot, stages, access);
            return cc::pair{a.mark_pending_barrier(_slot), a.mark_recorded(_slot)};
        });

    if (newly_recorded)
        _touched_buffers.push_back(buffer);
    if (newly_pending)
        _pending_buffers.push_back(buffer);

    // Any declared access means this list records GPU work a later list may have to wait for.
    //
    // Set here rather than where a barrier is emitted, which is the bug this replaces: a list whose ops need no
    // barrier at all — an upload into a buffer nothing has touched — published nothing, so the next list's wait found
    // no producer and read what was there before.
    _produced_queue_work = true;
}

void metal_command_list::flush_barriers()
{
    if (_pending_buffers.empty())
        return;

    // One MTL4 barrier names stages rather than resources, so every resource's requirement for this op folds into a
    // single stage pair — which is also why the batch is collected first and emitted once.
    MTL::Stages after = 0;
    MTL::Stages before = 0;
    auto visibility = MTL4::VisibilityOptions(MTL4::VisibilityOptionNone);
    auto any = false;

    for (auto const& buffer : _pending_buffers)
    {
        auto const& mtl_buffer = static_cast<metal_buffer const&>(*buffer);
        auto const barrier = mtl_buffer.access().lock([&](metal_buffer_access& a) { return a.flush(_slot); });

        auto const translated = translate_barrier(barrier);
        if (!translated.needed)
            continue;

        any = true;
        after |= translated.after_stages;
        before |= translated.before_stages;
        visibility = MTL4::VisibilityOptions(visibility | translated.visibility);
    }

    _pending_buffers.clear();

    if (!any)
        return;

    // Clamped to what a compute encoder can name: `barrierAfterEncoderStages` refuses any other stage, and with
    // validation armed it aborts rather than warning.
    // Nothing is lost while every op recorded here is a copy or a dispatch — a raster dependency will need the
    // queue-scoped form or an encoder boundary, which is the raster milestone's problem.
    compute_encoder()->barrierAfterEncoderStages(clamp_to_compute_encoder(after), clamp_to_compute_encoder(before),
                                                 visibility);
}

void metal_command_list::end_recording()
{
    if (!_is_recording)
        return;
    _is_recording = false;

    // The producer half of the queue barrier pair.
    //
    // **A consumer barrier alone synchronizes nothing.** `barrierAfterQueueStages` at the head of a list waits on
    // queue work, and `barrierAfterStages` here is what publishes this list's work to the lists committed after it —
    // without it there is nothing for that wait to find, and a write in one command buffer stays invisible to a read
    // in the next.
    // That is the shape a decay rule hides on dx12 and a submission dependency hides nowhere: Metal wants both halves
    // spelled out.
    if (_produced_queue_work)
        compute_encoder()->barrierAfterStages(k_compute_encoder_stages, k_compute_encoder_stages,
                                              MTL4::VisibilityOptionDevice);

    end_encoder();
    _buffer->endCommandBuffer();
}

void metal_command_list::release_ownership()
{
    _buffer = nullptr;
    _allocator = nullptr;
}

void metal_command_list::transition_texture_layout(raw_texture_handle,
                                                   texture_layout,
                                                   cc::optional<subresource_range> const&)
{
    SG_METAL_UNIMPLEMENTED("a texture layout transition");
}

void metal_command_list::upload_bytes_to_buffer(raw_buffer_handle buffer, cc::span<byte const> data, isize offset_in_bytes)
{
    // cmd.upload forwards straight to this seam with no checking at all, so the whole contract is the backend's.
    // Bounds are checked BEFORE the empty early-out, so an empty write at a bad offset is still a violation.
    CC_ASSERT(buffer != nullptr, "upload target buffer is null");
    CC_ASSERT(!buffer->is_expired(), "upload target is a transient resource used past its epoch");
    CC_ASSERT(buffer->usage().has(sg::buffer_usage::copy_dst), "upload target lacks copy_dst usage");
    CC_ASSERT(offset_in_bytes >= 0, "upload offset must be non-negative");
    CC_ASSERT(offset_in_bytes + isize(data.size()) <= buffer->size_in_bytes(), "upload range exceeds the buffer");

    if (data.empty())
        return;

    auto const staging
        = _metal_context.upload_ring().lock([&](metal_staging_ring& r) { return r.reserve(isize(data.size())); });

    // An overflow reservation brought its own buffer, which has to be resident like any other and freed with the epoch.
    if (staging.owned != nullptr)
    {
        _metal_context.residency().add(staging.owned);
        auto* const owned = staging.owned;
        auto& ctx = _metal_context;
        _metal_context.epochs().defer(
            [&ctx, owned]
            {
                ctx.residency().remove(owned);
                owned->release();
            });
    }

    // The CPU write happens now, at record time, into memory the GPU reads when the copy runs.
    cc::memcpy(staging.bytes().data(), data.data(), size_t(data.size()));

    declare_buffer(buffer, sg::pipeline_stage_flag::copy, sg::access_flag::copy_write);
    flush_barriers();

    auto const& mtl_buffer = static_cast<metal_buffer const&>(*buffer);
    compute_encoder()->copyFromBuffer(staging.buffer, NS::UInteger(staging.offset), mtl_buffer.buffer(),
                                      NS::UInteger(offset_in_bytes), NS::UInteger(data.size()));
}

void metal_command_list::upload_bytes_to_texture(raw_texture_handle,
                                                 cc::span<byte const>,
                                                 subresource_index const&,
                                                 texture_region const&)
{
    SG_METAL_UNIMPLEMENTED("inline texture upload");
}

sg::bytes_future metal_command_list::download_bytes_from_buffer(raw_buffer_handle buffer,
                                                                isize offset_in_bytes,
                                                                isize size_in_bytes)
{
    CC_ASSERT(buffer != nullptr, "download source buffer is null");
    CC_ASSERT(!buffer->is_expired(), "download source is a transient resource used past its epoch");
    CC_ASSERT(buffer->usage().has(sg::buffer_usage::copy_src), "download source lacks copy_src usage");
    CC_ASSERT(offset_in_bytes >= 0 && size_in_bytes >= 0, "download range must be non-negative");
    CC_ASSERT(offset_in_bytes + size_in_bytes <= buffer->size_in_bytes(), "download range exceeds the buffer");

    if (size_in_bytes == 0)
        return sg::bytes_future(cc::pinned_data<byte const>(), sg::make_ready_completion());

    auto const staging
        = _metal_context.download_ring().lock([&](metal_staging_ring& r) { return r.reserve(size_in_bytes); });

    if (staging.owned != nullptr)
    {
        _metal_context.residency().add(staging.owned);
        auto* const owned = staging.owned;
        auto& ctx = _metal_context;
        _metal_context.epochs().defer(
            [&ctx, owned]
            {
                ctx.residency().remove(owned);
                owned->release();
            });
    }

    declare_buffer(buffer, sg::pipeline_stage_flag::copy, sg::access_flag::copy_read);
    flush_barriers();

    auto const& mtl_buffer = static_cast<metal_buffer const&>(*buffer);
    compute_encoder()->copyFromBuffer(mtl_buffer.buffer(), NS::UInteger(offset_in_bytes), staging.buffer,
                                      NS::UInteger(staging.offset), NS::UInteger(size_in_bytes));

    // The destination the caller will read, and the node that says it is filled.
    auto destination = cc::pinned_data<byte>::create_uninitialized(size_in_bytes);
    auto completion = cc::make_async_manual<cc::unit>();

    // The copy out of the ring runs from the commit's own feedback handler, which fires once the GPU has finished the
    // command buffer this copy was recorded into.
    //
    // **No readback actor here**, where the other two backends have one: MTL4CommitFeedback already calls us at the
    // one moment this needs, so a thread of its own would buy nothing.
    // `block_until_transfers_drained` is what waits on the outstanding ones, which is the hook sg provides for exactly
    // this — an epoch deferral would not do, since block_until_idle drains without advancing and an open epoch's
    // payload never runs.
    //
    // **The copy-out holds the destination**, sharing its owner rather than borrowing a span into it.
    // A caller is free to drop the future before its epoch retires — nothing in sg says otherwise — and a bare span
    // would then be written into freed memory, which shows up as heap corruption somewhere else entirely.
    _pending_downloads.push_back(
        [staging, destination, size_in_bytes, completion]() mutable
        {
            cc::memcpy(destination.data(), staging.bytes().data(), size_t(size_in_bytes));
            completion->push_value(cc::unit{});
        });

    return sg::bytes_future(cc::pinned_data<byte const>(cc::move(destination)), cc::move(completion));
}

sg::bytes_future metal_command_list::download_bytes_from_texture(raw_texture_handle,
                                                                 subresource_index const&,
                                                                 texture_region const&)
{
    SG_METAL_UNIMPLEMENTED("inline texture download");
}

void metal_command_list::copy_buffer_region(raw_buffer_handle src,
                                            raw_buffer_handle dst,
                                            isize src_offset_in_bytes,
                                            isize dst_offset_in_bytes,
                                            isize size_in_bytes)
{
    CC_ASSERT(src != nullptr && dst != nullptr, "a copy needs both buffers");
    CC_ASSERT(size_in_bytes >= 0, "copy size must be non-negative");
    CC_ASSERT(src_offset_in_bytes >= 0 && dst_offset_in_bytes >= 0, "copy offsets must be non-negative");
    CC_ASSERT(src_offset_in_bytes + size_in_bytes <= src->size_in_bytes(), "copy source range exceeds the buffer");
    CC_ASSERT(dst_offset_in_bytes + size_in_bytes <= dst->size_in_bytes(), "copy dest range exceeds the buffer");
    CC_ASSERT(src->usage().has(sg::buffer_usage::copy_src), "copy source lacks copy_src usage");
    CC_ASSERT(dst->usage().has(sg::buffer_usage::copy_dst), "copy dest lacks copy_dst usage");

    // Bounds are checked before this, so an empty copy at a bad offset is still a contract violation.
    if (size_in_bytes == 0)
        return;

    auto const& mtl_src_check = static_cast<metal_buffer const&>(*src);
    auto const& mtl_dst_check = static_cast<metal_buffer const&>(*dst);
    auto const same_resource = mtl_src_check.buffer() == mtl_dst_check.buffer();
    if (same_resource)
        CC_ASSERT(dst_offset_in_bytes + size_in_bytes <= src_offset_in_bytes
                      || src_offset_in_bytes + size_in_bytes <= dst_offset_in_bytes,
                  "source and destination ranges overlap in a same-buffer copy");

    // A self-copy reads and writes one resource, so it declares a single combined access and produces one barrier.
    //
    // **Declaring it twice is the trap**, and it is silent: the tracker then treats the read and the write as two ops
    // on one resource, so the read is recorded as already ordered against the write it actually precedes.
    // The next op's read of the written range then needs no barrier by the tracker's reckoning, and reads what was
    // there before the copy.
    // Nothing reports it — the copy is encoded, the data is simply stale.
    if (same_resource)
        declare_buffer(src, sg::pipeline_stage_flag::copy, sg::access_flag::copy_read | sg::access_flag::copy_write);
    else
    {
        declare_buffer(src, sg::pipeline_stage_flag::copy, sg::access_flag::copy_read);
        declare_buffer(dst, sg::pipeline_stage_flag::copy, sg::access_flag::copy_write);
    }
    flush_barriers();

    auto const& mtl_src = static_cast<metal_buffer const&>(*src);
    auto const& mtl_dst = static_cast<metal_buffer const&>(*dst);

    compute_encoder()->copyFromBuffer(mtl_src.buffer(), NS::UInteger(src_offset_in_bytes), mtl_dst.buffer(),
                                      NS::UInteger(dst_offset_in_bytes), NS::UInteger(size_in_bytes));
}

void metal_command_list::compute_bind_pipeline(compute_pipeline const&)
{
    SG_METAL_UNIMPLEMENTED("binding a compute pipeline");
}

void metal_command_list::compute_bind_group(int, binding_group const&)
{
    SG_METAL_UNIMPLEMENTED("binding a compute binding group");
}

void metal_command_list::compute_dispatch(int, int, int)
{
    SG_METAL_UNIMPLEMENTED("a compute dispatch");
}

void metal_command_list::compute_set_inline_constants(cc::span<byte const>, cc::optional<isize>)
{
    SG_METAL_UNIMPLEMENTED("compute inline constants");
}

void metal_command_list::compute_declare_array_buffer_access(cc::string_view, cc::span<array_buffer_access const>)
{
    SG_METAL_UNIMPLEMENTED("declaring compute array buffer access");
}

void metal_command_list::compute_declare_array_texture_access(cc::string_view, cc::span<array_texture_access const>)
{
    SG_METAL_UNIMPLEMENTED("declaring compute array texture access");
}

void metal_command_list::raster_begin_rendering(rendering_info const&)
{
    SG_METAL_UNIMPLEMENTED("opening a rendering scope");
}

void metal_command_list::raster_end_rendering()
{
    SG_METAL_UNIMPLEMENTED("closing a rendering scope");
}

void metal_command_list::raster_bind_pipeline(raster_pipeline const&)
{
    SG_METAL_UNIMPLEMENTED("binding a raster pipeline");
}

void metal_command_list::raster_bind_group(int, binding_group const&)
{
    SG_METAL_UNIMPLEMENTED("binding a raster binding group");
}

void metal_command_list::raster_bind_vertex_buffers(int, cc::span<vertex_buffer_view const>)
{
    SG_METAL_UNIMPLEMENTED("binding vertex buffers");
}

void metal_command_list::raster_bind_index_buffer(index_buffer_view const&)
{
    SG_METAL_UNIMPLEMENTED("binding an index buffer");
}

void metal_command_list::raster_set_viewport(viewport const&)
{
    SG_METAL_UNIMPLEMENTED("setting the viewport");
}

void metal_command_list::raster_set_scissor(tg::aabb2i const&)
{
    SG_METAL_UNIMPLEMENTED("setting the scissor rect");
}

void metal_command_list::raster_set_stencil_reference(u32)
{
    SG_METAL_UNIMPLEMENTED("setting the stencil reference");
}

void metal_command_list::raster_set_blend_constants(tg::vec4f)
{
    SG_METAL_UNIMPLEMENTED("setting the blend constants");
}

void metal_command_list::raster_set_inline_constants(cc::span<byte const>, cc::optional<isize>)
{
    SG_METAL_UNIMPLEMENTED("raster inline constants");
}

void metal_command_list::raster_draw(draw_config const&)
{
    SG_METAL_UNIMPLEMENTED("a draw");
}

void metal_command_list::raster_draw_indexed(draw_indexed_config const&)
{
    SG_METAL_UNIMPLEMENTED("an indexed draw");
}

bool metal_command_list::raytracing_is_supported() const
{
    // Deliberately false while the build and dispatch seams are stubs, and pinned as deliberate by a tier-2 test.
    // Reporting the device's answer here would turn a clean skip into a crash — see
    // libs/graphics/shaped-graphics/docs/writing-a-backend.md.
    return false;
}

sg::blas_handle metal_command_list::raytracing_build_blas_triangles(cc::span<blas_triangles const>, accel_build_flags)
{
    SG_METAL_UNIMPLEMENTED("building a triangle BLAS");
}

sg::blas_handle metal_command_list::raytracing_build_blas_aabbs(cc::span<blas_aabbs const>, accel_build_flags)
{
    SG_METAL_UNIMPLEMENTED("building a procedural BLAS");
}

sg::tlas_handle metal_command_list::raytracing_build_tlas(cc::span<tlas_instance const>, accel_build_flags)
{
    SG_METAL_UNIMPLEMENTED("building a TLAS");
}

void metal_command_list::raytracing_bind_pipeline(raytracing_pipeline const&)
{
    SG_METAL_UNIMPLEMENTED("binding a ray-tracing pipeline");
}

void metal_command_list::raytracing_bind_group(int, binding_group const&)
{
    SG_METAL_UNIMPLEMENTED("binding a ray-tracing binding group");
}

void metal_command_list::raytracing_dispatch_rays(raytracing_shader_table const&, raygen_index, int, int, int)
{
    SG_METAL_UNIMPLEMENTED("dispatching rays");
}

bool metal_command_list::query_timestamps_supported() const
{
    return false;
}

sg::gpu_timestamp metal_command_list::query_record_gpu_timestamp()
{
    SG_METAL_UNIMPLEMENTED("recording a GPU timestamp");
}
} // namespace sg::backend::metal
