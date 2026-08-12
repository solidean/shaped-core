#include <clean-core/container/map.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <shaped-linter/config/config_resolver.hh>

using namespace scl;

namespace
{
/// A resolver over an in-memory tree, counting how often each path was asked for.
/// The count is what pins the cache: a walk that re-reads is a walk that would re-read 200 times a batch.
struct fake_tree
{
    cc::map<cc::string, cc::string> files;
    cc::map<cc::string, int> reads;

    config_resolver resolver()
    {
        return config_resolver(
            [this](cc::string_view path) -> cc::optional<cc::string>
            {
                ++reads[cc::string(path)];
                if (auto const* text = files.get_ptr(path))
                    return *text;
                return {};
            });
    }
};

constexpr cc::string_view k_deny_atomic
    = "rules:\n  - kind: deny-include\n    value: <atomic>\n    reason: use clean-core/thread/atomic.hh\n";
} // namespace

TEST("config resolver - a file with no config above it is unchecked")
{
    fake_tree tree;
    auto r = tree.resolver();

    CHECK(!r.resolve("libs/base/clean-core/src/a.hh").checks_includes());
    CHECK(r.errors().empty());
}

TEST("config resolver - the walk climbs to the root and merges root-first")
{
    fake_tree tree;
    tree.files[cc::string("/repo/.shaped-lint.yml")] = cc::string(k_deny_atomic);
    tree.files[cc::string("/repo/libs/clean-core/.shaped-lint.yml")]
        = "rules:\n  - kind: allow-include\n    value: <atomic>\n    reason: the seam\n    files: "
          "src/thread/atomic.hh\n";

    auto r = tree.resolver();
    auto const& cfg = r.resolve("/repo/libs/clean-core/src/thread/atomic.hh");

    REQUIRE(cfg.checks_includes());
    CHECK(cfg.classify_include("/repo/libs/clean-core/src/thread/atomic.hh", "<atomic>").verdict
          == include_verdict::allowed);
    CHECK(cfg.classify_include("/repo/libs/clean-core/src/vector.hh", "<atomic>").verdict == include_verdict::denied);
    // Another library sees the root's entry and nothing else.
    CHECK(r.resolve("/repo/libs/nexus/src/a.hh").classify_include("/repo/libs/nexus/src/a.hh", "<atomic>").verdict
          == include_verdict::denied);
}

TEST("config resolver - the nearest config is the one to write a new blessing into")
{
    fake_tree tree;
    tree.files[cc::string("/repo/.shaped-lint.yml")] = cc::string(k_deny_atomic);
    tree.files[cc::string("/repo/libs/clean-core/.shaped-lint.yml")] = cc::string(k_deny_atomic);

    auto r = tree.resolver();
    CHECK(r.resolve("/repo/libs/clean-core/src/a.hh").nearest_config_path == "/repo/libs/clean-core/.shaped-lint.yml");
    CHECK(r.resolve("/repo/tools/a.cc").nearest_config_path == "/repo/.shaped-lint.yml");
}

TEST("config resolver - each directory is read once, however many files it holds")
{
    fake_tree tree;
    tree.files[cc::string("/repo/.shaped-lint.yml")] = cc::string(k_deny_atomic);

    auto r = tree.resolver();
    r.resolve("/repo/libs/a/src/one.hh");
    r.resolve("/repo/libs/a/src/two.hh");
    r.resolve("/repo/libs/a/src/three.hh");

    CHECK(tree.reads[cc::string("/repo/libs/a/src/.shaped-lint.yml")] == 1);
    CHECK(tree.reads[cc::string("/repo/.shaped-lint.yml")] == 1);
}

TEST("config resolver - a relative path still reaches the working directory's config")
{
    // A run given repo-relative paths must see the repo-root config, exactly as an absolute run does.
    fake_tree tree;
    tree.files[cc::string("./.shaped-lint.yml")] = cc::string(k_deny_atomic);

    auto r = tree.resolver();
    auto const& cfg = r.resolve("libs/base/clean-core/src/a.hh");

    REQUIRE(cfg.checks_includes());
    CHECK(cfg.classify_include("libs/base/clean-core/src/a.hh", "<atomic>").verdict == include_verdict::denied);
}

TEST("config resolver - windows separators resolve like posix ones")
{
    // The tree is keyed by the canonical spelling cc::glob_normalize_path produces — forward slashes, lower-case drive.
    // Every way of writing the same file must reach it, since the resolver is fed paths from a command line, a compile database and git alike.
    fake_tree tree;
    tree.files[cc::string("c:/repo/.shaped-lint.yml")] = cc::string(k_deny_atomic);

    auto r = tree.resolver();
    CHECK(r.resolve("C:\\repo\\libs\\a\\src\\x.hh").checks_includes());
    CHECK(r.resolve("c:/repo/libs/a/src/x.hh").checks_includes());
    CHECK(r.resolve("/c/repo/libs/a/src/x.hh").checks_includes());
}

TEST("config resolver - a config that does not parse is an error, not an empty policy")
{
    fake_tree tree;
    tree.files[cc::string("/repo/.shaped-lint.yml")]
        = "rules:\n  - kind: allow-inclde\n    value: <a>\n    reason: r\n";

    auto r = tree.resolver();
    r.resolve("/repo/a.cc");

    REQUIRE(r.errors().size() == 1);
    CHECK(r.errors()[0].contains("/repo/.shaped-lint.yml"));
}
