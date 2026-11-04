#include "raster_test_support.hh"
#include "viewer_test_env.hh"

#include <clean-core/record/log.hh> // CC_LOG_INFO
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/all.hh>
#include <typed-geometry/scalar/scalar.hh> // tg::abs

// The camera inside a closed matte sphere of albedo ρ that glows with radiance Le everywhere.
//
// Every surface sees the same thing in every direction, so the answer is a sum and nothing else.
// Light that bounced k times before reaching the camera adds Le ρ^k, so a renderer lighting n bounces past its
// primary hit sees Le (1 + ρ + ... + ρ^(n+1)): the emission, the direct light, and one term per bounce.
// Followed forever it is Le / (1 − ρ), which a radiance cache ending the bounces is to approach.
// So the test pins the bounce chain exactly: a bounce that loses energy, counts it twice, or weighs an emitter wrongly
// against the light sample moves the sum off its value at once.

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

/// Le (1 + ρ + ... + ρ^(bounces + 1)).
[[nodiscard]] f64 bounce_sum(f64 le, f64 rho, int bounces)
{
    auto sum = 0.0;
    auto term = le;
    for (auto k = 0; k <= bounces + 1; ++k)
    {
        sum += term;
        term *= rho;
    }
    return sum;
}
} // namespace

ASYNC_INVOCABLE_TEST("sv - inside a glowing sphere the hybrid sees the emission plus one term per bounce",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (!sv_test::can_run(ctx) || !ctx.supports(sg::feature::ray_query))
        SKIP("no inline ray tracing or no SGL compiler, so the hybrid does not build");

    auto& lib = sv_test::shared_material_library();
    auto const openpbr = lib.acquire_type(sv::builtin_material::openpbr).value();
    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const geometry = inward_sphere(32, 48);

    constexpr auto size = tg::vec2i(48, 48);
    auto const output
        = ctx.persistent.create_texture_2d({.format = sg::pixel_format::bgra8_unorm,
                                            .width = size[0],
                                            .height = size[1],
                                            .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src});
    auto index = u64(0);

    // The converged mean luminance inside a sphere of albedo `rho`, lighting `bounces` bounces past the primary hit.
    auto const measure = [&](f32 rho, int bounces, sv::render_mode mode,
                             sv::hybrid_lighting lighting = sv::hybrid_lighting::deferred) -> cc::shared_async<f64>
    {
        using binding = sv::material_attribute_binding;
        auto const material = lib.acquire(sv::material::create(
            cc::format("glowing sphere {}", rho), openpbr,
            {binding::of("base_color", tg::vec3f(rho, rho, rho)), binding::of("specular_weight", 0.0f),
             binding::of("emission_luminance", 1.0f), binding::of("emission_color", tg::vec3f(1, 1, 1))}));
        auto const item = resources.acquire_scene_item(
            sv::mesh{.name = "glowing sphere", .geometry = geometry, .material = material});
        resources.wait_for_pending_uploads();

        auto def = sv::viewer_definition{};
        {
            auto v = sv::view_data{};
            v.id = sv::view_id::from_string("inside");
            v.camera = sv::camera::looking_at(tg::pos3d(0, 0, 0), tg::pos3d(0, 0, 1));
            v.camera.projection.aspect_ratio = 1.0;
            v.camera.projection.near_plane = 0.01;
            auto& scene = sv::ensure_scene_3d(v);
            scene.items.push_back(item);
            scene.background = sv::background::uniform(tg::vec3f(0, 0, 0));
            scene.fallback_light = cc::nullopt;
            scene.settings.mode = mode;
            scene.settings.hybrid_lighting = lighting;
            scene.settings.hybrid_bounces = bounces;
            // The tracers count the primary hit in a path's length.
            scene.settings.max_bounces = bounces + 1;
            scene.settings.reconstruct.denoiser = sr::denoise_method::none;
            def.views.push_back(cc::move(v));

            auto const root_node = def.nodes.add_container(sv::invalid_node);
            auto leaf = sv::layout_leaf{};
            leaf.views.push_back(sv::view_index(0));
            def.nodes.add_leaf(root_node, cc::move(leaf));
            auto root = sv::view_data{};
            root.id = sv::view_id::from_string("root");
            root.layers.push_back(
                {.kind = sv::layer_kind::layout, .blend = sv::layer_blend::replace, .root_node = root_node});
            def.root_view = sv::view_index(def.views.size());
            def.views.push_back(cc::move(root));
        }
        auto store = sv::view_store{};
        auto const history = sv::view_history{};
        auto const id = def.views[0].id;

        auto const start = cc::current_time_steady_secs();
        while (true)
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
                = state != nullptr ? state->temporal.get_ptr(sv::temporal_id::accumulation(0)) : nullptr;
            auto accumulated = sg::data_future<tg::vec4f>();
            if (slot != nullptr && slot->texture.raw() != nullptr)
                accumulated = sg::data_future<tg::vec4f>(cmd->download.bytes_from_texture(slot->texture.raw()));
            ctx.submit_command_list(cc::move(cmd));
            ctx.advance_epoch();
            co_await ctx.idle_completion();
            sv_test::drive_ambient_work();
            REQUIRE(cc::current_time_steady_secs() - start < 60.0);
            if (store.min_accumulated_samples(id) < 64 || !accumulated.is_valid())
                continue;

            auto const& pixels = co_await accumulated.data();
            auto sum = 0.0;
            for (auto const& p : pixels)
                sum += 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
            co_return sum / f64(pixels.size());
        }
    };

    // The glow alone, with nothing to reflect it, is what Le is in the image's own units.
    auto const le = co_await measure(0.0f, 1, sv::render_mode::hybrid);
    REQUIRE(le > 0.0);
    constexpr auto rho = 0.8;
    struct contender
    {
        sv::render_mode mode = sv::render_mode::hybrid;
        sv::hybrid_lighting lighting = sv::hybrid_lighting::deferred;
    };
    contender const contenders[] = {{.mode = sv::render_mode::hybrid, .lighting = sv::hybrid_lighting::deferred},
                                    {.mode = sv::render_mode::hybrid, .lighting = sv::hybrid_lighting::forward},
                                    {.mode = sv::render_mode::megakernel}};
    int const all_bounces[] = {1, 2};
    // a smoke run measures one contender at one bounce, which is what a preset's flags can break
    auto const smoke = nx::is_smoke();
    for (auto const c : cc::span<contender const>(contenders).subspan({.offset = 0, .size = smoke ? 1 : 3}))
        for (auto const bounces : cc::span<int const>(all_bounces).subspan({.offset = 0, .size = smoke ? 1 : 2}))
        {
            auto const seen = co_await measure(f32(rho), bounces, c.mode, c.lighting);
            auto const expected = bounce_sum(le, rho, bounces);
            CC_LOG_INFO("mode {}, lighting {}, {} bounce(s) inside a glowing sphere of albedo {}: {} Le, expected {} "
                        "Le",
                        int(c.mode), int(c.lighting), bounces, rho, seen / le, expected / le);
            CHECK(tg::abs(seen - expected) < 0.005 * expected)
                .dump("mode", int(c.mode))
                .dump("lighting", int(c.lighting))
                .dump("bounces", bounces)
                .dump("seen, in Le", seen / le)
                .dump("expected, in Le", expected / le);
        }
    co_await cc::async_settled(sv::background_work(ctx));
}
