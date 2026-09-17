// webgpu_stream_system: the async tier's immediate queue writes and maps, and the streaming tier's windows.

#include <clean-core/common/assert.hh>
#include <clean-core/common/log.hh>
#include <clean-core/common/utility.hh>
#include <shaped-graphics/backends/webgpu/webgpu_context.hh>
#include <shaped-graphics/backends/webgpu/webgpu_format.hh>

namespace sg::backend::webgpu
{
namespace
{
[[nodiscard]] webgpu_buffer const& buffer_of(sg::raw_buffer_handle const& buffer)
{
    auto const* b = dynamic_cast<webgpu_buffer const*>(buffer.get());
    CC_ASSERT(b != nullptr, "buffer is not a webgpu buffer");
    CC_ASSERT(!b->is_expired(), "a transfer names an expired buffer");
    return *b;
}

[[nodiscard]] webgpu_texture const& texture_of(sg::raw_texture_handle const& texture)
{
    auto const* t = dynamic_cast<webgpu_texture const*>(texture.get());
    CC_ASSERT(t != nullptr, "texture is not a webgpu texture");
    CC_ASSERT(!t->is_expired(), "a transfer names an expired texture");
    return *t;
}

/// Writes `data` into `buffer` at `offset` with the queue, padding a short tail that ends at the buffer's end.
void write_buffer(webgpu_context& ctx, webgpu_buffer const& buffer, cc::span<byte const> data, isize offset)
{
    if (data.empty())
        return;
    CC_ASSERT(offset % buffer_word_bytes == 0, "webgpu transfers start on a 4-byte boundary; this offset does not");
    auto const words = align_up(data.size(), buffer_word_bytes);
    if (words == data.size())
    {
        wgpuQueueWriteBuffer(ctx.queue(), buffer.raw(), u64(offset), data.data(), size_t(data.size()));
        return;
    }
    CC_ASSERT(offset + data.size() == buffer.size_in_bytes(),
              "webgpu writes whole 4-byte words; a write that is not a whole number of words must end at the buffer's "
              "end, where the rest is padding");
    auto padded = cc::vector<byte>::create_filled(words, byte(0));
    cc::memcpy(padded.data(), data.data(), size_t(data.size()));
    wgpuQueueWriteBuffer(ctx.queue(), buffer.raw(), u64(offset), padded.data(), size_t(words));
}

/// Writes whole rows `first_row` onwards of `region`'s packed bytes, splitting at image boundaries.
void write_texture_rows(webgpu_context& ctx,
                        webgpu_texture const& texture,
                        sg::subresource_index const& subresource,
                        sg::texture_region const& region,
                        cc::span<byte const> data,
                        isize first_row)
{
    auto const layout = texel_copy_layout_of(texture.format(), region.size);
    if (layout.row_bytes == 0)
        return;
    auto const block = isize(sg::format_block_extent(texture.format()));
    auto const is_3d = texture.dimension() == sg::texture_dimension::d3;

    auto row = first_row;
    auto remaining = data.size() / layout.row_bytes;
    auto cursor = data.data();
    while (remaining > 0)
    {
        auto const image = row / layout.rows;
        auto const y = row % layout.rows;
        auto const n = cc::min(remaining, layout.rows - y);

        auto const destination = WGPUTexelCopyTextureInfo{
            .texture = texture.raw(),
            .mipLevel = u32(subresource.mip_level),
            .origin = {u32(region.offset[0]), u32(isize(region.offset[1]) + y * block),
                       is_3d ? u32(isize(region.offset[2]) + image) : u32(subresource.array_layer)},
            .aspect = to_wgpu_copy_aspect(texture.format(), subresource.aspect),
        };
        auto const data_layout = WGPUTexelCopyBufferLayout{
            .offset = 0,
            .bytesPerRow = u32(layout.row_bytes),
            .rowsPerImage = u32(n),
        };
        // The last rows of an edge-sized block format may be partial, so the extent is clamped to the region.
        auto const height = cc::min(n * block, isize(region.size[1]) - y * block);
        auto const extent = WGPUExtent3D{u32(region.size[0]), u32(height), 1};
        wgpuQueueWriteTexture(ctx.queue(), &destination, cursor, size_t(n * layout.row_bytes), &data_layout, &extent);

        cursor += n * layout.row_bytes;
        remaining -= n;
        row += n;
    }
}

struct work_done_request
{
    std::shared_ptr<sg::impl::stream_control> control;
};
} // namespace

void webgpu_stream_system::initialize(webgpu_context& ctx, isize window_bytes)
{
    _ctx = &ctx;
    _window_bytes = window_bytes;
}

void webgpu_stream_system::shutdown()
{
    for (auto& job : _uploads)
        job.control->completion->push_error(cc::async_error::make_cancelled());
    _uploads = {};
    _pump.reset();
}

void webgpu_stream_system::ensure_pump()
{
    if (!_pump.is_registered())
        _pump = cc::register_thread_pump([this] { return pump(); });
    cc::thread_pump_notify();
}

namespace
{
[[nodiscard]] std::shared_ptr<sg::impl::stream_control> make_control(i64 total_hint)
{
    auto control = std::make_shared<sg::impl::stream_control>();
    control->completion = cc::make_async_manual<cc::unit>();
    control->total_hint.store(total_hint, std::memory_order_relaxed);
    // Every write lands in queue order ahead of any later list, so the wait a promotion asks for already holds.
    control->on_promote = [] {};
    return control;
}
} // namespace

// -- the async tier --

void webgpu_stream_system::upload_buffer(sg::raw_buffer_handle buffer, cc::pinned_data<byte const> data, isize offset)
{
    if (data.empty())
        return;
    auto const family = static_cast<void const*>(buffer.get());
    for (auto const& job : _uploads)
        if (job.family == family)
        {
            _uploads.push_back(upload_job{
                .control = make_control(data.size()),
                .source = sg::make_pinned_stream_source(cc::move(data)),
                .buffer = cc::move(buffer),
                .offset = offset,
                .sequence = _next_sequence++,
                .family = family,
                .is_async = true,
            });
            ensure_pump();
            return;
        }
    write_buffer(*_ctx, buffer_of(buffer), data.span(), offset);
}

void webgpu_stream_system::upload_texture(sg::raw_texture_handle texture,
                                          cc::pinned_data<byte const> data,
                                          sg::subresource_index const& subresource,
                                          sg::texture_region const& region)
{
    if (data.empty())
        return;
    auto const family = static_cast<void const*>(texture.get());
    for (auto const& job : _uploads)
        if (job.family == family)
        {
            _uploads.push_back(upload_job{
                .control = make_control(data.size()),
                .source = sg::make_pinned_stream_source(cc::move(data)),
                .texture = cc::move(texture),
                .subresource = subresource,
                .region = region,
                .sequence = _next_sequence++,
                .family = family,
                .is_async = true,
            });
            ensure_pump();
            return;
        }
    write_texture_rows(*_ctx, texture_of(texture), subresource, region, data.span(), 0);
}

sg::bytes_future webgpu_stream_system::read_buffer(sg::raw_buffer_handle const& buffer,
                                                   isize offset,
                                                   isize size,
                                                   std::shared_ptr<sg::impl::stream_control> control,
                                                   sg::stream_sink sink)
{
    auto const& src = buffer_of(buffer);
    flush_resource(buffer.get(), false);

    auto const start = offset / buffer_word_bytes * buffer_word_bytes;
    auto const skip = offset - start;
    auto const words = align_up(skip + size, buffer_word_bytes);

    auto readback = _ctx->_readbacks.acquire(words);
    auto encoder = wgpu_command_encoder(wgpuDeviceCreateCommandEncoder(_ctx->device(), nullptr));
    wgpuCommandEncoderCopyBufferToBuffer(encoder.get(), src.raw(), u64(start), readback.staging.get(), 0, u64(words));
    auto commands = wgpu_command_buffer(wgpuCommandEncoderFinish(encoder.get(), nullptr));
    auto const raw = commands.get();
    wgpuQueueSubmit(_ctx->queue(), 1, &raw);

    auto completion = control != nullptr ? control->completion : cc::make_async_manual<cc::unit>();
    readback.completion = completion;

    if (sink)
    {
        auto const chunk_bytes = cc::max(_window_bytes, isize(1));
        readback.deliver = [sink = cc::move(sink), control, skip, size, chunk_bytes](cc::span<byte const> mapped) mutable
        {
            for (isize at = 0; at < size; at += chunk_bytes)
            {
                auto const n = cc::min(chunk_bytes, size - at);
                if (!sink(mapped.subspan({.offset = skip + at, .size = n}), at))
                    return false;
                control->bytes_done.store(at + n, std::memory_order_relaxed);
            }
            return true;
        };
        _ctx->_readbacks.start_map(cc::move(readback));
        return {};
    }

    auto destination = cc::pinned_data<byte>::create_uninitialized(size);
    auto const dst_span = destination.span();
    readback.deliver = [dst_span, skip, control](cc::span<byte const> mapped)
    {
        cc::memcpy(dst_span.data(), mapped.data() + skip, size_t(dst_span.size()));
        if (control != nullptr)
            control->bytes_done.store(dst_span.size(), std::memory_order_relaxed);
        return true;
    };
    readback.pin = std::weak_ptr<void const>(destination.pin());
    readback.has_pin = true;
    _ctx->_readbacks.start_map(cc::move(readback));
    return sg::bytes_future(cc::pinned_data<byte const>(cc::move(destination)), cc::move(completion));
}

sg::bytes_future webgpu_stream_system::read_texture(sg::raw_texture_handle const& texture,
                                                    sg::subresource_index const& subresource,
                                                    sg::texture_region const& region,
                                                    std::shared_ptr<sg::impl::stream_control> control,
                                                    sg::stream_sink sink)
{
    auto const& src = texture_of(texture);
    auto const layout = texel_copy_layout_of(src.format(), region.size);
    flush_resource(texture.get(), false);

    auto readback = _ctx->_readbacks.acquire(layout.staged_bytes);
    auto const source = WGPUTexelCopyTextureInfo{
        .texture = src.raw(),
        .mipLevel = u32(subresource.mip_level),
        .origin = {u32(region.offset[0]), u32(region.offset[1]),
                   src.dimension() == sg::texture_dimension::d3 ? u32(region.offset[2]) : u32(subresource.array_layer)},
        .aspect = to_wgpu_copy_aspect(src.format(), subresource.aspect),
    };
    auto const destination_info = WGPUTexelCopyBufferInfo{
        .layout = {.offset = 0, .bytesPerRow = u32(layout.padded_row), .rowsPerImage = u32(layout.rows)},
        .buffer = readback.staging.get(),
    };
    auto const extent = WGPUExtent3D{u32(region.size[0]), u32(region.size[1]), u32(region.size[2])};
    auto encoder = wgpu_command_encoder(wgpuDeviceCreateCommandEncoder(_ctx->device(), nullptr));
    wgpuCommandEncoderCopyTextureToBuffer(encoder.get(), &source, &destination_info, &extent);
    auto commands = wgpu_command_buffer(wgpuCommandEncoderFinish(encoder.get(), nullptr));
    auto const raw = commands.get();
    wgpuQueueSubmit(_ctx->queue(), 1, &raw);

    auto completion = control != nullptr ? control->completion : cc::make_async_manual<cc::unit>();
    readback.completion = completion;
    auto const row_count = layout.rows * layout.images;

    if (sink)
    {
        // A sink sees tightly packed rows, a window's worth of whole rows at a time.
        auto const rows_per_chunk = cc::max(_window_bytes / cc::max(layout.row_bytes, isize(1)), isize(1));
        readback.deliver
            = [sink = cc::move(sink), control, layout, row_count, rows_per_chunk](cc::span<byte const> mapped) mutable
        {
            auto packed = cc::vector<byte>();
            for (isize first = 0; first < row_count; first += rows_per_chunk)
            {
                auto const n = cc::min(rows_per_chunk, row_count - first);
                packed.resize_to_uninitialized(n * layout.row_bytes);
                for (isize r = 0; r < n; ++r)
                    cc::memcpy(packed.data() + r * layout.row_bytes, mapped.data() + (first + r) * layout.padded_row,
                               size_t(layout.row_bytes));
                if (!sink(packed, first * layout.row_bytes))
                    return false;
                control->bytes_done.store((first + n) * layout.row_bytes, std::memory_order_relaxed);
            }
            return true;
        };
        _ctx->_readbacks.start_map(cc::move(readback));
        return {};
    }

    auto destination = cc::pinned_data<byte>::create_uninitialized(layout.packed_bytes);
    auto const dst_span = destination.span();
    readback.deliver = [dst_span, layout, row_count, control](cc::span<byte const> mapped)
    {
        for (isize r = 0; r < row_count; ++r)
            cc::memcpy(dst_span.data() + r * layout.row_bytes, mapped.data() + r * layout.padded_row,
                       size_t(layout.row_bytes));
        if (control != nullptr)
            control->bytes_done.store(dst_span.size(), std::memory_order_relaxed);
        return true;
    };
    readback.pin = std::weak_ptr<void const>(destination.pin());
    readback.has_pin = true;
    _ctx->_readbacks.start_map(cc::move(readback));
    return sg::bytes_future(cc::pinned_data<byte const>(cc::move(destination)), cc::move(completion));
}

sg::bytes_future webgpu_stream_system::download_buffer(sg::raw_buffer_handle buffer, isize offset, isize size)
{
    if (size == 0)
        return sg::bytes_future(cc::pinned_data<byte const>(), sg::make_ready_completion());
    return read_buffer(buffer, offset, size, nullptr, {});
}

sg::bytes_future webgpu_stream_system::download_texture(sg::raw_texture_handle texture,
                                                        sg::subresource_index const& subresource,
                                                        sg::texture_region const& region)
{
    return read_texture(texture, subresource, region, nullptr, {});
}

// -- the streaming tier --


sg::stream_upload_handle webgpu_stream_system::stream_to_buffer(sg::raw_buffer_handle buffer,
                                                                std::unique_ptr<sg::stream_source> source,
                                                                isize offset)
{
    auto control = make_control(source->total_size_hint());
    source->set_waker([] { cc::thread_pump_notify(); });
    auto const family = static_cast<void const*>(buffer.get()); // before the move below
    _uploads.push_back(upload_job{
        .control = control,
        .source = cc::move(source),
        .buffer = cc::move(buffer),
        .offset = offset,
        .sequence = _next_sequence++,
        .family = family,
    });
    ensure_pump();
    return sg::stream_upload_handle(cc::move(control));
}

sg::stream_upload_handle webgpu_stream_system::stream_to_texture(sg::raw_texture_handle texture,
                                                                 std::unique_ptr<sg::stream_source> source,
                                                                 sg::subresource_index const& subresource,
                                                                 sg::texture_region const& region)
{
    auto control = make_control(source->total_size_hint());
    source->set_waker([] { cc::thread_pump_notify(); });
    auto const family = static_cast<void const*>(texture.get()); // before the move below
    _uploads.push_back(upload_job{
        .control = control,
        .source = cc::move(source),
        .texture = cc::move(texture),
        .subresource = subresource,
        .region = region,
        .sequence = _next_sequence++,
        .family = family,
    });
    ensure_pump();
    return sg::stream_upload_handle(cc::move(control));
}

// A streaming download needs no window of its own — it is one copy and one map — so its handle settles straight from the map.
sg::stream_download_handle webgpu_stream_system::stream_from_buffer(sg::raw_buffer_handle buffer,
                                                                    sg::stream_sink sink,
                                                                    isize offset,
                                                                    isize size)
{
    auto control = make_control(size);
    auto future = read_buffer(buffer, offset, size, control, cc::move(sink));
    return sg::stream_download_handle(cc::move(control), cc::move(future));
}

sg::stream_download_handle webgpu_stream_system::stream_from_texture(sg::raw_texture_handle texture,
                                                                     sg::stream_sink sink,
                                                                     sg::subresource_index const& subresource,
                                                                     sg::texture_region const& region)
{
    auto const layout = texel_copy_layout_of(texture_of(texture).format(), region.size);
    auto control = make_control(layout.packed_bytes);
    auto future = read_texture(texture, subresource, region, control, cc::move(sink));
    return sg::stream_download_handle(cc::move(control), cc::move(future));
}

void webgpu_stream_system::settle_after_queue(std::shared_ptr<sg::impl::stream_control> control)
{
    auto request = std::make_unique<work_done_request>(work_done_request{.control = cc::move(control)});
    auto const info = WGPUQueueWorkDoneCallbackInfo{
        .nextInChain = nullptr,
        .mode = WGPUCallbackMode_AllowSpontaneous,
        .callback =
            [](WGPUQueueWorkDoneStatus status, WGPUStringView, void* userdata1, void*)
        {
            auto const r = std::unique_ptr<work_done_request>(static_cast<work_done_request*>(userdata1));
            if (status == WGPUQueueWorkDoneStatus_Success)
                r->control->completion->push_value(cc::unit{});
            else
                r->control->completion->push_error(cc::async_error::make_cancelled());
        },
        .userdata1 = request.release(),
        .userdata2 = nullptr,
    };
    (void)wgpuQueueOnSubmittedWorkDone(_ctx->queue(), info);
}

bool webgpu_stream_system::is_behind_its_family(isize index) const
{
    auto const& job = _uploads[index];
    for (auto const& other : _uploads)
        if (other.family == job.family && other.sequence < job.sequence)
            return true;
    return false;
}

bool webgpu_stream_system::advance_job(isize index, isize& budget, bool& progressed)
{
    auto& job = _uploads[index];
    while (budget > 0)
    {
        if (job.control->cancelled.load(std::memory_order_relaxed))
        {
            job.control->completion->push_error(cc::async_error::make_cancelled());
            return true;
        }

        auto const poll = job.source->try_next_chunk();
        if (poll.status == sg::stream_source_status::not_yet)
            return false;
        if (poll.status == sg::stream_source_status::failed)
        {
            job.control->completion->push_error(cc::async_error::make_error(cc::any_error("the stream source failed")));
            return true;
        }
        if (poll.status == sg::stream_source_status::done)
        {
            settle_after_queue(job.control);
            return true;
        }

        auto const& chunk = poll.chunk;
        if (job.buffer != nullptr)
            write_buffer(*_ctx, buffer_of(job.buffer), chunk.data.span(), job.offset + chunk.offset);
        else
        {
            auto const& texture = texture_of(job.texture);
            auto const layout = texel_copy_layout_of(texture.format(), job.region.size);
            CC_ASSERT(chunk.offset % layout.row_bytes == 0 && chunk.data.size() % layout.row_bytes == 0,
                      "a texture stream chunk must start and end on a row");
            write_texture_rows(*_ctx, texture, job.subresource, job.region, chunk.data.span(),
                               chunk.offset / layout.row_bytes);
        }
        job.control->bytes_done.fetch_add(chunk.data.size(), std::memory_order_relaxed);
        budget -= cc::max(chunk.data.size(), isize(1));
        progressed = true;
    }
    return false;
}

void webgpu_stream_system::flush_resource(void const* resource, bool for_list)
{
    if (_uploads.empty())
        return;

    auto unbounded = isize(1) << 62;
    auto progressed = false;
    while (true)
    {
        // The oldest queued job into this resource, which is the only one allowed to move.
        auto head = isize(-1);
        for (isize i = 0; i < _uploads.size(); ++i)
            if (_uploads[i].family == resource && (head < 0 || _uploads[i].sequence < _uploads[head].sequence))
                head = i;
        if (head < 0)
            return;

        auto& job = _uploads[head];
        if (for_list && !job.is_async && !job.control->promoted.load(std::memory_order_relaxed))
        {
            auto const claimed = job.buffer != nullptr ? job.buffer->claim_stream_wait_warning(job.sequence)
                                                       : job.texture->claim_stream_wait_warning(job.sequence);
            if (claimed)
                CC_LOG_WARNING("a command list touches a resource a stream is still filling, so the rest of the stream "
                               "was written ahead of it. Wait on the stream handle before using the resource, or call "
                               "promote_to_async on it if bringing it forward is what you want");
        }

        if (!advance_job(head, unbounded, progressed))
        {
            CC_LOG_WARNING("a stream into a resource about to be read has a source with nothing ready, and webgpu "
                           "cannot "
                           "wait for it; the reader sees only what has landed so far");
            return;
        }
        _uploads.remove_at(head);
    }
}

bool webgpu_stream_system::pump()
{
    auto progressed = false;

    // Highest priority first, oldest among equals.
    auto order = cc::vector<isize>();
    for (isize i = 0; i < _uploads.size(); ++i)
        order.push_back(i);
    auto const before = [&](isize a, isize b)
    {
        auto const pa = _uploads[a].control->priority.load(std::memory_order_relaxed);
        auto const pb = _uploads[b].control->priority.load(std::memory_order_relaxed);
        return pa != pb ? pa > pb : _uploads[a].sequence < _uploads[b].sequence;
    };
    for (isize i = 1; i < order.size(); ++i)
        for (auto j = i; j > 0 && before(order[j], order[j - 1]); --j)
            cc::swap(order[j], order[j - 1]);

    // One window's worth of bytes between them.
    // A source with nothing yet is passed over rather than waited on, and a job behind another in its family waits its turn.
    auto budget = _window_bytes;
    auto finished = cc::vector<char>::create_filled(_uploads.size(), char(0));
    for (auto const index : order)
    {
        if (budget <= 0)
            break;
        if (is_behind_its_family(index))
            continue;
        if (advance_job(index, budget, progressed))
            finished[index] = char(1);
    }

    for (auto i = _uploads.size(); i > 0; --i)
        if (finished[i - 1] != char(0))
        {
            _uploads.remove_at(i - 1);
            progressed = true;
        }

    // A window cut short by its budget has more to move on the next sweep.
    return progressed || (budget <= 0 && !_uploads.empty());
}
} // namespace sg::backend::webgpu
