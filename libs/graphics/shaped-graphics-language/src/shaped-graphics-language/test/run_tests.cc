#include "run_tests.hh"

#include <clean-core/string/format.hh>
#include <clean-core/string/glob.hh>
#include <shaped-graphics-language/legalize/impl/walk.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::test;

namespace
{
/// The span of what `from` names, and its text, read from the file it names.
struct located
{
    source_span where;
    cc::string text;
};

located locate(cc::span<module_file const> files, origin const& from)
{
    if (from.file < 0 || from.file >= files.size())
        return {};
    auto const& file = files[from.file];
    auto const form = ast::is_valid(from.expr) ? file.ast.at(from.expr).form
                    : ast::is_valid(from.stmt) ? file.ast.at(from.stmt).form
                                               : form_id::none;
    if (!is_valid(form))
        return {};
    auto const where = file.file.at(form).where;
    auto text = file.file.text_of(where);
    // a report quotes one line
    if (auto const end = text.find('\n'); end >= 0)
        text = text.subview({.offset = 0, .size = end});
    return {.where = where, .text = cc::string(text)};
}

/// Narrows the false node `index` of `nodes` into the parts that made it false (CHK-229).
struct narrower
{
    checked_module const& m;
    cc::span<module_file const> files;
    tree_view<flat_check_node> nodes;
    check_failure const& failure;
    cc::vector<narrowed_part> parts;

    [[nodiscard]] bool is_false(isize i) const
    {
        auto const& v = failure.values[i];
        return failure.is_evaluated[i] && v.leaves.size() == 1 && v.leaves[0].kind == value_kind::boolean
            && !v.leaves[0].as_bool();
    }

    void add(isize i, cc::string values)
    {
        auto const at = locate(files, nodes[i].from);
        parts.push_back({.file = nodes[i].from.file, .where = at.where, .text = at.text, .values = cc::move(values)});
    }

    /// A comparison node as its operand values: `0.5 > 0.6`.
    [[nodiscard]] cc::string compared(flat_check_node const& node) const
    {
        auto const operand = [&](i32 k)
        {
            return k >= 0 && k < nodes.size() && failure.is_evaluated[k] ? text_of_value(m, failure.values[k])
                                                                         : cc::string("?");
        };
        return cc::format("{} {} {}", operand(node.lhs), node.op, operand(node.rhs));
    }

    void narrow(isize i, int depth = 0)
    {
        if (depth > 64)
            return;
        auto const& node = nodes[i];
        auto const children = [&](auto&& fn)
        {
            for (auto k = isize(0); k < nodes.size(); ++k)
                if (nodes[k].parent == i32(i))
                    fn(k);
        };
        switch (node.kind)
        {
        case check_node_kind::and_:
        {
            // the first operand that was false decided it, and nothing right of it ran
            auto is_done = false;
            children(
                [&](isize k)
                {
                    if (!is_done && is_false(k))
                    {
                        narrow(k, depth + 1);
                        is_done = true;
                    }
                });
            return;
        }
        case check_node_kind::or_:
            // every operand was false, and each is a reason
            children([&](isize k) { narrow(k, depth + 1); });
            return;
        case check_node_kind::chain:
        {
            auto is_done = false;
            children(
                [&](isize k)
                {
                    if (!is_done && nodes[k].kind == check_node_kind::compare && is_false(k))
                    {
                        narrow(k, depth + 1);
                        is_done = true;
                    }
                });
            return;
        }
        case check_node_kind::compare:
        {
            add(i, compared(node));
            return;
        }
        case check_node_kind::not_:
        {
            // `not (4 == 4)`: a comparison under the `not` held, and its values are why the `not` is false
            auto is_done = false;
            children(
                [&](isize k)
                {
                    if (!is_done && nodes[k].kind == check_node_kind::compare && failure.is_evaluated[k])
                    {
                        add(i, cc::format("not ({})", compared(nodes[k])));
                        is_done = true;
                    }
                });
            if (!is_done)
                add(i, "false");
            return;
        }
        case check_node_kind::leaf:
            add(i, "false");
            return;
        }
    }
};
} // namespace

cc::string_view sgl::test::to_string(test_status s)
{
    switch (s)
    {
    case test_status::passed:
        return "passed";
    case test_status::failed:
        return "failed";
    case test_status::assertion_failed:
        return "assertion-failed";
    case test_status::out_of_fuel:
        return "out-of-fuel";
    case test_status::no_check_ran:
        return "no-check-ran";
    case test_status::not_run:
        return "not-run";
    case test_status::uninitialized_read:
        return "uninitialized-read";
    case test_status::internal_error:
        return "internal-error";
    }
    return "";
}

cc::string sgl::test::text_of_value(checked_module const& m, value const& v)
{
    auto const scalar_text = [](scalar const& leaf)
    {
        switch (leaf.kind)
        {
        case value_kind::scalar_float:
        {
            // `1.0` and not `1`, so a value reads as the type it is; and never the C library's `-nan`
            auto const f = leaf.as_float();
            if (f != f)
                return cc::string("nan");
            if (f - f != 0.0f)
                return cc::string(f < 0.0f ? "-inf" : "inf");
            auto text = cc::format("{}", f);
            if (!text.contains('.') && !text.contains('e'))
                text += ".0";
            return text;
        }
        case value_kind::scalar_int:
            return cc::format("{}", leaf.as_int());
        case value_kind::scalar_uint:
            return cc::format("{}u", leaf.as_uint());
        case value_kind::boolean:
            return cc::string(leaf.as_bool() ? "true" : "false");
        }
        return cc::string("?");
    };

    if (v.leaves.empty())
        return "void";
    // a case of an enum reads as its name
    if (m.is_plain_enum(v.type) && v.leaves.size() == 1)
        for (auto const& c : m.at(m.at(v.type).cases))
            if (c.value == v.leaves[0].as_int())
                return cc::format("{}.{}", m.name_of(v.type), c.name);
    if (v.leaves.size() == 1)
        return scalar_text(v.leaves[0]);

    auto text = cc::format("{}(", is_valid(v.type) ? m.name_of(v.type) : cc::string_view("?"));
    for (auto i = isize(0); i < v.leaves.size(); ++i)
        text.appendf("{}{}", i == 0 ? "" : ", ", scalar_text(v.leaves[i]));
    text += ")";
    return text;
}

cc::vector<test_result> sgl::test::run_tests(checked_module const& m,
                                             cc::span<module_file const> files,
                                             test_options const& options)
{
    auto results = cc::vector<test_result>();
    for (auto t = isize(0); t < m.tests.size(); ++t)
    {
        auto const& test = m.tests[t];
        if (options.file >= 0 && test.file != options.file)
            continue;
        if (test.expects_diagnostics())
            continue;
        auto result = test_result{.test = i32(t)};
        if (test.unit < 0)
        {
            results.push_back(cc::move(result));
            continue;
        }

        auto const& unit = m.test_units[test.unit];
        auto const o = interpret(m, unit, {}, options.limits);
        result.checks_run = o.checks_run - o.asserts_run;
        result.asserts_run = o.asserts_run;
        result.failures_dropped = o.failures_dropped;
        result.detail = o.detail;
        for (auto const& f : o.failures)
        {
            if (f.site < 0 || f.site >= unit.check_sites.size())
                continue;
            auto const& site = unit.check_sites[f.site];
            auto const nodes = unit.at(site.nodes);
            if (nodes.size() != f.values.size())
                continue;
            auto const at = locate(files, site.from);
            auto report
                = check_report{.file = site.from.file, .where = at.where, .text = at.text, .is_assert = site.stops};
            auto n = narrower{.m = m, .files = files, .nodes = nodes, .failure = f};
            n.narrow(0);
            report.parts = cc::move(n.parts);
            if (check::impl::is_known(unit, site.loop_variables))
                for (auto i = isize(0); i < f.loop_values.size() && i < unit.at(site.loop_variables).size(); ++i)
                {
                    auto const* const ref = unit.at(unit.at(site.loop_variables)[i]).node.try_as<flat_local_ref>();
                    auto const name = ref != nullptr ? cc::string_view(unit.at(ref->local).name) : cc::string_view("?");
                    report.loop_values.push_back(cc::format("{} = {}", name, text_of_value(m, f.loop_values[i])));
                }
            result.failures.push_back(cc::move(report));
        }

        switch (o.status)
        {
        case run_status::ok:
            result.status = !o.failures.empty() || o.failures_dropped > 0 ? test_status::failed
                          : o.checks_run == 0                             ? test_status::no_check_ran
                                                                          : test_status::passed;
            break;
        case run_status::assertion_failed:
            result.status = test_status::assertion_failed;
            break;
        case run_status::out_of_fuel:
            result.status = test_status::out_of_fuel;
            break;
        case run_status::uninitialized_read:
            result.status = test_status::uninitialized_read;
            break;
        case run_status::fell_off_the_end:
        case run_status::type_error:
            result.status = test_status::internal_error;
            break;
        }

        // CHK-232: a test that is to fail passes by failing, and one that is to stop at an assert by stopping there.
        // Every expectation is judged against the one run, so two of them never judge each other's verdict.
        auto const ran = result.status;
        auto has_run_expectation = false;
        auto const* unmet = static_cast<test_expectation const*>(nullptr);
        for (auto const& e : test.expectations)
        {
            if (e.kind != expectation_kind::fail && e.kind != expectation_kind::assert_)
                continue;
            has_run_expectation = true;
            auto const is_met = e.kind == expectation_kind::fail
                                  ? ran == test_status::failed || ran == test_status::assertion_failed
                                  : ran == test_status::assertion_failed;
            if (!is_met && unmet == nullptr)
                unmet = &e;
        }
        if (has_run_expectation && unmet == nullptr)
        {
            result.status = test_status::passed;
            result.failures.clear();
            result.failures_dropped = 0;
        }
        else if (unmet != nullptr && ran == test_status::passed)
        {
            result.status = test_status::failed;
            result.detail = unmet->kind == expectation_kind::fail ? "it was to fail, and it passed"
                                                                  : "it was to stop at an assert, and it ran to its end";
        }
        results.push_back(cc::move(result));
    }
    return results;
}

void sgl::test::contain_expected(checked_module const& m, cc::vector<located_diagnostic>& diagnostics)
{
    for (auto const& test : m.tests)
    {
        if (!test.expects_diagnostics())
            continue;
        auto const begin = test.extent.offset;
        auto const end = test.extent.offset + test.extent.length;
        auto const is_inside = [&](located_diagnostic const& d)
        { return d.file == test.file && d.what.where.offset >= begin && d.what.where.offset < end; };

        // Every diagnostic inside the test is its own: one expected error usually brings others with it, and the test
        // exists to show the one it names is reported, whatever else is.
        auto captured = cc::vector<located_diagnostic>();
        for (auto const& d : diagnostics)
            if (is_inside(d))
                captured.push_back(d);
        diagnostics.remove_all_where(is_inside);

        for (auto const& e : test.expectations)
        {
            if (e.kind != expectation_kind::error && e.kind != expectation_kind::warning)
                continue;
            auto const wants_warning = e.kind == expectation_kind::warning;
            auto is_met = false;
            for (auto const& d : captured)
                is_met = is_met
                      || ((d.what.level == severity::warning) == wants_warning
                          && cc::glob_matches(e.pattern, to_string(d.what.kind), cc::glob_option::text));
            if (!is_met)
                diagnostics.push_back({
                    .what = {.kind = diagnostic_kind::unmet_expectation,
                             .level = default_severity_of(diagnostic_kind::unmet_expectation),
                             .where = e.where},
                    .file = test.file,
                    .detail
                    = cc::format("no {} of kind {} stands in this test", wants_warning ? "warning" : "error", e.pattern),
                });
        }
    }
}

located_diagnostic sgl::test::diagnostic_of(checked_module const& m, test_result const& r)
{
    auto const& test = m.tests[r.test];
    auto detail = cc::string();
    switch (r.status)
    {
    case test_status::failed:
        detail = r.failures.empty() && !r.detail.empty()
                   ? r.detail
                   : cc::format("{} of {} checks failed", r.failures.size() + r.failures_dropped, r.checks_run);
        if (r.asserts_run > 0 && !r.failures.empty())
            detail.appendf(", {} asserts held", r.asserts_run);
        break;
    case test_status::assertion_failed:
        detail = "an assert failed, and the run stopped there";
        break;
    case test_status::out_of_fuel:
        detail = "the run ran out of fuel";
        break;
    case test_status::no_check_ran:
        detail = "the run ran no check";
        break;
    case test_status::uninitialized_read:
        detail = cc::format("the run read a variable nothing assigned: {}", r.detail);
        break;
    case test_status::internal_error:
        detail = cc::format("the compiler wrote a tree it cannot run: {}", r.detail);
        break;
    case test_status::passed:
    case test_status::not_run:
        detail = cc::string(to_string(r.status));
        break;
    }
    if (!test.comment.empty())
        detail.appendf(" ({})", test.comment);

    auto d = located_diagnostic{
        .what = {.kind = diagnostic_kind::test_failed,
                 .level = default_severity_of(diagnostic_kind::test_failed),
                 .where = test.where},
        .file = test.file,
        .detail = cc::move(detail),
    };
    for (auto const& f : r.failures)
    {
        auto loops = cc::string();
        for (auto const& l : f.loop_values)
            loops.appendf("{}{}", loops.empty() ? ", with " : ", ", l);
        if (f.parts.empty())
            d.notes.push_back({.file = f.file, .where = f.where, .message = cc::format("`{}` is false{}", f.text, loops)});
        for (auto const& p : f.parts)
            d.notes.push_back(
                {.file = p.file, .where = p.where, .message = cc::format("`{}` is {}{}", p.text, p.values, loops)});
    }
    if (r.failures_dropped > 0)
        d.notes.push_back({.file = test.file,
                           .where = test.where,
                           .message = cc::format("and {} more failures, left out", r.failures_dropped)});
    return d;
}
