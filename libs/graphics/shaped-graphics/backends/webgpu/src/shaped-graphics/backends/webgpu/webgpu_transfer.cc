// The upload ring, the readback pool and the inline-constant pages: the staging every transfer and constant goes through.

#include <clean-core/common/assert.hh>
#include <clean-core/common/log.hh>
#include <clean-core/common/utility.hh>
#include <shaped-graphics/backends/webgpu/webgpu_context.hh>
#include <shaped-graphics/backends/webgpu/webgpu_format.hh>
#include <shaped-graphics/binding/impl/layout_hash.hh>

namespace sg::backend::webgpu
{
// -- upload ring --

void webgpu_upload_ring::initialize(webgpu_context& ctx, isize capacity_in_bytes)
{
    CC_ASSERT(capacity_in_bytes > 0, "upload ring capacity must be positive");
    _ctx = &ctx;
    _capacity = align_up(capacity_in_bytes, buffer_word_bytes);
    auto const desc = WGPUBufferDescriptor{
        .nextInChain = nullptr,
        .label = to_wgpu("sg upload ring"),
        .usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_CopySrc,
        .size = u64(_capacity),
        .mappedAtCreation = WGPU_FALSE,
    };
    _buffer = wgpu_buffer(wgpuDeviceCreateBuffer(ctx.device(), &desc));
    _head = 0;
}

void webgpu_upload_ring::shutdown()
{
    _buffer = {};
    _capacity = 0;
    _scratch = {};
}

void webgpu_upload_ring::release_holder()
{
    CC_ASSERT(_holders > 0, "upload ring holder released twice");
    if (--_holders != 0)
        return;

    _head = 0;
    if (_pending_capacity > 0 && _ctx != nullptr)
    {
        auto const capacity = _pending_capacity;
        _pending_capacity = 0;
        initialize(*_ctx, capacity);
    }
}

void webgpu_upload_ring::set_budget(isize bytes)
{
    CC_ASSERT(bytes > 0, "upload ring budget must be positive");
    if (_holders == 0 && _ctx != nullptr)
        initialize(*_ctx, bytes);
    else
        _pending_capacity = bytes;
}

cc::optional<isize> webgpu_upload_ring::reserve(isize size, isize alignment)
{
    auto const start = align_up(_head, alignment);
    if (start + size > _capacity)
        return {};
    _head = start + size;
    return start;
}

void webgpu_upload_ring::write(WGPUBuffer buffer, isize offset, cc::span<byte const> padded)
{
    if (!padded.empty())
        wgpuQueueWriteBuffer(_ctx->queue(), buffer, u64(offset), padded.data(), size_t(padded.size()));
}

webgpu_upload_span webgpu_upload_ring::stage_outside_ring(cc::span<byte const> padded)
{
    auto const epoch = u64(_ctx->current_epoch());
    if (_last_warned_epoch != epoch)
    {
        _last_warned_epoch = epoch;
        CC_LOG_WARNING("an inline upload of {} bytes did not fit the {}-byte upload ring while other open command "
                       "lists "
                       "held it, so it went into a buffer of its own. Raise ctx.upload.set_inline_budget, or submit "
                       "lists before opening more",
                       padded.size(), _capacity);
    }

    auto const desc = WGPUBufferDescriptor{
        .nextInChain = nullptr,
        .label = to_wgpu("sg upload overflow"),
        .usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_CopySrc,
        .size = u64(padded.size()),
        .mappedAtCreation = WGPU_FALSE,
    };
    auto span = webgpu_upload_span();
    span.overflow = wgpu_buffer(wgpuDeviceCreateBuffer(_ctx->device(), &desc));
    span.buffer = span.overflow.get();
    span.offset = 0;
    write(span.buffer, 0, padded);
    return span;
}

webgpu_upload_span webgpu_upload_ring::stage(cc::span<byte const> data, isize staged_size, isize alignment)
{
    CC_ASSERT(staged_size % buffer_word_bytes == 0 && staged_size >= data.size(), "a staged upload must be whole "
                                                                                  "words");

    auto padded = data;
    if (staged_size != data.size())
    {
        _scratch.resize_to_uninitialized(staged_size);
        cc::memcpy(_scratch.data(), data.data(), size_t(data.size()));
        cc::memset(_scratch.data() + data.size(), 0, size_t(staged_size - data.size()));
        padded = _scratch;
    }

    // The ring rewinds only once no open list holds it, so a full ring here is full of spans somebody may still copy.
    auto offset = reserve(staged_size, cc::max(alignment, buffer_word_bytes));
    if (!offset.has_value())
        return stage_outside_ring(padded);

    auto span = webgpu_upload_span();
    span.buffer = _buffer.get();
    span.offset = offset.value();
    write(span.buffer, span.offset, padded);
    return span;
}

webgpu_upload_span webgpu_upload_ring::stage_rows(cc::span<byte const> data,
                                                  isize row_bytes,
                                                  isize padded_row,
                                                  isize staged_size,
                                                  isize block_bytes)
{
    if (row_bytes == padded_row || data.size() <= row_bytes)
        return stage(data, align_up(staged_size, buffer_word_bytes), block_bytes);

    auto const words = align_up(staged_size, buffer_word_bytes);
    auto rows = cc::vector<byte>::create_filled(words, byte(0));
    auto const row_count = data.size() / row_bytes;
    for (isize r = 0; r < row_count; ++r)
        cc::memcpy(rows.data() + r * padded_row, data.data() + r * row_bytes, size_t(row_bytes));
    return stage(rows, words, block_bytes);
}

// -- readback pool --

void webgpu_readback_pool::initialize(webgpu_context& ctx)
{
    _ctx = &ctx;
}

void webgpu_readback_pool::shutdown()
{
    _free = {};
}

webgpu_readback webgpu_readback_pool::acquire(isize size_in_bytes)
{
    auto const needed = align_up(size_in_bytes < 1 ? 1 : size_in_bytes, buffer_word_bytes);

    // The smallest pooled buffer that fits; a pool of a handful of sizes per frame stays short, so a scan is enough.
    auto best = isize(-1);
    for (isize i = 0; i < _free.size(); ++i)
        if (_free[i].capacity >= needed && (best < 0 || _free[i].capacity < _free[best].capacity))
            best = i;

    auto job = webgpu_readback();
    job.mapped_bytes = needed;
    if (best >= 0)
    {
        job.staging = cc::move(_free[best].buffer);
        job.staging_capacity = _free[best].capacity;
        _free.remove_at_unordered(best);
        return job;
    }

    // Round to a power of two from a page up, so one buffer serves the neighbouring sizes of the next frame too.
    auto capacity = isize(4096);
    while (capacity < needed)
        capacity *= 2;
    auto const desc = WGPUBufferDescriptor{
        .nextInChain = nullptr,
        .label = to_wgpu("sg readback"),
        .usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst,
        .size = u64(capacity),
        .mappedAtCreation = WGPU_FALSE,
    };
    job.staging = wgpu_buffer(wgpuDeviceCreateBuffer(_ctx->device(), &desc));
    job.staging_capacity = capacity;
    return job;
}

void webgpu_readback_pool::release(wgpu_buffer buffer, isize capacity)
{
    if (!buffer)
        return;
    // A frame's worth of readbacks is kept; beyond that a burst's buffers are simply dropped.
    if (_free.size() >= 32)
        return;
    _free.push_back({.buffer = cc::move(buffer), .capacity = capacity});
}

namespace
{
struct map_request
{
    std::shared_ptr<webgpu_callback_anchor> anchor;
    webgpu_readback job;
};

void on_buffer_mapped(WGPUMapAsyncStatus status, WGPUStringView, void* userdata1, void*)
{
    auto const request = std::unique_ptr<map_request>(static_cast<map_request*>(userdata1));
    auto* const ctx = request->anchor->ctx;
    if (ctx == nullptr)
    {
        request->job.completion->push_error(cc::async_error::make_cancelled());
        return;
    }
    ctx->_readbacks.finish_map(request->job, status);
}
} // namespace

void webgpu_readback_pool::start_map(webgpu_readback job)
{
    auto const staging = job.staging.get();
    auto const size = size_t(job.mapped_bytes);
    auto request = std::make_unique<map_request>(map_request{.anchor = _ctx->anchor(), .job = cc::move(job)});
    ++_outstanding;
    auto const info = WGPUBufferMapCallbackInfo{
        .nextInChain = nullptr,
        .mode = WGPUCallbackMode_AllowSpontaneous,
        .callback = on_buffer_mapped,
        .userdata1 = request.release(),
        .userdata2 = nullptr,
    };
    (void)wgpuBufferMapAsync(staging, WGPUMapMode_Read, 0, size, info);
}

void webgpu_readback_pool::finish_map(webgpu_readback& job, WGPUMapAsyncStatus status)
{
    --_outstanding;
    if (status != WGPUMapAsyncStatus_Success)
    {
        job.completion->push_error(cc::async_error::make_error(cc::any_error("mapping a readback staging buffer "
                                                                             "failed")));
        _ctx->settle_due_completions();
        return; // a buffer that failed to map is not worth pooling
    }

    auto const alive = !job.has_pin || !job.pin.expired();
    auto delivered = false;
    if (alive)
    {
        auto const* const mapped
            = static_cast<byte const*>(wgpuBufferGetConstMappedRange(job.staging.get(), 0, size_t(job.mapped_bytes)));
        if (mapped != nullptr)
            delivered = job.deliver(cc::span<byte const>(mapped, job.mapped_bytes));
    }
    wgpuBufferUnmap(job.staging.get());
    release(cc::move(job.staging), job.staging_capacity);

    if (!alive)
        job.completion->push_error(cc::async_error::make_cancelled());
    else if (!delivered)
        job.completion->push_error(cc::async_error::make_error(cc::any_error("the readback was refused by its sink")));
    else
        job.completion->push_value(cc::unit{});

    // Last, so a completion waiting on the drain sees the bytes already delivered.
    _ctx->settle_due_completions();
}

void webgpu_readback_pool::discard(webgpu_readback& job)
{
    release(cc::move(job.staging), job.staging_capacity);
    if (job.completion != nullptr)
        job.completion->push_error(cc::async_error::make_cancelled());
}

// -- constant pages --

void webgpu_constant_pages::initialize(webgpu_context& ctx, isize page_bytes, isize offset_alignment)
{
    _ctx = &ctx;
    _page_bytes = align_up(page_bytes, buffer_word_bytes);
    _alignment = offset_alignment;
}

void webgpu_constant_pages::shutdown()
{
    _free = {};
    _pages = {};
}

webgpu_constant_page* webgpu_constant_pages::lease()
{
    if (!_free.empty())
    {
        auto* const page = _free.pop_back();
        page->head = 0;
        page->last_offset = -1;
        page->last_size = 0;
        return page;
    }

    auto page = std::make_unique<webgpu_constant_page>();
    page->index = _pages.size();
    auto const desc = WGPUBufferDescriptor{
        .nextInChain = nullptr,
        .label = to_wgpu("sg inline constants page"),
        .usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
        .size = u64(_page_bytes),
        .mappedAtCreation = WGPU_FALSE,
    };
    page->buffer = wgpu_buffer(wgpuDeviceCreateBuffer(_ctx->device(), &desc));
    page->mirror = cc::vector<byte>::create_filled(_page_bytes, byte(0));
    auto* const raw = page.get();
    _pages.push_back(cc::move(page));
    return raw;
}

void webgpu_constant_pages::give_back(webgpu_constant_page* page)
{
    _free.push_back(page);
}

webgpu_constant_pages::placement webgpu_constant_pages::place(cc::vector<webgpu_constant_page*>& leased,
                                                              cc::span<byte const> block)
{
    CC_ASSERT(block.size() > 0 && block.size() <= _page_bytes, "an inline constants block must fit one page");

    auto* page = leased.empty() ? nullptr : leased.back();

    // An unchanged block binds again where it already is.
    if (page != nullptr && page->last_offset >= 0 && page->last_size == block.size()
        && cc::memcmp(page->mirror.data() + page->last_offset, block.data(), size_t(block.size())) == 0)
        return {.page = page, .offset = u32(page->last_offset)};

    auto offset = page == nullptr ? _page_bytes : align_up(page->head, _alignment);
    if (offset + block.size() > _page_bytes)
    {
        page = lease();
        leased.push_back(page);
        offset = 0;
    }

    cc::memcpy(page->mirror.data() + offset, block.data(), size_t(block.size()));
    page->head = offset + block.size();
    page->last_offset = offset;
    page->last_size = block.size();
    return {.page = page, .offset = u32(offset)};
}

void webgpu_constant_pages::flush_and_return(cc::vector<webgpu_constant_page*>& leased)
{
    for (auto* const page : leased)
    {
        auto const bytes = align_up(page->head, buffer_word_bytes);
        if (bytes > 0)
            wgpuQueueWriteBuffer(_ctx->queue(), page->buffer.get(), 0, page->mirror.data(), size_t(bytes));
        give_back(page);
    }
    leased.clear();
}

void webgpu_constant_pages::discard(cc::vector<webgpu_constant_page*>& leased)
{
    for (auto* const page : leased)
        give_back(page);
    leased.clear();
}

// -- samplers --

WGPUSampler webgpu_sampler_cache::acquire(sg::sampler const& s)
{
    auto entry = _samplers.entry(sg::impl::sampler_hash(s));
    if (!entry.exists())
    {
        auto const desc = to_wgpu_sampler(s);
        entry.emplace(wgpu_sampler(wgpuDeviceCreateSampler(_device, &desc)));
    }
    return entry.value().get();
}
} // namespace sg::backend::webgpu
