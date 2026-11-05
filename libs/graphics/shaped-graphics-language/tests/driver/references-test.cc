#include "../check/check-test-support.hh"

#include <shaped-graphics-language/check/checked_module.hh>
#include <shaped-graphics-language/driver/references.hh>

using namespace sgl_test;

namespace
{
/// `source` checked behind the library's prelude, with `|` marking the offset asked about, which is taken out.
struct marked
{
    cc::string source;
    sgl::u32 offset = 0;
    checked_sources checked;
    cc::vector<sgl::check::module_file> files;

    explicit marked(cc::string_view text)
    {
        auto const bar = text.find('|');
        REQUIRE(bar >= 0);
        source = cc::string(text.subview({.offset = 0, .size = bar})) + text.subview(bar + 1);
        offset = sgl::u32(bar);
        checked = check_sources(read_prelude(), source);
        for (auto i = isize(0); i < checked.files.size(); ++i)
            files.push_back({.file = *checked.files[i], .ast = *checked.asts[i]});
    }

    [[nodiscard]] cc::optional<sgl::reference> reference() const
    {
        return sgl::reference_at(files, checked.module, checked.user_file(), offset);
    }

    /// The declared name's text, and whether it stands in the user's file or the prelude's.
    [[nodiscard]] cc::string declared(sgl::reference const& r) const
    {
        if (r.declaring_file < 0)
            return "-";
        auto const where = r.declaring_file == checked.user_file() ? "user" : "prelude";
        return cc::format("{}@{}", files[r.declaring_file].file.text_of(r.declared), where);
    }

    /// Every completion's label, space-separated, in the order given.
    [[nodiscard]] cc::string labels() const
    {
        auto out = cc::string();
        for (auto const& c : sgl::completions_at(files, checked.module, checked.user_file(), offset))
            out.appendf("{} ", c.label);
        return out;
    }

    [[nodiscard]] cc::vector<sgl::completion> completions() const
    {
        return sgl::completions_at(files, checked.module, checked.user_file(), offset);
    }
};

[[nodiscard]] bool has_label(cc::span<sgl::completion const> cs, cc::string_view label)
{
    for (auto const& c : cs)
        if (c.label == label)
            return true;
    return false;
}
} // namespace

TEST("sgl references - a local names its let, with the type the check gave it")
{
    auto const m = marked("fun f(v: vec3) -> float:\n"
                          "    let d = dot(v, v)\n"
                          "    return |d\n");
    auto const r = m.reference();
    REQUIRE(r.has_value());
    CHECK(r.value().signature == "let d : float");
    CHECK(m.declared(r.value()) == "d@user");
    CHECK(r.value().cls == sgl::token_class::local);
    // a cursor right after the name still names it
    auto const after = marked("fun f(v: vec3) -> float:\n    let mut d = dot(v, v)\n    d = 1.0\n    return d|\n");
    REQUIRE(after.reference().has_value());
    CHECK(after.reference().value().signature == "let mut d : float");
}

TEST("sgl references - a call names the function it resolved to, with its signature and its doc comment")
{
    auto const m = marked("/// Half of `x`.\n"
                          "/// Exact for every float.\n"
                          "@pure\n"
                          "fun half(x: float, .bias: float = 0.0) -> float => x * 0.5 + bias\n"
                          "fun f() -> float => ha|lf(2.0)\n");
    auto const r = m.reference();
    REQUIRE(r.has_value());
    CHECK(r.value().signature == "fun half(x: float, .bias: float = 0.0) -> float");
    CHECK(r.value().doc == "Half of `x`.\nExact for every float.");
    CHECK(m.declared(r.value()) == "half@user");

    // the declaration itself answers the same
    auto const decl = marked("/// Half of `x`.\nfun ha|lf(x: float) -> float => x * 0.5\n");
    REQUIRE(decl.reference().has_value());
    CHECK(decl.reference().value().signature == "fun half(x: float) -> float");
    CHECK(decl.reference().value().doc == "Half of `x`.");
}

TEST("sgl references - a builtin names its declaration in the prelude, doc comment included")
{
    auto const m = marked("fun f(a: float, b: float) -> float => mi|x(a, b, 0.5)\n");
    auto const r = m.reference();
    REQUIRE(r.has_value());
    CHECK(r.value().signature == "fun mix(a: float, b: float, t: float) -> float");
    CHECK(m.declared(r.value()) == "mix@prelude");
    CHECK(r.value().doc == "`a` where `t` is 0 and `b` where it is 1.");
    // one without a doc comment has its signature alone
    auto const plain = marked("fun f(v: vec3) -> float => do|t(v, v)\n");
    REQUIRE(plain.reference().has_value());
    CHECK(plain.reference().value().signature == "fun dot(a: vec3, b: vec3) -> float");
    CHECK(plain.reference().value().doc == "");
}

TEST("sgl references - a member, a parameter, a type and an enum case each name their declaration")
{
    auto const source = cc::string_view("struct light:\n"
                                        "    /// Watts.\n"
                                        "    power: float\n"
                                        "enum kind:\n"
                                        "    point\n"
                                        "    spot\n"
                                        "fun f(l: light, k: kind) -> float:\n"
                                        "    if k == kind.spot:\n"
                                        "        return l.power\n"
                                        "    return 0.0\n");
    auto const at = [&](cc::string_view before, cc::string_view word)
    {
        auto const i = source.find(before);
        REQUIRE(i >= 0);
        auto const w = cc::string(source.subview({.offset = 0, .size = i})) + before;
        auto const m = marked(w + "|" + source.subview(i + before.size()));
        auto const r = m.reference();
        REQUIRE(r.has_value());
        CHECK(m.declared(r.value()) == cc::format("{}@user", word));
        return r.value();
    };
    auto const power = at("return l.", "power");
    CHECK(power.signature == "light.power : float");
    CHECK(power.doc == "Watts.");
    CHECK(at("return ", "l").signature == "l : light");
    CHECK(at("(l: ", "light").signature == "struct light");
    CHECK(at("kind.", "spot").signature == "kind.spot = 1");
}

TEST("sgl references - a keyword, an operator and whitespace name nothing")
{
    CHECK(!marked("f|un f() -> float => 1.0\n").reference().has_value());
    CHECK(!marked("fun f() -> float => 1.0 |+ 2.0\n").reference().has_value());
    CHECK(!marked("fun f() -> float => 1.0\n|\n").reference().has_value());
}

TEST("sgl references - after a dot, the completions are what the object has")
{
    auto const source = cc::string_view("struct light:\n"
                                        "    power: float\n"
                                        "    radius: float\n"
                                        "    area => self.radius * self.radius\n"
                                        "    fun scaled(self, k: float) -> light => light(power = self.power * k, "
                                        "radius = self.radius)\n");
    // a member that parses, its prefix filtering
    auto const typed = marked(cc::string(source) + "fun f(l: light) -> float => l.po|wer\n");
    CHECK(typed.labels() == "power ");
    // every member, the methods and properties after the fields
    auto const all = marked(cc::string(source) + "fun f(l: light) -> float => l.|power\n");
    CHECK(all.labels() == "power radius area scaled ");
    // a binding's members, and an enum's cases through its name
    auto const b = marked("binding frame:\n    time: float\nenum kind:\n    point\n    spot\n"
                          "fun f(){frame} -> float:\n    let k = kind.|point\n    return frame.time\n");
    CHECK(b.labels() == "point spot ");
}

TEST("sgl references - after a dot that does not parse yet, the object's type still decides")
{
    auto const m
        = marked("struct light:\n    power: float\nfun f(l: light) -> float:\n    let p = l.|\n    return 0.0\n");
    CHECK(m.labels() == "power ");
}

TEST("sgl references - a name's completions are the locals in scope, the parameters, the declarations and the keywords")
{
    auto const m = marked("fun helper() -> float => 1.0\n"
                          "fun f(width: float) -> float:\n"
                          "    let a = 1.0\n"
                          "    if width > 0.0:\n"
                          "        let hidden = 2.0\n"
                          "    for i in 0 ..< 3:\n"
                          "        let inner = a\n"
                          "    let b = |a\n"
                          "    return b\n");
    auto const cs = m.completions();
    CHECK(has_label(cs, "a"));
    CHECK(has_label(cs, "width"));
    CHECK(has_label(cs, "helper"));
    CHECK(has_label(cs, "dot"));
    CHECK(has_label(cs, "return"));
    // a block that ended takes its locals, and a let still being written declares nothing yet
    CHECK(!has_label(cs, "hidden"));
    CHECK(!has_label(cs, "inner"));
    CHECK(!has_label(cs, "i"));
    CHECK(!has_label(cs, "b"));
    // the document's own names come first, the parameters before them
    CHECK(cs[0].label == "a");
    CHECK(cs[0].detail == "let a : float");

    // the prefix written so far filters them
    auto const typed = marked("fun helper() -> float => 1.0\nfun f() -> float => hel|\n");
    CHECK(typed.labels() == "helper ");
}

TEST("sgl references - a name declared many times is one completion, and the prelude's internals are none")
{
    auto const cs = marked("fun f() -> float => |1.0\n").completions();
    auto dots = 0;
    for (auto const& c : cs)
        dots += c.label == "dot" ? 1 : 0;
    CHECK(dots == 1);
    CHECK(!has_label(cs, "by_target"));
}
