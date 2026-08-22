#pragma once

#include <clean-core/common/macros.hh>

// The one thing cc::async calls into the recording system.
//
// Deliberately its own header, including nothing: it is reached from clean-core/thread/async_ambient.hh, which sits
// under the poll path and must not grow a dependency on the recorder's own headers.
//
// **A poll that does not change the ambient context never calls this.**
// The sites compare against the value they already loaded to restore, so a node without its own token — the
// overwhelming majority in a deep chain — does not even reach the compare.

namespace cc::rec::impl
{
/// Publishes "this thread's ambient context is now `head`", where `head` may be null.
///
/// Out of line and never inlined, so a build that records nothing pays a call on a genuine context change and nothing
/// at all on a poll that does not change one.
CC_DONT_INLINE void note_ambient_change(void* head);
} // namespace cc::rec::impl
