#pragma once

// Which lobe of the closure a path took, and the one rule that says whether that makes it specular.
//
// Its own file because two shaders that share nothing else both need it: the closest-hit names the lobe its
// continuation was drawn from, and the raygen reads that name to decide which denoiser signal the rest of the path
// belongs to.
// The raygen has no business including the BSDF — the closure is a thousand lines of lobe math it never evaluates —
// so the vocabulary lives apart from the thing that produces it.

namespace sv
{
/// Which of the six lobes a sample was drawn from, so a caller can tell a sharp bounce from a matte one.
///
/// Only `bsdf_lobe_diffuse` belongs to the diffuse half of `bsdf_eval_split`; everything else is specular, transmission
/// included, for the reason that function gives.
static const uint bsdf_lobe_fuzz = 0;
static const uint bsdf_lobe_coat = 1;
static const uint bsdf_lobe_metal = 2;
static const uint bsdf_lobe_spec = 3;
static const uint bsdf_lobe_diffuse = 4;
static const uint bsdf_lobe_transmission = 5;

/// Whether a drawn lobe belongs to the specular half, which is every lobe but the diffuse substrate's.
/// The one rule, in one place, so the sampler and the evaluator cannot disagree about what a path is.
bool bsdf_lobe_is_specular(uint lobe)
{
    return lobe != bsdf_lobe_diffuse;
}
} // namespace sv
