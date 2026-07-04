#include <shaped-graphics/command_list.copy.hh>
#include <shaped-graphics/command_list.hh>

namespace sg
{
void command_list_copy_scope::buffer_bytes_region(buffer_bytes_copy args)
{
    _cmd.copy_buffer_region(cc::move(args.src), cc::move(args.dst), args.src_offset_in_bytes, args.dst_offset_in_bytes,
                            args.size_in_bytes);
}
} // namespace sg
