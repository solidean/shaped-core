#include "features.hh"

#include <clean-core/thread/async_coroutine.hh>

using namespace cc::primitive_defines;

cc::shared_async<sgl_lsp::test_run> sgl_lsp::run_tests(cc::shared_ptr<analysis> a, cc::shared_ptr<lsp::cancel_flag> stop)
{
    auto out = test_run{.checked = a};
    auto const files = a->module_files();
    auto const user = a->user_file();
    for (auto t = isize(0); t < a->module.tests.size(); ++t)
    {
        auto const& test = a->module.tests[t];
        if (test.file != user || test.expects_diagnostics())
            continue;
        if (stop->is_raised.load())
        {
            out.is_stopped = true;
            co_return out;
        }
        // the interpreter reads the flag too, so a test that runs long stops within microseconds, with threads
        auto result = sgl::test::run_test(a->module, files, i32(t), {.stop = &stop->is_raised});
        if (result.status == sgl::test::test_status::stopped)
        {
            out.is_stopped = true;
            co_return out;
        }
        out.results.push_back(cc::move(result));
        // without threads this is where input is read, so one test is the longest the server stays deaf
        co_await cc::async_yield();
    }
    co_return out;
}

sgl_lsp::check_results_params sgl_lsp::check_results_of(test_run const& run, lsp::position_encoding e)
{
    auto const& a = *run.checked;
    auto const user = a.user_file();
    auto out = check_results_params{.uri = a.document->uri, .version = a.document->version};

    // an assert in a helper is a site of every test that reaches it, so one mark sums them
    auto by_span = cc::map<u64, isize>();
    for (auto const& r : run.results)
        for (auto const& s : r.sites)
        {
            if (s.file != user)
                continue;
            auto const key = (u64(s.where.offset) << 32) | u64(s.where.length);
            auto& at = by_span[key];
            if (at == 0)
            {
                out.marks.push_back({.range = range_of(a, user, s.where, e)});
                at = out.marks.size();
            }
            out.marks[at - 1].passed += s.passed;
            out.marks[at - 1].failed += s.failed;
        }
    return out;
}

void sgl_lsp::write_fields(lsp::json::writer& w, check_mark const& v)
{
    lsp::json::put(w, "range", v.range);
    w.write("passed", v.passed);
    w.write("failed", v.failed);
}

void sgl_lsp::write_fields(lsp::json::writer& w, check_results_params const& v)
{
    lsp::json::put(w, "uri", v.uri);
    w.write("version", v.version);
    lsp::json::put(w, "marks", v.marks);
}
