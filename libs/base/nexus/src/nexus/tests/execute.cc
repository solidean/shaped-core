#include "execute.hh"

#include <nexus/tests/check.hh>
#include <nexus/tests/section.hh>

#include <clean-core/assert-handler.hh>
#include <clean-core/assert.hh>

#include <chrono>
#include <iomanip>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>


namespace nx
{
namespace
{
struct test_section
{
    std::unordered_map<std::string, std::unique_ptr<test_section>> subsections;
    std::vector<test_section*> subsections_ordered;

    test_section* next_open_section = nullptr;
    bool is_done = false;
    int last_visited_in_exec = -1;
    std::source_location location;
    std::string name;

    // associated stats
    int executed_checks = 0;
    int failed_checks = 0;
    std::vector<test_error> errors;
    double duration_seconds = 0.0;

    // accumulates stats for non-leaf sections
    // adds errors for "no checks" and "unreachable subsections"
    // computes "in_considered_failing"
    // populates the result with that
    void finalize_section_to(test_execution::section& sec) const
    {
        sec.name = name;
        sec.location = location;
        sec.is_considered_failing = false;
        sec.executed_checks = executed_checks;
        sec.failed_checks = failed_checks;
        sec.errors = errors;
        sec.duration_seconds = duration_seconds;

        // populate and aggregate subsections
        for (auto subsec : subsections_ordered)
        {
            auto& ssec = sec.subsections.emplace_back();
            subsec->finalize_section_to(ssec);

            // accumulate
            sec.executed_checks += ssec.executed_checks;
            sec.failed_checks += ssec.failed_checks;
            sec.duration_seconds += ssec.duration_seconds;
            for (auto const& e : ssec.errors)
                sec.errors.push_back(e);
            sec.is_considered_failing |= ssec.is_considered_failing;

            // unreachable section
            if (is_done && !subsec->is_done)
            {
                sec.errors.push_back(test_error{
                    .expr = "unreachable section",
                    .location = subsec->location,
                    .extra_lines = {},
                    .expanded = std::format("section \"{}\" was discovered but unreachable from parent", subsec->name),
                });
                sec.is_considered_failing = true;
            }
        }

        // we record missing CHECK/REQUIRE for _all_ sections, even intermediate ones
        if (sec.executed_checks == 0)
        {
            sec.errors.push_back(test_error{
                .expr = "no CHECK/REQUIRE",
                .location = location,
                .extra_lines = {"This is often a bug and can be silenced via CHECK(true)"},
                .expanded = "test did not contain CHECK/REQUIRE",
            });
            sec.is_considered_failing = true;
        }

        // final checks
        sec.is_considered_failing |= sec.failed_checks > 0;
        sec.is_considered_failing |= !errors.empty();
    }
};

struct test_context
{
    nx::test_execution* execution = nullptr;
    nx::test_schedule_config const* config = nullptr;
    std::unique_ptr<test_section> root_section;
    std::vector<test_section*> curr_section;

    // current stats
    int executed_checks = 0;
    int failed_checks = 0;
    std::vector<test_error> errors;

    // the first section we close becomes the current "leaf" section
    // after a run, all checks & errors are associated to the current leaf
    test_section* leaf_section = nullptr;

    int exec_count = 0;
};

// Exception thrown when a REQUIRE fails
struct test_require_failed
{
};

// Exception thrown when a SKIP is encountered
struct test_skipped
{
};

struct test_duplicate_section
{
    std::string name;
    std::source_location location;
};

thread_local std::vector<test_context> g_context_stack;

bool is_section_allowed(std::span<test_section* const> curr_section,
                        std::string const& section_name,
                        nx::test_schedule_config const* config)
{
    // No filter means all sections are allowed
    if (config == nullptr || config->section_filters.empty())
        return true;

    auto const& filter = config->section_filters;

    // Current section path (excluding root at index 0)
    auto const section_path = curr_section.subspan(1);

    // Check existing path elements against filter
    size_t const check_size = std::min(section_path.size(), filter.size());
    for (size_t i = 0; i < check_size; ++i)
    {
        if (section_path[i]->name != filter[i])
            return false;
    }

    // If we've checked all existing sections, check the new section name
    if (section_path.size() < filter.size())
    {
        // The new section should match the next filter element
        if (section_path.size() < filter.size() && section_name != filter[section_path.size()])
            return false;
    }

    return true;
}

void test_execute_begin(nx::test_execution& execution, nx::test_schedule_config const& config)
{
    g_context_stack.push_back(test_context{
        .execution = &execution,
        .config = &config,
        .root_section = std::make_unique<test_section>(),
    });
    g_context_stack.back().root_section->location = execution.instance.declaration->location;
    g_context_stack.back().curr_section.push_back(g_context_stack.back().root_section.get());
}

void test_execute_end()
{
    CC_ASSERT(!g_context_stack.empty(), "should be properly balanced");

    auto& ctx = g_context_stack.back();
    CC_ASSERT(ctx.execution != nullptr, "should always have a valid execution");

    ctx.root_section->finalize_section_to(ctx.execution->root);

    g_context_stack.pop_back();
}

// Operator to string conversion
char const* op_to_string(impl::cmp_op op)
{
    using namespace impl;
    switch (op)
    {
    case cmp_op::none: return "";
    case cmp_op::less: return "<";
    case cmp_op::less_equal: return "<=";
    case cmp_op::greater: return ">";
    case cmp_op::greater_equal: return ">=";
    case cmp_op::equal: return "==";
    case cmp_op::not_equal: return "!=";
    case cmp_op::throws: return "throws";
    case cmp_op::throws_as: return "throws_as";
    case cmp_op::assert_fail: return "assert_fail";
    case cmp_op::asserts: return "asserts";
    case cmp_op::skip: return "skip";
    }
    return "?";
}
} // namespace
} // namespace nx


nx::impl::raii_section_opener nx::impl::test_open_section(std::string name, std::source_location location)
{
    auto& ctx = g_context_stack.back();

    auto& curr_sec = *ctx.curr_section.back();

    // check section filter if provided
    if (!is_section_allowed(ctx.curr_section, name, ctx.config))
    {
        // we do this so early that the subsection is not even actually created
        return raii_section_opener(false);
    }

    // new subsection?
    auto& subsec = curr_sec.subsections[name];
    if (subsec == nullptr)
    {
        subsec = std::make_unique<test_section>();
        subsec->name = name;
        subsec->location = location;
        curr_sec.subsections_ordered.push_back(subsec.get());
    }

    // section opened twice in the same run
    if (subsec->last_visited_in_exec == ctx.exec_count)
        throw test_duplicate_section{
            .name = std::move(name),
            .location = location,
        };
    subsec->last_visited_in_exec = ctx.exec_count;

    // don't execute more sections if a leaf was already executed
    if (ctx.leaf_section != nullptr)
    {
        // but note down that parent could continue here
        curr_sec.next_open_section = subsec.get();
        return raii_section_opener(false);
    }

    // don't execute sections that are fully done
    if (subsec->is_done)
        return raii_section_opener(false);

    // .. otherwise enter it
    ctx.curr_section.push_back(subsec.get());
    subsec->next_open_section = nullptr;
    return raii_section_opener(true);
}

nx::impl::raii_section_opener::raii_section_opener(bool is_opened) : _is_opened(is_opened)
{
}

nx::impl::raii_section_opener::~raii_section_opener()
{
    if (_is_opened)
    {
        auto& ctx = g_context_stack.back();
        auto& subsec = *ctx.curr_section.back();

        CC_ASSERT(ctx.curr_section.size() >= 2, "should always have at least this + root on the stack");

        // if after the section we have no subsecs => found & executed a leaf!
        // (no next open, might have unreachable still)
        // also applies to our way back up
        if (subsec.next_open_section == nullptr)
        {
            if (ctx.leaf_section == nullptr)
                ctx.leaf_section = &subsec;
            subsec.is_done = true;
        }
        else
        {
            // make sure parent knows that children have open sections
            ctx.curr_section[ctx.curr_section.size() - 2]->next_open_section = subsec.next_open_section;
        }

        ctx.curr_section.pop_back();
    }
}

void nx::impl::report_check_result(check_kind kind,
                                   cmp_op op,
                                   std::string expr,
                                   bool passed,
                                   std::vector<std::string> extra_lines,
                                   std::source_location location)
{
    if (g_context_stack.empty())
        return; // No active test context

    auto& ctx = g_context_stack.back();

    // Increment executed checks
    ++ctx.executed_checks;

    // If this is a SKIP, throw to abort test execution (counts as success)
    if (op == cmp_op::skip)
        throw test_skipped{};

    // If the check failed, record it
    if (!passed)
    {
        ++ctx.failed_checks;

        std::string expanded;
        switch (op)
        {
        case cmp_op::none: expanded = std::format("'{}' failed", expr); break;

        case cmp_op::less:
        case cmp_op::less_equal:
        case cmp_op::greater:
        case cmp_op::greater_equal:
        case cmp_op::equal:
        case cmp_op::not_equal:
            if (extra_lines.size() >= 2)
                expanded = std::format("{} {} {}", extra_lines[0], op_to_string(op), extra_lines[1]);
            else
                expanded = "(could not capture expressions)";
            break;

        case cmp_op::throws:
            if (!extra_lines.empty())
                expanded = extra_lines[0];
            else
                expanded = "expression did not throw an exception (but should have)";
            break;

        case cmp_op::throws_as:
            if (!extra_lines.empty())
                expanded = extra_lines[0];
            else
                expanded = "expression did not throw an exception (but should have) or threw the wrong type";
            break;

        case cmp_op::assert_fail:
            if (!extra_lines.empty())
                expanded = extra_lines[0];
            else
                expanded = "assertion failed during test";
            break;

        case cmp_op::asserts:
            if (!extra_lines.empty())
                expanded = extra_lines[0];
            else
                expanded = "assertion should have failed (but did not)";
            break;

        case cmp_op::skip: CC_UNREACHABLE("skip should not produce a test error");
        }

        // Add test error
        ctx.errors.push_back(test_error{
            .expr = std::move(expr),
            .location = location,
            .extra_lines = std::move(extra_lines),
            .expanded = std::move(expanded),
        });

        // If this was a REQUIRE, throw exception to abort test execution
        if (kind == check_kind::require)
            throw test_require_failed{};
    }
}

bool nx::test_execution::is_considered_failing() const
{
    return root.is_considered_failing;
}

int nx::test_schedule_execution::count_total_tests() const
{
    return int(executions.size());
}

int nx::test_schedule_execution::count_failed_tests() const
{
    int failed = 0;
    for (auto const& exec : executions)
    {
        if (exec.is_considered_failing())
        {
            ++failed;
        }
    }
    return failed;
}

int nx::test_schedule_execution::count_total_checks() const
{
    int total = 0;
    for (auto const& exec : executions)
        total += exec.root.executed_checks;
    return total;
}

int nx::test_schedule_execution::count_failed_checks() const
{
    int failed = 0;
    for (auto const& exec : executions)
        failed += exec.root.failed_checks;
    return failed;
}

nx::test_schedule_execution nx::execute_tests(test_schedule const& schedule, test_schedule_config const& config)
{
    test_schedule_execution result;

    if (config.verbose)
    {
        std::cout << "executing " << schedule.instances.size() << " tests\n" << std::flush;
    }

    for (auto const& instance : schedule.instances)
    {
        CC_ASSERT(instance.declaration != nullptr, "instances must be valid");
        CC_ASSERT(instance.declaration->function != nullptr, "instances must be valid");
        test_execution execution;
        execution.instance = instance;

        // Set up test context for check reporting
        test_execute_begin(execution, config);

        // Execute the test function if it exists
        auto section_num = 0;
        auto should_continue = true;
        while (should_continue)
        {
            // CAUTION: a test is allowed to run nested tests, thus growing the context stack here
            {
                auto& ctx = g_context_stack.back();
                ctx.exec_count++;
                ctx.leaf_section = nullptr;
                ctx.root_section->next_open_section = nullptr;
            }

            if (config.verbose)
            {
                if (section_num == 0)
                    std::cout << "  - start \"" << instance.declaration->name << "\"\n" << std::flush;
                else
                    std::cout << "  - start \"" << instance.declaration->name << "\" section " << section_num << '\n'
                              << std::flush;
            }
            section_num++;
            auto const t_section_start = std::chrono::high_resolution_clock::now();

            try
            {
                auto _ = cc::impl::scoped_assertion_handler(
                    [](cc::impl::assertion_info const& info)
                    {
                        // failing assertion has same semantics as REQUIRE -> it aborts
                        nx::impl::report_check_result(impl::check_kind::require, impl::cmp_op::assert_fail,
                                                      info.expression, false, {info.message}, info.location);
                    });

                (*instance.declaration->function)();
            }
            catch (test_require_failed const&) // NOLINT(bugprone-empty-catch)
            {
                // REQUIRE failure already logged in report_check_result, this catch
                // only serves to abort test execution without treating it as a further error
            }
            catch (test_skipped const&) // NOLINT(bugprone-empty-catch)
            {
                // SKIP already counted as a successful check in report_check_result, this catch
                // only serves to abort test execution
            }
            catch (test_duplicate_section const& e)
            {
                g_context_stack.back().errors.push_back(test_error{
                    .expr = std::format("duplicate section: \"{}\"", e.name),
                    .location = e.location,
                    .extra_lines = {},
                    .expanded = std::format("duplicate section: \"{}\"", e.name),
                });
                should_continue = false; // wrong use of test framework
            }
            catch (std::exception const& e)
            {
                g_context_stack.back().errors.push_back(test_error{
                    .expr = std::format("uncaught exception: {}", e.what()),
                    .location = instance.declaration->location,
                    .extra_lines = {},
                    .expanded = std::format("uncaught exception: {}", e.what()),
                });
            }
            catch (...)
            {
                g_context_stack.back().errors.push_back(test_error{
                    .expr = "uncaught unknown exception",
                    .location = instance.declaration->location,
                    .extra_lines = {},
                    .expanded = "uncaught unknown exception",
                });
            }

            // associate stats & errors with leaf
            auto sec = g_context_stack.back().leaf_section;
            if (sec == nullptr)
                sec = g_context_stack.back().root_section.get();
            CC_ASSERT(sec != nullptr, "should always have a leaf section");
            {
                auto& ctx = g_context_stack.back();
                auto const t_section_end = std::chrono::high_resolution_clock::now();
                sec->duration_seconds = std::chrono::duration<double>(t_section_end - t_section_start).count();
                sec->executed_checks = cc::exchange(ctx.executed_checks, 0);
                sec->failed_checks = cc::exchange(ctx.failed_checks, 0);
                sec->errors = cc::exchange(ctx.errors, {});
            }

            // no new sections to execute? we're done
            CC_ASSERT(!g_context_stack.empty(), "test context should still be valid");
            if (g_context_stack.back().root_section->next_open_section == nullptr)
            {
                // so it's not marked as unreachable
                g_context_stack.back().root_section->is_done = true;

                // .. and we're done!
                should_continue = false;
            }
        }

        // Clean up test context
        test_execute_end();

        if (config.verbose)
        {
            double const duration_ms = execution.root.duration_seconds * 1000.0;
            std::cout << "    ... in " << std::fixed << std::setprecision(2) << duration_ms << " ms ("
                      << execution.root.executed_checks << " checks, "      //
                      << execution.root.failed_checks << " failed checks, " //
                      << execution.root.errors.size() << " errors)\n"
                      << std::flush;
        }

        result.executions.push_back(std::move(execution));
    }

    return result;
}
