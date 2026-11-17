#include <clean-core/common/log.hh>
#include <clean-core/platform/environment.hh>
#include <shaped-graphics/context/native_debug_info.hh>

sg::native_debug_info sg::native_debug_info_from_environment(native_debug_info configured)
{
    auto const value = cc::environment_variable("SC_SG_NATIVE_DEBUG_INFO");
    if (!value.has_value())
        return configured;
    auto const level = parse_native_debug_info(value.value());
    if (!level.has_value())
    {
        CC_LOG_WARNING("SC_SG_NATIVE_DEBUG_INFO is '{}', which is none of 'none', 'lines' or 'full'", value.value());
        return configured;
    }
    return level.value();
}

cc::optional<sg::native_debug_info> sg::parse_native_debug_info(cc::string_view value)
{
    if (value == "none")
        return native_debug_info::none;
    if (value == "lines")
        return native_debug_info::lines;
    if (value == "full")
        return native_debug_info::full;
    return {};
}
