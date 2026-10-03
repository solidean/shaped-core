#include "viewer_test_env.hh"

#include <clean-core/container/array.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/all.hh>
#include <shaped-viewer/rendering/sgl_pathtrace_routine.hh>
#include <typed-geometry/scalar/scalar.hh> // tg::abs

#if SV_TEST_HAS_VULKAN
#include <shaped-graphics/backends/dx12/dx12_context.hh>     // sg::create_dx12_context, the reference beside vulkan
#include <shaped-graphics/backends/vulkan/vulkan_context.hh> // sg::create_vulkan_context
#endif

using namespace cc::primitive_defines;

// The SGL tracer against the HLSL one: one scene, one camera, one seed, traced by both, and the two images compared.
//
// Each item shades with its own material permutation, the HLSL tracer through the permutation's DXIL hit group and the SGL one through its `sgl_hit_group`.
// They draw the same random stream and run the same math, so what the comparison measures is the port: the generated material code as much as the tracer.
// One case per thing the generator spells differently: constants, mesh attributes, a texture, a cutout, each builtin type, and quadrics.
//
// They agree to rounding rather than to the bit, and the cause is named: DXC compiles without `-Gis`, so every float operation carries LLVM's fast-math flags.
// The SGL text reaches DXC in another shape than the HLSL does, so some terms are reassociated differently and round a unit or two apart.
// On dx12 that is the quadric root solve alone, and compiled with `-Gis` every dx12 image is bit-identical.
// On vulkan every case rounds a few units apart, where the SPIR-V DXC writes reaches the driver's own compiler.
// Neither term steers a branch, so the difference stays at rounding and never forks a path.
//
// Every scene is open, so rays escape to the environment, and lit by one rect light both strategies of the MIS reach.
// The panel stands under a rotated, non-uniformly scaled placement, so the normal goes through the inverse transpose.

namespace
{
/// What one parity case varies: the materials and the geometry beside the floor.
enum class parity_case
{
    pbr,               ///< the builtin `pbr` type, per-triangle attributes on every mesh
    openpbr_constants, ///< `openpbr` with constants a material binds, coat and anisotropy included
    mesh_attributes,   ///< `openpbr` reading per-vertex colors, normals and a tangent frame
    textured,          ///< a base color texture through uvs past [0, 1], sampled nearest and clamped / mirrored
    cutout,            ///< a per-vertex opacity the any-hits cut out stochastically, and an alpha-cutoff one
    unlit,             ///< the builtin `unlit` type
    quadrics,          ///< spheres, a capped cylinder and a cone, per-primitive colors read through the batch
};

[[nodiscard]] cc::string_view name_of(parity_case c)
{
    switch (c)
    {
    case parity_case::pbr:
        return "pbr";
    case parity_case::openpbr_constants:
        return "openpbr constants";
    case parity_case::mesh_attributes:
        return "mesh attributes";
    case parity_case::textured:
        return "textured";
    case parity_case::cutout:
        return "cutout";
    case parity_case::unlit:
        return "unlit";
    case parity_case::quadrics:
        return "quadrics";
    }
    return "";
}

/// What both tracers integrate.
struct parity_scene
{
    cc::vector<sv::scene_item> items;
    cc::vector<sg::tlas_instance> instances;

    /// The permutations the instances' offsets index, two records each, as `view_renderer` lays them out.
    cc::vector<sv::material_permutation const*> hit_groups;
    bool has_quadrics = false;

    sv::camera camera;
    sv::pt_light_table lights;
    sv::background environment;
};

/// The images both tracers produced in one frame, empty where either never traced before the deadline.
struct parity_images
{
    cc::vector<tg::vec4f> hlsl;
    cc::vector<tg::vec4f> sgl;
};

constexpr i32 image_size = 48;
constexpr i32 samples_per_pixel = 8;
constexpr i32 max_bounces = 4;

/// The placements, row-major 3x4: the identity, and the panel's 30-degree turn about y, twice as tall as wide.
constexpr float identity_placement[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
constexpr float panel_placement[12] = {0.8660254f, 0, 0.5f, 0.3f, 0, 2.0f, 0, 0, -0.5f, 0, 0.8660254f, 0.5f};

[[nodiscard]] sv::material_type_id type_of(cc::string_view name)
{
    return sv_test::shared_material_library().acquire_type(name).value();
}

/// A material of builtin type `type`, binding `overrides`.
[[nodiscard]] sv::material_id material_of(cc::string name,
                                          cc::string_view type,
                                          cc::vector<sv::material_attribute_binding> overrides)
{
    return sv_test::shared_material_library().acquire(
        sv::material::create(cc::move(name), type_of(type), cc::move(overrides)));
}

/// The panel's quad, as six unshared vertices: two triangles over the unit square of its own plane.
[[nodiscard]] cc::vector<tg::pos3f> panel_positions()
{
    auto out = cc::vector<tg::pos3f>();
    auto const a = tg::pos3f(0, 0, 0);
    auto const b = tg::pos3f(1, 0, 0);
    auto const c = tg::pos3f(1, 1, 0);
    auto const d = tg::pos3f(0, 1, 0);
    for (auto const& p : {a, b, c, a, c, d})
        out.push_back(p);
    return out;
}

/// A per-vertex attribute of the panel: a value for each of its four corners, spread over the six vertices the way the corners repeat.
template <class T>
[[nodiscard]] sv::mesh_attribute panel_attribute(cc::string name, T const& a, T const& b, T const& c, T const& d)
{
    auto values = cc::vector<T>();
    for (auto const& v : {a, b, c, a, c, d})
        values.push_back(v);
    return sv::mesh_attribute::create(cc::move(name), sv::attribute_frequency::per_vertex, cc::move(values));
}

/// A rotation about the tangent-space normal by `degrees`, as the xyzw quaternion a tangent frame attribute holds.
[[nodiscard]] tg::vec4f rotation_about_z(float degrees)
{
    auto const half = degrees * 0.5f * 3.14159265f / 180.0f;
    return tg::vec4f(0, 0, tg::sin(tg::angle_f::make_from_radians(half)), tg::cos(tg::angle_f::make_from_radians(half)));
}

/// A 4x4 rgba8 checker of eight colors, so a nearest sample tells every texel apart and an address mode shows at the edges.
[[nodiscard]] sv::texture_data checker_texture()
{
    auto pixels = cc::array<byte>::create_defaulted(4 * 4 * 4);
    for (auto y = 0; y < 4; ++y)
        for (auto x = 0; x < 4; ++x)
        {
            auto const i = (y * 4 + x) * 4;
            pixels[i + 0] = byte(40 + 60 * x);
            pixels[i + 1] = byte(220 - 50 * y);
            pixels[i + 2] = byte((x + y) % 2 == 0 ? 200 : 30);
            pixels[i + 3] = byte(255);
        }
    return sv::texture_data::create(cc::move(pixels), sg::pixel_format::rgba8_unorm, 4, 4);
}

/// The panel this case shades, as a mesh: positions, attributes and textures, under `material`.
[[nodiscard]] sv::mesh panel_mesh(parity_case c)
{
    auto mesh = sv::mesh{.name = cc::format("parity panel ({})", name_of(c)),
                         .geometry = sv::triangle_geometry::create_from_positions(panel_positions())};

    switch (c)
    {
    case parity_case::pbr:
    {
        auto const red = sv::pbr_material{.base_color = tg::vec3f(0.7f, 0.2f, 0.1f), .metallic = 0.2f, .roughness = 0.4f};
        auto const materials = cc::array<sv::pbr_material>{red, red};
        mesh.attributes = sv_test::pbr_face_attributes(materials);
        mesh.material = sv::default_material(sv_test::shared_material_library());
        break;
    }
    case parity_case::openpbr_constants:
    {
        auto overrides = cc::vector<sv::material_attribute_binding>();
        overrides.push_back(sv::material_attribute_binding::of("base_color", tg::vec3f(0.2f, 0.5f, 0.8f)));
        overrides.push_back(sv::material_attribute_binding::of("base_metalness", 0.3f));
        overrides.push_back(sv::material_attribute_binding::of("specular_roughness", 0.25f));
        overrides.push_back(sv::material_attribute_binding::of("specular_roughness_anisotropy", 0.6f));
        overrides.push_back(sv::material_attribute_binding::of("coat_weight", 0.5f));
        overrides.push_back(sv::material_attribute_binding::of("coat_roughness", 0.1f));
        overrides.push_back(sv::material_attribute_binding::of("fuzz_weight", 0.3f));
        overrides.push_back(sv::material_attribute_binding::of("thin_film_weight", 0.4f));
        mesh.material = material_of("parity openpbr constants", sv::builtin_material::openpbr, cc::move(overrides));
        break;
    }
    case parity_case::mesh_attributes:
    {
        mesh.attributes.push_back(panel_attribute<tg::vec3f>("base_color", tg::vec3f(0.9f, 0.1f, 0.1f),
                                                             tg::vec3f(0.1f, 0.9f, 0.1f), tg::vec3f(0.1f, 0.1f, 0.9f),
                                                             tg::vec3f(0.9f, 0.9f, 0.1f)));
        mesh.attributes.push_back(panel_attribute<tg::vec3f>("normal", tg::vec3f(0.3f, 0.0f, 1.0f),
                                                             tg::vec3f(-0.2f, 0.2f, 1.0f), tg::vec3f(0.0f, -0.3f, 1.0f),
                                                             tg::vec3f(0.1f, 0.1f, 1.0f)));
        mesh.attributes.push_back(panel_attribute<tg::vec4f>("tangent_frame", rotation_about_z(0.0f),
                                                             rotation_about_z(20.0f), rotation_about_z(45.0f),
                                                             rotation_about_z(-30.0f)));
        mesh.attributes.push_back(panel_attribute<f32>("specular_roughness", 0.2f, 0.4f, 0.6f, 0.3f));
        auto overrides = cc::vector<sv::material_attribute_binding>();
        overrides.push_back(sv::material_attribute_binding::of("specular_roughness_anisotropy", 0.5f));
        mesh.material = material_of("parity openpbr attributes", sv::builtin_material::openpbr, cc::move(overrides));
        break;
    }
    case parity_case::textured:
    {
        // Past [0, 1] on both axes, so the clamp and the mirror each decide some of the panel.
        mesh.attributes.push_back(panel_attribute<tg::vec2f>("uv", tg::vec2f(-0.5f, -0.5f), tg::vec2f(1.5f, -0.5f),
                                                             tg::vec2f(1.5f, 1.5f), tg::vec2f(-0.5f, 1.5f)));
        mesh.textures.push_back({.name = "base_color",
                                 .source = {.texture = checker_texture(),
                                            .sampler = {.min_filter = sg::sampler_filter::nearest,
                                                        .mag_filter = sg::sampler_filter::nearest,
                                                        .address_u = sg::sampler_address_mode::clamp_edge,
                                                        .address_v = sg::sampler_address_mode::mirror_repeat}}});
        mesh.material = material_of("parity openpbr textured", sv::builtin_material::openpbr, {});
        break;
    }
    case parity_case::cutout:
    {
        // A coverage below 1 at three corners, so the stochastic test keeps a fraction of the panel and rejects the rest.
        mesh.attributes.push_back(panel_attribute<f32>("opacity", 0.2f, 0.9f, 0.5f, 1.0f));
        mesh.material = material_of("parity openpbr cutout", sv::builtin_material::openpbr, {});
        break;
    }
    case parity_case::unlit:
    {
        auto overrides = cc::vector<sv::material_attribute_binding>();
        overrides.push_back(sv::material_attribute_binding::of("color", tg::vec3f(0.9f, 0.6f, 0.2f)));
        mesh.material = material_of("parity unlit", sv::builtin_material::unlit, cc::move(overrides));
        break;
    }
    case parity_case::quadrics:
        mesh.material = material_of("parity quadric panel", sv::builtin_material::openpbr, {});
        break;
    }
    return mesh;
}

/// The meshes and batches of `c`, acquired and uploaded, with their placements and hit groups; empty when one did not build.
[[nodiscard]] cc::optional<parity_scene> make_parity_scene(sv::gpu_resource_manager& resources, parity_case c)
{
    auto const gray = sv::pbr_material{.base_color = tg::vec3f(0.5f, 0.5f, 0.5f), .roughness = 1.0f};

    auto floor = sv_test::cornell_box{};
    sv_test::cb_push_quad(floor, tg::pos3f(-3, 0, -3), tg::pos3f(-3, 0, 3), tg::pos3f(3, 0, 3), tg::pos3f(3, 0, -3),
                          gray);

    auto box = sv_test::cornell_box{};
    sv_test::cb_push_box(box, tg::pos3f(-0.9f, 0.0f, -0.2f), tg::pos3f(-0.1f, 0.8f, 0.6f), gray);

    auto scene = parity_scene{};
    auto placements = cc::vector<float const*>();
    scene.items.push_back(
        resources.acquire_scene_item(sv_test::as_mesh("parity floor", floor.positions, floor.materials)));
    placements.push_back(identity_placement);

    // The box is the second material of the cutout case, an alpha cutoff stepping a constant opacity it leaves whole.
    auto box_mesh = sv_test::as_mesh("parity box", box.positions, box.materials);
    if (c == parity_case::cutout)
    {
        auto overrides = cc::vector<sv::material_attribute_binding>();
        overrides.push_back(sv::material_attribute_binding::of("alpha_cutoff", 0.5f));
        box_mesh.attributes = {};
        box_mesh.attributes.push_back(
            sv::mesh_attribute::create("opacity", sv::attribute_frequency::per_triangle,
                                       cc::vector<f32>::create_filled(box.positions.size() / 3, 0.7f)));
        box_mesh.material
            = material_of("parity openpbr alpha cutoff", sv::builtin_material::openpbr, cc::move(overrides));
    }
    scene.items.push_back(resources.acquire_scene_item(box_mesh));
    placements.push_back(identity_placement);

    scene.items.push_back(resources.acquire_scene_item(panel_mesh(c)));
    placements.push_back(panel_placement);

    if (c == parity_case::quadrics)
    {
        auto set = sv::quadric_set();
        set.add_sphere(tg::sphere3f(tg::pos3f(1.3f, 0.45f, 0.2f), 0.45f));
        set.add_sphere(tg::sphere3f(tg::pos3f(0.6f, 0.2f, -0.8f), 0.2f));
        set.add_line(tg::segment3f(tg::pos3f(-1.6f, 0.0f, -0.6f), tg::pos3f(-1.4f, 1.1f, -0.4f)),
                     {.radius = 0.25f, .ends = sv::line_ends::flat});
        set.add_cone(tg::segment3f(tg::pos3f(1.6f, 0.0f, 1.2f), tg::pos3f(1.5f, 1.3f, 1.0f)), 0.4f, true);

        // One color per primitive, which a quadric batch numbers as `per_triangle`.
        auto colors = cc::vector<tg::vec3f>();
        for (auto i = 0; i < set.primitive_count(); ++i)
            colors.push_back(tg::vec3f(0.2f + 0.15f * float(i % 5), 0.8f - 0.1f * float(i % 7), 0.4f));
        set.attributes.push_back(
            sv::mesh_attribute::create("base_color", sv::attribute_frequency::per_triangle, cc::move(colors)));
        set.material = sv::default_material(sv_test::shared_material_library());

        scene.items.push_back(resources.acquire_scene_item(set));
        placements.push_back(identity_placement);
    }
    resources.wait_for_pending_uploads();

    // Laid out as `view_renderer` lays a view out: one hit group per distinct permutation, in first-use order, two records each.
    for (auto i = isize(0); i < scene.items.size(); ++i)
    {
        auto const& item = scene.items[i];
        auto const* const permutation = resources.shaders.find(item.shader_key);
        if (permutation == nullptr)
            return {};

        auto group = scene.hit_groups.size();
        for (auto g = isize(0); g < scene.hit_groups.size(); ++g)
            if (scene.hit_groups[g] == permutation)
                group = g;
        if (group == scene.hit_groups.size())
            scene.hit_groups.push_back(permutation);

        auto blas = sg::blas_handle();
        auto opaque = true;
        if (item.kind == sv::scene_item_kind::quadric_set)
        {
            auto const* const set = resources.quadrics.get_ptr(item.quadrics);
            if (set == nullptr)
                return {};
            blas = set->blas;
            scene.has_quadrics = true;
        }
        else
        {
            auto const* const mesh = resources.meshes.get_ptr(item.mesh);
            if (mesh == nullptr)
                return {};
            blas = mesh->blas;
            opaque = !permutation->can_cut_out;
        }

        auto instance = sg::tlas_instance{.blas = blas,
                                          .instance_id = u32(i),
                                          .hit_group_offset = u32(group) * 2,
                                          .opaque_override = opaque};
        for (auto k = 0; k < 12; ++k)
            instance.transform[k] = placements[i][k];
        scene.instances.push_back(cc::move(instance));
    }

    scene.camera = sv::camera{.position = tg::pos3d(0.4, 1.6, -4.0)};
    scene.camera.projection.vertical_fov = tg::angle_d::make_from_degree(45.0);
    scene.lights = sv_test::light_table_of(
        {.center = tg::vec3f(0.2f, 2.5f, 0.3f), .half_x = 0.6f, .half_z = 0.4f, .emission = tg::vec3f(6.0f, 5.5f, 5.0f)});
    scene.environment = sv::background::gradient(tg::vec3f(0.3f, 0.4f, 0.6f), tg::vec3f(0.05f, 0.04f, 0.03f));
    return scene;
}

/// Says why a trace kept declining, since a hit group that never compiles is otherwise a silent timeout.
void report_unbuilt(cc::span<sv::material_permutation const* const> permutations)
{
    for (auto const* const p : permutations)
    {
        if (p == nullptr)
            continue;
        if (auto const* const e = p->sgl_hit_group->try_error(); e != nullptr)
            CC_LOG_ERROR("an SGL hit group did not compile: {}\n--- source ---\n{}", e->underlying().to_string(),
                         p->sgl_source);
        if (auto const* const e = p->shader->try_error(); e != nullptr)
            CC_LOG_ERROR("an HLSL closest hit did not compile: {}", e->underlying().to_string());
    }
}

/// Traces `scene` once with each tracer, in one frame and from one frame block, and hands back both images.
/// `sgl_only` traces the SGL tracer into both instead, for a context the HLSL tracer cannot trace the scene on.
/// Parameters by value, as a coroutine's must be.
cc::shared_async<parity_images> trace_both(sg::context* ctx,
                                           sv::gpu_resource_manager* resources,
                                           parity_scene scene,
                                           bool sgl_only)
{
    auto fc = sv::pt_frame_constants_gpu{};
    fc.camera = sv::camera_gpu::from(scene.camera);
    fc.previous_camera = fc.camera;
    scene.lights.describe_in(fc);
    fc.samples_per_pixel = samples_per_pixel;
    fc.max_bounces = max_bounces;
    fc.seed = 7u;

    auto const* const fallback = &resources->shaders.acquire_fallback();
    auto const* const quadric_fallback = scene.has_quadrics ? &resources->shaders.acquire_quadric_fallback() : nullptr;

    // Each tracer reads the frame block and the environment from buffers of its own, the HLSL one as constants and the SGL one as storage.
    auto const hlsl_block_usage = sg::buffer_usage::constants_buffer;
    auto const sgl_block_usage = sg::buffer_usage::readonly_buffer;
    auto const target_usage = sg::texture_usage::texture | sg::texture_usage::image | sg::texture_usage::copy_src;

    // A stand-in is no parity: both tracers wait for every permutation of their own to land, rather than for a fallback to cover it.
    auto const all_landed = [&]
    {
        for (auto const* const p : scene.hit_groups)
        {
            (void)ctx->backlog.start(p->sgl_hit_group);
            if (p->sgl_hit_group->try_value() == nullptr)
                return false;
            if (sgl_only)
                continue;
            (void)cc::async_start(p->shader);
            if (p->shader->try_value() == nullptr)
                return false;
            if (p->intersection.is_valid())
            {
                (void)cc::async_start(p->intersection);
                if (p->intersection->try_value() == nullptr)
                    return false;
            }
            if (p->can_cut_out)
            {
                (void)cc::async_start(p->any_hit);
                (void)cc::async_start(p->shadow_any_hit);
                if (p->any_hit->try_value() == nullptr || p->shadow_any_hit->try_value() == nullptr)
                    return false;
            }
        }
        return true;
    };

    auto images = parity_images{};
    auto const loop_start = cc::current_time_steady_secs();
    while (images.hlsl.empty())
    {
        (void)ctx->routines.tick();

        auto cmd = ctx->create_command_list();
        auto records = cc::vector<sv::instance_gpu>();
        for (auto const& item : scene.items)
            records.push_back(item.kind == sv::scene_item_kind::quadric_set
                                  ? resources->describe_instance(*cmd, item.quadrics, item.instance)
                                  : resources->describe_instance(*cmd, item.mesh, item.instance));

        auto const environment = sv::background_gpu::from(scene.environment);
        auto const hlsl_frame = ctx->transient.create_buffer_from_pod(*cmd, fc, hlsl_block_usage);
        auto const hlsl_background = ctx->transient.create_buffer_from_pod(*cmd, environment, hlsl_block_usage);
        auto const sgl_frame = ctx->transient.create_buffer_from_pod(*cmd, fc, sgl_block_usage);
        auto const sgl_background = ctx->transient.create_buffer_from_pod(*cmd, environment, sgl_block_usage);
        auto const instance_table
            = ctx->transient.create_buffer_from_data(*cmd, records, sg::buffer_usage::readonly_buffer);
        auto const light_buffer = sv_test::upload_lights(*cmd, scene.lights);
        auto const make_target = [&]
        {
            return ctx->transient.create_texture_2d({.format = sg::pixel_format::rgba32_float,
                                                     .width = image_size,
                                                     .height = image_size,
                                                     .usage = target_usage});
        };
        auto const hlsl_target = make_target();
        auto const sgl_target = make_target();

        auto const bindless = resources->freeze();
        auto const desc_for = [&](sg::buffer<sv::pt_frame_constants_gpu> const& frame,
                                  sg::buffer<sv::background_gpu> const& background, sg::texture_2d const& target)
        {
            return sv::pt_trace_desc{.frame = frame,
                                     .background = background,
                                     .instances = scene.instances,
                                     .output = target,
                                     .instance_table = instance_table,
                                     .lights = light_buffer,
                                     .hit_groups = scene.hit_groups,
                                     .fallback = fallback,
                                     .quadric_fallback = quadric_fallback,
                                     .bindless = &bindless};
        };
        auto const landed = all_landed();
        auto hlsl_outcome = sg::routine_outcome::declined;
        if (landed && sgl_only)
            hlsl_outcome = sv::sgl_pathtrace_routine::execute(*cmd, desc_for(sgl_frame, sgl_background, hlsl_target));
        else if (landed)
            hlsl_outcome = sv::pathtrace_routine::execute(*cmd, desc_for(hlsl_frame, hlsl_background, hlsl_target));
        auto const sgl_outcome
            = landed ? sv::sgl_pathtrace_routine::execute(*cmd, desc_for(sgl_frame, sgl_background, sgl_target))
                     : sg::routine_outcome::declined;
        auto const both = hlsl_outcome == sg::routine_outcome::executed && sgl_outcome == sg::routine_outcome::executed;

        auto hlsl_back = cc::optional<sg::data_future<tg::vec4f>>();
        auto sgl_back = cc::optional<sg::data_future<tg::vec4f>>();
        if (both)
        {
            hlsl_back = sg::data_future<tg::vec4f>(cmd->download.bytes_from_texture(hlsl_target.raw()));
            sgl_back = sg::data_future<tg::vec4f>(cmd->download.bytes_from_texture(sgl_target.raw()));
        }
        ctx->submit_command_list(cc::move(cmd));
        ctx->advance_epoch();
        (void)co_await ctx->idle_completion();

        if (!both)
        {
            // Not a failure yet: a hit group or a state object has not landed, and the deadline turns a hang into a message.
            if (cc::current_time_steady_secs() - loop_start > 75.0)
            {
                report_unbuilt(scene.hit_groups);
                auto const stand_ins = cc::array<sv::material_permutation const*>{fallback, quadric_fallback};
                report_unbuilt(stand_ins);
                co_return images;
            }
            sv_test::drive_ambient_work();
            continue;
        }

        (void)co_await ctx->idle_completion(); // an epoch advance drains the GPU but not the readback actor
        auto const hlsl_pixels = hlsl_back.value().try_get_data();
        auto const sgl_pixels = sgl_back.value().try_get_data();
        if (!hlsl_pixels.has_value() || !sgl_pixels.has_value())
            co_return images;
        for (auto const& px : hlsl_pixels.value().span())
            images.hlsl.push_back(px);
        for (auto const& px : sgl_pixels.value().span())
            images.sgl.push_back(px);
    }
    co_return images;
}

/// How far a channel of the SGL image may stray from the HLSL one, relative to its value: eight units in the last place.
/// Rounding apart, as the header says why; a forked path would differ by the whole of a contribution instead.
constexpr float k_relative_tolerance = 8.0f * 1.1920929e-7f;

/// The same for a scene holding quadrics, sixteen units, since a root solve carries a reassociated term into the hit point.
/// A unit of `t` moves the point, its normal and every bounce after it, and the integrand carries that continuously: measured at ten on WARP.
/// With `-Gis` the quadric images are bit-identical too, on hardware and on WARP.
constexpr float k_quadric_relative_tolerance = 16.0f * 1.1920929e-7f;

[[nodiscard]] float tolerance_of(parity_case c)
{
    return c == parity_case::quadrics ? k_quadric_relative_tolerance : k_relative_tolerance;
}

/// How far apart two images are: the largest difference of any channel, absolute and relative, and how many pixels differ at all.
struct image_difference
{
    float max_abs = 0.0f;
    float max_relative = 0.0f;
    isize differing_pixels = 0;
    float mean_hlsl = 0.0f;
};

[[nodiscard]] image_difference difference_of(cc::vector<tg::vec4f> const& hlsl, cc::vector<tg::vec4f> const& sgl)
{
    auto d = image_difference{};
    auto sum = 0.0;
    for (auto i = isize(0); i < hlsl.size(); ++i)
    {
        auto differs = false;
        for (auto c = 0; c < 3; ++c)
        {
            auto const delta = tg::abs(hlsl[i][c] - sgl[i][c]);
            d.max_abs = delta > d.max_abs ? delta : d.max_abs;
            auto const magnitude = tg::abs(hlsl[i][c]);
            auto const relative = delta == 0.0f ? 0.0f : delta / (magnitude > 1e-30f ? magnitude : 1e-30f);
            d.max_relative = relative > d.max_relative ? relative : d.max_relative;
            differs = differs || hlsl[i][c] != sgl[i][c];
            sum += hlsl[i][c];
        }
        d.differing_pixels += differs ? 1 : 0;
    }
    d.mean_hlsl = float(sum / double(hlsl.size() * 3));
    return d;
}

/// Case `c` traced on `ctx`, by both tracers or by the SGL one into both images.
cc::shared_async<parity_images> images_of(sg::context* ctx, parity_case c, bool sgl_only)
{
    auto resources = sv::gpu_resource_manager::create(*ctx);
    auto const scene = make_parity_scene(resources, c);
    REQUIRE(scene.has_value());
    auto images = co_await trace_both(ctx, &resources, scene.value(), sgl_only);
    co_await cc::async_settled(sv::background_work(*ctx));
    co_return images;
}

/// Holds `sgl` to `hlsl`, whatever traced each, within `tolerance` relative, and says how far apart they are.
void check_images(parity_images const& images, cc::string_view against, float tolerance = k_relative_tolerance)
{
    REQUIRE(!images.hlsl.empty());
    REQUIRE(images.hlsl.size() == image_size * image_size);
    REQUIRE(images.sgl.size() == images.hlsl.size());

    auto const d = difference_of(images.hlsl, images.sgl);
    CC_LOG_INFO("SGL tracer parity, {}: max abs diff {}, max relative diff {}, {} of {} pixels differ, mean {}",
                against, d.max_abs, d.max_relative, d.differing_pixels, images.hlsl.size(), d.mean_hlsl);

    // The scene is lit, or the comparison compares black with black.
    CHECK(d.mean_hlsl > 0.05f);

    CHECK(d.max_relative <= tolerance)
        .context(cc::format("{}: {} of {} pixels differ, by up to {} ({} relative)", against, d.differing_pixels,
                            images.hlsl.size(), d.max_abs, d.max_relative));
}

/// One parity case, both tracers on whichever context the caller brought up.
cc::shared_async<cc::unit> check_parity(sg::context* ctx, cc::string_view backend, parity_case c)
{
    auto const images = co_await images_of(ctx, c, false);
    check_images(images, cc::format("{} on {}", name_of(c), backend), tolerance_of(c));
    co_return;
}

/// How far one SGL source traced on two APIs may stray apart: the drivers compile DXIL and SPIR-V each their own way.
/// Measured at 21 units in the last place on quadrics, whose root solve carries a difference into the hit point.
constexpr float k_across_api_tolerance = 64.0f * 1.1920929e-7f;

/// One parity case, the SGL tracer on `ctx` held to the HLSL one on `reference`, for a case the HLSL tracer cannot trace on `ctx`.
///
/// Two APIs never agree to the bit, so the port is held where it can be, on `reference`, and the two SGL images to each other.
/// The SGL image on `ctx` may then be no farther from the HLSL one than it is from its own source on `reference`, plus the port's bound.
cc::shared_async<cc::unit> check_parity_against(sg::context* ctx,
                                                cc::string_view backend,
                                                sg::context* reference,
                                                cc::string_view reference_backend,
                                                parity_case c)
{
    auto const held = co_await images_of(reference, c, false);
    auto const own = co_await images_of(ctx, c, true);

    check_images(held, cc::format("{} on {}", name_of(c), reference_backend), tolerance_of(c));
    REQUIRE(own.sgl.size() == held.sgl.size());
    auto const across = difference_of(held.sgl, own.sgl);
    check_images({.hlsl = held.sgl, .sgl = own.sgl},
                 cc::format("{}, the SGL tracer on {} against itself on {}", name_of(c), backend, reference_backend),
                 k_across_api_tolerance);
    check_images({.hlsl = held.hlsl, .sgl = own.sgl},
                 cc::format("{} on {}, against the HLSL tracer on {}", name_of(c), backend, reference_backend),
                 across.max_relative + tolerance_of(c));
    co_return;
}

/// Whether this device and this build can trace at all; the reason to skip when not.
[[nodiscard]] cc::optional<cc::string_view> cannot_trace(sg::context& ctx)
{
    auto probe = ctx.create_command_list();
    auto const supported = probe->raytracing.is_supported() && ctx.supports(sg::feature::raytracing_pipeline);
    ctx.drop_command_list(cc::move(probe));
    if (!supported)
        return cc::string_view("device reports no ray-tracing pipelines");
    if (!sv_test::shared_env().has_compiler)
        return cc::string_view("no DXC compiler to build the path-tracing shaders");
    return {};
}

constexpr parity_case all_cases[] = {parity_case::pbr,
                                     parity_case::openpbr_constants,
                                     parity_case::mesh_attributes,
                                     parity_case::textured,
                                     parity_case::cutout,
                                     parity_case::unlit,
                                     parity_case::quadrics};
} // namespace

ASYNC_INVOCABLE_TEST("sv::sgl_pathtrace_routine - traces what pathtrace_routine traces, to rounding",
                     (sg::context_handle const& ctx_h))
{
    if (auto const reason = cannot_trace(*ctx_h); reason.has_value())
        SKIP(reason.value());
    // In sequence on the one context rather than as sections, which would bring the invocation up again per case.
    for (auto const c : all_cases)
        (void)co_await check_parity(ctx_h.get(), "dx12", c);
}

#if SV_TEST_HAS_VULKAN
// The same comparison on vulkan, whose context this test brings up itself: sv's other GPU tests are dx12's alone.
//
// Two cases the HLSL tracer cannot trace on vulkan, so the SGL tracer there is held to the HLSL one on dx12 hardware instead.
// A texture needs the permutation's sampler group, which the HLSL tracer builds and never binds, and the device is lost.
// A quadric's HLSL closest hit does not compile to SPIR-V: DXC's legalization refuses a payload volatile for one entry point and not another.
ASYNC_TEST("sv vulkan - the SGL tracer traces what the HLSL one traces, to rounding")
{
    auto ctx = sg::create_vulkan_context({.enable_validation_layers = true});
    if (ctx.has_error())
        SKIP("no vulkan device");
    if (auto const reason = cannot_trace(*ctx.value()); reason.has_value())
        SKIP(reason.value());

    auto reference = sg::create_dx12_context({.adapter = sg::backend::dx12::dx12_adapter::hardware});
    for (auto const c : all_cases)
    {
        if (c != parity_case::textured && c != parity_case::quadrics)
            (void)co_await check_parity(ctx.value().get(), "vulkan", c);
        else if (reference.has_value() && !cannot_trace(*reference.value()).has_value())
            (void)co_await check_parity_against(ctx.value().get(), "vulkan", reference.value().get(), "dx12", c);
        else
            CC_LOG_INFO("SGL tracer parity, {} on vulkan: no dx12 hardware device to hold it to", name_of(c));
    }
    CHECK(!ctx.value()->is_device_lost());
}
#endif
