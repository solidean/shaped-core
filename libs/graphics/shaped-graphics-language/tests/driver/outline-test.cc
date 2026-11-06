#include <nexus/test.hh>
#include <shaped-graphics-language/ast/build.hh>
#include <shaped-graphics-language/driver/outline.hh>
#include <shaped-graphics-language/syntax/parsed_file.hh>

namespace
{
/// The outline as `kind name` per entry, children indented two spaces under their parent.
[[nodiscard]] cc::string outline(cc::string_view source)
{
    auto const file = sgl::parse(cc::string(source));
    auto const ast = sgl::ast::build(file);
    auto out = cc::string();
    constexpr cc::string_view kinds[] = {"function", "method", "property", "struct",   "enum", "case", "field",
                                         "binding",  "const",  "sampler",  "pipeline", "type", "test", "module"};
    auto const write = [&](auto const& self, cc::span<sgl::outline_entry const> entries, int depth) -> void
    {
        for (auto const& e : entries)
        {
            for (auto i = 0; i < depth; ++i)
                out += "  ";
            out.appendf("{} {}\n", kinds[int(e.kind)], e.name);
            self(self, e.children, depth + 1);
        }
    };
    write(write, sgl::outline_of(file, ast), 0);
    return out;
}
} // namespace

TEST("sgl outline - each declaration with what it holds, from the tree alone")
{
    CHECK(outline("module shading\n"
                  "struct light:\n"
                  "    power: float\n"
                  "    half_power => power * 0.5\n"
                  "    fun scaled(self, k: float) -> float => power * k\n"
                  "enum kind:\n"
                  "    point\n"
                  "    spot\n"
                  "binding frame:\n"
                  "    exposure: float\n"
                  "    sampler clamped:\n"
                  "        address = .clamp\n"
                  "const scale = 2.0\n"
                  "type color = float4\n"
                  "fun light.doubled(self) -> float => self.power * 2.0\n"
                  "fun shade(l: light) -> float:\n"
                  "    return l.power\n"
                  "test:\n"
                  "    check(1 == 1)\n")
          == "module shading\n"
             "struct light\n"
             "  field power\n"
             "  property half_power\n"
             "  method scaled\n"
             "enum kind\n"
             "  case point\n"
             "  case spot\n"
             "binding frame\n"
             "  field exposure\n"
             "  field clamped\n"
             "const scale\n"
             "type color\n"
             "method light.doubled\n"
             "function shade\n"
             "test test\n");
}

TEST("sgl outline - an entry spans its block, names its name, and shows its first line without the colon")
{
    auto const source = cc::string_view("fun shade(l: float) -> float:\n    return l\n");
    auto const file = sgl::parse(cc::string(source));
    auto const ast = sgl::ast::build(file);
    auto const entries = sgl::outline_of(file, ast);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].detail == "fun shade(l: float) -> float");
    CHECK(file.text_of(entries[0].name_span) == "shade");
    CHECK(entries[0].where.offset == 0);
    CHECK(entries[0].where.end() >= source.find("return l") + 8);
}

TEST("sgl outline - a line folds through its last deeper line, and a run of comment lines folds as a comment")
{
    auto const file = sgl::parse("// one\n"
                                 "// two\n"
                                 "fun f() -> float:\n"
                                 "    let a = 1.0\n"
                                 "\n"
                                 "    return a\n"
                                 "\n"
                                 "fun g() -> float => 0.0\n");
    auto const folds = sgl::folds_of(file);
    auto has = [&](sgl::u32 first, sgl::u32 last, bool is_comment)
    {
        for (auto const& f : folds)
            if (f == sgl::fold_range{.first_line = first, .last_line = last, .is_comment = is_comment})
                return true;
        return false;
    };
    CHECK(has(2, 5, false)); // the trailing blank line is not part of it
    CHECK(has(0, 1, true));
    CHECK(folds.size() == 2);
}
