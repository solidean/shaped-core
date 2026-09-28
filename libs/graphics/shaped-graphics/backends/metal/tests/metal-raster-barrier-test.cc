#include "mesh-fixture.hh"
#include "metal-test-common.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/backends/metal/metal_command_list.hh>
#include <shaped-graphics/binding/compiled_shader.hh>

// What the hazard tracking decides, and what it costs.
//
// **A barrier is invisible from outside**: one that was not needed is still correct, and one that was needed and got
// clamped away is still silent — so neither half of this can be tested through sg's public surface.
// `metal_command_list::barriers_emitted` is the seam that makes the first observable, and a read-back of what one draw
// wrote and the next read is what makes the second.
//
// libs/graphics/shaped-graphics/docs/concepts/barriers.md is the contract both halves are read against: a bind emits
// nothing, reads do not order against each other, and sg orders the accesses a caller never names.

namespace mtl = sg::backend::metal;
using namespace cc::primitive_defines;

namespace
{
constexpr auto k_size = 4;
constexpr auto k_copy_both = sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst;

/// The pixel count of the hazard fixture's target, which is also the length of the buffer one draw writes.
constexpr auto k_hazard_count = k_size * k_size;

[[nodiscard]] auto make_color_target(mtl::metal_context_handle const& ctx)
{
    return ctx->persistent.create_texture_2d({
        .format = sg::pixel_format::rgba8_unorm,
        .width = k_size,
        .height = k_size,
        .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src,
    });
}

/// A group layout holding one binding of `type` named `name`, at index 0.
[[nodiscard]] cc::result<sg::binding_group_layout_handle> one_binding_layout(mtl::metal_context_handle const& ctx,
                                                                             cc::string_view name,
                                                                             sg::binding_type type)
{
    auto bindings = cc::vector<sg::binding>();
    bindings.push_back({.name = cc::string(name), .space = 0, .index = 0, .count = 1, .type = type});

    auto layout = ctx->create_metal_binding_group_layout(bindings, {}, sg::lifetime_scope::persistent);
    if (layout.has_error())
        return cc::error(layout.error().to_string());
    return sg::binding_group_layout_handle(layout.value());
}

/// The raster pipeline of the two hazard draws: no vertex input, one group, and `entry` as the fragment stage.
[[nodiscard]] cc::result<mtl::metal_raster_pipeline_handle> make_hazard_pipeline(mtl::metal_context_handle const& ctx,
                                                                                 sg::pipeline_layout_handle const& layout,
                                                                                 cc::string entry)
{
    auto desc = sg::raster_pipeline_description{
        .layout = layout,
        .vertex_shader = mtl::test::mesh_shader(sg::shader_stage::vertex, "hazard_vertex_main"),
        .fragment_shader = mtl::test::mesh_shader(sg::shader_stage::fragment, cc::move(entry)),
        .rasterization = {.cull = sg::cull_mode::none},
    };
    desc.color_targets.push_back({.format = sg::pixel_format::rgba8_unorm});

    return ctx->create_metal_raster_pipeline(desc, sg::lifetime_scope::persistent);
}
} // namespace

TEST("sg metal - a draw loop over a readonly group emits no barrier")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    // Every draw declares what the bound groups name, so a group whose binding is readonly must add nothing a later
    // draw has to wait on.
    // Declaring `shader_read | shader_write` for every binding instead made each draw meet the previous draw's
    // unordered write — one barrier per draw, for a loop that never writes anything at all.
    auto group_layout = one_binding_layout(ctx, "palette", sg::binding_type::readonly_structured_buffer);
    REQUIRE(group_layout.has_value());

    auto layout_desc = sg::pipeline_layout_description{};
    layout_desc.groups.push_back(group_layout.value());
    auto pipeline_layout = ctx->create_metal_pipeline_layout(layout_desc, sg::lifetime_scope::persistent);
    REQUIRE(pipeline_layout.has_value());

    auto pipeline = make_hazard_pipeline(ctx, sg::pipeline_layout_handle(pipeline_layout.value()), "hazard_read_main");
    REQUIRE(pipeline.has_value()).context(pipeline.has_error() ? pipeline.error().to_string() : cc::string());

    auto const palette
        = ctx->persistent.create_raw_buffer(k_hazard_count * isize(sizeof(u32)), sg::buffer_usage::readonly_buffer);
    auto const nv = sg::named_view{
        .name = "palette",
        .view = palette->as_raw_readonly({.offset = 0, .size = palette->size_in_bytes()}, isize(sizeof(u32)))};
    auto group = ctx->create_metal_binding_group(group_layout.value(), cc::span<sg::named_view const>(&nv, 1), {},
                                                 sg::lifetime_scope::persistent);
    REQUIRE(group.has_value());

    auto const target = make_color_target(ctx);

    auto cmd = ctx->create_command_list();
    auto& mtl_cmd = static_cast<mtl::metal_command_list&>(*cmd);

    auto info = sg::rendering_info{};
    info.color_targets.push_back(target.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1)));

    auto emitted = i64(0);
    {
        auto scope = cmd->raster.render_to(info);
        scope.bind_pipeline(*pipeline.value());
        scope.bind_group(0, *group.value());

        // Sampled after the scope is set up, so the target's own transition is not what this counts.
        auto const before = mtl_cmd.barriers_emitted();
        for (auto i = 0; i < 64; ++i)
            scope.draw({.vertex_range = {.offset = 0, .size = 3}});
        emitted = mtl_cmd.barriers_emitted() - before;
    }

    ctx->submit_command_list(cc::move(cmd));

    CHECK(emitted == 0).context(cc::format("64 read-only draws emitted {} barrier(s)", emitted));
}

ASYNC_TEST("sg metal - two dispatches reading one buffer emit no barrier between them")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    // The compute twin of the draw loop, and the path that had the bug first: `declare_bound_groups` is shared.
    // One `source` read by both dispatches, and a `target` of its own for each, so the only thing either could order
    // against is the shared read.
    auto shader = mtl::test::mesh_kernel("copy_main");
    shader.bindings.push_back(
        {.name = "source", .space = 0, .index = 0, .count = 1, .type = sg::binding_type::readonly_structured_buffer});
    shader.bindings.push_back(
        {.name = "target", .space = 0, .index = 1, .count = 1, .type = sg::binding_type::readwrite_structured_buffer});

    auto group_layout = ctx->create_metal_binding_group_layout(shader.bindings, {}, sg::lifetime_scope::persistent);
    REQUIRE(group_layout.has_value());

    auto layout_desc = sg::pipeline_layout_description{};
    layout_desc.groups.push_back(group_layout.value());
    auto pipeline_layout = ctx->create_metal_pipeline_layout(layout_desc, sg::lifetime_scope::persistent);
    REQUIRE(pipeline_layout.has_value());

    auto pipeline = ctx->create_metal_compute_pipeline({.shader = shader, .layout = pipeline_layout.value()},
                                                       sg::lifetime_scope::persistent);
    REQUIRE(pipeline.has_value()).context(pipeline.has_error() ? pipeline.error().to_string() : cc::string());

    constexpr auto k_count = 8;
    auto const source = ctx->persistent.create_raw_buffer(k_count * isize(sizeof(u32)),
                                                          k_copy_both | sg::buffer_usage::readonly_buffer);

    auto make_group = [&](sg::raw_buffer_handle const& target)
    {
        auto views = cc::vector<sg::named_view>();
        views.push_back(
            {.name = "source",
             .view = source->as_raw_readonly({.offset = 0, .size = source->size_in_bytes()}, isize(sizeof(u32)))});
        views.push_back(
            {.name = "target",
             .view = target->as_raw_readwrite({.offset = 0, .size = target->size_in_bytes()}, isize(sizeof(u32)))});
        return ctx->create_metal_binding_group(group_layout.value(), views, {}, sg::lifetime_scope::persistent);
    };

    auto const first_target = ctx->persistent.create_raw_buffer(k_count * isize(sizeof(u32)),
                                                                k_copy_both | sg::buffer_usage::readwrite_buffer);
    auto const second_target = ctx->persistent.create_raw_buffer(k_count * isize(sizeof(u32)),
                                                                 k_copy_both | sg::buffer_usage::readwrite_buffer);

    auto first_group = make_group(first_target);
    REQUIRE(first_group.has_value());
    auto second_group = make_group(second_target);
    REQUIRE(second_group.has_value());

    auto values = cc::vector<u32>::create_uninitialized(k_count);
    for (auto i = 0; i < k_count; ++i)
        values[i] = u32(i + 1);

    auto cmd = ctx->create_command_list();
    auto& mtl_cmd = static_cast<mtl::metal_command_list&>(*cmd);

    cmd->upload.bytes_to_buffer(source, cc::as_bytes(cc::span<u32 const>(values)));
    cmd->compute.bind_pipeline(*pipeline.value());
    cmd->compute.bind_group(0, *first_group.value());

    // The first dispatch reads what the upload wrote and does have to wait; the second is the one under test.
    cmd->compute.dispatch_groups(k_count, 1, 1);

    auto const before = mtl_cmd.barriers_emitted();
    cmd->compute.bind_group(0, *second_group.value());
    cmd->compute.dispatch_groups(k_count, 1, 1);
    auto const emitted = mtl_cmd.barriers_emitted() - before;

    auto future = cmd->download.bytes_from_buffer(second_target, 0, second_target->size_in_bytes());
    ctx->submit_command_list(cc::move(cmd));

    co_await ctx->idle_completion();

    CHECK(emitted == 0).context(cc::format("the second dispatch emitted {} barrier(s)", emitted));

    // And it still ran: a barrier count is only worth something beside a result that says the work happened.
    auto const bytes = future.try_get_bytes();
    REQUIRE(bytes.has_value());
    auto const read_back = cc::span<u32 const>(reinterpret_cast<u32 const*>(bytes.value().data()), k_count);
    auto mismatches = 0;
    for (auto i = 0; i < k_count; ++i)
        if (read_back[i] != values[i])
            ++mismatches;
    CHECK(mismatches == 0);
}

ASYNC_TEST("sg metal - a draw sees what the previous draw's fragment shader wrote")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    // **The one hazard a render encoder's barrier cannot express.** `barrierAfterEncoderStages` refuses
    // `MTLStageFragment` as its source, so "after A's fragment stage" is not a barrier that can be emitted inside the
    // pass — and one clamped down to the vertex stage leaves B free to read while A is still writing.
    // The backend answers by closing the pass and opening it again, which is what dx12 gets from a UAV barrier and
    // vulkan from ending and reopening its own pass.
    //
    // **It may pass by timing rather than by ordering**: a 4×4 draw is short enough to finish before the next one
    // starts on an M-series GPU, so a failure here is real and a pass is not proof on its own.
    auto group_layout = one_binding_layout(ctx, "results", sg::binding_type::readwrite_structured_buffer);
    REQUIRE(group_layout.has_value());

    auto layout_desc = sg::pipeline_layout_description{};
    layout_desc.groups.push_back(group_layout.value());
    auto pipeline_layout = ctx->create_metal_pipeline_layout(layout_desc, sg::lifetime_scope::persistent);
    REQUIRE(pipeline_layout.has_value());

    auto const layout = sg::pipeline_layout_handle(pipeline_layout.value());
    auto writer = make_hazard_pipeline(ctx, layout, "hazard_write_main");
    REQUIRE(writer.has_value()).context(writer.has_error() ? writer.error().to_string() : cc::string());
    auto reader = make_hazard_pipeline(ctx, layout, "hazard_read_main");
    REQUIRE(reader.has_value()).context(reader.has_error() ? reader.error().to_string() : cc::string());

    auto const results = ctx->persistent.create_raw_buffer(k_hazard_count * isize(sizeof(u32)),
                                                           k_copy_both | sg::buffer_usage::readwrite_buffer);
    auto const nv = sg::named_view{
        .name = "results",
        .view = results->as_raw_readwrite({.offset = 0, .size = results->size_in_bytes()}, isize(sizeof(u32)))};
    auto group = ctx->create_metal_binding_group(group_layout.value(), cc::span<sg::named_view const>(&nv, 1), {},
                                                 sg::lifetime_scope::persistent);
    REQUIRE(group.has_value());

    auto const target = make_color_target(ctx);

    auto cmd = ctx->create_command_list();
    auto& mtl_cmd = static_cast<mtl::metal_command_list&>(*cmd);

    // Zeroed first, so a read that raced ahead of the write reads 0 rather than whatever the allocation held.
    auto const zeros = cc::vector<u32>::create_filled(k_hazard_count, u32(0));
    cmd->upload.bytes_to_buffer(results, cc::as_bytes(cc::span<u32 const>(zeros)));

    {
        auto info = sg::rendering_info{};
        info.color_targets.push_back(target.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1)));
        auto scope = cmd->raster.render_to(info);

        scope.bind_pipeline(*writer.value());
        scope.bind_group(0, *group.value());
        scope.draw({.vertex_range = {.offset = 0, .size = 3}});

        scope.bind_pipeline(*reader.value());
        scope.bind_group(0, *group.value());
        scope.draw({.vertex_range = {.offset = 0, .size = 3}});
    }

    // **Checked before the pixels are**, because the pixels can come out right by timing alone.
    // This is the mechanism itself: one reopen, between the two draws.
    auto const reopens = mtl_cmd.pass_reopens();
    CHECK(reopens == 1).context(cc::format("the pass was reopened {} time(s), expected once", reopens));

    auto future = cmd->download.bytes_from_texture(target.raw());
    ctx->submit_command_list(cc::move(cmd));

    co_await ctx->idle_completion();

    auto const bytes = future.try_get_bytes();
    REQUIRE(bytes.has_value());
    auto const pixels = bytes.value();

    // `hazard_write_main` writes `index + 1` per pixel and `hazard_read_main` scales it by 1/255, so texel i reads
    // back as i + 1 — and a texel the second draw found unwritten reads back as 0.
    auto wrong = 0;
    auto first_wrong = -1;
    for (auto i = 0; i < k_hazard_count; ++i)
    {
        auto const value = int(u8(pixels[i * 4]));
        auto const delta = value - (i + 1);
        if ((delta < 0 ? -delta : delta) > 1)
        {
            ++wrong;
            if (first_wrong < 0)
                first_wrong = i;
        }
    }

    CHECK(wrong == 0)
        .context(cc::format("{} of {} texels wrong; texel {} read back as {}, expected {}", wrong, k_hazard_count,
                            first_wrong, first_wrong >= 0 ? int(u8(pixels[first_wrong * 4])) : 0, first_wrong + 1));
}

ASYNC_TEST("sg metal - a reopened pass keeps its contents and its encoder state")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    // Reopening a pass is a real encoder boundary, so everything an encoder holds is lost across it: the attachments'
    // contents, the viewport and scissor, the pipeline and raster state.
    // The attachments reload instead of honouring their `target_op` again — a second clear would wipe the draws the
    // reopen exists to order — and the scope's state is replayed.
    //
    // Each half is a colour here: the left was drawn before the reopen and must survive it, and the right is drawn
    // after and says the scissor came back.
    auto group_layout = one_binding_layout(ctx, "results", sg::binding_type::readwrite_structured_buffer);
    REQUIRE(group_layout.has_value());

    auto layout_desc = sg::pipeline_layout_description{};
    layout_desc.groups.push_back(group_layout.value());
    auto pipeline_layout = ctx->create_metal_pipeline_layout(layout_desc, sg::lifetime_scope::persistent);
    REQUIRE(pipeline_layout.has_value());

    auto const layout = sg::pipeline_layout_handle(pipeline_layout.value());
    auto writer = make_hazard_pipeline(ctx, layout, "hazard_write_main");
    REQUIRE(writer.has_value());
    auto reader = make_hazard_pipeline(ctx, layout, "hazard_read_main");
    REQUIRE(reader.has_value());

    auto const results = ctx->persistent.create_raw_buffer(k_hazard_count * isize(sizeof(u32)),
                                                           k_copy_both | sg::buffer_usage::readwrite_buffer);
    auto const nv = sg::named_view{
        .name = "results",
        .view = results->as_raw_readwrite({.offset = 0, .size = results->size_in_bytes()}, isize(sizeof(u32)))};
    auto group = ctx->create_metal_binding_group(group_layout.value(), cc::span<sg::named_view const>(&nv, 1), {},
                                                 sg::lifetime_scope::persistent);
    REQUIRE(group.has_value());

    auto const target = make_color_target(ctx);

    auto cmd = ctx->create_command_list();
    auto& mtl_cmd = static_cast<mtl::metal_command_list&>(*cmd);

    auto const zeros = cc::vector<u32>::create_filled(k_hazard_count, u32(0));
    cmd->upload.bytes_to_buffer(results, cc::as_bytes(cc::span<u32 const>(zeros)));

    constexpr auto k_half = k_size / 2;
    {
        auto info = sg::rendering_info{};
        info.color_targets.push_back(target.as_render_target_view().cleared(tg::vec4f(1, 0, 0, 1)));
        auto scope = cmd->raster.render_to(info);

        // The left half only: green, and `results` written for those pixels alone.
        scope.set_scissor(tg::aabb2i(tg::pos2i(0, 0), tg::pos2i(k_half, k_size)));
        scope.bind_pipeline(*writer.value());
        scope.bind_group(0, *group.value());
        scope.draw({.vertex_range = {.offset = 0, .size = 3}});

        // The right half, where `results` is still zero — so this draw lands black wherever it reaches.
        // The scissor is set before the draw that reopens the pass, which is what makes it the replayed state.
        scope.set_scissor(tg::aabb2i(tg::pos2i(k_half, 0), tg::pos2i(k_size, k_size)));
        scope.bind_pipeline(*reader.value());
        scope.bind_group(0, *group.value());
        scope.draw({.vertex_range = {.offset = 0, .size = 3}});
    }

    auto const reopens = mtl_cmd.pass_reopens();
    CHECK(reopens == 1).context(cc::format("the pass was reopened {} time(s), expected once", reopens));

    auto future = cmd->download.bytes_from_texture(target.raw());
    ctx->submit_command_list(cc::move(cmd));

    co_await ctx->idle_completion();

    auto const bytes = future.try_get_bytes();
    REQUIRE(bytes.has_value());
    auto const pixels = bytes.value();

    // Green on the left: a reopen that recleared would show the red clear colour instead.
    // Black on the right: a reopen that lost the scissor would let the second draw cover the left half too.
    auto wrong_left = 0;
    auto wrong_right = 0;
    for (auto y = 0; y < k_size; ++y)
        for (auto x = 0; x < k_size; ++x)
        {
            auto const* const texel = &pixels[(y * k_size + x) * 4];
            auto const is_green = int(u8(texel[0])) < 8 && int(u8(texel[1])) > 247;
            auto const is_black = int(u8(texel[0])) < 8 && int(u8(texel[1])) < 8;
            if (x < k_half && !is_green)
                ++wrong_left;
            if (x >= k_half && !is_black)
                ++wrong_right;
        }

    CHECK(wrong_left == 0).context(cc::format("{} left-half texels lost the pre-reopen draw", wrong_left));
    CHECK(wrong_right == 0)
        .context(cc::format("{} right-half texels were not what the replayed scissor allows", wrong_right));
}
