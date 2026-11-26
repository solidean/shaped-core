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

/// Holds a configuration's differences from a ground truth: each fails, unless the file knows its driver diverges.
/// A known divergence that no longer shows fails too, so a fixed driver retires its line.
void judge(sg_diff::workload const& w,
           configuration const& c,
           cc::string_view against,
           cc::vector<cc::string> const& lines,
           cc::string_view scope = {})
{
    auto const* const known = scope == "across" ? w.known_between_gpus(*c.ctx) : w.known_on(*c.ctx, scope);
    if (known == nullptr)
    {
        for (auto const& line : lines)
            CHECK(false).context(cc::format("{} {} on {} against {}: {}", w.file, w.entry, c.label, against, line));
        return;
    }
    if (lines.empty())
        CHECK(false).context(cc::format("{} {} on {} agrees with {}, though its file expects {}: remove that `known` "
                                        "line, and check whether the issue is fixed",
                                        w.file, w.entry, c.label, against, known->issue));
    else
        CC_LOG_INFO("differential: {} {} on {} diverges from {} as {} says: {}", w.file, w.entry, c.label, against,
                    known->issue, lines.front());
}

/// The interpreter's run of `w`, whose ranges let two GPU results that each take a licensed departure agree;
/// none where the interpreter cannot run it, and the GPUs are then held to each other alone.
/// Interprets on `host`'s device home, where a setup's half that builds on the device does so.
cc::shared_async<cc::optional<sg_diff::run_result>> ranges_of(sg_diff::workload const& w, sg::context& host)
{
    if (auto* const home = host.device_home())
        co_await cc::async_resume_on(*home);
    auto oracle = sg_diff::interpreted(w, host);
    if (oracle.has_error() || oracle.value().skipped.has_value())
        co_return cc::nullopt;
    co_return cc::move(oracle.value());
}

/// Runs `w` on every configuration and holds each to the first one that ran, at the tolerance their relation calls for.
/// A configuration the file knows to diverge runs last, so it is never what the others are held to, and only logs.
cc::shared_async<cc::unit> held_alike(sg_diff::workload const& w, cc::span<configuration const> all)
{
    if (all.empty())
        co_return;
    auto const ranges = co_await ranges_of(w, *all[0].ctx);
    auto configurations = cc::vector<configuration const*>();
    for (auto const& c : all)
        if (w.known_between_gpus(*c.ctx) == nullptr)
            configurations.push_back(&c);
    for (auto const& c : all)
        if (w.known_between_gpus(*c.ctx) != nullptr)
            configurations.push_back(&c);

    auto reference = cc::optional<sg_diff::run_result>();
    auto const* reference_of = static_cast<configuration const*>(nullptr);
    for (auto const* const p : configurations)
    {
        auto const& c = *p;
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
        auto const lines
            = sg_diff::differences(reference.value(), result, tolerance, ranges.has_value() ? &ranges.value() : nullptr);
        // two routes of one known driver may agree with each other, so agreeing proves nothing here
        if (auto const* const known = w.known_between_gpus(*c.ctx); known != nullptr)
        {
            if (!lines.empty())
                CC_LOG_INFO("differential: {} {}: {} against {} diverges as {} says: {}", w.file, w.entry, c.label,
                            reference_of->label, known->issue, lines.front());
            continue;
        }
        for (auto const& line : lines)
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
    // `SC_DIFFERENTIAL_DUMP=<dir>` writes every configuration's outputs there whole, to compare two machines byte by byte
    auto const dump = cc::environment_variable("SC_DIFFERENTIAL_DUMP");
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
                // a driver the file knows to diverge writes no reference
                if (w.known_between_gpus(*c.ctx) != nullptr)
                    continue;
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
        auto const ranges = co_await ranges_of(w, *configurations[0].ctx);
        for (auto const& c : configurations)
        {
            auto const result = co_await sg_diff::run(c.ctx.get(), &w, c.route);
            if (result.skipped.has_value())
                continue;
            if (dump.has_value() && !dump.value().empty())
                for (auto const& o : result.outputs)
                {
                    auto const name = cc::format("{}/{}.{}.{}.{}.{}.bin", dump.value(), sg_diff::reference_name(w),
                                                 w.entry, o.name, c.ctx->metrics.adapter().name, c.route.name);
                    write_text(name, cc::string_view(reinterpret_cast<char const*>(o.bytes.data()), o.bytes.size()));
                }
            judge(w, c, sg_diff::reference_name(w),
                  sg_diff::against_reference(reference.value(), w, result, ranges.has_value() ? &ranges.value() : nullptr),
                  "across");
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
        if (oracle.value().unbounded > 0)
            CC_LOG_INFO("differential: {} {}: the interpreter bounds {} output elements by nothing", w.file, w.entry,
                        oracle.value().unbounded);
        auto used = 0.0;
        for (auto const& c : configurations)
        {
            auto const result = co_await sg_diff::run(c.ctx.get(), &w, c.route);
            if (result.skipped.has_value())
                continue;
            if (w.known_on(*c.ctx) == nullptr)
                used = cc::max(used, sg_diff::tolerance_used(oracle.value(), result, w.across));
            judge(w, c, "the interpreter", sg_diff::differences(oracle.value(), result, w.across));
        }
        if (w.across.kind != sg_diff::tolerance::measure::exact)
            CC_LOG_INFO("differential: {} {} uses {} of its across {} against the interpreter", w.file, w.entry, used,
                        w.across.to_string());
    }
    CC_LOG_INFO("differential: {} of {} entry points interpreted", interpreted, corpus.workloads.size());
}

// The strict-floor twin: every corpus entry point compiled at a `.precise` floor, where a target may reassociate,
// contract and keep a half at a float's precision no more, and the interpreter's ranges collapse to what is left.
// WGSL keeps only `.fast` (EMIT-185), so a webgpu context has nothing to compile here.
// A file's `known` line says how a driver diverges at the level the file states, so here it only logs either way:
// a driver that agrees at the floor is one that keeps it.
// A `known` line ending in `precise` is this twin's own, held as the other tests hold theirs.
// The twin's floor reaches the GPU's compile: its first run held a driver to a floor that slib had dropped on the way.
// SPIR-V says so in the open, a `NoContraction` on every contractable operation, where DXIL keeps it in flags.
ASYNC_TEST("sg differential - a compile at a precise floor keeps the floor through slib")
{
    auto const configurations = machine_configurations();
    auto const* vulkan = static_cast<configuration const*>(nullptr);
    for (auto const& c : configurations)
        if (c.route.format == sg::shader_format::spirv && vulkan == nullptr)
            vulkan = &c;
    if (vulkan == nullptr)
        SKIP("this machine runs SGL on no vulkan adapter");
    auto const corpus = sg_diff::read_corpus();
    auto const* half = static_cast<sg_diff::workload const*>(nullptr);
    for (auto const& w : corpus.workloads)
        if (w.entry == "half_operations")
            half = &w;
    REQUIRE(half != nullptr);
    if (auto* const home = vulkan->ctx->device_home())
        co_await cc::async_resume_on(*home);

    // OpDecorate is opcode 71 of three words, and NoContraction decoration 42
    auto const decorations = [](cc::span<byte const> spirv)
    {
        auto count = 0;
        auto const* const words = reinterpret_cast<u32 const*>(spirv.data());
        for (auto i = isize(5); i + 2 < spirv.size() / 4; i += isize(words[i] >> 16 > 0 ? words[i] >> 16 : 1))
            count += words[i] == ((3u << 16) | 71u) && words[i + 2] == 42u ? 1 : 0;
        return count;
    };
    for (auto const* const floor : {"", "precise"})
    {
        auto route = vulkan->route;
        route.float_floor = floor;
        auto const shader = co_await sg_diff::compiled_on(*vulkan->ctx, route, *half->asset);
        auto const count = decorations(shader.bytecode.span());
        if (route.float_floor.empty())
            CHECK(count == 0).context("half_operations at its own level, fast");
        else
            CHECK(count >= 4).context(cc::format("half_operations at a precise floor, on {}", vulkan->label));
    }
}

ASYNC_TEST("sg differential - every corpus entry point computes at a precise floor what SGL's interpreter says")
{
    auto const configurations = machine_configurations();
    if (configurations.empty())
        SKIP("this machine runs SGL on no hardware adapter");
    auto const corpus = sg_diff::read_corpus();
    check_corpus(corpus);
    REQUIRE(!corpus.workloads.empty());

    for (auto const& w : corpus.workloads)
    {
        auto& host = *configurations[0].ctx;
        if (auto* const home = host.device_home())
            co_await cc::async_resume_on(*home);
        auto const oracle = sg_diff::interpreted(w, host, sgl::check::float_level::precise);
        if (oracle.has_error())
        {
            CHECK(false).context(cc::format("{} {} on the interpreter at a precise floor: {}", w.file, w.entry,
                                            oracle.error().to_string()));
            continue;
        }
        if (oracle.value().skipped.has_value())
            continue;
        auto used = 0.0;
        for (auto const& c : configurations)
        {
            if (c.route.format == sg::shader_format::wgsl)
                continue;
            auto route = c.route;
            route.float_floor = "precise";
            auto const result = co_await sg_diff::run(c.ctx.get(), &w, route);
            if (result.skipped.has_value())
                continue;
            auto const lines = sg_diff::differences(oracle.value(), result, w.across);
            if (w.known_on(*c.ctx, "precise") != nullptr)
            {
                judge(w, c, "the interpreter at a precise floor", lines, "precise");
                continue;
            }
            if (auto const* const known = w.known_on(*c.ctx); known != nullptr)
            {
                CC_LOG_INFO("differential: {} {} on {} at a precise floor, which {} knows it to diverge from at its "
                            "own level, {}",
                            w.file, w.entry, c.label, known->issue,
                            lines.empty() ? cc::string("agrees") : cc::format("diverges too: {}", lines.front()));
                continue;
            }
            used = cc::max(used, sg_diff::tolerance_used(oracle.value(), result, w.across));
            for (auto const& line : lines)
                CHECK(false).context(cc::format("{} {} on {} at a precise floor against the interpreter: {}", w.file,
                                                w.entry, c.label, line));
        }
        if (w.across.kind != sg_diff::tolerance::measure::exact)
            CC_LOG_INFO("differential: {} {} uses {} of its across {} at a precise floor", w.file, w.entry, used,
                        w.across.to_string());
    }
}
