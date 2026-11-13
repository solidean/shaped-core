#include "agreement_test_support.hh"

#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/reconstruct.hh>
#include <shaped-viewer/all.hh>

#if SV_TEST_HAS_VULKAN
#include <shaped-graphics/backends/dx12/dx12_context.hh>     // sg::create_dx12_context, the reference beside vulkan
#include <shaped-graphics/backends/vulkan/vulkan_context.hh> // sg::create_vulkan_context
#endif

// Every mode and every denoiser drawn through the viewer on vulkan, held to the same view on dx12 hardware.
// One SGL source compiles to each API separately, so a mistake in one backend's generated code or its binding shows on that backend alone.
// `cross-backend-trace-test` holds the megakernel routine to rounding; this holds the views above it, the wavefront, the
// hybrid's lightings and the denoisers, which no other test draws on vulkan.
// Two converged images are held tile by tile, which needs neither API to draw the same samples.

#if SV_TEST_HAS_VULKAN
namespace
{
using namespace cc::primitive_defines;
namespace agreement = sv_test::agreement;

/// The lit box with a metal, a coated and a glass sphere on its floor, on `resources`' context.
[[nodiscard]] agreement::lit_box make_scene(sv::gpu_resource_manager& resources)
{
    auto box = agreement::make_lit_box(resources);
    auto& lib = sv_test::shared_material_library();
    using sv::material_attribute_binding;
    auto const pbr = lib.acquire_type(sv::builtin_material::pbr).value();
    auto const openpbr = lib.acquire_type(sv::builtin_material::openpbr).value();
    auto const metal = lib.acquire(sv::material::create(
        "backend metal", pbr,
        {material_attribute_binding::of("base_color", tg::vec3f(0.95f, 0.65f, 0.25f)),
         material_attribute_binding::of("metallic", 1.0f), material_attribute_binding::of("roughness", 0.35f)}));
    auto const coat = lib.acquire(sv::material::create(
        "backend coat", openpbr,
        {material_attribute_binding::of("base_color", tg::vec3f(0.6f, 0.1f, 0.1f)),
         material_attribute_binding::of("coat_weight", 1.0f), material_attribute_binding::of("coat_roughness", 0.1f)}));
    auto const glass = lib.acquire(sv::material::create("backend glass", openpbr,
                                                        {material_attribute_binding::of("transmission_weight", 1.0f),
                                                         material_attribute_binding::of("specular_roughness", 0.0f)}));
    auto const add = [&](cc::string_view name, sv::material_id material, tg::vec3f center)
    {
        box.items.push_back(
            resources.acquire_scene_item(sv::mesh{.name = cc::string(name),
                                                  .geometry = sv_test::sphere_geometry(0.22f, 24, 32, center),
                                                  .material = material}));
    };
    add("backend metal sphere", metal, tg::vec3f(-0.5f, -0.78f, -0.5f));
    add("backend coat sphere", coat, tg::vec3f(0.0f, -0.78f, -0.7f));
    add("backend glass sphere", glass, tg::vec3f(0.5f, -0.78f, -0.5f));
    resources.wait_for_pending_uploads();
    return box;
}

/// The largest difference between two images, relative to the brighter of each pair: rounding where both drew the same samples.
[[nodiscard]] f64 max_relative_difference(cc::span<tg::vec4f const> a, cc::span<tg::vec4f const> b)
{
    auto worst = 0.0;
    for (auto i = isize(0); i < a.size(); ++i)
        for (auto ch = 0; ch < 3; ++ch)
        {
            auto const scale = cc::max(cc::max(tg::abs(f64(a[i][ch])), tg::abs(f64(b[i][ch]))), 1e-3);
            worst = cc::max(worst, tg::abs(f64(a[i][ch]) - f64(b[i][ch])) / scale);
        }
    return worst;
}

/// Frames until `config`'s view holds a sample, so every pipeline it needs has compiled before anything is counted.
/// A cold forward stage compiles for longer on vulkan than `converge`'s frame cap, which a compile on dx12 fits in.
cc::shared_async<bool> warmed(sg::context& ctx,
                              sv::gpu_resource_manager& resources,
                              agreement::lit_box const& scene,
                              agreement::mode_config const& config)
{
    auto view = agreement::denoise_view(ctx, resources, scene.items, scene.camera, config);
    for (auto f = 0; f < 200000; ++f)
        if ((co_await view.frame()).samples > 0)
        {
            co_await cc::async_settled(sv::background_work(ctx));
            co_return true;
        }
    co_return false;
}

[[nodiscard]] f64 mean_luminance(cc::span<tg::vec4f const> image)
{
    auto total = 0.0;
    for (auto const& c : image)
        total += agreement::luminance_of(c);
    return total / f64(cc::max(image.size(), isize(1)));
}
} // namespace

ASYNC_TEST("sv vulkan - every mode and every denoiser draws the view it draws on dx12")
{
    auto vulkan = sg::create_vulkan_context({.enable_validation_layers = true});
    if (vulkan.has_error())
        SKIP("no vulkan device");
    if (!agreement::can_trace(*vulkan.value()))
        SKIP("no inline ray tracing on vulkan, or no SGL compiler");
    // The debug layer has to be active before the process's first dx12 device, and this test may race dx12-entry.cc's
    // drivers to it, so the reference asks for the layer exactly as they do.
    auto dx12 = sg::create_dx12_context(
        {.activate_global_debug_layer = true, .adapter = sg::backend::dx12::dx12_adapter::hardware});
    if (dx12.has_error() || !agreement::can_trace(*dx12.value()))
        SKIP("no dx12 hardware device that traces, to hold vulkan to");

    // The raster path applies its bindless tables' barriers at the first draw, inside the scope, which vulkan cannot.
    // issue: feat-sg-declare-scope-accesses-up-front
    nx::allow_warnings("a rendering scope*was closed and reopened around*");
    auto& vk_ctx = *vulkan.value();
    auto& dx_ctx = *dx12.value();
    auto vk_resources = sv::gpu_resource_manager::create(vk_ctx);
    auto dx_resources = sv::gpu_resource_manager::create(dx_ctx);
    auto const vk_scene = make_scene(vk_resources);
    auto const dx_scene = make_scene(dx_resources);

    agreement::mode_config const modes[] = {
        {.name = "megakernel", .mode = sv::render_mode::megakernel},
        {.name = "wavefront", .mode = sv::render_mode::wavefront},
        {.name = "forward", .mode = sv::render_mode::hybrid, .lighting = sv::hybrid_lighting::forward},
        {.name = "deferred", .mode = sv::render_mode::hybrid, .lighting = sv::hybrid_lighting::deferred},
    };
    for (auto const& m : modes)
    {
        auto const samples = m.mode == sv::render_mode::hybrid ? 256u : 512u;
        REQUIRE(co_await warmed(vk_ctx, vk_resources, vk_scene, m)).dump("mode", m.name).dump("on", "vulkan");
        REQUIRE(co_await warmed(dx_ctx, dx_resources, dx_scene, m)).dump("mode", m.name).dump("on", "dx12");
        auto const on_vulkan
            = co_await agreement::converge(vk_ctx, vk_resources, vk_scene.items, vk_scene.camera, m, samples);
        // Restarted past vulkan's counts, so where both APIs seed alike the two still draw independent samples.
        auto const on_dx12 = co_await agreement::converge(dx_ctx, dx_resources, dx_scene.items, dx_scene.camera, m,
                                                          samples, agreement::k_reference_restarts);
        CC_LOG_INFO("backend {}: largest relative difference of the two first halves {:.2e}", m.name,
                    max_relative_difference(on_vulkan.first, on_dx12.first));
        agreement::check_agreement(cc::format("backend-{}", m.name), on_vulkan, on_dx12, 0.01);
    }

    // A denoised image is one frame's, so it is held by its brightness and its distance from the converged image.
    auto const reference = co_await agreement::converge(
        dx_ctx, dx_resources, dx_scene.items, dx_scene.camera,
        {.name = "backend reference", .mode = sv::render_mode::megakernel, .max_bounces = 2}, 1024,
        agreement::k_reference_restarts);
    auto reference_mean = cc::vector<tg::vec4f>();
    for (auto i = isize(0); i < reference.first.size(); ++i)
        reference_mean.push_back(reference.mean_at(i));
    auto const exposure = agreement::exposure_of(reference_mean);
    auto const error_of = [&](cc::span<tg::vec4f const> image)
    {
        auto error = 0.0;
        for (auto i = isize(0); i < image.size(); ++i)
            error += tg::abs(agreement::luminance_of(image[i]) - agreement::luminance_of(reference_mean[i]));
        return error / (f64(image.size()) * mean_luminance(reference_mean));
    };

    auto const vk_support = sr::query_reconstruct_support(vk_ctx);
    auto const dx_support = sr::query_reconstruct_support(dx_ctx);
    sr::denoise_method const members[]
        = {sr::denoise_method::atrous, sr::denoise_method::svgf, sr::denoise_method::oidn, sr::denoise_method::dlss_rr};
    constexpr u32 denoised_at = 64;
    for (auto const member : members)
    {
        auto const name = cc::format("backend-denoised-{}", sr::to_string(member));
        if (!vk_support.supports(member) || !dx_support.supports(member))
        {
            CC_LOG_INFO("{}: not run, supported on vulkan {} and on dx12 {}", name, vk_support.supports(member),
                        dx_support.supports(member));
            continue;
        }
        auto images = cc::vector<cc::vector<tg::vec4f>>();
        for (auto const on_vulkan : {true, false})
        {
            auto& ctx = on_vulkan ? vk_ctx : dx_ctx;
            auto& resources = on_vulkan ? vk_resources : dx_resources;
            auto const& scene = on_vulkan ? vk_scene : dx_scene;
            auto view = agreement::denoise_view(ctx, resources, scene.items, scene.camera,
                                                {.name = name, .mode = sv::render_mode::hybrid, .denoiser = member});
            auto const warmed = co_await view.warm_up();
            CHECK(warmed).dump("test", cc::string_view(name)).dump("on vulkan", on_vulkan);
            auto shown = cc::vector<tg::vec4f>();
            for (auto f = 0; warmed && f < 4 * int(denoised_at) && shown.empty(); ++f)
            {
                auto const r = co_await view.frame();
                if (r.ran && !r.denoised.empty() && r.samples >= denoised_at)
                    shown = r.denoised;
            }
            REQUIRE(!shown.empty()).dump("test", cc::string_view(name)).dump("on vulkan", on_vulkan);
            images.push_back(cc::move(shown));
            co_await cc::async_settled(sv::background_work(ctx));
        }

        auto const vk_error = error_of(images[0]);
        auto const dx_error = error_of(images[1]);
        auto const brightness = mean_luminance(images[0]) / mean_luminance(images[1]);
        CC_LOG_INFO("{}: off the converged image by {:.1f}% on vulkan and {:.1f}% on dx12; vulkan {:.1f}% as bright; "
                    "largest relative difference {:.2e}",
                    name, 100.0 * vk_error, 100.0 * dx_error, 100.0 * brightness,
                    max_relative_difference(images[0], images[1]));
        cc::vector<tg::vec3f> const panels[]
            = {agreement::display_of(reference_mean, exposure), agreement::display_of(images[0], exposure),
               agreement::display_of(images[1], exposure)};
        agreement::write_panels(name, panels);

        CHECK(tg::abs(brightness - 1.0) <= 0.03).dump("test", cc::string_view(name)).dump("brightness", brightness);
        CHECK(vk_error <= dx_error * 1.25 + 0.01)
            .dump("test", cc::string_view(name))
            .dump("vulkan error", vk_error)
            .dump("dx12 error", dx_error);
    }
    co_await cc::async_settled(sv::background_work(vk_ctx));
    co_await cc::async_settled(sv::background_work(dx_ctx));
}
#endif
