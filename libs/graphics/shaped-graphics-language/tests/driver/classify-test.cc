#include "../check/check-test-support.hh"

#include <shaped-graphics-language/check/checked_module.hh>
#include <shaped-graphics-language/driver/classify.hh>
#include <shaped-graphics-language/driver/inferred_results.hh>
#include <shaped-graphics-language/driver/unannotated_bindings.hh>

using namespace sgl_test;

namespace
{
/// Each classified span of `source` checked behind the library's prelude, as `text:class`, with `!` for a declaration
/// and `^` for something the prelude declares; spans of class `op`, `keyword` and `control` are left out unless `all` is set.
cc::string classes_of(cc::string_view source, bool all = false)
{
    auto const checked = check_sources(read_prelude(), source);
    auto out = cc::string();
    auto const spans
        = sgl::classify(checked.user, checked.user_ast, {.module = &checked.module, .file_index = checked.user_file()});
    for (auto const& s : spans)
    {
        if (!all
            && (s.cls == sgl::token_class::op || s.cls == sgl::token_class::keyword || s.cls == sgl::token_class::control))
            continue;
        out.appendf("{}:{}{}{} ", checked.user.text_of(s.where), sgl::to_string(s.cls), s.is_declaration ? "!" : "",
                    s.is_from_prelude ? "^" : "");
    }
    return out;
}
} // namespace

TEST("sgl classify - the syntax alone tells keywords, numbers, strings, comments and operators apart")
{
    auto const file = sgl::parse("// note\n/// doc\nfun f():\n    let x = -1.5e3 + y\n    print \"hi\"\n");
    auto const ast = sgl::ast::build(file);
    auto out = cc::string();
    for (auto const& s : sgl::classify(file, ast))
        out.appendf("{}:{} ", file.text_of(s.where), sgl::to_string(s.cls));
    CHECK(out
          == "// note:comment /// doc:doc-comment fun:keyword f:function let:keyword x:local =:op -1.5e3:number +:op "
             "y:name "
             "print:keyword \":string hi:string \":string ");
}

TEST("sgl classify - a checked module says what every name is")
{
    CHECK(classes_of("struct light:\n    dir: vec3\n    power: float = 1.0\n")
          == "light:struct! dir:field! vec3:struct^ power:field! float:struct^ 1.0:number ");
    CHECK(classes_of("fun shade(n: vec3, l: light) -> float:\n    let mut d = dot(n, l.dir)\n    return d * l.power\n"
                     "struct light:\n    dir: vec3\n    power: float\n")
          == "shade:function! n:parameter! vec3:struct^ l:parameter! light:struct float:struct^ d:mutable-local! "
             "dot:function^ n:parameter l:parameter dir:field d:mutable-local l:parameter power:field "
             "light:struct! dir:field! vec3:struct^ power:field! float:struct^ ");
    CHECK(classes_of("enum mode:\n    a\n    b\nconst pick = mode.b\n")
          == "mode:enum! a:enum-case! b:enum-case! pick:constant! mode:enum b:enum-case ");
    CHECK(classes_of("binding frame:\n    time: float\nfun now(){frame} -> float => frame.time\n")
          == "frame:binding! time:binding-member! float:struct^ now:function! frame:binding float:struct^ "
             "frame:binding "
             "time:binding-member ");
}

TEST("sgl classify - methods, properties, self and attributes")
{
    CHECK(classes_of("struct box:\n    w: float\n    area => self.w * self.w\n    fun scaled(self, k: float) -> float "
                     "=> self.w * k\n")
          == "box:struct! w:field! float:struct^ area:property! self:self w:field self:self w:field scaled:method! "
             "self:self! k:parameter! "
             "float:struct^ float:struct^ self:self w:field k:parameter ");
    CHECK(classes_of("@inline\nfun f() -> int => 1\n", true).starts_with("@inline:attribute fun:keyword f:function!"));
}

TEST("sgl classify - the type an extension extends is classed as every other use of it")
{
    CHECK(classes_of("struct box:\n    w: float\nfun box.wide => self.w > 1.0\n")
              .contains("box:struct wide:property! self:self w:field"));
    CHECK(classes_of("fun float3.sum => self.x + self.y + self.z\n").starts_with("float3:struct^ sum:property!"));
    // without a check it is only known to be some type
    auto const file = sgl::parse("fun box.wide => self.w > 1.0\n");
    auto const ast = sgl::ast::build(file);
    CHECK(sgl::classify(file, ast)[1].cls == sgl::token_class::type);
}

TEST("sgl classify - control flow words are their own class, and so is the name a named argument gives")
{
    CHECK(classes_of("fun f(a: bool, b: bool) -> int:\n    if a and not b:\n        return 1\n    return 0\n", true)
              .contains("if:control a:parameter and:control not:control b:parameter"));
    CHECK(classes_of("struct span:\n    lo: float\n    hi: float\nfun w(s: span) -> float => s.hi - s.lo\n"
                     "test w({lo = 1.0, hi = 2.0}) == 1.0\n")
              .contains("lo:argument 1.0:number hi:argument"));
}

TEST("sgl classify - a call of a builtin that constrains control flow says so, by the overload it chose")
{
    // a barrier, a derivative and an atomic `max`, and a plain `max` that shares the atomic's name
    constexpr auto source = "@workgroup binding counters:\n"
                            "    slots: atomic[uint][4]\n"
                            "fun sync():\n"
                            "    workgroup_barrier()\n"
                            "fun edge(x: float) -> float => ddx(x)\n"
                            "fun raise(){counters} -> uint => counters.slots[2].max(3)\n"
                            "fun larger(a: float, b: float) -> float => max(a, b)\n";
    auto const checked = check_sources(read_prelude(), source);
    REQUIRE(reports_of(checked) == "");
    auto out = cc::string();
    for (auto const& s :
         sgl::classify(checked.user, checked.user_ast, {.module = &checked.module, .file_index = checked.user_file()}))
        if (s.constrains_control_flow)
            out.appendf("{}:{} ", checked.user.text_of(s.where), sgl::to_string(s.cls));
    CHECK(out == "workgroup_barrier:function ddx:function max:function ");
}

TEST("sgl classify - spans are in source order and never overlap, on the extension's whole sample")
{
    auto const source = read_text(cc::string(SGL_SAMPLES_DIR) + "/../../tools/vscode-extension/examples/sample.sgl");
    auto const checked = check_sources(read_prelude(), source);
    auto const spans
        = sgl::classify(checked.user, checked.user_ast, {.module = &checked.module, .file_index = checked.user_file()});
    REQUIRE(!spans.empty());
    for (auto i = isize(1); i < spans.size(); ++i)
        CHECK(spans[i - 1].where.end() <= spans[i].where.offset);
    // every symbol token has a class, so nothing a reader sees is left uncoloured
    for (auto const& t : checked.user.tokens)
        if (t.kind == sgl::token_kind::symbol)
        {
            auto found = false;
            for (auto const& s : spans)
                found = found || (s.where.offset <= t.where.offset && t.where.end() <= s.where.end());
            CHECK(found);
        }
}

TEST("sgl classify - an unannotated let has the type the check gave it, and an annotated or failed one has none")
{
    auto const checked = check_sources(read_prelude(), "fun f(v: vec3) -> float:\n"
                                                       "    let a = 1.0\n"
                                                       "    let b : float = 2.0\n"
                                                       "    let mut c = dot(v, v)\n"
                                                       "    let d = nope\n"
                                                       "    return a + b + c\n");
    auto out = cc::string();
    for (auto const& b : sgl::unannotated_bindings(checked.user, checked.user_ast, checked.module, checked.user_file()))
        out.appendf("{} : {}\n", checked.user.text_of(b.name), checked.module.name_of(b.type));
    CHECK(out == "a : float\nc : float\n");
}

TEST("sgl classify - a let whose value is a call of the type's own name says so, which an editor's hint leaves out")
{
    auto const checked = check_sources(read_prelude(), "fun f() -> float:\n"
                                                       "    let v = vec3(1.0, 2.0, 3.0)\n"
                                                       "    let x = v.x\n"
                                                       "    return x\n");
    auto out = cc::string();
    for (auto const& b : sgl::unannotated_bindings(checked.user, checked.user_ast, checked.module, checked.user_file()))
        out.appendf("{}{} ", checked.user.text_of(b.name), b.is_type_named ? " named" : "");
    CHECK(out == "v named x ");
}

TEST("sgl classify - an arrow body without a return type has the one the check inferred, placed before its arrow")
{
    auto const source = cc::string_view("fun half(x: float) => x * 0.5\n"
                                        "fun written(x: float) -> float => x\n"
                                        "fun block(x: float):\n"
                                        "    print x\n"
                                        "fun broken() => nope\n"
                                        "struct box:\n"
                                        "    w: float\n"
                                        "    area => self.w * self.w\n"
                                        "    fun twice(self) => self.w * 2.0\n"
                                        "fun box.wide => self.w > 1.0\n");
    auto const checked = check_sources(read_prelude(), source);
    auto out = cc::string();
    for (auto const& r : sgl::inferred_results(checked.user, checked.user_ast, checked.module, checked.user_file()))
    {
        // the text up to the arrow, so the place a `-> type` goes is visible
        auto line_start = isize(r.arrow.offset);
        while (line_start > 0 && source[line_start - 1] != '\n')
            --line_start;
        out.appendf("{}|{} {}{}\n", source.subview({.offset = line_start, .size = r.arrow.offset - line_start}),
                    checked.user.text_of(r.arrow), checked.module.name_of(r.type), r.is_writable ? "" : " shown");
    }
    CHECK(out
          == "fun half(x: float) |=> float\n"
             "    area |=> float shown\n"
             "    fun twice(self) |=> float\n"
             "fun box.wide |=> bool\n");
}
