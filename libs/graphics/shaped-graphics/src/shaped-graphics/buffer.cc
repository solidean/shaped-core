#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <shaped-graphics/backend/backend_buffer.hh>
#include <shaped-graphics/buffer.hh>

namespace sg
{
buffer::buffer(isize size_in_bytes, buffer_usage usage, std::shared_ptr<backend_buffer> backend)
  : _size_in_bytes(size_in_bytes), _usage(usage), _backend(cc::move(backend))
{
    CC_ASSERT(size_in_bytes > 0, "buffer size must be positive");
    CC_ASSERT(_backend != nullptr, "buffer requires a backend resource");
}
} // namespace sg
