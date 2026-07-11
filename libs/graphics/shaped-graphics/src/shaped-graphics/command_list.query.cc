#include <shaped-graphics/command_list.hh>
#include <shaped-graphics/command_list.query.hh>

namespace sg
{
bool command_list_query_scope::is_supported() const
{
    return _cmd.query_timestamps_supported();
}

gpu_timestamp command_list_query_scope::record_gpu_timestamp()
{
    return _cmd.query_record_gpu_timestamp();
}
} // namespace sg
