#include <clean-core/common/log.hh>
#include <clean-core/container/vector.hh>
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

namespace sr::impl
{
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

/// One device's NGX state, created on the first question and kept for the device's life.
///
/// Per device rather than per process because `NVSDK_NGX_D3D12_Init` takes one, and two contexts on one machine are
/// two devices as often as not.
struct device_state
{
    ID3D12Device* device = nullptr;
    NVSDK_NGX_Parameter* capabilities = nullptr;
    bool initialized = false;
    bool available = false;
};

cc::mutex<cc::vector<device_state>>& states()
{
    static auto s = cc::mutex<cc::vector<device_state>>();
    return s;
}

/// Whether NGX says this driver and adapter can run Ray Reconstruction, initializing NGX for the device on first ask.
///
/// The answer is cached whatever it is: a negative costs an NGX round trip and a positive costs nothing after, and
/// neither can change while a device lives.
bool available_for(ID3D12Device* device)
{
    return states().lock(
        [&](cc::vector<device_state>& all)
        {
            for (auto const& s : all)
                if (s.device == device)
                    return s.available;

            auto state = device_state{.device = device};

            auto const init = NVSDK_NGX_D3D12_Init_with_ProjectID(k_project_id, NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                                  k_engine_version, L".", device);
            if (NVSDK_NGX_FAILED(init))
            {
                // Not an error: a machine without an RTX adapter, or without the runtime beside the binary, lands here
                // and is told `unsupported` exactly as a machine with no SDK is.
                CC_LOG_INFO("dlss: NGX did not initialize ({:#x}); Ray Reconstruction is unavailable", u32(init));
                all.push_back(state);
                return false;
            }
            state.initialized = true;

            if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_GetCapabilityParameters(&state.capabilities)))
            {
                CC_LOG_INFO("dlss: NGX has no capability parameters; Ray Reconstruction is unavailable");
                all.push_back(state);
                return false;
            }

            auto supported = 0;
            if (NVSDK_NGX_FAILED(state.capabilities->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &supported))
                || supported == 0)
            {
                // The driver says which half is missing, and it is worth repeating: "no DLSS" and "your driver is too
                // old for this SDK" are different problems with the same symptom.
                auto needs_driver = 0;
                (void)state.capabilities->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_NeedsUpdatedDriver,
                                              &needs_driver);
                if (needs_driver != 0)
                {
                    auto major = 0u;
                    auto minor = 0u;
                    (void)state.capabilities->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMajor,
                                                  &major);
                    (void)state.capabilities->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMinor,
                                                  &minor);
                    CC_LOG_INFO("dlss: Ray Reconstruction needs driver {}.{} or newer", major, minor);
                }
                else
                {
                    CC_LOG_INFO("dlss: this adapter does not support Ray Reconstruction");
                }
                all.push_back(state);
                return false;
            }

            state.available = true;
            all.push_back(state);
            return true;
        });
}

/// The capability parameter map for a device already known to be available.
NVSDK_NGX_Parameter* capabilities_for(ID3D12Device* device)
{
    return states().lock(
        [&](cc::vector<device_state>& all) -> NVSDK_NGX_Parameter*
        {
            for (auto const& s : all)
                if (s.device == device && s.available)
                    return s.capabilities;
            return nullptr;
        });
}

[[nodiscard]] NVSDK_NGX_PerfQuality_Value quality_of(int quality)
{
    // Ray Reconstruction at a 1:1 render scale still reads these, since the preset also selects the network.
    if (quality <= 0)
        return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    if (quality >= 2)
        return NVSDK_NGX_PerfQuality_Value_MaxQuality;
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
    auto* const dx = dynamic_cast<dx12::dx12_context const*>(&ctx);
    if (dx == nullptr || !dx->_device)
        return false; // another backend; a vendor member needs the native scope dx12 has
    return available_for(dx->_device.Get());
}

void* dlss_create_feature(sg::command_list& cmd, dlss_feature_desc const& desc)
{
    if (!dlss_is_available(cmd.context()))
        return nullptr;

    // No resources: creation reads none, and the scope is opened for the command list and the bind-state reset alone.
    auto const native = dx12::dx12_native_scope::open(cmd, {});
    auto* const params = capabilities_for(native.device());
    if (params == nullptr)
        return nullptr;

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

    // The jitter is already in the motion vectors' frame of reference: the raygen projects a hit through the previous
    // camera at the same sub-pixel offset, so the motion it writes carries no jitter of its own.
    flags |= NVSDK_NGX_DLSS_Feature_Flags_MVJittered;
    create.InFeatureCreateFlags = flags;

    NVSDK_NGX_Handle* handle = nullptr;
    auto const created = NGX_D3D12_CREATE_DLSSD_EXT(native.list(), 1, 1, &handle, params, &create);
    if (NVSDK_NGX_FAILED(created))
    {
        CC_LOG_WARNING("dlss: could not create the Ray Reconstruction feature ({:#x})", u32(created));
        return nullptr;
    }
    return handle;
}

void dlss_release_feature(void* feature)
{
    if (feature == nullptr)
        return;
    (void)NVSDK_NGX_D3D12_ReleaseFeature(static_cast<NVSDK_NGX_Handle*>(feature));
}

bool dlss_evaluate(sg::command_list& cmd, void* feature, dlss_eval_desc const& desc)
{
    if (feature == nullptr)
        return false;

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
    auto* const params = capabilities_for(native.device());
    if (params == nullptr)
        return false;

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

    auto const evaluated
        = NGX_D3D12_EVALUATE_DLSSD_EXT(native.list(), static_cast<NVSDK_NGX_Handle*>(feature), params, &eval);
    if (NVSDK_NGX_FAILED(evaluated))
    {
        CC_LOG_WARNING("dlss: Ray Reconstruction evaluation failed ({:#x})", u32(evaluated));
        return false;
    }
    return true;
}
} // namespace sr::impl
