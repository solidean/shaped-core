#pragma once

#include <clean-core/error/optional.hh>
#include <shaped-graphics/bytes_future.hh>
#include <shaped-viewer/fwd.hh>

/// Where a settled render's image lands: the capture's readback, handed over in place of a file write.
/// Owned by `sv::render_settled`, which outlives the viewer it installs it in.
struct sv::impl::settled_landing
{
    /// The finished image's readback, submitted the frame it settled or ran out of time; empty until then.
    cc::optional<sg::bytes_future> readback;

    /// Whether it settled, rather than ran out of time.
    bool settled = false;
};
