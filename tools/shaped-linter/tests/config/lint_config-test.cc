#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <nexus/test.hh>
#include <shaped-linter/config/lint_config.hh>

using namespace scl;

namespace
{
/// Build a config from one or two config texts, the first standing for the repo root.
/// `base` is each one's directory, which is what its `files` globs are relative to.
lint_config config_of(cc::string_view root_text,
                      cc::string_view root_base = "",
                      cc::string_view near_text = "",
                      cc::string_view near_base = "")
{
    lint_config out;

    auto root = load_include_directives(root_text, root_base);
    CC_ASSERT(root.has_value(), "the test's root config must parse");
    for (auto& d : root.value())
        out.include_directives.push_back(cc::move(d));

    if (!near_text.empty())
    {
        auto near = load_include_directives(near_text, near_base);
        CC_ASSERT(near.has_value(), "the test's nearer config must parse");
        for (auto& d : near.value())
            out.include_directives.push_back(cc::move(d));
    }
    return out;
}
} // namespace

TEST("lint config - a config with no include rules checks nothing")
{
    lint_config const none;
    CHECK(!none.checks_includes());

    auto const some = config_of("rules:\n  - kind: allow-include\n    value: <atomic>\n    reason: seam\n");
    CHECK(some.checks_includes());
}

TEST("lint config - an unmatched include is unblessed, which is the default")
{
    auto const cfg = config_of("rules:\n  - kind: allow-include\n    value: <atomic>\n    reason: seam\n");

    CHECK(cfg.classify_include("a.cc", "<atomic>").verdict == include_verdict::allowed);
    CHECK(cfg.classify_include("a.cc", "<mutex>").verdict == include_verdict::unblessed);
    CHECK(cfg.classify_include("a.cc", "<mutex>").reason.empty());
}

TEST("lint config - a deny carries the reason that points at the replacement")
{
    auto const cfg
        = config_of("rules:\n  - kind: deny-include\n    value: <mutex>\n    reason: use clean-core/thread/mutex.hh\n");

    auto const d = cfg.classify_include("a.cc", "<mutex>");
    CHECK(d.verdict == include_verdict::denied);
    CHECK(d.reason == "use clean-core/thread/mutex.hh");
}

TEST("lint config - include matching folds case")
{
    // Both <dbghelp.h> and <DbgHelp.h> are live in the tree, and the SDK does not care which.
    auto const cfg = config_of("rules:\n  - kind: allow-include\n    value: <DbgHelp.h>\n    reason: the crash handler "
                               "needs it\n");

    CHECK(cfg.classify_include("a.cc", "<dbghelp.h>").verdict == include_verdict::allowed);
    CHECK(cfg.classify_include("a.cc", "<DbgHelp.h>").verdict == include_verdict::allowed);
}

TEST("lint config - one entry may name several includes")
{
    auto const cfg = config_of("rules:\n  - kind: deny-include\n    value: [<cstddef>, <cstdint>]\n    reason: use "
                               "clean-core/fwd.hh\n");

    CHECK(cfg.classify_include("a.cc", "<cstddef>").verdict == include_verdict::denied);
    CHECK(cfg.classify_include("a.cc", "<cstdint>").verdict == include_verdict::denied);
    CHECK(cfg.classify_include("a.cc", "<cstring>").verdict == include_verdict::unblessed);
}

TEST("lint config - files scopes an entry to part of the tree")
{
    auto const cfg = config_of(R"(rules:
  - kind: allow-include
    value: <vector>
    reason: tests may compare cc:: against std::
    files: tests/**
)");

    CHECK(cfg.classify_include("tests/a.cc", "<vector>").verdict == include_verdict::allowed);
    CHECK(cfg.classify_include("tests/deep/a.cc", "<vector>").verdict == include_verdict::allowed);
    CHECK(cfg.classify_include("src/a.cc", "<vector>").verdict == include_verdict::unblessed);
}

TEST("lint config - exclude-files carves the one site out of a blanket deny")
{
    auto const cfg = config_of(R"(rules:
  - kind: deny-include
    value: <cstddef>
    reason: use clean-core/fwd.hh
    exclude-files: src/clean-core/fwd.hh
)");

    CHECK(cfg.classify_include("src/clean-core/vector.hh", "<cstddef>").verdict == include_verdict::denied);
    // The one file that must include it is the one the deny does not reach.
    CHECK(cfg.classify_include("src/clean-core/fwd.hh", "<cstddef>").verdict == include_verdict::unblessed);
}

TEST("lint config - the last matching entry wins")
{
    auto const cfg = config_of(R"(rules:
  - kind: allow-include
    value: <atomic>
    reason: first
  - kind: deny-include
    value: <atomic>
    reason: second
)");

    CHECK(cfg.classify_include("a.cc", "<atomic>").reason == "second");
}

TEST("lint config - a nearer config re-opens what the root denied, narrowly")
{
    // The shape the repo actually uses: the root points everyone at cc::atomic, and clean-core re-allows
    // <atomic> for the one file that IS the seam.
    auto const cfg = config_of(R"(rules:
  - kind: deny-include
    value: <atomic>
    reason: use clean-core/thread/atomic.hh
)",
                               "",
                               R"(rules:
  - kind: allow-include
    value: <atomic>
    reason: this file is the seam
    files: src/clean-core/thread/atomic.hh
)",
                               "libs/base/clean-core");

    CHECK(cfg.classify_include("libs/base/clean-core/src/clean-core/thread/atomic.hh", "<atomic>").verdict
          == include_verdict::allowed);
    CHECK(cfg.classify_include("libs/base/clean-core/src/clean-core/vector.hh", "<atomic>").verdict
          == include_verdict::denied);
    // Another library is not reached by clean-core's entry at all.
    CHECK(cfg.classify_include("libs/data/babel-serializer/src/x.hh", "<atomic>").verdict == include_verdict::denied);
}

TEST("lint config - an entry only reaches files below its own config")
{
    auto const cfg = config_of("", "", "rules:\n  - kind: allow-include\n    value: <vector>\n    reason: local\n",
                               "libs/base/clean-core");

    CHECK(cfg.classify_include("libs/base/clean-core/src/a.hh", "<vector>").verdict == include_verdict::allowed);
    CHECK(cfg.classify_include("libs/base/nexus/src/a.hh", "<vector>").verdict == include_verdict::unblessed);
    // A prefix that is not a path boundary must not count as being below it.
    CHECK(cfg.classify_include("libs/base/clean-core-extra/src/a.hh", "<vector>").verdict == include_verdict::unblessed);
}

TEST("lint config - a rule needs a kind, a value and a reason")
{
    CHECK(load_include_directives("rules:\n  - value: <a>\n    reason: r\n", "").has_error());
    CHECK(load_include_directives("rules:\n  - kind: allow-include\n    reason: r\n", "").has_error());
    CHECK(load_include_directives("rules:\n  - kind: allow-include\n    value: <a>\n", "").has_error());
}

TEST("lint config - an unknown kind, key or section is an error")
{
    CHECK(load_include_directives("rules:\n  - kind: allow-inclde\n    value: <a>\n    reason: r\n", "").has_error());
    CHECK(load_include_directives("rules:\n  - kind: allow-include\n    value: <a>\n    reason: r\n    nope: x\n", "")
              .has_error());
    CHECK(load_include_directives("ruels:\n  - kind: allow-include\n    value: <a>\n    reason: r\n", "").has_error());
}

TEST("lint config - an empty config yields no directives")
{
    auto const none = load_include_directives("", "");
    REQUIRE(none.has_value());
    CHECK(none.value().empty());
}
