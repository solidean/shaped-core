#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/thread/async_ambient.hh>

namespace sg::impl
{
/// Drops a transfer job's hold on the context of whoever enqueued it.
///
/// Called immediately before settling anything that caller may await.
/// A settle resumes the awaiter, and a test that ends right there counts a job still holding its context as async work
/// it left running.
/// An install scope made from the same handle may still be open: nothing may report through it after this call.
inline void release_ambient(cc::async_ambient_handle& ambient)
{
    (void)cc::async_ambient_handle(cc::move(ambient));
}
} // namespace sg::impl
