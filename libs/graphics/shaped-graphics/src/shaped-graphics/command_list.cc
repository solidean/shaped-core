#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <shaped-graphics/backend/backend_command_list.hh>
#include <shaped-graphics/command_list.hh>

namespace sg
{
command_list::command_list(std::shared_ptr<backend_command_list> backend) : _backend(cc::move(backend))
{
    CC_ASSERT(_backend != nullptr, "command_list requires a backend");
}
} // namespace sg
