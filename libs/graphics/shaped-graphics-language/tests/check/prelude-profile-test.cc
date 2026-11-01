#include "check-test-support.hh"

#include <babel-serializer/trace/chrome_trace.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/common/time.hh>
#include <clean-core/platform/environment.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/record/hot_functions.hh>
#include <clean-core/record/recording.hh>
#include <clean-core/record/sampling.hh>
#include <clean-core/record/system.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>

// Where a compile's time goes when the program is small and the prelude is not.
// The prelude is checked once per process, before the rounds, so a round is the copy of its state and the program's check.
// Manual, since it measures rather than asserts: run it on a debug preset, where the corpus tests feel it most.
//
//   uv run dev.py test "sgl profile" --manual --preset debug-linux-clang
//
// It prints the hottest functions and writes a Chrome trace, to CC_TRACE_OUT or the temp directory, for ui.perfetto.dev.

namespace
{
constexpr cc::string_view small_program = R"(binding frame:
    values: mut buffer[float]

fun halve(x: float) -> float => x * 0.5

@compute(64) fun run(@thread_id id: int3){frame}:
    frame.values[id.x] = halve(frame.values[id.x])
)";
}

// The sampler walks other threads, which some platforms cannot, and it says so as a warning; perf covers Linux.
TEST("sgl profile - checking a small program behind the prelude, sampled",
     nx::config::manual,
     nx::config::exclusive(),
     nx::config::allow_logs(cc::rec::level::warning))
{
    auto const rounds = 100;
    REQUIRE(sgl::checked_prelude() != nullptr);

    cc::rec::recording captured;
    {
        cc::rec::recording_listener capture;
        auto const handle = cc::rec::register_listener(capture);
        {
            cc::rec::sampling_scope const sampling({.rate_hz = 4000.0});
            for (auto i = 0; i < rounds; ++i)
            {
                CC_RECORD_SCOPE("check behind the prelude");
                auto const checked = sgl_test::check_sources(sgl_test::read_prelude(), small_program);
                CHECK(checked.module.diagnostics.empty());
            }
            // what every compile pays whatever its program: the copy of the prelude's state and the whole-module passes
            for (auto i = 0; i < rounds; ++i)
            {
                CC_RECORD_SCOPE("check nothing behind the prelude");
                auto const checked = sgl_test::check_sources(sgl_test::read_prelude(), "");
                CHECK(checked.module.diagnostics.empty());
            }
        }
        cc::rec::flush_blocking();
        cc::rec::unregister_listener(handle);
        captured = capture.take().spliced_samples();
    }

    auto const report = [&](cc::string_view scope, cc::string_view what)
    {
        auto const checks = captured.scopes(scope);
        auto total = 0.0;
        for (auto const& s : checks)
            total += s.duration_secs();
        cc::println("{} checks{}, {:.2f} ms each", checks.size(), what,
                    checks.empty() ? 0.0 : total * 1000 / double(checks.size()));
    };
    report("check behind the prelude", "");
    report("check nothing behind the prelude", " of an empty program");

    auto const hot = cc::rec::hot_functions(captured);
    if (hot.sample_count > 0)
        cc::print("{}", hot.to_string(30));

    auto const path = cc::environment_variable("CC_TRACE_OUT")
                          .value_or(cc::format("{}/sgl-prelude-check.trace.json", cc::temp_directory_path()));
    auto opened = cc::file_write_stream_adapter::create(path);
    REQUIRE(opened.has_value());
    cc::write_stream stream = opened.value().stream();
    REQUIRE(babel::chrome_trace::write(stream, captured).has_value());
    REQUIRE(stream.flush().has_value());
    cc::println("wrote {}", path);
}

// What every process pays once, before its first compile: parsing the prelude's files, building their ASTs, and
// checking them.
// The rounds time it cold each time, since `parsed_prelude()` and `checked_prelude()` keep the first result.
// It samples the parse and the AST build, and prints where their time goes.
TEST("sgl profile - parsing and checking the prelude itself",
     nx::config::manual,
     nx::config::exclusive(),
     nx::config::allow_logs(cc::rec::level::warning))
{
    auto const rounds = 10;
    auto parse_secs = 0.0;
    auto build_secs = 0.0;
    auto check_secs = 0.0;
    cc::rec::recording captured;
    {
        cc::rec::recording_listener capture;
        auto const handle = cc::rec::register_listener(capture);
        for (auto i = 0; i < rounds; ++i)
        {
            auto files = cc::vector<sgl::parsed_file>();
            auto asts = cc::vector<sgl::ast::file_ast>();
            {
                cc::rec::sampling_scope const sampling({.rate_hz = 4000.0});
                auto const parse_start = cc::current_time_steady_secs();
                for (auto const& f : sgl::prelude_files())
                    files.push_back(sgl::parse(f.source));
                auto const build_start = cc::current_time_steady_secs();
                for (auto const& f : files)
                    asts.push_back(sgl::ast::build(f));
                parse_secs += build_start - parse_start;
                build_secs += cc::current_time_steady_secs() - build_start;
            }
            auto const check_start = cc::current_time_steady_secs();
            auto modules = cc::vector<sgl::check::module_file>();
            for (auto j = sgl::isize(0); j < files.size(); ++j)
                modules.push_back({.file = files[j], .ast = asts[j]});
            auto const checked = sgl::check::check_prelude(modules, sgl::builtins::default_registry());
            check_secs += cc::current_time_steady_secs() - check_start;
            CHECK(checked.has_value());
        }
        cc::rec::flush_blocking();
        cc::rec::unregister_listener(handle);
        captured = capture.take().spliced_samples();
    }
    auto bytes = sgl::isize(0);
    for (auto const& f : sgl::prelude_files())
        bytes += f.source.size();
    cc::println("prelude: {} bytes; parse {:.2f} ms ({:.0f} MB/s), AST build {:.2f} ms, check {:.2f} ms", bytes,
                parse_secs * 1000 / rounds, double(bytes) * rounds / parse_secs / 1e6, build_secs * 1000 / rounds,
                check_secs * 1000 / rounds);
    auto const hot = cc::rec::hot_functions(captured);
    if (hot.sample_count > 0)
        cc::print("{}", hot.to_string(25));
}
