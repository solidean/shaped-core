#pragma once

#include <format> // NOLINT(unused-includes) - used by SECTION macro
#include <source_location>

namespace nx::impl
{
struct raii_section_opener
{
    explicit raii_section_opener(bool is_opened);
    raii_section_opener(raii_section_opener&&) = delete;
    raii_section_opener(raii_section_opener const&) = delete;
    raii_section_opener& operator=(raii_section_opener&&) = delete;
    raii_section_opener& operator=(raii_section_opener const&) = delete;
    ~raii_section_opener();

    explicit operator bool() const { return _is_opened; }

private:
    bool _is_opened = false;
};

// true if this section should be explored
raii_section_opener test_open_section(std::string name, std::source_location location);

} // namespace nx::impl

// usage:
//   SECTION("scenario A")
//   {
//       CHECK(1 + 2 == 3);
//   }
#define SECTION(name, ...)    \
    if (auto _nx_raii_section \
        = ::nx::impl::test_open_section(std::format(name __VA_OPT__(, ) __VA_ARGS__), std::source_location::current()))
