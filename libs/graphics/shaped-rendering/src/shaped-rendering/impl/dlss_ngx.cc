#include <clean-core/common/log.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/mutex.hh>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_dlssd_d3d.h>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/backends/dx12/dx12_native_scope.hh>
#include <shaped-rendering/impl/dlss_ngx.hh>

// Ray Reconstruction over NGX, on dx12.
//
// The whole of NVIDIA's SDK is confined to this file: `dlss_ngx.hh` is the seam, and `dlss_null.cc` is the same seam
// for a build that never fetched it.
//
// Every resource NGX touches is named to `dx12_native_scope` first, which is what makes this legal rather than merely
// working: sg cannot see into NGX, so the accesses are declared and sg emits the barriers before the call.
// Closing the scope forgets the list's bind state, because NGX sets its own descriptor heaps and root signature.
//
// `nvsdk_ngx.h`: "Methods in this library are NOT thread safe."
// So every NGX call below runs under `ngx()`'s lock, and a native scope is opened before taking it, never inside.

namespace sr::impl
{
struct dlss_instance
{
    ID3D12Device* device = nullptr;

    /// From `GetCapabilityParameters`, read at open and destroyed at the last close; never written to.
    NVSDK_NGX_Parameter* capabilities = nullptr;

    /// How many `dlss_open` calls this record answers; the last `dlss_close` shuts NGX down.
    int opens = 0;

    /// Set by that last close, after which a stream still holding the record must not call NGX.
    bool closed = false;
};

namespace
{
namespace dx12 = sg::backend::dx12;

/// The project NGX is initialized under.
///
/// A project id rather than an application id: NVIDIA assigns those per shipping title, and a library has none.
/// It must be a GUID — NGX rejects anything else with `FAIL_InvalidParameter`, which is a generic enough code to cost
/// an afternoon — and it must be stable, since NGX keys its per-application settings and OTA model updates on it.
constexpr char const* k_project_id = "2438efd4-b2f3-4abb-aad5-1a97899e0bd4";
constexpr char const* k_engine_version = "0.1";

/// A probe's answer for one adapter.
struct probed_adapter
{
    u64 luid = 0;
    bool available = false;
};

/// Everything process-wide, under the one lock every NGX call takes.
struct ngx_state
{
    cc::vector<std::shared_ptr<dlss_instance>> open;
    cc::vector<probed_adapter> probed;
};

cc::mutex<ngx_state>& ngx()
{
    static auto s = cc::mutex<ngx_state>();
    return s;
}

auto released_streams = cc::atomic<int>(0);

[[nodiscard]] ID3D12Device* device_of(sg::context const& ctx)
{
    auto* const dx = dynamic_cast<dx12::dx12_context const*>(&ctx);
    if (dx == nullptr || !dx->_device)
        return nullptr; // another backend; a vendor member needs the native scope dx12 has
    return dx->_device.Get();
}

[[nodiscard]] u64 luid_of(ID3D12Device* device)
{
    auto const luid = device->GetAdapterLuid();
    return (u64(u32(luid.HighPart)) << 32) | u64(luid.LowPart);
}

/// `NVSDK_NGX_D3D12_Init` for `device`; false when NGX would not start, which is logged.
[[nodiscard]] bool init_locked(ID3D12Device* device)
{
    auto const init = NVSDK_NGX_D3D12_Init_with_ProjectID(k_project_id, NVSDK_NGX_ENGINE_TYPE_CUSTOM, k_engine_version,
                                                          L".", device);
    if (NVSDK_NGX_FAILED(init))
    {
        // Not an error: a machine without an RTX adapter, or without the runtime beside the binary, lands here and is
        // told `unsupported` exactly as a machine with no SDK is.
        CC_LOG_INFO("dlss: NGX did not initialize ({:#x}); Ray Reconstruction is unavailable", u32(init));
        return false;
    }
    return true;
}

/// Whether `capabilities` says this driver and adapter can run Ray Reconstruction, logging why not when not.
[[nodiscard]] bool reads_available(NVSDK_NGX_Parameter* capabilities)
{
    auto supported = 0;
    if (NVSDK_NGX_SUCCEED(capabilities->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &supported))
        && supported != 0)
        return true;

    // The driver says which half is missing, and it is worth repeating: "no DLSS" and "your driver is too old for this
    // SDK" are different problems with the same symptom.
    auto needs_driver = 0;
    (void)capabilities->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_NeedsUpdatedDriver, &needs_driver);
    if (needs_driver != 0)
    {
        auto major = 0u;
        auto minor = 0u;
        (void)capabilities->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMajor, &major);
        (void)capabilities->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMinor, &minor);
        CC_LOG_INFO("dlss: Ray Reconstruction needs driver {}.{} or newer", major, minor);
    }
    else
    {
        CC_LOG_INFO("dlss: this adapter does not support Ray Reconstruction");
    }
    return false;
}

/// Opens NGX, reads availability, and shuts it down again.
/// Only for a device no record is open for, since `Shutdown1` would close that record's instance too.
[[nodiscard]] bool probe_locked(ID3D12Device* device)
{
    if (!init_locked(device))
        return false;

    auto available = false;
    NVSDK_NGX_Parameter* capabilities = nullptr;
    if (NVSDK_NGX_SUCCEED(NVSDK_NGX_D3D12_GetCapabilityParameters(&capabilities)) && capabilities != nullptr)
    {
        available = reads_available(capabilities);
        (void)NVSDK_NGX_D3D12_DestroyParameters(capabilities);
    }
    else
    {
        CC_LOG_INFO("dlss: NGX has no capability parameters; Ray Reconstruction is unavailable");
    }

    (void)NVSDK_NGX_D3D12_Shutdown1(device);
    return available;
}

[[nodiscard]] NVSDK_NGX_PerfQuality_Value quality_of(denoise_quality quality)
{
    // Ray Reconstruction at a 1:1 render scale still reads these, since the preset also selects the network.
    switch (quality)
    {
    case denoise_quality::fast:
        return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    case denoise_quality::balanced:
        return NVSDK_NGX_PerfQuality_Value_Balanced;
    case denoise_quality::best:
        return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    }
    return NVSDK_NGX_PerfQuality_Value_Balanced;
}

/// One declared access, for the scope that brackets the NGX call.
[[nodiscard]] dx12::native_texture_access read_access(sg::texture_2d const& t)
{
    return {.texture = t.raw(), .access = sg::access_flag::shader_read, .stages = sg::pipeline_stage_flag::compute};
}
} // namespace

bool dlss_is_available(sg::context const& ctx)
{
    auto* const device = device_of(ctx);
    if (device == nullptr)
        return false;

    return ngx().lock(
        [&](ngx_state& s)
        {
            for (auto const& i : s.open)
                if (i->device == device)
                    return true;

            auto const luid = luid_of(device);
            for (auto const& p : s.probed)
                if (p.luid == luid)
                    return p.available;

            auto const available = probe_locked(device);
            s.probed.push_back({.luid = luid, .available = available});
            return available;
        });
}

std::shared_ptr<dlss_instance> dlss_open(sg::context& ctx)
{
    auto* const device = device_of(ctx);
    if (device == nullptr)
        return nullptr;

    return ngx().lock(
        [&](ngx_state& s) -> std::shared_ptr<dlss_instance>
        {
            for (auto const& i : s.open)
            {
                if (i->device == device)
                {
                    ++i->opens;
                    return i;
                }
            }

            if (!init_locked(device))
                return nullptr;

            NVSDK_NGX_Parameter* capabilities = nullptr;
            if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_GetCapabilityParameters(&capabilities)) || capabilities == nullptr)
            {
                CC_LOG_INFO("dlss: NGX has no capability parameters; Ray Reconstruction is unavailable");
                (void)NVSDK_NGX_D3D12_Shutdown1(device);
                return nullptr;
            }
            if (!reads_available(capabilities))
            {
                (void)NVSDK_NGX_D3D12_DestroyParameters(capabilities);
                (void)NVSDK_NGX_D3D12_Shutdown1(device);
                return nullptr;
            }

            auto instance = std::make_shared<dlss_instance>();
            instance->device = device;
            instance->capabilities = capabilities;
            instance->opens = 1;
            s.open.push_back(instance);
            return instance;
        });
}

void dlss_close(std::shared_ptr<dlss_instance> const& instance)
{
    if (instance == nullptr)
        return;

    ngx().lock(
        [&](ngx_state& s)
        {
            CC_ASSERT(!instance->closed && instance->opens > 0, "a DLSS instance was closed more often than opened");
            if (--instance->opens > 0)
                return;

            (void)NVSDK_NGX_D3D12_DestroyParameters(instance->capabilities);
            instance->capabilities = nullptr;
            (void)NVSDK_NGX_D3D12_Shutdown1(instance->device);
            instance->closed = true;

            for (auto i = s.open.size() - 1; i >= 0; --i)
                if (s.open[i] == instance)
                    s.open.remove_at(i);
        });
}

dlss_stream* dlss_create_stream(sg::command_list& cmd,
                                std::shared_ptr<dlss_instance> const& instance,
                                dlss_feature_desc const& desc)
{
    if (instance == nullptr)
        return nullptr;

    // No resources: creation reads none, and the scope is opened for the command list and the bind-state reset alone.
    auto const native = dx12::dx12_native_scope::open(cmd, {});
    CC_ASSERT(native.device() == instance->device, "a DLSS stream was created on another device than its instance");

    auto create = NVSDK_NGX_DLSSD_Create_Params{};
    create.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;

    // Our roughness is its own texture rather than packed into the normals' alpha, which is what the tracer writes.
    create.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Unpacked;

    // Linear view depth, which is what the depth guide holds — the raygen has no hardware depth buffer to hand over.
    create.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_Linear;

    create.InWidth = u32(desc.input_extent[0]);
    create.InHeight = u32(desc.input_extent[1]);
    create.InTargetWidth = u32(desc.output_extent[0]);
    create.InTargetHeight = u32(desc.output_extent[1]);
    create.InPerfQualityValue = quality_of(desc.quality);

    auto flags = int(NVSDK_NGX_DLSS_Feature_Flags_None);
    if (desc.hdr)
        flags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;

    // No MVJittered: `reconstruct_guide::motion` is measured from where the jittered sample was traced, so it carries
    // no jitter offset, and a still camera reports zero.
    create.InFeatureCreateFlags = flags;

    return ngx().lock(
        [&](ngx_state&) -> dlss_stream*
        {
            if (instance->closed)
            {
                CC_LOG_ERROR("dlss: a stream was created on an instance already closed");
                return nullptr;
            }

            NVSDK_NGX_Parameter* params = nullptr;
            if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_AllocateParameters(&params)) || params == nullptr)
            {
                CC_LOG_WARNING("dlss: could not allocate a parameter map for a Ray Reconstruction stream");
                return nullptr;
            }

            NVSDK_NGX_Handle* handle = nullptr;
            auto const created = NGX_D3D12_CREATE_DLSSD_EXT(native.list(), 1, 1, &handle, params, &create);
            if (NVSDK_NGX_FAILED(created))
            {
                (void)NVSDK_NGX_D3D12_DestroyParameters(params);
                CC_LOG_WARNING("dlss: could not create the Ray Reconstruction feature ({:#x})", u32(created));
                return nullptr;
            }

            return new dlss_stream{.feature = handle,
                                   .params = params,
                                   .instance = instance,
                                   .quality = desc.quality,
                                   .hdr = desc.hdr};
        });
}

void dlss_release_stream(dlss_stream* stream)
{
    if (stream == nullptr)
        return;

    ngx().lock(
        [&](ngx_state&)
        {
            if (stream->instance->closed)
            {
                // NGX is shut down for the device, so whatever the stream held went with it, and calling in now would
                // be a call into a closed instance.
                CC_LOG_ERROR("dlss: a denoise history outlived the DLSS routine it was built by; its stream is dropped "
                             "without release");
                return;
            }
            (void)NVSDK_NGX_D3D12_ReleaseFeature(static_cast<NVSDK_NGX_Handle*>(stream->feature));
            (void)NVSDK_NGX_D3D12_DestroyParameters(static_cast<NVSDK_NGX_Parameter*>(stream->params));
            ++released_streams;
        });

    delete stream;
}

int dlss_released_stream_count()
{
    return released_streams.load();
}

bool dlss_evaluate(sg::command_list& cmd, dlss_stream& stream, dlss_eval_desc const& desc)
{
    // Everything NGX reads, plus the one thing it writes.
    // Declaring them is the whole contract of a native scope: sg cannot see the call, so under-declaring here is a
    // barrier that never happens and a result that is wrong on some machines and fine on others.
    dx12::native_texture_access const textures[] = {
        read_access(desc.color),
        read_access(desc.albedo),
        read_access(desc.specular_albedo),
        read_access(desc.normal),
        read_access(desc.roughness),
        read_access(desc.depth),
        read_access(desc.motion),
        {.texture = desc.output.raw(), .access = sg::access_flag::shader_write, .stages = sg::pipeline_stage_flag::compute},
    };

    auto const native = dx12::dx12_native_scope::open(cmd, textures);

    auto eval = NVSDK_NGX_D3D12_DLSSD_Eval_Params{};
    eval.pInColor = native.resource(desc.color.raw());
    eval.pInOutput = native.resource(desc.output.raw());
    eval.pInDepth = native.resource(desc.depth.raw());
    eval.pInMotionVectors = native.resource(desc.motion.raw());
    eval.pInDiffuseAlbedo = native.resource(desc.albedo.raw());
    eval.pInSpecularAlbedo = native.resource(desc.specular_albedo.raw());
    eval.pInNormals = native.resource(desc.normal.raw());
    eval.pInRoughness = native.resource(desc.roughness.raw());

    eval.InJitterOffsetX = desc.jitter[0];
    eval.InJitterOffsetY = desc.jitter[1];
    eval.InReset = desc.reset ? 1 : 0;

    // Already in input pixels, which is what NGX wants and what the motion guide is specified in.
    eval.InMVScaleX = 1.0f;
    eval.InMVScaleY = 1.0f;

    // The whole image; sub-rects are for a caller rendering into a corner of a larger target, which sv never does.
    eval.InRenderSubrectDimensions = {u32(desc.color.width()), u32(desc.color.height())};

    // The scalar rather than the exposure TEXTURE, which is for a caller whose exposure is computed on the GPU.
    // NVIDIA's helper reads 0 as "unset" and substitutes 1, so a caller that never sets one lands where it would
    // anyway — but the zero would be OUR default rather than theirs, and that is the bug this replaced.
    eval.InPreExposure = desc.exposure;

    return ngx().lock(
        [&](ngx_state&)
        {
            if (stream.instance->closed)
            {
                CC_LOG_ERROR("dlss: a stream was evaluated on an instance already closed");
                return false;
            }

            auto const evaluated
                = NGX_D3D12_EVALUATE_DLSSD_EXT(native.list(), static_cast<NVSDK_NGX_Handle*>(stream.feature),
                                               static_cast<NVSDK_NGX_Parameter*>(stream.params), &eval);
            if (NVSDK_NGX_FAILED(evaluated))
            {
                CC_LOG_WARNING("dlss: Ray Reconstruction evaluation failed ({:#x})", u32(evaluated));
                return false;
            }
            return true;
        });
}
} // namespace sr::impl
