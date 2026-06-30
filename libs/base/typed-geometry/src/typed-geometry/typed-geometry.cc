// typed-geometry is currently header-dominant: the types and operations are templates living in
// headers. This translation unit exists so the library is a normal static target rather than a
// header-only INTERFACE one — concrete (non-template) algorithms, e.g. float/double-specialized
// kernels or bigint internals, will be implemented in .cc files like this one as the library grows.

#include <typed-geometry/all.hh>

namespace tg
{
} // namespace tg
