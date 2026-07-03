#include <clean-core/common/assert.hh>
#include <shaped-graphics/context.hh>
#include <shaped-graphics/context.persistent.hh>

namespace sg
{
cc::result<buffer_handle> context_persistent_scope::create_buffer(isize size_in_bytes,
                                                                  buffer_usage usage,
                                                                  allocation_info const& alloc)
{
    CC_ASSERT(alloc.scope == allocation_scope::persistent, "persistent scope requires a persistent allocation");
    return _ctx.create_buffer(size_in_bytes, usage, alloc);
}
} // namespace sg
