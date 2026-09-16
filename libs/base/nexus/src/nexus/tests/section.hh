#pragma once

#include <clean-core/platform/source_location.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>

namespace nx::impl
{
struct test_context;
struct test_section;

struct raii_section_opener
{
    raii_section_opener() = default; // not entered
    raii_section_opener(test_context& ctx, test_section& entered) : _ctx(&ctx), _entered(&entered) {}
    raii_section_opener(raii_section_opener&&) = delete;
    raii_section_opener(raii_section_opener const&) = delete;
    raii_section_opener& operator=(raii_section_opener&&) = delete;
    raii_section_opener& operator=(raii_section_opener const&) = delete;
    ~raii_section_opener();

    explicit operator bool() const { return _entered != nullptr; }

private:
    // Carried rather than looked up again on close: an async body's frame can be destroyed outside any poll, where no
    // test is installed on the thread.
    test_context* _ctx = nullptr;
    test_section* _entered = nullptr;
};

// true if this section should be explored
raii_section_opener test_open_section(cc::string name, cc::source_location location);

} // namespace nx::impl

// usage:
//   SECTION("scenario A")
//   {
//       CHECK(1 + 2 == 3);
//   }
//
// Works the same in an ASYNC_TEST body, which is replayed once per section path like any other.
// There, open sections from the body or from work it awaits one at a time — never from concurrently running work.
#define SECTION(name, ...)    \
    if (auto _nx_raii_section \
        = ::nx::impl::test_open_section(::cc::format(name __VA_OPT__(, ) __VA_ARGS__), cc::source_location::current()))
