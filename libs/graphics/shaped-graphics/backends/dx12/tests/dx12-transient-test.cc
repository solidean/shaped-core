#include "dx12-test-common.hh"

#include <nexus/test.hh>

// dx12-specific transient-buffer invariant: the bump heap's 64 KiB placement granularity. The generic
// transient contract (round-trips, independence, expiry, storage reuse, the deferred set_budget) is pinned
// backend-agnostically in tests/transient/transient-test.cc and runs here too via the dx12 driver — this
// file keeps only what is specific to the dx12 placement math. On WARP so it runs headless on CI.
// See libs/graphics/shaped-graphics/docs/testing.md and libs/graphics/shaped-graphics/docs/concepts/memory.md.

namespace
{
namespace dx12 = sg::backend::dx12;
} // namespace

// Allocate one transient buffer per epoch for many epochs on a small budget. Each 256-byte buffer occupies
// a 64 KiB placement (D3D12's default resource alignment), so a 512 KiB budget fits only a handful — yet
// the bump head resets every epoch, so successive epochs alias the same storage and every epoch's data
// still round-trips. The 512 KiB budget is set deferred and takes effect from the second epoch on.
TEST("sg dx12 - transient buffer storage reused across many epochs")
{
    auto handle = dx12::acquire_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = static_cast<dx12::dx12_context&>(*handle);
    c.transient.set_budget(cc::isize(512) * 1024); // applied at the next advance_epoch (see set_budget)

    auto const usage = sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst;

    for (int e = 0; e < 30; ++e)
    {
        auto buf = c.transient.create_raw_buffer(256, usage);
        REQUIRE(buf.has_value());

        cc::byte src[256];
        for (int i = 0; i < 256; ++i)
            src[i] = cc::byte((i + e) & 0xFF);

        auto up = c.create_dx12_command_list();
        REQUIRE(up.has_value());
        up.value()->upload.bytes_to_buffer(buf.value(), cc::span<cc::byte const>(src, 256));
        c.submit_dx12_command_list(cc::move(up.value()));

        auto down = c.create_dx12_command_list();
        REQUIRE(down.has_value());
        auto future = down.value()->download.bytes_from_buffer(buf.value(), 0, 256);
        c.submit_dx12_command_list(cc::move(down.value()));

        auto const bytes = c.wait_for(future);
        REQUIRE(bytes.has_value());
        bool matches = true;
        for (int i = 0; i < 256; ++i)
            if (bytes.value()[i] != cc::byte((i + e) & 0xFF))
                matches = false;
        CHECK(matches);

        c.advance_epoch(2); // keep at most 2 epochs in flight → the bump head resets, aliasing storage
    }
}
