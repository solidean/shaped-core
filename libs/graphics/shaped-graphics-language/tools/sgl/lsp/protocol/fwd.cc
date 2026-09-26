#include "fwd.hh"

#include <clean-core/record/domain.hh>

// Also checks that fwd.hh compiles standalone.

namespace lsp
{
CC_REC_DEFINE_DOMAIN(g_rec_domain, "lsp");
} // namespace lsp
