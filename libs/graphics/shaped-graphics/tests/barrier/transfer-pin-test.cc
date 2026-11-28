#include <nexus/test.hh>
#include <shaped-graphics/barrier/transfer_pin.hh>

// What a texture's transfers in flight hold in their layout: only the subresources they cover, only for an access
// that layout serves, and nothing once none is pending.

namespace
{
sg::subresource_range mips(int start, int end, int layers = 1)
{
    return sg::subresource_range({.start = start, .end = end}, {.start = 0, .end = layers}, {.start = 0, .end = 1});
}
} // namespace

TEST("sg - a transfer pins the subresources it covers, for sampling and copies alone")
{
    auto pin = sg::impl::transfer_pin();
    pin.note(mips(0, 1), false);

    // the uploaded level, sampled or copied while the upload is in flight, stays in the transfer's layout
    CHECK(pin.layout_for(mips(0, 1), sg::texture_layout::shader_texture, true) == sg::texture_layout::general);
    CHECK(pin.layout_for(mips(0, 1), sg::texture_layout::copy_src, true) == sg::texture_layout::general);
    CHECK(pin.layout_for(mips(0, 2), sg::texture_layout::shader_texture, true) == sg::texture_layout::general);

    // the next level is no part of it: a mip chain rendered from the uploaded level takes its own layout
    CHECK(pin.layout_for(mips(1, 2), sg::texture_layout::render_target, true) == sg::texture_layout::render_target);
    CHECK(pin.layout_for(mips(1, 2), sg::texture_layout::shader_texture, true) == sg::texture_layout::shader_texture);

    // a write the transfer's layout cannot take keeps its own even on the uploaded level
    CHECK(pin.layout_for(mips(0, 1), sg::texture_layout::render_target, true) == sg::texture_layout::render_target);
    CHECK(pin.layout_for(mips(0, 1), sg::texture_layout::shader_image, true) == sg::texture_layout::shader_image);
    CHECK(pin.layout_for(mips(0, 1), sg::texture_layout::depth_readwrite, true) == sg::texture_layout::depth_readwrite);

    // nothing in flight pins nothing
    CHECK(pin.layout_for(mips(0, 1), sg::texture_layout::shader_texture, false) == sg::texture_layout::shader_texture);
}

TEST("sg - transfers in flight together pin the box holding them, and a transfer after they all finished starts over")
{
    auto pin = sg::impl::transfer_pin();
    pin.note(mips(0, 1), false);
    pin.note(mips(2, 3), true); // the first still in flight
    CHECK(pin.layout_for(mips(1, 2), sg::texture_layout::shader_texture, true) == sg::texture_layout::general);

    pin.note(mips(3, 4), false); // the others finished first
    CHECK(pin.layout_for(mips(0, 1), sg::texture_layout::shader_texture, true) == sg::texture_layout::shader_texture);
    CHECK(pin.layout_for(mips(3, 4), sg::texture_layout::shader_texture, true) == sg::texture_layout::general);
}
