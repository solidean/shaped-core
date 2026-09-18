// webgpu_command_list: creation, submission, drop, and the transfer and copy paths.

#include <clean-core/common/assert.hh>
#include <clean-core/common/log.hh>
#include <clean-core/common/utility.hh>
#include <shaped-graphics/backends/webgpu/webgpu_context.hh>
#include <shaped-graphics/backends/webgpu/webgpu_format.hh>
#include <shaped-graphics/exceptions.hh>

namespace sg::backend::webgpu
{
WGPUCommandEncoder webgpu_command_list::encoder() const
{
    _ctx.assert_on_device_thread();
    return _encoder.get();
}

WGPUComputePassEncoder webgpu_command_list::compute_pass() const
{
    _ctx.assert_on_device_thread();
    return _compute_pass.get();
}

WGPURenderPassEncoder webgpu_command_list::render_pass() const
{
    _ctx.assert_on_device_thread();
    return _render_pass.get();
}

webgpu_command_list::webgpu_command_list(webgpu_context& ctx, sg::epoch created_in, wgpu_command_encoder encoder)
  : sg::command_list(ctx, created_in), _ctx(ctx), _encoder(cc::move(encoder))
{
}

webgpu_command_list::~webgpu_command_list()
{
    if (_consumed)
        return;
    CC_LOG_WARNING("command list destroyed without submit or drop — auto-dropping. Submit or drop every command list "
                   "explicitly through the context.");
    _ctx.reclaim_unsubmitted_command_list(*this);
}

cc::result<std::unique_ptr<webgpu_command_list>> webgpu_context::create_webgpu_command_list()
{
    assert_on_device_thread();
    if (is_device_lost())
        return cc::error(cc::format("the device was lost: {}", device_loss_reason()));

    auto const desc = WGPUCommandEncoderDescriptor{.nextInChain = nullptr, .label = to_wgpu("sg command list")};
    auto encoder = wgpu_command_encoder(wgpuDeviceCreateCommandEncoder(device(), &desc));
    if (!encoder)
        return cc::error("wgpuDeviceCreateCommandEncoder returned no encoder");

    ++_open_command_lists;
    return std::make_unique<webgpu_command_list>(*this, current_epoch(), cc::move(encoder));
}

void webgpu_command_list::end_open_pass()
{
    if (_compute_pass)
    {
        wgpuComputePassEncoderEnd(compute_pass());
        _compute_pass = {};
        _compute.needs_full_apply = true;
    }
    if (_render_pass)
    {
        wgpuRenderPassEncoderEnd(render_pass());
        _render_pass = {};
        _raster.needs_full_apply = true;
    }
}

wgpu_command_buffer webgpu_command_list::finish()
{
    CC_ASSERT(!_in_rendering_scope, "a rendering scope is still open at submit");
    end_open_pass();
    auto const desc = WGPUCommandBufferDescriptor{.nextInChain = nullptr, .label = to_wgpu("sg command list")};
    auto buffer = wgpu_command_buffer(wgpuCommandEncoderFinish(encoder(), &desc));
    _encoder = {};
    return buffer;
}

sg::submission_token webgpu_context::submit_webgpu_command_list(std::unique_ptr<webgpu_command_list> cmd)
{
    CC_ASSERT(cmd != nullptr, "cannot submit a null command list");
    assert_on_device_thread();
    CC_ASSERT(cmd->created_in_epoch() == current_epoch(), "a command list must be submitted in the epoch it was opened "
                                                          "in (it cannot span epochs)");
    CC_ASSERT(!cmd->_consumed, "command list already submitted or dropped");

    cmd->finalize_queries();
    auto const buffer = cmd->finish();

    // Every write this list's commands read is queued ahead of the submit: its ring spans at record time, its constant pages here.
    _constant_pages.flush_and_return(cmd->_constant_pages);

    // A stream still filling a resource this list reads is brought forward, so the list sees all of it; one still reading it is copied first, so the list cannot overtake the read.
    if (_streams.has_pending_streams())
        for (auto const* resource : cmd->_touched)
            _streams.flush_resource(resource, true);

    auto const raw = buffer.get();
    wgpuQueueSubmit(queue(), 1, &raw);

    auto const token = sg::submission_token(_next_submission++);
    notify_when_queue_done(u64(token), 0);

    // A later write to a span lands after this submit, so the spans are free again.
    if (cmd->_holds_upload_ring)
    {
        cmd->_holds_upload_ring = false;
        _upload_ring.release_holder();
    }

    for (auto& readback : cmd->_pending_readbacks)
    {
        if (readback.gate != nullptr)
            readback.gate->mark_submitted();
        _readbacks.start_map(cc::move(readback));
    }
    cmd->_pending_readbacks.clear();
    cmd->release_queries();

    cmd->_keep_alive.clear();
    cmd->_touched.clear();
    cmd->_consumed = true;
    --_open_command_lists;

    if (is_device_lost())
        throw sg::device_lost_exception(device_loss_reason());
    return token;
}

void webgpu_context::drop_webgpu_command_list(std::unique_ptr<webgpu_command_list> cmd)
{
    CC_ASSERT(cmd != nullptr, "cannot drop a null command list");
    CC_ASSERT(cmd->created_in_epoch() == current_epoch(), "a command list must be dropped in the epoch it was opened "
                                                          "in");
    reclaim_unsubmitted_command_list(*cmd);
}

void webgpu_context::reclaim_unsubmitted_command_list(webgpu_command_list& cmd)
{
    CC_ASSERT(!cmd._consumed, "command list already submitted or dropped");
    cmd._consumed = true;

    // The encoder is abandoned unfinished; WebGPU discards what it recorded.
    cmd._compute_pass = {};
    cmd._render_pass = {};
    cmd._encoder = {};

    if (cmd._holds_upload_ring)
    {
        cmd._holds_upload_ring = false;
        _upload_ring.release_holder();
    }
    _constant_pages.discard(cmd._constant_pages);
    for (auto& readback : cmd._pending_readbacks)
        _readbacks.discard(readback);
    cmd._pending_readbacks.clear();
    cmd.release_queries();
    cmd._keep_alive.clear();
    --_open_command_lists;
}

// -- transfers --

webgpu_upload_span webgpu_command_list::stage_upload(cc::span<byte const> data, isize staged_size)
{
    if (!_holds_upload_ring)
    {
        _holds_upload_ring = true;
        _ctx._upload_ring.acquire_holder();
    }
    return _ctx._upload_ring.stage(data, staged_size);
}

void webgpu_command_list::transition_texture_layout(sg::raw_texture_handle texture,
                                                    sg::texture_layout layout,
                                                    cc::optional<sg::subresource_range> const&)
{
    // WebGPU tracks usage itself, so there is no layout to leave a texture in.
    CC_ASSERT(texture != nullptr, "ensure_layout on a null texture");
    CC_ASSERT(layout != sg::texture_layout::undefined, "ensure_layout to undefined");
}

namespace
{
[[nodiscard]] webgpu_buffer const& as_webgpu_buffer(sg::raw_buffer_handle const& buffer)
{
    CC_ASSERT(buffer != nullptr, "buffer is null");
    auto const* b = dynamic_cast<webgpu_buffer const*>(buffer.get());
    CC_ASSERT(b != nullptr, "buffer is not a webgpu buffer");
    CC_ASSERT(!b->is_expired(), "a transient buffer is used past its epoch (expired)");
    return *b;
}

[[nodiscard]] webgpu_texture const& as_webgpu_texture(sg::raw_texture_handle const& texture)
{
    CC_ASSERT(texture != nullptr, "texture is null");
    auto const* t = dynamic_cast<webgpu_texture const*>(texture.get());
    CC_ASSERT(t != nullptr, "texture is not a webgpu texture");
    CC_ASSERT(!t->is_expired(), "a transient texture is used past its epoch (expired)");
    return *t;
}

/// How many bytes a buffer transfer of `size` at `offset` spans once rounded to whole words.
/// Rounding past the logical end is only safe into the allocation's padding, which is why a short write inside the buffer asserts.
[[nodiscard]] isize word_span(webgpu_buffer const& buffer, isize offset, isize size, bool is_write)
{
    CC_ASSERT(offset % buffer_word_bytes == 0, "webgpu transfers start on a 4-byte boundary; this offset does not");
    auto const rounded = align_up(size, buffer_word_bytes);
    if (is_write)
        CC_ASSERT(rounded == size || offset + size == buffer.size_in_bytes(),
                  "webgpu writes whole 4-byte words; a write that is not a whole number of words must end at the "
                  "buffer's end, where the rest is padding");
    return rounded;
}
} // namespace

void webgpu_command_list::upload_bytes_to_buffer(sg::raw_buffer_handle buffer,
                                                 cc::span<byte const> data,
                                                 isize offset_in_bytes)
{
    auto const& dst = as_webgpu_buffer(buffer);
    CC_ASSERT(offset_in_bytes >= 0 && offset_in_bytes + data.size() <= dst.size_in_bytes(), "upload range is out of "
                                                                                            "the buffer's bounds");
    if (data.empty())
        return;
    CC_ASSERT(dst.usage().has(sg::buffer_usage::copy_dst), "upload target buffer must have buffer_usage::copy_dst");

    auto const words = word_span(dst, offset_in_bytes, data.size(), true);
    auto const span = stage_upload(data, words);
    end_open_pass();
    wgpuCommandEncoderCopyBufferToBuffer(encoder(), span.buffer, u64(span.offset), dst.raw(), u64(offset_in_bytes),
                                         u64(words));
    if (span.overflow)
        _keep_alive.push_back(std::make_shared<wgpu_buffer>(span.overflow));
    touch(buffer);
}

void webgpu_command_list::upload_bytes_to_texture(sg::raw_texture_handle texture,
                                                  cc::span<byte const> pixels,
                                                  sg::subresource_index const& subresource,
                                                  sg::texture_region const& region)
{
    auto const& dst = as_webgpu_texture(texture);
    CC_ASSERT(dst.usage().has(sg::texture_usage::copy_dst), "upload target texture must have texture_usage::copy_dst");

    auto const layout = texel_copy_layout_of(dst.format(), region.size);
    CC_ASSERT(pixels.size() == layout.packed_bytes, "pixel data size does not match the copy region");

    if (!_holds_upload_ring)
    {
        _holds_upload_ring = true;
        _ctx._upload_ring.acquire_holder();
    }
    auto const span = _ctx._upload_ring.stage_rows(pixels, layout.row_bytes, layout.padded_row, layout.staged_bytes,
                                                   isize(sg::format_block_size(dst.format())));

    auto const source = WGPUTexelCopyBufferInfo{
        .layout = {.offset = u64(span.offset), .bytesPerRow = u32(layout.padded_row), .rowsPerImage = u32(layout.rows)},
        .buffer = span.buffer,
    };
    auto const destination = WGPUTexelCopyTextureInfo{
        .texture = dst.raw(),
        .mipLevel = u32(subresource.mip_level),
        .origin = {u32(region.offset[0]), u32(region.offset[1]),
                   dst.dimension() == sg::texture_dimension::d3 ? u32(region.offset[2]) : u32(subresource.array_layer)},
        .aspect = to_wgpu_copy_aspect(dst.format(), subresource.aspect),
    };
    auto const extent = copy_extent_of(dst.format(), region.size);

    end_open_pass();
    wgpuCommandEncoderCopyBufferToTexture(encoder(), &source, &destination, &extent);
    if (span.overflow)
        _keep_alive.push_back(std::make_shared<wgpu_buffer>(span.overflow));
    touch(texture);
}

sg::bytes_future webgpu_command_list::record_readback(webgpu_readback readback,
                                                      cc::pinned_data<byte> destination,
                                                      cc::unique_function<bool(cc::span<byte const>)> deliver)
{
    auto completion = cc::make_async_manual<cc::unit>();
    auto gate = std::make_shared<sg::bytes_wait_gate>();
    readback.deliver = cc::move(deliver);
    readback.pin = std::weak_ptr<void const>(destination.pin());
    readback.has_pin = true;
    readback.completion = completion;
    readback.gate = gate;
    _pending_readbacks.push_back(cc::move(readback));
    return sg::bytes_future(cc::pinned_data<byte const>(cc::move(destination)), cc::move(completion), cc::move(gate));
}

sg::bytes_future webgpu_command_list::download_bytes_from_buffer(sg::raw_buffer_handle buffer,
                                                                 isize offset_in_bytes,
                                                                 isize size_in_bytes)
{
    auto const& src = as_webgpu_buffer(buffer);
    CC_ASSERT(size_in_bytes >= 0, "download size must be non-negative");
    CC_ASSERT(offset_in_bytes >= 0 && offset_in_bytes + size_in_bytes <= src.size_in_bytes(), "download range is out "
                                                                                              "of the buffer's bounds");
    if (size_in_bytes == 0)
        return sg::bytes_future(cc::pinned_data<byte const>(), sg::make_ready_completion());
    CC_ASSERT(src.usage().has(sg::buffer_usage::copy_src), "download source buffer must have buffer_usage::copy_src");

    // A read may widen to whole words in both directions: the extra bytes are read and dropped.
    auto const start = offset_in_bytes / buffer_word_bytes * buffer_word_bytes;
    auto const skip = offset_in_bytes - start;
    auto const words = align_up(skip + size_in_bytes, buffer_word_bytes);

    auto readback = _ctx._readbacks.acquire(words);
    end_open_pass();
    wgpuCommandEncoderCopyBufferToBuffer(encoder(), src.raw(), u64(start), readback.staging.get(), 0, u64(words));
    touch(buffer);

    auto destination = cc::pinned_data<byte>::create_uninitialized(size_in_bytes);
    auto const dst_span = destination.span();
    return record_readback(cc::move(readback), cc::move(destination),
                           [dst_span, skip](cc::span<byte const> mapped)
                           {
                               cc::memcpy(dst_span.data(), mapped.data() + skip, size_t(dst_span.size()));
                               return true;
                           });
}

sg::bytes_future webgpu_command_list::download_bytes_from_texture(sg::raw_texture_handle texture,
                                                                  sg::subresource_index const& subresource,
                                                                  sg::texture_region const& region)
{
    auto const& src = as_webgpu_texture(texture);
    CC_ASSERT(src.usage().has(sg::texture_usage::copy_src), "download source texture must have "
                                                            "texture_usage::copy_src");

    auto const layout = texel_copy_layout_of(src.format(), region.size);
    auto readback = _ctx._readbacks.acquire(layout.staged_bytes);

    auto const source = WGPUTexelCopyTextureInfo{
        .texture = src.raw(),
        .mipLevel = u32(subresource.mip_level),
        .origin = {u32(region.offset[0]), u32(region.offset[1]),
                   src.dimension() == sg::texture_dimension::d3 ? u32(region.offset[2]) : u32(subresource.array_layer)},
        .aspect = to_wgpu_copy_aspect(src.format(), subresource.aspect),
    };
    auto const destination = WGPUTexelCopyBufferInfo{
        .layout = {.offset = 0, .bytesPerRow = u32(layout.padded_row), .rowsPerImage = u32(layout.rows)},
        .buffer = readback.staging.get(),
    };
    auto const extent = copy_extent_of(src.format(), region.size);

    end_open_pass();
    wgpuCommandEncoderCopyTextureToBuffer(encoder(), &source, &destination, &extent);
    touch(texture);

    auto out = cc::pinned_data<byte>::create_uninitialized(layout.packed_bytes);
    auto const dst_span = out.span();
    return record_readback(cc::move(readback), cc::move(out),
                           [dst_span, layout](cc::span<byte const> mapped)
                           {
                               auto const row_count = layout.rows * layout.images;
                               for (isize r = 0; r < row_count; ++r)
                                   cc::memcpy(dst_span.data() + r * layout.row_bytes,
                                              mapped.data() + r * layout.padded_row, size_t(layout.row_bytes));
                               return true;
                           });
}

void webgpu_command_list::copy_buffer_region(sg::raw_buffer_handle src,
                                             sg::raw_buffer_handle dst,
                                             isize src_offset_in_bytes,
                                             isize dst_offset_in_bytes,
                                             isize size_in_bytes)
{
    auto const& s = as_webgpu_buffer(src);
    auto const& d = as_webgpu_buffer(dst);
    CC_ASSERT(size_in_bytes >= 0, "copy size must be non-negative");
    CC_ASSERT(src_offset_in_bytes >= 0 && src_offset_in_bytes + size_in_bytes <= s.size_in_bytes(),
              "copy source range is out of the buffer's bounds");
    CC_ASSERT(dst_offset_in_bytes >= 0 && dst_offset_in_bytes + size_in_bytes <= d.size_in_bytes(),
              "copy dest range is out of the buffer's bounds");
    if (size_in_bytes == 0)
        return;
    CC_ASSERT(s.usage().has(sg::buffer_usage::copy_src), "copy source buffer must have buffer_usage::copy_src");
    CC_ASSERT(d.usage().has(sg::buffer_usage::copy_dst), "copy dest buffer must have buffer_usage::copy_dst");
    if (&s == &d)
        CC_ASSERT(dst_offset_in_bytes + size_in_bytes <= src_offset_in_bytes
                      || src_offset_in_bytes + size_in_bytes <= dst_offset_in_bytes,
                  "source and destination ranges overlap in a same-buffer copy");
    CC_ASSERT(src_offset_in_bytes % buffer_word_bytes == 0, "webgpu copies start on a 4-byte boundary");

    // WebGPU refuses a copy within one buffer outright, so that goes through a temporary buffer.
    auto const words = word_span(d, dst_offset_in_bytes, size_in_bytes, true);
    end_open_pass();
    if (&s == &d)
    {
        auto const desc = WGPUBufferDescriptor{
            .nextInChain = nullptr,
            .label = to_wgpu("sg self-copy"),
            .usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_CopySrc,
            .size = u64(words),
            .mappedAtCreation = WGPU_FALSE,
        };
        auto temp = std::make_shared<wgpu_buffer>(wgpuDeviceCreateBuffer(_ctx.device(), &desc));
        wgpuCommandEncoderCopyBufferToBuffer(encoder(), s.raw(), u64(src_offset_in_bytes), temp->get(), 0, u64(words));
        wgpuCommandEncoderCopyBufferToBuffer(encoder(), temp->get(), 0, d.raw(), u64(dst_offset_in_bytes), u64(words));
        _keep_alive.push_back(temp);
    }
    else
        wgpuCommandEncoderCopyBufferToBuffer(encoder(), s.raw(), u64(src_offset_in_bytes), d.raw(),
                                             u64(dst_offset_in_bytes), u64(words));
    touch(src);
    touch(dst);
}

// -- ray tracing: refused --

sg::blas_handle webgpu_command_list::raytracing_build_blas_triangles(cc::span<sg::blas_triangles const>,
                                                                     sg::accel_build_flags)
{
    CC_UNREACHABLE("webgpu has no ray tracing; check cmd.raytracing.is_supported()");
}

sg::blas_handle webgpu_command_list::raytracing_build_blas_aabbs(cc::span<sg::blas_aabbs const>, sg::accel_build_flags)
{
    CC_UNREACHABLE("webgpu has no ray tracing; check cmd.raytracing.is_supported()");
}

sg::tlas_handle webgpu_command_list::raytracing_build_tlas(cc::span<sg::tlas_instance const>, sg::accel_build_flags)
{
    CC_UNREACHABLE("webgpu has no ray tracing; check cmd.raytracing.is_supported()");
}

void webgpu_command_list::raytracing_bind_pipeline(sg::raytracing_pipeline const&)
{
    CC_UNREACHABLE("webgpu has no ray tracing; check cmd.raytracing.is_supported()");
}

void webgpu_command_list::raytracing_bind_group(int, sg::binding_group const&)
{
    CC_UNREACHABLE("webgpu has no ray tracing; check cmd.raytracing.is_supported()");
}

void webgpu_command_list::raytracing_dispatch_rays(sg::raytracing_shader_table const&, sg::raygen_index, int, int, int)
{
    CC_UNREACHABLE("webgpu has no ray tracing; check cmd.raytracing.is_supported()");
}
} // namespace sg::backend::webgpu
