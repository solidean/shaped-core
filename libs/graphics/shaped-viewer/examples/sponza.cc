#include <clean-core/record/log.hh> // CC_LOG_ERROR, CC_LOG_INFO
#include <nexus/test.hh>
#include <shaped-viewer/all.hh>

using namespace cc::primitive_defines;

// Intel's Sponza, loaded from its glTF and path-traced.
//
// A production asset rather than one built for the viewer: 3.7 million triangles in 405 meshes over 28 materials, and
// 72 textures at 4096x4096 — what the importer, the texture streaming and the tracer are measured by.
// It is NOT in the repository.
// `uv run extern/sponza/fetch-sponza.py` downloads it once (about 4 GB), and without it this example says so and stops.
//
// The load runs off this thread: `load_async` parses and imports on the viewer's workers, and the frame loop polls it.
// Until it lands the view shows the sky; once it has, the meshes arrive as their uploads stream, and a texture still in
// flight is sampled as a 1x1 of its material's factor.
// A capture would otherwise converge on that empty sky, so every frame before the load lands holds it.
//
// Two examples share this file, and differ only in their light.
// `sponza` is daylight: `sv::daylight`'s sky and a sun, aimed the way the file's own `SUN` node points.
// `sponza-evening` is a dusk sky with the atrium's 22 lanterns lit, and their bulbs glowing.
// The file places those lanterns as punctual lights but gives every one an intensity of 0, which is why their brightness
// is this example's own; only their positions and their warm color come from the file.
// Both are raised by `exposure_stops`: from inside the atrium only a sliver of sky is visible.
//
// Controls
//   right-drag   look           W/A/S/D   move along the view         E / Q   rise and fall
//   shift        faster         ctrl      slower                      wheel   retune the base speed
//
// Run it:
//   uv run extern/sponza/fetch-sponza.py
//   uv run dev.py example shaped-viewer/sponza
//   uv run dev.py example shaped-viewer/sponza-evening

namespace
{
/// How far both lightings are raised over the scales they are written in, in stops.
constexpr float exposure_stops = 4.0f;

/// What one lantern emits, in candela, before the exposure.
constexpr float lantern_candela = 1.0f;

/// How bright a lantern's bulb glows, in nits, before the exposure.
constexpr float bulb_nits = 10.0f;


enum class lighting
{
    daylight,
    evening,
};

/// The meshes as placed: the file's own, with the lanterns made fit to be lit in the evening.
///
/// A per-mesh value wins over the material's factor, so these changes leave the file's materials alone.
/// The panes are taken away through `opacity`: the file's lantern glass is an opaque gray dielectric — the export dropped
/// its transmission — so it would box every lantern's light in.
/// The bulbs emit and reflect nothing, through `base_weight` and `specular_weight`.
/// A surface a few centimeters from a point light receives hundreds of times what a wall does, so a bulb or a pane that
/// reflected it would glow white and spray fireflies over every surface that sees it.
cc::vector<sv::mesh> placed_meshes(sv::asset const& sponza, lighting light)
{
    auto meshes = cc::vector<sv::mesh>();
    auto const bulb = sponza.data().material("light_bulb");
    auto const pane = sponza.data().material("lamp_glass_01");
    for (auto const& mesh : sponza.meshes())
    {
        meshes.push_back(mesh);
        if (light != lighting::evening)
            continue;
        auto& attributes = meshes.back().attributes;
        if (mesh.material == bulb)
        {
            attributes.push_back(
                sv::mesh_attribute::create_value("emission_luminance", bulb_nits * tg::pow(2.0f, exposure_stops)));
            attributes.push_back(sv::mesh_attribute::create_value("base_weight", 0.0f));
            attributes.push_back(sv::mesh_attribute::create_value("specular_weight", 0.0f));
        }
        if (mesh.material == pane)
            attributes.push_back(sv::mesh_attribute::create_value("opacity", 0.0f));
    }
    return meshes;
}

void run(cc::string_view id, lighting light)
{
    // Held where it will not move: a load in flight reaches back into its loader.
    auto const loader = sv::asset_loader();
    auto sponza = loader.load_async(SV_SPONZA_GLTF);

    auto meshes = cc::vector<sv::mesh>();
    auto const exposure = tg::pow(2.0f, exposure_stops);
    auto const daylight = sv::daylight();
    CC_LOG_INFO("sponza: loading {}", SV_SPONZA_GLTF);

    for (auto f : sv::interactive(id))
    {
        sponza.poll();
        if (sponza.has_error())
        {
            CC_LOG_ERROR("sponza: could not load {}: {}", SV_SPONZA_GLTF, sponza.error());
            CC_LOG_ERROR("sponza: fetch the scene with `uv run extern/sponza/fetch-sponza.py`");
            return;
        }

        if (sponza.is_ready() && meshes.empty())
        {
            meshes = placed_meshes(sponza, light);
            CC_LOG_INFO("sponza: {} meshes, {} materials", sponza.meshes().size(), sponza.data().materials.size());
            // A real asset loads with issues as a matter of course, so they are information rather than warnings.
            // They say what was skipped or did not decode — the file's six cameras, for one.
            for (auto const& issue : sponza.issues())
                CC_LOG_INFO("sponza: {}", issue);
        }

        auto view = f.window().view();

        // Where the walk starts, applied the first time this view id is seen.
        // The file's `PhysCamera001`, at its own field of view: on the atrium floor at its west end, looking down its length.
        view.initial_fps({.position = tg::pos3d(-8.81, 1.59, -0.86),
                          .yaw = tg::angle_d::make_from_degree(79.3),
                          .pitch = tg::angle_d::make_from_degree(11.2),
                          .vertical_fov = tg::angle_d::make_from_degree(58.5)});
        view.camera_style(sv::camera_style::fly);

        // Until the load lands the view shows only the sky, and a capture would converge on that.
        if (!sponza.is_ready())
            f.hold_capture("Sponza is still loading");

        auto scene = view.add_scene();

        // A moving camera traces a quarter of the pixels and upscales them; a still one converges at full size.
        scene.settings({.reconstruct
                        = {.denoiser = sr::denoise_method::automatic, .scale = sr::render_scale_preset::performance}});

        // Empty until the load lands, so the loop needs no branch.
        // Re-placing them every frame uploads nothing after the first: every payload is keyed by its content hash.
        for (auto const& mesh : meshes)
            scene.add_mesh(mesh);

        if (light == lighting::daylight)
        {
            // The file's directional light is its sun, so its placement aims ours.
            auto sun = daylight.sun;
            sun.exposure(exposure_stops);
            for (auto const& l : sponza.data().lights)
                if (l.light.path() == sv::light_path::distant_point)
                    sun.placement = l.light.placement;

            scene.background(daylight.sky.scaled(exposure));
            scene.add_light("sun", sun);
        }
        else
        {
            // The lanterns are the lights of the file's `lamp_light_*` nodes.
            // The lights themselves are unnamed, so the node is what says which is which; the `HDRI_SKY` node's point
            // marks where an environment was, and is no light at all.
            auto const& data = sponza.data();
            for (auto const& node : data.nodes)
            {
                if (!node.name.starts_with("lamp_light"))
                    continue;
                for (auto i = node.first_light; i < node.first_light + node.light_count; ++i)
                {
                    auto lantern = data.lights[i].light;
                    lantern.candela(lantern_candela).exposure(exposure_stops);
                    scene.add_light(data.lights[i].id, lantern);
                }
            }

            scene.background(
                sv::background::gradient(tg::vec3f(0.05f, 0.07f, 0.16f), tg::vec3f(0.02f, 0.02f, 0.03f)).scaled(exposure));
        }
    }
}
} // namespace

EXAMPLE("shaped-viewer/sponza")
{
    run("shaped-viewer/sponza", lighting::daylight);
}

EXAMPLE("shaped-viewer/sponza-evening")
{
    run("shaped-viewer/sponza-evening", lighting::evening);
}
