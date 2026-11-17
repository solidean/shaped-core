#pragma once

#include <clean-core/error/optional.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/types.hh>

/// What a context's SGL binaries carry of the source, overridden by `SC_SG_NATIVE_DEBUG_INFO`: `none`, `lines` or `full`.
///
/// The variable wins over a config, so any program's shaders carry debug info without a change to it, as
/// `SC_SG_CAPTURE` captures any program; anything else is ignored with a warning.
/// Read when a context is created.

namespace sg
{
/// `configured`, or what `SC_SG_NATIVE_DEBUG_INFO` names where it is set.
[[nodiscard]] native_debug_info native_debug_info_from_environment(native_debug_info configured);

/// What a value of `SC_SG_NATIVE_DEBUG_INFO` names, or nothing for a value that names no level.
[[nodiscard]] cc::optional<native_debug_info> parse_native_debug_info(cc::string_view value);
} // namespace sg
