#include <nexus/test.hh>
#include <shaped-graphics/backends/dx12/dx12_buffer.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>

// dx12 backend bring-up: creating a context (WARP + hardware), a command list, and buffers through
// the public sg::context API. Part of the dx12 test binary (shaped-graphics-dx12-test), built only
// where the dx12 backend builds (Windows), so it never needs an #ifdef guard. See how backend tests
// are organized:
// libs/graphics/shaped-graphics/docs/concepts/backends.md
//
// Two contexts are exercised:
//   - WARP (software adapter): always available on any Windows host, so it runs on headless CI.
//   - hardware (default): exercises a real GPU locally; skipped when no adapter is present (CI).
// The debug layer is requested but best-effort — create_dx12_context proceeds without it when the
// Graphics Tools feature isn't installed.

namespace
{
namespace dx12 = sg::backend::dx12;

// Drives the real GPU paths against a live context via the public sg::context API — no downcasts.
void exercise_context(sg::context& ctx)
{
    // Command list: handed out already recording. Submitting consumes it (moved in).
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
} // namespace

TEST("sg dx12 - warp context")
{
    auto ctx = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    REQUIRE(ctx.has_value());
    CHECK(ctx.value()->backend() == sg::backend_kind::dx12);
    CHECK(ctx.value()->threading() == sg::thread_model::multi_threaded);
    exercise_context(*ctx.value());
}

TEST("sg dx12 - hardware context")
{
    auto ctx = sg::create_dx12_context({.enable_debug_layer = true});
    if (ctx.has_error())
        return; // no D3D12-capable hardware adapter (e.g. headless CI); WARP test covers the path.

    CHECK(ctx.value()->backend() == sg::backend_kind::dx12);
    exercise_context(*ctx.value());
}

// Backend-internal invariant (no public equivalent — hence the downcast to the concrete buffer): a size-0
// buffer holds no ID3D12Resource, while a non-empty one does. Pins the dx12 empty-buffer optimization.
TEST("sg dx12 - a zero-size buffer allocates no backing resource")
{
    auto ctx = sg::create_dx12_context({.use_warp = true});
    REQUIRE(ctx.has_value());
    auto& c = static_cast<dx12::dx12_context&>(*ctx.value());

    auto buf = c.create_dx12_buffer(256, sg::buffer_usage::copy_dst, sg::allocation_info{});
    REQUIRE(buf.has_value());
    CHECK(buf.value()->_resource != nullptr); // real storage -> a backing resource

    auto empty = c.create_dx12_buffer(0, sg::buffer_usage::none, sg::allocation_info{});
    REQUIRE(empty.has_value());
    CHECK(empty.value()->_resource == nullptr); // size 0 -> no resource allocated
}
