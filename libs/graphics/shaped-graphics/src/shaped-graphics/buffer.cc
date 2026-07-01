#include <clean-core/common/assert.hh>
#include <shaped-graphics/buffer.hh>

namespace sg
{
buffer::~buffer() = default;

buffer::buffer(isize size_in_bytes, buffer_usage usage) : _size_in_bytes(size_in_bytes), _usage(usage)
{
    CC_ASSERT(size_in_bytes > 0, "buffer size must be positive");
}
} // namespace sg
