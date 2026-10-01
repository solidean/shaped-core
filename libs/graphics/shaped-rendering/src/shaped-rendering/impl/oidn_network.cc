#include <clean-core/common/assert.hh>
#include <clean-core/common/endian.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/record/log.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/impl/oidn_network.hh>
#include <shaped-rendering/impl/tza.hh>
#include <sr_sgl_shaders.hh>
#include <sr_shaders.hh>
#include <typed-geometry/scalar/half_float.hh>

namespace sr::impl
{
namespace
{
/// Where the weights blob sits, baked in at configure time.
/// A shipped binary wants the weights staged beside it instead; libs/graphics/shaped-rendering/docs/TODO.md has it.
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
    char const* name = nullptr;
    int source = 0;
    int skip = 0;
    int target = 0;
    int level = 0; // 0 full resolution, 1 half, and so on
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
    int source = 0;
    int target = 0;
    int level = 0; // the level of the SMALLER of the two
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
constexpr int k_conv_texels = 8;

/// `v` rounded up to a multiple of sixteen, and at least sixteen.
/// Four pools halve a tensor four times, so every tensor extent and every tile origin lives on this grid.
[[nodiscard]] int round_up(int v)
{
    return ((cc::max(v, 1) + 15) / 16) * 16;
}

/// The extent at `level`, where each level halves.
[[nodiscard]] tg::vec2i level_extent(tg::vec2i extent, int level)
{
    return tg::vec2i(extent[0] >> level, extent[1] >> level);
}
/// The file each network is fetched as, in `k_weights_dir`.
[[nodiscard]] char const* weights_file(oidn_network_size size)
{
    return size == oidn_network_size::small ? "rt_hdr_alb_nrm_small.tza" : "rt_hdr_alb_nrm.tza";
}

/// Reads, checks and packs one weights file, which is what `oidn_load_weights` does once per network per process.
[[nodiscard]] cc::optional<oidn_weights> load_weights_from_disk(oidn_network_size size);
} // namespace

bool oidn_weights_present()
{
    // Asked once per process, because `query_reconstruct_support` asks on every denoise call, whichever member runs.
    // Opened rather than merely tested for: the path is baked in at configure time, and an install removed since
    // then has to answer `false`.
    static auto const present = []
    {
        auto const dir = cc::string_view(k_weights_dir);
        if (dir.empty())
            return false;
        for (auto const size : {oidn_network_size::small, oidn_network_size::base})
            if (!cc::file_read_stream_adapter::open(cc::string(dir) + "/" + weights_file(size)).has_value())
                return false;
        return true;
    }();
    return present;
}

oidn_weights const* oidn_load_weights(oidn_network_size size)
{
    // Thread-safe by the language's rules for a function-local static, and logged at most once for the same reason.
    static auto const small = load_weights_from_disk(oidn_network_size::small);
    static auto const base = load_weights_from_disk(oidn_network_size::base);
    auto const& loaded = size == oidn_network_size::small ? small : base;
    return loaded.has_value() ? &loaded.value() : nullptr;
}

namespace
{
cc::optional<oidn_weights> load_weights_from_disk(oidn_network_size size)
{
    auto out = oidn_weights();

    auto const path = cc::string(k_weights_dir) + "/" + weights_file(size);
    auto adapter = cc::file_read_stream_adapter::open(path);
    if (adapter.has_error())
    {
        CC_LOG_WARNING("oidn: the weights are not at '{}'", path);
        return cc::nullopt;
    }

    auto stream = adapter.value().stream();
    auto blob = stream.read_all();
    if (blob.has_error())
    {
        CC_LOG_WARNING("oidn: the weights at '{}' could not be read", path);
        return cc::nullopt;
    }

    auto const tensors = read_tza(blob.value());
    if (tensors.empty())
        return cc::nullopt;

    // Read every layer first, because the weights cannot be packed until the feature shapes are known.
    //
    // CHANNELS ARE PADDED TO A MULTIPLE OF FOUR so the convolution can read its source four channels at a time.
    // That is OIDN's `tensorBlockC` in our own terms, and it is what the measurement asked for: the input reads were
    // two thirds of the shader's time.
    // Only three of the network's shapes are not already a multiple of four — the nine input channels, the three
    // output ones, and the seventy-three `dec_conv1a` concatenates — so the padding costs almost nothing to compute.
    struct layer_source
    {
        tza_tensor const* weight = nullptr;
        tza_tensor const* bias = nullptr;
        i32 in_channels = 0;
        i32 out_channels = 0;
    };

    auto sources = cc::vector<layer_source>();
    sources.reserve(k_conv_count);

    for (auto const& step : k_convs)
    {
        auto const* const weight = find_tza(tensors, cc::string(step.name) + ".weight");
        auto const* const bias = find_tza(tensors, cc::string(step.name) + ".bias");
        if (weight == nullptr || bias == nullptr || weight->dims.size() != 4 || bias->dims.size() != 1)
        {
            CC_LOG_WARNING("oidn: the weights carry no layer '{}'", step.name);
            return cc::nullopt;
        }

        auto const out_channels = weight->dims[0];
        auto const in_channels = weight->dims[1];
        if (weight->dims[2] != 3 || weight->dims[3] != 3 || bias->dims[0] != out_channels)
        {
            CC_LOG_WARNING("oidn: layer '{}' is not a 3x3 convolution with a matching bias", step.name);
            return cc::nullopt;
        }
        if (weight->layout != "oihw")
        {
            CC_LOG_WARNING("oidn: layer '{}' is laid out as '{}', not the oihw this packs from", step.name,
                           weight->layout);
            return cc::nullopt;
        }
        if (weight->element != tza_element::float16 || bias->element != tza_element::float16)
        {
            CC_LOG_WARNING("oidn: layer '{}' is not stored as half precision", step.name);
            return cc::nullopt;
        }

        sources.push_back({.weight = weight, .bias = bias, .in_channels = in_channels, .out_channels = out_channels});
    }

    // Every feature map's shape, in one forward pass.
    // The enum's order is the order the network produces them, so a tensor's source is always already known — which
    // is what lets this be a loop rather than a recursion.
    auto real_channels = cc::vector<i32>::create_filled(f_count, 0);
    out.feature_levels = cc::vector<i32>::create_filled(f_count, 0);

    real_channels[f_input] = 9;
    for (auto n = 0; n < k_conv_count; ++n)
    {
        real_channels[k_convs[n].target] = sources[n].out_channels;
        out.feature_levels[k_convs[n].target] = k_convs[n].level;
    }
    for (auto const& p : k_pools)
    {
        real_channels[p.target] = real_channels[p.source];
        out.feature_levels[p.target] = p.level;
    }
    for (auto const& u : k_upsamples)
    {
        real_channels[u.target] = real_channels[u.source];
        out.feature_levels[u.target] = u.level - 1; // an upsample lands one level finer than its source
    }

    // What every tensor is actually stored with: the padded count, which is what every shader is told.
    // A padding channel carries a hard zero rather than whatever was left in memory, because the next layer would
    // multiply it by a weight of zero and a NaN times zero is still a NaN.
    out.feature_channels = cc::vector<i32>::create_filled(f_count, 0);
    for (auto t = 0; t < f_count; ++t)
        out.feature_channels[t] = ((real_channels[t] + 3) / 4) * 4;

    // Every layer's weights, as [ky][kx][i][o] over the PADDED channel spaces, then its bias.
    // One buffer for the network, because a layer is cheaper to address as a pair of offsets than as a binding.
    auto& packed = out.packed;

    for (auto n = 0; n < k_conv_count; ++n)
    {
        auto const& step = k_convs[n];
        auto const& src = sources[n];

        auto const real_a = real_channels[step.source];
        auto const stored_a = out.feature_channels[step.source];
        auto const real_b = step.skip == f_count ? 0 : real_channels[step.skip];
        auto const stored_b = step.skip == f_count ? 0 : out.feature_channels[step.skip];

        auto const stored_in = stored_a + stored_b;
        auto const stored_out = out.feature_channels[step.target];

        if (real_a + real_b != src.in_channels)
        {
            CC_LOG_WARNING("oidn: layer '{}' wants {} input channels and the tensors carry {}", step.name,
                           src.in_channels, real_a + real_b);
            return cc::nullopt;
        }

        out.in_channels.push_back(stored_in);
        out.out_channels.push_back(stored_out);
        out.weight_offsets.push_back(u32(packed.size()));

        // Which real input channel a stored one carries, or -1 where it is padding.
        auto const real_of = [&](i32 stored)
        {
            return stored < stored_a ? (stored < real_a ? stored : -1)
                                     : (stored - stored_a < real_b ? real_a + (stored - stored_a) : -1);
        };

        // Read a half at a time with an explicit byte order, since the file offsets need not be aligned for a u16.
        auto const half_at = [](tza_tensor const& t, i64 index)
        { return tg::f16::make_from_bits(cc::load_bytes_le<u16>(t.data, index * 2)).to_f32(); };
        for (auto k = 0; k < 9; ++k)
            for (auto i = 0; i < stored_in; ++i)
            {
                auto const j = real_of(i);
                for (auto o = 0; o < stored_out; ++o)
                {
                    // oihw: o major, then i, then the 3x3 — so one element is at ((o * in + i) * 9 + k).
                    auto const live = j >= 0 && o < src.out_channels;
                    packed.push_back(live ? half_at(*src.weight, (o * src.in_channels + j) * 9 + k) : 0.0f);
                }
            }

        out.bias_offsets.push_back(u32(packed.size()));
        for (auto o = 0; o < stored_out; ++o)
            packed.push_back(o < src.out_channels ? half_at(*src.bias, o) : 0.0f);
    }

    return out;
}
} // namespace

oidn_tile_plan plan_tiles(tg::vec2i image, int max_tile, int overlap)
{
    CC_ASSERT(overlap >= 0 && overlap % 16 == 0, "the tile overlap must be a non-negative multiple of 16");

    auto const whole = tg::vec2i(round_up(image[0]), round_up(image[1]));
    // No tile smaller than an overlap on both sides plus an interior that actually advances.
    auto const cap = round_up(cc::max(max_tile, 2 * overlap + 16));

    // An image that fits is run whole with no overlap, which is both cheaper and the case every accuracy test covers.
    if (whole[0] <= cap && whole[1] <= cap)
        return {.extent = whole, .step = whole, .counts = tg::vec2i(1, 1), .overlap = 0};

    // The tile is CHOSEN to compute the fewest pixels, not taken as large as the cap allows.
    //
    // Cost is flat per computed pixel, so what a tile size decides is only how much of the image is computed more
    // than once.
    // That is not monotonic: a tile whose interior divides the image badly computes more than a smaller one whose
    // interior divides it well.
    // The two axes are independent, because a tile's count along one depends on its extent along that one alone.
    auto const best_extent = [&](int length)
    {
        auto chosen = 0;
        auto computed = 0;
        for (auto candidate = 2 * overlap + 16; candidate <= cap; candidate += 16)
        {
            auto const step = candidate - 2 * overlap;
            auto const total = ((length + step - 1) / step) * candidate;
            if (chosen == 0 || total < computed)
            {
                chosen = candidate;
                computed = total;
            }
        }
        return chosen;
    };

    auto plan = oidn_tile_plan{.overlap = overlap};
    for (auto a = 0; a < 2; ++a)
    {
        // An axis that fits under the cap stays one untiled span, whatever the other axis needs.
        // Tiling it anyway would compute the same tensor once per tile row, since every one of them clamps to 0.
        if (whole[a] <= cap)
        {
            plan.extent[a] = whole[a];
            plan.step[a] = whole[a];
            plan.counts[a] = 1;
            continue;
        }

        plan.extent[a] = cc::min(whole[a], best_extent(image[a]));
        // The interior has to be a real advance, or the loop in `execute` would not terminate.
        plan.step[a] = cc::max(plan.extent[a] - 2 * overlap, 16);
        plan.counts[a] = (image[a] + plan.step[a] - 1) / plan.step[a];
    }
    return plan;
}

bool oidn_network::create(sg::context& ctx, tg::vec2i image_extent, int max_tile, int overlap, oidn_network_size size)
{
    _ctx = &ctx;
    _size = size;
    _max_tile = max_tile;
    _image_extent = image_extent;

    auto const plan = plan_tiles(image_extent, max_tile, overlap);
    _extent = plan.extent;
    _tile_step = plan.step;
    _tile_counts = plan.counts;
    _overlap = plan.overlap;

    auto const extent = _extent;

    _source = oidn_load_weights(size);
    if (_source == nullptr)
        return false;

    // One buffer per tensor: the skips stay live across the whole decoder, and reusing one that is still wanted is a
    // wrong image rather than a crash.
    _features.clear();
    _features.reserve(f_count);
    for (auto t = 0; t < f_count; ++t)
    {
        auto const e = level_extent(extent, _source->feature_levels[t]);
        auto const count = isize(e[0]) * isize(e[1]) * isize(_source->feature_channels[t]);
        // The last tensor can also be copied out, which is what `output_tensor` is for.
        auto const usage = sg::buffer_usage::readonly_buffer | sg::buffer_usage::readwrite_buffer
                         | (t == f_out ? sg::buffer_usage::copy_src : sg::buffer_usage{});
        _features.push_back(ctx.persistent.create_buffer<f32>(count, usage));
    }

    _weights = ctx.persistent.create_buffer<f32>(_source->packed.size(),
                                                 sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);

    // Recorded by the first `execute`, on the caller's list.
    _weights_uploaded = false;

    // A second `create` on one object would otherwise keep groups naming the buffers it just replaced.
    _conv_groups.clear();
    _pool_groups.clear();
    _upsample_groups.clear();

    // Whether the network was CREATED, which is not whether it can run yet.
    // The pipelines compile in the background, so `prepare` is what a caller drives afterwards — returning its answer
    // here would report a first call as a failure to create, which is a different thing entirely.
    (void)prepare();
    return true;
}

i64 oidn_network::feature_bytes() const
{
    if (_source == nullptr)
        return 0;

    auto total = i64(0);
    for (auto t = 0; t < f_count; ++t)
    {
        auto const e = level_extent(_extent, _source->feature_levels[t]);
        total += i64(e[0]) * i64(e[1]) * i64(_source->feature_channels[t]) * i64(sizeof(f32));
    }
    return total;
}

i64 oidn_network::feature_bytes_for(tg::vec2i image_extent) const
{
    if (_source == nullptr)
        return 0;

    auto const extent = tg::vec2i(round_up(image_extent[0]), round_up(image_extent[1]));

    auto total = i64(0);
    for (auto t = 0; t < f_count; ++t)
    {
        auto const e = level_extent(extent, _source->feature_levels[t]);
        total += i64(e[0]) * i64(e[1]) * i64(_source->feature_channels[t]) * i64(sizeof(f32));
    }
    return total;
}

bool oidn_programs::build(sg::context& ctx)
{
    conv_layout = ctx.cached.acquire_binding_group_layout<shaders::nn_conv_bindings>();
    input_layout = ctx.cached.acquire_binding_group_layout<shaders::nn_input_bindings>();
    output_layout = ctx.cached.acquire_binding_group_layout<shaders::nn_output_bindings>();
    pool_layout = ctx.cached.acquire_binding_group_layout<sgl_shaders::nn_pool_features>();
    upsample_layout = ctx.cached.acquire_binding_group_layout<sgl_shaders::nn_upsample_features>();

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
                if (b.type == sg::binding_type::constants_buffer)
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

    // An SGL entry point states its own layout, so nothing is looked up in what the shader reflects.
    auto const one_sgl = [&](auto const& entry, sg::async_compute_pipeline& out)
    {
        if (out != nullptr)
            return;

        auto const shader = entry->acquire(ctx);
        auto const* const compiled = shader->try_value();
        if (compiled == nullptr)
            return; // still compiling, or failed; `is_ready` reports both as not ready

        out = ctx.cached.acquire_compute_pipeline({.shader = *compiled, .layout = entry.acquire_layout(ctx)});
    };

    one_sgl(sgl_shaders::nn_pool.main_cs, pool);
    one_sgl(sgl_shaders::nn_upsample.main_cs, upsample);

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
          sgl_shaders::nn_pool.main_cs.asset, sgl_shaders::nn_upsample.main_cs.asset})
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
    if (!_conv_groups.empty())
        return true; // built, and nothing a later call could change
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
        // The sources are READ four channels at a time, so they are bound as a float4 view of the same memory the
        // target writes one channel at a time.
        // Padding every channel count to four is what makes that reinterpret exact rather than a truncation.
        auto const source_a = _features[step.source].template try_reinterpret_as<tg::vec4f>();
        auto const source_b = (step.skip == f_count ? _features[step.source] : _features[step.skip])
                                  .template try_reinterpret_as<tg::vec4f>();
        CC_ASSERT(source_a.has_value() && source_b.has_value(), "a feature map is not a whole number of float4s");

        _conv_groups.push_back(ctx.persistent.create_binding_group(
            _programs.conv_layout, shaders::nn_conv_bindings{.gSourceA = source_a.value().as_readonly_buffer(),
                                                             .gSourceB = source_b.value().as_readonly_buffer(),
                                                             .gWeights = _weights.as_readonly_buffer(),
                                                             .gTarget = _features[step.target].as_readwrite_buffer()}));
    }

    for (auto const& p : k_pools)
        _pool_groups.push_back(ctx.persistent.create_binding_group(
            _programs.pool_layout, sgl_shaders::nn_pool_features{.source = _features[p.source].as_readonly_buffer(),
                                                                 .target = _features[p.target].as_readwrite_buffer()}));

    for (auto const& u : k_upsamples)
        _upsample_groups.push_back(ctx.persistent.create_binding_group(
            _programs.upsample_layout,
            sgl_shaders::nn_upsample_features{.source = _features[u.source].as_readonly_buffer(),
                                              .target = _features[u.target].as_readwrite_buffer()}));
}

sg::buffer<f32> const& oidn_network::output_tensor() const
{
    CC_ASSERT(_features.size() == f_count, "the network was not created");
    return _features[f_out];
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
    if (!_weights_uploaded)
    {
        cmd.upload.data_to_buffer(_weights, cc::span<f32 const>(_source->packed));
        _weights_uploaded = true;
    }

    // The two groups that name the CALLER's textures, built once per call rather than once per tile.
    // Everything else was built with the network, because a tile changes push constants and nothing a group names.
    auto const input_group = ctx.transient.create_binding_group(
        cmd, _programs.input_layout,
        shaders::nn_input_bindings{.gColor = color.as_texture_view(),
                                   .gAlbedo = albedo.as_texture_view(),
                                   .gNormal = normal.as_texture_view(),
                                   .gTarget = _features[f_input].as_readwrite_buffer()});
    auto const output_group = ctx.transient.create_binding_group(
        cmd, _programs.output_layout,
        shaders::nn_output_bindings{.gSource = _features[f_out].as_readonly_buffer(),
                                    .gTarget = output.as_any_image_view()});

    // One pass per tile, each writing only its interior.
    // The tensors are reused across tiles, which is the point: they are sized for one tile and never for the image.
    for (auto ty = 0; ty < _tile_counts[1]; ++ty)
        for (auto tx = 0; tx < _tile_counts[0]; ++tx)
        {
            auto const interior_origin = tg::vec2i(tx * _tile_step[0], ty * _tile_step[1]);
            auto const interior = tg::vec2i(cc::min(_tile_step[0], _image_extent[0] - interior_origin[0]),
                                            cc::min(_tile_step[1], _image_extent[1] - interior_origin[1]));

            // An edge tile is SHIFTED INWARD, to end where the whole run's padded tensor ends.
            //
            // The bound is the image rounded up to sixteen, not the image: an origin must stay a multiple of sixteen
            // or the tile pools over different windows than the whole run does.
            // The at most fifteen rows or columns past the image are then the same zeros the whole run pads with.
            auto const clamp_origin
                = [](int want, int tensor, int image) { return cc::clamp(want, 0, round_up(image) - tensor); };
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
                        auto const channels = _source->feature_channels[p.target];
                        cmd.compute.bind_pipeline(**_programs.pool->try_value());
                        cmd.compute.bind_group(0, *_pool_groups[i]);
                        cmd.compute.set_inline_constants(
                            sgl_shaders::nn_pool_constants{.width = e[0], .height = e[1], .channels = channels}.to_block());
                        cmd.compute.dispatch_threads(channels, e[0], e[1]);
                    }
                }

                for (auto i = 0; i < k_upsample_count; ++i)
                {
                    auto const& u = k_upsamples[i];
                    if (u.target == target)
                    {
                        auto const e = level_extent(_extent, u.level);
                        auto const channels = _source->feature_channels[u.target];
                        cmd.compute.bind_pipeline(**_programs.upsample->try_value());
                        cmd.compute.bind_group(0, *_upsample_groups[i]);
                        cmd.compute.set_inline_constants(
                            sgl_shaders::nn_upsample_constants{.width = e[0], .height = e[1], .channels = channels}
                                .to_block());
                        cmd.compute.dispatch_threads(channels, e[0], e[1]);
                    }
                }
            };

            for (auto n = 0; n < k_conv_count; ++n)
            {
                auto const& step = k_convs[n];
                run_resamples_before(step.source);

                auto const e = level_extent(_extent, step.level);
                auto const in_channels = _source->in_channels[n];
                auto const out_channels = _source->out_channels[n];

                // A concatenated source is two buffers and a split point; a plain one names the same buffer twice, which the
                // shader never reads past.
                auto const& source_a = _features[step.source];
                auto const& source_b = step.skip == f_count ? _features[step.source] : _features[step.skip];
                auto const channels_a = _source->feature_channels[step.source];

                cmd.compute.bind_pipeline(**_programs.conv->try_value());
                cmd.compute.bind<shaders::nn_conv_bindings>(*_conv_groups[n]);
                cmd.compute.set_inline_constants(shaders::nn_conv_constants{
                    .width = u32(e[0]),
                    .height = u32(e[1]),
                    .in_channels = u32(in_channels),
                    .out_channels = u32(out_channels),
                    .weight_offset = _source->weight_offsets[n],
                    .bias_offset = _source->bias_offsets[n],
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
                                                                          ._pad0 = 0});
            cmd.compute.dispatch_threads(interior[0], interior[1], 1);
        }

    return true;
}
} // namespace sr::impl
