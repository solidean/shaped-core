#include <clean-core/common/assert.hh>
#include <clean-core/common/macros.hh>
#include <clean-core/platform/environment.hh>

#include <cstdlib>

using namespace cc::primitive_defines;

namespace
{
/// ASCII-only, which is all the values this compares against need.
[[nodiscard]] bool equals_ignoring_case(cc::string_view a, cc::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (auto i = isize(0); i < a.size(); ++i)
    {
        auto const lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; };
        if (lower(a[i]) != lower(b[i]))
            return false;
    }
    return true;
}
} // namespace

cc::optional<cc::string> cc::environment_variable(cc::string_view name)
{
    // getenv wants a null-terminated name, which a string_view is not.
    // Non-const because materializing the terminator is a write.
    auto key = cc::string(name);
    auto const* const value = std::getenv(key.c_str_materialize());
    if (value == nullptr || value[0] == '\0')
        return cc::nullopt;
    return cc::string(value);
}

void cc::set_environment_variable(cc::string_view name, cc::string_view value)
{
    CC_ASSERT(!name.empty(), "an environment variable needs a name");
    CC_ASSERT(name.find('=') < 0, "'=' separates a name from its value and cannot be part of the name");

    if (value.empty())
        return unset_environment_variable(name);

    // Both APIs want null-terminated strings, which a string_view is not.
    auto key = cc::string(name);
    auto owned = cc::string(value);

#ifdef CC_OS_WINDOWS
    _putenv_s(key.c_str_materialize(), owned.c_str_materialize());
#else
    setenv(key.c_str_materialize(), owned.c_str_materialize(), /*overwrite=*/1);
#endif
}

void cc::unset_environment_variable(cc::string_view name)
{
    CC_ASSERT(!name.empty(), "an environment variable needs a name");
    CC_ASSERT(name.find('=') < 0, "'=' separates a name from its value and cannot be part of the name");

    auto key = cc::string(name);

#ifdef CC_OS_WINDOWS
    _putenv_s(key.c_str_materialize(), ""); // an empty value is how the CRT spells removal
#else
    unsetenv(key.c_str_materialize());
#endif
}

bool cc::is_environment_flag_set(cc::string_view name)
{
    auto const value = cc::environment_variable(name);
    if (!value.has_value())
        return false;

    for (auto const negative :
         {cc::string_view("0"), cc::string_view("false"), cc::string_view("no"), cc::string_view("off")})
        if (equals_ignoring_case(value.value(), negative))
            return false;

    return true;
}
