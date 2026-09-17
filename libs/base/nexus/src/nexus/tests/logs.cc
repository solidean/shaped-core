#include "logs.hh"

#include <clean-core/common/log.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/record/system.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/glob.hh>
#include <clean-core/string/string.hh>
#include <nexus/impl/rec_session.hh>
#include <nexus/tests/execute.hh>

using namespace cc::primitive_defines;

// Inside nx so the verdicts this file logs carry the nexus domain, which the rule never judges.
namespace nx
{
namespace
{
struct log_allowance
{
    cc::rec::level level = {};
    char const* domain = nullptr;
    char const* pattern = nullptr;
    cc::source_location location;
};

/// Filled during static initialization and read-only once a run starts.
cc::vector<log_allowance>& log_allowances()
{
    static cc::vector<log_allowance> allowances;
    return allowances;
}

[[nodiscard]] char const* level_name(cc::rec::level level)
{
    return level == cc::rec::level::error ? "error" : "warning";
}

/// A pattern matches anywhere in the message, and a line pasted out of the console matches itself.
[[nodiscard]] bool matches(cc::string_view pattern, cc::string_view domain, nx::impl::kept_log_record const& record)
{
    if (!domain.empty() && domain != record.domain)
        return false;
    return cc::glob_matches(cc::format("*{}*", pattern), record.text, cc::glob_option::text);
}

void declare(cc::rec::level level,
             cc::string_view pattern,
             cc::string_view domain,
             bool is_expectation,
             int at_least,
             int at_most,
             cc::source_location location)
{
    nx::impl::add_log_declaration({
        .level = level,
        .pattern = cc::string(pattern),
        .domain = cc::string(domain),
        .is_expectation = is_expectation,
        .at_least = at_least,
        .at_most = at_most,
        .location = location,
    });
}

/// Files `error` under the leaf `path` names and every section above it, the way finalize aggregates a pass's own.
void file_error(nx::test_execution& exec, cc::span<int const> path, nx::test_error error)
{
    auto* sec = &exec.root;
    auto chain = cc::vector<nx::test_execution::section*>{sec};
    for (auto const i : path)
    {
        if (i < 0 || i >= sec->subsections.size())
            break;
        sec = &sec->subsections[i];
        chain.push_back(sec);
    }

    for (auto* const s : chain)
    {
        s->errors.push_back(error);
        s->failed_checks += 1;
        s->is_considered_failing = true;
    }
}

void judge_execution(nx::test_execution& exec, bool outermost)
{
    auto const& decl = *exec.instance.declaration;
    auto const allowed_level = decl.test_config.allowed_log_level;

    for (auto const& pass : exec.log_passes)
    {
        if (pass.owner == 0)
            continue;

        auto const records = nx::impl::take_log_records(pass.owner);
        auto counts = cc::vector<int>::create_filled(pass.declarations.size(), 0);

        for (auto const& record : records)
        {
            auto declared = false;
            for (auto i = 0; i < int(pass.declarations.size()); ++i)
            {
                auto const& d = pass.declarations[i];
                if (d.level != record.level || !matches(d.pattern, d.domain, record))
                    continue;
                declared = true;
                ++counts[i];
            }

            if (!declared && allowed_level >= 0 && int(record.level) <= allowed_level)
                declared = true;

            for (auto const& a : log_allowances())
                if (!declared && record.level <= a.level && matches(a.pattern, a.domain, record))
                    declared = true;

            if (declared)
                continue;

            auto site = cc::format("logged at {}:{}", record.file != nullptr ? record.file : "?", record.line);
            if (outermost)
                CC_LOG_ERROR("undeclared {} in \"{}\": [{}] {} ({})", level_name(record.level), decl.name,
                             record.domain, record.text, site);

            file_error(exec, pass.section_path,
                       {
                           .expr = cc::format("undeclared {}", level_name(record.level)),
                           .location = decl.location,
                           .extra_lines = {cc::move(site), "declare it with nx::expect_* or nx::allow_*, or fix what "
                                                           "logs it"},
                           .expanded = cc::format("[{}] {}", record.domain, record.text),
                       });
        }

        for (auto i = 0; i < int(pass.declarations.size()); ++i)
        {
            auto const& d = pass.declarations[i];
            if (!d.is_expectation)
                continue;

            auto const n = counts[i];
            if (n >= d.at_least && (d.at_most < 0 || n <= d.at_most))
                continue;

            auto const wanted = d.at_most < 0           ? cc::format("at least {}", d.at_least)
                              : d.at_least == d.at_most ? cc::format("exactly {}", d.at_least)
                                                        : cc::format("{} to {}", d.at_least, d.at_most);
            auto const what
                = cc::format("expected {} {} matching \"{}\", logged {}", wanted, level_name(d.level), d.pattern, n);
            if (outermost)
                CC_LOG_ERROR("in \"{}\": {}", decl.name, what);

            file_error(exec, pass.section_path,
                       {
                           .expr = cc::format("nx::expect_{}(\"{}\")", level_name(d.level), d.pattern),
                           .location = d.location,
                           .extra_lines = {},
                           .expanded = what,
                       });
        }
    }

    for (auto& child : exec.nested)
        judge_execution(child, outermost);
}
} // namespace
} // namespace nx

void nx::expect_warning(cc::string_view pattern, log_expectation const& expectation, cc::source_location location)
{
    declare(cc::rec::level::warning, pattern, expectation.domain, true, expectation.at_least, expectation.at_most,
            location);
}

void nx::expect_error(cc::string_view pattern, log_expectation const& expectation, cc::source_location location)
{
    declare(cc::rec::level::error, pattern, expectation.domain, true, expectation.at_least, expectation.at_most,
            location);
}

void nx::allow_warnings(cc::string_view pattern, cc::string_view domain, cc::source_location location)
{
    declare(cc::rec::level::warning, pattern, domain, false, 0, -1, location);
}

void nx::allow_errors(cc::string_view pattern, cc::string_view domain, cc::source_location location)
{
    declare(cc::rec::level::error, pattern, domain, false, 0, -1, location);
}

void nx::impl::register_log_allowance(cc::rec::level level,
                                      char const* domain,
                                      char const* pattern,
                                      cc::source_location location)
{
    log_allowances().push_back({
        .level = level,
        .domain = domain != nullptr ? domain : "",
        .pattern = pattern != nullptr ? pattern : "",
        .location = location,
    });
}

void nx::impl::judge_logs(nx::test_schedule_execution& result, bool outermost)
{
    if (!nx::impl::run_recording_active())
        return;

    // The one drain the rule costs, per run rather than per test.
    cc::rec::flush_blocking();

    for (auto& exec : result.executions)
        judge_execution(exec, outermost);

    if (!outermost)
        return;

    // No allowance reaches these: a warning under no test is a defect to fix rather than a case to declare.
    for (auto const& record : nx::impl::take_unattributed_log_records())
    {
        result.unattributed_logs.push_back({
            .expr = cc::format("{} under no test", level_name(record.level)),
            .location = {},
            .extra_lines = {cc::format("logged at {}:{}", record.file != nullptr ? record.file : "?", record.line)},
            .expanded = cc::format("[{}] {}", record.domain, record.text),
        });
    }

    // Whatever is left belongs to owners no run judged — work that outlived its test, or another harness's.
    nx::impl::discard_log_records();
}
