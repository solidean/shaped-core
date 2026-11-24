#include "check-test-support.hh"

using namespace sgl_test;

// Structs with cases (CHK-540 to CHK-551): what a declaration says of its layout, and what it refuses.
// What a body does with one is the corpus's, in tests/corpus/types/struct-cases.sgl and its neighbours.

namespace
{
/// The `(struct …)` line of the module dump, which carries its members, its cases and its variant fields.
cc::string struct_line(sgl::check::checked_module const& m, cc::string_view name)
{
    auto const text = sgl::check::dump(m);
    auto const head = cc::format("(struct {}", name);
    auto const at = cc::string_view(text).find(cc::string_view(head));
    if (at < 0)
        return {};
    auto const rest = cc::string_view(text).subview({.offset = at, .size = text.size() - at});
    auto const end = rest.find('\n');
    return cc::string(end < 0 ? rest : rest.subview({.offset = 0, .size = end}));
}
} // namespace

TEST("sgl check - a struct with cases is one carrier: its common fields, the tag in a hole they leave, then the words")
{
    auto const s = check_sources(read_prelude(), "struct shape:\n"
                                                 "    tint: float3\n"
                                                 "    case sphere(center: float3, radius: float)\n"
                                                 "    case flat:\n"
                                                 "        normal: float3\n"
                                                 "        case disk(radius: float)\n"
                                                 "        case plane\n");
    CHECK(reports_of(s) == "");
    auto const line = struct_line(s.module, "shape");
    // the tag fills the four bytes a float3 leaves before the next 16-byte boundary
    CHECK(line.contains("(tint : float3) (_tag : uint) (_w0 : uint)"));
    // a case that holds cases is no leaf, and its field has the same words in each of its leaves
    CHECK(line.contains("(case sphere leaf:0) (case flat) (case disk leaf:1 in:flat) (case plane leaf:2 in:flat)"));
    CHECK(line.contains("(variant center : float3 words:0..<3) (variant radius : float words:3..<4)"));
    CHECK(line.contains("(variant normal : float3 words:0..<3) (variant radius : float words:3..<4)"));
}

TEST("sgl check - a struct whose cases its @whens tell apart has no tag, and one they leave values outside is marked "
     "so")
{
    auto const s = check_sources(read_prelude(), "struct reading:\n"
                                                 "    value: int\n"
                                                 "    @when(value == 0)\n"
                                                 "    case zero\n"
                                                 "    @when(value == 1)\n"
                                                 "    case one\n");
    CHECK(reports_of(s) == "");
    auto const line = struct_line(s.module, "reading");
    CHECK(!line.contains("_tag"));
    CHECK(line.contains(" uncovered"));
}

TEST("sgl check - one level's cases are told apart by @whens on each or on none, and never overlap")
{
    // CHK-546: a @when on some cases of a level and not on others
    CHECK(reports_for("struct s:\n    x: int\n    @when(x == 0)\n    case a\n    case b\n")
              .contains("invalid-attribute-arguments"));
    // two cases one value is in
    CHECK(reports_for("struct s:\n    x: int\n    @when(x >= 0)\n    case a\n    @when(x <= 0)\n    case b\n")
              .contains("overlapping-cases"));
    // a condition outside the checked fragment needs @unchecked
    CHECK(reports_for("struct s:\n    x: float\n    @when(x * x > 1.0)\n    case a\n    @when(x * x <= 1.0)\n    case "
                      "b\n")
              .contains("invalid-attribute-arguments"));
    CHECK(reports_for("struct s:\n    x: float\n    @unchecked @when(x * x > 1.0)\n    case a\n"
                      "    @unchecked @when(x * x <= 1.0)\n    case b\n")
          == "");
}

TEST("sgl check - what a struct with cases refuses of its names and its attributes")
{
    // CHK-540: a case's name is one in its struct
    CHECK(reports_for("struct s:\n    case a\n    case b:\n        case a\n        case c\n")
              .contains("duplicate-declaration"));
    // CHK-543: a field of a case is no other field the same value holds
    CHECK(reports_for("struct s:\n    x: int\n    case a(x: float)\n    case b\n").contains("duplicate-declaration"));
    CHECK(reports_for("struct s:\n    case a(x: float)\n    case b(x: int)\n") == "");
    // the layout's own names are no field's
    CHECK(reports_for("struct s:\n    _tag: int\n    case a\n    case b\n").contains("duplicate-declaration"));
    // CHK-548: @cases names cases by a leading dot
    CHECK(reports_for("struct s:\n    @cases(.c)\n    y: int\n    case a\n    case b\n")
              .contains("invalid-attribute-arguments"));
    CHECK(reports_for("struct s:\n    @cases(.a)\n    y: int\n").contains("invalid-attribute-arguments"));
    // CHK-547: @tag(.external) is the one tag it takes, and only a struct with cases takes it
    CHECK(reports_for("@tag(.inline)\nstruct s:\n    case a\n    case b\n").contains("invalid-attribute-arguments"));
    CHECK(reports_for("@tag(.external)\nstruct s:\n    x: int\n").contains("invalid-attribute-arguments"));
    // CHK-238: a case is a member of its type scope, as a field is
    CHECK(reports_for("struct s:\n    case a\n    case b\n    a => 1\n").contains("member-name-clash"));
}

TEST("sgl check - a struct with cases has no constructor of its own name, and a leaf's takes the common fields first")
{
    CHECK(reports_for("struct s:\n    x: int\n    case a(y: float)\n    case b\n\ntest s(1).x == 1\n") != "");
    CHECK(reports_for("struct s:\n    x: int\n    case a(y: float)\n    case b\n\ntest s.a(1, 2.0).x == 1\n") == "");
    // a block's fields after a paren payload are named
    CHECK(reports_for("struct s:\n    case a(y: float):\n        z: int = 0\n    case b\n\ntest s.a(2.0, z = 1).z == "
                      "1\n")
          == "");
}

TEST("sgl check - a case of a struct is a type where a value's cases are kept, and nowhere else")
{
    CHECK(reports_for("struct s:\n    case a\n    case b\n\nfun f(v: s.a) -> s.a => v\n") == "");
    CHECK(reports_for("struct s:\n    case a\n    case b\n\nstruct holder:\n    v: s.a\n").contains("unsupported-yet"));
}
