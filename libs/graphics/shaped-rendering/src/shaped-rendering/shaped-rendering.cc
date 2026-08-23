// Anchor translation unit for the shaped-rendering static library.
// It keeps the target well-formed whatever set of routines is compiled in; each lands in its own .cc.

#include <clean-core/record/domain.hh>
#include <shaped-rendering/all.hh>

namespace sr
{
CC_REC_DEFINE_DOMAIN(g_rec_domain, "sr");
} // namespace sr
