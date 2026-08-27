#include "fwd.hh"

#include <clean-core/record/domain.hh>

// One TU for every domain babel declares, so a format's own .cc stays about the format.
// Also checks that fwd.hh compiles standalone.
//
// Each format gets its own domain rather than sharing babel's, because they are silenced at different times:
// a tool loading meshes has no use for the image decoder's profiling scopes, and vice versa.

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

namespace babel::obj
{
CC_REC_DEFINE_DOMAIN(g_rec_domain, "babel.obj");
} // namespace babel::obj

namespace babel::gltf
{
CC_REC_DEFINE_DOMAIN(g_rec_domain, "babel.gltf");
} // namespace babel::gltf

namespace babel::sqlite
{
CC_REC_DEFINE_DOMAIN(g_rec_domain, "babel.sqlite");
} // namespace babel::sqlite

namespace babel::png
{
CC_REC_DEFINE_DOMAIN(g_rec_domain, "babel.png");
} // namespace babel::png

namespace babel::jpg
{
CC_REC_DEFINE_DOMAIN(g_rec_domain, "babel.jpg");
} // namespace babel::jpg

namespace babel::hdr
{
CC_REC_DEFINE_DOMAIN(g_rec_domain, "babel.hdr");
} // namespace babel::hdr

namespace babel::pfm
{
CC_REC_DEFINE_DOMAIN(g_rec_domain, "babel.pfm");
} // namespace babel::pfm

namespace babel::image
{
CC_REC_DEFINE_DOMAIN(g_rec_domain, "babel.image");
} // namespace babel::image
