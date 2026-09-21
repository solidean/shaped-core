#pragma once

#include <shaped-graphics/backends/metal/fwd.hh>
#include <shaped-graphics/backends/metal/metal_common.hh>
#include <shaped-graphics/barrier/resource_access.hh>
#include <shaped-graphics/barrier/resource_access_state.hh>
#include <shaped-graphics/fwd.hh>

// Translating sg's backend-neutral access vocabulary into what MTL4 barriers speak.
//
// **Device-free on purpose.** Nothing here touches a device, an encoder or a resource, so its tests run on any machine
// rather than only where a Metal 4 GPU exists — which on a platform with no software adapter is the difference between
// covered and skipped.
// libs/graphics/shaped-graphics/docs/writing-a-backend.md argues for splitting files along exactly this line.
//
// **An MTL4 barrier names stages, not resources.** `barrierAfterEncoderStages:beforeEncoderStages:visibilityOptions:`
// takes two stage masks and a cache-flush option, and nothing else — so the resource list an sg::access_barrier carries
// has nowhere to go, and the texture layouts have no counterpart at all.
// What survives the translation is the stage pair plus whether caches must be flushed.

/// One MTL4 barrier, as the stage pair and visibility an encoder takes.
/// `needed` false means the translation decided nothing has to be emitted.
struct sg::backend::metal::metal_barrier
{
    bool needed = false;
    MTL::Stages after_stages = MTL::StageAll;
    MTL::Stages before_stages = MTL::StageAll;
    MTL4::VisibilityOptions visibility = MTL4::VisibilityOptionDevice;
};

namespace sg::backend::metal
{
/// The MTLStages one sg pipeline stage runs on.
///
/// Metal has no separate stage for indirect argument fetch or for depth/stencil output: an indirect draw's arguments
/// are read by the vertex stage, and depth and colour output both belong to the fragment stage.
/// So the mapping is onto, not one-to-one, and several sg stages fold into `MTLStageFragment`.
[[nodiscard]] MTL::Stages stages_of(sg::pipeline_stage_flag stage);

/// The MTLStages a whole set of sg stages runs on, folded into one mask.
/// An empty set maps to `MTLStageAll` rather than to nothing: "unknown" has to be conservative, and a barrier against
/// no stage at all orders nothing.
[[nodiscard]] MTL::Stages stages_of(sg::pipeline_stage_flags stages);

/// Whether a barrier between these two access sets has to flush caches, or only order execution.
///
/// `MTL4VisibilityOptionDevice` is the flushing form and the default here.
/// The cheap form is only correct when nothing needs to become *visible* — that is, when the source wrote nothing, so
/// the barrier is ordering a read against a read or against a layout change Metal does not have.
[[nodiscard]] MTL4::VisibilityOptions visibility_for(sg::access_flags src_access, sg::access_flags dst_access);

/// The only stages an encoder-scoped barrier on a compute encoder may name.
///
/// `barrierAfterEncoderStages` rejects anything else outright — with validation armed it aborts — because an encoder
/// can only order the stages it is able to encode work for.
/// The queue-scoped form has no such restriction, which is what an entry barrier uses.
inline constexpr MTL::Stages k_compute_encoder_stages
    = MTL::StageDispatch | MTL::StageBlit | MTL::StageAccelerationStructure;

/// The only stages an encoder-scoped barrier on a *render* encoder may name — the same restriction, one encoder over.
///
/// A render encoder encodes the geometry and pixel stages, so a dispatch or blit stage reaching one is dropped exactly
/// as a vertex stage reaching a compute encoder is.
/// `MTLStageTile`, `MTLStageObject` and `MTLStageMesh` are deliberately absent though a render encoder can encode
/// them: `stages_of` cannot produce one — sg has no tile, object or mesh stage — so listing them would only widen the
/// fallback below onto stages no pipeline here has work in.
inline constexpr MTL::Stages k_render_encoder_stages = MTL::StageVertex | MTL::StageFragment;

/// `stages` narrowed to what a compute encoder accepts.
///
/// A mask that clamps to nothing becomes the encoder's whole set rather than an empty one: an empty mask orders
/// nothing, and the stages being dropped are ones this encoder cannot name anyway.
/// That is conservative and legal, where the alternative is a barrier the API refuses.
[[nodiscard]] MTL::Stages clamp_to_compute_encoder(MTL::Stages stages);

/// `stages` narrowed to what a render encoder accepts, the same rule and the same fallback.
///
/// The fallback carries the weight here: a draw reading what a copy or a dispatch wrote names `StageBlit` or
/// `StageDispatch` as its *after* stage, neither of which a render encoder can name.
/// Widening that to every geometry stage orders the draw against more than it strictly must, which is the safe
/// direction — the unsafe one is dropping the barrier.
[[nodiscard]] MTL::Stages clamp_to_render_encoder(MTL::Stages stages);

/// Translate one sg access barrier into the MTL4 form.
///
/// The layouts are dropped rather than mapped, and that is not a gap: a Metal texture has no layout, so
/// `src_layout` / `dst_layout` describe a transition with no counterpart to emit.
/// A barrier whose *only* content was a layout change therefore translates to nothing needed.
[[nodiscard]] metal_barrier translate_barrier(sg::access_barrier const& barrier);
} // namespace sg::backend::metal
