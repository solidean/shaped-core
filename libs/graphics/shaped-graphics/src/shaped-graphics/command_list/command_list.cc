#include <clean-core/common/assert.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/resource/raw_texture.hh>

namespace sg
{
command_list::~command_list() = default;

void command_list::ensure_layout(raw_texture_handle texture, texture_layout layout, cc::optional<subresource_range> range)
{
    CC_ASSERT(texture != nullptr, "ensure_layout: texture is null");
    CC_ASSERT(layout != texture_layout::undefined, "ensure_layout: undefined is not a layout to leave a texture in");
    transition_texture_layout(cc::move(texture), layout, range);
}

void command_list::prepare_for_async(raw_texture_handle texture,
                                     async_direction direction,
                                     cc::optional<subresource_range> range)
{
    CC_ASSERT(texture != nullptr, "prepare_for_async: texture is null");
    ensure_layout(cc::move(texture), context().async_ready_layout(direction), range);
}

command_list::command_list(sg::context& ctx, epoch created_in)
  : upload(*this),
    download(*this),
    copy(*this),
    compute(*this),
    raster(*this),
    raytracing(*this),
    query(*this),
    _epoch(created_in),
    _context(&ctx)
{
    // The scopes only store the back-reference; they don't touch any not-yet-constructed member.
}
} // namespace sg
