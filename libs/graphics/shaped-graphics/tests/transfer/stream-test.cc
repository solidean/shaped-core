#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/fwd.hh> // cc::byte
#include <clean-core/thread/async.hh>
#include <nexus/test.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/fwd.hh> // std::unique_ptr / std::shared_ptr
#include <shaped-graphics/resource/raw_buffer.hh>
#include <shaped-graphics/resource/raw_texture.hh>
#include <shaped-graphics/resource/texture_descriptions.hh>
#include <shaped-graphics/transfer/stream_source.hh>
#include <shaped-graphics/types.hh>

#include <atomic>

using namespace cc::primitive_defines;

// The streaming transfer tier, against every backend compiled into this binary.
//
// What is worth pinning here is the CONTRACT rather than the scheduling: a streamed extent is the caller's alone
// until the handle settles, and the handle is the only thing that says when that is.
// Window sharing, priority ordering and aging are policy and are tested without a GPU in transfer_scheduler-test.cc.

namespace
{
[[nodiscard]] cc::vector<byte> pattern(isize n, int seed = 0)
{
    cc::vector<byte> v;
    v.reserve(n);
    for (isize i = 0; i < n; ++i)
        v.push_back(byte((i * 7 + seed) & 0xFF));
    return v;
}
} // namespace

INVOCABLE_TEST("sg stream - an upload round-trips once the handle settles", (sg::context_handle const& handle))
{
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    auto const src = pattern(4096);
    auto buf = c.persistent.create_raw_buffer(4096, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    REQUIRE(buf != nullptr);

    auto stream = c.stream.bytes_to_buffer(buf, cc::make_pinned_data(src));
    REQUIRE(stream.is_valid());

    // The contract's whole point: nothing may touch the extent until the handle says so, and blocking on the
    // completion node is the way to learn that without polling.
    REQUIRE(cc::try_async_blocking_get(stream.completion()).has_value());
    CHECK(stream.is_complete());
    CHECK(stream.progress().bytes_done == 4096);
    REQUIRE(stream.progress().total_hint.has_value());
    CHECK(stream.progress().total_hint.value() == 4096);

    // Only NOW may a list that reads the streamed extent be submitted.
    auto const back = c.download.bytes_from_buffer(buf, 0, 4096);
    auto const bytes = c.wait_for(back);
    REQUIRE(bytes.has_value());
    CHECK(bytes.value()[0] == src[0]);
    CHECK(bytes.value()[1234] == src[1234]);
    CHECK(bytes.value()[4095] == src[4095]);
}

INVOCABLE_TEST("sg stream - a download round-trips through its future", (sg::context_handle const& handle))
{
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    auto const src = pattern(2048, 11);
    auto buf = c.persistent.create_raw_buffer(2048, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    REQUIRE(buf != nullptr);

    auto up = c.create_command_list();
    REQUIRE(up != nullptr);
    up->upload.bytes_to_buffer(buf, cc::span<byte const>(src));
    c.submit_command_list(cc::move(up));

    auto stream = c.stream.bytes_from_buffer(buf, 0, 2048);
    REQUIRE(stream.is_valid());
    REQUIRE(cc::try_async_blocking_get(stream.completion()).has_value());
    CHECK(stream.is_complete());

    auto const got = stream.future().try_get_bytes();
    REQUIRE(got.has_value());
    CHECK(got.value().size() == 2048);
    CHECK(got.value()[0] == src[0]);
    CHECK(got.value()[2047] == src[2047]);
}

INVOCABLE_TEST("sg stream - an empty transfer settles immediately", (sg::context_handle const& handle))
{
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    auto buf = c.persistent.create_raw_buffer(256, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    REQUIRE(buf != nullptr);

    auto const empty = c.stream.bytes_to_buffer(buf, cc::pinned_data<byte const>());
    CHECK(empty.is_settled());
    CHECK(empty.is_complete());

    auto const empty_read = c.stream.bytes_from_buffer(buf, 0, 0);
    CHECK(empty_read.is_settled());
    CHECK(empty_read.is_complete());
}

INVOCABLE_TEST("sg stream - a cancelled transfer settles as cancelled", (sg::context_handle const& handle))
{
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    auto const src = pattern(1024, 3);
    auto buf = c.persistent.create_raw_buffer(1024, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    REQUIRE(buf != nullptr);

    auto stream = c.stream.bytes_to_buffer(buf, cc::make_pinned_data(src));
    auto const completion = stream.completion();
    stream.cancel();

    // Cancellation SETTLES rather than abandoning: a transfer whose node nobody ever pushed would park every
    // dependent for the process's lifetime, so the actor must say so even when there is nothing left to do.
    // Whether it settles as delivered or cancelled is a race with the copy actor and deliberately not asserted;
    // that it settles at all is the invariant.
    auto const outcome = cc::try_async_blocking_get(completion);
    CHECK(completion->is_ready());
    if (!outcome.has_value())
        CHECK(!stream.is_complete());
}

INVOCABLE_TEST("sg stream - dropping the handle cancels the transfer", (sg::context_handle const& handle))
{
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    auto const src = pattern(512, 5);
    auto buf = c.persistent.create_raw_buffer(512, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    REQUIRE(buf != nullptr);

    cc::shared_async<cc::unit const> completion;
    {
        auto stream = c.stream.bytes_to_buffer(buf, cc::make_pinned_data(src));
        completion = stream.completion();
    } // dropped: a stream always has an observer, so losing the observer ends the transfer

    // It must still settle — the node outlives the handle, and anything chained onto it has to be released.
    bool const settled = cc::try_async_blocking_get(completion).has_value() || completion->has_error();
    CHECK(settled);
    CHECK(completion->is_ready());
}

INVOCABLE_TEST("sg stream - promote_to_async makes a later list wait on the transfer", (sg::context_handle const& handle))
{
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    auto const src = pattern(8192, 17);
    auto buf = c.persistent.create_raw_buffer(8192, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    REQUIRE(buf != nullptr);

    auto stream = c.stream.bytes_to_buffer(buf, cc::make_pinned_data(src));
    REQUIRE(stream.is_valid());

    // Promotion is ADDITIVE: the handle keeps working, and the transfer additionally gains the automatic wait.
    // So a readback recorded after this sees the streamed bytes with no explicit synchronization at all — which is
    // what makes a low-priority stream a safe prewarm for something that turns out to be needed now.
    stream.promote_to_async();

    auto const back = c.download.bytes_from_buffer(buf, 0, 8192);
    auto const bytes = c.wait_for(back);
    REQUIRE(bytes.has_value());
    CHECK(bytes.value()[0] == src[0]);
    CHECK(bytes.value()[8191] == src[8191]);

    CHECK(stream.is_settled()); // still reporting, exactly as before the promotion
}

INVOCABLE_TEST("sg stream - streaming makes progress while async work saturates the queue",
               (sg::context_handle const& handle))
{
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    auto const payload = pattern(64 * 1024, 23);
    auto target = c.persistent.create_raw_buffer(64 * 1024, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    REQUIRE(target != nullptr);

    auto flood = c.persistent.create_raw_buffer(64 * 1024, sg::buffer_usage::copy_dst);
    REQUIRE(flood != nullptr);

    auto stream = c.stream.bytes_to_buffer(target, cc::make_pinned_data(payload));

    // Hammer the async tier while the stream is in flight.
    // The ratio is a floor on share, so the stream must still finish rather than being starved out by unbounded
    // higher-tier work.
    for (int i = 0; i < 32; ++i)
        c.upload.bytes_to_buffer(flood, cc::make_pinned_data(payload));

    REQUIRE(cc::try_async_blocking_get(stream.completion()).has_value());
    CHECK(stream.is_complete());

    auto const back = c.download.bytes_from_buffer(target, 0, 64 * 1024);
    auto const bytes = c.wait_for(back);
    REQUIRE(bytes.has_value());
    CHECK(bytes.value()[65535] == payload[65535]);
}

namespace
{
/// A source that hands its payload over in fixed-size pieces, always ready.
/// The simplest thing above the resident default, and enough to pin that chunk offsets land where they say.
class chunked_source final : public sg::stream_source
{
public:
    chunked_source(cc::span<byte const> bytes, isize chunk_bytes) : _bytes(bytes), _chunk(chunk_bytes) {}

    [[nodiscard]] sg::stream_poll try_next_chunk() override
    {
        if (_done == _bytes.size())
            return {.status = sg::stream_source_status::done};
        auto const n = cc::min(_chunk, _bytes.size() - _done);
        auto const offset = _done;
        _done += n;
        return {.status = sg::stream_source_status::ready,
                .chunk = {.data = cc::pinned_data<byte>::create_copy_of(_bytes.subspan(cc::offset_size{offset, n})),
                          .offset = offset}};
    }

    [[nodiscard]] i64 total_size_hint() const override { return i64(_bytes.size()); }

private:
    cc::span<byte const> _bytes;
    isize _chunk = 0;
    isize _done = 0;
};

/// A source that withholds its payload until release() is called, then wakes the actor.
/// This is the shape a real loader has, and what the waker exists for.
class gated_source final : public sg::stream_source
{
public:
    explicit gated_source(cc::span<byte const> bytes) : _bytes(bytes) {}

    [[nodiscard]] sg::stream_poll try_next_chunk() override
    {
        if (!_open.load(std::memory_order_acquire))
            return {.status = sg::stream_source_status::not_yet};
        if (_handed_over)
            return {.status = sg::stream_source_status::done};
        _handed_over = true;
        return {.status = sg::stream_source_status::ready,
                .chunk = {.data = cc::pinned_data<byte>::create_copy_of(_bytes)}};
    }

    [[nodiscard]] i64 total_size_hint() const override { return i64(_bytes.size()); }
    void set_waker(cc::unique_function<void()> waker) override { _waker = cc::move(waker); }

    /// Called from the test thread, exactly as a loader would call it from its own.
    void release()
    {
        _open.store(true, std::memory_order_release);
        if (_waker)
            _waker();
    }

private:
    cc::span<byte const> _bytes;
    std::atomic<bool> _open = false;
    cc::unique_function<void()> _waker;
    bool _handed_over = false;
};

/// A source that gives up, which is the only way out for one that cannot deliver what it promised.
class failing_source final : public sg::stream_source
{
public:
    [[nodiscard]] sg::stream_poll try_next_chunk() override { return {.status = sg::stream_source_status::failed}; }
};
} // namespace

INVOCABLE_TEST("sg stream - a chunked source lands every chunk where it says", (sg::context_handle const& handle))
{
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    auto const src = pattern(9000, 31);
    auto buf = c.persistent.create_raw_buffer(9000, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    REQUIRE(buf != nullptr);

    // 700 is deliberately not a divisor of 9000, so the last chunk is short.
    auto stream = c.stream.from_source_to_buffer(buf, std::make_unique<chunked_source>(cc::span<byte const>(src), 700));
    REQUIRE(cc::try_async_blocking_get(stream.completion()).has_value());
    CHECK(stream.is_complete());
    CHECK(stream.progress().bytes_done == 9000);

    auto const back = c.download.bytes_from_buffer(buf, 0, 9000);
    auto const bytes = c.wait_for(back);
    REQUIRE(bytes.has_value());
    CHECK(bytes.value()[0] == src[0]);
    CHECK(bytes.value()[699] == src[699]);   // end of the first chunk
    CHECK(bytes.value()[700] == src[700]);   // start of the second
    CHECK(bytes.value()[8999] == src[8999]); // the short tail
}

INVOCABLE_TEST("sg stream - a stalled source does not block other transfers", (sg::context_handle const& handle))
{
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    auto const gated_bytes = pattern(2048, 41);
    auto const ready_bytes = pattern(2048, 43);

    auto gated_buf = c.persistent.create_raw_buffer(2048, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    auto ready_buf = c.persistent.create_raw_buffer(2048, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    REQUIRE(gated_buf != nullptr);
    REQUIRE(ready_buf != nullptr);

    auto source = std::make_unique<gated_source>(cc::span<byte const>(gated_bytes));
    auto* const source_ptr = source.get();
    auto blocked = c.stream.from_source_to_buffer(gated_buf, cc::move(source));

    // The stalled transfer must be passed over rather than queueing everything behind it.
    // That is the whole reason not_yet is a distinct answer instead of a blocking read.
    auto ready = c.stream.bytes_to_buffer(ready_buf, cc::make_pinned_data(ready_bytes));
    REQUIRE(cc::try_async_blocking_get(ready.completion()).has_value());
    CHECK(!blocked.is_settled());

    source_ptr->release(); // the waker is what turns this into progress rather than an indefinite wait
    REQUIRE(cc::try_async_blocking_get(blocked.completion()).has_value());
    CHECK(blocked.is_complete());

    auto const back = c.download.bytes_from_buffer(gated_buf, 0, 2048);
    auto const bytes = c.wait_for(back);
    REQUIRE(bytes.has_value());
    CHECK(bytes.value()[2047] == gated_bytes[2047]);
}

INVOCABLE_TEST("sg stream - a failing source settles the transfer on its error channel",
               (sg::context_handle const& handle))
{
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    auto buf = c.persistent.create_raw_buffer(256, sg::buffer_usage::copy_dst);
    REQUIRE(buf != nullptr);

    auto stream = c.stream.from_source_to_buffer(buf, std::make_unique<failing_source>());

    // Without a failed answer, a source that cannot deliver would sit in the queue forever, and anything chained
    // onto its completion with it.
    CHECK(!cc::try_async_blocking_get(stream.completion()).has_value());
    CHECK(stream.is_settled());
    CHECK(!stream.is_complete());
}

INVOCABLE_TEST("sg stream - a chunked source fills a texture region", (sg::context_handle const& handle))
{
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    sg::texture_description desc;
    desc.format = sg::pixel_format::rgba8_unorm;
    desc.dimension = sg::texture_dimension::d2;
    desc.width = 64;
    desc.height = 64;
    desc.usage = sg::texture_usage::copy_src | sg::texture_usage::copy_dst;
    auto tex = c.persistent.create_raw_texture(desc);
    REQUIRE(tex != nullptr);

    isize const row_bytes = 64 * 4;
    auto const src = pattern(row_bytes * 64, 53);

    // Eight rows at a time: a texture chunk must fall on row boundaries, a row being the smallest unit a copy can
    // place.
    auto stream = c.stream.from_source_to_texture(
        tex, std::make_unique<chunked_source>(cc::span<byte const>(src), row_bytes * 8));
    REQUIRE(cc::try_async_blocking_get(stream.completion()).has_value());
    CHECK(stream.is_complete());

    auto const back = c.download.bytes_from_texture(tex);
    auto const bytes = c.wait_for(back);
    REQUIRE(bytes.has_value());
    REQUIRE(bytes.value().size() == src.size());
    CHECK(bytes.value()[0] == src[0]);
    CHECK(bytes.value()[row_bytes * 8] == src[row_bytes * 8]); // the second chunk's first row
    CHECK(bytes.value()[src.size() - 1] == src[src.size() - 1]);
}
