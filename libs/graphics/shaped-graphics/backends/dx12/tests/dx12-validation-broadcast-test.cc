#include "dx12-test-common.hh"

#include <clean-core/thread/atomic.hh>
#include <nexus/test.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>

// Does D3D12 hand one debug-layer message to EVERY callback in the process, or only to the device that raised it?
//
// The whole answer is load-bearing.
// `scoped_expected_validation_messages` is thread-scoped rather than per-context precisely because the broadcast was
// observed, and dx12-test-common.hh plus a paragraph of libs/graphics/shaped-graphics/docs/testing.md exist to
// explain that choice.
// If the message does NOT cross, all three should go: a per-context guard is simpler, it is what the vulkan backend
// already does, and the thread-scoped one would be silencing more than it needs to.
//
// So this test pins the observation rather than the workaround, and says which way it went and what follows.

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

    ComPtr<ID3D12Resource> unused;
    (void)ctx._device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_COPY_DEST, // must be GENERIC_READ on UPLOAD
                                               nullptr, IID_PPV_ARGS(&unused));
}
} // namespace

TEST("sg dx12 - a debug-layer message reaches every context's listener")
{
    // Two contexts, so "which device raised it" and "which listener saw it" are different questions.
    auto first = dx12::make_warp_context();
    if (first == nullptr)
        SKIP("no WARP adapter");
    auto second = dx12::make_warp_context();
    if (second == nullptr)
        SKIP("could not create a second WARP context");

    // The provocation is this test's subject, so the shared listener must not fail the test on it.
    dx12::scoped_expected_validation_messages const expect_complaint;

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
        .context("D3D12 broadcasts a debug-layer message to every registered callback, which is why "
                 "scoped_expected_validation_messages is thread-scoped rather than per-context; a failure here means "
                 "it no longer does, and that guard, its helper and the paragraph in testing.md should go");
}
