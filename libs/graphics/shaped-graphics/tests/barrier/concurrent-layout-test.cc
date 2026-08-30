#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>

using namespace cc::primitive_defines;

// What a texture's contents survive when two command lists are open over it at once.
//
// Backend-agnostic, because the rule is sg's rather than a backend's: a texture layout is data-preserving, and
// nothing sg does to keep layouts predictable across concurrently recorded lists may throw the contents away.
//
// The mechanism both backends use is the same, and is worth stating because these tests are written against it.
// Each open list keeps a private layout partition seeded from `canonical`, the between-lists state; a per-texture
// count tracks how many lists are open over it.
// The list whose submit drops that count to zero commits its partition as the new canonical.
// A list that submits while another is still open instead hands the texture back in the canonical layout, so the
// still-open list finds it as it left it.
//
// A texture therefore rests in a real layout from the start, never in `undefined`.
// Creation does leave it there, and the one-time transition out of it belongs to no list — see libs/graphics/shaped-graphics/docs/concepts/barriers.md.
// Handing a texture back "in the canonical layout" must never mean handing it back as `undefined`, which is both a
// discard of what the list just wrote and, on Vulkan, a barrier the spec forbids outright.

namespace
{
constexpr int k_extent = 8;
constexpr int k_pixels = k_extent * k_extent;

// bgra8/rgba8 both put a full-intensity blue somewhere; what matters is that every channel is non-zero, so a
// discarded texture (all zeroes) cannot be mistaken for a correctly written one.
constexpr u8 k_write_value = 0x40;
} // namespace

INVOCABLE_TEST("sg - a texture written while another list is open keeps its contents", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto target
        = ctx->persistent.create_texture_2d({.format = sg::pixel_format::rgba8_unorm,
                                             .width = k_extent,
                                             .height = k_extent,
                                             .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src});
    REQUIRE(target.raw() != nullptr);

    // The list that stays open.
    // It only has to *declare* the texture, so the count is 2 while the writer submits.
    // A readback declares it as a copy source and records no writes, which is the cheapest way to hold a slot.
    auto holder = ctx->create_command_list();
    REQUIRE(holder != nullptr);
    (void)holder->download.bytes_from_texture(target.raw());

    // The writer.
    // Its submit therefore takes the "another list is still open" path rather than promoting canonical.
    {
        auto writer = ctx->create_command_list();
        REQUIRE(writer != nullptr);
        {
            float const c = float(k_write_value) / 255.0f;
            auto pass = writer->raster.render_to(
                {.color_targets = {target.as_render_target_view().cleared(tg::vec4f(c, c, c, 1))}});
        }
        ctx->submit_command_list(cc::move(writer));
    }

    // The holder's own readback is not the subject and never runs.
    ctx->drop_command_list(cc::move(holder));

    auto reader = ctx->create_command_list();
    REQUIRE(reader != nullptr);
    auto future = reader->download.bytes_from_texture(target.raw());
    ctx->submit_command_list(cc::move(reader));

    auto const pixels = ctx->wait_for(future);
    REQUIRE(pixels.has_value());
    REQUIRE(pixels.value().size() == isize(k_pixels) * 4);

    // The whole point: a texture handed back to the canonical layout must still hold what the writer put in it.
    // If the revert targets `undefined`, every byte here is zero.
    int written = 0;
    int zeroed = 0;
    auto const* const p = reinterpret_cast<u8 const*>(pixels.value().data());
    for (int i = 0; i < k_pixels; ++i)
    {
        bool const all_zero = p[i * 4 + 0] == 0 && p[i * 4 + 1] == 0 && p[i * 4 + 2] == 0 && p[i * 4 + 3] == 0;
        if (all_zero)
            ++zeroed;
        else if (p[i * 4 + 3] == 255)
            ++written;
    }

    CHECK(zeroed == 0).context("the texture was discarded, not preserved, when the other list was still open");
    CHECK(written == k_pixels);

    ctx->advance_epoch_and_wait_for_idle();
}
