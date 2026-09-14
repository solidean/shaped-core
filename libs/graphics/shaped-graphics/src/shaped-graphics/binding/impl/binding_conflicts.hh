#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics/fwd.hh>

namespace sg::impl
{
/// The first disagreement between the stages of one pipeline, as a message, or nothing when they are consistent.
///
/// Stages of a pipeline share one binding interface, so name and address must agree across all of them, both ways:
/// two stages reaching one address must mean the same resource, and one name must live at one address, because a
/// binding_group resolves a name.
/// Neither holds by construction — every stage is reflected from its own module, and nothing has compared them.
///
/// A shader declaring a group its neighbour numbers differently is the usual cause, which is why shaders sharing a
/// group are meant to declare it in one shared header.
[[nodiscard]] cc::optional<cc::string> find_binding_conflict(cc::span<compiled_shader const* const> shaders);
} // namespace sg::impl
