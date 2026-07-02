#pragma once

#include <shaped-graphics/fwd.hh>

namespace sg
{
/// Records GPU work, submitted through the context that created it. Single-use and single-threaded:
/// recorded by one thread, then submitted or dropped once — in the epoch it was opened in (command
/// lists must not span epochs; see libs/graphics/shaped-graphics/docs/concepts/epochs.md).
///
/// Abstract: a backend subclasses it. The recording API (copy/upload/download buffer, ...) lands
/// here with the first milestone.
class command_list
{
public:
    virtual ~command_list();

    /// The epoch this list was opened in. It must be submitted or dropped before that epoch advances.
    [[nodiscard]] epoch created_in_epoch() const { return _epoch; }

protected:
    explicit command_list(epoch created_in);

    epoch _epoch = epoch::invalid;
};
} // namespace sg
