#include <MetalFX/MetalFX.hpp>
#include <clean-core/common/assert.hh>
#include <clean-core/record/log.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/metal/metal_common.hh>
#include <shaped-graphics/backends/metal/metal_context.hh>
#include <shaped-graphics/backends/metal/metal_format.hh>
#include <shaped-graphics/backends/metal/metal_native_scope.hh>
#include <shaped-graphics/backends/metal/metal_texture.hh>
#include <shaped-rendering/impl/metalfx_backend.hh>

namespace mtl = sg::backend::metal;

/// The scaler, retained for as long as the stream lives.
class sr::impl::metalfx_stream
{
public:
    explicit metalfx_stream(MTL4FX::TemporalScaler* scaler) : _scaler(scaler) {}
    metalfx_stream(metalfx_stream const&) = delete;
    metalfx_stream& operator=(metalfx_stream const&) = delete;
    ~metalfx_stream() { _scaler->release(); }

    MTL4FX::TemporalScaler* _scaler = nullptr;

    /// A refusal of this stream's textures was logged, so the next one is not.
    bool _refusal_logged = false;
};

/// The denoised scaler, retained for as long as the stream lives.
class sr::impl::metalfx_denoised_stream
{
public:
    explicit metalfx_denoised_stream(MTL4FX::TemporalDenoisedScaler* scaler) : _scaler(scaler) {}
    metalfx_denoised_stream(metalfx_denoised_stream const&) = delete;
    metalfx_denoised_stream& operator=(metalfx_denoised_stream const&) = delete;
    ~metalfx_denoised_stream() { _scaler->release(); }

    MTL4FX::TemporalDenoisedScaler* _scaler = nullptr;

    /// A refusal of this stream's textures was logged, so the next one is not.
    bool _refusal_logged = false;
};

namespace sr::impl
{
namespace
{
[[nodiscard]] mtl::metal_context const* metal_of(sg::context const& ctx)
{
    return dynamic_cast<mtl::metal_context const*>(&ctx);
}

/// The usage bits `texture` has, as Metal spells them.
[[nodiscard]] MTL::TextureUsage usage_of(sg::texture_2d const& texture)
{
    return static_cast<mtl::metal_texture const&>(*texture.raw()).texture()->usage();
}

/// Whether `texture` has every usage bit `needed` names; logs the ones it lacks under `name`, once per stream.
[[nodiscard]] bool has_usage(sg::texture_2d const& texture, MTL::TextureUsage needed, char const* name, bool& logged)
{
    auto const missing = needed & ~usage_of(texture);
    if (missing == 0)
        return true;
    if (!logged)
        CC_LOG_ERROR("metalfx: the {} texture lacks usage bits {:#x} the scaler needs", name, u64(missing));
    logged = true;
    return false;
}

[[nodiscard]] simd::float4x4 simd_of(tg::mat4f const& m)
{
    auto const col = [&](int c) { return simd::float4{m[c, 0], m[c, 1], m[c, 2], m[c, 3]}; };
    return simd::float4x4(col(0), col(1), col(2), col(3));
}
} // namespace

bool metalfx_available(sg::context const& ctx)
{
    auto const* const metal = metal_of(ctx);
    return metal != nullptr && MTLFX::TemporalScalerDescriptor::supportsMetal4FX(metal->device());
}

f32 metalfx_max_scale(sg::context const& ctx)
{
    if (!metalfx_available(ctx))
        return 0.0f;
    return MTLFX::TemporalScalerDescriptor::supportedInputContentMaxScale(metal_of(ctx)->device());
}

std::shared_ptr<metalfx_stream> metalfx_create_stream(sg::context& ctx, metalfx_stream_desc const& desc)
{
    auto const* const metal = metal_of(ctx);
    CC_ASSERT(metal != nullptr, "MetalFX needs a metal context");

    auto const scope = mtl::autorelease_scope();
    auto* const d = MTLFX::TemporalScalerDescriptor::alloc()->init();
    d->setColorTextureFormat(mtl::pixel_format_of(desc.color_format));
    d->setDepthTextureFormat(MTL::PixelFormatR32Float);
    d->setMotionTextureFormat(MTL::PixelFormatRG16Float);
    d->setOutputTextureFormat(mtl::pixel_format_of(desc.output_format));
    d->setInputWidth(NS::UInteger(desc.input_extent[0]));
    d->setInputHeight(NS::UInteger(desc.input_extent[1]));
    d->setOutputWidth(NS::UInteger(desc.output_extent[0]));
    d->setOutputHeight(NS::UInteger(desc.output_extent[1]));

    // The exposure comes from the texel metalfx_prepare.sgl writes, as sr's `exposure` means it.
    d->setAutoExposureEnabled(false);

    auto* const scaler = d->newTemporalScaler(metal->device(), metal->compiler());
    d->release();
    if (scaler == nullptr)
    {
        CC_LOG_ERROR("metalfx: the temporal scaler refused {}x{} -> {}x{} (color {}, output {})", desc.input_extent[0],
                     desc.input_extent[1], desc.output_extent[0], desc.output_extent[1], int(desc.color_format),
                     int(desc.output_format));
        return nullptr;
    }

    // The release waits for every frame that encoded the scaler.
    auto* const stream = new metalfx_stream(scaler);
    return std::shared_ptr<metalfx_stream>(
        stream, [ctx = &ctx](metalfx_stream* s) { ctx->defer_until_retired([s] { delete s; }); });
}

bool metalfx_dispatch(metalfx_stream& stream, sg::command_list& cmd, metalfx_dispatch_desc const& desc)
{
    auto* const scaler = stream._scaler;

    // MetalFX states what each texture must allow; sg's `image` usage covers reads and writes, and anything more is
    // the caller's to add.
    if (!has_usage(desc.output, scaler->outputTextureUsage(), "output", stream._refusal_logged))
        return false;

    using sg::access_flag;
    using sg::pipeline_stage_flag;
    mtl::native_texture_access const textures[] = {
        {.texture = desc.color.raw(), .access = access_flag::shader_read},
        {.texture = desc.depth.raw(), .access = access_flag::shader_read},
        {.texture = desc.motion.raw(), .access = access_flag::shader_read},
        {.texture = desc.exposure.raw(), .access = access_flag::shader_read},
        {.texture = desc.output.raw(), .access = access_flag::shader_write},
    };
    auto const native = mtl::metal_native_scope::open(cmd, textures);

    scaler->setColorTexture(native.texture(desc.color.raw()));
    scaler->setDepthTexture(native.texture(desc.depth.raw()));
    scaler->setMotionTexture(native.texture(desc.motion.raw()));
    scaler->setExposureTexture(native.texture(desc.exposure.raw()));
    scaler->setOutputTexture(native.texture(desc.output.raw()));
    scaler->setInputContentWidth(NS::UInteger(desc.color.width()));
    scaler->setInputContentHeight(NS::UInteger(desc.color.height()));

    // metalfx_prepare.sgl wrote `near / depth`: larger is nearer.
    scaler->setDepthReversed(true);

    // sr's motion is where a pixel is minus where it was; MetalFX's points from where it is to where it was.
    scaler->setMotionVectorScaleX(-1.0f);
    scaler->setMotionVectorScaleY(-1.0f);

    // sr's jitter is where the sample lies; MetalFX's offset is the one that moves it back to the pixel centre.
    // metalfx-test's sign comparison is what settles it: 0.044 against 0.181 the other way.
    scaler->setJitterOffsetX(-desc.jitter[0]);
    scaler->setJitterOffsetY(-desc.jitter[1]);

    scaler->setReset(desc.reset);
    scaler->setFence(native.fence());
    scaler->encodeToCommandBuffer(native.command_buffer());
    return true;
}

bool metalfx_denoised_available(sg::context const& ctx)
{
    auto const* const metal = metal_of(ctx);
    return metal != nullptr && MTLFX::TemporalDenoisedScalerDescriptor::supportsMetal4FX(metal->device());
}

f32 metalfx_denoised_max_scale(sg::context const& ctx)
{
    if (!metalfx_denoised_available(ctx))
        return 0.0f;
    return MTLFX::TemporalDenoisedScalerDescriptor::supportedInputContentMaxScale(metal_of(ctx)->device());
}

std::shared_ptr<metalfx_denoised_stream> metalfx_create_denoised_stream(sg::context& ctx,
                                                                        metalfx_denoised_stream_desc const& desc)
{
    auto const* const metal = metal_of(ctx);
    CC_ASSERT(metal != nullptr, "MetalFX needs a metal context");

    auto const scope = mtl::autorelease_scope();
    auto* const d = MTLFX::TemporalDenoisedScalerDescriptor::alloc()->init();
    d->setColorTextureFormat(mtl::pixel_format_of(desc.color_format));
    d->setDepthTextureFormat(MTL::PixelFormatR32Float);
    d->setMotionTextureFormat(MTL::PixelFormatRG16Float);
    d->setDiffuseAlbedoTextureFormat(mtl::pixel_format_of(desc.albedo_format));
    d->setSpecularAlbedoTextureFormat(mtl::pixel_format_of(desc.specular_albedo_format));
    d->setNormalTextureFormat(mtl::pixel_format_of(desc.normal_format));
    d->setRoughnessTextureFormat(mtl::pixel_format_of(desc.roughness_format));
    d->setOutputTextureFormat(mtl::pixel_format_of(desc.output_format));
    d->setInputWidth(NS::UInteger(desc.input_extent[0]));
    d->setInputHeight(NS::UInteger(desc.input_extent[1]));
    d->setOutputWidth(NS::UInteger(desc.output_extent[0]));
    d->setOutputHeight(NS::UInteger(desc.output_extent[1]));
    d->setAutoExposureEnabled(false);

    auto* const scaler = d->newTemporalDenoisedScaler(metal->device(), metal->compiler());
    d->release();
    if (scaler == nullptr)
    {
        CC_LOG_ERROR("metalfx: the denoised scaler refused {}x{} -> {}x{} (color {}, albedo {}, specular {}, normal "
                     "{}, roughness {}, output {})",
                     desc.input_extent[0], desc.input_extent[1], desc.output_extent[0], desc.output_extent[1],
                     int(desc.color_format), int(desc.albedo_format), int(desc.specular_albedo_format),
                     int(desc.normal_format), int(desc.roughness_format), int(desc.output_format));
        return nullptr;
    }

    auto* const stream = new metalfx_denoised_stream(scaler);
    return std::shared_ptr<metalfx_denoised_stream>(
        stream, [ctx = &ctx](metalfx_denoised_stream* s) { ctx->defer_until_retired([s] { delete s; }); });
}

bool metalfx_denoised_dispatch(metalfx_denoised_stream& stream,
                               sg::command_list& cmd,
                               metalfx_denoised_dispatch_desc const& desc)
{
    auto* const scaler = stream._scaler;

    if (!has_usage(desc.color, scaler->colorTextureUsage(), "color", stream._refusal_logged)
        || !has_usage(desc.albedo, scaler->diffuseAlbedoTextureUsage(), "albedo", stream._refusal_logged)
        || !has_usage(desc.specular_albedo, scaler->specularAlbedoTextureUsage(), "specular albedo", stream._refusal_logged)
        || !has_usage(desc.normal, scaler->normalTextureUsage(), "normal", stream._refusal_logged)
        || !has_usage(desc.roughness, scaler->roughnessTextureUsage(), "roughness", stream._refusal_logged)
        || !has_usage(desc.output, scaler->outputTextureUsage(), "output", stream._refusal_logged))
        return false;

    using sg::access_flag;
    mtl::native_texture_access const textures[] = {
        {.texture = desc.color.raw(), .access = access_flag::shader_read},
        {.texture = desc.depth.raw(), .access = access_flag::shader_read},
        {.texture = desc.motion.raw(), .access = access_flag::shader_read},
        {.texture = desc.exposure.raw(), .access = access_flag::shader_read},
        {.texture = desc.albedo.raw(), .access = access_flag::shader_read},
        {.texture = desc.specular_albedo.raw(), .access = access_flag::shader_read},
        {.texture = desc.normal.raw(), .access = access_flag::shader_read},
        {.texture = desc.roughness.raw(), .access = access_flag::shader_read},
        {.texture = desc.output.raw(), .access = access_flag::shader_write},
    };
    auto const native = mtl::metal_native_scope::open(cmd, textures);

    scaler->setColorTexture(native.texture(desc.color.raw()));
    scaler->setDepthTexture(native.texture(desc.depth.raw()));
    scaler->setMotionTexture(native.texture(desc.motion.raw()));
    scaler->setExposureTexture(native.texture(desc.exposure.raw()));
    scaler->setDiffuseAlbedoTexture(native.texture(desc.albedo.raw()));
    scaler->setSpecularAlbedoTexture(native.texture(desc.specular_albedo.raw()));
    scaler->setNormalTexture(native.texture(desc.normal.raw()));
    scaler->setRoughnessTexture(native.texture(desc.roughness.raw()));
    scaler->setOutputTexture(native.texture(desc.output.raw()));

    // The same conventions as the upscaler's, for the same reasons: see metalfx_dispatch.
    scaler->setDepthReversed(true);
    scaler->setMotionVectorScaleX(-1.0f);
    scaler->setMotionVectorScaleY(-1.0f);
    scaler->setJitterOffsetX(-desc.jitter[0]);
    scaler->setJitterOffsetY(-desc.jitter[1]);

    scaler->setWorldToViewMatrix(simd_of(desc.world_to_view));
    scaler->setViewToClipMatrix(simd_of(desc.view_to_clip));

    scaler->setShouldResetHistory(desc.reset);
    scaler->setFence(native.fence());
    scaler->encodeToCommandBuffer(native.command_buffer());
    return true;
}
} // namespace sr::impl
