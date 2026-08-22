#include <blob-cache/fwd.hh>
#include <clean-core/record/domain.hh>

// Also checks that fwd.hh compiles standalone.

namespace bcache
{
CC_REC_DEFINE_DOMAIN(g_rec_domain, "bcache");
} // namespace bcache
