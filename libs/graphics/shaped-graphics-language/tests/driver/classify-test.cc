#include "../check/check-test-support.hh"

#include <shaped-graphics-language/check/checked_module.hh>
#include <shaped-graphics-language/driver/classify.hh>
#include <shaped-graphics-language/driver/unannotated_bindings.hh>

using namespace sgl_test;

namespace
{
/// Each classified span of `source` checked behind the library's prelude, as `text:class`, with `!` for a declaration
/// and `^` for something the prelude declares; spans of class `op` and `keyword` are left out unless `all` is set.
cc::string classes_of(cc::string_view source, bool all = false)
{
    auto const checked = check_sources(read_prelude(), source);
    auto out = cc::string();
    auto const spans = sgl::classify(
        checked.user, checked.user_ast,
        {.module = &checked.module, .file = checked.user_file(), .prelude_file_count = checked.user_file()});
    for (auto const& s : spans)
    {
        if (!all && (s.cls == sgl::token_class::op || s.cls == sgl::token_class::keyword))
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

TEST("sgl classify - spans are in source order and never overlap, on the extension's whole sample")
{
    auto const source = read_text(cc::string(SGL_SAMPLES_DIR) + "/../../tools/vscode-extension/examples/sample.sgl");
    auto const checked = check_sources(read_prelude(), source);
    auto const spans = sgl::classify(
        checked.user, checked.user_ast,
        {.module = &checked.module, .file = checked.user_file(), .prelude_file_count = checked.user_file()});
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
    for (auto const& b : sgl::unannotated_bindings(checked.user_ast, checked.module, checked.user_file()))
        out.appendf("{} : {}\n", checked.user.text_of(b.name), checked.module.name_of(b.type));
    CHECK(out == "a : float\nc : float\n");
}
