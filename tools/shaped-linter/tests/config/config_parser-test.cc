#include <clean-core/common/assert.hh>
#include <nexus/test.hh>
#include <shaped-linter/config/config_parser.hh>

using namespace scl;

namespace
{
/// The scalar reached by `root[key]`, for the flat cases below.
cc::string_view scalar_of(config_document const& doc, cc::string_view key)
{
    auto const id = doc.find(doc.root, key);
    CC_ASSERT(id >= 0 && doc[id].kind == config_value_kind::scalar, "expected a scalar entry");
    return doc[id].scalar;
}
} // namespace

TEST("config parser - an empty file is a config that says nothing")
{
    auto const doc = parse_config("");
    REQUIRE(doc.has_value());
    CHECK(doc.value().root == -1);

    auto const only_comments = parse_config("# nothing here\n\n   # nor here\n");
    REQUIRE(only_comments.has_value());
    CHECK(only_comments.value().root == -1);
}

TEST("config parser - scalars keep everything after the colon")
{
    auto const doc = parse_config("kind: allow-include\nreason: use clean-core/fwd.hh — it is the seam\n");
    REQUIRE(doc.has_value());

    CHECK(scalar_of(doc.value(), "kind") == "allow-include");
    CHECK(scalar_of(doc.value(), "reason") == "use clean-core/fwd.hh — it is the seam");
}

TEST("config parser - a colon inside a value is not a key separator")
{
    // Only a `:` followed by a space ends a key, which is what lets a reason spell `cc::atomic`.
    auto const doc = parse_config("reason: use cc::atomic instead\n");
    REQUIRE(doc.has_value());
    CHECK(scalar_of(doc.value(), "reason") == "use cc::atomic instead");
}

TEST("config parser - quotes are stripped and protect their contents")
{
    auto const doc = parse_config("a: \"# not a comment\"\nb: 'kept: verbatim'\n");
    REQUIRE(doc.has_value());

    CHECK(scalar_of(doc.value(), "a") == "# not a comment");
    CHECK(scalar_of(doc.value(), "b") == "kept: verbatim");
}

TEST("config parser - a comment needs whitespace in front of it")
{
    auto const doc = parse_config("a: value # trailing\nb: c#d\n");
    REQUIRE(doc.has_value());

    CHECK(scalar_of(doc.value(), "a") == "value");
    CHECK(scalar_of(doc.value(), "b") == "c#d");
}

TEST("config parser - a flow list of scalars")
{
    auto const doc = parse_config("value: [<cstddef>, <cstdint>]\n");
    REQUIRE(doc.has_value());

    auto const id = doc.value().find(doc.value().root, "value");
    REQUIRE(id >= 0);
    REQUIRE(doc.value()[id].kind == config_value_kind::list);
    REQUIRE(doc.value()[id].children.size() == 2);
    CHECK(doc.value()[doc.value()[id].children[0]].scalar == "<cstddef>");
    CHECK(doc.value()[doc.value()[id].children[1]].scalar == "<cstdint>");

    auto const empty = parse_config("value: []\n");
    REQUIRE(empty.has_value());
    CHECK(empty.value()[empty.value().find(empty.value().root, "value")].children.empty());
}

TEST("config parser - a block list of mappings")
{
    auto const doc = parse_config(R"(rules:
  - kind: allow-include
    value: <atomic>
  - kind: deny-include
    value: <mutex>
)");
    REQUIRE(doc.has_value());
    auto const& d = doc.value();

    auto const rules = d.find(d.root, "rules");
    REQUIRE(rules >= 0);
    REQUIRE(d[rules].kind == config_value_kind::list);
    REQUIRE(d[rules].children.size() == 2);

    auto const first = d[rules].children[0];
    REQUIRE(d[first].kind == config_value_kind::mapping);
    CHECK(d[d.find(first, "kind")].scalar == "allow-include");
    CHECK(d[d.find(first, "value")].scalar == "<atomic>");
    CHECK(d[d.find(d[rules].children[1], "value")].scalar == "<mutex>");
}

TEST("config parser - a block list of scalars")
{
    auto const doc = parse_config("files:\n  - src/a.hh\n  - tests/**\n");
    REQUIRE(doc.has_value());
    auto const& d = doc.value();

    auto const files = d.find(d.root, "files");
    REQUIRE(d[files].kind == config_value_kind::list);
    REQUIRE(d[files].children.size() == 2);
    CHECK(d[d[files].children[0]].scalar == "src/a.hh");
    CHECK(d[d[files].children[1]].scalar == "tests/**");
}

TEST("config parser - a list item's siblings align under its key, not its dash")
{
    auto const doc = parse_config(R"(rules:
  - kind: deny-include
    value: <cstddef>
    reason: use clean-core/fwd.hh
    exclude-files:
      - src/clean-core/fwd.hh
)");
    REQUIRE(doc.has_value());
    auto const& d = doc.value();

    auto const item = d[d.find(d.root, "rules")].children[0];
    CHECK(d[item].keys.size() == 4);
    auto const excludes = d.find(item, "exclude-files");
    REQUIRE(d[excludes].kind == config_value_kind::list);
    CHECK(d[d[excludes].children[0]].scalar == "src/clean-core/fwd.hh");
}

TEST("config parser - line numbers are recorded for error messages")
{
    auto const doc = parse_config("a: 1\n\n# comment\nb: 2\n");
    REQUIRE(doc.has_value());

    CHECK(doc.value()[doc.value().find(doc.value().root, "a")].line == 1);
    CHECK(doc.value()[doc.value().find(doc.value().root, "b")].line == 4);
}

TEST("config parser - CRLF reads the same as LF")
{
    auto const doc = parse_config("rules:\r\n  - kind: allow-include\r\n");
    REQUIRE(doc.has_value());

    auto const& d = doc.value();
    auto const item = d[d.find(d.root, "rules")].children[0];
    CHECK(d[d.find(item, "kind")].scalar == "allow-include");
}

TEST("config parser - a tab is an error, never a silent eight columns")
{
    auto const doc = parse_config("rules:\n\t- kind: allow-include\n");
    REQUIRE(doc.has_error());
    CHECK(doc.error().to_string().contains("tab"));
}

TEST("config parser - a line that is not an entry fails loudly")
{
    auto const doc = parse_config("rules\n");
    CHECK(doc.has_error());
}

TEST("config parser - a key with no value and no block below it fails")
{
    auto const doc = parse_config("rules:\n");
    REQUIRE(doc.has_error());
    CHECK(doc.error().to_string().contains("indented block"));
}

TEST("config parser - an over-indented line has no key above it")
{
    auto const doc = parse_config("a: 1\n    b: 2\n");
    CHECK(doc.has_error());
}

TEST("config parser - the first entry must start at column 1")
{
    auto const doc = parse_config("  a: 1\n");
    CHECK(doc.has_error());
}

TEST("config parser - an unterminated flow list fails")
{
    auto const doc = parse_config("value: [<a>, <b>\n");
    CHECK(doc.has_error());
}

TEST("config parser - errors name a line")
{
    auto const doc = parse_config("a: 1\nnot an entry\n");
    REQUIRE(doc.has_error());
    CHECK(doc.error().to_string().contains("line 2"));
}
