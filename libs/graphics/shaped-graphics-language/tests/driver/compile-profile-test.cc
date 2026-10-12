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
#include <clean-core/string/from_string.hh>
#include <clean-core/string/print.hh>
#include <nexus/test.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>
#include <shaped-graphics-language/driver/prelude.hh>

// Where one compile's time goes, from a file's text to one entry point's text: parse, check, flatten, legalize, emit.
// The prelude is checked once per process, before the first of them, and timed apart.
// Manual, since it measures rather than asserts, and it reads what it compiles from the environment:
//
//   SGL_PROFILE_SOURCE=<file.sgl> SGL_PROFILE_ENTRY=<name> uv run dev.py test "sgl profile - one compile" --manual
//
// SGL_PROFILE_TARGET is the target, hlsl-dx12 unless given.
// SGL_PROFILE_ROUNDS is how often it compiles, once unless given.
// SGL_PROFILE_RATE_HZ is how often it samples, 4000 a second unless given, which a compile of minutes wants fewer of.
// It prints the hottest functions and writes a Chrome trace, to CC_TRACE_OUT or the temp directory, for ui.perfetto.dev.

namespace
{
cc::string read_text(cc::string_view path)
{
    auto adapter = cc::file_read_stream_adapter::open(path);
    REQUIRE(adapter.has_value());
    auto stream = adapter.value().stream();
    auto const bytes = stream.read_all();
    REQUIRE(bytes.has_value());
    return cc::string(reinterpret_cast<char const*>(bytes.value().data()), bytes.value().size());
}

int int_from_environment(cc::string_view name, int fallback)
{
    auto const text = cc::environment_variable(name);
    if (!text.has_value())
        return fallback;
    auto const value = cc::from_string<int>(text.value());
    REQUIRE(value.has_value());
    return value.value();
}
} // namespace

// The sampler walks other threads, which some platforms cannot, and it says so as a warning; perf covers Linux.
TEST("sgl profile - one compile of a file, sampled",
     nx::config::manual,
     nx::config::exclusive(),
     nx::config::allow_logs(cc::rec::level::warning))
{
    auto const path = cc::environment_variable("SGL_PROFILE_SOURCE");
    auto const entry = cc::environment_variable("SGL_PROFILE_ENTRY");
    REQUIRE(path.has_value());
    REQUIRE(entry.has_value());
    auto const target_name = cc::environment_variable("SGL_PROFILE_TARGET").value_or("hlsl-dx12");
    auto target = sgl::emit::target::hlsl_dx12;
    auto is_known = false;
    for (auto const t : sgl::emit::all_targets())
        if (sgl::emit::to_string(t) == target_name)
        {
            target = t;
            is_known = true;
        }
    REQUIRE(is_known);
    auto const rounds = int_from_environment("SGL_PROFILE_ROUNDS", 1);
    auto const rate_hz = int_from_environment("SGL_PROFILE_RATE_HZ", 4000);
    auto const source = read_text(path.value());

    auto captured = cc::rec::recording();
    {
        auto capture = cc::rec::recording_listener();
        auto const handle = cc::rec::register_listener(capture);
        {
            auto const sampling = cc::rec::sampling_scope({.rate_hz = double(rate_hz)});
            // once per process, before the first compile, so it is timed apart from them
            {
                CC_RECORD_SCOPE("check the prelude");
                REQUIRE(sgl::checked_prelude() != nullptr);
            }
            for (auto i = 0; i < rounds; ++i)
            {
                CC_RECORD_SCOPE("compile to text");
                auto const text = sgl::compile_to_text(
                    {.source = source, .source_name = path.value(), .entry_point = entry.value(), .target = target});
                CHECK(text.has_value());
            }
        }
        cc::rec::flush_blocking();
        cc::rec::unregister_listener(handle);
        captured = capture.take().spliced_samples();
    }

    for (auto const& s : captured.scopes("check the prelude"))
        cc::println("the prelude checked in {:.2f} ms", s.duration_secs() * 1000);
    auto const compiles = captured.scopes("compile to text");
    auto total = 0.0;
    for (auto const& s : compiles)
        total += s.duration_secs();
    cc::println("{} compiles of {}, {:.2f} ms each", compiles.size(), path.value(),
                compiles.empty() ? 0.0 : total * 1000 / double(compiles.size()));

    auto const hot = cc::rec::hot_functions(captured);
    if (hot.sample_count > 0)
        cc::print("{}", hot.to_string(40));

    auto const out = cc::environment_variable("CC_TRACE_OUT")
                         .value_or(cc::format("{}/sgl-compile.trace.json", cc::temp_directory_path()));
    auto opened = cc::file_write_stream_adapter::create(out);
    REQUIRE(opened.has_value());
    auto stream = cc::write_stream(opened.value().stream());
    REQUIRE(babel::chrome_trace::write(stream, captured).has_value());
    REQUIRE(stream.flush().has_value());
    cc::println("wrote {}", out);
}
