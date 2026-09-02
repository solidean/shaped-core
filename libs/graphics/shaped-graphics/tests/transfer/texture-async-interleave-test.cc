#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>

using namespace cc::primitive_defines;

// Interleaving a command list with an async transfer of ONE texture, which sg did not promise until the direct queue
// began settling the layout for the transfer.
//
// These are the cases libs/graphics/shaped-graphics/docs/TODO.md carried as its gate while neither backend could hold
// them: an async transfer runs on a queue that cannot settle a layout for itself, so a list that left the texture
// somewhere else made the copy illegal on dx12 and unverifiable on vulkan.
// Both edges are the point — a transfer composing after a list, and a list composing after a transfer — because each
// is carried by a different half of the per-resource stamps.
// The forward edge read through an INLINE readback is the one case still missing here, and
// libs/graphics/shaped-graphics/docs/TODO.md carries it with what it is waiting on.
//
// The oracle is as much the debug layer as the bytes: both entry points fail a test on any message of warning severity
// or worse, and the vulkan one runs synchronization validation too.
// See libs/graphics/shaped-graphics/docs/concepts/barriers.md.

namespace
{
constexpr int k_extent = 16;
constexpr isize k_bytes = isize(k_extent) * k_extent * 4;

byte pattern_at(isize i, int salt)
{
    return byte((int(i) * 7 + salt) & 0xFF);
}

cc::vector<byte> pattern(int salt)
{
    cc::vector<byte> out;
    out.reserve(k_bytes);
    for (isize i = 0; i < k_bytes; ++i)
        out.push_back(pattern_at(i, salt));
    return out;
}

cc::pinned_data<byte const> pinned_pattern(int salt)
{
    return cc::make_pinned_data(pattern(salt));
}

bool matches(cc::span<byte const> bytes, int salt)
{
    if (bytes.size() != k_bytes)
        return false;
    for (isize i = 0; i < k_bytes; ++i)
        if (bytes[i] != pattern_at(i, salt))
            return false;
    return true;
}

// Sampled as well as copyable, so the layout the texture starts in is a specific one rather than a transfer layout —
// which is what makes the fixup actually have something to do.
sg::raw_texture_handle make_texture(sg::context_handle const& ctx)
{
    sg::texture_description desc;
    desc.format = sg::pixel_format::rgba8_unorm;
    desc.dimension = sg::texture_dimension::d2;
    desc.width = k_extent;
    desc.height = k_extent;
    desc.usage = sg::texture_usage::copy_src | sg::texture_usage::copy_dst | sg::texture_usage::readonly_texture;
    auto tex = ctx->persistent.create_raw_texture(desc);
    CC_ASSERT(tex != nullptr, "texture allocation failed");
    return tex;
}

// Read the texture back through a command list, which is the consumer that has to compose with the transfer.
cc::optional<cc::pinned_data<byte const>> read_back(sg::context_handle const& ctx, sg::raw_texture_handle const& tex)
{
    auto cmd = ctx->create_command_list();
    CC_ASSERT(cmd != nullptr, "command list creation failed");
    auto future = cmd->download.bytes_from_texture(tex);
    ctx->submit_command_list(cc::move(cmd));
    return ctx->wait_for(future);
}
} // namespace

INVOCABLE_TEST("sg - async texture upload composes after a list that wrote the texture", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    auto const tex = make_texture(ctx);

    // The reverse edge: the list submits first, and the async upload must land after it rather than under it.
    // Nothing here waits, so the ordering is entirely the reverse stamp's.
    {
        auto cmd = ctx->create_command_list();
        REQUIRE(cmd != nullptr);
        cmd->upload.bytes_to_texture(tex, cc::span<byte const>(pattern(31)));
        ctx->submit_command_list(cc::move(cmd));
    }

    ctx->upload.bytes_to_texture(tex, pinned_pattern(59));

    auto const bytes = ctx->wait_for(ctx->download.bytes_from_texture(tex));
    REQUIRE(bytes.has_value());
    CHECK(matches(bytes.value(), 59)).context("the async upload did not compose after the list's write");

    ctx->advance_epoch_and_wait_for_idle();
}

INVOCABLE_TEST("sg - an async texture transfer leaves the texture async-ready, and a later list transitions out of it",
               (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    auto const tex = make_texture(ctx);

    // Nothing restores a layout any more: the transfer leaves the texture where it needed it, and the next list's
    // entry barrier repairs that like it would after any other predecessor.
    // What this pins is that the repair happens — an inline write, an async readback, then another inline write and
    // readback, all on one texture with no ceremony between them.
    {
        auto cmd = ctx->create_command_list();
        REQUIRE(cmd != nullptr);
        cmd->upload.bytes_to_texture(tex, cc::span<byte const>(pattern(13)));
        ctx->submit_command_list(cc::move(cmd));
    }

    auto const first = ctx->wait_for(ctx->download.bytes_from_texture(tex));
    REQUIRE(first.has_value());
    CHECK(matches(first.value(), 13));

    {
        auto cmd = ctx->create_command_list();
        REQUIRE(cmd != nullptr);
        cmd->upload.bytes_to_texture(tex, cc::span<byte const>(pattern(29)));
        ctx->submit_command_list(cc::move(cmd));
    }

    auto const second = read_back(ctx, tex);
    REQUIRE(second.has_value());
    CHECK(matches(second.value(), 29)).context("the texture did not survive an async transfer taken in between");

    ctx->advance_epoch_and_wait_for_idle();
}

INVOCABLE_TEST("sg - prepare_for_async settles the layout the transfer needs", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    auto const tex = make_texture(ctx);

    // The point of the call: the list the caller was building anyway leaves the texture ready, so the transfer needs
    // no fixup submit of its own and says nothing.
    // The bytes prove the prepare did not cost the write — a layout transition is not a discard.
    {
        auto cmd = ctx->create_command_list();
        REQUIRE(cmd != nullptr);
        cmd->upload.bytes_to_texture(tex, cc::span<byte const>(pattern(41)));
        cmd->prepare_for_async(tex, sg::async_direction::download);
        ctx->submit_command_list(cc::move(cmd));
    }

    auto const bytes = ctx->wait_for(ctx->download.bytes_from_texture(tex));
    REQUIRE(bytes.has_value());
    CHECK(matches(bytes.value(), 41));

    ctx->advance_epoch_and_wait_for_idle();
}

INVOCABLE_TEST("sg - ensure_layout leaves a texture where the next list finds it", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    auto const tex = make_texture(ctx);

    // A pure transition, declared and flushed like any other access, with no copy attached.
    // It must not discard what the list just wrote, which is the failure mode worth pinning: `undefined` as a
    // destination is exactly how a texture loses its contents.
    {
        auto cmd = ctx->create_command_list();
        REQUIRE(cmd != nullptr);
        cmd->upload.bytes_to_texture(tex, cc::span<byte const>(pattern(67)));
        cmd->ensure_layout(tex, sg::texture_layout::shader_readonly);
        ctx->submit_command_list(cc::move(cmd));
    }

    auto const bytes = read_back(ctx, tex);
    REQUIRE(bytes.has_value());
    CHECK(matches(bytes.value(), 67));

    ctx->advance_epoch_and_wait_for_idle();
}

INVOCABLE_TEST("sg - two concurrent lists, a submit, an async download, and the second submit",
               (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    auto const tex = make_texture(ctx);

    // The scenario the entry-barrier model exists for, end to end.
    //
    // Both lists open before either records, so both assume whatever the texture is in now.
    // The first submits; the async download's fixup then moves the layout out from under the second, which submits
    // afterwards with barriers recorded against a layout that is gone.
    // The old model reverted at submit and could not see the fixup at all, so the second list named a layout the
    // texture had left — a debug-layer message rather than wrong bytes, which is why the listener is the oracle here.
    auto writer = ctx->create_command_list();
    auto reader = ctx->create_command_list();
    REQUIRE(writer != nullptr);
    REQUIRE(reader != nullptr);

    writer->upload.bytes_to_texture(tex, cc::span<byte const>(pattern(83)));
    auto future = reader->download.bytes_from_texture(tex);

    ctx->submit_command_list(cc::move(writer));
    REQUIRE(ctx->wait_for(ctx->download.bytes_from_texture(tex)).has_value());
    ctx->submit_command_list(cc::move(reader));

    auto const bytes = ctx->wait_for(future);
    REQUIRE(bytes.has_value());
    CHECK(matches(bytes.value(), 83));

    ctx->advance_epoch_and_wait_for_idle();
}

INVOCABLE_TEST("sg - a list recorded before an async transfer, and submitted after it", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    auto const tex = make_texture(ctx);

    {
        auto cmd = ctx->create_command_list();
        REQUIRE(cmd != nullptr);
        cmd->upload.bytes_to_texture(tex, cc::span<byte const>(pattern(97)));
        ctx->submit_command_list(cc::move(cmd));
    }

    // A list whose entry requirement was recorded before a transfer existed, submitted after that transfer was
    // enqueued — with nothing awaited in between, unlike every case above.
    //
    // `has_pending_transfer` cannot see this one: it is read while recording, and no transfer was pending then.
    // So the list asks for `shader_readonly`, the upload's fixup settles the texture at the async-ready layout, and
    // the list's entry barrier moves it off that layout at a submit the transfer's copy does not wait for.
    // The copy names the async-ready layout and carries no image barrier of its own.
    //
    // That composes correctly today, and this pins it — the ordering it rests on is the transfer's own, and nothing
    // in the layout bookkeeping is what makes it hold.
    auto cmd = ctx->create_command_list();
    REQUIRE(cmd != nullptr);
    cmd->ensure_layout(tex, sg::texture_layout::shader_readonly);

    ctx->upload.bytes_to_texture(tex, pinned_pattern(101));

    ctx->submit_command_list(cc::move(cmd));

    // The bytes are the transfer's: the list declared a layout and wrote nothing.
    auto const bytes = read_back(ctx, tex);
    REQUIRE(bytes.has_value());
    CHECK(matches(bytes.value(), 101)).context("the async upload did not land");

    ctx->advance_epoch_and_wait_for_idle();
}
