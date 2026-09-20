#pragma once

#include <clean-core/container/map.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/fwd.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-rendering/fwd.hh>
#include <typed-geometry/linalg/mat.hh>
#include <typed-geometry/linalg/vec.hh>

/// One NRD instance and everything it needs to run, for one image stream.
///
/// NRD is a planner rather than a renderer: it answers "which compute dispatches would denoise this frame, against
/// which resources, with which constants", and this executes that answer through sg.
/// So the pipelines are sg compute pipelines built from the DXIL NRD embeds, the scratch textures are sg textures, and
/// the dispatches are ours — which is why this member needs no native scope and runs on any adapter.
///
/// Held in the caller's `sr::denoise_history`, because an NRD instance carries the temporal history and a stream is
/// what owns one.
namespace sr::impl
{
/// Which NRD denoiser a session runs.
///
/// One member rather than the whole catalogue: REBLUR is the recurrent-blur denoiser for diffuse and specular
/// radiance together, which is exactly the pair the tracer now produces.
/// RELAX and the shadow/AO denoisers are the same integration with different identifiers, and can join without this
/// file changing shape.
enum class nrd_denoiser
{
    reblur_diffuse_specular,
};

/// What one frame hands NRD, beside the resources.
///
/// Every matrix is row-major with row vectors, which is what NRD documents and what `tg` produces transposed —
/// `nrd_session` does that transpose, so a caller passes its own convention unchanged.
struct nrd_frame
{
    tg::mat4f world_to_view = tg::mat4f::identity;
    tg::mat4f view_to_clip = tg::mat4f::identity;
    tg::mat4f previous_world_to_view = tg::mat4f::identity;
    tg::mat4f previous_view_to_clip = tg::mat4f::identity;

    /// This frame's sub-pixel offset, in input pixels.
    tg::vec2f jitter = tg::vec2f(0, 0);
    tg::vec2f previous_jitter = tg::vec2f(0, 0);

    /// How many frames this stream has produced, which is what NRD ages its history by.
    u32 frame_index = 0;

    /// Whether the history is meaningless this frame — a first call, a resize, or a camera cut.
    bool reset = false;
};

/// The textures one REBLUR dispatch reads and writes, all at the session's extent.
///
/// The packing is NRD's, not ours: `normal_roughness` is what `NRD_FrontEnd_PackNormalAndRoughness` produces, and the
/// radiance pair carries a hit distance in alpha.
/// `sr::nrd_denoise_routine` is what packs them; this only binds what it is given.
struct nrd_resources
{
    sg::texture_2d motion;           ///< IN_MV, non-jittered, previous minus current
    sg::texture_2d normal_roughness; ///< IN_NORMAL_ROUGHNESS, in NRD's own encoding
    sg::texture_2d view_z;           ///< IN_VIEWZ, linear view depth

    sg::texture_2d diffuse_radiance_hit_distance;  ///< IN_DIFF_RADIANCE_HITDIST
    sg::texture_2d specular_radiance_hit_distance; ///< IN_SPEC_RADIANCE_HITDIST

    sg::texture_2d out_diffuse_radiance_hit_distance;  ///< OUT_DIFF_RADIANCE_HITDIST
    sg::texture_2d out_specular_radiance_hit_distance; ///< OUT_SPEC_RADIANCE_HITDIST
};

/// Owns an NRD instance, its pipelines and its scratch pools.
///
/// Move-only and not copyable: an instance is the history, and two sessions sharing one would each believe they are
/// the continuation.
class nrd_session
{
public:
    nrd_session() = default;
    nrd_session(nrd_session&&) noexcept;
    nrd_session& operator=(nrd_session&&) noexcept;
    nrd_session(nrd_session const&) = delete;
    nrd_session& operator=(nrd_session const&) = delete;
    ~nrd_session();

    /// Brings up an instance for `denoiser` at `extent`, building every pipeline and pool texture it asks for.
    ///
    /// Blocks on nothing and records nothing: the pipelines are acquired through the context's cache, so a first call
    /// reports not-ready and a later one succeeds.
    /// False when NRD refused, which is logged once.
    [[nodiscard]] bool create(sg::context& ctx, nrd_denoiser denoiser, tg::vec2i extent);

    /// Whether every pipeline has finished building, which is what `execute` needs.
    [[nodiscard]] bool is_ready() const;

    [[nodiscard]] bool is_valid() const { return _instance != nullptr; }
    [[nodiscard]] tg::vec2i extent() const { return _extent; }

    /// Records this frame's dispatches onto `cmd`.
    /// False when NRD produced none, or when a pipeline is still building.
    [[nodiscard]] bool execute(sg::command_list& cmd, nrd_frame const& frame, nrd_resources const& resources);

private:
    void _destroy();

    /// The opaque `nrd::Instance*`; this header names no NRD type, for the reason `nrd_instance.hh` gives.
    void* _instance = nullptr;

    sg::context* _ctx = nullptr;
    tg::vec2i _extent = tg::vec2i(0, 0);

    /// One compute pipeline per `nrd::PipelineDesc`, in NRD's own order — `DispatchDesc::pipelineIndex` indexes this.
    ///
    /// Held as the cache's async rather than the handle: a pipeline builds in the background, so a session exists
    /// well before it can run, and `is_ready` is what tells the two apart.
    cc::vector<sg::async_compute_pipeline> _pipelines;
    cc::vector<sg::binding_group_layout_handle> _layouts;

    /// NRD's scratch, in its own order — a `ResourceDesc` naming a pool indexes these.
    cc::vector<sg::texture_2d> _permanent;
    cc::vector<sg::texture_2d> _transient;
};
} // namespace sr::impl
