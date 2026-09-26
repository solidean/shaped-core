#include "../features.hh"
#include "lsp-test-support.hh"

#include <clean-core/streams/file_stream.hh>
#include <nexus/bench/run.hh>
#include <shaped-graphics-language/ast/build.hh>
#include <shaped-graphics-language/driver/prelude.hh>

using namespace cc::primitive_defines;

namespace
{
cc::shared_ptr<lsp::document> document_of(cc::string text)
{
    auto d = cc::make_shared<lsp::document>();
    d->uri = "file:///bench.sgl";
    d->version = 1;
    d->text = cc::move(text);
    d->index = lsp::text_index(d->text);
    return d;
}

cc::string read_sample(cc::string_view name)
{
    auto adapter = cc::file_read_stream_adapter::open(cc::string(SGL_SAMPLES_DIR) + "/" + name);
    REQUIRE(adapter.has_value());
    auto const bytes = adapter.value().stream().read_all();
    REQUIRE(bytes.has_value());
    return cc::string(reinterpret_cast<char const*>(bytes.value().data()), bytes.value().size());
}
} // namespace

// What one keystroke costs the server: the document parsed and checked behind the prelude.
// The second loop also parses and checks the prelude, which the server caches the parse of and the check cannot.
BENCHMARK("sgl lsp - analyzing a sample, against a cached prelude parse and a fresh one")
{
    auto const doc = document_of(read_sample("helpers.sgl"));
    (void)sgl_lsp::the_prelude(); // built once, outside the timing

    nx::bench::run("analyze, prelude parse cached",
                   [&] { nx::bench::sink(sgl_lsp::analyze(doc)->module.symbols.size()); });
    nx::bench::run("analyze, prelude parsed again",
                   [&]
                   {
                       auto files = cc::vector<sgl::parsed_file>();
                       for (auto const& p : sgl::prelude_files())
                           files.push_back(sgl::parse(cc::string(p.source)));
                       auto asts = cc::vector<sgl::ast::file_ast>();
                       for (auto const& f : files)
                           asts.push_back(sgl::ast::build(f));
                       nx::bench::sink(asts.size());
                       nx::bench::sink(sgl_lsp::analyze(doc)->module.symbols.size());
                   });
}

// The longest a single test holds the server when it runs to its fuel limit, a million steps by default.
// Stopping between tests cannot interrupt it; this is the number that decides whether the interpreter should read a
// stop flag itself.
BENCHMARK("sgl lsp - a test that runs to its fuel limit")
{
    auto const doc = document_of("test:\n    let mut i = 0\n    while i >= 0:\n        i = i + 1\n    i < 0\n");
    auto const a = sgl_lsp::analyze(doc);
    REQUIRE(a->module.tests.size() == 1);
    auto const files = a->module_files();
    nx::bench::run("one test, out of fuel", {.min_time_secs = 1.0},
                   [&] { nx::bench::sink(i32(sgl::test::run_test(a->module, files, 0).status)); });
}
