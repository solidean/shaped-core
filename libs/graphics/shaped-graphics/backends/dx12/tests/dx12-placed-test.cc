#include "dx12-test-common.hh"

#include <nexus/test.hh>

// Placed resources: buffers sub-allocated into a shared memory_heap via CreatePlacedResource, on WARP
// so they run headless on CI. Verifies that two placements at distinct offsets in one heap are
// independent GPU storage, and that a UAV-usage buffer places correctly (its desc carries the
// ALLOW_UNORDERED_ACCESS flag). See libs/graphics/shaped-graphics/docs/concepts/memory.md.

namespace
{
namespace dx12 = sg::backend::dx12;

cc::isize align_up(cc::isize value, cc::isize alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}
} // namespace

TEST("sg dx12 - two placed buffers share one heap without aliasing")
{
    auto handle = dx12::make_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = static_cast<dx12::dx12_context&>(*handle);

    auto const usage = sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst;

    auto heap_r = c.persistent.create_memory_heap(cc::isize(1) << 20); // 1 MiB, ample for two buffers
    REQUIRE(heap_r.has_value());
    auto const heap = heap_r.value();

    // Query each buffer's footprint, then lay them out back-to-back: A at 0, B after A's occupied size.
    auto const reqs_a = heap->memory_requirements_for_buffer(256, usage);
    auto const reqs_b = heap->memory_requirements_for_buffer(256, usage);
    cc::isize const off_a = 0;
    cc::isize const off_b = align_up(reqs_a.size_in_bytes, reqs_b.alignment_in_bytes);

    auto const buf_a = c.create_dx12_buffer(256, usage, heap->acquire_allocation_for_buffer(256, usage, off_a));
    auto const buf_b = c.create_dx12_buffer(256, usage, heap->acquire_allocation_for_buffer(256, usage, off_b));
    REQUIRE(buf_a.has_value());
    REQUIRE(buf_b.has_value());

    cc::byte src_a[256];
    cc::byte src_b[256];
    for (int i = 0; i < 256; ++i)
    {
        src_a[i] = cc::byte(0xA0 + (i & 0xF));
        src_b[i] = cc::byte(0xB0 + (i & 0xF));
    }

    auto up = c.create_dx12_command_list();
    REQUIRE(up.has_value());
    up.value()->upload.bytes_to_buffer(buf_a.value(), cc::span<cc::byte const>(src_a, 256));
    up.value()->upload.bytes_to_buffer(buf_b.value(), cc::span<cc::byte const>(src_b, 256));
    c.submit_dx12_command_list(cc::move(up.value()));

    auto down = c.create_dx12_command_list();
    REQUIRE(down.has_value());
    auto future_a = down.value()->download.bytes_from_buffer(buf_a.value(), 0, 256);
    auto future_b = down.value()->download.bytes_from_buffer(buf_b.value(), 0, 256);
    c.submit_dx12_command_list(cc::move(down.value()));

    auto const bytes_a = future_a.wait_get_bytes();
    auto const bytes_b = future_b.wait_get_bytes();
    REQUIRE(bytes_a.has_value());
    REQUIRE(bytes_b.has_value());

    // Each placement holds exactly its own data — distinct offsets don't alias.
    bool ok_a = true;
    bool ok_b = true;
    for (int i = 0; i < 256; ++i)
    {
        if (bytes_a.value()[i] != cc::byte(0xA0 + (i & 0xF)))
            ok_a = false;
        if (bytes_b.value()[i] != cc::byte(0xB0 + (i & 0xF)))
            ok_b = false;
    }
    CHECK(ok_a);
    CHECK(ok_b);
}

TEST("sg dx12 - placed buffer keeps its heap alive")
{
    auto handle = dx12::make_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = static_cast<dx12::dx12_context&>(*handle);

    auto const usage = sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst;

    auto heap_r = c.persistent.create_memory_heap(cc::isize(1) << 20);
    REQUIRE(heap_r.has_value());

    auto const buf = c.create_dx12_buffer(256, usage, heap_r.value()->acquire_allocation_for_buffer(256, usage, 0));
    REQUIRE(buf.has_value());

    // Drop our heap handle: the placement must keep the heap's ID3D12Heap alive, so the buffer stays usable.
    heap_r.value().reset();

    cc::byte src[256];
    for (int i = 0; i < 256; ++i)
        src[i] = cc::byte(i);

    auto up = c.create_dx12_command_list();
    REQUIRE(up.has_value());
    up.value()->upload.bytes_to_buffer(buf.value(), cc::span<cc::byte const>(src, 256));
    c.submit_dx12_command_list(cc::move(up.value()));

    auto down = c.create_dx12_command_list();
    REQUIRE(down.has_value());
    auto future = down.value()->download.bytes_from_buffer(buf.value(), 0, 256);
    c.submit_dx12_command_list(cc::move(down.value()));

    auto const bytes = future.wait_get_bytes();
    REQUIRE(bytes.has_value());
    CHECK(bytes.value()[100] == cc::byte(100));
}

TEST("sg dx12 - placed read-write (UAV) buffer creates")
{
    auto handle = dx12::make_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = static_cast<dx12::dx12_context&>(*handle);

    // readwrite_buffer usage sets ALLOW_UNORDERED_ACCESS on the desc; make sure that places cleanly.
    auto const usage = sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src;

    auto heap_r = c.persistent.create_memory_heap(cc::isize(1) << 20);
    REQUIRE(heap_r.has_value());

    auto const buf = c.create_dx12_buffer(1024, usage, heap_r.value()->acquire_allocation_for_buffer(1024, usage, 0));
    REQUIRE(buf.has_value());
    CHECK(buf.value()->size_in_bytes() == 1024);
}
