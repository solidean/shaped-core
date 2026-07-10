#include <clean-core/common/utility.hh>
#include <shaped-graphics/context.hh>
#include <shaped-graphics/context.upload.hh>
#include <shaped-graphics/impl/texture_copy_region.hh>

namespace sg
{
void context_upload_scope::bytes_to_buffer(raw_buffer_handle buffer,
                                           cc::pinned_data<cc::byte const> data,
                                           cc::isize offset_in_bytes)
{
    _ctx.async_upload_bytes_to_buffer(cc::move(buffer), cc::move(data), offset_in_bytes);
}

void context_upload_scope::bytes_to_texture(raw_texture_handle texture,
                                            cc::pinned_data<cc::byte const> data,
                                            subresource_index const& subresource,
                                            cc::optional<texture_region> region)
{
    // No region copies the whole subresource; a given region is used as-is, bounds-checked, and an empty
    // one is a no-op.
    impl::assert_valid_subresource(texture, subresource);
    texture_region const box = region.has_value() ? region.value() : impl::full_subresource_region(texture, subresource);
    impl::assert_texture_region_in_bounds(texture, subresource, box);
    if (box.is_empty())
        return; // no-op
    _ctx.async_upload_bytes_to_texture(cc::move(texture), cc::move(data), subresource, box);
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
