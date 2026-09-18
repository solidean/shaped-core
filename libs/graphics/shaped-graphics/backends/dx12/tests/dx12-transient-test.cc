#include "dx12-test-common.hh"

#include <clean-core/common/utility.hh> // CC_DEFER
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// dx12-specific transient-buffer invariant: the bump heap's 64 KiB placement granularity.
// The generic transient contract — round-trips, independence, expiry, storage reuse, the deferred set_budget — is pinned backend-agnostically in tests/transient/transient-test.cc.
// That suite runs here too, via the dx12 driver.
// This file keeps only what is specific to the dx12 placement math.
// It runs on every adapter the entry drivers bring up, WARP included.
// See libs/graphics/shaped-graphics/docs/testing.md and libs/graphics/shaped-graphics/docs/concepts/memory.md.

namespace
{
namespace dx12 = sg::backend::dx12;
} // namespace

// Allocate one transient buffer per epoch for many epochs on a small budget.
// Each 256-byte buffer occupies a 64 KiB placement (D3D12's default resource alignment), so a 512 KiB budget fits only a handful.
// Yet the bump head resets every epoch, so successive epochs alias the same storage and every epoch's data still round-trips.
// The 512 KiB budget is set deferred and takes effect from the second epoch on.
ASYNC_INVOCABLE_TEST("sg dx12 - transient buffer storage reused across many epochs",
                     (dx12::dx12_context_handle const& handle))
{
    REQUIRE(handle != nullptr);
    auto& c = *handle;
    c.transient.set_budget(isize(512) * 1024); // applied at the next advance_epoch (see set_budget)
    // The context is shared with every later test under this driver, so the default goes back even past a failed REQUIRE.
    // set_budget is deferred, hence the advance that applies it.
    CC_DEFER
    {
        c.transient.set_budget(sg::context_transient_scope::default_budget_bytes);
        c.advance_epoch();
    };

    auto const usage = sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst;

    for (int e = 0; e < 30; ++e)
    {
        auto buf = c.transient.create_raw_buffer(256, usage);
        REQUIRE(buf != nullptr);

        byte src[256];
        for (int i = 0; i < 256; ++i)
            src[i] = byte((i + e) & 0xFF);

        auto up = c.create_command_list();
        REQUIRE(up != nullptr);
        up->upload.bytes_to_buffer(buf, cc::span<byte const>(src, 256));
        c.submit_command_list(cc::move(up));

        auto down = c.create_command_list();
        REQUIRE(down != nullptr);
        auto future = down->download.bytes_from_buffer(buf, 0, 256);
        c.submit_command_list(cc::move(down));

        co_await c.idle_completion();
        auto const bytes = future.try_get_bytes();
        REQUIRE(bytes.has_value());
        bool matches = true;
        for (int i = 0; i < 256; ++i)
            if (bytes.value()[i] != byte((i + e) & 0xFF))
                matches = false;
        CHECK(matches);

        c.advance_epoch();
        co_await c.epochs_in_flight_completion(2); // keep at most 2 epochs in flight → the bump head resets, aliasing storage
    }
}
