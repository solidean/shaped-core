#pragma once

#include <shaped-graphics/fwd.hh>

namespace sg
{
/// Records GPU work, submitted through the context that created it. Single-use and single-threaded:
/// recorded by one thread, then submitted or dropped once.
///
/// Abstract: a backend subclasses it. The recording API (copy/upload/download buffer, ...) lands
/// here with the first milestone.
class command_list
{
public:
    virtual ~command_list();

protected:
    command_list();
};
} // namespace sg
