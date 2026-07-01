#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <shaped-graphics/backend/backend_context.hh>
#include <shaped-graphics/buffer.hh>
#include <shaped-graphics/command_list.hh>
#include <shaped-graphics/context.hh>

namespace sg
{
context::context(std::shared_ptr<backend_context> backend) : _backend(cc::move(backend))
{
    CC_ASSERT(_backend != nullptr, "context requires a backend");
}

backend_kind context::backend() const
{
    return _backend->kind();
}

command_list_handle context::create_command_list()
{
    CC_UNREACHABLE("not implemented yet");
}

buffer_handle context::create_buffer(isize size_in_bytes, buffer_usage usage)
{
    CC_UNREACHABLE("not implemented yet");
}
} // namespace sg
