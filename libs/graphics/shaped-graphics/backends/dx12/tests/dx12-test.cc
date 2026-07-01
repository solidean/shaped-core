#include <nexus/test.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>

// dx12 backend bring-up test. This is its own binary (shaped-graphics-dx12-test), built only where
// the dx12 backend builds (Windows), so it never needs an #ifdef guard. Backend-specific tests live
// here rather than in the backend-agnostic shaped-graphics-test.
//
// Two contexts are exercised:
//   - WARP (software adapter): always available on any Windows host, so it runs on headless CI.
//   - hardware (default): exercises a real GPU locally; skipped when no adapter is present (CI).
// The debug layer is requested but best-effort — create_dx12_context proceeds without it when the
// Graphics Tools feature isn't installed.

namespace
{
namespace dx12 = sg::backend::dx12;

// Drives the real dx12 paths against a live context via the backend-typed API (create_dx12_*), so
// there are no downcasts and we can inspect the concrete resources.
void exercise_context(dx12::dx12_context& ctx)
{
    // Command list: handed out already recording. Submitting consumes it (moved in).
    auto cmd = ctx.create_dx12_command_list();
    REQUIRE(cmd.has_value());
    REQUIRE(cmd.value() != nullptr);
    ctx.submit_dx12_command_list(cc::move(cmd.value()));

    // Buffer with real GPU storage.
    auto buf = ctx.create_dx12_buffer(256, sg::buffer_usage::copy_dst);
    REQUIRE(buf.has_value());
    CHECK(buf.value()->size_in_bytes() == 256);
    CHECK(sg::has_flag(buf.value()->usage(), sg::buffer_usage::copy_dst));
    CHECK(buf.value()->_resource != nullptr);

    // Empty buffer: valid, and allocates no GPU resource.
    auto empty = ctx.create_dx12_buffer(0, sg::buffer_usage::none);
    REQUIRE(empty.has_value());
    CHECK(empty.value()->size_in_bytes() == 0);
    CHECK(empty.value()->_resource == nullptr);

    // storage usage takes the ALLOW_UNORDERED_ACCESS path.
    auto storage = ctx.create_dx12_buffer(1024, sg::buffer_usage::storage);
    REQUIRE(storage.has_value());
    CHECK(storage.value()->size_in_bytes() == 1024);

    // Explicit drop (backend-typed) also consumes the list.
    auto to_drop = ctx.create_dx12_command_list();
    REQUIRE(to_drop.has_value());
    ctx.drop_dx12_command_list(cc::move(to_drop.value()));

    // The abstract sg::context API forwards to the same places: create a buffer, and create + drop a
    // command list, all through the base type.
    auto& base = static_cast<sg::context&>(ctx);

    auto via_base = base.create_buffer(64, sg::buffer_usage::vertex);
    REQUIRE(via_base.has_value());
    CHECK(via_base.value()->size_in_bytes() == 64);

    auto base_cmd = base.create_command_list();
    REQUIRE(base_cmd.has_value());
    base.drop_command_list(cc::move(base_cmd.value()));
}
} // namespace

TEST("sg dx12 - warp context")
{
    auto ctx = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    REQUIRE(ctx.has_value());
    CHECK(ctx.value()->backend() == sg::backend_kind::dx12);
    exercise_context(static_cast<dx12::dx12_context&>(*ctx.value()));
}

TEST("sg dx12 - hardware context")
{
    auto ctx = sg::create_dx12_context({.enable_debug_layer = true});
    if (ctx.has_error())
        return; // no D3D12-capable hardware adapter (e.g. headless CI); WARP test covers the path.

    CHECK(ctx.value()->backend() == sg::backend_kind::dx12);
    exercise_context(static_cast<dx12::dx12_context&>(*ctx.value()));
}
