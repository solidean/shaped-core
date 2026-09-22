#include <clean-core/common/utility.hh>
#include <clean-core/record/log.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/impl/oidn_network.hh>
#include <shaped-rendering/impl/tza.hh>
#include <sr_shaders.hh>

namespace sr::impl
{
namespace
{
/// Where the weights blob sits, baked in at configure time.
/// See the viewer TODO: a shipped binary wants it staged beside itself rather than read out of the source tree.
constexpr char const* k_weights_dir = SR_OIDN_WEIGHTS_DIR;

/// The tensors the network runs through, in the order they are produced.
///
/// Indices into `_features`, so the table below can name them.
enum feature : int
{
    f_input = 0, // the nine packed channels, which is also dec_conv1a's skip

    f_enc0,  // enc_conv0, full resolution
    f_enc1,  // enc_conv1 before its pool, full resolution
    f_pool1, // half
    f_enc2,  // half
    f_pool2, // quarter
    f_enc3,  // quarter
    f_pool3, // eighth
    f_enc4,  // eighth
    f_pool4, // sixteenth

    f_enc5a, // sixteenth
    f_enc5b, // sixteenth

    f_up4, // eighth
    f_dec4a,
    f_dec4b,

    f_up3, // quarter
    f_dec3a,
    f_dec3b,

    f_up2, // half
    f_dec2a,
    f_dec2b,

    f_up1, // full
    f_dec1a,
    f_dec1b,

    f_out, // three channels, full resolution

    f_count
};

/// One convolution, as the table drives it.
///
/// `skip` is the second half of a concatenated source, or `f_count` when the layer takes one tensor.
/// The widths are NOT here: they are read out of the weights, which is what keeps this table a topology.
struct conv_step
{
    char const* name;
    int source;
    int skip;
    int target;
    int level; // 0 full resolution, 1 half, and so on
};

/// The sixteen convolutions, in order, exactly as `UNetFilter::addUNet` builds them.
constexpr conv_step k_convs[] = {
    {"enc_conv0", f_input, f_count, f_enc0, 0},   {"enc_conv1", f_enc0, f_count, f_enc1, 0},
    {"enc_conv2", f_pool1, f_count, f_enc2, 1},   {"enc_conv3", f_pool2, f_count, f_enc3, 2},
    {"enc_conv4", f_pool3, f_count, f_enc4, 3},   {"enc_conv5a", f_pool4, f_count, f_enc5a, 4},
    {"enc_conv5b", f_enc5a, f_count, f_enc5b, 4}, {"dec_conv4a", f_up4, f_pool3, f_dec4a, 3},
    {"dec_conv4b", f_dec4a, f_count, f_dec4b, 3}, {"dec_conv3a", f_up3, f_pool2, f_dec3a, 2},
    {"dec_conv3b", f_dec3a, f_count, f_dec3b, 2}, {"dec_conv2a", f_up2, f_pool1, f_dec2a, 1},
    {"dec_conv2b", f_dec2a, f_count, f_dec2b, 1}, {"dec_conv1a", f_up1, f_input, f_dec1a, 0},
    {"dec_conv1b", f_dec1a, f_count, f_dec1b, 0}, {"dec_conv0", f_dec1b, f_count, f_out, 0},
};

constexpr int k_conv_count = int(sizeof(k_convs) / sizeof(k_convs[0]));

/// The four pools, each taking a convolution's output down one level.
struct resample_step
{
    int source;
    int target;
    int level; // the level of the SMALLER of the two
};

constexpr resample_step k_pools[] = {
    {f_enc1, f_pool1, 1},
    {f_enc2, f_pool2, 2},
    {f_enc3, f_pool3, 3},
    {f_enc4, f_pool4, 4},
};

constexpr int k_pool_count = int(sizeof(k_pools) / sizeof(k_pools[0]));

constexpr resample_step k_upsamples[] = {
    {f_enc5b, f_up4, 4}, // from the sixteenth up to the eighth
    {f_dec4b, f_up3, 3},
    {f_dec3b, f_up2, 2},
    {f_dec2b, f_up1, 1},
};

constexpr int k_upsample_count = int(sizeof(k_upsamples) / sizeof(k_upsamples[0]));

/// How many texels along x one convolution thread produces.
/// Must match NN_CONV_TEXELS in nn_conv.hlsl; the oracle test is what notices if it does not.
constexpr int k_conv_texels = 16;

/// Half a fp16 lane, widened.
///
/// Written out rather than taken from a library because this is the only place the repo reads one, and the weights
/// arrive in no other format.
[[nodiscard]] f32 from_half(u16 h)
{
    auto const sign = u32(h >> 15) << 31;
    auto exponent = u32((h >> 10) & 0x1F);
    auto mantissa = u32(h & 0x3FF);

    if (exponent == 0)
    {
        if (mantissa == 0)
            return cc::bit_cast<f32>(sign); // a signed zero

        // Subnormal: normalize it by shifting until the implicit bit appears.
        auto e = -1;
        do
        {
            ++e;
            mantissa <<= 1;
        } while ((mantissa & 0x400) == 0);
        mantissa &= 0x3FF;
        exponent = u32(1 - e);
    }
    else if (exponent == 0x1F)
    {
        // Infinity or NaN, which the trained weights do not contain but a corrupt file would.
        return cc::bit_cast<f32>(sign | 0x7F800000u | (mantissa << 13));
    }

    return cc::bit_cast<f32>(sign | ((exponent + 127 - 15) << 23) | (mantissa << 13));
}

/// The extent at `level`, where each level halves.
[[nodiscard]] tg::vec2i level_extent(tg::vec2i extent, int level)
{
    return tg::vec2i(extent[0] >> level, extent[1] >> level);
}
} // namespace

bool oidn_weights_present()
{
    auto const dir = cc::string_view(k_weights_dir);
    if (dir.empty())
        return false;

    // Opened rather than merely tested for, because the path is baked in at configure time and an install that was
    // removed afterwards is exactly the case this has to answer `false` for.
    auto adapter = cc::file_read_stream_adapter::open(cc::string(dir) + "/rt_hdr_alb_nrm.tza");
    return adapter.has_value();
}

bool oidn_network::create(sg::context& ctx, tg::vec2i image_extent, int max_tile, int overlap)
{
    _ctx = &ctx;
    _image_extent = image_extent;

    // Four pools halve the tensor four times, so it is sized to a multiple of sixteen whatever the image is.
    auto const round_up = [](int v) { return ((cc::max(v, 1) + 15) / 16) * 16; };

    // One tile or many, decided here and nowhere else.
    //
    // An image that fits is run whole with no overlap, which is both cheaper and the case every accuracy test covers.
    // Otherwise the tensor is the capped tile, and its interior advances by the tile less the overlap on both sides.
    auto const whole = tg::vec2i(round_up(image_extent[0]), round_up(image_extent[1]));
    // No tile smaller than an overlap on both sides plus an interior that actually advances.
    auto const cap = round_up(cc::max(max_tile, 2 * overlap + 16));

    if (whole[0] <= cap && whole[1] <= cap)
    {
        _extent = whole;
        _overlap = 0;
        _tile_step = whole;
        _tile_counts = tg::vec2i(1, 1);
    }
    else
    {
        _extent = tg::vec2i(cc::min(whole[0], cap), cc::min(whole[1], cap));
        _overlap = overlap;

        // The interior has to be a real advance, or the loop below would not terminate.
        _tile_step = tg::vec2i(cc::max(_extent[0] - 2 * _overlap, 16), cc::max(_extent[1] - 2 * _overlap, 16));
        _tile_counts = tg::vec2i((image_extent[0] + _tile_step[0] - 1) / _tile_step[0],
                                 (image_extent[1] + _tile_step[1] - 1) / _tile_step[1]);
    }

    auto const extent = _extent;

    auto const path = cc::string(k_weights_dir) + "/rt_hdr_alb_nrm.tza";
    auto adapter = cc::file_read_stream_adapter::open(path);
    if (adapter.has_error())
    {
        CC_LOG_WARNING("oidn: the weights are not at '{}'", path);
        return false;
    }

    auto stream = adapter.value().stream();
    auto blob = stream.read_all();
    if (blob.has_error())
    {
        CC_LOG_WARNING("oidn: the weights at '{}' could not be read", path);
        return false;
    }

    auto const tensors = read_tza(blob.value());
    if (tensors.empty())
        return false;

    // Every layer's weights, transposed out of `oihw` into the [o][ky][kx][i] the shader walks, then its bias.
    // One buffer for the network, because a layer is cheaper to address as a pair of offsets than as a binding.
    auto packed = cc::vector<f32>();
    _weight_offsets.clear();
    _bias_offsets.clear();

    auto widths = cc::vector<tg::vec2i>(); // in, out — per layer, read from the weights themselves
    widths.reserve(k_conv_count);

    for (auto const& step : k_convs)
    {
        auto const* const weight = find_tza(tensors, cc::string(step.name) + ".weight");
        auto const* const bias = find_tza(tensors, cc::string(step.name) + ".bias");
        if (weight == nullptr || bias == nullptr || weight->dims.size() != 4 || bias->dims.size() != 1)
        {
            CC_LOG_WARNING("oidn: the weights carry no layer '{}'", step.name);
            return false;
        }

        auto const out_channels = weight->dims[0];
        auto const in_channels = weight->dims[1];
        if (weight->dims[2] != 3 || weight->dims[3] != 3 || bias->dims[0] != out_channels)
        {
            CC_LOG_WARNING("oidn: layer '{}' is not a 3x3 convolution with a matching bias", step.name);
            return false;
        }
        if (weight->element != tza_element::float16 || bias->element != tza_element::float16)
        {
            CC_LOG_WARNING("oidn: layer '{}' is not stored as half precision", step.name);
            return false;
        }

        widths.push_back(tg::vec2i(in_channels, out_channels));
        _weight_offsets.push_back(u32(packed.size()));

        auto const* const source = reinterpret_cast<u16 const*>(weight->data.data());
        for (auto k = 0; k < 9; ++k)
            for (auto i = 0; i < in_channels; ++i)
                for (auto o = 0; o < out_channels; ++o)
                {
                    // oihw: o major, then i, then the 3x3 — so one element is at ((o * in + i) * 9 + k).
                    // Written out as [ky][kx][i][o], with the OUTPUT channel innermost: that is what varies across a
                    // wave, so it is what has to be contiguous for a weight load to touch one cache line.
                    packed.push_back(from_half(source[(o * in_channels + i) * 9 + k]));
                }

        _bias_offsets.push_back(u32(packed.size()));
        auto const* const bias_source = reinterpret_cast<u16 const*>(bias->data.data());
        for (auto o = 0; o < out_channels; ++o)
            packed.push_back(from_half(bias_source[o]));
    }

    // Every feature map's shape, in one forward pass.
    // The enum's order is the order the network produces them, so a tensor's source is always already known — which
    // is what lets this be a loop rather than a recursion.
    _feature_channels = cc::vector<i32>::create_filled(f_count, 0);
    _feature_levels = cc::vector<i32>::create_filled(f_count, 0);

    _feature_channels[f_input] = 9;
    for (auto n = 0; n < k_conv_count; ++n)
    {
        _feature_channels[k_convs[n].target] = widths[n][1];
        _feature_levels[k_convs[n].target] = k_convs[n].level;
    }
    for (auto const& p : k_pools)
    {
        _feature_channels[p.target] = _feature_channels[p.source];
        _feature_levels[p.target] = p.level;
    }
    for (auto const& u : k_upsamples)
    {
        _feature_channels[u.target] = _feature_channels[u.source];
        _feature_levels[u.target] = u.level - 1; // an upsample lands one level finer than its source
    }

    // One buffer per tensor: the skips stay live across the whole decoder, and reusing one that is still wanted is a
    // wrong image rather than a crash.
    _features.clear();
    _features.reserve(f_count);
    for (auto t = 0; t < f_count; ++t)
    {
        auto const e = level_extent(extent, _feature_levels[t]);
        auto const count = isize(e[0]) * isize(e[1]) * isize(_feature_channels[t]);
        _features.push_back(ctx.persistent.create_buffer<f32>(
            count, sg::buffer_usage::readonly_buffer | sg::buffer_usage::readwrite_buffer));
    }

    _in_channels.clear();
    _out_channels.clear();
    for (auto const& w : widths)
    {
        _in_channels.push_back(w[0]);
        _out_channels.push_back(w[1]);
    }

    _weights = ctx.persistent.create_buffer<f32>(packed.size(),
                                                 sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);

    // Held until the first `execute`, which records the upload on the caller's list.
    _pending_weights = cc::move(packed);

    // Whether the network was CREATED, which is not whether it can run yet.
    // The pipelines compile in the background, so `prepare` is what a caller drives afterwards — returning its answer
    // here would report a first call as a failure to create, which is a different thing entirely.
    (void)prepare();
    return true;
}

i64 oidn_network::feature_bytes() const
{
    if (_feature_channels.size() != f_count)
        return 0;

    auto total = i64(0);
    for (auto t = 0; t < f_count; ++t)
    {
        auto const e = level_extent(_extent, _feature_levels[t]);
        total += i64(e[0]) * i64(e[1]) * i64(_feature_channels[t]) * i64(sizeof(f32));
    }
    return total;
}

i64 oidn_network::feature_bytes_for(tg::vec2i image_extent) const
{
    if (_feature_channels.size() != f_count)
        return 0;

    auto const round_up = [](int v) { return ((cc::max(v, 1) + 15) / 16) * 16; };
    auto const extent = tg::vec2i(round_up(image_extent[0]), round_up(image_extent[1]));

    auto total = i64(0);
    for (auto t = 0; t < f_count; ++t)
    {
        auto const e = level_extent(extent, _feature_levels[t]);
        total += i64(e[0]) * i64(e[1]) * i64(_feature_channels[t]) * i64(sizeof(f32));
    }
    return total;
}

bool oidn_programs::build(sg::context& ctx)
{
    conv_layout = ctx.cached.acquire_binding_group_layout<shaders::nn_conv_bindings>();
    input_layout = ctx.cached.acquire_binding_group_layout<shaders::nn_input_bindings>();
    output_layout = ctx.cached.acquire_binding_group_layout<shaders::nn_output_bindings>();
    pool_layout = ctx.cached.acquire_binding_group_layout<shaders::nn_pool_bindings>();
    upsample_layout = ctx.cached.acquire_binding_group_layout<shaders::nn_upsample_bindings>();

    // Acquiring is idempotent and cached, so this simply picks up whatever has finished compiling since last time.
    auto const one = [&](slib::shader_asset_handle const& asset, sg::binding_group_layout_handle const& layout,
                         sg::async_compute_pipeline& out)
    {
        if (out != nullptr)
            return;

        auto const shader = asset->acquire(ctx);
        auto const* const compiled = shader->try_value();
        if (compiled == nullptr)
            return; // still compiling, or failed; `is_ready` reports both as not ready

        auto const* const constants = [&]() -> sg::binding const*
        {
            for (auto const& b : compiled->bindings)
                if (b.type == sg::binding_type::uniform_buffer)
                    return &b;
            return nullptr;
        }();
        if (constants == nullptr)
            return;

        out = ctx.cached.acquire_compute_pipeline(
            {.shader = *compiled,
             .layout = ctx.cached.acquire_pipeline_layout({.groups = {layout}, .inline_constants = *constants})});
    };

    one(shaders::nn_conv.compute.main_cs, conv_layout, conv);
    one(shaders::nn_input.compute.main_cs, input_layout, input);
    one(shaders::nn_output.compute.main_cs, output_layout, output);
    one(shaders::nn_pool.compute.main_cs, pool_layout, pool);
    one(shaders::nn_upsample.compute.main_cs, upsample_layout, upsample);

    return is_ready();
}

bool oidn_programs::is_ready() const
{
    for (auto const* const p : {&conv, &input, &output, &pool, &upsample})
        if (*p == nullptr || (*p)->try_value() == nullptr)
            return false;
    return true;
}

cc::shared_async<bool> oidn_prewarm_pipelines(sg::context& ctx)
{
    // The shaders first, because a pipeline cannot be built before its shader exists.
    for (auto const& asset :
         {shaders::nn_conv.compute.main_cs, shaders::nn_input.compute.main_cs, shaders::nn_output.compute.main_cs,
          shaders::nn_pool.compute.main_cs, shaders::nn_upsample.compute.main_cs})
    {
        auto const shader = asset->acquire(ctx);
        co_await cc::async_settled(shader);
        if (shader->try_value() == nullptr)
        {
            CC_LOG_WARNING("oidn: one of the network's shaders did not compile");
            co_return false;
        }
    }

    // Every shader is compiled now, so one pass creates all five pipelines; what is left is waiting for them.
    auto programs = oidn_programs();
    (void)programs.build(ctx);

    for (auto const* const p : {&programs.conv, &programs.input, &programs.output, &programs.pool, &programs.upsample})
    {
        if (*p == nullptr)
        {
            CC_LOG_WARNING("oidn: one of the network's pipelines could not be created");
            co_return false;
        }
        co_await cc::async_settled(*p);
        if ((*p)->try_value() == nullptr)
        {
            CC_LOG_WARNING("oidn: one of the network's pipelines did not build");
            co_return false;
        }
    }
    co_return true;
}

bool oidn_network::prepare()
{
    if (_ctx == nullptr || !is_valid())
        return false;
    if (!_programs.build(*_ctx))
        return false;

    // Built once, the first time the pipelines are all there, and reused by every tile and every frame afterwards.
    if (_conv_groups.empty())
        build_groups();
    return true;
}

void oidn_network::build_groups()
{
    auto& ctx = *_ctx;

    for (auto n = 0; n < k_conv_count; ++n)
    {
        auto const& step = k_convs[n];
        auto const& source_a = _features[step.source];
        auto const& source_b = step.skip == f_count ? _features[step.source] : _features[step.skip];
        _conv_groups.push_back(ctx.persistent.create_binding_group(
            _programs.conv_layout, shaders::nn_conv_bindings{.gSourceA = source_a.as_readonly_buffer(),
                                                             .gSourceB = source_b.as_readonly_buffer(),
                                                             .gWeights = _weights.as_readonly_buffer(),
                                                             .gTarget = _features[step.target].as_readwrite_buffer()}));
    }

    for (auto const& p : k_pools)
        _pool_groups.push_back(ctx.persistent.create_binding_group(
            _programs.pool_layout, shaders::nn_pool_bindings{.gSource = _features[p.source].as_readonly_buffer(),
                                                             .gTarget = _features[p.target].as_readwrite_buffer()}));

    for (auto const& u : k_upsamples)
        _upsample_groups.push_back(ctx.persistent.create_binding_group(
            _programs.upsample_layout,
            shaders::nn_upsample_bindings{.gSource = _features[u.source].as_readonly_buffer(),
                                          .gTarget = _features[u.target].as_readwrite_buffer()}));
}

bool oidn_network::is_ready() const
{
    return is_valid() && _programs.is_ready();
}

bool oidn_network::execute(sg::command_list& cmd,
                           sg::texture_2d const& color,
                           sg::texture_2d const& albedo,
                           sg::texture_2d const& normal,
                           sg::texture_2d const& output,
                           f32 input_scale)
{
    if (!is_ready())
        return false;

    auto& ctx = cmd.context();

    // The weights go up once, ahead of the first layer that reads them and on this same list, so the ordering is the
    // command list's rather than something this has to arrange.
    if (!_pending_weights.empty())
    {
        cmd.upload.data_to_buffer(_weights, _pending_weights);
        _pending_weights.clear();
    }

    // The two groups that name the CALLER's textures, built once per call rather than once per tile.
    // Everything else was built with the network, because a tile changes push constants and nothing a group names.
    auto const input_group = ctx.transient.create_binding_group(
        _programs.input_layout, shaders::nn_input_bindings{.gColor = color.as_readonly_view(),
                                                           .gAlbedo = albedo.as_readonly_view(),
                                                           .gNormal = normal.as_readonly_view(),
                                                           .gTarget = _features[f_input].as_readwrite_buffer()});
    auto const output_group = ctx.transient.create_binding_group(
        _programs.output_layout, shaders::nn_output_bindings{.gSource = _features[f_out].as_readonly_buffer(),
                                                             .gTarget = output.as_readwrite_view()});

    // One pass per tile, each writing only its interior.
    // The tensors are reused across tiles, which is the point: they are sized for one tile and never for the image.
    for (auto ty = 0; ty < _tile_counts[1]; ++ty)
        for (auto tx = 0; tx < _tile_counts[0]; ++tx)
        {
            auto const interior_origin = tg::vec2i(tx * _tile_step[0], ty * _tile_step[1]);
            auto const interior = tg::vec2i(cc::min(_tile_step[0], _image_extent[0] - interior_origin[0]),
                                            cc::min(_tile_step[1], _image_extent[1] - interior_origin[1]));

            // An edge tile is SHIFTED INWARD rather than allowed to hang over the image.
            //
            // Hanging over would fill the overhang by repeating the border pixel, and that smear is an image the whole-frame
            // run never sees — so it moves the result, and it moves it further the wider the overlap is.
            // Shifting instead means every tile's tensor is real content, and the border is where the network finds it.
            auto const clamp_origin = [](int want, int tensor, int image)
            { return image <= tensor ? 0 : cc::clamp(want, 0, image - tensor); };
            auto const tensor_origin
                = tg::vec2i(clamp_origin(interior_origin[0] - _overlap, _extent[0], _image_extent[0]),
                            clamp_origin(interior_origin[1] - _overlap, _extent[1], _image_extent[1]));

            // What is kept therefore sits wherever the shift left it, rather than always at `_overlap`.
            auto const read_offset = interior_origin - tensor_origin;

            // The nine packed channels.
            cmd.compute.bind_pipeline(**_programs.input->try_value());
            cmd.compute.bind<shaders::nn_input_bindings>(*input_group);
            cmd.compute.set_inline_constants(shaders::nn_input_constants{.width = u32(_extent[0]),
                                                                         .height = u32(_extent[1]),
                                                                         .source_width = u32(_image_extent[0]),
                                                                         .source_height = u32(_image_extent[1]),
                                                                         .source_offset_x = tensor_origin[0],
                                                                         .source_offset_y = tensor_origin[1],
                                                                         .input_scale = input_scale,
                                                                         ._pad0 = 0});
            cmd.compute.dispatch_threads(_extent[0], _extent[1], 1);

            // The convolutions, with the pools and upsamples that feed them.
            // Walked in table order, and each resample runs as soon as its source exists, which is what the fixed order of
            // `k_convs` already guarantees.
            auto const run_resamples_before = [&](int target)
            {
                for (auto i = 0; i < k_pool_count; ++i)
                {
                    auto const& p = k_pools[i];
                    if (p.target == target)
                    {
                        auto const e = level_extent(_extent, p.level);
                        auto const channels = _feature_channels[p.target];
                        cmd.compute.bind_pipeline(**_programs.pool->try_value());
                        cmd.compute.bind<shaders::nn_pool_bindings>(*_pool_groups[i]);
                        cmd.compute.set_inline_constants(shaders::nn_pool_constants{.width = u32(e[0]),
                                                                                    .height = u32(e[1]),
                                                                                    .channels = u32(channels),
                                                                                    ._pad = 0});
                        cmd.compute.dispatch_threads(channels, e[0], e[1]);
                    }
                }

                for (auto i = 0; i < k_upsample_count; ++i)
                {
                    auto const& u = k_upsamples[i];
                    if (u.target == target)
                    {
                        auto const e = level_extent(_extent, u.level);
                        auto const channels = _feature_channels[u.target];
                        cmd.compute.bind_pipeline(**_programs.upsample->try_value());
                        cmd.compute.bind<shaders::nn_upsample_bindings>(*_upsample_groups[i]);
                        cmd.compute.set_inline_constants(shaders::nn_upsample_constants{.width = u32(e[0]),
                                                                                        .height = u32(e[1]),
                                                                                        .channels = u32(channels),
                                                                                        ._pad = 0});
                        cmd.compute.dispatch_threads(channels, e[0], e[1]);
                    }
                }
            };

            for (auto n = 0; n < k_conv_count; ++n)
            {
                auto const& step = k_convs[n];
                run_resamples_before(step.source);

                auto const e = level_extent(_extent, step.level);
                auto const in_channels = _in_channels[n];
                auto const out_channels = _out_channels[n];

                // A concatenated source is two buffers and a split point; a plain one names the same buffer twice, which the
                // shader never reads past.
                auto const& source_a = _features[step.source];
                auto const& source_b = step.skip == f_count ? _features[step.source] : _features[step.skip];
                auto const channels_a = _feature_channels[step.source];

                cmd.compute.bind_pipeline(**_programs.conv->try_value());
                cmd.compute.bind<shaders::nn_conv_bindings>(*_conv_groups[n]);
                cmd.compute.set_inline_constants(shaders::nn_conv_constants{
                    .width = u32(e[0]),
                    .height = u32(e[1]),
                    .in_channels = u32(in_channels),
                    .out_channels = u32(out_channels),
                    .weight_offset = _weight_offsets[n],
                    .bias_offset = _bias_offsets[n],
                    .in_channels_a = u32(step.skip == f_count ? in_channels : channels_a),
                    ._pad = 0,
                });
                cmd.compute.dispatch_threads(out_channels, (e[0] + k_conv_texels - 1) / k_conv_texels, e[1]);
            }

            // Back to radiance.
            cmd.compute.bind_pipeline(**_programs.output->try_value());
            cmd.compute.bind<shaders::nn_output_bindings>(*output_group);
            cmd.compute.set_inline_constants(shaders::nn_output_constants{.width = u32(_extent[0]),
                                                                          .height = u32(_extent[1]),
                                                                          .target_width = u32(_image_extent[0]),
                                                                          .target_height = u32(_image_extent[1]),
                                                                          .write_width = u32(interior[0]),
                                                                          .write_height = u32(interior[1]),
                                                                          .target_offset_x = interior_origin[0],
                                                                          .target_offset_y = interior_origin[1],
                                                                          .read_offset_x = read_offset[0],
                                                                          .read_offset_y = read_offset[1],
                                                                          .input_scale = input_scale,
                                                                          ._pad0 = 0,
                                                                          ._pad1 = 0,
                                                                          ._pad2 = 0});
            cmd.compute.dispatch_threads(interior[0], interior[1], 1);
        }

    return true;
}
} // namespace sr::impl
