// AMD's FidelityFX headers first: they pull in their own C runtime and D3D-free types, and ours must not shadow them.
#include <api/internal/ffx_assert.h>
#include <api/internal/ffx_backends.h>
#include <api/internal/ffx_message.h>
#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/record/log.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/impl/denoise_images.hh>
#include <shaped-rendering/impl/fsr_backend.hh>
#include <shaped-shader-library/shader_asset.hh>
#include <sr_shaders.hh>
#include <typed-geometry/scalar/half_float.hh>
#include <typed-geometry/scalar/scalar.hh>
#include <upscalers/fsr3/include/ffx_fsr3upscaler.h>
#include <upscalers/fsr3/internal/ffx_fsr3upscaler_private.h>
#include <upscalers/fsr3/internal/ffx_fsr3upscaler_shaderblobs.h>

// AMD FSR 3.1's host code, driven through sg.
//
// The host code decides everything about a frame — which passes run, their constants, which of its images each one
// reads and writes, which get cleared — and reaches the GPU only through the `FfxInterface` table filled in below.
// So this file is the mechanical half: sg images for AMD's resource ids, sg pipelines for AMD's passes, and a queue of
// jobs recorded into the caller's command list, with sg inferring every barrier.
//
// Every SDK type stays in this file.
// The host code also leaves a few functions for its backend to define, and they are at the bottom.

namespace sr::impl
{
namespace
{
/// The flags every context is created with, which is what fixes the one permutation of each pass sr builds.
/// Linear HDR radiance in, and the inverted, infinite-far depth fsr_prepare_depth.hlsl writes.
constexpr u32 k_context_flags = FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE | FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED
                              | FFX_FSR3UPSCALER_ENABLE_DEPTH_INFINITE;

/// The near plane FSR is told, and the one fsr_prepare_depth.hlsl divides by: any positive value reads back as the
/// same view depth, and FSR advises against one below 0.075.
constexpr f32 k_near_plane = 0.1f;

/// The far plane FSR is told; with an infinite depth it only has to lie past the near one.
constexpr f32 k_far_plane = 1.0e6f;

/// The permutation of `pass` that the context flags above make AMD's host code ask for.
[[nodiscard]] u32 expected_permutation(FfxFsr3UpscalerPass pass)
{
    auto flags = u32(FSR3UPSCALER_SHADER_PERMUTATION_HDR_COLOR_INPUT)
               | u32(FSR3UPSCALER_SHADER_PERMUTATION_LOW_RES_MOTION_VECTORS)
               | u32(FSR3UPSCALER_SHADER_PERMUTATION_DEPTH_INVERTED);
    if (pass == FFX_FSR3UPSCALER_PASS_ACCUMULATE_SHARPEN)
        flags |= u32(FSR3UPSCALER_SHADER_PERMUTATION_ENABLE_SHARPENING);
    return flags;
}

/// The compiled wrapper for each pass, in `FfxFsr3UpscalerPass` order; null for the deprecated TCR pass, which the
/// host code never builds.
[[nodiscard]] cc::fixed_array<slib::shader_asset_handle, FFX_FSR3UPSCALER_PASS_COUNT> pass_assets()
{
    auto assets = cc::fixed_array<slib::shader_asset_handle, FFX_FSR3UPSCALER_PASS_COUNT>();
    assets[FFX_FSR3UPSCALER_PASS_PREPARE_INPUTS] = shaders::fsr3_prepare_inputs.compute.CS;
    assets[FFX_FSR3UPSCALER_PASS_LUMA_PYRAMID] = shaders::fsr3_luma_pyramid.compute.CS;
    assets[FFX_FSR3UPSCALER_PASS_SHADING_CHANGE_PYRAMID] = shaders::fsr3_shading_change_pyramid.compute.CS;
    assets[FFX_FSR3UPSCALER_PASS_SHADING_CHANGE] = shaders::fsr3_shading_change.compute.CS;
    assets[FFX_FSR3UPSCALER_PASS_PREPARE_REACTIVITY] = shaders::fsr3_prepare_reactivity.compute.CS;
    assets[FFX_FSR3UPSCALER_PASS_LUMA_INSTABILITY] = shaders::fsr3_luma_instability.compute.CS;
    assets[FFX_FSR3UPSCALER_PASS_ACCUMULATE] = shaders::fsr3_accumulate.compute.CS;
    assets[FFX_FSR3UPSCALER_PASS_ACCUMULATE_SHARPEN] = shaders::fsr3_accumulate_sharpen.compute.CS;
    assets[FFX_FSR3UPSCALER_PASS_RCAS] = shaders::fsr3_rcas.compute.CS;
    assets[FFX_FSR3UPSCALER_PASS_DEBUG_VIEW] = shaders::fsr3_debug_view.compute.CS;
    assets[FFX_FSR3UPSCALER_PASS_GENERATE_REACTIVE] = shaders::fsr3_autogen_reactive.compute.CS;
    return assets;
}

/// One tag per pass, so a shader blob can name its pass by address: the host code hands `fpCreatePipeline` only the blob
/// that `fsr3UpscalerGetPermutationBlobByIndex` filled in, and no other trace of which pass it is building.
constexpr u8 k_pass_tags[FFX_FSR3UPSCALER_PASS_COUNT] = {};

/// The sg format an FSR surface format is created as; nullopt for one FSR 3.1 never asks for.
///
/// R16_SNORM has no sg spelling, and only the Lanczos lookup table uses it, which the permutation sr builds never reads.
/// It is created as r16_float with its values converted, so it still means what it says if a future permutation does.
[[nodiscard]] cc::optional<sg::pixel_format> sg_format_of(u32 format)
{
    switch (format)
    {
    case FFX_API_SURFACE_FORMAT_R8_UNORM:
        return sg::pixel_format::r8_unorm;
    case FFX_API_SURFACE_FORMAT_R8_UINT:
        return sg::pixel_format::r8_uint;
    case FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM:
        return sg::pixel_format::rgba8_unorm;
    case FFX_API_SURFACE_FORMAT_R16_FLOAT:
    case FFX_API_SURFACE_FORMAT_R16_SNORM:
        return sg::pixel_format::r16_float;
    case FFX_API_SURFACE_FORMAT_R16_UINT:
        return sg::pixel_format::r16_uint;
    case FFX_API_SURFACE_FORMAT_R16G16_FLOAT:
        return sg::pixel_format::rg16_float;
    case FFX_API_SURFACE_FORMAT_R16G16_UINT:
        return sg::pixel_format::rg16_uint;
    case FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT:
        return sg::pixel_format::rgba16_float;
    case FFX_API_SURFACE_FORMAT_R32_FLOAT:
        return sg::pixel_format::r32_float;
    case FFX_API_SURFACE_FORMAT_R32_UINT:
        return sg::pixel_format::r32_uint;
    case FFX_API_SURFACE_FORMAT_R32G32_FLOAT:
        return sg::pixel_format::rg32_float;
    case FFX_API_SURFACE_FORMAT_R32G32_UINT:
        return sg::pixel_format::rg32_uint;
    case FFX_API_SURFACE_FORMAT_R32G32B32A32_FLOAT:
        return sg::pixel_format::rgba32_float;
    case FFX_API_SURFACE_FORMAT_R32G32B32A32_UINT:
        return sg::pixel_format::rgba32_uint;
    default:
        return {};
    }
}

/// The FSR surface format a caller's sg texture is described as; UNKNOWN for one FSR has no name for.
/// The host code reads only the extent of what it is handed, so an unknown format is not an error.
[[nodiscard]] u32 ffx_format_of(sg::pixel_format format)
{
    switch (format)
    {
    case sg::pixel_format::r8_unorm:
        return FFX_API_SURFACE_FORMAT_R8_UNORM;
    case sg::pixel_format::rgba8_unorm:
        return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    case sg::pixel_format::r16_float:
        return FFX_API_SURFACE_FORMAT_R16_FLOAT;
    case sg::pixel_format::rg16_float:
        return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
    case sg::pixel_format::rgba16_float:
        return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    case sg::pixel_format::r32_float:
        return FFX_API_SURFACE_FORMAT_R32_FLOAT;
    case sg::pixel_format::r32_uint:
        return FFX_API_SURFACE_FORMAT_R32_UINT;
    case sg::pixel_format::rg32_float:
        return FFX_API_SURFACE_FORMAT_R32G32_FLOAT;
    case sg::pixel_format::rgba32_float:
        return FFX_API_SURFACE_FORMAT_R32G32B32A32_FLOAT;
    default:
        return FFX_API_SURFACE_FORMAT_UNKNOWN;
    }
}

[[nodiscard]] bool is_uint_format(sg::pixel_format format)
{
    switch (format)
    {
    case sg::pixel_format::r8_uint:
    case sg::pixel_format::r16_uint:
    case sg::pixel_format::rg16_uint:
    case sg::pixel_format::r32_uint:
    case sg::pixel_format::rg32_uint:
    case sg::pixel_format::rgba32_uint:
        return true;
    default:
        return false;
    }
}

/// The full mip chain of a `width` x `height` image, which FSR asks for with a mip count of 0.
[[nodiscard]] int full_mip_count(int width, int height)
{
    auto levels = 1;
    auto size = cc::max(width, height);
    while (size > 1)
    {
        size /= 2;
        ++levels;
    }
    return levels;
}

[[nodiscard]] cc::string narrow(wchar_t const* text)
{
    auto out = cc::string();
    if (text == nullptr)
        return out;
    for (auto const* c = text; *c != 0; ++c)
        out.push_back(*c < 128 ? char(*c) : '?');
    return out;
}

/// The name a reflected binding goes by, as the host code's tables spell it.
void widen_into(wchar_t (&out)[FFX_RESOURCE_NAME_SIZE], cc::string_view name)
{
    auto const count = cc::min(name.size(), isize(FFX_RESOURCE_NAME_SIZE - 1));
    for (auto i = isize(0); i < count; ++i)
        out[i] = wchar_t(name[i]);
    out[count] = 0;
}

/// The block FSR's constants travel in: a fixed, generously sized POD, since every one of FSR's cbuffers fits it and
/// binding a block larger than the shader declared reads the same.
struct fsr_constants_block
{
    tg::vec4f words[64] = {};
};
} // namespace

/// One stream's FSR.
class fsr_stream
{
public:
    /// One of FSR's resources: an image it created, or a caller's texture registered for the frame.
    struct resource
    {
        sg::texture_2d texture;
        FfxApiResourceDescription description = {};

        /// What the texture has to be filled with before its first use, applied by the next dispatch.
        cc::vector<byte> pending_init;
    };

    /// What `fpCreatePipeline` reported for one pass, in the order the host code indexes a job's bindings by.
    struct pipeline_record
    {
        FfxFsr3UpscalerPass pass = FFX_FSR3UPSCALER_PASS_COUNT;
        cc::vector<cc::string> srv_names;
        cc::vector<cc::string> uav_names;
        cc::vector<cc::string> cb_names;
    };

    sg::context* ctx = nullptr;
    std::shared_ptr<fsr_upscale_routine::programs const> programs;
    tg::vec2i input_extent = tg::vec2i(0, 0);
    tg::vec2i output_extent = tg::vec2i(0, 0);

    FfxInterface backend = {};

    /// Half a megabyte, which is why it is a heap object rather than a member.
    std::unique_ptr<FfxFsr3UpscalerContext> context;
    bool context_live = false;

    /// Every resource the host code created, then the frame's registered ones after `created_count`.
    cc::vector<resource> resources;
    isize created_count = 0;

    cc::vector<pipeline_record> pipelines;
    cc::vector<FfxGpuJobDescription> jobs;

    /// The constants the frame staged, alive until its jobs are recorded; separate allocations, so a pointer the host
    /// code keeps stays valid while later ones are added.
    cc::vector<cc::vector<u32>> staged_constants;

    /// Where the frame's constants live on the GPU: one block per constant buffer a job binds, in job order.
    /// Persistent rather than a transient buffer per job, since sg tracks a persistent buffer from frame to frame and so
    /// orders this frame's copy after the last frame's reads; a transient window recycled across epochs is not tracked.
    sg::buffer<fsr_constants_block> constants;
    isize constants_capacity = 0;

    /// The next block of `constants` a job binds, while the frame's jobs record.
    isize constants_cursor = 0;

    /// The three images FSR 3.1 has the application allocate, and sr's own depth conversion and exposure.
    sg::texture_2d reconstructed_previous_depth;
    sg::texture_2d dilated_depth;
    sg::texture_2d dilated_motion;
    sg::texture_2d device_depth;
    sg::texture_2d exposure;
    f32 exposure_value = -1.0f;

    /// The list the current dispatch records into; set only for its duration.
    sg::command_list* recording = nullptr;

    fsr_stream() = default;
    fsr_stream(fsr_stream const&) = delete;
    fsr_stream& operator=(fsr_stream const&) = delete;

    ~fsr_stream()
    {
        // The host code destroys its pipelines and resources through the backend, which only forgets them here; the
        // images themselves are sg handles and release themselves.
        if (context_live)
            (void)ffxFsr3UpscalerContextDestroy(context.get());
    }

    [[nodiscard]] static fsr_stream& of(FfxInterface* backend)
    {
        return *static_cast<fsr_stream*>(backend->scratchBuffer);
    }

    [[nodiscard]] resource* find(FfxResourceInternal r)
    {
        if (r.internalIndex < 0 || r.internalIndex >= resources.size())
            return nullptr;
        return &resources[r.internalIndex];
    }

    /// Records every scheduled job into `cmd`, in the order the host code scheduled them.
    [[nodiscard]] bool record_jobs(sg::command_list& cmd);

    /// Fills in each image's initial contents the first time a dispatch runs after it was created.
    void apply_pending_inits(sg::command_list& cmd);

private:
    [[nodiscard]] bool record_compute(sg::command_list& cmd, FfxComputeJobDescription const& job);
    [[nodiscard]] bool record_clear(sg::command_list& cmd, FfxClearFloatJobDescription const& job);
};

namespace
{
/// The description the host code reads back for a caller's texture.
[[nodiscard]] FfxApiResourceDescription describe(sg::texture_2d const& texture, u32 usage)
{
    auto d = FfxApiResourceDescription{};
    d.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    d.format = ffx_format_of(texture.format());
    d.width = u32(texture.width());
    d.height = u32(texture.height());
    d.depth = 1;
    d.mipCount = u32(texture.mip_levels());
    d.flags = FFX_API_RESOURCE_FLAGS_NONE;
    d.usage = usage;
    return d;
}

/// A caller's texture as the host code's dispatch description takes it; `resource` points at the handle for the
/// length of the dispatch.
[[nodiscard]] FfxApiResource api_resource(sg::texture_2d const& texture, u32 usage)
{
    auto r = FfxApiResource{};
    if (!is_set(texture))
        return r;
    r.resource = const_cast<sg::texture_2d*>(&texture);
    r.description = describe(texture, usage);
    r.state = usage & FFX_API_RESOURCE_USAGE_UAV ? FFX_API_RESOURCE_STATE_UNORDERED_ACCESS
                                                 : FFX_API_RESOURCE_STATE_COMPUTE_READ;
    return r;
}

// ---------------------------------------------------------------------------------------------------------------
// The FfxInterface table.
// Each entry is what the host code calls; the ones FSR 3.1's upscaler never reaches answer with an error rather than
// pretending, so a future SDK that starts calling one says so.
// ---------------------------------------------------------------------------------------------------------------

FfxVersionNumber get_sdk_version(FfxInterface*)
{
    return FFX_SDK_MAKE_VERSION(FFX_SDK_VERSION_MAJOR, FFX_SDK_VERSION_MINOR, FFX_SDK_VERSION_PATCH);
}

FfxErrorCode get_effect_gpu_memory_usage(FfxInterface*, FfxUInt32, FfxApiEffectMemoryUsage* out)
{
    *out = {};
    return FFX_OK;
}

FfxErrorCode create_backend_context(FfxInterface*, FfxEffect, FfxEffectBindlessConfig*, FfxUInt32* effect_context_id)
{
    *effect_context_id = 0;
    return FFX_OK;
}

FfxErrorCode get_device_capabilities(FfxInterface*, FfxDeviceCapabilities* out)
{
    // The portable permutation, whatever the device: no 16-bit arithmetic, no forced wave64, and a wave width that
    // keeps the host code off the Lanczos lookup table.
    // What each device could run faster is a follow-up once sg reports 16-bit support.
    // libs/graphics/shaped-rendering/docs/TODO.md has it.
    *out = {};
    out->maximumSupportedShaderModel = FFX_SHADER_MODEL_6_2;
    out->waveLaneCountMin = 32;
    out->waveLaneCountMax = 32;
    out->fp16Supported = false;
    return FFX_OK;
}

FfxErrorCode destroy_backend_context(FfxInterface*, FfxUInt32)
{
    return FFX_OK;
}

FfxErrorCode create_resource(FfxInterface* backend,
                             FfxCreateResourceDescription const* description,
                             FfxUInt32,
                             FfxResourceInternal* out)
{
    auto& stream = fsr_stream::of(backend);
    auto const& d = description->resourceDescription;

    if (d.type != FFX_API_RESOURCE_TYPE_TEXTURE2D)
    {
        CC_LOG_ERROR("fsr: the host code asked for a resource of type {}, and only 2D textures are mapped", d.type);
        return FFX_ERROR_INVALID_ARGUMENT;
    }
    auto const format = sg_format_of(d.format);
    if (!format.has_value())
    {
        CC_LOG_ERROR("fsr: the host code asked for surface format {}, which this backend does not map", d.format);
        return FFX_ERROR_INVALID_ARGUMENT;
    }

    auto const width = int(cc::max(d.width, 1u));
    auto const height = int(cc::max(d.height, 1u));
    auto const mips = d.mipCount == 0 ? full_mip_count(width, height) : int(d.mipCount);

    auto entry = fsr_stream::resource{};
    entry.description = d;
    entry.description.mipCount = u32(mips);
    entry.texture = stream.ctx->persistent.create_texture_2d(
        {.format = format.value(),
         .width = width,
         .height = height,
         .mip_levels = mips,
         .usage = sg::texture_usage::texture | sg::texture_usage::image | sg::texture_usage::copy_dst});

    // Initial contents are whole images, tightly packed, and uploaded by the next dispatch: nothing is recording yet.
    auto const texels = isize(width) * isize(height);
    auto const init = description->initData;
    if (init.type == FFX_RESOURCE_INIT_DATA_TYPE_VALUE)
    {
        auto const bytes = texels * isize(sg::format_block_size(format.value()));
        entry.pending_init.resize_to_filled(bytes, byte(init.value));
    }
    else if (init.type == FFX_RESOURCE_INIT_DATA_TYPE_BUFFER && d.format == FFX_API_SURFACE_FORMAT_R16_SNORM)
    {
        // The one conversion: signed-normalized 16-bit values, as the half floats they are created as.
        auto const count = isize(init.size / sizeof(int16_t));
        auto const* const source = static_cast<int16_t const*>(init.buffer);
        entry.pending_init.resize_to_filled(count * isize(sizeof(u16)), byte(0));
        for (auto i = isize(0); i < count; ++i)
        {
            auto const value = cc::max(-1.0f, f32(source[i]) / 32767.0f);
            auto const bits = tg::f16(value).bits();
            cc::memcpy(entry.pending_init.data() + i * isize(sizeof(u16)), &bits, sizeof(u16));
        }
    }
    else if (init.type == FFX_RESOURCE_INIT_DATA_TYPE_BUFFER)
    {
        auto const* const source = static_cast<byte const*>(init.buffer);
        entry.pending_init = cc::vector<byte>::create_copy_of(cc::span<byte const>(source, isize(init.size)));
    }

    CC_ASSERT(stream.created_count == stream.resources.size(), "the host code creates resources only between frames");
    out->internalIndex = int32_t(stream.resources.size());
    stream.resources.push_back(cc::move(entry));
    stream.created_count = stream.resources.size();
    return FFX_OK;
}

FfxErrorCode register_resource(FfxInterface* backend, FfxApiResource const* in, FfxUInt32, FfxResourceInternal* out)
{
    auto& stream = fsr_stream::of(backend);
    auto entry = fsr_stream::resource{};
    if (in->resource != nullptr)
        entry.texture = *static_cast<sg::texture_2d const*>(in->resource);
    entry.description = in->description;
    out->internalIndex = int32_t(stream.resources.size());
    stream.resources.push_back(cc::move(entry));
    return FFX_OK;
}

FfxApiResource get_resource(FfxInterface* backend, FfxResourceInternal resource)
{
    auto& stream = fsr_stream::of(backend);
    auto out = FfxApiResource{};
    if (auto* const r = stream.find(resource); r != nullptr)
    {
        out.resource = &r->texture;
        out.description = r->description;
        out.state = FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    return out;
}

FfxErrorCode unregister_resources(FfxInterface* backend, FfxCommandList, FfxUInt32)
{
    auto& stream = fsr_stream::of(backend);
    stream.resources.resize_down_to(stream.created_count);
    return FFX_OK;
}

FfxErrorCode register_static_resource(FfxInterface*, FfxStaticResourceDescription const*, FfxUInt32)
{
    return FFX_ERROR_BACKEND_API_ERROR;
}

FfxApiResourceDescription get_resource_description(FfxInterface* backend, FfxResourceInternal resource)
{
    auto& stream = fsr_stream::of(backend);
    if (auto const* const r = stream.find(resource); r != nullptr)
        return r->description;
    return {};
}

FfxErrorCode destroy_resource(FfxInterface* backend, FfxResourceInternal resource, FfxUInt32)
{
    auto& stream = fsr_stream::of(backend);
    if (auto* const r = stream.find(resource); r != nullptr)
    {
        r->texture = {};
        r->pending_init.clear();
    }
    return FFX_OK;
}

FfxErrorCode map_resource(FfxInterface*, FfxResourceInternal, void**)
{
    return FFX_ERROR_BACKEND_API_ERROR;
}

FfxErrorCode unmap_resource(FfxInterface*, FfxResourceInternal)
{
    return FFX_ERROR_BACKEND_API_ERROR;
}

FfxErrorCode create_heap(FfxInterface*, FfxCreateHeapDescription const*, FfxUInt32, FfxResourceHeap*)
{
    return FFX_ERROR_BACKEND_API_ERROR;
}

FfxErrorCode destroy_heap(FfxInterface*, FfxResourceHeap, FfxUInt32)
{
    return FFX_OK;
}

FfxErrorCode stage_constant_buffer_data(FfxInterface* backend, void* data, FfxUInt32 size, FfxConstantBuffer* out)
{
    auto& stream = fsr_stream::of(backend);
    auto const words = (size + 3u) / 4u;
    auto& storage = stream.staged_constants.emplace_back();
    storage.resize_to_filled(isize(words), 0u);
    cc::memcpy(storage.data(), data, size);
    out->num32BitEntries = words;
    out->data = storage.data();
    return FFX_OK;
}

FfxErrorCode create_pipeline(FfxInterface* backend,
                             FfxShaderBlob* blob,
                             FfxPipelineDescription const*,
                             FfxUInt32,
                             FfxPipelineState* out)
{
    auto& stream = fsr_stream::of(backend);

    // `fsr3UpscalerGetPermutationBlobByIndex` has already refused a permutation sr does not build, so a blob with no
    // tag is that refusal arriving here.
    if (blob->data == nullptr)
        return FFX_ERROR_INVALID_ARGUMENT;
    auto const pass = FfxFsr3UpscalerPass(blob->data - k_pass_tags);
    auto const& programs = stream.programs->fsr->passes;
    if (isize(pass) >= programs.size() || programs[pass].pipeline == nullptr)
        return FFX_ERROR_INVALID_ARGUMENT;

    auto record = fsr_stream::pipeline_record{.pass = pass};
    *out = {};
    for (auto const& b : programs[pass].bindings)
    {
        if (b.type == sg::binding_type::texture)
        {
            auto& slot = out->srvTextureBindings[out->srvTextureCount++];
            slot.slotIndex = b.index;
            widen_into(slot.name, b.name);
            record.srv_names.push_back(cc::string::create_copy_of(b.name));
        }
        else if (b.type == sg::binding_type::image)
        {
            auto& slot = out->uavTextureBindings[out->uavTextureCount++];
            slot.slotIndex = b.index;
            widen_into(slot.name, b.name);
            record.uav_names.push_back(cc::string::create_copy_of(b.name));
        }
        else if (b.type == sg::binding_type::constants_buffer)
        {
            auto& slot = out->constantBufferBindings[out->constCount++];
            slot.slotIndex = b.index;
            widen_into(slot.name, b.name);
            record.cb_names.push_back(cc::string::create_copy_of(b.name));
        }
    }

    // The pipeline "object" is the record's position, off by one so that no pipeline is ever null.
    out->pipeline = reinterpret_cast<FfxPipeline>(uintptr_t(stream.pipelines.size() + 1));
    stream.pipelines.push_back(cc::move(record));
    return FFX_OK;
}

FfxErrorCode destroy_pipeline(FfxInterface*, FfxPipelineState*, FfxUInt32)
{
    return FFX_OK;
}

FfxErrorCode schedule_gpu_job(FfxInterface* backend, FfxGpuJobDescription const* job)
{
    fsr_stream::of(backend).jobs.push_back(*job);
    return FFX_OK;
}

FfxErrorCode query_next_gpu_job(FfxInterface* backend, FfxGpuJobDescription** out)
{
    auto& jobs = fsr_stream::of(backend).jobs;
    *out = jobs.empty() ? nullptr : &jobs.back();
    return FFX_OK;
}

FfxErrorCode execute_gpu_jobs(FfxInterface* backend, FfxCommandList, FfxUInt32)
{
    auto& stream = fsr_stream::of(backend);
    CC_ASSERT(stream.recording != nullptr, "the host code executes jobs only inside a dispatch");
    auto const ok = stream.record_jobs(*stream.recording);
    stream.jobs.clear();
    stream.staged_constants.clear();
    return ok ? FFX_OK : FFX_ERROR_BACKEND_API_ERROR;
}

void fill_interface(FfxInterface& backend, fsr_stream& stream)
{
    backend = {};
    backend.fpGetSDKVersion = get_sdk_version;
    backend.fpGetEffectGpuMemoryUsage = get_effect_gpu_memory_usage;
    backend.fpCreateBackendContext = create_backend_context;
    backend.fpGetDeviceCapabilities = get_device_capabilities;
    backend.fpDestroyBackendContext = destroy_backend_context;
    backend.fpCreateResource = create_resource;
    backend.fpRegisterResource = register_resource;
    backend.fpGetResource = get_resource;
    backend.fpUnregisterResources = unregister_resources;
    backend.fpRegisterStaticResource = register_static_resource;
    backend.fpGetResourceDescription = get_resource_description;
    backend.fpDestroyResource = destroy_resource;
    backend.fpMapResource = map_resource;
    backend.fpUnmapResource = unmap_resource;
    backend.fpStageConstantBufferDataFunc = stage_constant_buffer_data;
    backend.fpCreatePipeline = create_pipeline;
    backend.fpDestroyPipeline = destroy_pipeline;
    backend.fpScheduleGpuJob = schedule_gpu_job;
    backend.fpExecuteGpuJobs = execute_gpu_jobs;
    backend.fpCreateHeap = create_heap;
    backend.fpDestroyHeap = destroy_heap;
    backend.fpQueryNextGpuJobDesc = query_next_gpu_job;

    // The one back-reference every entry needs; the host code treats the scratch buffer as opaque, and refuses one
    // with no size.
    backend.scratchBuffer = &stream;
    backend.scratchBufferSize = sizeof(stream);
    backend.device = stream.ctx;
}

/// FSR's two samplers, bound per group rather than as static samplers, which the vulkan backend refuses.
[[nodiscard]] cc::optional<sg::named_sampler> sampler_for(cc::string_view name)
{
    auto const clamp = [](sg::sampler_filter filter)
    {
        return sg::sampler{.min_filter = filter,
                           .mag_filter = filter,
                           .mip_filter = filter,
                           .address_u = sg::sampler_address_mode::clamp_edge,
                           .address_v = sg::sampler_address_mode::clamp_edge,
                           .address_w = sg::sampler_address_mode::clamp_edge};
    };
    if (name == "s_PointClamp")
        return sg::named_sampler{.name = cc::string::create_copy_of(name), .sampler = clamp(sg::sampler_filter::nearest)};
    if (name == "s_LinearClamp")
        return sg::named_sampler{.name = cc::string::create_copy_of(name), .sampler = clamp(sg::sampler_filter::linear)};
    return {};
}
} // namespace

void fsr_stream::apply_pending_inits(sg::command_list& cmd)
{
    for (auto i = isize(0); i < created_count; ++i)
    {
        auto& r = resources[i];
        if (r.pending_init.empty() || !is_set(r.texture))
            continue;
        cmd.upload.bytes_to_texture(r.texture.raw(), cc::span<byte const>(r.pending_init));
        r.pending_init.clear();
    }
}

bool fsr_stream::record_compute(sg::command_list& cmd, FfxComputeJobDescription const& job)
{
    auto const index = isize(reinterpret_cast<uintptr_t>(job.pipeline->pipeline)) - 1;
    CC_ASSERT(index >= 0 && index < pipelines.size(), "a job names a pipeline this stream never created");
    auto const& record = pipelines[index];
    auto const& program = programs->fsr->passes[record.pass];

    auto views = cc::vector<sg::named_view>();
    for (auto i = isize(0); i < record.srv_names.size(); ++i)
    {
        auto const* const r = find(job.srvTextures[i].resource);
        if (r == nullptr || !is_set(r->texture))
        {
            CC_LOG_ERROR("fsr: pass {} reads '{}', which is not bound", int(record.pass), record.srv_names[i]);
            return false;
        }
        views.push_back({.name = record.srv_names[i], .view = r->texture.as_texture_view()});
    }
    for (auto i = isize(0); i < record.uav_names.size(); ++i)
    {
        auto const* const r = find(job.uavTextures[i].resource);
        if (r == nullptr || !is_set(r->texture))
        {
            CC_LOG_ERROR("fsr: pass {} writes '{}', which is not bound", int(record.pass), record.uav_names[i]);
            return false;
        }
        // FSR's downsampler binds six mip views whatever the image's size, and writes only as many as its constants
        // say the chain has; below 64 pixels the chain is shorter than six, and the views past its end go unwritten.
        // They are bound to the last mip that exists, where AMD's own backend binds whatever descriptor follows.
        auto const mip = cc::min(int(job.uavTextures[i].mip), r->texture.mip_levels() - 1);
        views.push_back({.name = record.uav_names[i], .view = r->texture.as_any_image_view({.mip = mip})});
    }
    for (auto i = isize(0); i < record.cb_names.size(); ++i)
    {
        auto const& cb = job.cbs[i];
        CC_ASSERT(isize(cb.num32BitEntries) * 4 <= isize(sizeof(fsr_constants_block)), "an FSR constant buffer outgrew "
                                                                                       "the block it travels in");
        CC_ASSERT(constants_cursor < constants_capacity, "a job binds more constant buffers than record_jobs uploaded");
        views.push_back({.name = record.cb_names[i], .view = constants.as_constants_buffer(constants_cursor)});
        ++constants_cursor;
    }

    auto samplers = cc::vector<sg::named_sampler>();
    for (auto const& b : program.bindings)
        if (b.type == sg::binding_type::sampler)
            if (auto s = sampler_for(b.name); s.has_value())
                samplers.push_back(cc::move(s.value()));

    auto const group = ctx->transient.create_binding_group(program.layout, views, samplers);
    cmd.compute.bind_pipeline(*program.pipeline);
    cmd.compute.bind_group(0, *group);
    cmd.compute.dispatch_groups(int(job.dimensions[0]), int(job.dimensions[1]), int(cc::max(job.dimensions[2], 1u)));
    return true;
}

bool fsr_stream::record_clear(sg::command_list& cmd, FfxClearFloatJobDescription const& job)
{
    auto const* const r = find(job.target);
    if (r == nullptr || !is_set(r->texture))
        return true; // nothing there to clear, as for an optional input the caller left out

    // Mip 0 only, as AMD's own backend clears through the view of the first mip.
    auto const target = r->texture.as_any_image_view({.mip = 0});
    if (is_uint_format(r->texture.format()))
    {
        auto bits = u32(0);
        cc::memcpy(&bits, &job.color[0], sizeof(bits));
        auto const group = ctx->transient.create_binding_group(cmd, programs->clear_uint.layout,
                                                               shaders::fsr_clear_uint_bindings{.gTarget = target});
        cmd.compute.bind_pipeline(*programs->clear_uint.pipeline);
        cmd.compute.bind<shaders::fsr_clear_uint_bindings>(*group);
        cmd.compute.set_inline_constants(shaders::fsr_clear_uint_constants{.value = bits});
    }
    else
    {
        auto const group = ctx->transient.create_binding_group(cmd, programs->clear_float.layout,
                                                               shaders::fsr_clear_float_bindings{.gTarget = target});
        cmd.compute.bind_pipeline(*programs->clear_float.pipeline);
        cmd.compute.bind<shaders::fsr_clear_float_bindings>(*group);
        cmd.compute.set_inline_constants(
            shaders::fsr_clear_float_constants{.value = {job.color[0], job.color[1], job.color[2], job.color[3]}});
    }
    cmd.compute.dispatch_threads(r->texture.width(), r->texture.height(), 1);
    return true;
}

bool fsr_stream::record_jobs(sg::command_list& cmd)
{
    // Every job's constants first, in one copy, so the frame's dispatches read them with no copy between them.
    auto blocks = cc::vector<fsr_constants_block>();
    for (auto const& job : jobs)
    {
        if (job.jobType != FFX_GPU_JOB_COMPUTE)
            continue;
        auto const& compute = job.computeJobDescriptor;
        auto const index = isize(reinterpret_cast<uintptr_t>(compute.pipeline->pipeline)) - 1;
        for (auto i = isize(0); i < pipelines[index].cb_names.size(); ++i)
        {
            auto& block = blocks.emplace_back();
            auto const bytes = cc::min(isize(compute.cbs[i].num32BitEntries) * 4, isize(sizeof(fsr_constants_block)));
            cc::memcpy(&block, compute.cbs[i].data, bytes);
        }
    }
    if (blocks.size() > constants_capacity)
    {
        constants = ctx->persistent.create_buffer<fsr_constants_block>(
            blocks.size(), sg::buffer_usage::constants_buffer | sg::buffer_usage::copy_dst);
        constants_capacity = blocks.size();
    }
    if (!blocks.empty())
        cmd.upload.bytes_to_buffer(constants.raw(), cc::span<fsr_constants_block const>(blocks).as_bytes());
    constants_cursor = 0;

    for (auto const& job : jobs)
    {
        switch (job.jobType)
        {
        case FFX_GPU_JOB_COMPUTE:
            if (!record_compute(cmd, job.computeJobDescriptor))
                return false;
            break;
        case FFX_GPU_JOB_CLEAR_FLOAT:
            if (!record_clear(cmd, job.clearJobDescriptor))
                return false;
            break;
        case FFX_GPU_JOB_BARRIER:
        case FFX_GPU_JOB_DISCARD:
            // sg infers every barrier from what each dispatch binds, and discarding an aliasable image is only a hint.
            break;
        case FFX_GPU_JOB_COPY:
        default:
            CC_LOG_ERROR("fsr: the host code scheduled a job of type {}, which this backend does not record",
                         int(job.jobType));
            return false;
        }
    }
    return true;
}

bool fsr_passes_available(sg::context const& ctx)
{
    auto const assets = pass_assets();
    for (auto i = isize(0); i < assets.size(); ++i)
    {
        // The deprecated TCR pass has no wrapper, and the host code never builds it.
        if (i == FFX_FSR3UPSCALER_PASS_TCR_AUTOGENERATE)
            continue;
        // A null asset is a package nobody added to a library, which is the same answer as "cannot build".
        if (assets[i] == nullptr || !assets[i]->can_acquire(ctx))
            return false;
    }
    return true;
}

cc::shared_async<std::shared_ptr<fsr_pass_programs const>> fsr_build_passes(sg::context& ctx)
{
    auto const assets = pass_assets();

    auto shaders = cc::vector<sg::async_compiled_shader>();
    for (auto const& asset : assets)
        shaders.push_back(asset == nullptr ? sg::async_compiled_shader() : asset->acquire(ctx));
    for (auto const& s : shaders)
        if (s != nullptr)
            co_await cc::async_settled(s);

    auto built = std::make_shared<fsr_pass_programs>();
    built->passes.resize_to_defaulted(shaders.size());
    auto pipelines = cc::vector<sg::async_compute_pipeline>::create_defaulted(shaders.size());
    for (auto i = isize(0); i < shaders.size(); ++i)
    {
        if (shaders[i] == nullptr)
            continue;
        auto const* const compiled = shaders[i]->try_value();
        if (compiled == nullptr)
        {
            CC_LOG_ERROR("fsr: pass {} did not compile", i);
            co_return nullptr;
        }
        auto& program = built->passes[i];
        program.bindings = compiled->bindings;
        program.layout = ctx.cached.acquire_binding_group_layout(program.bindings);
        auto const pipeline_layout = ctx.cached.acquire_pipeline_layout({.groups = {program.layout}});
        pipelines[i] = ctx.cached.acquire_compute_pipeline({.shader = *compiled, .layout = pipeline_layout});
    }

    for (auto i = isize(0); i < pipelines.size(); ++i)
    {
        if (pipelines[i] == nullptr)
            continue;
        co_await cc::async_settled(pipelines[i]);
        auto const* const pipeline = pipelines[i]->try_value();
        if (pipeline == nullptr)
        {
            CC_LOG_ERROR("fsr: pass {} did not build a pipeline", i);
            co_return nullptr;
        }
        built->passes[i].pipeline = *pipeline;
    }
    co_return built;
}

std::shared_ptr<fsr_stream> fsr_create_stream(sg::context& ctx,
                                              std::shared_ptr<fsr_upscale_routine::programs const> programs,
                                              tg::vec2i input_extent,
                                              tg::vec2i output_extent)
{
    auto stream = std::make_shared<fsr_stream>();
    stream->ctx = &ctx;
    stream->programs = cc::move(programs);
    stream->input_extent = input_extent;
    stream->output_extent = output_extent;
    fill_interface(stream->backend, *stream);

    auto description = FfxFsr3UpscalerContextDescription{};
    description.flags = k_context_flags;
    description.maxRenderSize = {u32(input_extent[0]), u32(input_extent[1])};
    description.maxUpscaleSize = {u32(output_extent[0]), u32(output_extent[1])};
    description.backendInterface = stream->backend;

    stream->context = std::make_unique<FfxFsr3UpscalerContext>();
    if (auto const error = ffxFsr3UpscalerContextCreate(stream->context.get(), &description); error != FFX_OK)
    {
        CC_LOG_ERROR("fsr: the host code refused a {}x{} -> {}x{} context (error {})", input_extent[0], input_extent[1],
                     output_extent[0], output_extent[1], error);
        return nullptr;
    }
    stream->context_live = true;

    // The three images FSR 3.1 has the application allocate, in the formats its own descriptions give.
    auto shared = FfxFsr3UpscalerSharedResourceDescriptions{};
    (void)ffxFsr3UpscalerGetSharedResourceDescriptions(stream->context.get(), &shared);
    auto const make_shared_image = [&](FfxCreateResourceDescription const& d) -> sg::texture_2d
    {
        auto const format = sg_format_of(d.resourceDescription.format);
        CC_ASSERT(format.has_value(), "FSR describes a shared image in a format this backend does not map");
        return ctx.persistent.create_texture_2d({.format = format.value(),
                                                 .width = int(d.resourceDescription.width),
                                                 .height = int(d.resourceDescription.height),
                                                 .usage = sg::texture_usage::texture | sg::texture_usage::image});
    };
    stream->reconstructed_previous_depth = make_shared_image(shared.reconstructedPrevNearestDepth);
    stream->dilated_depth = make_shared_image(shared.dilatedDepth);
    stream->dilated_motion = make_shared_image(shared.dilatedMotionVectors);

    stream->device_depth
        = ctx.persistent.create_texture_2d({.format = sg::pixel_format::r32_float,
                                            .width = input_extent[0],
                                            .height = input_extent[1],
                                            .usage = sg::texture_usage::texture | sg::texture_usage::image});
    stream->exposure
        = ctx.persistent.create_texture_2d({.format = sg::pixel_format::rg32_float,
                                            .width = 1,
                                            .height = 1,
                                            .usage = sg::texture_usage::texture | sg::texture_usage::copy_dst});
    return stream;
}

fsr_upscale_routine::programs const* fsr_stream_programs(fsr_stream const& stream)
{
    return stream.programs.get();
}

bool fsr_dispatch(fsr_stream& stream, sg::command_list& cmd, upscale_inputs const& in, fsr_options const& options, bool reset)
{
    CC_ASSERT(extent_of(in.color) == stream.input_extent && extent_of(in.output) == stream.output_extent,
              "an FSR stream runs at the extents it was created for");

    auto& ctx = cmd.context();
    stream.apply_pending_inits(cmd);

    // The exposure FSR reads its history by, uploaded only when it changes.
    if (in.exposure != stream.exposure_value)
    {
        f32 const texel[2] = {in.exposure, 0.0f};
        cmd.upload.bytes_to_texture(stream.exposure.raw(),
                                    cc::span<byte const>(reinterpret_cast<byte const*>(texel), sizeof(texel)));
        stream.exposure_value = in.exposure;
    }

    // sr's linear view depth, as the inverted device depth the context was created for.
    {
        auto const& pass = stream.programs->prepare_depth;
        auto const group = ctx.transient.create_binding_group(
            cmd, pass.layout,
            shaders::fsr_prepare_depth_bindings{.gLinearDepth = in.depth.as_texture_view(),
                                                .gDeviceDepth = stream.device_depth.as_any_image_view()});
        cmd.compute.bind_pipeline(*pass.pipeline);
        cmd.compute.bind<shaders::fsr_prepare_depth_bindings>(*group);
        cmd.compute.set_inline_constants(shaders::fsr_prepare_depth_constants{.near_plane = k_near_plane});
        cmd.compute.dispatch_threads(stream.input_extent[0], stream.input_extent[1], 1);
    }

    // The vertical field of view, from the projection's y scale; a projection left at identity reads as 90 degrees.
    auto const y_scale = in.view_to_clip[1, 1];
    auto const fov
        = y_scale > 0.0f ? 2.0f * tg::atan(1.0f / y_scale).radians() : tg::angle_f::make_from_degree(90.0f).radians();

    auto d = FfxFsr3UpscalerDispatchDescription{};
    d.commandList = &cmd;
    d.color = api_resource(in.color, FFX_API_RESOURCE_USAGE_READ_ONLY);
    d.depth = api_resource(stream.device_depth, FFX_API_RESOURCE_USAGE_READ_ONLY);
    d.motionVectors = api_resource(in.motion, FFX_API_RESOURCE_USAGE_READ_ONLY);
    d.exposure = api_resource(stream.exposure, FFX_API_RESOURCE_USAGE_READ_ONLY);
    d.dilatedDepth = api_resource(stream.dilated_depth, FFX_API_RESOURCE_USAGE_UAV);
    d.dilatedMotionVectors = api_resource(stream.dilated_motion, FFX_API_RESOURCE_USAGE_UAV);
    d.reconstructedPrevNearestDepth = api_resource(stream.reconstructed_previous_depth, FFX_API_RESOURCE_USAGE_UAV);
    d.output = api_resource(in.output, FFX_API_RESOURCE_USAGE_UAV);

    // sr's jitter is where each sample lies; FSR's is the projection offset that put it there, the other way round.
    d.jitterOffset = {-in.jitter[0], -in.jitter[1]};

    // sr's motion is this frame's pixel minus last frame's, and FSR's the way back, in the same pixels.
    d.motionVectorScale = {-1.0f, -1.0f};

    d.renderSize = {u32(stream.input_extent[0]), u32(stream.input_extent[1])};
    d.upscaleSize = {u32(stream.output_extent[0]), u32(stream.output_extent[1])};
    d.enableSharpening = options.sharpening;
    d.sharpness = cc::clamp(options.sharpness, 0.0f, 1.0f);
    d.frameTimeDelta = options.frame_time_ms;
    d.preExposure = 1.0f;
    d.reset = reset;
    d.cameraNear = k_near_plane;
    d.cameraFar = k_far_plane;
    d.cameraFovAngleVertical = fov;
    d.viewSpaceToMetersFactor = 1.0f;
    d.flags = 0;

    stream.recording = &cmd;
    auto const error = ffxFsr3UpscalerContextDispatch(stream.context.get(), &d);
    stream.recording = nullptr;
    stream.jobs.clear();
    stream.staged_constants.clear();
    stream.resources.resize_down_to(stream.created_count);

    if (error != FFX_OK)
    {
        CC_LOG_ERROR("fsr: the host code refused the dispatch (error {})", error);
        return false;
    }
    return true;
}

tg::vec2f fsr_jitter_offset(u32 frame_index, tg::vec2i input_extent, tg::vec2i output_extent)
{
    auto const phases = ffxFsr3UpscalerGetJitterPhaseCount(input_extent[0], output_extent[0]);
    auto x = 0.0f;
    auto y = 0.0f;
    if (phases <= 0 || ffxFsr3UpscalerGetJitterOffset(&x, &y, int32_t(frame_index % u32(phases)), phases) != FFX_OK)
        return tg::vec2f(0, 0);
    return tg::vec2f(x, y);
}

namespace
{
void log_ffx_message(bool is_error, wchar_t const* message)
{
    auto const text = narrow(message);
    if (is_error)
        CC_LOG_ERROR("fsr: {}", text);
    else
        CC_LOG_WARNING("fsr: {}", text);
}

void log_ffx_assert(char const* file, int32_t line, char const* condition, char const* message)
{
    CC_LOG_ERROR("fsr: assertion '{}' failed at {}:{}{}{}", condition != nullptr ? condition : "", file, line,
                 message != nullptr ? ": " : "", message != nullptr ? message : "");
}

void log_unbuilt_permutation(int pass, u32 options)
{
    CC_LOG_ERROR("fsr: the host code asked for pass {} in permutation {:#x}, which sr does not build", pass, options);
}
} // namespace
} // namespace sr::impl

// ---------------------------------------------------------------------------------------------------------------
// What AMD's host code leaves for its backend to define.
// ---------------------------------------------------------------------------------------------------------------

/// Stands in for AMD's table of precompiled blobs, which the public SDK does not carry.
/// sr compiles each pass itself, so a "blob" is only a tag naming the pass, and any permutation but the one the
/// context flags imply is refused: sr builds no other.
FfxErrorCode fsr3UpscalerGetPermutationBlobByIndex(FfxFsr3UpscalerPass pass, uint32_t options, FfxShaderBlob* out)
{
    *out = {};
    if (pass >= FFX_FSR3UPSCALER_PASS_COUNT || options != sr::impl::expected_permutation(pass))
    {
        sr::impl::log_unbuilt_permutation(pass, options);
        return FFX_ERROR_INVALID_ARGUMENT;
    }
    out->data = &sr::impl::k_pass_tags[pass];
    out->size = 1;
    out->entryName = "CS";
    return FFX_OK;
}

FfxErrorCode fsr3UpscalerIsWave64(uint32_t, bool& is_wave64)
{
    is_wave64 = false;
    return FFX_OK;
}

void ffxSetPrintMessageCallback(ffxMessageCallback, uint32_t)
{
}

/// The host code's own diagnostics, on sr's domain.
void ffxPrintMessage(uint32_t type, wchar_t const* message)
{
    sr::impl::log_ffx_message(type == FFX_API_MESSAGE_TYPE_ERROR, message);
}

bool ffxAssertReport(char const* file, int32_t line, char const* condition, char const* message)
{
    sr::impl::log_ffx_assert(file, line, condition, message);
    return false;
}

void ffxAssertSetPrintingCallback(FfxAssertCallback)
{
}

/// Asked only by the host code's memory-estimate queries, which sr never makes.
FfxErrorCode GetResourceSizeFromDescription(FfxDevice,
                                            FfxCreateResourceDescription const* description,
                                            uint64_t* size,
                                            uint64_t* alignment)
{
    auto const& d = description->resourceDescription;
    *size = uint64_t(d.width) * uint64_t(d.height) * 16u;
    if (alignment != nullptr)
        *alignment = 65536u;
    return FFX_OK;
}
