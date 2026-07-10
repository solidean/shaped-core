#include <shaped-graphics/command_list.hh>
#include <shaped-graphics/command_list.upload.hh>
#include <shaped-graphics/impl/texture_copy_region.hh>

namespace sg
{
void command_list_upload_scope::bytes_to_buffer(raw_buffer_handle buffer,
                                                cc::span<cc::byte const> data,
                                                cc::isize offset_in_bytes)
{
    _cmd.upload_bytes_to_buffer(cc::move(buffer), data, offset_in_bytes);
}

void command_list_upload_scope::bytes_to_texture(raw_texture_handle texture,
                                                 cc::span<cc::byte const> pixels,
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
    _cmd.upload_bytes_to_texture(cc::move(texture), pixels, subresource, box);
}
} // namespace sg
