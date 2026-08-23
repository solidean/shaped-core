#include "fwd.hh"

#include <clean-core/record/domain.hh>

// The library's domain, and a check that fwd.hh compiles standalone.
//
// Here rather than in whichever feature file happened to log first: a domain is declared in fwd.hh, so this is where
// a reader looks for its definition, and moving or splitting a feature file cannot take it with them.

namespace sg
{
CC_REC_DEFINE_DOMAIN(g_rec_domain, "sg");
} // namespace sg
