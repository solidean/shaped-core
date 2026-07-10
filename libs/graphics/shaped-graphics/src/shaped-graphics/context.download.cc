#include <clean-core/common/utility.hh>
#include <clean-core/container/pinned_data.hh>
#include <shaped-graphics/context.download.hh>
#include <shaped-graphics/context.hh>
#include <shaped-graphics/impl/texture_copy_region.hh>

#include <memory>

namespace sg
{
bytes_future context_download_scope::bytes_from_buffer(raw_buffer_handle buffer,
                                                       cc::isize offset_in_bytes,
                                                       cc::isize size_in_bytes)
{
    return _ctx.async_download_bytes_from_buffer(cc::move(buffer), offset_in_bytes, size_in_bytes);
}

bytes_future context_download_scope::bytes_from_texture(raw_texture_handle texture,
                                                        subresource_index const& subresource,
                                                        cc::optional<texture_region> region)
{
    // No region reads the whole subresource; a given region is used as-is, bounds-checked, and an empty one
    // returns a ready, empty future.
    impl::assert_valid_subresource(texture, subresource);
    texture_region const box = region.has_value() ? region.value() : impl::full_subresource_region(texture, subresource);
    impl::assert_texture_region_in_bounds(texture, subresource, box);
    if (box.is_empty()) // no copy — a ready, empty future
        return bytes_future(cc::pinned_data<cc::byte const>(), std::make_shared<ready_bytes_waiter>());
    return _ctx.async_download_bytes_from_texture(cc::move(texture), subresource, box);
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
