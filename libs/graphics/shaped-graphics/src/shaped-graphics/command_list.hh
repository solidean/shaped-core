#pragma once

#include <shaped-graphics/fwd.hh>

namespace sg
{
/// Records GPU work for submission through its context. Mutable and single-threaded: a
/// command_list is recorded by one thread at a time.
///
/// This is an abstract interface: a backend subclasses it directly (e.g.
/// sg::backend::vulkan::vulkan_command_list). The recording API (copy_buffer, upload_buffer,
/// download_buffer, ...) lands here as pure-virtual methods with the first milestone.
class command_list
{
public:
    virtual ~command_list();

protected:
    command_list();
};
} // namespace sg
