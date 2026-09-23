#pragma once

#include <clean-core/common/assert.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/raw_buffer.hh>
#include <shaped-graphics/resource/raw_texture.hh>

namespace sg::impl
{
/// The target of ctx.upload, ctx.download and ctx.stream must be persistent.
/// Those copies run on the transfer queue, which nothing orders against the epoch boundary that recycles a transient resource's storage.
/// A transient resource transfers inline instead, through cmd.upload / cmd.download.
inline void assert_async_transfer_target(raw_buffer const& buffer)
{
    CC_ASSERT(buffer.scope() == lifetime_scope::persistent,
              "a transient buffer cannot be the target of ctx.upload, ctx.download or ctx.stream — use cmd.upload / "
              "cmd.download");
}

inline void assert_async_transfer_target(raw_texture const& texture)
{
    CC_ASSERT(texture.scope() == lifetime_scope::persistent,
              "a transient texture cannot be the target of ctx.upload, ctx.download or ctx.stream — use cmd.upload / "
              "cmd.download");
}
} // namespace sg::impl
