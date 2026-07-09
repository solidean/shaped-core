#include <shaped-graphics/command_list.hh>
#include <shaped-graphics/command_list.upload.hh>

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
                                                 subresource_index subresource,
                                                 texture_region region)
{
    _cmd.upload_bytes_to_texture(cc::move(texture), pixels, subresource, region);
}
} // namespace sg
