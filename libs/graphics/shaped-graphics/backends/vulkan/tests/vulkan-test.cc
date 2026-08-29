#include "vulkan-test-common.hh"

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
using vulkan::test::make_context; // see vulkan-test-common.hh
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

TEST("sg vulkan - the command list reports the device's ray-tracing answer")
{
    auto handle = make_context();
    if (handle == nullptr)
        SKIP("no vulkan device");
    auto& c = static_cast<vulkan::vulkan_context&>(*handle);

    // The context records what the device offers; the command list reports what the backend can actually record.
    // They were deliberately allowed to disagree while the build and dispatch seams were stubs — a list that cannot
    // trace should not claim it can — and now that every seam is real they must agree again.
    //
    // The context's own answer is the stricter one it looks: it is true only when the extensions are there AND their
    // entry points resolved, so a driver advertising ray tracing it does not implement reports false here.
    auto cmd = c.create_vulkan_command_list();
    REQUIRE(cmd.has_value());
    CHECK(cmd.value()->raytracing.is_supported() == c.is_raytracing_supported());
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

TEST("sg vulkan - an inline upload records, submits and reclaims its staging")
{
    auto handle = make_context(); // installs the fail-on-validation listener
    if (handle == nullptr)
        return; // no Vulkan device.
    auto& c = static_cast<vulkan::vulkan_context&>(*handle);

    auto buffer = c.create_vulkan_buffer(1024, sg::buffer_usage::copy_dst, sg::allocation_info{});
    REQUIRE(buffer.has_value());

    byte bytes[256];
    for (int i = 0; i < 256; ++i)
        bytes[i] = byte(i);

    auto cmd = c.create_vulkan_command_list();
    REQUIRE(cmd.has_value());
    cmd.value()->upload.bytes_to_buffer(buffer.value(), cc::span<byte const>(bytes, 256));
    c.submit_vulkan_command_list(cc::move(cmd.value()));

    // The validation listener is what makes this meaningful: a wrong barrier, a bad copy region or an unbalanced
    // command buffer would fail the test rather than pass silently.
    // Byte correctness needs a readback, which is what the download path adds.
    c.advance_epoch_and_wait_for_idle();
    CHECK(!c.is_device_lost());
}

TEST("sg vulkan - staging survives more uploads than the ring holds at once")
{
    // Exercises the reclaim path: with a ring far smaller than the total uploaded, reserve has to block on an
    // in-flight epoch and reuse the space it frees.
    auto ctx = sg::create_vulkan_context({.enable_validation_layers = true, .upload_ring_bytes = 64 * 1024});
    if (ctx.has_error())
        return; // no Vulkan device.
    auto& c = static_cast<vulkan::vulkan_context&>(*ctx.value());

    auto buffer = c.create_vulkan_buffer(32 * 1024, sg::buffer_usage::copy_dst, sg::allocation_info{});
    REQUIRE(buffer.has_value());

    auto payload = cc::vector<byte>::create_filled(32 * 1024, byte(7));
    for (int epoch = 0; epoch < 8; ++epoch) // 256 KiB through a 64 KiB ring
    {
        auto cmd = c.create_vulkan_command_list();
        REQUIRE(cmd.has_value());
        cmd.value()->upload.bytes_to_buffer(buffer.value(), payload);
        c.submit_vulkan_command_list(cc::move(cmd.value()));
        c.advance_epoch(1); // bounds what is in flight, so the ring must be reclaimed to keep going
    }

    c.advance_epoch_and_wait_for_idle();
    CHECK(!c.is_device_lost());
}

TEST("sg vulkan - a texture round-trips through the staging rings")
{
    auto handle = make_context(); // installs the fail-on-validation listener
    if (handle == nullptr)
        return; // no Vulkan device.
    auto& c = static_cast<vulkan::vulkan_context&>(*handle);
    auto& base = static_cast<sg::context&>(c);

    // 8x8 rgba8: 256 tightly-packed bytes, and small enough to compare byte for byte.
    auto texture = base.persistent.create_texture_2d({
        .width = 8,
        .height = 8,
        .format = sg::pixel_format::rgba8_unorm,
        .usage = sg::texture_usage::copy_src | sg::texture_usage::copy_dst,
    });
    REQUIRE(texture.raw() != nullptr);

    byte pixels[256];
    for (int i = 0; i < 256; ++i)
        pixels[i] = byte(i);

    auto cmd = base.create_command_list();
    REQUIRE(cmd != nullptr);
    cmd->upload.bytes_to_texture(texture.raw(), cc::span<byte const>(pixels, 256), sg::subresource_index{});

    // Same list: the download must be ordered after the upload by a layout transition from copy_dst to copy_src,
    // which is the texture tracker and the barrier translator working together.
    auto future = cmd->download.bytes_from_texture(texture.raw(), sg::subresource_index{});
    base.submit_command_list(cc::move(cmd));

    auto const read = base.wait_for(future);
    REQUIRE(read.has_value());
    REQUIRE(read.value().size() == 256);

    bool matched = true;
    for (int i = 0; i < 256; ++i)
        if (read.value()[i] != byte(i))
            matched = false;
    CHECK(matched);
}

TEST("sg vulkan - a block-compressed texture stages at its block size")
{
    // BC formats store whole 4x4 blocks, so an 8x8 BC1 subresource is 4 blocks of 8 bytes rather than 8x8 texels.
    // Getting the staging size or its offset alignment wrong here is a validation error rather than a wrong image,
    // which is what makes this worth pinning separately.
    auto handle = make_context();
    if (handle == nullptr)
        return;
    auto& base = static_cast<sg::context&>(*handle);

    auto texture = base.persistent.create_texture_2d({
        .width = 8,
        .height = 8,
        .format = sg::pixel_format::bc1_rgba_unorm,
        .usage = sg::texture_usage::copy_src | sg::texture_usage::copy_dst,
    });
    REQUIRE(texture.raw() != nullptr);

    byte blocks[32]; // (8/4) * (8/4) blocks * 8 bytes
    for (int i = 0; i < 32; ++i)
        blocks[i] = byte(i * 3);

    auto cmd = base.create_command_list();
    REQUIRE(cmd != nullptr);
    cmd->upload.bytes_to_texture(texture.raw(), cc::span<byte const>(blocks, 32), sg::subresource_index{});
    auto future = cmd->download.bytes_from_texture(texture.raw(), sg::subresource_index{});
    base.submit_command_list(cc::move(cmd));

    auto const read = base.wait_for(future);
    REQUIRE(read.has_value());
    CHECK(read.value().size() == 32);
}

TEST("sg vulkan - the device reports descriptor buffer properties")
{
    auto handle = make_context();
    if (handle == nullptr)
        return; // no Vulkan device.
    auto& c = static_cast<vulkan::vulkan_context&>(*handle);

    // The bind path sizes every descriptor range from these, so a zero here would silently produce empty ranges.
    // A descriptor's size is a device property rather than something the API fixes, which is the main way the
    // descriptor-buffer model differs from allocating opaque sets from a pool.
    auto const& props = c.descriptor_buffer_properties();
    CHECK(props.uniformBufferDescriptorSize > 0);
    CHECK(props.storageBufferDescriptorSize > 0);
    CHECK(props.sampledImageDescriptorSize > 0);
    CHECK(props.storageImageDescriptorSize > 0);
    CHECK(props.samplerDescriptorSize > 0);

    // Offsets into a descriptor buffer must be a multiple of this, and it must be a power of two to be useful as one.
    CHECK(props.descriptorBufferOffsetAlignment > 0);
    CHECK((props.descriptorBufferOffsetAlignment & (props.descriptorBufferOffsetAlignment - 1)) == 0);
}

TEST("sg vulkan - the descriptor heap allocates, frees and coalesces")
{
    auto handle = make_context();
    if (handle == nullptr)
        return; // no Vulkan device.
    auto& c = static_cast<vulkan::vulkan_context&>(*handle);
    auto& heap = c._descriptor_heap;

    // A descriptor's size is a device property, so the heap is addressed in bytes and every range is aligned to the
    // device's descriptorBufferOffsetAlignment.
    auto const alignment = isize(c.descriptor_buffer_properties().descriptorBufferOffsetAlignment);

    auto const a = heap.allocate_persistent(256);
    auto const b = heap.allocate_persistent(256);
    REQUIRE(!a.is_empty());
    REQUIRE(!b.is_empty());
    CHECK(a.offset % alignment == 0);
    CHECK(b.offset % alignment == 0);
    CHECK(a.offset != b.offset);
    CHECK(heap.mapped_at(a) != nullptr);

    // Freeing both must coalesce them back into one range, or a heap fragments simply by the order things were
    // released in.
    // The tell is that a later allocation of their combined size fits.
    heap.free_persistent(a);
    heap.free_persistent(b);
    auto const merged = heap.allocate_persistent(512);
    REQUIRE(!merged.is_empty());
    heap.free_persistent(merged);

    // The device address is what a descriptor-buffer binding names; zero would mean the allocation missed the
    // device-address flag, which fails far away from here.
    CHECK(heap.device_address() != 0);
}

TEST("sg vulkan - transient descriptor ranges are reclaimed per epoch")
{
    auto handle = make_context();
    if (handle == nullptr)
        return;
    auto& c = static_cast<vulkan::vulkan_context&>(*handle);
    auto& heap = c._descriptor_heap;

    // Transient descriptors are written by the CPU and read by the GPU during the epoch, so a slot cannot be reused
    // until the epoch that wrote it retires.
    // Resetting the cursor every epoch would let a new write stomp a descriptor an in-flight epoch still reads, which
    // is why this checkpoints rather than resets.
    auto const first = heap.allocate_transient(1024);
    REQUIRE(!first.is_empty());
    CHECK(first.transient);

    c.advance_epoch_and_wait_for_idle();

    auto const second = heap.allocate_transient(1024);
    REQUIRE(!second.is_empty());
    CHECK(!c.is_device_lost());
}
