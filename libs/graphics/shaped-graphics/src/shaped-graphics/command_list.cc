#include <shaped-graphics/command_list.hh>

namespace sg
{
command_list::~command_list() = default;

command_list::command_list(epoch created_in) : _epoch(created_in)
{
}
} // namespace sg
