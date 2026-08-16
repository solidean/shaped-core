#pragma once

#include <clean-core/thread/async_ambient.hh>

// Which test a CHECK belongs to, answerable from any thread — nexus' tag in cc::async's ambient chain.
//
// The tag is the whole public surface: the value behind it is the running test's internal context, so execute.cc is the only place that dereferences it.
// Everything else either installs one (nx::test_thread_scope, for a thread nexus did not start) or carries it without looking.
//
// Attribution is cc's drive-site rule, which is what makes this the SINGLE source of truth rather than a fallback for when the thread-local stack is empty.
// A thread inside blocking_get steals like any worker, so it runs OTHER tests' nodes while its own test sits on its stack — and only the ambient tells them apart.

namespace nx::impl
{
CC_ASYNC_AMBIENT_TAG(test_ambient_tag)
} // namespace nx::impl
