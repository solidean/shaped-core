#include <shaped-graphics/context.hh>
#include <shaped-graphics/context.persistent.hh>

namespace sg
{
cc::result<buffer_handle> persistent_scope::create_buffer(isize size_in_bytes,
                                                          buffer_usage usage,
                                                          allocation_info const& alloc)
{
    // Funnels through to the context's backend virtual (persistent_scope is a friend of context). The
    // scope is only a lifetime facade; the real allocation is the backend's create_buffer override.
    return _ctx.create_buffer(size_in_bytes, usage, alloc);
}
} // namespace sg
