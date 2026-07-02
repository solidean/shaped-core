#include <clean-core/common/assert.hh>
#include <shaped-graphics/buffer.hh>

namespace sg
{
buffer::~buffer() = default;

buffer::buffer(isize size_in_bytes, buffer_usage usage) : _size_in_bytes(size_in_bytes), _usage(usage)
{
    // Zero is allowed — an empty buffer, like an empty span/vector. A backend allocates no GPU
    // storage for it. Only a negative size is programmer misuse.
    CC_ASSERT(size_in_bytes >= 0, "buffer size must be non-negative");
}
} // namespace sg
