#include "raster_test_support.hh"
#include "viewer_test_env.hh"

#include <clean-core/record/log.hh> // CC_LOG_INFO
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/dlss_rr_routine.hh>
#include <shaped-viewer/all.hh>
#include <typed-geometry/scalar/scalar.hh> // tg::abs

// The hit distance a deferred hybrid frame hands DLSS Ray Reconstruction, which reprojects reflections at their own depth by it.
//
// The camera stands at the center of a mirror sphere seen from inside.
// Every primary ray meets the sphere head-on and reflects straight back through the center, so every pixel's specular hit
// distance is the diameter, and no pixel draws a diffuse direction.

namespace
{
using namespace cc::primitive_defines;

/// A unit sphere wound inward, so every face fronts the center the camera stands at.
[[nodiscard]] sv::triangle_geometry inward_sphere(int rings, int segments)
{
    auto positions = cc::vector<tg::pos3f>();
    for (auto r = 0; r <= rings; ++r)
    {
        auto const theta = tg::angle_f::make_from_radians(f32(r) / f32(rings) * tg::pi<f32>);
        for (auto s = 0; s <= segments; ++s)
        {
            auto const phi = tg::angle_f::make_from_radians(f32(s) / f32(segments) * 2.0f * tg::pi<f32>);
            positions.push_back(tg::pos3f(tg::sin(theta) * tg::cos(phi), tg::cos(theta), tg::sin(theta) * tg::sin(phi)));
        }
    }
    auto indices = cc::vector<u32>();
    auto const index_of = [segments](int r, int s) { return u32(r * (segments + 1) + s); };
    for (auto r = 0; r < rings; ++r)
        for (auto s = 0; s < segments; ++s)
            for (auto const i : {index_of(r, s), index_of(r + 1, s), index_of(r + 1, s + 1), index_of(r, s),
                                 index_of(r + 1, s + 1), index_of(r, s + 1)})
                indices.push_back(i);
    return sv::triangle_geometry::create_from_indexed_triangles(positions, indices);
}
} // namespace

ASYNC_INVOCABLE_TEST("sv - a deferred hybrid frame hands dlss_rr each pixel's specular hit distance",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (!sv_test::can_run(ctx) || !ctx.supports(sg::feature::ray_query))
        SKIP("no inline ray tracing or no SGL compiler, so the hybrid does not build");
    if (!sr::dlss_rr_routine::is_available(ctx))
        SKIP("no DLSS Ray Reconstruction here: the SDK is fetched on request, and it needs an RTX adapter on dx12");

    auto& lib = sv_test::shared_material_library();
    auto const openpbr = lib.acquire_type(sv::builtin_material::openpbr).value();
    auto resources = sv::gpu_resource_manager::create(ctx);
    using binding = sv::material_attribute_binding;
    auto const item = resources.acquire_scene_item(
        sv::mesh{.name = "mirror sphere",
                 .geometry = inward_sphere(32, 48),
                 .material = lib.acquire(sv::material::create(
                     "mirror sphere", openpbr,
                     {binding::of("base_color", tg::vec3f(0.9f, 0.9f, 0.9f)), binding::of("base_metalness", 1.0f),
                      binding::of("specular_roughness", 0.0f)}))});
    resources.wait_for_pending_uploads();

    constexpr auto size = tg::vec2i(48, 48);
    auto const output
        = ctx.persistent.create_texture_2d({.format = sg::pixel_format::bgra8_unorm,
                                            .width = size[0],
                                            .height = size[1],
                                            .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src});

    auto def = sv::viewer_definition{};
    {
        auto v = sv::view_data{};
        v.id = sv::view_id::from_string("inside");
        v.camera = sv::camera::looking_at(tg::pos3d(0, 0, 0), tg::pos3d(0, 0, 1));
        v.camera.projection.aspect_ratio = 1.0;
        v.camera.projection.near_plane = 0.01;
        auto& scene = sv::ensure_scene_3d(v);
        scene.items.push_back(item);
        scene.background = sv::background::uniform(tg::vec3f(0.5f, 0.5f, 0.5f));
        scene.settings.mode = sv::render_mode::hybrid;
        scene.settings.hybrid_lighting = sv::hybrid_lighting::deferred;
        scene.settings.reconstruct.denoiser = sr::denoise_method::dlss_rr;
        // Every frame its own temporal denoise, so every frame writes the distances rather than the first few after a restart.
        scene.settings.temporal_denoise_frames = u32(-1);
        def.views.push_back(cc::move(v));

        auto const root_node = def.nodes.add_container(sv::invalid_node);
        auto leaf = sv::layout_leaf{};
        leaf.views.push_back(sv::view_index(0));
        def.nodes.add_leaf(root_node, cc::move(leaf));
        auto root = sv::view_data{};
        root.id = sv::view_id::from_string("root");
        root.layers.push_back({.kind = sv::layer_kind::layout, .blend = sv::layer_blend::replace, .root_node = root_node});
        def.root_view = sv::view_index(def.views.size());
        def.views.push_back(cc::move(root));
    }
    auto store = sv::view_store{};
    auto const history = sv::view_history{};
    auto const id = def.views[0].id;

    // Frames until one has written the distances, as the slot's mark says: the guide slots arrive a frame after their declaration.
    // And past the stand-ins: a frame shaded through the fallback while the mirror compiles bounces diffusely, and restarts the mean.
    auto guide = cc::vector<tg::vec2f>();
    auto index = u64(0);
    auto const start = cc::current_time_steady_secs();
    while (guide.empty())
    {
        (void)ctx.routines.tick();
        auto cmd = ctx.create_command_list();
        resources.advance_to(ctx.current_epoch());
        store.begin_frame(u64(ctx.current_epoch()));
        auto const plan = sv::build_render_plan(def, size, index++, history);
        (void)sv::viewer_renderer::execute(*cmd, def, plan, resources, store,
                                           output.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1)));
        auto const* const state = store.peek_ptr(id);
        auto const* const slot
            = state != nullptr ? state->temporal.get_ptr(sv::temporal_id::hit_distance_guide(0)) : nullptr;
        auto read = sg::data_future<tg::vec2f>();
        if (slot != nullptr && slot->texture.raw() != nullptr && slot->accum_frame != 0
            && store.min_accumulated_samples(id) >= 64)
            read = sg::data_future<tg::vec2f>(cmd->download.bytes_from_texture(slot->texture.raw()));
        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();
        co_await ctx.idle_completion();
        sv_test::drive_ambient_work();
        REQUIRE(cc::current_time_steady_secs() - start < 60.0)
            .dump("slot", slot != nullptr)
            .dump("texture", slot != nullptr && slot->texture.raw() != nullptr)
            .dump("marked", slot != nullptr ? int(slot->accum_frame) : -1)
            .dump("samples", store.min_accumulated_samples(id));
        if (!read.is_valid())
            continue;
        for (auto const& d : co_await read.data())
            guide.push_back(d);
    }

    auto on_diameter = 0;
    auto worst = 0.0f;
    auto diffuse = 0.0f;
    for (auto const& d : guide)
    {
        worst = cc::max(worst, tg::abs(d[1] - 2.0f));
        diffuse = cc::max(diffuse, d[0]);
        if (tg::abs(d[1] - 2.0f) < 0.02f)
            ++on_diameter;
    }
    CC_LOG_INFO("a mirror sphere from its center: {} of {} pixels' specular hit distance within 1% of its diameter, "
                "the worst off by {}",
                on_diameter, guide.size(), worst);
    // The faceted sphere's flat triangles turn a reflection a little off the center, which the tolerance allows.
    CHECK(on_diameter > isize(guide.size()) * 9 / 10).dump("on the diameter", on_diameter).dump("worst", worst);
    CHECK(diffuse == 0.0f);
    co_await cc::async_settled(sv::background_work(ctx));
}
