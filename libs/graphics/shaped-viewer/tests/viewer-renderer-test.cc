#include "viewer_test_env.hh"

#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#include <shaped-rendering/blit_routine.hh>              // the stand-in overlay pass
#include <shaped-viewer/all.hh>

using namespace cc::primitive_defines;

namespace
{
/// Wraps `def`'s existing views in a root view whose single layer is a layout tree, one leaf per view.
/// This is what sv::viewer does per frame, and what turns a flat list of views into something the plan can walk.
[[nodiscard]] sv::view_index add_layout_root(sv::viewer_definition& def,
                                             sv::box_style style = {},
                                             sv::grid_params grid = {})
{
    auto const root_node = def.nodes.add_container(sv::invalid_node, style, grid);
    for (auto i = u32(0); i < def.views.size(); ++i)
    {
        auto leaf = sv::layout_leaf{};
        leaf.views.push_back(sv::view_index(i));
        def.nodes.add_leaf(root_node, cc::move(leaf));
    }

    auto root = sv::view_data{};
    root.id = sv::view_id::from_string("root");
    root.layers.push_back({.kind = sv::layer_kind::layout, .blend = sv::layer_blend::replace, .root_node = root_node});

    def.root_view = sv::view_index(def.views.size());
    def.views.push_back(cc::move(root));
    return def.root_view;
}

/// Builds `def`'s plan for a first frame at `output_size` — no history, so everything refreshes.
[[nodiscard]] sv::render_plan plan_for(sv::viewer_definition const& def, tg::vec2i output_size)
{
    return sv::build_render_plan(def, output_size, 0, {});
}
} // namespace

// Headless whole-frame render: three views, each carrying its own cell of the output, driven through viewer_renderer in one call.
// What this adds over view-renderer-test is the multi-view path: three traces recorded before any pass opens, then one pass over which the viewport / scissor moves per view.
// The WARP debug layer validates the transitions from three UAV writes to three sampled reads for us.
//
// No pixel readback: reaching the end without an assert / exception / debug-layer error means the whole frame recorded and ran.
TEST("sv - viewer renderer places every view in its own rect (headless)")
{
    auto ctx_r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (ctx_r.has_error())
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context_handle const ctx_h = ctx_r.value();
    sg::context& ctx = *ctx_h;

    {
        auto probe = ctx.create_command_list();
        auto const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }

    auto const& env = sv_test::shared_env();
    if (!env.has_compiler)
        SKIP("no DXC compiler to build the shaders");

    auto const cloud = sv_test::make_triangle_cloud(32);
    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const mesh = resources.meshes.acquire(sv::triangle_data::create(cloud.positions));
    auto const materials = resources.materials.acquire(sv::material_data::create(cloud.materials));

    // An output split into a row of three cells; each view traces at its cell's size and knows where it sits.
    // The odd width is on purpose: the cells must not have to divide evenly.
    auto const output_size = tg::vec2i(129, 64);
    char const* const ids[] = {"left", "middle", "right"};

    auto def = sv::viewer_definition{};
    for (auto i = 0; i < 3; ++i)
    {
        auto const x0 = output_size[0] * i / 3;
        auto const x1 = output_size[0] * (i + 1) / 3;

        auto v = sv::view_data{};
        v.id = sv::view_id::from_string(ids[i]);
        v.resolution = tg::vec2i(x1 - x0, output_size[1]);
        v.camera = sv::camera::orbiting(tg::pos3d::zero, 4.0, tg::angle_d::make_from_degree(40.0 * i),
                                        tg::angle_d::make_from_degree(20.0));
        sv::ensure_scene_3d(v).items.push_back({.mesh = mesh, .materials = materials});
        def.views.push_back(cc::move(v));
    }
    (void)add_layout_root(def, {}, {.rows = 1});

    auto const plan = plan_for(def, output_size);
    REQUIRE(plan.validate());

    // Four textures: one per view, then the output.
    // Each view traces at its own cell rather than the output's size.
    REQUIRE(plan.targets.size() == 4);
    CHECK(plan.targets.back().is_output);
    CHECK(plan.traces.size() == 3);

    // The cells tile the output exactly, with no gap and no overlap — the property a layout must preserve.
    auto const draws = plan.draws_of(3);
    REQUIRE(draws.size() == 3);
    CHECK(draws.front().dst_rect.min == tg::pos2i(0, 0));
    CHECK(draws.back().dst_rect.max == tg::pos2i(output_size[0], output_size[1]));
    CHECK(draws[0].dst_rect.max[0] == draws[1].dst_rect.min[0]);
    CHECK(draws[1].dst_rect.max[0] == draws[2].dst_rect.min[0]);

    auto const output = ctx.persistent.create_texture_2d({.format = sg::pixel_format::bgra8_unorm,
                                                          .width = output_size[0],
                                                          .height = output_size[1],
                                                          .usage = sg::texture_usage::render_target});

    auto cmd = ctx.create_command_list();
    resources.advance_to(ctx.current_epoch()); // the frame's job, not a routine's
    auto store = sv::view_store{};             // and so is what its views keep across frames
    sv::viewer_renderer::execute(*cmd, def, plan, resources, store,
                                 output.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1)));
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch_and_wait_for_idle();

    // Every view resolved against the same two resources, so nothing was uploaded per view.
    CHECK(resources.meshes.count() == 1);
    CHECK(resources.materials.count() == 1);
}

// No views at all: the pass still opens, so the output's clear lands and the target is defined.
// This is the path an authored-nothing frame takes, and it must not be a silent skip that leaves stale contents.
TEST("sv - viewer renderer with no views still runs the clear (headless)")
{
    auto ctx_r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (ctx_r.has_error())
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context_handle const ctx_h = ctx_r.value();
    sg::context& ctx = *ctx_h;

    auto const& env = sv_test::shared_env();
    if (!env.has_compiler)
        SKIP("no DXC compiler to build the blit shader");

    auto resources = sv::gpu_resource_manager::create(ctx);

    auto const output = ctx.persistent.create_texture_2d(
        {.format = sg::pixel_format::bgra8_unorm, .width = 64, .height = 64, .usage = sg::texture_usage::render_target});

    auto cmd = ctx.create_command_list();
    resources.advance_to(ctx.current_epoch());
    auto store = sv::view_store{};
    sv::viewer_renderer::execute(*cmd, {}, {}, resources, store,
                                 output.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1)));
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch_and_wait_for_idle();

    CHECK(true); // the pass opened and closed with no draws, so the begin-op ran
}

// A GUI drawn over the frame is a *second* pass on the same target, not a share of viewer_renderer's.
// Every trace has to be recorded before any pass opens, so the frame's pass cannot be handed in from outside.
// `preserved()` is what keeps the rendered frame underneath; the overlay here is a plain blit standing in for imgui.
TEST("sv - an overlay pass draws over the rendered frame (headless)")
{
    auto ctx_r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (ctx_r.has_error())
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context_handle const ctx_h = ctx_r.value();
    sg::context& ctx = *ctx_h;

    {
        auto probe = ctx.create_command_list();
        auto const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }

    auto const& env = sv_test::shared_env();
    if (!env.has_compiler)
        SKIP("no DXC compiler to build the shaders");

    auto const cloud = sv_test::make_triangle_cloud(16);
    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const mesh = resources.meshes.acquire(sv::triangle_data::create(cloud.positions));
    auto const materials = resources.materials.acquire(sv::material_data::create(cloud.materials));

    auto const output_size = tg::vec2i(96, 48);

    auto def = sv::viewer_definition{};
    {
        auto v = sv::view_data{};
        v.id = sv::view_id::from_string("half");
        // Half the target, so the frame pass really does leave a narrowed viewport behind it.
        v.resolution = tg::vec2i(output_size[0] / 2, output_size[1]);
        v.camera = sv::camera{.position = tg::pos3d(0, 0, -3.5)};
        sv::ensure_scene_3d(v).items.push_back({.mesh = mesh, .materials = materials});
        def.views.push_back(cc::move(v));
    }
    (void)add_layout_root(def);

    auto const output = ctx.persistent.create_texture_2d({.format = sg::pixel_format::bgra8_unorm,
                                                          .width = output_size[0],
                                                          .height = output_size[1],
                                                          .usage = sg::texture_usage::render_target});
    auto const rt = output.as_render_target_view();

    // Something for the overlay to draw; a real one is imgui's draw data.
    auto const overlay = ctx.persistent.create_texture_2d(
        {.format = sg::pixel_format::rgba16_float,
         .width = 8,
         .height = 8,
         .usage = sg::texture_usage::readonly_texture | sg::texture_usage::readwrite_texture});

    auto cmd = ctx.create_command_list();
    resources.advance_to(ctx.current_epoch());
    auto store = sv::view_store{};
    sv::viewer_renderer::execute(*cmd, def, plan_for(def, output_size), resources, store,
                                 rt.cleared(tg::vec4f(0, 0, 0, 1)));

    {
        // The second pass keeps what the frame just wrote, and starts from a full-target viewport of its own.
        auto scope = cmd->raster.render_to({.color_targets = {rt.preserved()}});
        sr::blit_routine::execute(scope, overlay);
    }

    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch_and_wait_for_idle();

    CHECK(true); // frame pass + overlay pass recorded onto one command list without a device / barrier error
}

// Nesting, end to end: a view whose layer is a layout tree holding two further views, composited up to the output.
//
// This is the case the flat model could not express at all — the middle view renders into its own texture, and the
// output samples that rather than the leaves.
// Three view textures plus the output, and one dispatch group ahead of every pass, whatever the depth.
TEST("sv - viewer renderer composites a nested layout (headless)")
{
    auto ctx_r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (ctx_r.has_error())
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context_handle const ctx_h = ctx_r.value();
    sg::context& ctx = *ctx_h;

    {
        auto probe = ctx.create_command_list();
        auto const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }

    auto const& env = sv_test::shared_env();
    if (!env.has_compiler)
        SKIP("no DXC compiler to build the shaders");

    auto const cloud = sv_test::make_triangle_cloud(16);
    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const mesh = resources.meshes.acquire(sv::triangle_data::create(cloud.positions));
    auto const materials = resources.materials.acquire(sv::material_data::create(cloud.materials));

    auto const output_size = tg::vec2i(128, 64);

    auto def = sv::viewer_definition{};

    auto const traced = [&](char const* name)
    {
        auto v = sv::view_data{};
        v.id = sv::view_id::from_string(name);
        v.camera = sv::camera{.position = tg::pos3d(0, 0, -3.5)};
        sv::ensure_scene_3d(v).items.push_back({.mesh = mesh, .materials = materials});
        def.views.push_back(cc::move(v));
        return sv::view_index(def.views.size() - 1);
    };

    auto const left = traced("left");
    auto const right = traced("right");

    // The middle view: its own texture, filled by a layout tree over the two traced views.
    auto const inner_node = def.nodes.add_container(
        sv::invalid_node, {.border = 2, .border_color = tg::vec4f(1, 0, 0, 1), .spacing = 4}, {.rows = 1});
    for (auto const child : {left, right})
    {
        auto leaf = sv::layout_leaf{};
        leaf.views.push_back(child);
        def.nodes.add_leaf(inner_node, cc::move(leaf));
    }

    auto middle = sv::view_data{};
    middle.id = sv::view_id::from_string("middle");
    middle.layers.push_back({.kind = sv::layer_kind::layout, .blend = sv::layer_blend::replace, .root_node = inner_node});
    def.views.push_back(cc::move(middle));
    auto const middle_index = sv::view_index(def.views.size() - 1);

    // And the root, whose one leaf names the middle view.
    auto const root_node = def.nodes.add_container(sv::invalid_node);
    {
        auto leaf = sv::layout_leaf{};
        leaf.views.push_back(middle_index);
        def.nodes.add_leaf(root_node, cc::move(leaf));
    }

    auto root = sv::view_data{};
    root.id = sv::view_id::from_string("root");
    root.layers.push_back({.kind = sv::layer_kind::layout, .blend = sv::layer_blend::replace, .root_node = root_node});
    def.root_view = sv::view_index(def.views.size());
    def.views.push_back(cc::move(root));

    auto const plan = plan_for(def, output_size);
    REQUIRE(plan.validate());

    // left, right, middle, then the output — children before the target that samples them.
    REQUIRE(plan.targets.size() == 4);
    CHECK(plan.targets[2].id == sv::view_id::from_string("middle"));
    CHECK(plan.targets[3].is_output);
    CHECK(plan.traces.size() == 2);

    // The output's single draw samples the middle view's texture, not either leaf's.
    auto const output_draws = plan.draws_of(3);
    REQUIRE(output_draws.size() == 1);
    CHECK(output_draws[0].primary.kind == sv::draw_source_kind::target);
    CHECK(output_draws[0].primary.index == 2u);

    // The middle view's own pass carries the border bands its box style asked for, plus the two leaves.
    auto const middle_draws = plan.draws_of(2);
    CHECK(middle_draws.size() == 6); // four bands + two views

    auto const output = ctx.persistent.create_texture_2d({.format = sg::pixel_format::bgra8_unorm,
                                                          .width = output_size[0],
                                                          .height = output_size[1],
                                                          .usage = sg::texture_usage::render_target});

    auto cmd = ctx.create_command_list();
    resources.advance_to(ctx.current_epoch());
    auto store = sv::view_store{};
    store.begin_frame(u64(ctx.current_epoch()));
    sv::viewer_renderer::execute(*cmd, def, plan, resources, store,
                                 output.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1)));
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch_and_wait_for_idle();

    // Two traces and three passes on one command list, with the debug layer validating every transition from a UAV
    // write to a sampled read and from a render target to a sampled read.
    CHECK(resources.meshes.count() == 1);
}

// The context is a process-wide resource, so acquiring it twice must not build two devices.
//
// This is what lets viewers run in succession — or side by side — on one device, and it is why a provider needs no
// caching of its own.
// It deliberately clears the provider again on the way out; the context it caused to be cached is the
// same WARP one the rest of this file uses.
TEST("sv - the rendering context is created once and shared")
{
    auto builds = 0;
    sv::set_acquire_context(
        [&builds]
        {
            ++builds;
            return sg::create_dx12_context({.use_warp = true});
        });

    auto const first = sv::acquire_viewer_context();
    if (first.has_error())
    {
        sv::set_acquire_context({});
        SKIP("no Direct3D 12 device (hardware or WARP)");
    }

    auto const second = sv::acquire_viewer_context();
    REQUIRE(!second.has_error());

    // Asked once, answered forever: the second call never reached the provider.
    CHECK(builds == 1);
    CHECK(first.value().get() == second.value().get());

    // Clearing the hook does not un-cache the context — which is the documented behavior, not an oversight.
    sv::set_acquire_context({});
    auto const third = sv::acquire_viewer_context();
    REQUIRE(!third.has_error());
    CHECK(builds == 1);
    CHECK(third.value().get() == first.value().get());
}
