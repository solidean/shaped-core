#include <nexus/test.hh>
#include <shaped-graphics/backends/dx12/dx12_buffer.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>

// dx12 backend bring-up: a command list and buffers through the public sg::context API.
// Part of the dx12 test binary (shaped-graphics-dx12-test), built only where the dx12 backend builds (Windows), so it never needs an #ifdef guard.
// See libs/graphics/shaped-graphics/docs/concepts/backends.md for how backend tests are organized.
//
// Both adapters are covered because the entry drivers (dx12-entry.cc) run every invocable on each: WARP, always present, and the real GPU when there is one.

namespace
{
namespace dx12 = sg::backend::dx12;
} // namespace

INVOCABLE_TEST("sg dx12 - context brings up lists and buffers", (dx12::dx12_context_handle const& handle))
{
    REQUIRE(handle != nullptr);
    CHECK(handle->backend() == sg::backend_kind::dx12);
    CHECK(handle->threading() == sg::thread_model::multi_threaded);

    auto& ctx = *handle;

    // Command list: handed out already recording.
    // Submitting consumes it, since it is moved in.
    auto cmd = ctx.create_command_list();
    REQUIRE(cmd != nullptr);
    ctx.submit_command_list(cc::move(cmd));

    // Buffer with real GPU storage.
    auto buf = ctx.persistent.create_raw_buffer(256, sg::buffer_usage::copy_dst);
    REQUIRE(buf != nullptr);
    CHECK(buf->size_in_bytes() == 256);
    CHECK(sg::has_flag(buf->usage(), sg::buffer_usage::copy_dst));

    // Empty buffer: valid and zero-length (the "no backing GPU resource" invariant is pinned separately below).
    auto empty = ctx.persistent.create_raw_buffer(0, sg::buffer_usage::none);
    REQUIRE(empty != nullptr);
    CHECK(empty->size_in_bytes() == 0);

    // read-write storage (UAV) usage takes the storage-buffer allocation path.
    auto storage = ctx.persistent.create_raw_buffer(1024, sg::buffer_usage::readwrite_buffer);
    REQUIRE(storage != nullptr);
    CHECK(storage->size_in_bytes() == 1024);

    // Explicit drop also consumes the list.
    auto to_drop = ctx.create_command_list();
    REQUIRE(to_drop != nullptr);
    ctx.drop_command_list(cc::move(to_drop));

    // The same paths once more: create a buffer, and create + drop a command list.
    auto via_base = ctx.persistent.create_raw_buffer(64, sg::buffer_usage::vertex_buffer);
    REQUIRE(via_base != nullptr);
    CHECK(via_base->size_in_bytes() == 64);

    auto base_cmd = ctx.create_command_list();
    REQUIRE(base_cmd != nullptr);
    ctx.drop_command_list(cc::move(base_cmd));
}

// Backend-internal invariant, with no public equivalent — hence the downcast to the concrete buffer.
// A size-0 buffer holds no ID3D12Resource, while a non-empty one does.
INVOCABLE_TEST("sg dx12 - a zero-size buffer allocates no backing resource", (dx12::dx12_context_handle const& handle))
{
    REQUIRE(handle != nullptr);
    auto& c = *handle;

    auto buf = c.create_dx12_buffer(256, sg::buffer_usage::copy_dst, sg::allocation_info{});
    REQUIRE(buf.has_value());
    CHECK(buf.value()->_resource != nullptr); // real storage -> a backing resource

    auto empty = c.create_dx12_buffer(0, sg::buffer_usage::none, sg::allocation_info{});
    REQUIRE(empty.has_value());
    CHECK(empty.value()->_resource == nullptr); // size 0 -> no resource allocated
}
