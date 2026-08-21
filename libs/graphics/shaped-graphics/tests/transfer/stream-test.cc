#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/fwd.hh> // cc::byte
#include <clean-core/thread/async.hh>
#include <nexus/test.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/fwd.hh> // std::unique_ptr / std::shared_ptr
#include <shaped-graphics/resource/raw_buffer.hh>
#include <shaped-graphics/types.hh>

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
