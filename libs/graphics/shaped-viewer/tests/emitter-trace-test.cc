#include "viewer_test_env.hh"

#include <clean-core/common/time.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/all.hh>
#include <typed-geometry/linalg/quat.hh>
#include <typed-geometry/scalar/scalar.hh>
#include <typed-geometry/transform/compose.hh>

// Emissive triangles as lights, in both tracers: an instance whose material declares a constant emission is sampled by next-event estimation and weighed against the bounce ray that meets it.
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
    // No frame budget: a budgeted still frame holds however many rounds this GPU fits, so two images would hold different counts.
    return {.samples_per_pixel = 16,
            .max_bounces = max_bounces,
            .mode = mode,
            .rounds_per_frame = 16,
            .frame_budget_ms = 0.0f};
}

/// Traces `scene` through the single-view renderer until its mean holds `frames` frames, and reads it back.
/// The count restarts whenever the image does, so the frames before an emitter's area CDF lands are never in it.
/// One manager serves every image of a test, so a permutation compiles once for all of them.
cc::shared_async<cc::vector<tg::vec3f>> render(sg::context& ctx,
                                               sv::gpu_resource_manager& resources,
                                               emitter_scene const& scene,
                                               sv::render_settings settings,
                                               u32 frames)
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
        if (store.accumulated_frames(v.id) >= frames)
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

/// The frames each tracer accumulates for one image: the megakernel's hold 16 samples, the wavefront's fewer.
[[nodiscard]] u32 frames_for(sv::render_mode mode)
{
    return mode == sv::render_mode::megakernel ? 32u : 96u;
}

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
            auto const lit = co_await render(ctx, resources, emitter, settings, frames_for(mode));
            auto const reference = co_await render(ctx, resources, rect, settings, frames_for(mode));
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

    for (auto const mode : k_modes)
    {
        if (!can_trace(ctx, mode))
            continue;
        auto const settings = settings_of(mode, 3);
        auto const lit = co_await render(ctx, resources, emitter, settings, frames_for(mode));
        auto const reference = co_await render(ctx, resources, rect, settings, frames_for(mode));
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

    for (auto const mode : k_modes)
    {
        if (!can_trace(ctx, mode))
            continue;
        auto const settings = settings_of(mode, 3);
        auto const alone = co_await render(ctx, resources, sampled, settings, frames_for(mode));
        auto const mostly_bounced = co_await render(ctx, resources, bounced, settings, 2 * frames_for(mode));
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

    for (auto const mode : k_modes)
    {
        if (!can_trace(ctx, mode))
            continue;
        auto const image = co_await render(ctx, resources, furnace, settings_of(mode, bounces), frames_for(mode));
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

    for (auto const mode : k_modes)
    {
        if (!can_trace(ctx, mode))
            continue;
        auto const settings = settings_of(mode, 3);
        auto const a = co_await render(ctx, resources, placed, settings, frames_for(mode));
        auto const b = co_await render(ctx, resources, baked, settings, frames_for(mode));
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

    for (auto const mode : k_modes)
    {
        if (!can_trace(ctx, mode))
            continue;
        auto const settings = settings_of(mode, 3);
        auto const both = co_await render(ctx, resources, make(0.5f, 40.0f), settings, frames_for(mode));
        auto const large_alone = co_await render(ctx, resources, make(0.5f, 0.0f), settings, frames_for(mode));
        auto const small_alone = co_await render(ctx, resources, make(0.0f, 40.0f), settings, frames_for(mode));
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

    for (auto const mode : k_modes)
    {
        if (!can_trace(ctx, mode))
            continue;
        auto const settings = settings_of(mode, 2);
        auto const half = co_await render(ctx, resources, make(0.5f), settings, frames_for(mode));
        auto const whole = co_await render(ctx, resources, make(1.0f), settings, frames_for(mode));
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
