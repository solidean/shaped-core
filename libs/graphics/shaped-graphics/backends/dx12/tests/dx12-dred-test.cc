#include <clean-core/thread/async.hh>
#include <nexus/test.hh>
#include <shaped-graphics/backends/dx12/dx12_buffer.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/backends/dx12/dx12_dred.hh>
#include <shaped-graphics/exceptions.hh>

// DRED, driven through a device removal this test causes on purpose.
//
// `ID3D12Device5::RemoveDevice` is the runtime's own way to remove a device without a GPU fault, which is what
// makes the reporting path testable at all: a real removal is a driver event nobody can schedule.
//
// Its own context rather than an invocable on the shared one, for the obvious reason — the device does not
// survive, and every later invocable would run against a corpse.

using namespace cc::primitive_defines;

namespace
{
namespace dx12 = sg::backend::dx12;
} // namespace

TEST("sg dx12 - DRED reports the removal it was armed for", nx::config::exclusive())
{
    auto created = sg::create_dx12_context({.activate_global_debug_layer = true, .enable_dred = true});
    if (created.has_error())
        SKIP("no Direct3D 12 device");

    sg::context_handle const handle = created.value();
    auto& ctx = *handle;

    // A runtime without the DRED interfaces reports nothing, and there is no behaviour left to check.
    if (!dx12::enable_dred_once())
        SKIP("this D3D12 runtime has no DRED");

    // Some GPU work first, so the breadcrumbs have something to record.
    auto const buffer = ctx.persistent.create_buffer<u32>(64, sg::buffer_usage::copy_dst);
    {
        auto cmd = ctx.create_command_list();
        cmd->upload.pod_to_buffer(buffer, u32(7));
        ctx.submit_command_list(cc::move(cmd));
    }
    ctx.advance_epoch();
    ctx.block_until_idle();

    CHECK(!ctx.is_device_lost()); // the premise: nothing has gone wrong yet

    // And now take the device away.
    {
        auto& dx = static_cast<dx12::dx12_context&>(ctx);
        Microsoft::WRL::ComPtr<ID3D12Device5> device5;
        if (FAILED(dx._device->QueryInterface(IID_PPV_ARGS(&device5))))
            SKIP("ID3D12Device5 unavailable, so the removal cannot be provoked");
        device5->RemoveDevice();
    }

    // Any context operation now notices, which is what folds the DRED report into the reason.
    CHECK_THROWS_AS(ctx.persistent.create_buffer<u32>(64, sg::buffer_usage::copy_dst), sg::device_lost_exception);
    REQUIRE(ctx.is_device_lost());

    auto const reason = ctx.device_loss_reason();
    CHECK(reason.contains("device removed"));

    // And the report is EMPTY here, which is the assertion rather than a tolerated outcome.
    //
    // `RemoveDevice` takes a healthy device away: every list submitted above ran to completion, and nothing
    // faulted.
    // So there is no incomplete breadcrumb and no page fault, and a report that printed something anyway would
    // be printing the history of lists that were fine -- the noise that makes a real hang unreadable.
    // What a genuine hang looks like needs a GPU fault, which no test can raise without taking the machine's
    // display with it.
    auto const report = dx12::dred_report(static_cast<dx12::dx12_context&>(ctx)._device.Get());
    CHECK(report.empty());
}
