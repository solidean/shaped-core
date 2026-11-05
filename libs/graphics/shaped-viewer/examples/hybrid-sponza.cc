#include <clean-core/record/log.hh>
#include <nexus/source_root.hh>
#include <nexus/test.hh>
#include <shaped-viewer/all.hh>

using namespace cc::primitive_defines;

// Intel's Sponza three times: path-traced, and through the hybrid lit forward and deferred, both denoised.
//
// The hybrid rasterizes the primary hits and traces their light inline: one light and one environment sample per pixel, and one bounce.
// So a moving hybrid frame is one sample per pixel, which its denoiser cleans, and a still one accumulates toward the tracer's first bounce.
// ../docs/plans/hybrid-renderer.md is the design.
//
// Sponza is fetched on request, never by a build: `uv run extern/sponza/fetch-sponza.py` installs it.
//
// Controls, per panel
//   left-drag    orbit          middle-drag    pan          wheel    zoom
//
// Run it:
//   uv run dev.py example shaped-viewer/hybrid-sponza

EXAMPLE("shaped-viewer/hybrid-sponza")
{
    auto const loader = sv::asset_loader();
    auto asset = loader.load_async(nx::source_path(SV_SPONZA_DIR "/NewSponza_Main_glTF_003.gltf"));
    auto reported = false;

    for (auto f : sv::interactive("shaped-viewer/hybrid-sponza"))
    {
        if (asset.has_error())
        {
            if (!reported)
                CC_LOG_ERROR("Sponza did not load ({}); uv run extern/sponza/fetch-sponza.py installs it", asset.error());
            reported = true;
            continue;
        }
        if (!asset.poll())
        {
            f.hold_capture("Sponza is still loading");
            continue;
        }

        auto columns = f.window().view().layout_columns({.padding = 6, .spacing = 6});
        auto const panel = [&](cc::string_view id, sv::render_settings const& settings)
        {
            auto view = columns.add_view(id);
            // Inside the nave at head height, looking along it.
            view.initial_orbit({.target = tg::pos3d(0, 1.7, 0),
                                .distance = 4.0,
                                .azimuth = tg::angle_d::make_from_degree(90.0),
                                .elevation = tg::angle_d::make_from_degree(5.0)});
            auto scene = view.add_scene();
            for (auto const& mesh : asset.meshes())
                scene.add_mesh(mesh);
            // Bright, since the nave is lit through its open roof: by the sky, and by the sun only where it falls in.
            scene.background(sv::background::studio().scaled(8.0f));
            scene.add_light("sun", sv::daylight().sun);
            scene.settings(settings);
        };

        panel("traced", {.mode = sv::render_mode::wavefront});
        // A moving frame is drawn at the upscaler's quality extent and upscaled, as the 60 fps budget wants.
        auto const reconstruct = sr::reconstruct_settings{.denoiser = sr::denoise_method::automatic,
                                                          .scale = sr::render_scale_preset::quality};
        panel("forward", {.mode = sv::render_mode::hybrid,
                          .hybrid_lighting = sv::hybrid_lighting::forward,
                          .reconstruct = reconstruct});
        panel("deferred", {.mode = sv::render_mode::hybrid,
                           .hybrid_lighting = sv::hybrid_lighting::deferred,
                           .reconstruct = reconstruct});
    }
}
