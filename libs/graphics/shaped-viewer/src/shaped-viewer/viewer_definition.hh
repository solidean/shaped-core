#pragma once

#include <clean-core/container/vector.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/view.hh>

namespace sv
{
/// The whole frame's worth of views: everything the renderer should produce this frame.
///
/// One or many views — each renders independently into its own target; how those targets land in windows is
/// the caller's compositing step. This slice drives a single view into a single window.
struct viewer_definition
{
    cc::vector<view> views;
};
} // namespace sv
