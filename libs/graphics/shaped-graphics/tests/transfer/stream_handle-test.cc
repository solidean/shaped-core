#include <clean-core/container/pinned_data.hh>
#include <clean-core/thread/async.hh>
#include <nexus/test.hh>
#include <shaped-graphics/transfer/stream_handle.hh>

using namespace cc::primitive_defines;

// The handle's own semantics, with no GPU and no backend behind it.
// A control block is all a handle needs, so the rules that matter most — dropping cancels, and every way of losing a
// handle is a drop — are testable here rather than only against a live copy actor.

namespace
{
[[nodiscard]] std::shared_ptr<sg::impl::stream_control> make_control()
{
    auto control = std::make_shared<sg::impl::stream_control>();
    control->completion = cc::make_async_manual<cc::unit>();
    return control;
}
} // namespace

TEST("sg stream_handle - dropping cancels, and so does being assigned over")
{
    auto first = make_control();
    auto second = make_control();

    {
        auto h = sg::stream_upload_handle(first);
        CHECK(h.is_valid());
        CHECK(!first->cancelled.load(std::memory_order_relaxed));

        // Assigning over a live handle is a drop of what it held: `first` loses its last observer here, and the
        // streaming tier's whole bargain is that a transfer always has one.
        h = sg::stream_upload_handle(second);
        CHECK(first->cancelled.load(std::memory_order_relaxed));
        CHECK(!second->cancelled.load(std::memory_order_relaxed));
    }

    CHECK(second->cancelled.load(std::memory_order_relaxed)); // the ordinary drop, at end of scope
}

TEST("sg stream_handle - moving out does not cancel")
{
    auto control = make_control();

    {
        auto from = sg::stream_upload_handle(control);
        auto to = cc::move(from);
        CHECK(to.is_valid());
        CHECK(!from.is_valid()); // moved-from is empty, so its destructor has nothing to cancel

        {
            auto assigned = sg::stream_upload_handle();
            assigned = cc::move(to);
            CHECK(assigned.is_valid());
            CHECK(!control->cancelled.load(std::memory_order_relaxed));
        }
        CHECK(control->cancelled.load(std::memory_order_relaxed));
    }
}

TEST("sg stream_handle - a download handle carries its future across a move")
{
    auto control = make_control();
    auto future = sg::bytes_future(cc::pinned_data<byte const>(), control->completion);

    auto h = sg::stream_download_handle(control, cc::move(future));
    CHECK(h.future().is_valid());

    auto moved = cc::move(h);
    CHECK(moved.future().is_valid());
    CHECK(!control->cancelled.load(std::memory_order_relaxed));
}
