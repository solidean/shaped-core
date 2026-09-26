#include "test_source.hh"

#include <shaped-graphics-language/driver/impl/front_end.hh>
#include <shaped-graphics-language/test/run_tests.hh>

sgl::tested_source sgl::test_source(cc::string_view source, cc::string_view source_name)
{
    auto const front = driver::impl::run_front_end(source, source_name);
    auto result = tested_source{.errors = front.errors, .warnings = front.warnings};

    for (auto const& t : front.module.tests)
        if (t.file == front.program_file())
        {
            ++result.test_count;
            if (t.expects_diagnostics())
                ++result.tests_expecting_diagnostics;
        }
    for (auto const& r :
         test::run_tests(front.module, driver::impl::module_files_of(front), {.file = front.program_file()}))
    {
        // a test without a flat tree did not check, which an error of the front end says already
        if (r.status == test::test_status::not_run)
            continue;
        ++result.tests_run;
        if (r.is_passed())
            ++result.tests_passed;
        else
            result.errors += driver::impl::format_located(front, test::diagnostic_of(front.module, r));
    }

    for (auto const& e : front.module.entry_points)
        result.entry_points.push_back({.name = e.name, .stage = e.entry_stage});
    return result;
}
