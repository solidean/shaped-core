#include <shaped-graphics/command_list.hh>

namespace sg
{
command_list::~command_list() = default;

command_list::command_list(epoch created_in, command_list_slot slot)
  : upload(*this), download(*this), copy(*this), compute(*this), _epoch(created_in), _slot(slot)
{
    // The scopes only store the back-reference; they don't touch any not-yet-constructed member.
}
} // namespace sg
