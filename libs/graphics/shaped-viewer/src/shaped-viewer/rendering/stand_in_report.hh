#pragma once

#include <clean-core/container/span.hh>
#include <shaped-viewer/fwd.hh>

/// What a frame bound in place of the permutations it was handed, as the routine that recorded it reports it.
///
/// A caller folds `hash` into the hash its image restarts on.
/// A substitution that lasts, a material that failed for good, then converges like any other image, and one that ends restarts the image once.
struct sv::stand_in_report
{
    /// 0 where every permutation bound its own shader, and otherwise a hash over the permutations actually bound, in order.
    u64 hash = 0;

    /// Whether a permutation stood in for is still compiling, so the substitution ends by itself.
    /// A frame of one is an image of materials nobody asked for, which must not read as converged.
    bool pending = false;

    /// The report of a frame that bound `bound` for the permutations it was handed, `substituted` saying whether any of them differs.
    [[nodiscard]] static stand_in_report of(cc::span<material_permutation const* const> bound,
                                            bool substituted,
                                            bool pending);
};
