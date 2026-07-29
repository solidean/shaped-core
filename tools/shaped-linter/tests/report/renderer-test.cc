#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-linter/lex/source_manager.hh>
#include <shaped-linter/report/renderer.hh>

using namespace scl;

namespace
{
source_span span_of(u32 begin, u32 end)
{
    return {.file_id = 0, .byte_begin = begin, .byte_end = end};
}

finding warn(cc::string_view rule_id, u32 begin, u32 end, cc::string_view message)
{
    return {.rule_id = rule_id, .span = span_of(begin, end), .message = cc::string(message)};
}
} // namespace

TEST("shaped-linter - renderer - a finding is a header, a snippet and its advice")
{
    source_manager sm;
    sm.add_from_text("aaa\nbbb", "x.hh");

    auto f = warn("my-rule", 0, 3, "thing is wrong");
    f.suggested_fix = fix{.edits = {{.span = span_of(0, 3), .replacement = " = x"}}};
    f.suggested_hint = hint{.message = "prefer y", .edits = {{.span = span_of(0, 3), .replacement = " = y"}}};

    CHECK(render_finding(f, sm) == R"([my-rule] thing is wrong
 --> x.hh:1:1
  |
1 | aaa
  | ^^^
2 | bbb
  |
  fix: replace `aaa` with `= x` (applied by --fix)
  help: prefer y
        consider `= y` (not applied)
)");
}

TEST("shaped-linter - renderer - a multi-edit fix lists each edit")
{
    source_manager sm;
    sm.add_from_text("aaa\nbbb", "x.cc");

    // The shape a rule produces when its rewrite only compiles once the file also gains a line: an empty
    // span reads as an insertion, and the spliced-in newlines are escaped so the phrase stays on its line.
    auto f = warn("my-rule", 0, 3, "thing is wrong");
    f.suggested_fix = fix{.edits = {
                              {.span = span_of(0, 3), .replacement = ""},
                              {.span = span_of(4, 4), .replacement = "\nusing namespace n;\n"},
                          }};

    CHECK(render_finding(f, sm) == R"([my-rule] thing is wrong
 --> x.cc:1:1
  |
1 | aaa
  | ^^^
2 | bbb
  |
  fix: 2 edits (applied by --fix)
       replace `aaa` with ``
       insert `\nusing namespace n;\n`
)");
}

TEST("shaped-linter - renderer - a single insertion reads as an insertion")
{
    source_manager sm;
    sm.add_from_text("aaa", "x.cc");

    auto f = warn("my-rule", 0, 3, "thing is wrong");
    f.suggested_fix = fix{.edits = {{.span = span_of(0, 0), .replacement = "X\n"}}};

    CHECK(render_finding(f, sm).contains("fix: insert `X\\n` (applied by --fix)"));
}

TEST("shaped-linter - renderer - a finding without fix or hint is just the snippet")
{
    source_manager sm;
    sm.add_from_text("aaa", "x.hh");

    CHECK(render_finding(warn("my-rule", 0, 3, "thing is wrong"), sm) == R"([my-rule] thing is wrong
 --> x.hh:1:1
  |
1 | aaa
  | ^^^
  |
)");
}

TEST("shaped-linter - renderer - a prose-only hint prints without a replacement")
{
    source_manager sm;
    sm.add_from_text("aaa", "x.hh");

    auto f = warn("my-rule", 0, 3, "thing is wrong");
    f.suggested_hint = hint{.message = "only a human can call this one"};

    CHECK(render_finding(f, sm).contains("  help: only a human can call this one\n"));
}

TEST("shaped-linter - renderer - color adds escapes and changes nothing else")
{
    source_manager sm;
    sm.add_from_text("aaa", "x.hh");

    auto const f = warn("my-rule", 0, 3, "thing is wrong");
    auto const plain = render_finding(f, sm);
    auto const colored = render_finding(f, sm, {.color = true});

    CHECK(colored != plain);
    CHECK(colored.contains("\033[33m[my-rule]\033[0m")); // warning is yellow
    CHECK(colored.contains("\033[33m^^^\033[0m"));

    // Strip every escape sequence and the two must agree byte for byte.
    auto stripped = cc::string();
    for (isize i = 0; i < colored.size(); ++i)
    {
        if (colored[i] != '\033')
        {
            stripped += colored[i];
            continue;
        }
        while (i < colored.size() && colored[i] != 'm')
            ++i;
    }
    CHECK(stripped == plain);
}

TEST("shaped-linter - renderer - severity picks the header color")
{
    source_manager sm;
    sm.add_from_text("aaa", "x.hh");

    auto f = warn("my-rule", 0, 3, "thing is wrong");
    f.sev = severity::error;
    CHECK(render_finding(f, sm, {.color = true}).contains("\033[31m[my-rule]\033[0m"));

    f.sev = severity::note;
    CHECK(render_finding(f, sm, {.color = true}).contains("\033[36m[my-rule]\033[0m"));
}

TEST("shaped-linter - renderer - findings are ordered by file, then line, then column")
{
    source_manager sm;
    sm.add_from_text("aaa\nbbb", "b.hh");
    sm.add_from_text("ccc\nddd", "a.hh");

    auto const findings = cc::vector<finding>{
        warn("my-rule", 4, 7, "second line of b"),
        {.rule_id = "my-rule", .span = {.file_id = 1, .byte_begin = 0, .byte_end = 3}, .message = "first line of a"},
        warn("my-rule", 0, 3, "first line of b"),
    };

    auto const out = render_report(findings, sm);
    auto const first = out.find("first line of a");
    auto const second = out.find("first line of b");
    auto const third = out.find("second line of b");

    CHECK(first >= 0);
    CHECK(first < second);
    CHECK(second < third);
}

TEST("shaped-linter - renderer - a rule's rationale prints once, however often it fired")
{
    source_manager sm;
    sm.add_from_text("aaa\nbbb", "x.hh");

    auto const findings = cc::vector<finding>{
        warn("default-init-assignment", 0, 3, "one"),
        warn("default-init-assignment", 4, 7, "two"),
    };

    auto out = render_report(findings, sm);

    CHECK(out.contains("rule rationale"));
    CHECK(out.contains("2 findings in 1 file\n"));
    CHECK(out.replace_all("[default-init-assignment]", "") == 3); // two finding headers plus one rationale entry
}

TEST("shaped-linter - renderer - the footer counts files and fixable findings")
{
    source_manager sm;
    sm.add_from_text("aaa", "x.hh");

    auto f = warn("my-rule", 0, 3, "thing is wrong");
    f.suggested_fix = fix{.edits = {{.span = span_of(0, 3), .replacement = " = x"}}};

    CHECK(render_report(cc::span<finding const>(&f, 1), sm).contains("1 finding in 1 file (1 fixable with --fix)\n"));
}

TEST("shaped-linter - renderer - a clean run renders nothing")
{
    source_manager sm;
    CHECK(render_report({}, sm) == "");
}
