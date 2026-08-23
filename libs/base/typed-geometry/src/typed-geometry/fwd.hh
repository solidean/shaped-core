#pragma once

/// Aggregate forward declarations for all of typed-geometry.
/// Each module owns its own `fwd.hh`, holding its forward decls and dimensional/typed aliases, and this header only pulls them together.
/// Include a single module's fwd directly when that is all you need.

#include <clean-core/record/domain_fwd.hh>
#include <typed-geometry/geometry/fwd.hh>
#include <typed-geometry/linalg/fwd.hh>
#include <typed-geometry/scalar/fwd.hh>
#include <typed-geometry/transform/fwd.hh>

namespace tg
{
/// The domain every recording site in typed-geometry is attributed to.
CC_REC_DECLARE_DOMAIN(g_rec_domain);
} // namespace tg
