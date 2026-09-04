#include "fwd.hh"

#include <clean-core/record/domain.hh>

// One TU for every domain babel-data declares, so a format's own .cc stays about the format.
// Also checks that fwd.hh compiles standalone.
//
// Each format gets its own domain rather than sharing babel's, because they are silenced at different times.
// `babel` itself is defined here, not in babel-serializer, because this is the target every babel consumer links.

namespace babel
{
CC_REC_DEFINE_DOMAIN(g_rec_domain, "babel");
} // namespace babel

namespace babel::json
{
CC_REC_DEFINE_DOMAIN(g_rec_domain, "babel.json");
} // namespace babel::json

namespace babel::markdown
{
CC_REC_DEFINE_DOMAIN(g_rec_domain, "babel.markdown");
} // namespace babel::markdown
