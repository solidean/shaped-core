#include <shaped-graphics/command_list.hh>

namespace sg
{
command_list::~command_list() = default;

command_list::command_list(epoch created_in) : upload(*this), download(*this), _epoch(created_in)
{
    // upload(*this) / download(*this) only store the back-reference; they don't touch any
    // not-yet-constructed member.
}
} // namespace sg
