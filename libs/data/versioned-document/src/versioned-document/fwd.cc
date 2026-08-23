#include "fwd.hh"

#include <clean-core/record/domain.hh>

// Beyond the domain there are no definitions here — this TU also exists so fwd.hh is proven to compile standalone.

namespace vdoc
{
CC_REC_DEFINE_DOMAIN(g_rec_domain, "vdoc");
} // namespace vdoc
