#include <clean-core/platform/environment.hh>
#include <clean-core/record/log.hh>
#include <clean-core/string/format.hh>
#include <imgui/imgui.h>
#include <nexus/async-test.hh>
#include <shaped-viewer/all.hh>

using namespace cc::primitive_defines;

// The glTF Research Scenes, drawn by the hybrid at the window's full resolution.
//
// The objects are shown in studio mode: Philip's studio spec, a softbox dome whose key, fill and rim turn with the camera, a diffuse floor, a fixed exposure and the neutral curve.
// libs/graphics/shaped-viewer/docs/issues/feat-sv-studio-mode.md is the design.
// The rooms and test scenes keep their own staging and their own lights, seen from the file's camera at an automatic exposure.
// Four rooms carry no light at all, their original sky lost in the conversion, and are lit by a daylight sky and sun instead, which the panel says.
//
// The scenes are fetched on request, never by a build: `uv run extern/gltf-research-scenes/fetch-research-scenes.py` installs them.
// The extended files, since only they carry real glass: KHR_materials_transmission, rather than the core files' 35% coverage.
// A CC-BY-3.0 scene is shown with its author's credit, which its license asks for.
//
// Controls
//   left-drag    orbit          middle-drag    pan          wheel    zoom
//   the panel picks the scene, one of the six sheet views, and the renderer
//
// A capture takes its scene from SV_STUDIO_SCENE (an index), its renderer from SV_STUDIO_MODE (0 deferred, 1 forward, 2 traced, 3 automatic)
// its sheet view from SV_STUDIO_VIEW (0 front to 5 top, as the panel lists them), its still-image denoise from SV_STUDIO_STILL (an index into k_stills),
// and the hybrid's bounces from SV_STUDIO_BOUNCES (1 or 2).
//
// Run it:
//   uv run dev.py example shaped-viewer/research-studio
// On the web, build the emscripten-webgpu preset and open web/research-studio.html, served with the installed scenes
// beside it as `scenes/`: the bound raster mode draws it there, without the renderer and denoiser choices.

namespace
{
/// How a scene is lit.
enum class lighting : u8
{
    studio,   ///< studio mode around the object alone, its staging left out
    own,      ///< the scene's own emitters, from the file's camera, at an automatic exposure
    daylight, ///< as `own`, under a daylight sky and sun standing in for the light the file lost
};

struct scene_entry
{
    char const* name;
    char const* file;   ///< under the install directory
    char const* credit; ///< empty for a CC0 scene
    lighting light;

    /// The meshes each scene was staged with for its own renders: floors, backdrops, bounce cards and the lights over them.
    /// Studio mode brings its own, so these are left out, and the framing sees the object alone.
    cc::span<char const* const> staging;

    /// The orbit azimuth the object's front is seen from, in degrees: the cars run along x with their headlights at -x, and the dragon faces +z.
    f64 front_azimuth;
};

constexpr char const* k_bmw_staging[] = {"105_floor"};
constexpr char const* k_pontiac_staging[] = {"04_Ground"};
constexpr char const* k_dragon_staging[] = {"03_GroundOuter"};
constexpr char const* k_coffee_staging[] = {"19_Floor", "20_diffuse_00", "21_diffuse_00", "22_diffuse_00"};
constexpr char const* k_lamp_staging[] = {"00_BounceCard", "01_Emitter", "02_Backdrop"};
constexpr char const* k_spaceship_staging[] = {"00_Backdrop", "90_Backdrop", "88_diffuse_00", "89_diffuse_00"};

constexpr char const* k_cc_by = ", CC BY 3.0 (creativecommons.org/licenses/by/3.0)";

constexpr scene_entry k_scenes[] = {
    {"BMW M6", "bmw-m6/bmw_m6_extended.glb", "", lighting::studio, k_bmw_staging, -90.0},
    {"Pontiac GTO 67", "pontiac-gto/pontiac_gto_extended.glb", "", lighting::studio, k_pontiac_staging, -90.0},
    {"Dragon", "dragon/dragon_extended.glb", "'Dragon' by Delatronic", lighting::studio, k_dragon_staging, 180.0},
    {"Coffee Maker", "coffee-maker/coffee_maker_extended.glb", "'Coffee Maker' by cekuhnen", lighting::studio,
     k_coffee_staging, 0.0},
    {"Little Lamp", "lamp/lamp_extended.glb", "", lighting::studio, k_lamp_staging, 0.0},
    {"Spaceship", "spaceship/spaceship_extended.glb", "", lighting::studio, k_spaceship_staging, 0.0},
    {"Bathroom", "bathroom/bathroom_extended.glb", "", lighting::own, {}, 0.0},
    {"Salle de bain", "bathroom2/bathroom2_extended.glb", "'Salle de bain' by nacimus", lighting::own, {}, 0.0},
    {"Breakfast Room",
     "breakfast-room/breakfast_room_extended.glb",
     "'The Breakfast Room' by Wig42",
     lighting::daylight,
     {},
     0.0},
    {"Japanese Classroom", "classroom/classroom_extended.glb", "'Japanese Classroom' by NovaZeeke", lighting::daylight, {}, 0.0},
    {"Cornell Box", "cornell-box/cornell_box_extended.glb", "", lighting::own, {}, 0.0},
    {"Water Caustic", "cornell-caustic/cornell_caustic_extended.glb", "", lighting::own, {}, 0.0},
    {"Glass of Water", "glass-of-water/glass_of_water_extended.glb", "", lighting::own, {}, 0.0},
    {"Victorian House", "house/house_extended.glb", "", lighting::daylight, {}, 0.0},
    {"Country Kitchen", "kitchen/kitchen_extended.glb", "'Country Kitchen' by Jay-Artist", lighting::own, {}, 0.0},
    {"Grey & White Room",
     "living-room/living_room_extended.glb",
     "'The Grey & White Room' by Wig42",
     lighting::daylight,
     {},
     0.0},
    {"White Room", "living-room-2/living_room_2_extended.glb", "'The White Room' by Jay-Artist", lighting::own, {}, 0.0},
    {"Modern Hall", "modern-hall/modern_hall_extended.glb", "'Modern Hall' by NewSee2l035", lighting::own, {}, 0.0},
    {"Wooden Staircase", "staircase/staircase_extended.glb", "'The Wooden Staircase' by Wig42", lighting::own, {}, 0.0},
    {"Veach, Ajar", "veach-ajar/veach_ajar_extended.glb", "", lighting::own, {}, 0.0},
    {"Veach, Bidir Room", "veach-bidir/veach_bidir_extended.glb", "", lighting::own, {}, 0.0},
    {"Veach, MIS", "veach-mis/veach_mis_extended.glb", "", lighting::own, {}, 0.0},
};
constexpr int k_scene_count = int(sizeof(k_scenes) / sizeof(k_scenes[0]));

constexpr char const* k_view_names[] = {"front", "front right", "right", "back", "front left", "top"};

/// Every denoiser sr has, the ones this machine cannot run shown and greyed out.
/// A temporal one names the temporal phase alone, and the still image's spatial phase stays `automatic`'s.
constexpr sr::denoise_method k_denoisers[] = {
    sr::denoise_method::automatic, sr::denoise_method::dlss_rr, sr::denoise_method::nrd,  sr::denoise_method::fsr_rr,
    sr::denoise_method::svgf,      sr::denoise_method::atrous,  sr::denoise_method::oidn, sr::denoise_method::none,
};

/// How much smaller than the window a moving image is traced, and upscaled by the temporal member; a still one is always traced at full size.
constexpr sr::render_scale_preset k_scales[] = {sr::render_scale_preset::native, sr::render_scale_preset::quality,
                                                sr::render_scale_preset::balanced, sr::render_scale_preset::performance};
constexpr char const* k_scale_names[] = {"native", "quality", "balanced", "performance"};

/// What denoises the image once the camera stops: the temporal member on each frame forever, or one of sv's still-image modes.
struct still_choice
{
    char const* name;
    bool temporal_forever;
    sv::still_denoise mode;
};
constexpr still_choice k_stills[] = {
    {"temporal, each frame, forever", true, sv::still_denoise::spatial},
    {"temporal, on the mean", false, sv::still_denoise::temporal_on_mean},
    {"OIDN on the mean, at doublings", false, sv::still_denoise::oidn},
    {"spatial, each frame", false, sv::still_denoise::spatial},
};
constexpr int k_still_count = int(sizeof(k_stills) / sizeof(k_stills[0]));

[[nodiscard]] bool is_staging(scene_entry const& entry, sv::mesh const& m)
{
    for (auto const* name : entry.staging)
        if (cc::string_view(m.name) == cc::string_view(name))
            return true;
    return false;
}

/// The world-space box around every mesh of `asset` but its staging, from each mesh's own box through its transform.
[[nodiscard]] tg::aabb3f bounds_of(scene_entry const& entry, sv::asset const& asset)
{
    auto lo = tg::pos3f(1e30f, 1e30f, 1e30f);
    auto hi = tg::pos3f(-1e30f, -1e30f, -1e30f);
    for (auto const& m : asset.meshes())
    {
        if (!m.bounds.has_value() || is_staging(entry, m))
            continue;
        auto const b = m.bounds.value();
        for (auto corner = 0; corner < 8; ++corner)
        {
            auto const local = tg::pos3f(corner & 1 ? b.max[0] : b.min[0], corner & 2 ? b.max[1] : b.min[1],
                                         corner & 4 ? b.max[2] : b.min[2]);
            auto const p = m.transform.transform(local);
            for (auto i = 0; i < 3; ++i)
            {
                lo[i] = p[i] < lo[i] ? p[i] : lo[i];
                hi[i] = p[i] > hi[i] ? p[i] : hi[i];
            }
        }
    }
    if (lo[0] > hi[0])
        return tg::aabb3f(tg::pos3f(-1, 0, -1), tg::pos3f(1, 2, 1));
    return tg::aabb3f(lo, hi);
}

/// The orbit `studio_camera` frames `bounds` from, so the user orbits on from the sheet view.
[[nodiscard]] sv::orbit_state orbit_of(tg::aabb3f const& bounds, sv::studio_view view, f64 front_azimuth)
{
    auto const cam = sv::studio_camera(bounds, view, 16.0 / 9.0, tg::angle_d::make_from_degree(front_azimuth));
    auto const center
        = tg::pos3d(0.5 * (f64(bounds.min[0]) + f64(bounds.max[0])), 0.5 * (f64(bounds.min[1]) + f64(bounds.max[1])),
                    0.5 * (f64(bounds.min[2]) + f64(bounds.max[2])));
    auto const offset = cam.position - center;
    auto const distance = tg::sqrt(tg::dot(offset, offset));
    auto const azimuth = [&]
    {
        switch (view)
        {
        case sv::studio_view::front_right:
            return 45.0;
        case sv::studio_view::right:
            return 90.0;
        case sv::studio_view::back:
            return 180.0;
        case sv::studio_view::front_left:
            return -45.0;
        default:
            return 0.0;
        }
    }();
    // The orbit cannot look straight down, so the top view stops just short of it.
    auto const elevation = view == sv::studio_view::top ? 89.0 : 10.0;
    return {.target = center,
            .distance = distance,
            .azimuth = tg::angle_d::make_from_degree(front_azimuth + azimuth),
            .elevation = tg::angle_d::make_from_degree(elevation),
            .vertical_fov = cam.projection.vertical_fov};
}
} // namespace

ASYNC_EXAMPLE("shaped-viewer/research-studio")
{
    // The research scenes leave the volume off solid glass, so a closed glass mesh is read as the solid it is.
    auto const loader = sv::asset_loader({.closed_transmission_is_solid = true});
    cc::optional<sv::asset> assets[k_scene_count] = {};
    tg::aabb3f bounds[k_scene_count] = {};
    bool framed[k_scene_count] = {};

    auto const from_environment = [](cc::string_view name, int fallback, int count)
    {
        auto const v = cc::environment_variable(name);
        if (!v.has_value() || v.value().empty())
            return fallback;
        auto i = 0;
        for (auto const c : cc::string_view(v.value()))
        {
            if (c < '0' || c > '9')
                return fallback;
            i = i * 10 + int(c - '0');
        }
        return i < count ? i : fallback;
    };
    auto scene_index = from_environment("SV_STUDIO_SCENE", 0, k_scene_count);
    auto view_index = from_environment("SV_STUDIO_VIEW", 0, 6);
    auto reframe = true;
    auto mode
        = from_environment("SV_STUDIO_MODE", 0, 4); // 0 hybrid deferred, 1 hybrid forward, 2 path traced, 3 automatic
    auto bounces = from_environment("SV_STUDIO_BOUNCES", 0, 3); // render_settings::hybrid_bounces, 0 for its default
    if (bounces == 0)
        bounces = sv::render_settings{}.hybrid_bounces;
    auto moving_samples = sv::render_settings{}.hybrid_moving_samples;
    auto denoiser = 0;         // into k_denoisers
    auto temporal_frames = 16; // render_settings::temporal_denoise_frames
    // On unless asked otherwise: the spatial member a still image is otherwise handed to is far noisier than DLSS.
    auto still = from_environment("SV_STUDIO_STILL", 0, k_still_count); // into k_stills
    auto scale = 0;                                                     // into k_scales
    // FSR after the temporal member at the native scale too, as anti-aliasing; below native it upscales either way.
    auto fsr = false;
    auto fade_frames = 8; // render_settings::temporal_denoise_fade_frames
    auto ceiling = false;
    auto exposure_offset = 0.0f;
    auto restart = false; // the panel asked for the image to converge again
    auto samples = u32(0);
    // Each a quadrature over the dome, so taken once rather than every frame.
    auto const floor_exposure = sv::studio_exposure(sv::studio_dome(sv::studio_setup::floor));
    auto const ceiling_exposure = sv::studio_exposure(sv::studio_dome(sv::studio_setup::ceiling));

    // A scene left behind gives its GPU resources back within a few frames: a browser's acceleration pool holds one
    // large scene, not several.
    auto const released = sv::manager_config{.budget = {.max_idle_epochs = 4}};
    auto frames = sv::interactive_async(
        "shaped-viewer/research-studio",
        {.resources = {.meshes = released, .quadrics = released, .textures = released, .attributes = released}});
    while (auto* f = co_await frames.next())
    {
        // The panel first: a scene picked there is the one this frame loads, frames and draws.
        if (f->imgui())
        {
            ImGui::Begin("studio");
            if (ImGui::BeginCombo("scene", k_scenes[scene_index].name))
            {
                for (auto i = 0; i < k_scene_count; ++i)
                    if (ImGui::Selectable(k_scenes[i].name, i == scene_index))
                    {
                        scene_index = i;
                        reframe = true;
                    }
                ImGui::EndCombo();
            }
            for (auto i = 0; i < 6; ++i)
            {
                if (i > 0 && i % 3 != 0)
                    ImGui::SameLine();
                if (ImGui::Button(k_view_names[i]))
                {
                    view_index = i;
                    reframe = true;
                }
            }
#ifndef __EMSCRIPTEN__
            // A page has no ray-tracing pipeline, so it draws in the bound raster mode and none of these apply there.
            ImGui::RadioButton("hybrid, deferred", &mode, 0);
            ImGui::RadioButton("hybrid, forward", &mode, 1);
            ImGui::RadioButton("path traced", &mode, 2);
            // What the device picks, `SV_AUTOMATIC_MODE` included: how the bound raster mode a page draws in is seen here.
            ImGui::RadioButton("automatic", &mode, 3);
            if (mode != 2)
            {
                ImGui::SliderInt("hybrid bounces", &bounces, 1, 2);
                ImGui::SliderInt("moving samples", &moving_samples, 1, 4);
            }
            if (ImGui::BeginCombo("denoiser", sr::to_string(k_denoisers[denoiser]).data()))
            {
                // Asked while the list is open rather than once up front: sr's shaders answer only once a viewer has
                // added them to its library, so before the first frame every member built from them reads as missing.
                auto support = sr::reconstruct_support{};
                if (auto const ctx = sv::acquire_viewer_context(); ctx.has_value() && ctx.value() != nullptr)
                    support = sr::query_reconstruct_support(*ctx.value());
                for (auto i = 0; i < int(sizeof(k_denoisers) / sizeof(k_denoisers[0])); ++i)
                {
                    auto const m = k_denoisers[i];
                    auto const runs
                        = m == sr::denoise_method::automatic || m == sr::denoise_method::none || support.supports(m);
                    auto label = runs ? cc::string(sr::to_string(m))
                                      : cc::format("{} (not on this machine)", sr::to_string(m));
                    if (ImGui::Selectable(label.c_str_materialize(), i == denoiser,
                                          runs ? 0 : ImGuiSelectableFlags_Disabled))
                        denoiser = i;
                }
                ImGui::EndCombo();
            }
            if (ImGui::BeginCombo("moving scale", k_scale_names[scale]))
            {
                for (auto i = 0; i < 4; ++i)
                    if (ImGui::Selectable(k_scale_names[i], i == scale))
                        scale = i;
                ImGui::EndCombo();
            }
            if (mode != 2)
                ImGui::Checkbox("FSR anti-aliasing at native", &fsr);
            if (ImGui::BeginCombo("still image", k_stills[still].name))
            {
                for (auto i = 0; i < k_still_count; ++i)
                    if (ImGui::Selectable(k_stills[i].name, i == still))
                        still = i;
                ImGui::EndCombo();
            }
            // How long after the camera stops the temporal member keeps each frame's own samples, before the still-image mode takes over.
            if (!k_stills[still].temporal_forever)
            {
                ImGui::SliderInt("temporal frames", &temporal_frames, 0, 256);
                ImGui::SliderInt("fade frames", &fade_frames, 0, 64);
            }
#endif
            ImGui::Checkbox("ceiling setup", &ceiling);
            ImGui::SliderFloat("exposure offset", &exposure_offset, -3.0f, 3.0f, "%.1f stops");
            restart = ImGui::Button("reset accumulation");
            ImGui::SameLine();
            ImGui::Text("%u samples", samples);
            ImGui::Text("%.1f ms / frame", 1000.0f / ImGui::GetIO().Framerate);
            if (k_scenes[scene_index].credit[0] != '\0')
                ImGui::TextWrapped("%s%s", k_scenes[scene_index].credit, k_cc_by);
            if (k_scenes[scene_index].light == lighting::daylight)
                ImGui::TextWrapped("The file carries no light: a daylight sky and sun stand in for the original's.");
            if (k_scenes[scene_index].light != lighting::studio)
                ImGui::TextWrapped("Exposure: automatic, the offset compensating it.");
            ImGui::End();
        }

        // Only the scene shown stays loaded, so its resources are the only ones in use.
        for (auto i = 0; i < k_scene_count; ++i)
            if (i != scene_index)
                assets[i] = cc::nullopt;

        auto const& entry = k_scenes[scene_index];
        if (!assets[scene_index].has_value())
#ifdef __EMSCRIPTEN__
            // Fetched relative to the page, which the dev server serves the installed scenes beside as `scenes/`.
            assets[scene_index] = loader.load_async(cc::format("scenes/{}", entry.file));
#else
            assets[scene_index] = loader.load_async(cc::format("{}/{}", SV_RESEARCH_SCENES_DIR, entry.file));
#endif
        auto& asset = assets[scene_index].value();

        if (asset.has_error())
        {
            CC_LOG_ERROR("{} did not load ({}); uv run extern/gltf-research-scenes/fetch-research-scenes.py installs "
                         "it",
                         entry.name, asset.error());
            assets[scene_index] = cc::nullopt;
            scene_index = (scene_index + 1) % k_scene_count;
            continue;
        }
        if (!asset.poll())
        {
            f->hold_capture("the scene is still loading");
            continue;
        }
        if (!framed[scene_index])
        {
            // Nothing is staging in a scene shown as its author lit it, so the box is the whole scene.
            bounds[scene_index] = bounds_of(entry, asset);
            framed[scene_index] = true;
        }

        auto view = f->window().view();
        if (restart)
            view.restart_accumulation();
        samples = view.accumulated_samples();
        auto const& cameras = asset.data().cameras;
        if (reframe)
        {
            // A scene with its own staging starts where its author's camera stood, orbiting the middle of what it frames.
            if (entry.light != lighting::studio && !cameras.empty())
            {
                auto const& cam = cameras[0].camera;
                auto const& b = bounds[scene_index];
                auto const middle
                    = tg::pos3d(0.5 * (f64(b.min[0]) + f64(b.max[0])), 0.5 * (f64(b.min[1]) + f64(b.max[1])),
                                0.5 * (f64(b.min[2]) + f64(b.max[2])));
                auto const along = tg::dot(middle - cam.position, cam.basis().forward);
                auto orbit = sv::orbit_state::from_camera(cam, along > 0.1 ? along : 1.0);
                orbit.vertical_fov = cam.projection.vertical_fov;
                view.reset_orbit(orbit);
            }
            else
                view.reset_orbit(orbit_of(bounds[scene_index], sv::studio_view(view_index), entry.front_azimuth));
            reframe = false;
        }

        auto scene = view.add_scene();
        for (auto const& mesh : asset.meshes())
            if (!is_staging(entry, mesh))
                scene.add_mesh(mesh);
        for (auto const& l : asset.data().lights)
            scene.add_light(l.id, l.light);

        // Below native, a moving image is traced smaller and upscaled by the temporal member; a still one restarts at full size.
        auto const reconstruct
            = sr::reconstruct_settings{.denoiser = k_denoisers[denoiser],
                                       .upscaler = fsr ? sr::upscale_method::fsr : sr::upscale_method::automatic,
                                       .scale = k_scales[scale]};
        auto const temporal = k_stills[still].temporal_forever ? u32(-1) : u32(temporal_frames);
        auto const scene_settings = sv::render_settings{
#ifdef __EMSCRIPTEN__
            .mode = sv::render_mode::automatic,
#else
            .mode = mode == 3 ? sv::render_mode::automatic
                  : mode == 2 ? sv::render_mode::wavefront
                              : sv::render_mode::hybrid,
#endif
            .hybrid_lighting = mode == 1 ? sv::hybrid_lighting::forward : sv::hybrid_lighting::deferred,
            .hybrid_bounces = bounces,
            .hybrid_moving_samples = moving_samples,
            .reconstruct = reconstruct,
            .temporal_denoise_frames = temporal,
            .temporal_denoise_fade_frames = u32(fade_frames),
            .still_denoise = k_stills[still].mode};
        scene.settings(scene_settings);

        if (entry.light == lighting::studio)
        {
            auto const setup = ceiling ? sv::studio_setup::ceiling : sv::studio_setup::floor;
            auto const exposure = (ceiling ? ceiling_exposure : floor_exposure) + exposure_offset;
            scene.studio({.setup = setup, .exposure = exposure, .floor_height = bounds[scene_index].min[1]});
            continue;
        }

        // Its own light, or daylight where the file lost it; either way metered, since the files' emitters span five orders of magnitude.
        if (entry.light == lighting::daylight)
        {
            auto const day = sv::daylight();
            scene.background(day.sky);
            scene.add_light("sun", day.sun);
        }
        scene.fallback_light(cc::nullopt);
        auto settings = scene_settings;
        settings.display = {.exposure = exposure_offset, .curve = sr::display_curve::neutral, .automatic = true};
        scene.settings(settings);
    }
}
