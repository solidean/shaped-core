#include <shaped-graphics/command_list.download.hh>
#include <shaped-graphics/command_list.hh>

namespace sg
{
bytes_future command_list_download_scope::bytes_from_buffer(raw_buffer_handle buffer,
                                                            cc::isize offset_in_bytes,
                                                            cc::isize size_in_bytes)
{
    return _cmd.download_bytes_from_buffer(cc::move(buffer), offset_in_bytes, size_in_bytes);
}
} // namespace sg
