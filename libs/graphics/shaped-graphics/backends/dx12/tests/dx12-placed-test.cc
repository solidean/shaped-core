#include "dx12-test-common.hh"

#include <nexus/test.hh>
#include <shaped-graphics/all.hh>

// Placed resources: buffers sub-allocated into a shared memory_heap at explicit offsets, on WARP so they
// run headless on CI. Verifies that two placements at distinct offsets in one heap are independent GPU
// storage, and that a UAV-usage buffer places correctly. All through the public sg API — placement is a
// backend-neutral feature (ctx.persistent.create_memory_heap + memory_heap placement queries). See
// libs/graphics/shaped-graphics/docs/concepts/memory.md.

namespace
{
cc::isize align_up(cc::isize value, cc::isize alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}
} // namespace

TEST("sg dx12 - two placed buffers share one heap without aliasing")
{
    auto handle = sg::backend::dx12::acquire_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    auto const usage = sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst;

    auto heap = c.persistent.create_memory_heap(cc::isize(1) << 20); // 1 MiB, ample for two buffers
    REQUIRE(heap != nullptr);

    // Query each buffer's footprint, then lay them out back-to-back: A at 0, B after A's occupied size.
    auto const reqs_a = heap->memory_requirements_for_buffer(256, usage);
    auto const reqs_b = heap->memory_requirements_for_buffer(256, usage);
    cc::isize const off_a = 0;
    cc::isize const off_b = align_up(reqs_a.size_in_bytes, reqs_b.alignment_in_bytes);

    auto const buf_a = c.persistent.create_raw_buffer(256, usage, heap->acquire_allocation_for_buffer(256, usage, off_a));
    auto const buf_b = c.persistent.create_raw_buffer(256, usage, heap->acquire_allocation_for_buffer(256, usage, off_b));
    REQUIRE(buf_a != nullptr);
    REQUIRE(buf_b != nullptr);

    cc::byte src_a[256];
    cc::byte src_b[256];
    for (int i = 0; i < 256; ++i)
    {
        src_a[i] = cc::byte(0xA0 + (i & 0xF));
        src_b[i] = cc::byte(0xB0 + (i & 0xF));
    }

    auto up = c.create_command_list();
    REQUIRE(up != nullptr);
    up->upload.bytes_to_buffer(buf_a, cc::span<cc::byte const>(src_a, 256));
    up->upload.bytes_to_buffer(buf_b, cc::span<cc::byte const>(src_b, 256));
    c.submit_command_list(cc::move(up));

    auto down = c.create_command_list();
    REQUIRE(down != nullptr);
    auto future_a = down->download.bytes_from_buffer(buf_a, 0, 256);
    auto future_b = down->download.bytes_from_buffer(buf_b, 0, 256);
    c.submit_command_list(cc::move(down));

    auto const bytes_a = c.wait_for(future_a);
    auto const bytes_b = c.wait_for(future_b);
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
    auto handle = sg::backend::dx12::acquire_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    auto const usage = sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst;

    auto heap = c.persistent.create_memory_heap(cc::isize(1) << 20);
    REQUIRE(heap != nullptr);

    auto const buf = c.persistent.create_raw_buffer(256, usage, heap->acquire_allocation_for_buffer(256, usage, 0));
    REQUIRE(buf != nullptr);

    // Drop our heap handle: the placement must keep the backing heap alive, so the buffer stays usable.
    heap.reset();

    cc::byte src[256];
    for (int i = 0; i < 256; ++i)
        src[i] = cc::byte(i);

    auto up = c.create_command_list();
    REQUIRE(up != nullptr);
    up->upload.bytes_to_buffer(buf, cc::span<cc::byte const>(src, 256));
    c.submit_command_list(cc::move(up));

    auto down = c.create_command_list();
    REQUIRE(down != nullptr);
    auto future = down->download.bytes_from_buffer(buf, 0, 256);
    c.submit_command_list(cc::move(down));

    auto const bytes = c.wait_for(future);
    REQUIRE(bytes.has_value());
    CHECK(bytes.value()[100] == cc::byte(100));
}

TEST("sg dx12 - placed read-write (UAV) buffer creates")
{
    auto handle = sg::backend::dx12::acquire_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    // readwrite_buffer usage sets the UAV flag on the desc; make sure that places cleanly.
    auto const usage = sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src;

    auto heap = c.persistent.create_memory_heap(cc::isize(1) << 20);
    REQUIRE(heap != nullptr);

    auto const buf = c.persistent.create_raw_buffer(1024, usage, heap->acquire_allocation_for_buffer(1024, usage, 0));
    REQUIRE(buf != nullptr);
    CHECK(buf->size_in_bytes() == 1024);
}
