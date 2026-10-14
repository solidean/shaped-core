#include "vulkan-test-common.hh"

#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>

// A rendering scope's attachment view lives on its texture for one epoch: every list of that epoch shares it, and the
// epoch destroys it when it retires — whichever list created it, and whether that list was submitted or dropped.
// The parallel-recording side of this is tests/command_list/parallel-rendering-test.cc in shaped-graphics-test.

using namespace cc::primitive_defines;

namespace
{
namespace vulkan = sg::backend::vulkan;

constexpr int k_extent = 16;

[[nodiscard]] vulkan::vulkan_command_list& as_vulkan(sg::command_list& cmd)
{
    return static_cast<vulkan::vulkan_command_list&>(cmd);
}

/// Opens a scope clearing `target`, and returns the view its color attachment names.
[[nodiscard]] VkImageView render_into(sg::command_list& cmd, sg::texture_2d const& target)
{
    auto pass = cmd.raster.render_to({.color_targets = {target.as_render_target_view().cleared(tg::vec4f(1, 0, 0, 1))}});
    return as_vulkan(cmd)._rendering_color_attachments[0].imageView;
}
} // namespace

ASYNC_INVOCABLE_TEST("sg vulkan - an attachment view is shared within its epoch and outlives the list that made it",
                     (vulkan::vulkan_context_handle const& handle))
{
    auto& ctx = *handle;
    auto const target = ctx.persistent.create_texture_2d({.format = sg::pixel_format::rgba8_unorm,
                                                          .width = k_extent,
                                                          .height = k_extent,
                                                          .usage = sg::texture_usage::render_target});

    auto creator = ctx.create_command_list();
    auto sharer = ctx.create_command_list();
    REQUIRE(creator != nullptr);
    REQUIRE(sharer != nullptr);

    auto const created = render_into(*creator, target);
    auto const shared = render_into(*sharer, target);
    CHECK(created == shared);
    CHECK(as_vulkan(*creator)._created_attachment_views.size() == 1);
    CHECK(as_vulkan(*sharer)._created_attachment_views.empty());

    // Rendering again in one list reuses the view as well, rather than creating one per scope.
    CHECK(render_into(*sharer, target) == shared);
    CHECK(as_vulkan(*sharer)._created_attachment_views.empty());

    // The creator is dropped, and a list recorded after that still attaches the view it made.
    // The validation layer checks a view's handle when the scope records it, so a view the drop destroyed fails here
    // through the log rule — a list that only recorded before the drop would pass either way.
    ctx.drop_command_list(cc::move(creator));
    auto late = ctx.create_command_list();
    REQUIRE(late != nullptr);
    CHECK(render_into(*late, target) == created);
    CHECK(as_vulkan(*late)._created_attachment_views.empty());
    (void)ctx.submit_command_list(cc::move(sharer));
    (void)ctx.submit_command_list(cc::move(late));
    ctx.advance_epoch();

    // A new epoch does not reuse the old epoch's view, which is destroyed once that epoch retires.
    auto next = ctx.create_command_list();
    REQUIRE(next != nullptr);
    (void)render_into(*next, target);
    CHECK(as_vulkan(*next)._created_attachment_views.size() == 1);
    (void)ctx.submit_command_list(cc::move(next));

    ctx.advance_epoch();
    co_await ctx.idle_completion();
    CHECK(!ctx.is_device_lost());
}
