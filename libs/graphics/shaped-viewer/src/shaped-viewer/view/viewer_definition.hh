#pragma once

#include <clean-core/container/vector.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/layout/layout_tree.hh>
#include <shaped-viewer/view/view_data.hh>

/// A whole frame's worth of work: every view it renders, and the layout nodes those views arrange each other with.
///
/// Both pools are flat and referenced by index, so the recursion — a view holding a layout layer whose leaves name
/// further views — is a graph over two vectors rather than a tree of allocations.
/// That is what lets `build_render_plan` walk it, detect cycles and share a view between two leaves without copying.
///
/// `root_view` is the view the frame's output is drawn from; everything else is reached through it.
struct sv::viewer_definition
{
    cc::vector<view_data> views;
    layout_tree nodes;
    view_index root_view = view_index(0);

    [[nodiscard]] view_data& operator[](view_index i) { return views[u32(i)]; }
    [[nodiscard]] view_data const& operator[](view_index i) const { return views[u32(i)]; }
};
