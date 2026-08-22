#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <nexus/args/fwd.hh>

// Asking the operating system what this process was invoked with.
//
// The fallback for a binary that never went through nx::run — a library reaching for nx::has_arg deep in a
// call stack is the case this exists for.
// When the harness did run, its captured copy is used instead and none of this is touched.
//
// Answering is best-effort by design: a platform with no notion of a command line yields an empty list
// rather than an assertion, because a debug helper must never be the reason a program dies.

namespace nx::impl
{
/// argv for this process, argv[0] included, or empty when the platform cannot say.
/// Materialized on first use rather than at static-init time: this allocates, and running before main is
/// exactly how a static-initialization-order bug is written.
[[nodiscard]] cc::vector<cc::string> const& os_process_args();
} // namespace nx::impl
