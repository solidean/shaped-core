#pragma once

#include <shaped-viewer/fwd.hh>
#include <typed-geometry/linalg/vec.hh>

/// What a post-process does to the views of the leaf it sits on.
enum class sv::post_process_kind : sv::u8
{
    none,
    wipe,
};

namespace sv
{
// todo: fade, difference, false_color, tonemap — each wants its own entry point in shaders/layout.hlsl
} // namespace sv

/// One combining step applied to a leaf's views before they land in its rect.
///
/// A kind-tagged value rather than a polymorphic object: a definition is rebuilt from scratch every frame, and the
/// render plan it feeds must stay a pure value so it can be built and checked without a GPU.
///
/// Nothing here may reach a trace.
/// Dragging a wipe's split must not restart either side's accumulation, and what enforces that is simply that these
/// fields never reach anything a trace uploads.
struct sv::post_process
{
    post_process_kind kind = post_process_kind::none;

    /// wipe: where the split sits across the leaf, in [0, 1] along the axis `horizontal` picks.
    float split = 0.5f;
    bool horizontal = true;

    /// wipe: the band drawn on the split itself; a width of 0 draws none.
    int separator_width = 1;
    tg::vec4f separator_color = tg::vec4f(1, 1, 1, 1);
};

namespace sv
{
/// How many views `kind` combines.
/// A leaf whose view count differs is a caller error the plan builder reports, rather than a draw-time assert.
[[nodiscard]] constexpr int post_process_source_count(post_process_kind kind)
{
    return kind == post_process_kind::wipe ? 2 : 1;
}
} // namespace sv
