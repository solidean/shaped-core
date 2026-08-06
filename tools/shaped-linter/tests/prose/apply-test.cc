#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <shaped-linter/prose/apply.hh>
#include <shaped-linter/prose/plan.hh>

using namespace scl;

namespace
{
/// Build the rewrite the plan describes over `original`. Both parses must succeed.
planned_rewrite build(cc::string_view plan_text, cc::string_view original)
{
    auto plan = parse_prose_plan(plan_text);
    REQUIRE(plan.has_value());
    REQUIRE(plan.value().files.size() == 1);

    auto built = build_rewrite(plan.value().files[0], original);
    REQUIRE(built.has_value());
    return cc::move(built.value());
}

/// The edited lines as `a,b,c` — the new-file coordinates the prose re-check is scoped to.
cc::string edited(planned_rewrite const& r)
{
    cc::string out;
    for (auto const l : r.edited_lines)
    {
        if (!out.empty())
            out += ',';
        out += cc::to_string(l);
    }
    return out;
}

/// How many problems judging `r` against `original` turns up.
isize problems(planned_rewrite const& r, cc::string_view original)
{
    source_manager sources;
    apply_problems out;
    validate_rewrite(r, original, sources, out);
    return out.count();
}
} // namespace

TEST("prose apply - a replacement swaps whole lines")
{
    auto const r = build("## a.hh\n[2-3]\n| // new one\n", "int a;\n// old one\n// old two\nint b;\n");

    CHECK(r.text == "int a;\n// new one\nint b;\n");
    CHECK(edited(r) == "2");
}

TEST("prose apply - an empty span deletes")
{
    auto const r = build("## a.hh\n[2]\n", "int a;\n// gone\nint b;\n");

    CHECK(r.text == "int a;\nint b;\n");
    CHECK(edited(r) == "");
}

TEST("prose apply - an insertion goes before its line")
{
    auto const r = build("## a.hh\n[+2]\n| // added\n", "int a;\nint b;\n");

    CHECK(r.text == "int a;\n// added\nint b;\n");
    CHECK(edited(r) == "2");
}

TEST("prose apply - edited lines are in new-file coordinates")
{
    // The first span shrinks 3 lines to 1, so the second span's replacement lands two lines earlier.
    auto const r
        = build("## a.hh\n[1-3]\n| // one\n[5]\n| // two\n| // three\n", "//a\n//b\n//c\nint x;\n//d\nint y;\n");

    CHECK(r.text == "// one\nint x;\n// two\n// three\nint y;\n");
    CHECK(edited(r) == "1,3,4");
}

TEST("prose apply - a file that ends mid-line keeps ending mid-line")
{
    auto const r = build("## a.hh\n[2]\n| // last\n", "int a;\n// old");

    CHECK(r.text == "int a;\n// last");
}

TEST("prose apply - a CRLF file stays CRLF")
{
    // Splicing LF lines into a CRLF file leaves it mixed, and the diff then shows lines nobody edited.
    auto const r = build("## a.hh\n[2]\n| // new\n", "int a;\r\n// old\r\nint b;\r\n");

    CHECK(r.text == "int a;\r\n// new\r\nint b;\r\n");
}

TEST("prose apply - a span past the end of the file is an error")
{
    auto plan = parse_prose_plan("## a.hh\n[5-6]\n| // x\n");
    REQUIRE(plan.has_value());

    CHECK(build_rewrite(plan.value().files[0], "int a;\nint b;\n").has_error());
}

TEST("prose apply - changing code is rejected")
{
    auto const original = cc::string("int x = 1; // note\n");
    auto const r = build("## a.hh\n[1]\n| int y = 1; // note\n", original);

    CHECK(problems(r, original) > 0);
}

TEST("prose apply - deleting a line that holds code is rejected")
{
    auto const original = cc::string("// doc\nint x = 1;\n");
    auto const r = build("## a.hh\n[1-2]\n| // doc\n", original);

    CHECK(problems(r, original) > 0);
}

TEST("prose apply - a trailing comment may be rewritten and re-aligned")
{
    auto const original = cc::string("#include <memory>        // std::shared_ptr, std::make_shared\n");
    auto const r = build("## a.hh\n[1]\n| #include <memory> // shared_ptr\n", original);

    CHECK(problems(r, original) == 0);
}

TEST("prose apply - prose that violates a rule is rejected")
{
    auto const original = cc::string("/// a comment\nint x;\n");
    auto const r = build("## a.hh\n[1]\n| /// One point. And a second one on the same line.\n", original);

    CHECK(problems(r, original) > 0);
}

TEST("prose apply - a violation the plan did not write is not the plan's problem")
{
    // Line 1 is reflowed prose and stays exactly as it was; only line 2 is rewritten.
    auto const original = cc::string("/// One point. And a second one on the same line.\n/// stale\nint x;\n");
    auto const r = build("## a.hh\n[2]\n| /// fresh\n", original);

    CHECK(problems(r, original) == 0);
}

TEST("prose apply - markdown is prose end to end")
{
    auto const original = cc::string("# Title\n\nold body\n");
    auto const r = build("## a.md\n[3]\n| new body\n", original);

    CHECK(r.text == "# Title\n\nnew body\n");
    CHECK(problems(r, original) == 0);
}

TEST("prose apply - python comments go through the python lexer")
{
    auto const original = cc::string("x = 1  # old\n");
    auto const r = build("## a.py\n[1]\n| x = 1  # new\n", original);

    CHECK(problems(r, original) == 0);
}

TEST("prose apply - a python docstring is prose, not code")
{
    // It lexes as a string literal, but every prose rule reads it as prose, so the rule and the
    // code-unchanged check would otherwise disagree: flagged and unfixable.
    auto const original = cc::string("def f():\n    \"\"\"Old. Two points.\"\"\"\n    return 1\n");
    auto const r = build("## a.py\n[2]\n|     \"\"\"One point per line.\"\"\"\n", original);

    CHECK(problems(r, original) == 0);
}

TEST("prose apply - a triple-quoted string used as data is still code")
{
    auto const original = cc::string("x = \"\"\"payload\"\"\"\n");
    auto const r = build("## a.py\n[1]\n| x = \"\"\"changed\"\"\"\n", original);

    CHECK(problems(r, original) > 0);
}

TEST("prose apply - changing python code is rejected")
{
    auto const original = cc::string("x = 1  # note\n");
    auto const r = build("## a.py\n[1]\n| x = 2  # note\n", original);

    CHECK(problems(r, original) > 0);
}

TEST("prose apply - every bad line is reported, not just the first")
{
    // Authoring a plan is a batch job across many files, so stopping at the first problem costs the
    // author one round trip per problem.
    auto const original = cc::string("# Title\n\na\nb\nc\n");
    auto const r = build("## a.md\n[3-5]\n"
                         "| One point. And a second one on the same line.\n"
                         "| Another point. With a second one too.\n"
                         "| A third. And again.\n",
                         original);

    CHECK(problems(r, original) == 3);
}

TEST("prose apply - a code change and a prose violation are both reported")
{
    // The code check runs first but must not swallow the prose findings behind it.
    auto const original = cc::string("int x = 1; // note\n");
    auto const r = build("## a.hh\n[1]\n| int y = 1; // One point. And a second one.\n", original);

    source_manager sources;
    apply_problems out;
    validate_rewrite(r, original, sources, out);

    CHECK(out.errors.size() == 1);
    CHECK(out.findings.size() == 1);
}

TEST("prose apply - findings resolve against the rewritten text, not the file")
{
    // The carets have to land on the prose the plan WROTE; nothing of it is on disk yet.
    auto const original = cc::string("# Title\n\nold\n");
    auto const r = build("## a.md\n[3]\n| One point. And a second one on the same line.\n", original);

    source_manager sources;
    apply_problems out;
    validate_rewrite(r, original, sources, out);

    REQUIRE(out.findings.size() == 1);
    auto const& buffer = sources.buffer(out.findings[0].span.file_id);
    CHECK(buffer.path() == "a.md");
    CHECK(buffer.line_text(3) == "One point. And a second one on the same line.");
}
