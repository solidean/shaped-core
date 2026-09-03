#include <clean-core/record/domain.hh>
#include <clean-net/fwd.hh>

// Also checks that fwd.hh compiles standalone.

namespace cnet
{
CC_REC_DEFINE_DOMAIN(g_rec_domain, "cnet");
} // namespace cnet
