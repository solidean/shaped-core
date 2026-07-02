#include <nexus/test.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>

// vulkan backend bring-up test. This is its own binary (shaped-graphics-vulkan-test), built only
// where the vulkan backend builds (the SDK is present), so it never needs an #ifdef guard. Backend-
// specific tests live here rather than in the backend-agnostic shaped-graphics-test.
//
// Unlike dx12's WARP adapter, Vulkan has no guaranteed software device, so a context can't be created
// on a driver-less headless host. Each test therefore skips (returns) when creation fails — the same
// shape as dx12's "hardware context" test. The debug/validation layer is requested but best-effort:
// create_vulkan_context proceeds without it when the layer isn't installed.

namespace
{
namespace vulkan = sg::backend::vulkan;

// Drives the real vulkan paths against a live context via the backend-typed API (create_vulkan_*), so
// there are no downcasts and we can inspect the concrete resources.
void exercise_context(vulkan::vulkan_context& ctx)
{
    // Command list: handed out already recording. Submitting consumes it (moved in).
    auto cmd = ctx.create_vulkan_command_list();
    REQUIRE(cmd.has_value());
    REQUIRE(cmd.value() != nullptr);
    ctx.submit_vulkan_command_list(cc::move(cmd.value()));

    // Buffer with real GPU storage.
    auto buf = ctx.create_vulkan_buffer(256, sg::buffer_usage::copy_dst);
    REQUIRE(buf.has_value());
    CHECK(buf.value()->size_in_bytes() == 256);
    CHECK(sg::has_flag(buf.value()->usage(), sg::buffer_usage::copy_dst));
    CHECK(buf.value()->_buffer != VK_NULL_HANDLE);
    CHECK(buf.value()->_memory != VK_NULL_HANDLE);

    // Empty buffer: valid, and allocates no GPU resource.
    auto empty = ctx.create_vulkan_buffer(0, sg::buffer_usage::none);
    REQUIRE(empty.has_value());
    CHECK(empty.value()->size_in_bytes() == 0);
    CHECK(empty.value()->_buffer == VK_NULL_HANDLE);
    CHECK(empty.value()->_memory == VK_NULL_HANDLE);

    // storage usage takes the STORAGE_BUFFER path.
    auto storage = ctx.create_vulkan_buffer(1024, sg::buffer_usage::storage);
    REQUIRE(storage.has_value());
    CHECK(storage.value()->size_in_bytes() == 1024);

    // Explicit drop (backend-typed) also consumes the list.
    auto to_drop = ctx.create_vulkan_command_list();
    REQUIRE(to_drop.has_value());
    ctx.drop_vulkan_command_list(cc::move(to_drop.value()));

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

TEST("sg vulkan - context")
{
    auto ctx = sg::create_vulkan_context({.enable_validation_layers = true});
    if (ctx.has_error())
        return; // no Vulkan loader/driver/device (e.g. headless CI) — nothing to exercise.

    CHECK(ctx.value()->backend() == sg::backend_kind::vulkan);
    exercise_context(static_cast<vulkan::vulkan_context&>(*ctx.value()));
}

TEST("sg vulkan - software-preferred context")
{
    // prefer_software picks a CPU device (e.g. lavapipe) when one is present; otherwise it falls back
    // to hardware. Either way the same paths are exercised.
    auto ctx = sg::create_vulkan_context({.enable_validation_layers = true, .prefer_software_device = true});
    if (ctx.has_error())
        return; // no Vulkan-capable device available.

    CHECK(ctx.value()->backend() == sg::backend_kind::vulkan);
    exercise_context(static_cast<vulkan::vulkan_context&>(*ctx.value()));
}

namespace
{
// Fresh context for an epoch test, or nullptr on a host without a Vulkan device.
sg::context_handle make_context()
{
    auto ctx = sg::create_vulkan_context({.enable_validation_layers = true});
    return ctx.has_value() ? ctx.value() : nullptr;
}
} // namespace

TEST("sg vulkan - epoch advance and retire")
{
    auto handle = make_context();
    if (handle == nullptr)
        return; // no Vulkan device (e.g. headless CI).
    auto& c = static_cast<vulkan::vulkan_context&>(*handle);

    CHECK(c.current_epoch() == sg::epoch::first);
    // Nothing has finished yet, so the completed epoch is first-1.
    CHECK(cc::u64(c.completed_epoch()) == cc::u64(sg::epoch::first) - 1);

    c.advance_epoch_and_wait_for_idle();
    CHECK(c.current_epoch() == sg::epoch(cc::u64(sg::epoch::first) + 1));
    CHECK(cc::u64(c.completed_epoch()) >= cc::u64(sg::epoch::first)); // the first epoch is now done
}

TEST("sg vulkan - deferred deletion runs finalizers only after the owning epoch retires")
{
    auto handle = make_context();
    if (handle == nullptr)
        return;
    auto& c = static_cast<vulkan::vulkan_context&>(*handle);

    bool finalized = false;
    {
        auto buf = c.create_vulkan_buffer(256, sg::buffer_usage::copy_dst);
        REQUIRE(buf.has_value());
        buf.value()->add_finalizer([&finalized] { finalized = true; });
    } // last handle dropped here → deferred deletion staged in the current epoch

    // The owning epoch has not advanced/retired yet, so the resource is still (potentially) in use.
    CHECK(!finalized);

    c.advance_epoch_and_wait_for_idle(); // closes + drains the epoch the buffer died in
    CHECK(finalized);
}

TEST("sg vulkan - command pools are recycled across epochs")
{
    auto handle = make_context();
    if (handle == nullptr)
        return;
    auto& c = static_cast<vulkan::vulkan_context&>(*handle);

    auto const free_count
        = [&] { return c._command_pools.lock([](vulkan::vulkan_command_pool_set& p) { return p.free.size(); }); };

    auto cmd = c.create_vulkan_command_list();
    REQUIRE(cmd.has_value());
    c.submit_vulkan_command_list(cc::move(cmd.value()));
    CHECK(free_count() == 0); // still in flight — captured by the current epoch

    c.advance_epoch_and_wait_for_idle();
    CHECK(free_count() == 1); // reset and returned to the free set on retire

    // The next list reuses the pooled command pool rather than creating a new one.
    auto cmd2 = c.create_vulkan_command_list();
    REQUIRE(cmd2.has_value());
    CHECK(free_count() == 0);
    c.submit_vulkan_command_list(cc::move(cmd2.value()));
    c.advance_epoch_and_wait_for_idle();
    CHECK(free_count() == 1);
}

TEST("sg vulkan - submission token reports completion")
{
    auto handle = make_context();
    if (handle == nullptr)
        return;
    auto& c = static_cast<vulkan::vulkan_context&>(*handle);

    auto cmd = c.create_vulkan_command_list();
    REQUIRE(cmd.has_value());
    auto const token = c.submit_vulkan_command_list(cc::move(cmd.value()));

    c.advance_epoch_and_wait_for_idle(); // forces the GPU to catch up
    CHECK(c.is_submission_complete(token));
    CHECK(!c.is_submission_complete(sg::submission_token::not_submitted));
}

TEST("sg vulkan - throttle bounds epochs in flight")
{
    auto handle = make_context();
    if (handle == nullptr)
        return;
    auto& c = static_cast<vulkan::vulkan_context&>(*handle);

    // Allow at most one prior epoch in flight; after several advances the FIFO stays bounded.
    for (int i = 0; i < 5; ++i)
        c.advance_epoch(1);

    auto const in_flight = c._epoch_state.lock([](vulkan::vulkan_epoch_state& s) { return s.in_flight.size(); });
    CHECK(in_flight <= 1);
}
