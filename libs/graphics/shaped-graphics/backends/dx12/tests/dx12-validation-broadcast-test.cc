#include "dx12-test-common.hh"

#include <clean-core/common/utility.hh> // CC_DEFER
#include <clean-core/string/format.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/thread.hh>
#include <nexus/test.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>

// Does D3D12 hand one debug-layer message to every callback on the device that raised it, and to no other device's?
//
// Both halves are load-bearing for the backend's logging: without listeners, a message is logged once per device by the
// oldest context on it that has none — see dx12_context::set_message_callback.
// Two contexts on one adapter share a device, which is why the message crosses between them; WARP and hardware do not.

namespace
{
namespace dx12 = sg::backend::dx12;

// A pure diagnostic with nothing to clean up: an UPLOAD-heap resource must be created in GENERIC_READ, and the debug
// layer errors on any other initial state.
// Creation fails, so there is no resource to release and no device state to undo — the message is the only effect.
//
// Bypasses sg deliberately.
// Every sg path either asserts on bad input first or needs a shader, and what this needs is one message attributable
// to one device.
void provoke_validation_message(dx12::dx12_context& ctx)
{
    auto const heap = D3D12_HEAP_PROPERTIES{.Type = D3D12_HEAP_TYPE_UPLOAD};
    auto const desc = D3D12_RESOURCE_DESC{
        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Width = 256,
        .Height = 1,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = DXGI_FORMAT_UNKNOWN,
        .SampleDesc = {.Count = 1},
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
    };

    dx12::ComPtr<ID3D12Resource> unused;
    (void)ctx._device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_COPY_DEST, // must be GENERIC_READ on UPLOAD
                                               nullptr, IID_PPV_ARGS(&unused));
}
} // namespace

TEST("sg dx12 - a debug-layer message reaches every context's listener")
{
    // Two contexts, so "which device raised it" and "which listener saw it" are different questions.
    auto first = dx12::make_fresh_context();
    if (first == nullptr)
        SKIP("no dx12 adapter");
    auto second = dx12::make_fresh_context();
    if (second == nullptr)
        SKIP("could not create a second dx12 context");

    // The provocation is this test's subject.
    // Whether another context in the process logs it depends on what else is alive, so it is allowed rather than expected.
    nx::allow_errors("debug layer:", "sg.dx12");

    cc::atomic<int> seen_by_first = {0};
    cc::atomic<int> seen_by_second = {0};
    first->set_message_callback([&](dx12::dx12_message_severity, cc::string_view)
                                { seen_by_first.fetch_add(1, cc::memory_order_relaxed); });
    second->set_message_callback([&](dx12::dx12_message_severity, cc::string_view)
                                 { seen_by_second.fetch_add(1, cc::memory_order_relaxed); });

    provoke_validation_message(*first);

    // Both listeners go before the contexts do: they capture locals by reference, and a message raised during
    // teardown would otherwise run a callback over a dead frame.
    auto const first_count = seen_by_first.load(cc::memory_order_relaxed);
    auto const second_count = seen_by_second.load(cc::memory_order_relaxed);
    first->set_message_callback({});
    second->set_message_callback({});

    // "Did it cross" is only answerable once "was there a message at all" is answered.
    // A debug layer that is present but silent here means the provocation stopped working, not that the broadcast
    // went away — and asserting on the second listener then would quietly pass forever.
    if (first_count == 0)
        SKIP("the debug layer raised no message for the provocation, so this test learns nothing");

    CHECK(second_count > 0)
        .context("D3D12 broadcasts a debug-layer message to every callback on its device, which is why the backend "
                 "logs each message from one context per device; a failure here means it no longer does, and that "
                 "dedupe in dx12_context.create.cc should go");
}

/// What a listener on a third context saw of the provocation, which reaches it without changing who logs it.
///
/// The two tests below count LOG records, so they rest on two things the listener test above does not check: the message
/// is an error, and it is raised on the provoking thread, which is what attributes the record to the test.
/// A runtime delivering it otherwise is a runtime these tests learn nothing on, so they skip and say which half failed.
struct provocation_probe
{
    cc::atomic<int> errors = {0};
    cc::atomic<int> off_thread = {0};
    cc::atomic<int> other = {0};
    cc::thread_id provoking_thread = cc::current_thread_id();

    void listen(dx12::dx12_context& ctx)
    {
        ctx.set_message_callback(
            [this](dx12::dx12_message_severity severity, cc::string_view message)
            {
                if (!message.contains("CreateCommittedResource"))
                    other.fetch_add(1, cc::memory_order_relaxed);
                else if (cc::current_thread_id() != provoking_thread)
                    off_thread.fetch_add(1, cc::memory_order_relaxed);
                else if (severity >= dx12::dx12_message_severity::error)
                    errors.fetch_add(1, cc::memory_order_relaxed);
                else
                    other.fetch_add(1, cc::memory_order_relaxed);
            });
    }

    /// Why the counting tests cannot run here, or empty when they can.
    [[nodiscard]] cc::string unusable_because() const
    {
        auto const e = errors.load(cc::memory_order_relaxed);
        auto const t = off_thread.load(cc::memory_order_relaxed);
        auto const o = other.load(cc::memory_order_relaxed);
        CC_LOG_INFO("provocation probe: {} error(s) on the provoking thread, {} off it, {} other message(s)", e, t, o);
        if (e > 0)
            return {};
        return cc::format("the debug layer delivered the provocation as {} off-thread and {} other message(s), not as "
                          "an "
                          "error on the provoking thread",
                          t, o);
    }
};

TEST("sg dx12 - a debug-layer message is logged once however many contexts are alive")
{
    // Two contexts, neither with a listener: both callbacks receive the message, and only one of them may log it.
    auto first = dx12::make_fresh_context();
    if (first == nullptr)
        SKIP("no dx12 adapter");
    auto second = dx12::make_fresh_context();
    if (second == nullptr)
        SKIP("could not create a second dx12 context");
    auto probe_ctx = dx12::make_fresh_context();
    if (probe_ctx == nullptr)
        SKIP("could not create a probe dx12 context");

    auto probe = provocation_probe();
    probe.listen(*probe_ctx);
    CC_DEFER
    {
        probe_ctx->set_message_callback({});
    };

    provoke_validation_message(*first);

    // Declared after the probe: a skip must not leave an expectation behind to fail it.
    if (auto const why = probe.unusable_because(); !why.empty())
    {
        nx::allow_errors("CreateCommittedResource", "sg.dx12");
        nx::allow_warnings("CreateCommittedResource", "sg.dx12");
        SKIP(why);
    }

    // Exactly once whichever context is oldest, since the provoking thread is what attributes the record to this test.
    nx::expect_error("CreateCommittedResource", nx::exactly(1, "sg.dx12"));
    CHECK(true);
}

TEST("sg dx12 - a debug-layer message is logged by its own device when another adapter's context is older")
{
    // The broadcast stops at the device, so an older context on a different adapter must not be the one that logs.
    auto const warp = dx12::as_test_context(
        sg::create_dx12_context({.activate_global_debug_layer = true, .adapter = dx12::dx12_adapter::warp}));
    if (warp.has_error())
        SKIP("no dx12 WARP device");
    auto const hardware = dx12::as_test_context(
        sg::create_dx12_context({.activate_global_debug_layer = true, .adapter = dx12::dx12_adapter::hardware}));
    if (hardware.has_error())
        SKIP("no dx12 hardware device");
    auto const probe_ctx = dx12::as_test_context(
        sg::create_dx12_context({.activate_global_debug_layer = true, .adapter = dx12::dx12_adapter::hardware}));
    if (probe_ctx.has_error())
        SKIP("could not create a probe dx12 context");

    auto probe = provocation_probe();
    probe.listen(*probe_ctx.value());
    CC_DEFER
    {
        probe_ctx.value()->set_message_callback({});
    };

    provoke_validation_message(*hardware.value());

    if (auto const why = probe.unusable_because(); !why.empty())
    {
        nx::allow_errors("CreateCommittedResource", "sg.dx12");
        nx::allow_warnings("CreateCommittedResource", "sg.dx12");
        SKIP(why);
    }

    nx::expect_error("CreateCommittedResource", nx::exactly(1, "sg.dx12"));
    CHECK(true);
}
