#pragma once

#include <shaped-graphics/backend/fwd.hh>
#include <shaped-graphics/fwd.hh>

#include <memory>

namespace sg
{
/// Records GPU work for submission through its context. Mutable and single-threaded: a
/// command_list is recorded by one thread at a time. It wraps a backend_command_list and adds
/// sg-generic validation before each backend call.
class command_list
{
public:
    explicit command_list(std::shared_ptr<backend_command_list> backend);

    // Recording API (copy_buffer, upload_buffer, download_buffer, ...) lands here next — this is
    // the first milestone: command-list buffer upload/download/copy.

private:
    std::shared_ptr<backend_command_list> _backend;
};
} // namespace sg
