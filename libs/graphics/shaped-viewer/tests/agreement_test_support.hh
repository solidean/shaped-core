#pragma once

// The render-mode matrix's harness: one scene converged in one mode at the view level, as a viewer draws it, and held tile
// by tile to the megakernel's image of the same scene.
//
// Each image is converged twice from seeds that share nothing, and the difference between the two halves is what sets a
// tile's tolerance: a tile is wrong where it sits more standard errors from the reference than noise explains.
// A mean over the whole image hides a wrong wall behind a right floor, and a fixed tolerance either flakes on noise or
// passes a real bias, which is what this replaces.
//
// Set SV_TEST_IMAGES to a directory and each comparison writes what it compared there: reference, contender, and the
// tiles colored by how far off they are.

#include "raster_test_support.hh"
#include "viewer_test_env.hh"

#include <babel-serializer/image/image.hh>
#include <clean-core/algorithm/sort.hh>
#include <clean-core/platform/environment.hh>
#include <clean-core/record/log.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <typed-geometry/scalar/scalar.hh>

namespace sv_test::agreement
{
struct mode_config;
struct converged;
struct tile_report;
struct firefly;
struct lit_box;
struct denoised_frame;
class denoise_view;
enum class enforce : u8;

/// What a comparison is drawn at, small enough that a hybrid's thousand frames stay cheap.
inline constexpr auto k_size = tg::vec2i(64, 64);
inline constexpr int k_tile = 8;

/// How many restarts a reference runs before its first half, so its seeds are none the contender draws.
inline constexpr int k_reference_restarts = 3;
} // namespace sv_test::agreement

/// Whether a comparison's tiles fail the test, or only its log and its picture tell where they stand.
/// A mode that approximates on purpose is measured all the same, so the size of what it gives up stays visible.
enum class sv_test::agreement::enforce : sv::u8
{
    tiles,
    report_only,
};

/// The Cornell box lit by its lamp alone, as the view-level tests place it, and the camera it fills.
struct sv_test::agreement::lit_box
{
    cc::vector<sv::scene_item> items;
    sv::camera camera;
};

/// One way of drawing a layer: the settings its mode reads.
struct sv_test::agreement::mode_config
{
    cc::string_view name;
    sv::render_mode mode = sv::render_mode::megakernel;
    sv::hybrid_lighting lighting = sv::hybrid_lighting::deferred;
    i32 hybrid_bounces = 1;
    /// the tracers' path length, which the hybrid does not read
    i32 max_bounces = 5;
    /// what one megakernel frame traces
    i32 samples_per_pixel = 16;
    sr::denoise_method denoiser = sr::denoise_method::none;
    sv::background background = {};
    /// false lights the layer by its background alone when it has no light of its own
    bool fallback_light = true;
    sv::environment_estimate environment_estimate = sv::environment_estimate::exact;
};

/// A layer's mean converged twice, from seeds that share nothing, with the G-buffer a rasterizing mode writes beside it.
struct sv_test::agreement::converged
{
    cc::vector<tg::vec4f> first;
    cc::vector<tg::vec4f> second;
    /// empty for a traced layer, which writes no G-buffer while it does not denoise
    cc::vector<rgba16> normals;
    cc::vector<rgba16> albedo;
    /// per half
    u32 samples = 0;

    [[nodiscard]] tg::vec4f mean_at(isize i) const { return (first[i] + second[i]) * 0.5f; }
};

/// How a contender's tiles sit against the reference's.
struct sv_test::agreement::tile_report
{
    int compared = 0;
    int failed = 0;
    int non_finite = 0;
    f64 worst_z = 0;
    /// the relative difference of the tile with the worst `worst_z`
    f64 worst_relative = 0;
    tg::vec2i worst_tile = tg::vec2i(-1, -1);
    /// How far apart each image's own two halves sit per tile, in the standard errors the comparison assumes: about 1 where
    /// its pixels' errors are independent, less where the sampler stratifies across pixels.
    f64 calibration_test = 0;
    f64 calibration_reference = 0;
    /// The smallest bias the median tile would flag, relative to the reference: what the comparison can see at all.
    f64 resolution = 0;
    /// per tile, the worst channel's z, or -1 where too few pixels were compared
    cc::vector<f64> z;
};

/// A pixel far brighter than the surface around it: one sample that carried far more than the rest.
struct sv_test::agreement::firefly
{
    tg::vec2i pixel;
    /// how far the pixel's two halves differ, over the median of its eight neighbors in the mean
    f64 ratio = 0;
    /// what the one sample that made it was worth, in multiples of the neighborhood's value
    f64 sample_weight = 0;
};

namespace sv_test::agreement
{
/// Whether the device traces and rasterizes both, which every mode compared here needs.
[[nodiscard]] inline bool can_trace(sg::context& ctx)
{
    return sv_test::can_run(ctx) && ctx.supports(sg::feature::ray_query);
}

/// The Cornell box under its 15-nit lamp; `lamp_drop` lowers the lamp from its 2 mm under the ceiling.
[[nodiscard]] inline lit_box make_lit_box(sv::gpu_resource_manager& resources, f32 lamp_drop = 0.0f)
{
    auto box = sv_test::make_cornell_box();
    auto const lamp_vertices = 3 * sv_test::cornell_lamp_triangles;
    for (auto i = box.positions.size() - lamp_vertices; i < box.positions.size(); ++i)
        box.positions[i] = box.positions[i] - tg::vec3f(0, lamp_drop, 0);
    auto const lit = sv_test::as_lit_meshes(box, 15.0f);
    auto out = lit_box{.camera = sv_test::cornell_camera()};
    out.items.push_back(resources.acquire_scene_item(lit.enclosure));
    out.items.push_back(resources.acquire_scene_item(lit.lamp));
    resources.wait_for_pending_uploads();
    // Narrow enough that the box fills the view, so no tile is half background.
    out.camera.projection.vertical_fov = tg::angle_d::make_from_degree(30.0);
    return out;
}

[[nodiscard]] inline bool is_finite(tg::vec4f const& c)
{
    for (auto ch = 0; ch < 4; ++ch)
        if (!(tg::abs(c[ch]) < 1e30f))
            return false;
    return true;
}

[[nodiscard]] inline bool is_finite(f32 v)
{
    return tg::abs(v) < 1e30f;
}

[[nodiscard]] inline bool is_finite(rgba16 const& c)
{
    return is_finite(tg::vec4f(f32(c.r), f32(c.g), f32(c.b), f32(c.a)));
}

/// Fails the test where any pixel of `image`, frame `frame` of the stream `stream` reads back, is not a finite number.
///
/// Every frame a test reads is checked, not only the one it measures.
/// A non-finite sample shows black, spreads through a denoiser into a block, and in an accumulated mean stays forever;
/// one that flashes up on a single frame of a moving image is exactly what a check of the final image misses.
template <class Texel>
void check_finite(cc::string_view stream, cc::string_view image, u64 frame, cc::span<Texel const> pixels)
{
    auto count = 0;
    auto first = isize(-1);
    for (auto i = isize(0); i < pixels.size(); ++i)
        if (!is_finite(pixels[i]))
        {
            first = count == 0 ? i : first;
            ++count;
        }
    CHECK(count == 0)
        .dump("stream", stream)
        .dump("image", image)
        .dump("frame", frame)
        .dump("non-finite pixels", count)
        .dump("first at x", first < 0 ? -1 : int(first % k_size[0]))
        .dump("first at y", first < 0 ? -1 : int(first / k_size[0]));
}

/// One view of `items` through `camera`, drawn the `config` way, inside a root layout that shows it alone.
/// The view is `views[0]`, its layer the scene's.
[[nodiscard]] inline sv::viewer_definition definition_of(cc::span<sv::scene_item const> items,
                                                         sv::camera const& camera,
                                                         mode_config const& config)
{
    auto def = sv::viewer_definition{};
    auto v = sv::view_data{};
    v.id = sv::view_id::from_string("agreement");
    v.camera = camera;
    auto& scene = sv::ensure_scene_3d(v);
    for (auto const& item : items)
        scene.items.push_back(item);
    scene.settings.mode = config.mode;
    scene.settings.hybrid_lighting = config.lighting;
    scene.settings.hybrid_bounces = config.hybrid_bounces;
    scene.settings.max_bounces = config.max_bounces;
    scene.settings.samples_per_pixel = config.samples_per_pixel;
    scene.settings.motion_hold_ms = 0.0f;
    scene.settings.reconstruct.denoiser = config.denoiser;
    scene.background = config.background;
    scene.settings.environment_estimate = config.environment_estimate;
    if (!config.fallback_light)
        scene.fallback_light = cc::nullopt;
    def.views.push_back(cc::move(v));

    auto const root_node = def.nodes.add_container(sv::invalid_node);
    auto leaf = sv::layout_leaf{};
    leaf.views.push_back(sv::view_index(0));
    def.nodes.add_leaf(root_node, cc::move(leaf));
    auto root = sv::view_data{};
    root.id = sv::view_id::from_string("root");
    root.layers.push_back({.kind = sv::layer_kind::layout, .blend = sv::layer_blend::replace, .root_node = root_node});
    def.root_view = sv::view_index(def.views.size());
    def.views.push_back(cc::move(root));
    return def;
}

/// `items` drawn the `config` way, its mean converged to `samples` per pixel twice over.
/// The second half starts after a restart, so it draws from other seeds than the first.
/// `may_be_noiseless` accepts halves that agree to the bit, as every sample of a mirror in a uniform environment does.
inline cc::shared_async<converged> converge(sg::context& ctx,
                                            sv::gpu_resource_manager& resources,
                                            cc::span<sv::scene_item const> items,
                                            sv::camera const& camera,
                                            mode_config const& config,
                                            u32 samples,
                                            int restarts_before = 0,
                                            tg::vec2i size = k_size,
                                            bool may_be_noiseless = false)
{
    auto const output
        = ctx.persistent.create_texture_2d({.format = sg::pixel_format::bgra8_unorm,
                                            .width = size[0],
                                            .height = size[1],
                                            .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src});

    auto def = definition_of(items, camera, config);
    auto store = sv::view_store{};
    auto const history = sv::view_history{};
    auto const id = def.views[0].id;
    auto index = u64(0);

    struct reading
    {
        u32 samples = 0;
        bool stand_ins = true;
        cc::vector<tg::vec4f> mean;
        cc::vector<rgba16> normals;
        cc::vector<rgba16> albedo;
    };

    auto const frame = [&]() -> cc::shared_async<reading>
    {
        (void)ctx.routines.tick();
        auto cmd = ctx.create_command_list();
        resources.advance_to(ctx.current_epoch());
        // As the viewer records it: what landed since the last frame is finished ahead of the traces that read it.
        (void)resources.record_pending_work(*cmd);
        store.begin_frame(u64(ctx.current_epoch()));
        auto const plan = sv::build_render_plan(def, size, index++, history);
        (void)sv::viewer_renderer::execute(*cmd, def, plan, resources, store,
                                           output.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1)));
        auto const* const state = store.peek_ptr(id);
        auto const slot_of = [&](u64 temporal) { return state != nullptr ? state->temporal.get_ptr(temporal) : nullptr; };
        auto const* const slot = slot_of(sv::temporal_id::accumulation(0));
        auto const* const normal_slot = slot_of(sv::temporal_id::normal_guide(0));
        auto const* const albedo_slot = slot_of(sv::temporal_id::albedo_guide(0));
        auto mean = sg::data_future<tg::vec4f>();
        auto normals = sg::data_future<rgba16>();
        auto albedo = sg::data_future<rgba16>();
        if (slot != nullptr && slot->texture.raw() != nullptr)
            mean = sg::data_future<tg::vec4f>(cmd->download.bytes_from_texture(slot->texture.raw()));
        if (normal_slot != nullptr && normal_slot->texture.raw() != nullptr && albedo_slot != nullptr
            && albedo_slot->texture.raw() != nullptr)
        {
            normals = sg::data_future<rgba16>(cmd->download.bytes_from_texture(normal_slot->texture.raw()));
            albedo = sg::data_future<rgba16>(cmd->download.bytes_from_texture(albedo_slot->texture.raw()));
        }
        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();
        co_await ctx.idle_completion();
        sv_test::drive_ambient_work();

        auto out = reading{.samples = store.min_accumulated_samples(id),
                           .stand_ins = slot == nullptr || slot->stand_in_hash != 0};
        auto const take = [](auto const& future, auto& into)
        {
            auto const data = future.try_get_data();
            REQUIRE(data.has_value());
            for (auto const& v : data.value())
                into.push_back(v);
        };
        if (mean.is_valid())
            take(mean, out.mean);
        if (normals.is_valid())
        {
            take(normals, out.normals);
            take(albedo, out.albedo);
        }
        check_finite(config.name, "mean", index, cc::span<tg::vec4f const>(out.mean));
        check_finite(config.name, "normal guide", index, cc::span<rgba16 const>(out.normals));
        check_finite(config.name, "albedo guide", index, cc::span<rgba16 const>(out.albedo));
        co_return out;
    };

    // A frame a hair to the side and the camera's return each restart the mean, onto a sequence of its own:
    // an image draws one sequence however often it restarts, so a restart alone would draw the same samples again.
    auto const restart = [&]() -> cc::shared_async<cc::unit>
    {
        ++def.views[0].layers[0].settings.sequence;
        def.views[0].camera.position = camera.position + tg::vec3d(1e-3, 0, 0);
        (void)co_await frame();
        def.views[0].camera = camera;
        (void)co_await frame();
    };
    // A reference restarted past the contender's counts draws other seeds, so the two estimates' errors are independent.
    for (auto r = 0; r < restarts_before; ++r)
        co_await restart();

    // A count rather than a time, since what a frame takes is the GPU's and the load's.
    // Compiles come first, and a frame declined while one runs is cheap: a forward hybrid's cold compile is some 9000 of them.
    auto const max_frames = i32(samples) * 4 + 16384;
    auto const start = cc::current_time_steady_secs();
    auto compiled = false;
    auto out = converged{};
    for (auto half = 0; half < 2; ++half)
    {
        auto frames = 0;
        auto r = reading{};
        while (r.stand_ins || r.samples < samples || r.mean.empty())
        {
            r = co_await frame();
            if (!compiled && !r.stand_ins)
            {
                compiled = true;
                CC_LOG_INFO("{}: drawn without stand-ins {:.1f} s and {} frames in", config.name,
                            cc::current_time_steady_secs() - start, frames);
            }
            REQUIRE(++frames < max_frames).dump("mode", config.name).dump("samples", r.samples);
        }
        (half == 0 ? out.first : out.second) = cc::move(r.mean);
        out.samples = r.samples;
        out.normals = cc::move(r.normals);
        out.albedo = cc::move(r.albedo);

        if (half == 0)
            co_await restart();
    }

    // Two halves drawn from one sequence would agree to the bit, and their difference would claim no noise at all.
    auto differing = 0;
    for (auto i = isize(0); i < out.first.size(); ++i)
        if (out.first[i] != out.second[i])
            ++differing;
    if (!may_be_noiseless)
        REQUIRE(differing > out.first.size() / 2).dump("mode", config.name).dump("differing pixels", differing);

    co_await cc::async_settled(sv::background_work(ctx));
    co_return out;
}

} // namespace sv_test::agreement

/// A layer's raw mean and its denoised image after one frame, and whether the denoiser produced the image shown.
struct sv_test::agreement::denoised_frame
{
    cc::vector<tg::vec4f> raw;
    cc::vector<tg::vec4f> denoised;
    bool ran = false;
    u32 samples = 0;
    /// whether the viewer drew the frame, rather than declining it and leaving the output as it was
    bool executed = false;
    /// the output the frame presented, bgra8 a pixel, as a window shows it
    cc::vector<u32> shown;
};

/// One denoised view through the viewer, stepped a frame at a time; the view is `def.views[0]`, which a caller may move.
/// Every frame's raw mean and denoised image are checked for non-finite pixels as they are read.
class sv_test::agreement::denoise_view
{
public:
    denoise_view(sg::context& ctx,
                 sv::gpu_resource_manager& resources,
                 cc::span<sv::scene_item const> items,
                 sv::camera const& camera,
                 mode_config const& config)
      : def(definition_of(items, camera, config)),
        _ctx(ctx),
        _resources(resources),
        _name(config.name),
        _output(
            ctx.persistent.create_texture_2d({.format = sg::pixel_format::bgra8_unorm,
                                              .width = k_size[0],
                                              .height = k_size[1],
                                              .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src}))
    {
    }

    /// One frame, and what the view holds after it.
    cc::shared_async<denoised_frame> frame()
    {
        (void)_ctx.routines.tick();
        auto cmd = _ctx.create_command_list();
        _resources.advance_to(_ctx.current_epoch());
        _store.begin_frame(u64(_ctx.current_epoch()));
        auto const index = _index++;
        auto const plan = sv::build_render_plan(def, k_size, index, _history);
        auto const outcome = sv::viewer_renderer::execute(
            *cmd, def, plan, _resources, _store, _output.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1)));
        auto const shown = sg::data_future<u32>(cmd->download.bytes_from_texture(_output.raw()));
        auto const id = def.views[0].id;
        auto const* const state = _store.peek_ptr(id);
        auto const slot_of = [&](u64 temporal) { return state != nullptr ? state->temporal.get_ptr(temporal) : nullptr; };
        auto const* const mean_slot = slot_of(sv::temporal_id::accumulation(0));
        auto const* const denoised_slot = slot_of(sv::temporal_id::denoised(0));
        auto mean = sg::data_future<tg::vec4f>();
        auto denoised = sg::data_future<rgba16>();
        // The guides a denoiser steers by, which a blend over the mean's frames keeps a bad value in for good.
        auto const download = [&](u64 temporal, auto into)
        {
            auto const* const s = slot_of(temporal);
            return s != nullptr && s->texture.raw() != nullptr
                     ? decltype(into)(cmd->download.bytes_from_texture(s->texture.raw()))
                     : decltype(into)();
        };
        auto const normal_guide = download(sv::temporal_id::normal_guide(0), sg::data_future<rgba16>());
        auto const albedo_guide = download(sv::temporal_id::albedo_guide(0), sg::data_future<rgba16>());
        auto const depth_guide = download(sv::temporal_id::depth_guide(0), sg::data_future<f32>());
        if (mean_slot != nullptr && mean_slot->texture.raw() != nullptr)
            mean = sg::data_future<tg::vec4f>(cmd->download.bytes_from_texture(mean_slot->texture.raw()));
        if (denoised_slot != nullptr && denoised_slot->texture.raw() != nullptr)
            denoised = sg::data_future<rgba16>(cmd->download.bytes_from_texture(denoised_slot->texture.raw()));
        // The slot holds a current image only on a frame the denoiser produced it, and the raw mean is presented otherwise.
        // A layer switched to no denoiser keeps its old slot a while, still marked current, so the setting is read too.
        auto const ran = def.views[0].layers[0].settings.reconstruct.denoiser != sr::denoise_method::none
                      && denoised_slot != nullptr && denoised_slot->accum_frame != 0 && mean_slot != nullptr
                      && mean_slot->stand_in_hash == 0;
        _ctx.submit_command_list(cc::move(cmd));
        _ctx.advance_epoch();
        co_await _ctx.idle_completion();
        sv_test::drive_ambient_work();

        auto out = denoised_frame{.ran = ran,
                                  .samples = _store.min_accumulated_samples(id),
                                  .executed = outcome == sg::routine_outcome::executed};
        if (auto const s = shown.try_get_data(); s.has_value())
            for (auto const px : s.value())
                out.shown.push_back(px);
        if (auto const m = mean.try_get_data(); mean.is_valid() && m.has_value())
            for (auto const& v : m.value())
                out.raw.push_back(v);
        if (auto const d = denoised.try_get_data(); denoised.is_valid() && d.has_value())
            for (auto const& v : d.value())
                out.denoised.push_back(tg::vec4f(f32(v.r), f32(v.g), f32(v.b), f32(v.a)));
        check_finite(_name, "mean", index, cc::span<tg::vec4f const>(out.raw));
        if (auto const d = normal_guide.try_get_data(); normal_guide.is_valid() && d.has_value())
            check_finite(_name, "normal guide", index, cc::span<rgba16 const>(d.value()));
        if (auto const d = albedo_guide.try_get_data(); albedo_guide.is_valid() && d.has_value())
            check_finite(_name, "albedo guide", index, cc::span<rgba16 const>(d.value()));
        if (auto const d = depth_guide.try_get_data(); depth_guide.is_valid() && d.has_value())
            check_finite(_name, "depth guide", index, cc::span<f32 const>(d.value()));
        // A denoised image the member did not produce this frame is an older one, which its own frame already checked.
        if (ran)
            check_finite(_name, "denoised", index, cc::span<tg::vec4f const>(out.denoised));
        co_return out;
    }

    /// Frames until the member has denoised once, so every compile is behind it; false if it never did.
    /// A frame declined while a compile runs is cheap, and a cold denoiser plus a forward stage is some thousands of them.
    cc::shared_async<bool> warm_up()
    {
        for (auto f = 0; f < 16384; ++f)
            if ((co_await frame()).ran)
                co_return true;
        co_return false;
    }

    sv::viewer_definition def;

private:
    sg::context& _ctx;
    sv::gpu_resource_manager& _resources;
    cc::string _name;
    sg::texture_2d _output;
    sv::view_store _store;
    sv::view_history _history;
    u64 _index = 0;
};

namespace sv_test::agreement
{
/// Whether the pixel at (`x`, `y`) and its eight neighbors see one smooth surface of one material: where a tracer's jittered
/// samples and a raster's pixel center agree, and an edge, where they do not, is left out.
/// Neighbors' normals may differ by some 18 degrees, which a curved surface's do and a crease's do not.
/// The albedo tells apart what the normal cannot, such as a lamp set into a ceiling.
[[nodiscard]] inline bool is_interior(converged const& c, tg::vec2i size, int x, int y)
{
    if (c.normals.empty())
        return true;
    if (x == 0 || y == 0 || x == size[0] - 1 || y == size[1] - 1)
        return false;
    auto const& n = c.normals[isize(y) * size[0] + x];
    auto const& a = c.albedo[isize(y) * size[0] + x];
    for (auto dy = -1; dy <= 1; ++dy)
        for (auto dx = -1; dx <= 1; ++dx)
        {
            auto const i = isize(y + dy) * size[0] + (x + dx);
            auto const& m = c.normals[i];
            auto const& b = c.albedo[i];
            auto const facing = f32(n.r) * f32(m.r) + f32(n.g) * f32(m.g) + f32(n.b) * f32(m.b);
            if (facing < 0.95f)
                return false;
            if (tg::abs(f32(a.r) - f32(b.r)) > 0.01f || tg::abs(f32(a.g) - f32(b.g)) > 0.01f
                || tg::abs(f32(a.b) - f32(b.b)) > 0.01f)
                return false;
        }
    return true;
}

/// Holds `test` tile by tile to `reference`, per channel.
/// A tile's difference is counted in standard errors of the two means, which each image's two halves estimate, widened by
/// `relative_floor` of the reference's value for what the two modes may legitimately differ by.
/// Where either carries a G-buffer, only the pixels inside one face count, since raster primary hits are not antialiased.
[[nodiscard]] inline tile_report compare_tiles(converged const& test,
                                               converged const& reference,
                                               f64 relative_floor,
                                               f64 z_limit,
                                               tg::vec2i size = k_size,
                                               int tile = k_tile)
{
    auto const& masked = test.normals.empty() ? reference : test;
    auto report = tile_report{};
    for (auto const& c : test.first)
        report.non_finite += is_finite(c) ? 0 : 1;
    for (auto const& c : test.second)
        report.non_finite += is_finite(c) ? 0 : 1;

    auto self_test = 0.0;
    auto self_reference = 0.0;
    auto self_count = 0;
    auto resolutions = cc::vector<f64>();
    for (auto ty = 0; ty < size[1] / tile; ++ty)
        for (auto tx = 0; tx < size[0] / tile; ++tx)
        {
            auto worst = -1.0;
            auto worst_relative = 0.0;
            for (auto ch = 0; ch < 3; ++ch)
            {
                auto sum_test = 0.0;
                auto sum_reference = 0.0;
                auto variance = 0.0;
                auto split_test = 0.0;
                auto split_reference = 0.0;
                auto squares_test = 0.0;
                auto squares_reference = 0.0;
                auto n = 0;
                for (auto y = ty * tile; y < (ty + 1) * tile; ++y)
                    for (auto x = tx * tile; x < (tx + 1) * tile; ++x)
                    {
                        if (!is_interior(masked, size, x, y))
                            continue;
                        auto const i = isize(y) * size[0] + x;
                        auto const dt = f64(test.first[i][ch]) - f64(test.second[i][ch]);
                        auto const dr = f64(reference.first[i][ch]) - f64(reference.second[i][ch]);
                        sum_test += f64(test.mean_at(i)[ch]);
                        sum_reference += f64(reference.mean_at(i)[ch]);
                        // Half the difference of two independent estimates is one standard error of their mean.
                        variance += (dt * dt + dr * dr) / 4.0;
                        split_test += dt;
                        split_reference += dr;
                        squares_test += dt * dt;
                        squares_reference += dr * dr;
                        ++n;
                    }
                if (n < tile * tile / 4)
                    continue;
                if (squares_test > 0 && squares_reference > 0)
                {
                    self_test += split_test * split_test / squares_test;
                    self_reference += split_reference * split_reference / squares_reference;
                    ++self_count;
                }
                auto const mean_test = sum_test / n;
                auto const mean_reference = sum_reference / n;
                auto const allowed = variance / (f64(n) * f64(n)) + tg::pow(relative_floor * mean_reference, 2.0) + 1e-6;
                auto const z = tg::abs(mean_test - mean_reference) / tg::sqrt(allowed);
                if (mean_reference > 1e-3)
                    resolutions.push_back(z_limit * tg::sqrt(allowed) / mean_reference);
                if (z > worst)
                {
                    worst = z;
                    worst_relative = tg::abs(mean_test - mean_reference) / cc::max(mean_reference, 1e-3);
                }
            }
            report.z.push_back(worst);
            if (worst < 0)
                continue;
            ++report.compared;
            if (worst > z_limit)
                ++report.failed;
            if (worst > report.worst_z)
            {
                report.worst_z = worst;
                report.worst_relative = worst_relative;
                report.worst_tile = tg::vec2i(tx, ty);
            }
        }
    if (self_count > 0)
    {
        report.calibration_test = tg::sqrt(self_test / self_count);
        report.calibration_reference = tg::sqrt(self_reference / self_count);
    }
    if (!resolutions.empty())
    {
        cc::sort(resolutions);
        report.resolution = resolutions[resolutions.size() / 2];
    }
    return report;
}

[[nodiscard]] inline f64 luminance_of(tg::vec4f const& c)
{
    return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
}

/// The pixels where one half of `image` exceeds the other by more than `ratio` times the median of the pixel's eight
/// neighbors in the mean.
/// One sample lands in one half only, where an edge or a highlight is the same in both, so only the sample is found.
/// A neighborhood holding a pixel at `emitter_floor` or above is left out, since an emitter's edge jitters between halves.
[[nodiscard]] inline cc::vector<firefly> fireflies_of(converged const& image,
                                                      f64 ratio,
                                                      f64 emitter_floor,
                                                      tg::vec2i size = k_size)
{
    auto out = cc::vector<firefly>();
    for (auto y = 1; y < size[1] - 1; ++y)
        for (auto x = 1; x < size[0] - 1; ++x)
        {
            f64 neighbors[8] = {};
            auto k = 0;
            auto near_emitter = false;
            for (auto dy = -1; dy <= 1; ++dy)
                for (auto dx = -1; dx <= 1; ++dx)
                {
                    auto const l = luminance_of(image.mean_at(isize(y + dy) * size[0] + (x + dx)));
                    near_emitter = near_emitter || l >= emitter_floor;
                    if (dx != 0 || dy != 0)
                        neighbors[k++] = l;
                }
            if (near_emitter)
                continue;
            cc::sort(cc::span<f64>(neighbors));
            auto const median = (neighbors[3] + neighbors[4]) / 2.0;
            auto const i = isize(y) * size[0] + x;
            auto const excess = tg::abs(luminance_of(image.first[i]) - luminance_of(image.second[i]));
            if (median <= 1e-4 || excess < ratio * median)
                continue;
            out.push_back({.pixel = tg::vec2i(x, y),
                           .ratio = excess / median,
                           .sample_weight = excess * f64(image.samples) / median});
        }
    return out;
}

/// The linear `image` scaled by `exposure` and encoded as sRGB, one byte per channel.
[[nodiscard]] inline cc::vector<tg::vec3f> display_of(cc::span<tg::vec4f const> image, f32 exposure)
{
    auto const encode = [](f32 v)
    {
        v = cc::min(cc::max(v, 0.0f), 1.0f);
        return v <= 0.0031308f ? 12.92f * v : 1.055f * tg::pow(v, 1.0f / 2.4f) - 0.055f;
    };
    auto out = cc::vector<tg::vec3f>();
    for (auto const& c : image)
        out.push_back(tg::vec3f(encode(c[0] * exposure), encode(c[1] * exposure), encode(c[2] * exposure)));
    return out;
}

/// The tiles of `report` as a picture: green within two standard errors, yellow up to `z_limit`, red past it, gray where
/// nothing was compared.
[[nodiscard]] inline cc::vector<tg::vec3f> z_map(tile_report const& report,
                                                 f64 z_limit,
                                                 tg::vec2i size = k_size,
                                                 int tile = k_tile)
{
    auto out = cc::vector<tg::vec3f>::create_filled(isize(size[0]) * size[1], tg::vec3f(0.3f, 0.3f, 0.3f));
    auto const tiles_x = size[0] / tile;
    for (auto t = isize(0); t < report.z.size(); ++t)
    {
        auto const z = report.z[t];
        auto color = tg::vec3f(0.9f, 0.1f, 0.1f);
        if (z < 0)
            color = tg::vec3f(0.3f, 0.3f, 0.3f);
        else if (z < 2.0)
            color = tg::vec3f(0.1f, 0.6f, 0.1f);
        else if (z < z_limit)
            color = tg::vec3f(0.9f, 0.8f, 0.1f);
        auto const tx = int(t % tiles_x);
        auto const ty = int(t / tiles_x);
        for (auto y = ty * tile; y < (ty + 1) * tile; ++y)
            for (auto x = tx * tile; x < (tx + 1) * tile; ++x)
                out[isize(y) * size[0] + x] = color;
    }
    return out;
}

/// The exposure that shows `image`'s mean luminance as middle gray.
[[nodiscard]] inline f32 exposure_of(cc::span<tg::vec4f const> image)
{
    auto sum = 0.0;
    for (auto const& c : image)
        sum += 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
    auto const mean = image.empty() ? 0.0 : sum / f64(image.size());
    return mean > 0 ? f32(0.18 / mean) : 1.0f;
}

/// `panels` side by side, each scaled up by `zoom`, written to `<SV_TEST_IMAGES>/<name>.png`.
/// Writes nothing unless SV_TEST_IMAGES names a directory, so a normal run leaves no files behind.
inline void write_panels(cc::string_view name,
                         cc::span<cc::vector<tg::vec3f> const> panels,
                         tg::vec2i size = k_size,
                         int zoom = 4)
{
    auto const dir = cc::environment_variable("SV_TEST_IMAGES");
    if (!dir.has_value())
        return;

    auto const gap = 4;
    auto const width = isize(panels.size()) * (size[0] * zoom + gap) - gap;
    auto const height = isize(size[1]) * zoom;
    auto img = babel::image::image{.width = int(width), .height = int(height), .channels = 3};
    img.pixels.resize_to_uninitialized(width * height * 3);
    for (auto& b : img.pixels)
        b = byte(255);
    for (auto p = isize(0); p < panels.size(); ++p)
        for (auto y = isize(0); y < height; ++y)
            for (auto x = isize(0); x < isize(size[0]) * zoom; ++x)
            {
                auto const& c = panels[p][(y / zoom) * size[0] + x / zoom];
                auto const at = (y * width + p * (size[0] * zoom + gap) + x) * 3;
                for (auto ch = 0; ch < 3; ++ch)
                    img.pixels[at + ch] = byte(int(c[ch] * 255.0f + 0.5f));
            }

    auto const path = cc::format("{}/{}.png", dir.value(), name);
    auto adapter = cc::file_write_stream_adapter::create(path);
    REQUIRE(adapter.has_value()).dump("path", path);
    cc::write_stream out = adapter.value();
    REQUIRE(babel::image::write(out, img, babel::image::format::png).has_value());
    REQUIRE(out.flush().has_value());
    CC_LOG_INFO("wrote {}", path);
}

/// Holds `contender` to `reference`, logs and writes what it found, and fails the tiles past `z_limit`.
/// Under `enforce::report_only` it fails only on a non-finite pixel.
inline void check_agreement(cc::string_view name,
                            converged const& contender,
                            converged const& reference,
                            f64 relative_floor,
                            enforce how = enforce::tiles,
                            f64 z_limit = 5.0)
{
    auto const report = compare_tiles(contender, reference, relative_floor, z_limit);
    CC_LOG_INFO("{}{}: {} of {} tiles past {} standard errors; worst {:.2f} ({:.1f}% off) at tile ({}, {}); {} samples "
                "against {}; calibration {:.2f} / {:.2f}; sees a bias of {:.1f}% in the median tile",
                name, how == enforce::report_only ? " (reported only)" : "", report.failed, report.compared, z_limit,
                report.worst_z, 100.0 * report.worst_relative, report.worst_tile[0], report.worst_tile[1],
                contender.samples, reference.samples, report.calibration_test, report.calibration_reference,
                100.0 * report.resolution);

    auto reference_mean = cc::vector<tg::vec4f>();
    auto contender_mean = cc::vector<tg::vec4f>();
    for (auto i = isize(0); i < reference.first.size(); ++i)
    {
        reference_mean.push_back(reference.mean_at(i));
        contender_mean.push_back(contender.mean_at(i));
    }
    auto const exposure = exposure_of(reference_mean);
    cc::vector<tg::vec3f> const panels[]
        = {display_of(reference_mean, exposure), display_of(contender_mean, exposure), z_map(report, z_limit)};
    write_panels(name, panels);

    CHECK(report.non_finite == 0).dump("test", name).dump("non-finite pixels", report.non_finite);
    if (how == enforce::report_only)
        return;
    // A comparison that left most of the image out has said nothing about it.
    CHECK(report.compared * 2 > isize(report.z.size())).dump("test", name).dump("compared", report.compared);
    CHECK(report.failed == 0)
        .dump("test", name)
        .dump("failed tiles", report.failed)
        .dump("worst z", report.worst_z)
        .dump("worst relative", report.worst_relative)
        .dump("worst tile x", report.worst_tile[0])
        .dump("worst tile y", report.worst_tile[1]);
}
} // namespace sv_test::agreement
