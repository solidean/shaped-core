#include "dx12-test-common.hh"

#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/thread/async.hh>
#include <nexus/test.hh>
#include <shaped-graphics/backends/dx12/dx12_buffer.hh>
#include <shaped-graphics/backends/dx12/dx12_completion_group.hh>

using namespace cc::primitive_defines;

// The completion-timeline invariant behind every async and streaming transfer, checked at the backend level because
// it is not observable through the sg API at all — only through what a resource's fence has reached.
//
// A window signals "the highest value I finished". That is exact only where completion order matches reservation
// order, which the transfer scheduler guarantees within one FAMILY — jobs sharing a destination — and deliberately
// breaks across families, since overtaking an unrelated blocked transfer is the point of out-of-order selection.
// So the timeline has to be per family too.
// Share one across resources and finishing a high-numbered transfer reports every lower-numbered one complete,
// handing a reader bytes that were never copied.

namespace
{
namespace dx12 = sg::backend::dx12;

[[nodiscard]] cc::vector<byte> pattern(isize n, int seed)
{
    cc::vector<byte> v;
    v.reserve(n);
    for (isize i = 0; i < n; ++i)
        v.push_back(byte((i * 11 + seed) & 0xFF));
    return v;
}
} // namespace

INVOCABLE_TEST("sg dx12 - each resource counts its transfers on its own timeline",
               (dx12::dx12_context_handle const& handle))
{
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    auto a = c.persistent.create_raw_buffer(1024, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    auto b = c.persistent.create_raw_buffer(1024, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);

    auto const* const da = static_cast<dx12::dx12_buffer const*>(a.get());
    auto const* const db = static_cast<dx12::dx12_buffer const*>(b.get());

    // Two copyable resources never share a timeline, which is what makes their values independent below.
    REQUIRE(da->_upload_group != nullptr);
    REQUIRE(db->_upload_group != nullptr);
    CHECK(da->_upload_group != db->_upload_group);

    // A resource that cannot be copied gets no timeline at all, so it costs no fence.
    auto plain = c.persistent.create_raw_buffer(256, sg::buffer_usage::readonly_buffer);
    REQUIRE(plain != nullptr);
    CHECK(static_cast<dx12::dx12_buffer const*>(plain.get())->_upload_group == nullptr);

    auto const bytes_a = pattern(1024, 1);
    auto const bytes_b = pattern(1024, 2);

    // Absolute values mean nothing — a recycled group carries its predecessor's counter on, so a fresh resource's
    // first value is wherever that counter had got to.
    // What must hold is that the two timelines advance INDEPENDENTLY, so prime `a` first and measure from there.
    c.upload.bytes_to_buffer(a, cc::make_pinned_data(bytes_a));
    u64 const a1 = da->_pending_async_upload_value.load();
    u64 const b0 = db->_pending_async_upload_value.load();

    // An upload to `b` in between must not consume a value from `a`'s timeline.
    // On the shared counter it did, and that is the whole defect: `a`'s value then sat a step further out than its
    // own work accounted for, so `b` finishing carried the fence past it while `a`'s copy was still outstanding.
    c.upload.bytes_to_buffer(b, cc::make_pinned_data(bytes_b));
    CHECK(da->_pending_async_upload_value.load() == a1);
    CHECK(db->_pending_async_upload_value.load() > b0);

    c.upload.bytes_to_buffer(a, cc::make_pinned_data(bytes_a));
    CHECK(da->_pending_async_upload_value.load() == a1 + 1); // one step, not two

    // And the bytes still land, which is the point of all of it.
    auto const back_a = c.wait_for(c.download.bytes_from_buffer(a, 0, 1024));
    auto const back_b = c.wait_for(c.download.bytes_from_buffer(b, 0, 1024));
    REQUIRE(back_a.has_value());
    REQUIRE(back_b.has_value());
    CHECK(back_a.value()[1023] == bytes_a[1023]);
    CHECK(back_b.value()[1023] == bytes_b[1023]);
}

INVOCABLE_TEST("sg dx12 - a stream finishing does not report an unrelated upload complete",
               (dx12::dx12_context_handle const& handle))
{
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    // The shape that broke: a big async upload spanning several staging windows, and a tiny streaming transfer to a
    // different destination that finishes in one of the earlier ones.
    // On a shared timeline the stream's completion signals past the upload's reserved value, so a reader of the
    // upload's destination stops waiting while its copy still has windows to go.
    auto const big = pattern(4 * 1024 * 1024, 3);
    auto const tiny = pattern(4096, 4);

    auto slow = c.persistent.create_raw_buffer(big.size(), sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    auto quick = c.persistent.create_raw_buffer(tiny.size(), sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    REQUIRE(slow != nullptr);
    REQUIRE(quick != nullptr);

    c.upload.bytes_to_buffer(slow, cc::make_pinned_data(big));
    auto stream = c.stream.bytes_to_buffer(quick, cc::make_pinned_data(tiny));

    REQUIRE(cc::try_async_blocking_get(stream.completion()).has_value());

    // The stream is done, which says nothing about `slow`.
    // Every byte of it must still arrive: on the shared timeline the readback's wait was satisfied by the stream's
    // signal, so it read a buffer whose later windows had not run yet.
    auto const back = c.wait_for(c.download.bytes_from_buffer(slow, 0, isize(big.size())));
    REQUIRE(back.has_value());
    bool matches = true;
    for (isize i = 0; i < isize(big.size()); ++i)
        if (back.value()[i] != big[i])
        {
            matches = false;
            break;
        }
    CHECK(matches);
}
