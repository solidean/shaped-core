#include <clean-core/common/utility.hh>
#include <shaped-graphics/context.download.hh>
#include <shaped-graphics/context.hh>

namespace sg
{
bytes_future context_download_scope::bytes_from_buffer(raw_buffer_handle buffer,
                                                       cc::isize offset_in_bytes,
                                                       cc::isize size_in_bytes)
{
    return _ctx.async_download_bytes_from_buffer(cc::move(buffer), offset_in_bytes, size_in_bytes);
}

void context_download_scope::set_async_window_size(cc::isize bytes)
{
    _ctx.set_async_download_window_bytes(bytes);
}

void context_download_scope::set_budget(cc::isize bytes)
{
    _ctx.set_inline_download_budget(bytes);
}
} // namespace sg
