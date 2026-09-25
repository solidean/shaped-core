// sr's denoising, with every knob on screen.
//
// A small path tracer draws three spheres on a checkered floor and hands `sr::denoise_routine` the noisy result plus
// the guides it wrote — albedo, normal, depth, motion.
// The panel switches the member, the quality, the sharpness and the guides while it runs, so the difference between
// two settings is something you look at rather than something you remember between two runs.
//
// The two halves of libs/graphics/shaped-rendering/docs/denoising.md's "meeting a progressive path tracer":
//
//   fresh samples OFF — the tracer keeps a running mean, `sample_count` climbs, and a SPATIAL member backs off as it
//   converges.
//   The image stops being touched as it converges, which is what lets a denoiser sit on a mean at all.
//
//   fresh samples ON — every frame is its own low-sample estimate with motion vectors, and a TEMPORAL member (svgf)
//   reprojects its history along them.
//   Turn the orbit on and watch it hold together at one sample a pixel.
//
// Run it with `uv run dev.py example shaped-rendering/denoise-playground`.

#include <clean-core/common/time.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <imgui/imgui.h>
#include <nexus/async-test.hh>
#include <scene_shaders.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/blit_routine.hh>
#include <shaped-rendering/capture.hh>
#include <shaped-rendering/denoise.hh>
#include <shaped-rendering/imgui_context.hh>
#include <shaped-rendering/imgui_routine.hh>
#include <shaped-rendering/shaders.hh>
#include <shaped-rendering/window.hh>
#include <shaped-shader-library/compiler/dxc_compiler.hh>
#include <shaped-shader-library/shader_library.hh>
#include <typed-geometry/linalg/cross.hh>
#include <typed-geometry/linalg/vec_ops.hh>
#include <typed-geometry/scalar/angle.hh>
#include <typed-geometry/scalar/scalar.hh>

#if DENOISE_PLAYGROUND_BACKEND_DX12
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#else
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#endif

using namespace cc::primitive_defines;

namespace
{
constexpr auto k_scene_format = sg::pixel_format::rgba32_float;

/// Everything the panel can change, in one struct so "did anything change" is one comparison.
///
/// That comparison is what restarts the accumulated mean: every field below changes what the tracer or the denoiser
/// produces, so carrying a mean across a change to one of them would be averaging two different images together.
struct controls
{
    /// `fresh_samples` on by default, which is what opens the example on the interesting half: one sample a pixel,
    /// permanently noisy on the left of the split, and svgf holding it together on the right.
    /// Turning it off switches to the accumulating half, where the mean converges and a spatial member backs off.
    sr::denoise_settings denoise = {.fresh_samples = true};
    bool denoise_enabled = true;
    int spp = 1;
    float light_size = 0.6f;
    bool orbiting = false;
    bool use_albedo = true;
    bool use_normal = true;
    bool use_depth = true;
    bool show_split = true;
    float split = 0.5f;
    float exposure = 1.0f;

    [[nodiscard]] bool restarts_against(controls const& o) const
    {
        return denoise_enabled != o.denoise_enabled || spp != o.spp || light_size != o.light_size
            || use_albedo != o.use_albedo || use_normal != o.use_normal || use_depth != o.use_depth
            || denoise.method != o.denoise.method || denoise.quality != o.denoise.quality
            || denoise.sharpness != o.denoise.sharpness || denoise.fresh_samples != o.denoise.fresh_samples
            || denoise.temporal_responsiveness != o.denoise.temporal_responsiveness;
    }
};

/// The images one view holds: the tracer's output and its guides, plus the denoiser's target.
/// Recreated on a resize, which is also one of the things that restarts the mean.
struct view_images
{
    tg::vec2i extent = tg::vec2i(0, 0);
    sg::texture_2d color;
    sg::texture_2d albedo;
    sg::texture_2d normal;
    sg::texture_2d depth;
    sg::texture_2d motion;
    sg::texture_2d denoised;
    sg::texture_2d composed;
};

[[nodiscard]] sg::texture_2d make_image(sg::context& ctx, tg::vec2i extent)
{
    return ctx.persistent.create_texture_2d({.format = k_scene_format,
                                             .width = extent[0],
                                             .height = extent[1],
                                             .usage = sg::texture_usage::texture | sg::texture_usage::image});
}

void resize_images(sg::context& ctx, view_images& v, tg::vec2i extent)
{
    v.extent = extent;
    v.color = make_image(ctx, extent);
    v.albedo = make_image(ctx, extent);
    v.normal = make_image(ctx, extent);
    v.depth = make_image(ctx, extent);
    v.motion = make_image(ctx, extent);
    v.denoised = make_image(ctx, extent);
    v.composed = make_image(ctx, extent);
}

/// The classic 3-vector cross product.
/// tg's `cross` is the wedge and returns a bivector, so the vector form is its Hodge dual.
[[nodiscard]] tg::vec3f cross3(tg::vec3f a, tg::vec3f b)
{
    return tg::dual(tg::cross(a, b));
}

// TODO(typed-geometry): perspective / look_at belong in tg's transform module, which does not carry them yet.
// The convention is pinned here, as it is in examples/vdoc/cube-editor/camera.hh: left-handed, z into [0, 1].
// tg::mat is COLUMN-major and subscripts m[col, row], which is what the row gather below is about.

/// An orbiting pinhole camera, plus the view-projection that says where its world points were last frame.
struct camera
{
    tg::angle_f azimuth = tg::angle_f::make_from_degree(52.0f);
    tg::angle_f elevation = tg::angle_f::make_from_degree(22.0f);
    float distance = 7.0f;
    tg::pos3f target = tg::pos3f(0, 0.85f, 0);
    tg::angle_f vertical_fov = tg::angle_f::make_from_degree(42.0f);

    [[nodiscard]] tg::pos3f eye() const
    {
        auto const cos_e = tg::cos(elevation);
        auto const dir = tg::vec3f(cos_e * tg::sin(azimuth), tg::sin(elevation), cos_e * tg::cos(azimuth));
        return target + dir * distance;
    }

    [[nodiscard]] tg::vec3f forward() const { return tg::normalize(target - this->eye()); }
    [[nodiscard]] tg::vec3f right() const { return tg::normalize(cross3(tg::vec3f(0, 1, 0), this->forward())); }
    [[nodiscard]] tg::vec3f up() const { return cross3(this->forward(), this->right()); }

    [[nodiscard]] tg::mat4f view_projection(float aspect) const
    {
        auto const f = this->forward();
        auto const r = this->right();
        auto const u = this->up();
        auto const to_eye = this->eye() - tg::pos3f::zero;

        auto view = tg::mat4f::identity;
        for (auto i = 0; i < 3; ++i)
        {
            view[i, 0] = r[i];
            view[i, 1] = u[i];
            view[i, 2] = f[i];
        }
        view[3, 0] = -tg::dot(r, to_eye);
        view[3, 1] = -tg::dot(u, to_eye);
        view[3, 2] = -tg::dot(f, to_eye);

        auto const z_near = 0.1f;
        auto const z_far = 500.0f;
        auto const t = 1.0f / tg::tan(vertical_fov / 2.0f);
        auto proj = tg::mat4f::zero;
        proj[0, 0] = t / aspect;
        proj[1, 1] = t;
        proj[2, 2] = z_far / (z_far - z_near);
        proj[3, 2] = -z_near * z_far / (z_far - z_near);
        proj[2, 3] = 1.0f;
        return proj * view;
    }
};

/// Fills the tracer's constants block, which the shader package generates from noisy_scene.hlsl.
[[nodiscard]] shaders::scene_constants scene_constants_for(camera const& cam,
                                                           tg::mat4f const& prev_view_projection,
                                                           tg::vec2i extent,
                                                           controls const& c,
                                                           u32 frame,
                                                           u32 accum_frame)
{
    auto const e = cam.eye();
    auto const f = cam.forward();
    auto const r = cam.right();
    auto const u = cam.up();
    auto const aspect = extent[1] > 0 ? float(extent[0]) / float(extent[1]) : 1.0f;

    auto out = shaders::scene_constants{};

    // One shader float4 per matrix ROW, so nothing here depends on how a float4x4 would be packed into a constant
    // buffer.
    // tg::mat subscripts [col, row], so a row gathers across the columns.
    float* const rows[] = {out.prev_vp_row0, out.prev_vp_row1, out.prev_vp_row2, out.prev_vp_row3};
    for (auto row = 0; row < 4; ++row)
        for (auto col = 0; col < 4; ++col)
            rows[row][col] = prev_view_projection[col, row];

    out.origin[0] = e[0];
    out.origin[1] = e[1];
    out.origin[2] = e[2];
    out.origin[3] = tg::tan(cam.vertical_fov / 2.0f);
    out.forward[0] = f[0];
    out.forward[1] = f[1];
    out.forward[2] = f[2];
    out.forward[3] = aspect;
    out.right[0] = r[0];
    out.right[1] = r[1];
    out.right[2] = r[2];
    out.up[0] = u[0];
    out.up[1] = u[1];
    out.up[2] = u[2];

    out.frame = frame;
    out.accum_frame = accum_frame;
    out.spp = c.spp;
    out.light_size = c.light_size;
    return out;
}

constexpr char const* k_method_names[] = {"automatic", "atrous", "svgf", "oidn", "dlss_rr", "fsr_rr"};
constexpr sr::denoise_method k_method_values[] = {
    sr::denoise_method::automatic, sr::denoise_method::atrous,  sr::denoise_method::svgf,
    sr::denoise_method::oidn,      sr::denoise_method::dlss_rr, sr::denoise_method::fsr_rr,
};
constexpr char const* k_quality_names[] = {"fast", "balanced", "best"};

/// Draws the panel, editing `ui` in place.
void draw_panel(controls& ui,
                sr::denoise_support const& support,
                sr::denoise_outcome const& outcome,
                sr::denoise_history& history,
                u32 sample_count,
                bool& restart_requested)
{
    ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(370, 790), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("denoise"))
    {
        ImGui::End();
        return;
    }

    ImGui::Checkbox("denoise", &ui.denoise_enabled);
    ImGui::SameLine();
    ImGui::TextDisabled("(off = the raw image)");

    auto method_index = 0;
    for (auto i = 0; i < 6; ++i)
        if (k_method_values[i] == ui.denoise.method)
            method_index = i;
    if (ImGui::Combo("method", &method_index, k_method_names, 6))
        ui.denoise.method = k_method_values[method_index];
    if (ui.denoise.method != sr::denoise_method::automatic && !support.supports(ui.denoise.method))
        ImGui::TextDisabled("not in this build: the call is refused");

    ImGui::Checkbox("fresh samples", &ui.denoise.fresh_samples);
    ImGui::SameLine();
    ImGui::TextDisabled("(temporal)");

    auto quality_index = int(ui.denoise.quality);
    if (ImGui::Combo("quality", &quality_index, k_quality_names, 3))
        ui.denoise.quality = sr::denoise_quality(quality_index);

    ImGui::SliderFloat("sharpness", &ui.denoise.sharpness, 0.0f, 1.0f);
    if (ui.denoise.fresh_samples)
        ImGui::SliderFloat("responsiveness", &ui.denoise.temporal_responsiveness, 0.0f, 1.0f);

    ImGui::SeparatorText("guides");
    ImGui::Checkbox("albedo", &ui.use_albedo);
    ImGui::SameLine();
    ImGui::Checkbox("normal", &ui.use_normal);
    ImGui::SameLine();
    ImGui::Checkbox("depth", &ui.use_depth);
    ImGui::TextDisabled("svgf requires normal + depth + motion");

    ImGui::SeparatorText("tracer");
    ImGui::SliderInt("samples/frame", &ui.spp, 1, 16);
    ImGui::SliderFloat("light size", &ui.light_size, 0.0f, 2.0f);
    ImGui::Checkbox("orbit camera", &ui.orbiting);

    ImGui::SeparatorText("view");
    ImGui::Checkbox("split", &ui.show_split);
    ImGui::SameLine();
    ImGui::TextDisabled("(left = raw)");
    if (ui.show_split)
        ImGui::SliderFloat("at", &ui.split, 0.0f, 1.0f);
    ImGui::SliderFloat("exposure", &ui.exposure, 0.1f, 4.0f);
    if (ImGui::Button("restart"))
        restart_requested = true;
    ImGui::SameLine();
    if (ImGui::Button("cut history"))
        history.reset();

    ImGui::SeparatorText("what happened");
    // A cc::string_view is not null-terminated, so it goes through ImGui as a counted string rather than %s.
    auto const status = sr::to_string(outcome.status);
    auto const member = sr::to_string(outcome.method);
    ImGui::Text("status     %.*s", int(status.size()), status.data());
    ImGui::Text("member     %.*s", int(member.size()), member.data());
    ImGui::Text("restarted  %s", outcome.restarted ? "yes" : "no");
    ImGui::Text("samples    %u", sample_count);
    ImGui::Text("%.1f fps", double(ImGui::GetIO().Framerate));
    ImGui::TextDisabled("turn `fresh samples` off and `samples`");
    ImGui::TextDisabled("climbs: a spatial member then backs off");
    ImGui::End();
}
} // namespace

ASYNC_EXAMPLE("shaped-rendering/denoise-playground")
{
    // Picking a member this build cannot run is the refusal path, and the method dropdown offers three of them on
    // purpose — so the one line each of them logs is expected here rather than a surprise.
    // Showing what a refusal does is a thing this example is FOR, and it would otherwise fail the moment it is used.
    nx::allow_warnings("did not run: not supported by this build or device");

    auto const capture = sr::capture_request::from_environment();
    if (capture.active && !capture.name.empty())
    {
        cc::eprintln("this example offers only the default view, so it cannot take {}", capture.name);
        co_return;
    }

    auto wsys = sr::window_system::try_create({.headless = capture.active});
    if (wsys.has_error())
    {
        cc::eprintln("no window backend: {}", wsys.error().to_string());
        co_return;
    }

    auto const size = capture.active ? capture.size : tg::vec2i(1600, 900);
    auto win = wsys.value()->create_window({.title = "sr denoise playground", .width = size[0], .height = size[1]});

#if DENOISE_PLAYGROUND_BACKEND_DX12
    auto context = sg::create_dx12_context({.adapter = sg::backend::dx12::dx12_adapter::hardware_or_warp});
#else
    auto context = sg::create_vulkan_context({});
#endif
    if (context.has_error())
    {
        cc::eprintln("no graphics device: {}", context.error().to_string());
        co_return;
    }
    auto& ctx = *context.value();

    // Both formats, because the backend decides which one the context accepts: DXIL for dx12, SPIR-V for vulkan.
    // Registering only one is a build that compiles for every backend and runs on exactly one, which is what this
    // example did until somebody ran the vulkan arm.
    auto lib = slib::shader_library();
    auto dxil = slib::create_dxc_compiler();
    auto spirv = slib::create_dxc_spirv_compiler();
    if (dxil.has_error() && spirv.has_error())
    {
        cc::eprintln("no shader compiler: {}", dxil.error().to_string());
        co_return;
    }
    if (dxil.has_value())
        lib.add_compiler(cc::move(dxil.value()));
    if (spirv.has_value())
        lib.add_compiler(cc::move(spirv.value()));
    lib.add_package(sr::shader_package()); // imgui, blit and the denoise members
    lib.add_package(shaders::package());   // the tracer

    // Every supported member starts compiling now rather than on the first call that wants one, so switching the
    // method in the panel does not cost a frame of `pending`.
    sr::denoise_routine::prewarm(ctx);

    auto swapchain = sg::swapchain_handle();
    auto capture_target = sg::texture_2d();
    if (capture.active)
        capture_target
            = ctx.persistent.create_texture_2d({.format = sg::pixel_format::bgra8_unorm,
                                                .width = size[0],
                                                .height = size[1],
                                                .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src});
    else
        swapchain = ctx.create_swapchain({.window = win->native_window(), .format = sg::pixel_format::bgra8_unorm});

    auto imgui = sr::imgui_context::create();

    // The tracer is a plain compute pipeline rather than a render routine, so there is no readiness to poll and
    // nothing worth drawing without it: build it up front and fail loudly.
    auto const scene_layout = ctx.cached.acquire_binding_group_layout<shaders::scene_bindings>();
    auto const scene_shader = shaders::noisy_scene.compute.main_cs->acquire(ctx);
    co_await cc::async_settled(scene_shader);
    auto const* const scene_compiled = scene_shader->try_value();
    if (scene_compiled == nullptr)
    {
        auto const* const why = scene_shader->try_error();
        FAIL(cc::format("noisy_scene.hlsl did not compile: {}",
                        why != nullptr ? why->underlying().to_string() : cc::string("no reason given")));
        co_return;
    }
    auto const* scene_constants_binding = static_cast<sg::binding const*>(nullptr);
    for (auto const& b : scene_compiled->bindings)
        if (b.type == sg::binding_type::uniform_buffer)
            scene_constants_binding = &b;
    if (scene_constants_binding == nullptr)
    {
        FAIL("the scene shader reflects no constants block"); // a bare co_return here would read as a passing example
        co_return;
    }
    auto const scene_pipeline_async = ctx.cached.acquire_compute_pipeline(
        {.shader = *scene_compiled,
         .layout
         = ctx.cached.acquire_pipeline_layout({.groups = {scene_layout}, .inline_constants = *scene_constants_binding})});
    co_await cc::async_settled(scene_pipeline_async);
    auto const* const scene_pipeline = scene_pipeline_async->try_value();
    if (scene_pipeline == nullptr)
    {
        FAIL("the scene pipeline did not build"); // a bare co_return here would read as a passing example
        co_return;
    }

    auto const compose_layout = ctx.cached.acquire_binding_group_layout<shaders::compose_bindings>();
    auto const compose_shader = shaders::compose.compute.main_cs->acquire(ctx);
    co_await cc::async_settled(compose_shader);
    auto const* const compose_compiled = compose_shader->try_value();
    if (compose_compiled == nullptr)
    {
        auto const* const why = compose_shader->try_error();
        FAIL(cc::format("compose.hlsl did not compile: {}",
                        why != nullptr ? why->underlying().to_string() : cc::string("no reason given")));
        co_return;
    }
    auto const* compose_constants_binding = static_cast<sg::binding const*>(nullptr);
    for (auto const& b : compose_compiled->bindings)
        if (b.type == sg::binding_type::uniform_buffer)
            compose_constants_binding = &b;
    if (compose_constants_binding == nullptr)
    {
        FAIL("the compose shader reflects no constants block"); // a bare co_return here would read as a passing example
        co_return;
    }
    auto const compose_pipeline_async = ctx.cached.acquire_compute_pipeline(
        {.shader = *compose_compiled,
         .layout = ctx.cached.acquire_pipeline_layout(
             {.groups = {compose_layout}, .inline_constants = *compose_constants_binding})});
    co_await cc::async_settled(compose_pipeline_async);
    auto const* const compose_pipeline = compose_pipeline_async->try_value();
    if (compose_pipeline == nullptr)
    {
        FAIL("the compose pipeline did not build"); // a bare co_return here would read as a passing example
        co_return;
    }

    auto const support = sr::query_denoise_support(ctx);

    auto images = view_images();
    auto history = sr::denoise_history();
    auto cam = camera();
    auto prev_view_projection = tg::mat4f::identity;

    auto ui = controls();
    auto applied = ui;
    auto frame = u32(0);
    auto accum_frame = u32(0);
    auto captured_frames = 0;
    auto last_time = cc::current_time_steady_secs();
    auto last_outcome = sr::denoise_outcome();

    while (true)
    {
        if (capture.active && captured_frames >= capture.accumulate_frames)
            break;
        if (!capture.active && win->is_close_requested())
            break;

        wsys.value()->poll_events();
        imgui.process_events(*wsys.value());

        auto const now = cc::current_time_steady_secs();
        auto const dt = float(now - last_time);
        last_time = now;

        (void)ctx.routines.tick(); // nothing else brings the routines up

        auto const viewport = win->is_minimized() ? tg::vec2i(0, 0) : tg::vec2i(win->width(), win->height());
        if (viewport[0] == 0 || viewport[1] == 0)
            continue;

        imgui.begin_frame(*win, dt);

        auto const sample_count = ui.denoise.fresh_samples ? u32(ui.spp) : (accum_frame + 1) * u32(ui.spp);
        auto restart_requested = false;
        draw_panel(ui, support, last_outcome, history, sample_count, restart_requested);

        // A camera that moved, a setting that changed what is traced, or a resize: each restarts the mean, because
        // averaging across any of them would be averaging two different images.
        if (ui.orbiting)
        {
            cam.azimuth = cam.azimuth + tg::angle_f::make_from_degree(dt * 18.0f);
            accum_frame = 0;
        }
        if (restart_requested || ui.restarts_against(applied))
            accum_frame = 0;
        applied = ui;

        // Fresh samples means never accumulating: a temporal member wants this frame's own estimate, not a mean.
        if (ui.denoise.fresh_samples)
            accum_frame = 0;

        if (images.extent != viewport)
        {
            resize_images(ctx, images, viewport);
            accum_frame = 0;
        }

        // Closed before anything below suspends: `begin_frame` opened a record scope on this thread, and a co_await
        // with it still open is a scope that crossed a suspension.
        // It is also what runs ImGui::Render, so it is what makes GetDrawData() below non-null.
        imgui.end_frame();

        auto cmd = ctx.create_command_list();

        // -- trace
        {
            auto const group = ctx.transient.create_binding_group(*cmd, scene_layout,
                                                                  shaders::scene_bindings{
                                                                      .gColor = images.color.as_any_image_view(),
                                                                      .gAlbedo = images.albedo.as_any_image_view(),
                                                                      .gNormal = images.normal.as_any_image_view(),
                                                                      .gDepth = images.depth.as_any_image_view(),
                                                                      .gMotion = images.motion.as_any_image_view(),
                                                                  });
            cmd->compute.bind_pipeline(**scene_pipeline);
            cmd->compute.bind<shaders::scene_bindings>(*group);
            cmd->compute.set_inline_constants(
                scene_constants_for(cam, prev_view_projection, viewport, ui, frame, accum_frame));
            cmd->compute.dispatch_threads(viewport[0], viewport[1], 1);
        }

        // -- denoise
        //
        // A guide is handed over only when the panel says so, and an empty texture is how the API says "I do not
        // have this one" — which is what lets the checkboxes turn a guide off with no second code path.
        auto shown = images.color;
        if (ui.denoise_enabled)
        {
            auto inputs
                = sr::denoise_inputs{.color = images.color, .output = images.denoised, .sample_count = sample_count};
            if (ui.use_albedo)
                inputs.guides.albedo = images.albedo;
            if (ui.use_normal)
                inputs.guides.normal = images.normal;
            if (ui.use_depth)
                inputs.guides.depth = images.depth;
            inputs.guides.motion = images.motion; // always written by the tracer; svgf requires it

            last_outcome = sr::denoise_routine::execute(*cmd, inputs, history, ui.denoise);

            // Nothing was written unless the outcome says so, which is why a refusal shows the raw image rather than
            // whatever `denoised` happened to be holding from an earlier frame.
            if (last_outcome.is_denoised())
                shown = images.denoised;
        }
        else
        {
            last_outcome = {.status = sr::denoise_status::denoised, .method = sr::denoise_method::none};
        }

        // -- compose: the raw image and the denoised one either side of the divider, tonemapped
        {
            auto const group = ctx.transient.create_binding_group(*cmd, compose_layout,
                                                                  shaders::compose_bindings{
                                                                      .gRaw = images.color.as_texture_view(),
                                                                      .gDenoised = shown.as_texture_view(),
                                                                      .gTarget = images.composed.as_any_image_view(),
                                                                  });
            cmd->compute.bind_pipeline(**compose_pipeline);
            cmd->compute.bind<shaders::compose_bindings>(*group);
            cmd->compute.set_inline_constants(shaders::compose_constants{
                .split = ui.split,
                .exposure = ui.exposure,
                .show_split = ui.show_split ? 1 : 0,
            });
            cmd->compute.dispatch_threads(viewport[0], viewport[1], 1);
        }

        // -- present
        {
            auto rt = capture.active ? capture_target.as_render_target_view() : swapchain->acquire_backbuffer();
            auto blitted = sg::routine_outcome::declined;
            auto panelled = sg::routine_outcome::declined;
            {
                auto pass = cmd->raster.render_to({.color_targets = {rt.cleared(tg::vec4f(0.02f, 0.02f, 0.03f, 1))}});
                blitted = sr::blit_routine::execute(pass, images.composed);
            }
            {
                auto pass = cmd->raster.render_to({.color_targets = {rt.preserved()}});
                panelled = sr::imgui_routine::execute(pass, ImGui::GetDrawData());
            }
            auto const drew_everything
                = blitted == sg::routine_outcome::executed && panelled == sg::routine_outcome::executed;

            if (!capture.active)
            {
                ctx.submit_command_list_and_present(*swapchain, cc::move(cmd));
                ctx.advance_epoch();
                co_await ctx.epochs_in_flight_completion(swapchain->buffer_count());
            }
            else
            {
                ctx.submit_command_list(cc::move(cmd));
                ctx.advance_epoch();
                co_await ctx.epochs_in_flight_completion(2);

                // A frame only counts once every routine actually drew.
                // Both decline while their shaders compile, and imgui's also uploads its font atlas on the way up —
                // so counting a declined frame would race that upload with the readback below, and could write an
                // image with no panel in it.
                if (drew_everything)
                    ++captured_frames;
            }
        }

        prev_view_projection = cam.view_projection(float(viewport[0]) / float(viewport[1]));
        ++frame;
        if (!ui.denoise.fresh_samples)
            ++accum_frame;
    }

    if (capture.active && captured_frames >= capture.accumulate_frames)
    {
        // The readback is an async transfer, and it would otherwise find this texture in the layout the last render
        // pass left it in — a layout the transfer queue cannot use — and submit a fixup list of its own, which warns.
        // Recorded here rather than on the frame's own list because the render-target view above outlives the
        // transition there and puts the texture straight back.
        {
            auto prepare = ctx.create_command_list();
            prepare->prepare_for_async(capture_target.raw(), sg::async_direction::download);
            ctx.submit_command_list(cc::move(prepare));
            ctx.advance_epoch();
        }

        auto const writing = sr::write_capture_image_async(ctx, capture_target, capture.output_path);
        co_await cc::async_settled(writing);
        auto const* const written = writing->try_value();
        if (written == nullptr || written->has_error())
            cc::eprintln("capture failed");
    }

    ctx.advance_epoch();
    co_await ctx.idle_completion();
}
