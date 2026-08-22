// typed-geometry is currently header-dominant: the types and operations are templates living in headers.
// This translation unit exists so the library is a normal static target rather than a header-only INTERFACE one.
// Concrete non-template algorithms — float/double-specialized kernels, bigint internals — will be implemented in .cc files like this one as the library grows.

#include <clean-core/record/domain.hh>
#include <typed-geometry/all.hh>

namespace tg
{
CC_REC_DEFINE_DOMAIN(g_rec_domain, "tg");
} // namespace tg
