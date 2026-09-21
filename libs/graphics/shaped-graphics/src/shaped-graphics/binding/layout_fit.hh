#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/fwd.hh>

namespace sg
{
/// What keeps the bindings a shader reflects from fitting a layout, one line each; empty where they fit.
///
/// Every reflected binding must be declared, under its name, by the group at some slot, at the same index, count and kind.
/// A reflected descriptor set must be that slot, and a reflected register space must be the declared binding's space,
/// or the slot where the declaration states none, which is the slot dx12 then puts it in.
/// A declared binding the shader never reads may be missing from the reflection, since compilers strip those.
/// A reflected sampler no group declares is a static sampler, which the layout places itself, and is not judged.
/// `inline_constants` is the layout's inline block, which a target reflects as a uniform buffer or not at all.
///
/// `groups[i]` is the bindings of the group at slot `i`.
[[nodiscard]] cc::string describe_layout_misfit(cc::string_view entry,
                                                cc::span<binding const> reflected,
                                                cc::span<cc::span<binding const> const> groups,
                                                cc::optional<binding> const& inline_constants);

/// The same, against a pipeline layout.
[[nodiscard]] cc::string describe_layout_misfit(compiled_shader const& shader, pipeline_layout const& layout);
} // namespace sg
