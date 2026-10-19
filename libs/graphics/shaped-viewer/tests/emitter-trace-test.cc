#include "viewer_test_env.hh"

#include <clean-core/common/time.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <nexus/tests/thorough.hh> // nx::is_thorough
#include <shaped-graphics/all.hh>
#include <shaped-viewer/all.hh>
#include <shaped-viewer/rendering/impl/emitters.hh>
#include <typed-geometry/linalg/quat.hh>
#include <typed-geometry/scalar/scalar.hh>
#include <typed-geometry/transform/compose.hh>

// Emissive triangles as lights, in both tracers: an instance whose material declares an emission is sampled by next-event estimation and weighed against the bounce ray that meets it.
// Its emission may be a constant, a texture or a mesh attribute, which the light sample reads at its point as the closest hit does.
// Every scene is traced through `sv::view_renderer`, which is where an instance qualifies, gets its area CDF and joins the light table.
// The comparisons are of block means, so what is left between two images of one expectation is Monte Carlo noise.
// libs/graphics/shaped-viewer/docs/plans/emissive-triangles.md lists what these pin.

namespace
{
using namespace cc::primitive_defines;

constexpr auto k_size = tg::vec2i(32, 32);
constexpr int k_block = 8;

/// An openpbr material that reflects `albedo` as a Lambertian surface and emits `luminance` nits of white.
/// The specular layer is off, so the albedo is exact, which the furnace needs.
[[nodiscard]] sv::material_id lambert(tg::vec3f albedo, f32 luminance = 0.0f, f32 opacity = 1.0f)
{
    auto& lib = sv_test::shared_material_library();
    auto const type = lib.acquire_type(sv::builtin_material::openpbr).value();
    auto bindings = cc::vector<sv::material_attribute_binding>();
    bindings.push_back(sv::material_attribute_binding::of("base_color", albedo));
    bindings.push_back(sv::material_attribute_binding::of("specular_weight", 0.0f));
    bindings.push_back(sv::material_attribute_binding::of("emission_luminance", luminance));
    if (opacity < 1.0f)
        bindings.push_back(sv::material_attribute_binding::of("opacity", opacity));
    return lib.acquire(sv::material::create(
        cc::format("emitter test {} {} {} {} {}", albedo[0], albedo[1], albedo[2], luminance, opacity), type,
        cc::move(bindings)));
}

/// The quad a-b-c-d as two triangles.
void push_quad(cc::vector<tg::pos3f>& out, tg::pos3f a, tg::pos3f b, tg::pos3f c, tg::pos3f d)
{
    out.push_back(a);
    out.push_back(b);
    out.push_back(c);
    out.push_back(a);
    out.push_back(c);
    out.push_back(d);
}

/// The closed box over [lo, hi], as twelve triangles.
[[nodiscard]] cc::vector<tg::pos3f> box_positions(tg::pos3f lo, tg::pos3f hi)
{
    auto out = cc::vector<tg::pos3f>();
    auto const p
        = [&](int x, int y, int z) { return tg::pos3f(x ? hi[0] : lo[0], y ? hi[1] : lo[1], z ? hi[2] : lo[2]); };
    push_quad(out, p(0, 0, 0), p(1, 0, 0), p(1, 0, 1), p(0, 0, 1));
    push_quad(out, p(0, 1, 0), p(0, 1, 1), p(1, 1, 1), p(1, 1, 0));
    push_quad(out, p(0, 0, 0), p(0, 1, 0), p(1, 1, 0), p(1, 0, 0));
    push_quad(out, p(0, 0, 1), p(1, 0, 1), p(1, 1, 1), p(0, 1, 1));
    push_quad(out, p(0, 0, 0), p(0, 0, 1), p(0, 1, 1), p(0, 1, 0));
    push_quad(out, p(1, 0, 0), p(1, 1, 0), p(1, 1, 1), p(1, 0, 1));
    return out;
}

[[nodiscard]] sv::mesh mesh_of(cc::string name, cc::span<tg::pos3f const> positions, sv::material_id material)
{
    return {.name = cc::move(name),
            .geometry = sv::triangle_geometry::create_from_positions(positions),
            .material = material};
}

/// One mesh of a scene and where it is placed.
struct placed_mesh
{
    sv::mesh mesh;
    tg::affine_transform3f transform = {};
};

/// What one image is traced from.
struct emitter_scene
{
    cc::vector<placed_mesh> meshes;
    cc::vector<sv::light> lights;
    sv::camera camera;
};

/// A white floor, and a white back wall that stops below the emitters, so nothing sees an emitter's upper face.
[[nodiscard]] cc::vector<placed_mesh> room()
{
    auto floor = cc::vector<tg::pos3f>();
    push_quad(floor, tg::pos3f(-2, 0, -2), tg::pos3f(2, 0, -2), tg::pos3f(2, 0, 2), tg::pos3f(-2, 0, 2));
    auto wall = cc::vector<tg::pos3f>();
    push_quad(wall, tg::pos3f(-2, 0, 2), tg::pos3f(2, 0, 2), tg::pos3f(2, 1.1f, 2), tg::pos3f(-2, 1.1f, 2));
    auto out = cc::vector<placed_mesh>();
    out.push_back({.mesh = mesh_of("floor", floor, lambert(tg::vec3f(0.8f, 0.8f, 0.8f)))});
    out.push_back({.mesh = mesh_of("wall", wall, lambert(tg::vec3f(0.6f, 0.7f, 0.8f)))});
    return out;
}

/// Looking down at the floor steeply enough that the top of its 60° view stays below every emitter, which hang at 1.4 and above.
/// So what each image shows is light the floor and the wall received.
[[nodiscard]] sv::camera floor_camera()
{
    return sv::camera::looking_at(tg::pos3d(0, 1.2, -1.5), tg::pos3d(0, 0, 0.5));
}

/// The horizontal quad at height `y` spanning `center ± half` in x and z.
[[nodiscard]] cc::vector<tg::pos3f> ceiling_quad(tg::pos3f center, tg::vec2f half)
{
    auto out = cc::vector<tg::pos3f>();
    auto const x0 = center[0] - half[0];
    auto const x1 = center[0] + half[0];
    auto const z0 = center[2] - half[1];
    auto const z1 = center[2] + half[1];
    push_quad(out, tg::pos3f(x0, center[1], z0), tg::pos3f(x1, center[1], z0), tg::pos3f(x1, center[1], z1),
              tg::pos3f(x0, center[1], z1));
    return out;
}

[[nodiscard]] sv::render_settings settings_of(sv::render_mode mode, i32 max_bounces)
{
    return {.samples_per_pixel = 16, .max_bounces = max_bounces, .mode = mode, .rounds_per_frame = 16};
}

/// Traces `scene` through the single-view renderer until its mean holds `samples` per pixel, and reads it back.
/// The count restarts whenever the image does, so the frames before an emitter's area CDF lands are never in it.
/// One manager serves every image of a test, so a permutation compiles once for all of them.
cc::shared_async<cc::vector<tg::vec3f>> render(sg::context& ctx,
                                               sv::gpu_resource_manager& resources,
                                               emitter_scene const& scene,
                                               sv::render_settings settings,
                                               u32 samples)
{
    auto v = sv::view_data{};
    v.id = sv::view_id::from_string("emitter test");
    v.resolution = k_size;
    v.camera = scene.camera;
    auto& l = sv::ensure_scene_3d(v);
    for (auto const& m : scene.meshes)
    {
        auto item = resources.acquire_scene_item(m.mesh);
        item.transform = m.transform;
        l.items.push_back(item);
    }
    for (auto i = isize(0); i < scene.lights.size(); ++i)
        l.lights.push_back({.id = sv::light_id::from_string(cc::format("light {}", i)), .light = scene.lights[i]});
    l.fallback_light = cc::nullopt;
    l.settings = settings;
    resources.wait_for_pending_uploads();

    auto store = sv::view_store{};
    auto const start = cc::current_time_steady_secs();
    while (true)
    {
        (void)ctx.routines.tick();
        auto cmd = ctx.create_command_list();
        resources.advance_to(ctx.current_epoch());
        store.begin_frame(u64(ctx.current_epoch()));
        auto const image = sv::view_renderer::execute(*cmd, v, resources, store);
        auto readback = sg::data_future<tg::vec4f>();
        if (store.min_accumulated_samples(v.id) >= samples)
            readback = sg::data_future<tg::vec4f>(cmd->download.bytes_from_texture(image.raw()));
        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();
        co_await ctx.idle_completion();

        if (readback.is_valid())
        {
            auto const pixels = readback.try_get_data();
            REQUIRE(pixels.has_value());
            REQUIRE(pixels.value().size() == isize(k_size[0]) * isize(k_size[1]));
            auto out = cc::vector<tg::vec3f>();
            for (auto const& p : pixels.value())
                out.push_back(tg::vec3f(p[0], p[1], p[2]));
            (void)co_await cc::async_settled(sv::background_work(ctx));
            co_return out;
        }
        // A cold compile of every kernel and every permutation takes an unoptimized build a minute or more.
        REQUIRE(cc::current_time_steady_secs() - start < 240.0);
        sv_test::drive_ambient_work();
    }
}

/// The samples per pixel an image accumulates, the same for both tracers so their noise compares: 32 megakernel frames of 16.
constexpr u32 k_samples = 512;

[[nodiscard]] tg::vec3f block_mean(cc::span<tg::vec3f const> image, int bx, int by)
{
    auto sum = tg::vec3f(0, 0, 0);
    for (auto y = by * k_block; y < (by + 1) * k_block; ++y)
        for (auto x = bx * k_block; x < (bx + 1) * k_block; ++x)
            sum += image[y * k_size[0] + x];
    return sum / f32(k_block * k_block);
}

[[nodiscard]] f32 luminance_of(tg::vec3f c)
{
    return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
}

/// Checks that every block of `image` holds the luminance `expected` does, to `tolerance` relative to the expected block.
/// That tolerance is floored at a twentieth of the brightest block, so a nearly black block is held to the image's scale.
void check_blocks_match(cc::span<tg::vec3f const> image,
                        cc::span<tg::vec3f const> expected,
                        f32 tolerance,
                        cc::string_view what)
{
    auto brightest = 0.0f;
    for (auto by = 0; by < k_size[1] / k_block; ++by)
        for (auto bx = 0; bx < k_size[0] / k_block; ++bx)
            brightest = cc::max(brightest, luminance_of(block_mean(expected, bx, by)));
    REQUIRE(brightest > 0.0f);

    for (auto by = 0; by < k_size[1] / k_block; ++by)
        for (auto bx = 0; bx < k_size[0] / k_block; ++bx)
        {
            auto const got = luminance_of(block_mean(image, bx, by));
            auto const want = luminance_of(block_mean(expected, bx, by));
            auto const err = tg::abs(got - want) / (want + 0.05f * brightest);
            CHECK(err < tolerance).context(cc::format("{}: block ({}, {}) holds {}, expected {}", what, bx, by, got, want));
        }
}

/// `check_blocks_match` per channel: every block of `image` holds each channel `expected` does, to `tolerance` relative to the expected block's channel.
/// That tolerance is floored at a twentieth of the brightest channel of any block, so a dim channel is held to the image's scale.
/// Luminance alone barely sees red and blue, so an emission read with its channels swapped, or at its mean, passes it where it fails this.
void check_channels_match(cc::span<tg::vec3f const> image,
                          cc::span<tg::vec3f const> expected,
                          f32 tolerance,
                          cc::string_view what)
{
    auto brightest = 0.0f;
    for (auto by = 0; by < k_size[1] / k_block; ++by)
        for (auto bx = 0; bx < k_size[0] / k_block; ++bx)
        {
            auto const m = block_mean(expected, bx, by);
            brightest = cc::max(brightest, cc::max(m[0], cc::max(m[1], m[2])));
        }
    REQUIRE(brightest > 0.0f);

    for (auto by = 0; by < k_size[1] / k_block; ++by)
        for (auto bx = 0; bx < k_size[0] / k_block; ++bx)
        {
            auto const got = block_mean(image, bx, by);
            auto const want = block_mean(expected, bx, by);
            for (auto c = 0; c < 3; ++c)
            {
                auto const err = tg::abs(got[c] - want[c]) / (want[c] + 0.05f * brightest);
                CHECK(err < tolerance)
                    .context(cc::format("{}: block ({}, {}) channel {} holds {}, expected {}", what, bx, by, c, got[c],
                                        want[c]));
            }
        }
}

[[nodiscard]] bool can_trace(sg::context& ctx, sv::render_mode mode)
{
    if (!ctx.supports(sg::feature::raytracing_pipeline))
        return false;
    return mode != sv::render_mode::wavefront || ctx.supports(sg::feature::ray_query);
}

/// Why this device and build cannot trace at all, or empty where they can.
[[nodiscard]] cc::string_view skip_reason(sg::context& ctx)
{
    if (!sv_test::shared_env().has_compiler)
        return "no SGL compiler that reaches DXIL to build the path-tracing shaders";
    if (!can_trace(ctx, sv::render_mode::megakernel))
        return "device reports no ray tracing support";
    return {};
}

constexpr sv::render_mode k_modes[] = {sv::render_mode::megakernel, sv::render_mode::wavefront};

/// The tracers a scene is traced with on `ctx`: both, but the megakernel alone in a default run on a software adapter.
/// There the wavefront's cost is its dispatches, a dozen per round, and its legs here would take most of the binary's budget.
/// The emissive quad's test traces both regardless, so the wavefront's emitter sampling is traced by default there too.
[[nodiscard]] cc::span<sv::render_mode const> modes_of(sg::context& ctx)
{
    if (!nx::is_thorough() && ctx.metrics.adapter().is_software)
        return cc::span<sv::render_mode const>(k_modes).subspan(cc::offset_size{.offset = 0, .size = 1});
    return k_modes;
}

[[nodiscard]] cc::string_view name_of(sv::render_mode mode)
{
    return mode == sv::render_mode::megakernel ? "megakernel" : "wavefront";
}
} // namespace

// The emitter and the rect are the same patch at the same luminance, so the floor receives the same light from either.
// The rect sits a hair below a black quad of the emitter's shape, so the two scenes occlude alike.
// It emits downward alone, as nothing above the emitter sees its upper face.
// One bounce pins the light sample alone; several pin it against the bounce ray that meets the emitter.
ASYNC_INVOCABLE_TEST("sv - an emissive quad lights a floor as a rect light of its size and luminance does",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (auto const why = skip_reason(ctx); !why.empty())
        SKIP(why);
    auto resources = sv::gpu_resource_manager::create(ctx);

    auto const center = tg::pos3f(0, 1.5f, 0.3f);
    auto const half = tg::vec2f(0.5f, 0.4f);
    constexpr auto nits = 5.0f;

    auto emitter = emitter_scene{.meshes = room(), .camera = floor_camera()};
    emitter.meshes.push_back({.mesh = mesh_of("lamp", ceiling_quad(center, half), lambert(tg::vec3f(0, 0, 0), nits))});

    auto rect = emitter_scene{.meshes = room(), .camera = floor_camera()};
    rect.meshes.push_back({.mesh = mesh_of("shade", ceiling_quad(center, half), lambert(tg::vec3f(0, 0, 0)))});
    rect.lights.push_back(
        sv::light::rect(center - tg::vec3f(0, 1e-3f, 0), tg::vec3f(half[0], 0, 0), tg::vec3f(0, 0, half[1])).nits(nits));

    for (auto const mode : k_modes)
    {
        if (!can_trace(ctx, mode))
            continue;
        for (auto const bounces : {1, 4})
        {
            auto const settings = settings_of(mode, bounces);
            auto const lit = co_await render(ctx, resources, emitter, settings, k_samples);
            auto const reference = co_await render(ctx, resources, rect, settings, k_samples);
            check_blocks_match(lit, reference, 0.08f, cc::format("{}, {} bounces", name_of(mode), bounces));
        }
    }
}

// The same patch as above, tessellated into 648 triangles of uneven sizes, which spans three blocks of the area CDF, the last one partial.
// A pick that favored some triangles over their area, in either search, would light the floor unevenly under the lamp.
// Three bounces, so the bounce ray's density for a triangle of a later block is weighed too.
ASYNC_INVOCABLE_TEST("sv - an emitter of many uneven triangles lights as a rect of its size and luminance does",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (auto const why = skip_reason(ctx); !why.empty())
        SKIP(why);
    auto resources = sv::gpu_resource_manager::create(ctx);

    auto const center = tg::pos3f(0, 1.5f, 0.3f);
    auto const half = tg::vec2f(0.5f, 0.4f);
    constexpr auto nits = 5.0f;
    constexpr auto cells = 18;

    // Cell edges bunched toward one corner, so the triangles' areas span about two orders of magnitude.
    auto const edge = [](f32 lo, f32 hi, int i) { return lo + (hi - lo) * tg::pow(f32(i) / f32(cells), 1.8f); };
    auto lamp = cc::vector<tg::pos3f>();
    for (auto i = 0; i < cells; ++i)
        for (auto j = 0; j < cells; ++j)
        {
            auto const x0 = edge(center[0] - half[0], center[0] + half[0], i);
            auto const x1 = edge(center[0] - half[0], center[0] + half[0], i + 1);
            auto const z0 = edge(center[2] - half[1], center[2] + half[1], j);
            auto const z1 = edge(center[2] - half[1], center[2] + half[1], j + 1);
            push_quad(lamp, tg::pos3f(x0, center[1], z0), tg::pos3f(x1, center[1], z0), tg::pos3f(x1, center[1], z1),
                      tg::pos3f(x0, center[1], z1));
        }
    REQUIRE(sv::emitter_cdf_block_count(lamp.size() / 3) == 3);

    auto emitter = emitter_scene{.meshes = room(), .camera = floor_camera()};
    emitter.meshes.push_back({.mesh = mesh_of("tessellated lamp", lamp, lambert(tg::vec3f(0, 0, 0), nits))});

    auto rect = emitter_scene{.meshes = room(), .camera = floor_camera()};
    rect.meshes.push_back({.mesh = mesh_of("shade", ceiling_quad(center, half), lambert(tg::vec3f(0, 0, 0)))});
    rect.lights.push_back(
        sv::light::rect(center - tg::vec3f(0, 1e-3f, 0), tg::vec3f(half[0], 0, 0), tg::vec3f(0, 0, half[1])).nits(nits));

    for (auto const mode : modes_of(ctx))
    {
        if (!can_trace(ctx, mode))
            continue;
        auto const settings = settings_of(mode, 3);
        auto const lit = co_await render(ctx, resources, emitter, settings, k_samples);
        auto const reference = co_await render(ctx, resources, rect, settings, k_samples);
        check_blocks_match(lit, reference, 0.08f, name_of(mode));
    }
}

// The balance heuristic's two weights sum to one, so how often next-event estimation picks the emitter moves the noise and never the mean.
// A point light below the floor and behind the wall takes nearly all of the power-weighted choice and lights neither face the camera sees.
// So the emitter is reached almost only by bounce rays, and the mean must be the one it has when it is the only light.
// Behind the wall rather than below the room's middle: a shadow ray from where the wall meets the floor starts within its minimum distance of the floor, and so passes it.
ASYNC_INVOCABLE_TEST("sv - an emitter's mean is the same sampled or reached by bounce rays almost alone",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (auto const why = skip_reason(ctx); !why.empty())
        SKIP(why);
    auto resources = sv::gpu_resource_manager::create(ctx);

    auto sampled = emitter_scene{.meshes = room(), .camera = floor_camera()};
    sampled.meshes.push_back({.mesh = mesh_of("lamp", ceiling_quad(tg::pos3f(0, 1.4f, 0.4f), tg::vec2f(0.6f, 0.6f)),
                                              lambert(tg::vec3f(0, 0, 0), 3.0f))});

    auto bounced = sampled;
    bounced.lights.push_back(sv::light::point(tg::pos3f(0, -0.5f, 3)).candela(1e5f));

    for (auto const mode : modes_of(ctx))
    {
        if (!can_trace(ctx, mode))
            continue;
        auto const settings = settings_of(mode, 3);
        auto const alone = co_await render(ctx, resources, sampled, settings, k_samples);
        auto const mostly_bounced = co_await render(ctx, resources, bounced, settings, 2 * k_samples);
        check_blocks_match(mostly_bounced, alone, 0.1f, name_of(mode));
    }
}

// A closed box whose walls emit E and reflect a Lambertian albedo ρ: every pixel sees E (1 + ρ + ... + ρ^N) after N bounces.
// The camera's view of a wall is its emission at full weight.
// Each bounce after it is a light sample and a bounce ray weighed against each other, so a weight that drifted would show as a geometric series that does not sum.
ASYNC_INVOCABLE_TEST("sv - an emissive furnace converges to its geometric series", (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (auto const why = skip_reason(ctx); !why.empty())
        SKIP(why);
    auto resources = sv::gpu_resource_manager::create(ctx);

    constexpr auto albedo = 0.5f;
    constexpr auto emission = 1.0f;
    constexpr auto bounces = 4;
    auto expected = 0.0f;
    auto term = emission;
    for (auto k = 0; k <= bounces; ++k)
    {
        expected += term;
        term *= albedo;
    }

    auto furnace = emitter_scene{.camera = sv::camera::looking_at(tg::pos3d(0.1, -0.2, 0.05), tg::pos3d(0.3, 0.1, 1))};
    furnace.meshes.push_back({.mesh = mesh_of("furnace", box_positions(tg::pos3f(-1, -1, -1), tg::pos3f(1, 1, 1)),
                                              lambert(tg::vec3f(albedo, albedo, albedo), emission))});

    for (auto const mode : modes_of(ctx))
    {
        if (!can_trace(ctx, mode))
            continue;
        auto const image = co_await render(ctx, resources, furnace, settings_of(mode, bounces), k_samples);
        auto mean = tg::vec3f(0, 0, 0);
        for (auto const& p : image)
            mean += p;
        mean = mean / f32(image.size());
        for (auto c = 0; c < 3; ++c)
            CHECK(tg::abs(mean[c] - expected) / expected < 0.03f)
                .context(cc::format("{}: channel {} averages {}, expected {}", name_of(mode), c, mean[c], expected));
    }
}

// The CDF is in object space and the density a sample carries is its triangle's width over its WORLD area.
// So a placement that scales unevenly and rotates must light exactly as the same mesh transformed in advance.
ASYNC_INVOCABLE_TEST("sv - an emitter placed by a non-uniform scale and a rotation lights as the mesh transformed in "
                     "advance",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (auto const why = skip_reason(ctx); !why.empty())
        SKIP(why);
    auto resources = sv::gpu_resource_manager::create(ctx);

    auto const placement
        = tg::compose(tg::affine_transform3f::make_translation(tg::vec3f(0.2f, 1.3f, 0.5f)),
                      tg::compose(tg::affine_transform3f::make_rotation(tg::quat_f::make_rotation_axis_angle(
                                      tg::normalize(tg::vec3f(1, 0.5f, 0.2f)), tg::angle_f::make_from_degree(35.0f))),
                                  tg::affine_transform3f::make_scaling(tg::vec3f(0.6f, 0.08f, 0.3f))));
    auto const local = box_positions(tg::pos3f(-1, -1, -1), tg::pos3f(1, 1, 1));
    auto world = cc::vector<tg::pos3f>();
    for (auto const& p : local)
        world.push_back(placement.transform(p));

    auto const material = lambert(tg::vec3f(0, 0, 0), 6.0f);
    auto placed = emitter_scene{.meshes = room(), .camera = floor_camera()};
    placed.meshes.push_back({.mesh = mesh_of("placed lamp", local, material), .transform = placement});
    auto baked = emitter_scene{.meshes = room(), .camera = floor_camera()};
    baked.meshes.push_back({.mesh = mesh_of("baked lamp", world, material)});

    for (auto const mode : modes_of(ctx))
    {
        if (!can_trace(ctx, mode))
            continue;
        auto const settings = settings_of(mode, 3);
        auto const a = co_await render(ctx, resources, placed, settings, k_samples);
        auto const b = co_await render(ctx, resources, baked, settings, k_samples);
        check_blocks_match(a, b, 0.08f, name_of(mode));
    }
}

// Two emitters, a dim large one and a bright small one, picked by power: the mean of both is the sum of each alone.
// Each alone keeps the other's quad, black and so no light, so the three scenes occlude alike.
ASYNC_INVOCABLE_TEST("sv - power selection over a dim large and a bright small emitter keeps the mean",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (auto const why = skip_reason(ctx); !why.empty())
        SKIP(why);
    auto resources = sv::gpu_resource_manager::create(ctx);

    auto const large = ceiling_quad(tg::pos3f(-0.7f, 1.6f, 0.4f), tg::vec2f(0.8f, 0.5f));
    auto const small = ceiling_quad(tg::pos3f(0.8f, 1.2f, 0.2f), tg::vec2f(0.15f, 0.15f));
    auto const make = [&](f32 large_nits, f32 small_nits)
    {
        auto s = emitter_scene{.meshes = room(), .camera = floor_camera()};
        s.meshes.push_back({.mesh = mesh_of("large", large, lambert(tg::vec3f(0, 0, 0), large_nits))});
        s.meshes.push_back({.mesh = mesh_of("small", small, lambert(tg::vec3f(0, 0, 0), small_nits))});
        return s;
    };

    for (auto const mode : modes_of(ctx))
    {
        if (!can_trace(ctx, mode))
            continue;
        auto const settings = settings_of(mode, 3);
        auto const both = co_await render(ctx, resources, make(0.5f, 40.0f), settings, k_samples);
        auto const large_alone = co_await render(ctx, resources, make(0.5f, 0.0f), settings, k_samples);
        auto const small_alone = co_await render(ctx, resources, make(0.0f, 40.0f), settings, k_samples);
        auto sum = cc::vector<tg::vec3f>();
        for (auto i = isize(0); i < both.size(); ++i)
            sum.push_back(large_alone[i] + small_alone[i]);
        check_blocks_match(both, sum, 0.08f, name_of(mode));
    }
}

// A cutout emitter is there with the probability of its opacity, which a bounce ray meets it with and a light sample is scaled by.
// So at opacity one half it delivers half the light.
ASYNC_INVOCABLE_TEST("sv - a cutout emitter at opacity one half delivers half the light",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (auto const why = skip_reason(ctx); !why.empty())
        SKIP(why);
    auto resources = sv::gpu_resource_manager::create(ctx);

    auto const quad = ceiling_quad(tg::pos3f(0, 1.4f, 0.4f), tg::vec2f(0.6f, 0.5f));
    auto const make = [&](f32 opacity)
    {
        auto s = emitter_scene{.meshes = room(), .camera = floor_camera()};
        s.meshes.push_back({.mesh = mesh_of("lamp", quad, lambert(tg::vec3f(0, 0, 0), 4.0f, opacity))});
        return s;
    };

    for (auto const mode : modes_of(ctx))
    {
        if (!can_trace(ctx, mode))
            continue;
        auto const settings = settings_of(mode, 2);
        auto const half = co_await render(ctx, resources, make(0.5f), settings, k_samples);
        auto const whole = co_await render(ctx, resources, make(1.0f), settings, k_samples);
        auto halved = cc::vector<tg::vec3f>();
        for (auto const& p : whole)
            halved.push_back(p * 0.5f);
        check_blocks_match(half, halved, 0.1f, name_of(mode));
    }
}

// The area CDF the GPU builds: monotone within each block and over the blocks, each ending at exactly 1.
// Every width is proportional to its triangle's area, a zero-area triangle's width is exactly 0, and the whole area follows the blocks.
// Over 300 triangles, so it spans two blocks and the second one is partial.
ASYNC_INVOCABLE_TEST("sv - an emitter's area CDF is monotone, ends at one, and gives a zero-area triangle zero width",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (!sv_test::shared_env().has_compiler)
        SKIP("no SGL compiler that reaches DXIL to build the kernels");

    constexpr auto triangle_count = 300;
    constexpr auto degenerate = 7;
    auto positions = cc::vector<tg::pos3f>();
    auto areas = cc::vector<f64>();
    auto total = 0.0;
    for (auto i = 0; i < triangle_count; ++i)
    {
        // Right triangles of legs 1 and s, so each area is s / 2; one is a point.
        auto const s = i == degenerate ? 0.0f : 0.1f + 0.01f * f32(i % 37);
        auto const o = tg::pos3f(f32(i), 0, 0);
        positions.push_back(o);
        positions.push_back(o + tg::vec3f(1, 0, 0));
        positions.push_back(o + tg::vec3f(0, s, 0));
        areas.push_back(0.5 * f64(s));
        total += 0.5 * f64(s);
    }

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const id = resources.meshes.acquire(sv::triangle_data::create(positions));
    resources.wait_for_pending_uploads();
    REQUIRE(tg::abs(f64(resources.meshes.get(id).surface_area) - total) < 1e-3 * total);

    auto readback = sg::data_future<f32>();
    auto const start = cc::current_time_steady_secs();
    while (!readback.is_valid())
    {
        (void)ctx.routines.tick();
        auto cmd = ctx.create_command_list();
        auto const* const cdf = resources.meshes.acquire_emitter_cdf(*cmd, id);
        if (cdf != nullptr)
            readback = cmd->download.data_from_buffer(*cdf);
        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();
        co_await ctx.idle_completion();
        if (!readback.is_valid())
        {
            REQUIRE(cc::current_time_steady_secs() - start < 120.0);
            sv_test::drive_ambient_work();
        }
    }
    (void)co_await cc::async_settled(sv::background_work(ctx));

    auto const data = readback.try_get_data();
    REQUIRE(data.has_value());
    auto const& cdf = data.value();
    auto const blocks = isize(sv::emitter_cdf_block_count(triangle_count));
    REQUIRE(blocks == 2);
    REQUIRE(cdf.size() == triangle_count + blocks + 1);

    auto const block_cdf = [&](isize b) { return cdf[triangle_count + b]; };
    CHECK(block_cdf(blocks - 1) == 1.0f);
    CHECK(tg::abs(f64(cdf[triangle_count + blocks]) - total) < 1e-4 * total);

    auto monotone = true;
    auto proportional = true;
    for (auto b = isize(0); b < blocks; ++b)
    {
        auto const block_low = b > 0 ? block_cdf(b - 1) : 0.0f;
        monotone = monotone && block_cdf(b) >= block_low;
        auto const first = b * isize(sv::emitter_cdf_block_size);
        auto const last = cc::min(first + isize(sv::emitter_cdf_block_size), isize(triangle_count));
        CHECK(cdf[last - 1] == 1.0f).context(cc::format("block {} ends at {}", b, cdf[last - 1]));
        for (auto i = first; i < last; ++i)
        {
            auto const low = i > first ? cdf[i - 1] : 0.0f;
            monotone = monotone && cdf[i] >= low;
            auto const width = f64(block_cdf(b) - block_low) * f64(cdf[i] - low);
            proportional = proportional && tg::abs(width - areas[i] / total) < 1e-5;
            if (i == degenerate)
                CHECK(cdf[i] == low);
        }
    }
    CHECK(monotone);
    CHECK(proportional);
}

// Selection weighs an emitter by its world area, and the estimate from a placement's determinant undercounts a flattened lamp.
// So the GPU sums the exact area for a placement that is no similarity.
// A 2x2x2 cube made a 4x4 panel 0.02 thick, then turned, has 32.32 of area where the estimate gives ~6.4.
// A horizontal quad flattened to nothing along its normal keeps its whole area, where the estimate gives 0.
ASYNC_INVOCABLE_TEST("sv - an emitter's exact world area under a non-uniform placement is summed on the GPU",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (!sv_test::shared_env().has_compiler)
        SKIP("no SGL compiler that reaches DXIL to build the kernels");

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const cube
        = resources.meshes.acquire(sv::triangle_data::create(box_positions(tg::pos3f(-1, -1, -1), tg::pos3f(1, 1, 1))));
    auto const quad
        = resources.meshes.acquire(sv::triangle_data::create(ceiling_quad(tg::pos3f(0, 1, 0), tg::vec2f(0.5f, 0.4f))));
    resources.wait_for_pending_uploads();

    auto const turn = tg::affine_transform3f::make_rotation(tg::quat_f::make_rotation_axis_angle(
        tg::normalize(tg::vec3f(1, 0.5f, 0.2f)), tg::angle_f::make_from_degree(35.0f)));
    auto const panel = tg::compose(turn, tg::affine_transform3f::make_scaling(tg::vec3f(2, 2, 0.01f))).linear_mat();
    auto const flat = tg::affine_transform3f::make_scaling(tg::vec3f(1, 0, 1)).linear_mat();
    REQUIRE(!sv::impl::is_similarity(panel));
    REQUIRE(!sv::impl::is_similarity(flat));

    auto panel_area = cc::optional<f32>();
    auto flat_area = cc::optional<f32>();
    auto const start = cc::current_time_steady_secs();
    while (!panel_area.has_value() || !flat_area.has_value())
    {
        (void)ctx.routines.tick();
        auto cmd = ctx.create_command_list();
        panel_area = resources.meshes.acquire_world_area(*cmd, cube, panel);
        flat_area = resources.meshes.acquire_world_area(*cmd, quad, flat);
        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();
        co_await ctx.idle_completion();
        if (!panel_area.has_value() || !flat_area.has_value())
        {
            REQUIRE(cc::current_time_steady_secs() - start < 120.0);
            sv_test::drive_ambient_work();
        }
    }
    (void)co_await cc::async_settled(sv::background_work(ctx));

    CHECK(tg::abs(panel_area.value() - 32.32f) < 1e-3f * 32.32f).context(cc::format("panel: {}", panel_area.value()));
    CHECK(tg::abs(flat_area.value() - 0.8f) < 1e-5f).context(cc::format("flattened quad: {}", flat_area.value()));
    CHECK(sv::impl::world_area_of(0.8f, tg::affine_transform3f::make_scaling(tg::vec3f(1, 0, 1))) == 0.0f);
}

namespace
{
/// A uv per vertex of `positions`, a horizontal patch spanning `center ± half`: u runs with x across it, v with z.
[[nodiscard]] cc::vector<tg::vec2f> patch_uvs(cc::span<tg::pos3f const> positions, tg::pos3f center, tg::vec2f half)
{
    auto out = cc::vector<tg::vec2f>();
    for (auto const& p : positions)
        out.push_back(
            tg::vec2f((p[0] - center[0] + half[0]) / (2 * half[0]), (p[2] - center[2] + half[1]) / (2 * half[1])));
    return out;
}

/// The patch at `center ± half`, emitting `nits` times a 2x1 texture whose left texel is black and right one white.
/// Nearest and clamped, so its left half emits nothing and its right half `nits`, split exactly where x is `center`'s.
[[nodiscard]] sv::mesh half_white_lamp(tg::pos3f center, tg::vec2f half, f32 nits)
{
    auto const positions = ceiling_quad(center, half);
    auto mesh = mesh_of("half-white lamp", positions, lambert(tg::vec3f(0, 0, 0), nits));
    mesh.attributes.push_back(
        sv::mesh_attribute::create("uv", sv::attribute_frequency::per_vertex, patch_uvs(positions, center, half)));

    auto pixels = cc::array<byte>::create_filled(2 * 4, byte(255));
    for (auto i = 0; i < 3; ++i)
        pixels[i] = byte(0);
    mesh.textures.push_back(
        {.name = "emission_color",
         .source = {.texture = sv::texture_data::create(cc::move(pixels), sg::pixel_format::rgba8_unorm, 2, 1),
                    .uv_attribute = "uv",
                    .sampler = {.mag_filter = sg::sampler_filter::nearest,
                                .address_u = sg::sampler_address_mode::clamp_edge,
                                .address_v = sg::sampler_address_mode::clamp_edge}}});
    return mesh;
}

/// The brightest block of `image`, by luminance.
[[nodiscard]] f32 brightest_block(cc::span<tg::vec3f const> image)
{
    auto brightest = 0.0f;
    for (auto by = 0; by < k_size[1] / k_block; ++by)
        for (auto bx = 0; bx < k_size[0] / k_block; ++bx)
            brightest = cc::max(brightest, luminance_of(block_mean(image, bx, by)));
    return brightest;
}
} // namespace

// A textured emitter's light sample reads its emission from the texture at the sampled point, as its closest hit does.
// Half black and half white, it lights the floor as a rect over its white half does, the full patch a black shade in the rect's scene.
// And the camera sees its texture: the patch split into a black mesh and a constant emitter of the white half's luminance looks the same.
// Two bounces, so the bounce ray that meets the patch is weighed against the light sample at a texel of either color.
ASYNC_INVOCABLE_TEST("sv - a half-black, half-white textured emitter lights as a rect over its white half",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (auto const why = skip_reason(ctx); !why.empty())
        SKIP(why);
    auto resources = sv::gpu_resource_manager::create(ctx);

    auto const center = tg::pos3f(0, 1.5f, 0.3f);
    auto const half = tg::vec2f(0.5f, 0.4f);
    auto const quarter = 0.5f * half[0];
    constexpr auto nits = 5.0f;

    auto textured = emitter_scene{.meshes = room(), .camera = floor_camera()};
    textured.meshes.push_back({.mesh = half_white_lamp(center, half, nits)});

    auto rect = emitter_scene{.meshes = room(), .camera = floor_camera()};
    rect.meshes.push_back({.mesh = mesh_of("shade", ceiling_quad(center, half), lambert(tg::vec3f(0, 0, 0)))});
    rect.lights.push_back(
        sv::light::rect(center + tg::vec3f(quarter, -1e-3f, 0), tg::vec3f(quarter, 0, 0), tg::vec3f(0, 0, half[1]))
            .nits(nits));

    // Looking up at the patch from below and in front, so the image holds both halves and the floor and wall they light.
    auto const up = sv::camera::looking_at(tg::pos3d(0, 0.3, -0.9), tg::pos3d(0, 1.5, 0.3));
    auto seen = textured;
    seen.camera = up;
    auto split = emitter_scene{.meshes = room(), .camera = up};
    split.meshes.push_back(
        {.mesh = mesh_of("black half", ceiling_quad(center - tg::vec3f(quarter, 0, 0), tg::vec2f(quarter, half[1])),
                         lambert(tg::vec3f(0, 0, 0)))});
    split.meshes.push_back(
        {.mesh = mesh_of("white half", ceiling_quad(center + tg::vec3f(quarter, 0, 0), tg::vec2f(quarter, half[1])),
                         lambert(tg::vec3f(0, 0, 0), nits))});

    for (auto const mode : modes_of(ctx))
    {
        if (!can_trace(ctx, mode))
            continue;
        auto const settings = settings_of(mode, 2);
        auto const lit = co_await render(ctx, resources, textured, settings, k_samples);
        auto const reference = co_await render(ctx, resources, rect, settings, k_samples);
        check_blocks_match(lit, reference, 0.08f, cc::format("{}, the floor", name_of(mode)));

        auto const looked_at = co_await render(ctx, resources, seen, settings, k_samples);
        auto const halves = co_await render(ctx, resources, split, settings, k_samples);
        check_blocks_match(looked_at, halves, 0.08f, cc::format("{}, the patch seen", name_of(mode)));
    }
}

namespace
{
/// The emission ramp the per-vertex and per-corner emitters carry: linear in x across the patch at `center ± half`, so any triangle interpolates it exactly.
/// Red and green rise from 0.2 to 1.8 while blue falls, so the luminance follows the ramp and the channels tell its ends apart.
/// A read at the mean, or with red and blue swapped, then lights the floor askew in the luminance and in each channel.
[[nodiscard]] tg::vec3f ramp_at(tg::pos3f p, tg::pos3f center, tg::vec2f half)
{
    auto const u = (p[0] - center[0] + half[0]) / (2 * half[0]);
    return tg::vec3f(0.2f + 1.6f * u, 0.2f + 1.6f * u, 1.8f - 1.6f * u);
}
} // namespace

// A per-vertex emission ramps across the patch, which the light sample interpolates at its point as the closest hit does at its own.
// Were the two to read different values, the balance heuristic's weights would no longer sum to one, and the mean would move with how often the patch is sampled.
// So the image lit by the patch alone matches the one where a point light below the floor takes nearly every pick, as for a constant emitter.
// Compared per channel, at the 0.1 the bounced comparisons take: the room is near gray, so a channel's noise is the luminance's.
ASYNC_INVOCABLE_TEST("sv - a per-vertex emitter's mean is the same sampled or reached by bounce rays almost alone",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (auto const why = skip_reason(ctx); !why.empty())
        SKIP(why);
    auto resources = sv::gpu_resource_manager::create(ctx);

    auto const center = tg::pos3f(0, 1.4f, 0.4f);
    auto const half = tg::vec2f(0.6f, 0.6f);
    auto const positions = ceiling_quad(center, half);
    auto ramp = cc::vector<tg::vec3f>();
    for (auto const& p : positions)
        ramp.push_back(ramp_at(p, center, half));
    auto lamp = mesh_of("ramped lamp", positions, lambert(tg::vec3f(0, 0, 0), 3.0f));
    lamp.attributes.push_back(
        sv::mesh_attribute::create("emission_color", sv::attribute_frequency::per_vertex, cc::move(ramp)));

    auto sampled = emitter_scene{.meshes = room(), .camera = floor_camera()};
    sampled.meshes.push_back({.mesh = lamp});
    auto bounced = sampled;
    bounced.lights.push_back(sv::light::point(tg::pos3f(0, -0.5f, 3)).candela(1e5f));

    for (auto const mode : modes_of(ctx))
    {
        if (!can_trace(ctx, mode))
            continue;
        auto const settings = settings_of(mode, 3);
        auto const alone = co_await render(ctx, resources, sampled, settings, k_samples);
        auto const mostly_bounced = co_await render(ctx, resources, bounced, settings, 2 * k_samples);

        // A patch that lit nothing would match too, so the floor must be lit at all.
        auto const brightest = brightest_block(alone);
        CHECK(brightest > 0.1f).context(cc::format("{}: the brightest block holds {}", name_of(mode), brightest));
        check_channels_match(mostly_bounced, alone, 0.1f, name_of(mode));
    }
}

// The test Cornell box's lamp carries its emission as a per-triangle `emissive` attribute of the `pbr` type, which makes it a light.
// It lights the box as the same lamp does with that emission bound as a constant, the rest of its material alike.
// The walls carry the attribute too, zero throughout, which keeps them out of the light table.
ASYNC_INVOCABLE_TEST("sv - the Cornell box's per-triangle lamp lights the box as a constant one does",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (auto const why = skip_reason(ctx); !why.empty())
        SKIP(why);
    auto resources = sv::gpu_resource_manager::create(ctx);

    auto const box = sv_test::make_cornell_box();
    auto const lamp_vertices = 3 * sv_test::cornell_lamp_triangles;
    auto const walls = box.positions.size() - lamp_vertices;
    auto const lamp_positions
        = cc::span<tg::pos3f const>(box.positions).subspan(cc::offset_size{.offset = walls, .size = lamp_vertices});
    auto const lamp_materials
        = cc::span<sv_test::pbr_material const>(box.materials)
              .subspan(cc::offset_size{.offset = box.materials.size() - sv_test::cornell_lamp_triangles,
                                       .size = sv_test::cornell_lamp_triangles});
    auto const& lamp = lamp_materials[0];

    auto& lib = sv_test::shared_material_library();
    auto const pbr = lib.acquire_type(sv::builtin_material::pbr).value();
    auto const constant_material
        = lib.acquire(sv::material::create("cornell lamp, constant", pbr,
                                           {sv::material_attribute_binding::of("base_color", lamp.base_color),
                                            sv::material_attribute_binding::of("metallic", lamp.metallic),
                                            sv::material_attribute_binding::of("roughness", lamp.roughness),
                                            sv::material_attribute_binding::of("emissive", lamp.emissive)}));

    auto camera = sv::camera{.position = tg::pos3d(0, 0, -3.4)};
    camera.projection.vertical_fov = tg::angle_d::make_from_degree(45.0);
    auto const enclosure = sv_test::as_lit_meshes(box, lamp.emissive[0]).enclosure;

    auto per_triangle = emitter_scene{.camera = camera};
    per_triangle.meshes.push_back({.mesh = enclosure});
    per_triangle.meshes.push_back(
        {.mesh = sv_test::as_mesh("cornell lamp, per triangle", lamp_positions, lamp_materials)});

    auto constant = emitter_scene{.camera = camera};
    constant.meshes.push_back({.mesh = enclosure});
    constant.meshes.push_back({.mesh = mesh_of("cornell lamp, constant", lamp_positions, constant_material)});

    for (auto const mode : modes_of(ctx))
    {
        if (!can_trace(ctx, mode))
            continue;
        auto const settings = settings_of(mode, 3);
        auto const lit = co_await render(ctx, resources, per_triangle, settings, k_samples);
        auto const reference = co_await render(ctx, resources, constant, settings, k_samples);
        CHECK(brightest_block(reference) > 0.1f);
        check_blocks_match(lit, reference, 0.08f, name_of(mode));
    }
}

// The ramp above on an INDEXED patch, where a per-vertex element is the position buffer's and a per-corner one the index buffer's.
// So the light sample's `ctx.corner` must name the indexed vertex, and a per-corner read the triangle's own three elements.
// The per-vertex patch alone must match itself reached by bounce rays almost alone, as the non-indexed one does above.
// And the same ramp per corner must light the floor as per vertex does, since both interpolate one linear function.
// A per-corner read that took the vertex index, or a per-vertex read that took the corner, would light one of them askew.
// 0.1 for the bounced comparison, as above, since its bounce-ray estimate is the noisier; 0.08 between two sampled images, as elsewhere; both per channel.
ASYNC_INVOCABLE_TEST("sv - an indexed emitter reads its per-vertex and per-corner emission at the indexed vertex",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (auto const why = skip_reason(ctx); !why.empty())
        SKIP(why);
    auto resources = sv::gpu_resource_manager::create(ctx);

    auto const center = tg::pos3f(0, 1.4f, 0.4f);
    auto const half = tg::vec2f(0.6f, 0.6f);
    auto const welded = sv_test::weld_triangle_list(ceiling_quad(center, half));
    REQUIRE(welded.positions.size() == 4);

    auto per_vertex = cc::vector<tg::vec3f>();
    for (auto const& p : welded.positions)
        per_vertex.push_back(ramp_at(p, center, half));
    auto per_corner = cc::vector<tg::vec3f>();
    for (auto const i : welded.indices)
        per_corner.push_back(ramp_at(welded.positions[i], center, half));

    auto const material = lambert(tg::vec3f(0, 0, 0), 3.0f);
    auto const lamp = [&](cc::string name, sv::attribute_frequency frequency, cc::vector<tg::vec3f> values)
    {
        auto m = sv::mesh{
            .name = cc::move(name),
            .geometry = sv::triangle_geometry::create_from_indexed_triangles(welded.positions, welded.indices),
            .material = material};
        m.attributes.push_back(sv::mesh_attribute::create("emission_color", frequency, cc::move(values)));
        return m;
    };

    auto vertex_lit = emitter_scene{.meshes = room(), .camera = floor_camera()};
    vertex_lit.meshes.push_back(
        {.mesh = lamp("indexed ramp, per vertex", sv::attribute_frequency::per_vertex, per_vertex)});
    auto vertex_bounced = vertex_lit;
    vertex_bounced.lights.push_back(sv::light::point(tg::pos3f(0, -0.5f, 3)).candela(1e5f));
    auto corner_lit = emitter_scene{.meshes = room(), .camera = floor_camera()};
    corner_lit.meshes.push_back(
        {.mesh = lamp("indexed ramp, per corner", sv::attribute_frequency::per_corner, per_corner)});

    for (auto const mode : modes_of(ctx))
    {
        if (!can_trace(ctx, mode))
            continue;
        auto const settings = settings_of(mode, 3);
        auto const alone = co_await render(ctx, resources, vertex_lit, settings, k_samples);
        auto const brightest = brightest_block(alone);
        CHECK(brightest > 0.1f).context(cc::format("{}: the brightest block holds {}", name_of(mode), brightest));

        auto const mostly_bounced = co_await render(ctx, resources, vertex_bounced, settings, 2 * k_samples);
        check_channels_match(mostly_bounced, alone, 0.1f, cc::format("{}, per vertex bounced", name_of(mode)));

        auto const cornered = co_await render(ctx, resources, corner_lit, settings, k_samples);
        check_channels_match(cornered, alone, 0.08f, cc::format("{}, per corner", name_of(mode)));
    }
}

// The Cornell box's lamp with its two triangles emitting different colors and luminances, as one per-triangle emitter.
// It lights the box as two constant emitters do, one per triangle, so the light sample reads each triangle's own element.
// The two lean red and blue, so a read of the other triangle's element, or of the mean, shows in the channels the walls reflect apart.
// Its record's estimate is the mean of the two, which shapes the noise alone, so the tolerance is the file's 0.08, per channel.
ASYNC_INVOCABLE_TEST("sv - a per-triangle lamp of two different triangles lights as two constant ones do",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (auto const why = skip_reason(ctx); !why.empty())
        SKIP(why);
    auto resources = sv::gpu_resource_manager::create(ctx);

    auto const box = sv_test::make_cornell_box();
    auto const lamp_vertices = 3 * sv_test::cornell_lamp_triangles;
    auto const walls = box.positions.size() - lamp_vertices;
    auto const lamp_positions
        = cc::span<tg::pos3f const>(box.positions).subspan(cc::offset_size{.offset = walls, .size = lamp_vertices});
    auto lamp_materials = cc::vector<sv_test::pbr_material>();
    lamp_materials.push_back(box.materials[box.materials.size() - 2]);
    lamp_materials.push_back(box.materials[box.materials.size() - 1]);
    lamp_materials[0].emissive = tg::vec3f(24, 18, 9);
    lamp_materials[1].emissive = tg::vec3f(6, 12, 21);

    auto& lib = sv_test::shared_material_library();
    auto const pbr = lib.acquire_type(sv::builtin_material::pbr).value();
    auto const constant_of = [&](sv_test::pbr_material const& m, i32 triangle)
    {
        return lib.acquire(sv::material::create(cc::format("cornell lamp triangle {}, constant", triangle), pbr,
                                                {sv::material_attribute_binding::of("base_color", m.base_color),
                                                 sv::material_attribute_binding::of("metallic", m.metallic),
                                                 sv::material_attribute_binding::of("roughness", m.roughness),
                                                 sv::material_attribute_binding::of("emissive", m.emissive)}));
    };

    auto camera = sv::camera{.position = tg::pos3d(0, 0, -3.4)};
    camera.projection.vertical_fov = tg::angle_d::make_from_degree(45.0);
    auto const enclosure = sv_test::as_lit_meshes(box, 15.0f).enclosure;

    auto per_triangle = emitter_scene{.camera = camera};
    per_triangle.meshes.push_back({.mesh = enclosure});
    per_triangle.meshes.push_back(
        {.mesh = sv_test::as_mesh("cornell lamp, two triangles", lamp_positions, lamp_materials)});

    auto constants = emitter_scene{.camera = camera};
    constants.meshes.push_back({.mesh = enclosure});
    for (auto t = 0; t < 2; ++t)
        constants.meshes.push_back({.mesh = mesh_of(cc::format("cornell lamp triangle {}", t),
                                                    lamp_positions.subspan(cc::offset_size{.offset = 3 * t, .size = 3}),
                                                    constant_of(lamp_materials[t], t))});

    for (auto const mode : modes_of(ctx))
    {
        if (!can_trace(ctx, mode))
            continue;
        auto const settings = settings_of(mode, 3);
        auto const lit = co_await render(ctx, resources, per_triangle, settings, k_samples);
        auto const reference = co_await render(ctx, resources, constants, settings, k_samples);
        CHECK(brightest_block(reference) > 0.1f);
        check_channels_match(lit, reference, 0.08f, name_of(mode));
    }
}

namespace
{
/// An openpbr material that reflects nothing and emits `color` at `luminance`.
[[nodiscard]] sv::material_id glow(tg::vec3f color, f32 luminance)
{
    auto& lib = sv_test::shared_material_library();
    auto const type = lib.acquire_type(sv::builtin_material::openpbr).value();
    return lib.acquire(
        sv::material::create(cc::format("emitter test glow {} {} {} {}", color[0], color[1], color[2], luminance), type,
                             {sv::material_attribute_binding::of("base_color", tg::vec3f(0, 0, 0)),
                              sv::material_attribute_binding::of("specular_weight", 0.0f),
                              sv::material_attribute_binding::of("emission_color", color),
                              sv::material_attribute_binding::of("emission_luminance", luminance)}));
}

/// A 2x1 rgba8 texture of `left` then `right`, sampled nearest and clamped, so each half of a patch reads one texel.
[[nodiscard]] sv::texture_sample two_texel_texture(tg::vec4f left,
                                                   tg::vec4f right,
                                                   sv::sample_transform transform = {},
                                                   sv::channel_swizzle swizzle = {})
{
    auto pixels = cc::array<byte>::create_filled(2 * 4, byte(0));
    for (auto c = 0; c < 4; ++c)
    {
        pixels[c] = byte(u8(left[c] * 255.0f + 0.5f));
        pixels[4 + c] = byte(u8(right[c] * 255.0f + 0.5f));
    }
    return {.texture = sv::texture_data::create(cc::move(pixels), sg::pixel_format::rgba8_unorm, 2, 1),
            .uv_attribute = "uv",
            .sampler = {.mag_filter = sg::sampler_filter::nearest,
                        .address_u = sg::sampler_address_mode::clamp_edge,
                        .address_v = sg::sampler_address_mode::clamp_edge},
            .swizzle = swizzle,
            .transform = transform};
}
} // namespace

// A textured color under a swizzle, a scale and a bias, times a textured luminance: the light sample's one-lane read and its transform.
// The color's texels are (0.2, 0.4, 0) and (0.6, 0.8, 1), which the swizzle (b, r, g) reads as (0, 0.2, 0.4) and (1, 0.6, 0.8).
// A scale of 4 and that bias lift them to (1, 1.3, 1.85) and (5, 2.9, 3.45).
// The luminance's red channel is 1 on the left and 0.6 on the right, so the halves emit (1, 1.3, 1.85) and (3, 1.74, 2.07).
// Its other channels are 0, 0.2 and 0.4 on both halves, so a scale read from any channel but red lights the halves otherwise.
// Every texel is a multiple of 51, which an 8-bit unorm stores exactly.
// It lights the floor as two constant emitters of those halves do; 0.08, as the half-white texture's test above.
ASYNC_INVOCABLE_TEST("sv - a textured emitter under a transform and a textured luminance lights as its constant halves",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    if (auto const why = skip_reason(ctx); !why.empty())
        SKIP(why);
    auto resources = sv::gpu_resource_manager::create(ctx);

    auto const center = tg::pos3f(0, 1.5f, 0.3f);
    auto const half = tg::vec2f(0.5f, 0.4f);
    auto const quarter = 0.5f * half[0];

    auto const positions = ceiling_quad(center, half);
    auto lamp = mesh_of("transformed textured lamp", positions, lambert(tg::vec3f(0, 0, 0), 1.0f));
    lamp.attributes.push_back(
        sv::mesh_attribute::create("uv", sv::attribute_frequency::per_vertex, patch_uvs(positions, center, half)));
    lamp.textures.push_back(
        {.name = "emission_color",
         .source = two_texel_texture(
             tg::vec4f(0.2f, 0.4f, 0, 1), tg::vec4f(0.6f, 0.8f, 1, 1),
             {.scale = tg::vec4f(4, 4, 4, 1), .bias = tg::vec4f(1, 0.5f, 0.25f, 0)},
             sv::channel_swizzle::of(sv::texture_channel::b, sv::texture_channel::r, sv::texture_channel::g))});
    lamp.textures.push_back({.name = "emission_luminance",
                             .source = two_texel_texture(tg::vec4f(1, 0, 0.2f, 0.4f), tg::vec4f(0.6f, 0, 0.2f, 0.4f))});

    auto textured = emitter_scene{.meshes = room(), .camera = floor_camera()};
    textured.meshes.push_back({.mesh = cc::move(lamp)});

    auto halves = emitter_scene{.meshes = room(), .camera = floor_camera()};
    halves.meshes.push_back(
        {.mesh = mesh_of("left half", ceiling_quad(center - tg::vec3f(quarter, 0, 0), tg::vec2f(quarter, half[1])),
                         glow(tg::vec3f(1, 1.3f, 1.85f), 1.0f))});
    halves.meshes.push_back(
        {.mesh = mesh_of("right half", ceiling_quad(center + tg::vec3f(quarter, 0, 0), tg::vec2f(quarter, half[1])),
                         glow(tg::vec3f(5, 2.9f, 3.45f), 0.6f))});

    for (auto const mode : modes_of(ctx))
    {
        if (!can_trace(ctx, mode))
            continue;
        auto const settings = settings_of(mode, 2);
        auto const lit = co_await render(ctx, resources, textured, settings, k_samples);
        auto const reference = co_await render(ctx, resources, halves, settings, k_samples);
        CHECK(brightest_block(reference) > 0.1f);
        check_blocks_match(lit, reference, 0.08f, name_of(mode));
    }
}
