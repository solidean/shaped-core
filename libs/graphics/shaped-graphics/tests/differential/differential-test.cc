#include "differential.hh"

#include "../backends/sg_backends.hh"
#include "../shaders/shader_fixtures.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/platform/environment.hh>
#include <clean-core/record/log.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/source_root.hh>
#include <nexus/test.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/context/metrics.hh>

// The corpus run every way one context runs it, every way this machine runs it, and against what another machine committed.
// ../../docs/differential-testing.md is the design.

using namespace cc::primitive_defines;

namespace
{
/// One context and one of its routes, as a comparison names it.
struct configuration
{
    sg::context_handle ctx;
    sg_diff::route route;
    cc::string label;
};

/// Runs `w` on every configuration and holds each to the first one that ran, at the tolerance their relation calls for.
cc::shared_async<cc::unit> held_alike(sg_diff::workload const& w, cc::span<configuration const> configurations)
{
    auto reference = cc::optional<sg_diff::run_result>();
    auto const* reference_of = static_cast<configuration const*>(nullptr);
    for (auto const& c : configurations)
    {
        auto result = co_await sg_diff::run(c.ctx.get(), &w, c.route);
        if (result.skipped.has_value())
        {
            CC_LOG_INFO("differential: {} {} on {} has nothing to compare: {}", w.file, w.entry, c.label,
                        result.skipped.value());
            continue;
        }
        if (!reference.has_value())
        {
            reference = cc::move(result);
            reference_of = &c;
            continue;
        }
        auto const tolerance = c.ctx == reference_of->ctx ? w.routes : w.across;
        for (auto const& line : sg_diff::differences(reference.value(), result, tolerance))
            CHECK(false).context(
                cc::format("{} {}: {} against {}: {}", w.file, w.entry, c.label, reference_of->label, line));
    }
}

/// One context per backend this binary has and its every route, a software adapter left out.
/// A software adapter is no device a shader ships to, and WARP's float maths is its own.
cc::vector<configuration> machine_configurations()
{
    auto configurations = cc::vector<configuration>();
    for (auto& factory : sg_test::context_factories())
    {
        auto created = factory.create_for_test();
        if (created.has_error())
            continue;
        auto const ctx = created.value();
        (void)sg_test::shader_fixtures();
        if (ctx->metrics.adapter().is_software || !sg_test::shaders_reach(*ctx))
            continue;
        for (auto const& r : sg_diff::routes_of(*ctx))
            configurations.push_back(
                {.ctx = ctx,
                 .route = r,
                 .label = cc::format("{} ({}) {}", factory.backend, ctx->metrics.adapter().name, r.name)});
    }
    return configurations;
}

constexpr cc::string_view k_refresh_variable = "SC_DIFFERENTIAL_REFRESH";

[[nodiscard]] cc::string reference_path(sg_diff::workload const& w)
{
    return cc::format("{}/{}", nx::source_path(SG_DIFFERENTIAL_REFERENCES_DIR), sg_diff::reference_name(w));
}

[[nodiscard]] cc::optional<cc::string> read_text(cc::string const& path)
{
    auto file = cc::file_read_stream_adapter::open(path);
    if (file.has_error())
        return cc::nullopt;
    auto stream = file.value().stream();
    auto bytes = stream.read_all();
    if (bytes.has_error())
        return cc::nullopt;
    return cc::string(cc::string_view(reinterpret_cast<char const*>(bytes.value().data()), bytes.value().size()));
}

void write_text(cc::string const& path, cc::string_view text)
{
    auto file = cc::file_write_stream_adapter::create(path);
    REQUIRE(file.has_value()).context(cc::format("cannot create {}", path));
    auto stream = file.value().stream();
    REQUIRE(stream.write(cc::span<char const>(text.data(), text.size()).as_bytes()).has_value());
    REQUIRE(stream.flush().has_value());
}

/// Every problem of the corpus's comment blocks is a failure naming its file.
void check_corpus(sg_diff::corpus const& corpus)
{
    for (auto const& problem : corpus.problems)
        CHECK(false).context(problem);
}
} // namespace

ASYNC_INVOCABLE_TEST("sg differential - every corpus entry point computes alike on each route this context takes",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    auto const routes = sg_diff::routes_of(*ctx);
    if (routes.size() < 2)
        SKIP("this context takes SGL one way, so there is nothing to hold it to here");

    auto const corpus = sg_diff::read_corpus();
    check_corpus(corpus);
    REQUIRE(!corpus.workloads.empty());

    auto configurations = cc::vector<configuration>();
    for (auto const& r : routes)
        configurations.push_back({.ctx = ctx, .route = r, .label = r.name});
    for (auto const& w : corpus.workloads)
        co_await held_alike(w, configurations);
}

ASYNC_TEST("sg differential - every corpus entry point computes alike on every backend and route of this machine")
{
    auto const configurations = machine_configurations();
    if (configurations.size() < 2)
        SKIP("this machine runs SGL one way, so there is nothing to hold it to");

    auto const corpus = sg_diff::read_corpus();
    check_corpus(corpus);
    REQUIRE(!corpus.workloads.empty());

    for (auto const& w : corpus.workloads)
        co_await held_alike(w, configurations);
    CC_LOG_INFO("differential: {} entry points held alike on {} configurations", corpus.workloads.size(),
                configurations.size());
}

ASYNC_TEST("sg differential - every corpus entry point computes what its committed reference says")
{
    auto const configurations = machine_configurations();
    if (configurations.empty())
        SKIP("this machine runs SGL on no hardware adapter");
    auto const corpus = sg_diff::read_corpus();
    check_corpus(corpus);
    REQUIRE(!corpus.workloads.empty());

    // a refresh writes each file's reference from the first configuration that runs it, then holds every one to it
    auto const refresh = cc::environment_variable(k_refresh_variable) == cc::optional<cc::string>(cc::string("1"));
    auto written = cc::vector<cc::string>(); // the paths this refresh has started, each rewritten once
    auto texts = cc::vector<cc::string>();   // what each of them holds so far
    for (auto const& w : corpus.workloads)
    {
        auto const path = reference_path(w);
        if (refresh)
        {
            auto slot = isize(-1);
            for (auto i = isize(0); i < written.size(); ++i)
                if (written[i] == path)
                    slot = i;
            if (slot < 0)
            {
                written.push_back(path);
                texts.push_back(cc::format(
                    "# The differential reference of {}: what one machine computed, which every other is held to.\n"
                    "# Rewritten by `{}=1 uv run dev.py test \"sg differential - every corpus entry point "
                    "computes what its committed reference says\"`; review the diff like code.\n",
                    w.file, k_refresh_variable));
                slot = written.size() - 1;
            }
            for (auto const& c : configurations)
            {
                auto const result = co_await sg_diff::run(c.ctx.get(), &w, c.route);
                if (result.skipped.has_value())
                    continue;
                texts[slot].appendf("# {} written on {}\n{}", w.entry, c.label, sg_diff::reference_lines(w, result));
                break;
            }
            write_text(path, texts[slot]);
        }

        auto const reference = read_text(path);
        if (!reference.has_value())
        {
            CHECK(false).context(cc::format("{} has no committed reference {}; write it with {}=1 and this test",
                                            w.file, sg_diff::reference_name(w), k_refresh_variable));
            continue;
        }
        for (auto const& c : configurations)
        {
            auto const result = co_await sg_diff::run(c.ctx.get(), &w, c.route);
            if (result.skipped.has_value())
                continue;
            for (auto const& line : sg_diff::against_reference(reference.value(), w, result))
                CHECK(false).context(cc::format("{} {} on {} against {}: {}", w.file, w.entry, c.label,
                                                sg_diff::reference_name(w), line));
        }
    }
}

ASYNC_TEST("sg differential - every corpus entry point computes on the GPU what SGL's interpreter says")
{
    auto const configurations = machine_configurations();
    if (configurations.empty())
        SKIP("this machine runs SGL on no hardware adapter");
    auto const corpus = sg_diff::read_corpus();
    check_corpus(corpus);
    REQUIRE(!corpus.workloads.empty());

    auto interpreted = isize(0);
    for (auto const& w : corpus.workloads)
    {
        // a half that builds on the device does so on its home
        auto& host = *configurations[0].ctx;
        if (auto* const home = host.device_home())
            co_await cc::async_resume_on(*home);
        auto const oracle = sg_diff::interpreted(w, host);
        if (oracle.has_error())
        {
            CHECK(false).context(cc::format("{} {} on the interpreter: {}", w.file, w.entry, oracle.error().to_string()));
            continue;
        }
        if (oracle.value().skipped.has_value())
        {
            CC_LOG_INFO("differential: {} {} is not interpreted: {}", w.file, w.entry, oracle.value().skipped.value());
            continue;
        }
        ++interpreted;
        for (auto const& c : configurations)
        {
            auto const result = co_await sg_diff::run(c.ctx.get(), &w, c.route);
            if (result.skipped.has_value())
                continue;
            for (auto const& line : sg_diff::differences(oracle.value(), result, w.across))
                CHECK(false).context(cc::format("{} {}: {} against the interpreter: {}", w.file, w.entry, c.label, line));
        }
    }
    CC_LOG_INFO("differential: {} of {} entry points interpreted", interpreted, corpus.workloads.size());
}
