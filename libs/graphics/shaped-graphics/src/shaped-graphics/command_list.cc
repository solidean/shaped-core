#include <shaped-graphics/command_list.hh>

namespace sg
{
command_list::~command_list() = default;

command_list::command_list(sg::context& ctx, epoch created_in)
  : upload(*this),
    download(*this),
    copy(*this),
    compute(*this),
    raster(*this),
    raytracing(*this),
    query(*this),
    _epoch(created_in),
    _context(&ctx)
{
    // The scopes only store the back-reference; they don't touch any not-yet-constructed member.
}
} // namespace sg
