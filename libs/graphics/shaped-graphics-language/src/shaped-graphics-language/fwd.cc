#include "fwd.hh"

#include <clean-core/record/domain.hh>

// Also checks that fwd.hh compiles standalone.

namespace sgl
{
CC_REC_DEFINE_DOMAIN(g_rec_domain, "sgl");
} // namespace sgl
