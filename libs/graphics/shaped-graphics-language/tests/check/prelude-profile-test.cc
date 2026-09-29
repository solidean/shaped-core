#include "check-test-support.hh"

#include <babel-serializer/trace/chrome_trace.hh>
#include <clean-core/common/profiling.hh>
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
// Every compile checks the whole prelude, so a program of a few lines costs what the prelude costs.
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
        }
        cc::rec::flush_blocking();
        cc::rec::unregister_listener(handle);
        captured = capture.take().spliced_samples();
    }

    auto const checks = captured.scopes("check behind the prelude");
    auto total = 0.0;
    for (auto const& s : checks)
        total += s.duration_secs();
    cc::println("{} checks, {:.2f} ms each", checks.size(), checks.empty() ? 0.0 : total * 1000 / double(checks.size()));

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
