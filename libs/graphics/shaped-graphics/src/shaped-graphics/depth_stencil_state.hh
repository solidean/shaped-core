#pragma once

#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/sampler.hh> // compare_op — shared depth/stencil comparison vocabulary

/// Depth and stencil test state baked into a raster pipeline. Reuses `compare_op` (from sampler.hh) for
/// the depth and stencil comparisons. Defaults describe "no depth, no stencil".

namespace sg
{
/// What happens to a stencil value at one point of the test.
enum class stencil_op
{
    keep,            // DX12 STENCIL_OP_KEEP     / Vk STENCIL_OP_KEEP
    zero,            // DX12 STENCIL_OP_ZERO     / Vk STENCIL_OP_ZERO
    replace,         // DX12 STENCIL_OP_REPLACE  / Vk STENCIL_OP_REPLACE (with the dynamic reference value)
    increment_clamp, // DX12 STENCIL_OP_INCR_SAT / Vk STENCIL_OP_INCREMENT_AND_CLAMP
    decrement_clamp, // DX12 STENCIL_OP_DECR_SAT / Vk STENCIL_OP_DECREMENT_AND_CLAMP
    invert,          // DX12 STENCIL_OP_INVERT   / Vk STENCIL_OP_INVERT
    increment_wrap,  // DX12 STENCIL_OP_INCR     / Vk STENCIL_OP_INCREMENT_AND_WRAP
    decrement_wrap,  // DX12 STENCIL_OP_DECR     / Vk STENCIL_OP_DECREMENT_AND_WRAP
};

/// The stencil operations + comparison for one face (front or back). `compare` tests the masked stencil
/// value against the dynamic reference; the ops select what to write on stencil-fail / depth-fail / pass.
struct stencil_face
{
    stencil_op fail = stencil_op::keep;       ///< stencil test failed
    stencil_op depth_fail = stencil_op::keep; ///< stencil passed, depth failed
    stencil_op pass = stencil_op::keep;       ///< both passed
    compare_op compare = compare_op::always;  ///< stencil comparison function
};

/// Depth + stencil test configuration. Defaults leave both disabled — a pipeline with no depth-stencil
/// target uses this as-is. `depth_compare` applies only when `depth_test` is set; the stencil fields
/// apply only when `stencil_test` is set (the write reference is dynamic — cmd.raster.set_stencil_reference).
struct depth_stencil_state
{
    bool depth_test = false;                     ///< enable the depth comparison
    bool depth_write = false;                    ///< write passing fragments' depth
    compare_op depth_compare = compare_op::less; ///< depth comparison function (when depth_test)

    bool stencil_test = false;    ///< enable the stencil test
    u8 stencil_read_mask = 0xFF;  ///< masks the value read for the comparison
    u8 stencil_write_mask = 0xFF; ///< masks the value written back
    stencil_face front = {};
    stencil_face back = {};
};
} // namespace sg
