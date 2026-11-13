#include "../features.hh"
#include "lsp-test-support.hh"

#include <clean-core/streams/file_stream.hh>
#include <nexus/bench/run.hh>
#include <nexus/source_root.hh>
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
    auto adapter = cc::file_read_stream_adapter::open(nx::source_path(SGL_SAMPLES_DIR) + "/" + name);
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

// What one keystroke costs behind shaped-viewer's modules.
// In the program it continues from the one layer of the library; in a module file it parses and checks them again.
BENCHMARK("sgl lsp - a keystroke in a shaped-viewer program, and in one of the modules it uses")
{
    auto const sv = nx::source_path(SGL_LSP_SV_SHADERS_DIR);
    sgl_lsp::set_module_dirs({sv, sv + "/sgl", nx::source_path(SGL_LSP_SR_MODULES_DIR)});
    auto adapter = cc::file_read_stream_adapter::open(sv + "/tracer_pipeline.sgl");
    REQUIRE(adapter.has_value());
    auto const bytes = adapter.value().stream().read_all();
    REQUIRE(bytes.has_value());
    auto const doc = document_of(cc::string(reinterpret_cast<char const*>(bytes.value().data()), bytes.value().size()));
    doc->uri = lsp::uri_of_path(sv + "/tracer_pipeline.sgl");
    auto const first = sgl_lsp::analyze(doc);
    REQUIRE(first->layer != nullptr);

    nx::bench::run("keystroke in the program", [&] { nx::bench::sink(sgl_lsp::analyze(doc)->module.symbols.size()); });
    // an open module file whose text differs every time, as typing in it makes it
    // The library is a chain of layers, so an edit of its first module checks every one again, and of its last that one.
    auto const first_file = first->module.prelude_file_count();
    auto const last_file = first_file + i32(first->module.library_files.size()) - 1;
    for (auto const [label, file] : {cc::pair{"keystroke in the chain's first module", first_file},
                                     cc::pair{"keystroke in the chain's last module", last_file}})
    {
        auto module_text = cc::string(first->text_of(file));
        auto const module_uri = first->uri_of(file);
        nx::bench::run(label,
                       [&]
                       {
                           module_text += " ";
                           auto const open = cc::vector<sgl_lsp::open_text>{{.uri = module_uri, .text = module_text}};
                           nx::bench::sink(sgl_lsp::analyze(doc, open)->module.symbols.size());
                       });
    }
    sgl_lsp::set_module_dirs({});
}

// What a test costs that runs to its fuel limit, a million steps by default, when nothing stops it.
// A new version stops it early, since the interpreter reads the stop flag every 4096 steps.
BENCHMARK("sgl lsp - a test that runs to its fuel limit")
{
    auto const doc = document_of("test:\n    let mut i = 0\n    while i >= 0:\n        i = i + 1\n    i < 0\n");
    auto const a = sgl_lsp::analyze(doc);
    REQUIRE(a->module.tests.size() == 1);
    auto const files = a->module_files();
    nx::bench::run("one test, out of fuel", {.min_time_secs = 1.0},
                   [&] { nx::bench::sink(i32(sgl::test::run_test(a->module, files, 0).status)); });
}
