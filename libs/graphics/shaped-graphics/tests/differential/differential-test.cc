#include "differential.hh"

#include "../backends/sg_backends.hh"
#include "../shaders/shader_fixtures.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/record/log.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/context/metrics.hh>

// The corpus run every way one context runs it, and every way this machine runs it: docs/differential-testing.md.

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
    // a software adapter is no device a shader ships to, and WARP's float maths is its own
    auto configurations = cc::vector<configuration>();
    for (auto& factory : sg_test::context_factories())
    {
        auto created = factory.create();
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
