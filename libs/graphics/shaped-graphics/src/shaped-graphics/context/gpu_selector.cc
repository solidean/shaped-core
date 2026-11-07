#include <clean-core/platform/environment.hh>
#include <shaped-graphics/context/gpu_selector.hh>

namespace sg
{
namespace
{
constexpr u32 k_intel_vendor_id = 0x8086;
constexpr u32 k_amd_vendor_id = 0x1002;
constexpr u32 k_nvidia_vendor_id = 0x10DE;

[[nodiscard]] char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
}

[[nodiscard]] bool equals_ignoring_case(cc::string_view a, cc::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (auto i = isize(0); i < a.size(); ++i)
        if (lower(a[i]) != lower(b[i]))
            return false;
    return true;
}

[[nodiscard]] bool contains_ignoring_case(cc::string_view haystack, cc::string_view needle)
{
    for (auto start = isize(0); start + needle.size() <= haystack.size(); ++start)
        if (equals_ignoring_case(haystack.subview({.start = start, .end = start + needle.size()}), needle))
            return true;
    return false;
}
} // namespace
} // namespace sg

cc::string sg::gpu_selector_from_environment()
{
    auto const value = cc::environment_variable("SC_GPU");
    return value.has_value() ? cc::string(value.value()) : cc::string();
}

bool sg::gpu_selector_matches(cc::string_view selector, u32 vendor_id, cc::string_view name)
{
    if (selector.empty())
        return true;
    if (equals_ignoring_case(selector, "intel"))
        return vendor_id == k_intel_vendor_id;
    if (equals_ignoring_case(selector, "amd"))
        return vendor_id == k_amd_vendor_id;
    if (equals_ignoring_case(selector, "nvidia"))
        return vendor_id == k_nvidia_vendor_id;
    return contains_ignoring_case(name, selector);
}
