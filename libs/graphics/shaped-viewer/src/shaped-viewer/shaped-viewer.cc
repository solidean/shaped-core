// Anchor translation unit for the shaped-viewer static library.
// The real code lives in its own .cc files (camera, resource_managers, the routines, view_renderer, viewer_renderer, shaders);
// this just keeps the umbrella header self-compiling.

#include <clean-core/record/domain.hh>
#include <shaped-viewer/all.hh>

namespace sv
{
CC_REC_DEFINE_DOMAIN(g_rec_domain, "sv");
} // namespace sv
