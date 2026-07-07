#include <clean-core/common/utility.hh>
#include <shaped-graphics/context.hh>
#include <shaped-graphics/context.upload.hh>

namespace sg
{
void context_upload_scope::bytes_to_buffer(buffer_handle buffer,
                                           cc::pinned_data<cc::byte const> data,
                                           cc::isize offset_in_bytes)
{
    _ctx.async_upload_bytes_to_buffer(cc::move(buffer), cc::move(data), offset_in_bytes);
}

void context_upload_scope::set_async_window_size(cc::isize bytes)
{
    _ctx.set_async_upload_window_bytes(bytes);
}

void context_upload_scope::set_inline_budget(cc::isize bytes)
{
    _ctx.set_inline_upload_budget(bytes);
}
} // namespace sg
