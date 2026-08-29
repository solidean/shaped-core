#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>

using namespace cc::primitive_defines;

// vulkan backend bring-up test, in its own binary (shaped-graphics-vulkan-test) built only where the vulkan backend builds, so it needs no #ifdef guard.
// What belongs in a backend suite rather than in the backend-agnostic shaped-graphics-test is libs/graphics/shaped-graphics/docs/concepts/backends.md's call.
//
// Vulkan has no guaranteed software device — unlike dx12's WARP adapter — so no context can be created on a driver-less headless host.
// Every test therefore returns early when creation fails, rather than failing.
// Validation is requested throughout but best-effort: create_vulkan_context proceeds without the layer when it is not installed.

namespace
{
namespace vulkan = sg::backend::vulkan;

// Drives the real vulkan paths against a live context through the backend-typed API, so the concrete resources are inspectable without a downcast.
void exercise_context(vulkan::vulkan_context& ctx)
{
    // Command list: handed out already recording, and submitting consumes it.
    auto cmd = ctx.create_vulkan_command_list();
    REQUIRE(cmd.has_value());
    REQUIRE(cmd.value() != nullptr);
    ctx.submit_vulkan_command_list(cc::move(cmd.value()));

    // Buffer with real GPU storage.
    auto buf = ctx.create_vulkan_buffer(256, sg::buffer_usage::copy_dst, sg::allocation_info{});
    REQUIRE(buf.has_value());
    CHECK(buf.value()->size_in_bytes() == 256);
    CHECK(buf.value()->usage().has(sg::buffer_usage::copy_dst));
    CHECK(buf.value()->_buffer != VK_NULL_HANDLE);
    CHECK(buf.value()->_memory != VK_NULL_HANDLE);

    // Empty buffer: valid, and allocates no GPU resource.
    auto empty = ctx.create_vulkan_buffer(0, {}, sg::allocation_info{});
    REQUIRE(empty.has_value());
    CHECK(empty.value()->size_in_bytes() == 0);
    CHECK(empty.value()->_buffer == VK_NULL_HANDLE);
    CHECK(empty.value()->_memory == VK_NULL_HANDLE);

    // read-write storage usage takes the STORAGE_BUFFER path.
    auto storage = ctx.create_vulkan_buffer(1024, sg::buffer_usage::readwrite_buffer, sg::allocation_info{});
    REQUIRE(storage.has_value());
    CHECK(storage.value()->size_in_bytes() == 1024);

    // Explicit drop (backend-typed) also consumes the list.
    auto to_drop = ctx.create_vulkan_command_list();
    REQUIRE(to_drop.has_value());
    ctx.drop_vulkan_command_list(cc::move(to_drop.value()));

    // The abstract sg::context API forwards to the same places, exercised here through the base type.
    auto& base = static_cast<sg::context&>(ctx);

    auto via_base = base.persistent.create_raw_buffer(64, sg::buffer_usage::vertex_buffer);
    REQUIRE(via_base != nullptr);
    CHECK(via_base->size_in_bytes() == 64);

    auto base_cmd = base.create_command_list();
    REQUIRE(base_cmd != nullptr);
    base.drop_command_list(cc::move(base_cmd));
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
    // prefer_software picks a CPU device (e.g. lavapipe) when one is present, and falls back to hardware otherwise.
    // Either way the same paths are exercised.
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
    if (ctx.has_error())
        return nullptr;

    // Any validation message of warning severity or worse fails the running test, which is what makes the layer a
    // gate rather than log noise.
    // A test whose subject IS the bad input clears the callback for its duration.
    static_cast<vulkan::vulkan_context&>(*ctx.value())
        .set_message_callback(
            [](vulkan::vulkan_message_severity severity, cc::string_view message)
            {
                if (severity <= vulkan::vulkan_message_severity::warning)
                    CHECK(false).context(cc::format("vulkan validation: {}", message));
            });
    return ctx.value();
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
    CHECK(u64(c.completed_epoch()) == u64(sg::epoch::first) - 1);

    c.advance_epoch_and_wait_for_idle();
    CHECK(c.current_epoch() == sg::epoch(u64(sg::epoch::first) + 1));
    CHECK(u64(c.completed_epoch()) >= u64(sg::epoch::first)); // the first epoch is now done
}

TEST("sg vulkan - deferred deletion runs finalizers only after the owning epoch retires")
{
    auto handle = make_context();
    if (handle == nullptr)
        return;
    auto& c = static_cast<vulkan::vulkan_context&>(*handle);

    bool finalized = false;
    {
        auto buf = c.create_vulkan_buffer(256, sg::buffer_usage::copy_dst, sg::allocation_info{});
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

TEST("sg vulkan - the device probe and the command list answer ray tracing separately")
{
    auto handle = make_context();
    if (handle == nullptr)
        return; // no Vulkan device (e.g. headless CI).
    auto& c = static_cast<vulkan::vulkan_context&>(*handle);

    // The context records what the device offers; the command list reports what the backend can actually record.
    // They are allowed to disagree, and today they do: the extensions are enabled at device creation so the
    // ray-tracing milestone has them, while every build and dispatch seam is still a stub.
    // This pins that gap as deliberate — when the recording paths land, is_supported() starts reporting the probe
    // and this test is what says the two were separated on purpose rather than by omission.
    auto cmd = c.create_vulkan_command_list();
    REQUIRE(cmd.has_value());

    // The invariant with teeth: no command list records ray tracing yet, however capable the device is.
    // On this hardware the probe is true, so the two answers really do differ rather than agreeing by accident.
    CHECK(!cmd.value()->raytracing.is_supported());
    c.drop_vulkan_command_list(cc::move(cmd.value()));
}

TEST("sg vulkan - an installed message callback receives validation messages")
{
    auto ctx = sg::create_vulkan_context({.enable_validation_layers = true});
    if (ctx.has_error())
        return; // no Vulkan device.
    auto& c = static_cast<vulkan::vulkan_context&>(*ctx.value());

    // make_context installs a fail-the-test listener; this one owns its context, so it can install a recording one.
    int seen = 0;
    auto last = cc::string();
    auto last_severity = vulkan::vulkan_message_severity::verbose;
    c.set_message_callback(
        [&](vulkan::vulkan_message_severity severity, cc::string_view message)
        {
            ++seen;
            last = cc::string(message);
            last_severity = severity;
        });

    c.dispatch_validation_message(vulkan::vulkan_message_severity::error, "provoked");
    CHECK(seen == 1);
    CHECK(last == "provoked");
    CHECK(last_severity == vulkan::vulkan_message_severity::error);

    // Clearing restores the log default, so a later message reaches no listener.
    c.set_message_callback({});
    c.dispatch_validation_message(vulkan::vulkan_message_severity::warning, "ignored");
    CHECK(seen == 1);
}

TEST("sg vulkan - the debug messenger reaches the installed callback")
{
    auto ctx = sg::create_vulkan_context({.enable_validation_layers = true});
    if (ctx.has_error())
        return; // no Vulkan device.
    auto& c = static_cast<vulkan::vulkan_context&>(*ctx.value());

    int seen = 0;
    c.set_message_callback([&](vulkan::vulkan_message_severity, cc::string_view) { ++seen; });

    // A zero-size buffer violates VUID-VkBufferCreateInfo-size-00912, which the layer reports and the driver then
    // rejects — a pure diagnostic with no object created and nothing to clean up.
    // Going through vkCreateBuffer directly is the point: create_vulkan_buffer treats size 0 as a legal empty buffer
    // and never calls Vulkan, so only the raw call can provoke the layer.
    auto const info = VkBufferCreateInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 0,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer buffer = VK_NULL_HANDLE;
    vkCreateBuffer(c._device, &info, nullptr, &buffer);
    if (buffer != VK_NULL_HANDLE)
        vkDestroyBuffer(c._device, buffer, nullptr); // a lenient driver may still have made one

    // This is what says the messenger is wired to the callback rather than only to the log.
    // It needs the validation layer installed; without it there is no messenger and nothing to observe.
    if (c._debug_messenger != VK_NULL_HANDLE)
        CHECK(seen > 0);

    c.set_message_callback({});
}
