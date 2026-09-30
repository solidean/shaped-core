#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>

namespace slib::impl
{
/// What moved from the frozen lines a build baked to the ones a reloaded source states, one `key: was -> is` line
/// each; empty where nothing did.
/// Each line is `key = value`, and a key only one side has reads as `<unset>` on the other.
[[nodiscard]] cc::string frozen_moved(cc::span<cc::string_view const> built, cc::span<cc::string const> now);
} // namespace slib::impl
