#include <clean-core/record/log.hh>          // CC_LOG_ERROR, CC_LOG_INFO
#include <clean-core/streams/file_stream.hh> // cc::file_read_stream_adapter
#include <nexus/test.hh>
#include <shaped-viewer/all.hh>

using namespace cc::primitive_defines;

// Amazon Lumberyard's Bistro, from NVIDIA's ORCA release, loaded from its FBX and path-traced.
//
// A production scene in a format nothing else here reads: 2.8 million triangles over 132 materials outside, and 1.3 million over 74 inside, with every texture a DDS.
// It is NOT in the repository.
// `uv run extern/bistro/fetch-bistro.py` downloads it once (about 850 MB), and without it this example says so and stops.
//
// The materials are read the way Falcor reads them (`fbx_material_convention::orca_metal_rough`).
// That is a base color, a packed occlusion-roughness-metalness map, a DirectX-convention normal map and an emissive map.
//
// Two scenes share this file.
// `bistro` is the exterior under a low sun: `sv::daylight`'s sky, with its sun aimed the way the file's own directional light points.
// That sun stands behind the buildings, so the street lies in shadow and only the upper floors catch it.
// The archive's san_giuseppe_bridge_4k.hdr is not used: sv's background is a spherical-harmonics probe, not a map.
// `bistro-interior` is the interior with filled wine glasses (BistroInterior_Wine.fbx) at night, lit by small rects standing in for its four point lights, and by its emissive lamps.
// FBX gives a light no physical unit; the file's intensities are taken as candela and lowered by a level tuned by eye.
//
// Controls
//   right-drag   look           W/A/S/D   move along the view         E / Q   rise and fall
//   shift        faster         ctrl      slower                      wheel   retune the base speed
//
// Run it:
//   uv run extern/bistro/fetch-bistro.py
//   uv run dev.py example shaped-viewer/bistro
//   uv run dev.py example shaped-viewer/bistro-interior

namespace
{
enum class scene_kind
{
    exterior,
    interior,
};

/// How far the exterior's daylight is raised, in stops: the street is open sky, so it needs little.
constexpr float exterior_exposure_stops = 1.0f;

/// What the interior's pendant lights are lowered by, in stops, tuned by eye on the start view's capture.
constexpr float interior_light_stops = -1.0f;

/// How far below its bulb each pendant's stand-in rect hangs, and half its side, in meters.
constexpr float pendant_drop = 0.05f;
constexpr float pendant_half_size = 0.05f;

/// What the interior's emissive lamps are raised by, in stops — their file luminance of one nit is a dim glow at night.
constexpr float interior_emission_stops = 3.0f;

/// One of the glass and liquid materials Falcor's BistroInterior_Wine.pyscene sets up, which the FBX itself cannot say are transmissive.
struct glass_override
{
    cc::string_view material = {};
    float ior = 1.5f;
    float roughness = 0.0f;
    /// Beer-Lambert absorption per meter; zero for clear glass.
    tg::vec3f absorption = tg::vec3f(0, 0, 0);
};

/// The pyscene's values, as it writes them.
constexpr glass_override glass_overrides[] = {
    {.material = "TransparentGlass", .ior = 1.55f},
    {.material = "TransparentGlassWine", .ior = 1.55f, .absorption = tg::vec3f(102.68f, 168.02f, 246.80f)},
    {.material = "Water", .ior = 1.33f},
    {.material = "Ice", .ior = 1.31f, .roughness = 0.1f},
    {.material = "White_Wine", .ior = 1.33f, .absorption = tg::vec3f(12.29f, 16.52f, 20.30f)},
    {.material = "Red_Wine", .ior = 1.33f, .absorption = tg::vec3f(117.13f, 251.91f, 294.34f)},
    {.material = "Beer", .ior = 1.33f, .absorption = tg::vec3f(11.79f, 25.46f, 58.37f)},
};

/// The depth OpenPBR's `transmission_color` is measured at, in meters: what the absorption above leaves after a centimeter.
constexpr float glass_color_depth = 0.01f;

/// Whether the file's material `name` is `base`, which FBX may suffix — Bistro's are `TransparentGlass.DoubleSided` and the like.
[[nodiscard]] bool names_material(cc::string_view name, cc::string_view base)
{
    return name.starts_with(base) && (name.size() == base.size() || name[base.size()] == '.');
}

[[nodiscard]] cc::string_view file_of(scene_kind kind)
{
    return kind == scene_kind::exterior ? SV_BISTRO_DIR "/BistroExterior.fbx" : SV_BISTRO_DIR "/BistroInterior_Wine.fbx";
}

/// The meshes as placed: the file's own, with glass made transmissive and the interior's lamps brightened.
/// Both are per-mesh values over the material, which win over its own and leave the file's materials alone.
cc::vector<sv::mesh> placed_meshes(sv::asset const& bistro, scene_kind kind)
{
    auto meshes = cc::vector<sv::mesh>();
    for (auto const& mesh : bistro.meshes())
        meshes.push_back(mesh);

    // The FBX gives glass only an opacity below one, which the importer reads as coverage; the pyscene says what it is.
    for (auto const& g : glass_overrides)
    {
        auto matched = false;
        for (auto const& slot : bistro.data().materials)
        {
            if (!names_material(slot.name, g.material))
                continue;
            matched = true;
            auto const clear = g.absorption == tg::vec3f(0, 0, 0);
            auto const color = tg::vec3f(tg::exp(-g.absorption[0] * glass_color_depth),
                                         tg::exp(-g.absorption[1] * glass_color_depth),
                                         tg::exp(-g.absorption[2] * glass_color_depth));
            for (auto const index : slot.meshes)
            {
                auto& attributes = meshes[index].attributes;
                attributes.push_back(sv::mesh_attribute::create_value("opacity", 1.0f));
                attributes.push_back(sv::mesh_attribute::create_value("base_metalness", 0.0f));
                attributes.push_back(sv::mesh_attribute::create_value("transmission_weight", 1.0f));
                attributes.push_back(sv::mesh_attribute::create_value("specular_ior", g.ior));
                attributes.push_back(sv::mesh_attribute::create_value("specular_roughness", g.roughness));
                if (!clear)
                {
                    attributes.push_back(sv::mesh_attribute::create_value("transmission_color", color));
                    attributes.push_back(sv::mesh_attribute::create_value("transmission_depth", glass_color_depth));
                }
            }
        }
        if (!matched)
            CC_LOG_INFO("bistro: the glass override '{}' matches no material", g.material);
    }

    if (kind != scene_kind::interior)
        return meshes;

    // A mesh glows when its material connected an emissive map, which the importer hands over as the mesh's `emission_color` texture.
    // Only those are brightened, since raising the luminance of any other mesh would light it in OpenPBR's default white.
    for (auto& mesh : meshes)
        for (auto const& t : mesh.textures)
            if (t.name == "emission_color")
            {
                mesh.attributes.push_back(
                    sv::mesh_attribute::create_value("emission_luminance", tg::pow(2.0f, interior_emission_stops)));
                break;
            }
    return meshes;
}

void run(cc::string_view id, scene_kind kind)
{
    // Held where it will not move: a load in flight reaches back into its loader.
    auto const loader = sv::asset_loader({.fbx_materials = sv::fbx_material_convention::orca_metal_rough});
    auto const file = file_of(kind);

    // A scene that was never fetched is the normal state of a checkout, so it is information; a present one that fails to load is an error.
    if (cc::file_read_stream_adapter::open(file).has_error())
    {
        CC_LOG_INFO("bistro: {} is not here; fetch the scene with `uv run extern/bistro/fetch-bistro.py`", file);
        return;
    }

    auto bistro = loader.load_async(file);

    auto meshes = cc::vector<sv::mesh>();
    auto const daylight = sv::daylight();
    CC_LOG_INFO("bistro: loading {}", file);

    for (auto f : sv::interactive(id))
    {
        bistro.poll();
        if (bistro.has_error())
        {
            CC_LOG_ERROR("bistro: could not load {}: {}", file, bistro.error());
            return;
        }

        if (bistro.is_ready() && meshes.empty())
        {
            meshes = placed_meshes(bistro, kind);
            CC_LOG_INFO("bistro: {} meshes, {} materials, {} lights", bistro.meshes().size(),
                        bistro.data().materials.size(), bistro.data().lights.size());
            // A real asset loads with issues as a matter of course, so they are information rather than warnings.
            for (auto const& issue : bistro.issues())
                CC_LOG_INFO("bistro: {}", issue);
        }

        auto view = f.window().view();

        // Where the walk starts: the file's own `Camera` node, at its own field of view.
        // Outside, on the street south of the bistro looking north along it; inside, by the door looking down the room.
        if (kind == scene_kind::exterior)
            view.initial_fps({.position = tg::pos3d(14.61, 4.29, -41.81),
                              .yaw = tg::angle_d::make_from_degree(-18.6),
                              .pitch = tg::angle_d::make_from_degree(-4.0),
                              .vertical_fov = tg::angle_d::make_from_degree(34.0)});
        else
            view.initial_fps({.position = tg::pos3d(0.05, 1.44, -1.43),
                              .yaw = tg::angle_d::make_from_degree(96.6),
                              .pitch = tg::angle_d::make_from_degree(-2.1),
                              .vertical_fov = tg::angle_d::make_from_degree(34.0)});
        view.camera_style(sv::camera_style::fly);

        // Until the load lands the view shows only the background, and a capture would converge on that.
        if (!bistro.is_ready())
            f.hold_capture("Bistro is still loading");

        auto scene = view.add_scene();

        // A moving camera traces a quarter of the pixels and upscales them; a still one converges at full size.
        scene.settings({.reconstruct
                        = {.denoiser = sr::denoise_method::automatic, .scale = sr::render_scale_preset::performance}});

        // Empty until the load lands, so the loop needs no branch.
        for (auto const& mesh : meshes)
            scene.add_mesh(mesh);

        if (kind == scene_kind::exterior)
        {
            // The file's directional light is its sun, so its placement aims ours; its intensity is Falcor's, not ours.
            auto sun = daylight.sun;
            sun.exposure(exterior_exposure_stops);
            for (auto const& l : bistro.data().lights)
                if (l.light.path() == sv::light_path::distant_point)
                    sun.placement = l.light.placement;

            scene.background(daylight.sky.scaled(tg::pow(2.0f, exterior_exposure_stops)));
            scene.add_light("sun", sun);
        }
        else
        {
            // The interior's four pendant lights, just below where the file puts them and in their warm color.
            // A point inside a shade lights the shade's inside from centimeters away, and every path bouncing off it is a firefly.
            // So each is a small rect just below its bulb, facing down, which sends nothing up into the shade.
            // Its area bounds what a near surface receives.
            // Its lumens are the point's downward half, 2 pi candela: the same flux into the lower half, more of it straight down than toward the walls.
            // Any other light is added as the file gives it, at the same level.
            for (auto const& l : bistro.data().lights)
            {
                if (l.light.path() != sv::light_path::point || l.light.shaping.kind != sv::light_shaping_kind::none)
                {
                    auto other = l.light;
                    other.exposure(interior_light_stops);
                    scene.add_light(l.id, other);
                    continue;
                }
                auto const bulb = l.light.placement.transform(tg::pos3f::zero);
                auto const u = tg::vec3f(pendant_half_size, 0, 0);
                auto const v = tg::vec3f(0, 0, pendant_half_size);
                auto lamp = sv::light::rect(bulb - tg::vec3f(0, pendant_drop, 0), u, v)
                                .lumens(2.0f * tg::pi<f32> * l.light.emission.intensity)
                                .color(l.light.emission.color)
                                .exposure(interior_light_stops);
                scene.add_light(l.id, lamp);
            }

            // Night outside the windows.
            scene.background(sv::background::gradient(tg::vec3f(0.02f, 0.03f, 0.07f), tg::vec3f(0.01f, 0.01f, 0.015f)));
        }
    }
}
} // namespace

EXAMPLE("shaped-viewer/bistro")
{
    run("shaped-viewer/bistro", scene_kind::exterior);
}

EXAMPLE("shaped-viewer/bistro-interior")
{
    run("shaped-viewer/bistro-interior", scene_kind::interior);
}
