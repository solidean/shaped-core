#include "ast-test-support.hh"

#include <nexus/test.hh>

using sgl_test::ast_of;
using sgl_test::body_of;

TEST("sgl ast - an empty file has no declarations")
{
    CHECK(ast_of("") == "");
    CHECK(ast_of("// only a comment\n\n") == "");
}

TEST("sgl ast - module comes first, and at most once")
{
    CHECK(ast_of("module example\nuse brdf\n") == "(module example)\n(use brdf)");
    CHECK(ast_of("module shaped.lighting\n") == "(module (member shaped lighting))");

    CHECK(ast_of("use brdf\nmodule example\n") == "(use brdf)\n(module example) !! misplaced-module @9+6\n");
    CHECK(ast_of("module a\nmodule b\n") == "(module a)\n(module b) !! misplaced-module @9+6\n");
    CHECK(body_of("module inner\n") == "(module inner) !! misplaced-module @13+6\n");

    CHECK(ast_of("module\n") == "(module (invalid \"module\")) !! expected-name @0+6\n");
    CHECK(ast_of("module 5\n") == "(module (invalid \"5\")) !! expected-name @7+1\n");
}

TEST("sgl ast - use, with and without an alias, at file level and in a body")
{
    CHECK(ast_of("use materials\n") == "(use materials)");
    CHECK(ast_of("use materials as mat\n") == "(use materials as mat)");
    CHECK(ast_of("use shaped.brdf as brdf\n") == "(use (member shaped brdf) as brdf)");
    CHECK(body_of("use brdf_library as brdf\n") == "(use brdf_library as brdf)");

    CHECK(ast_of("use\n") == "(use (invalid \"use\")) !! expected-name @0+3\n");
    CHECK(ast_of("use a as 5\n") == "(use a) !! expected-name @9+1\n");
    CHECK(ast_of("use a, b\n") == "(use a) !! too-many-arguments @7+1\n");
}

TEST("sgl ast - a function signature: type parameters, parameters, bindings, return type, body")
{
    CHECK(ast_of("fun make_mvp(model: mat4){frame} => frame.proj * frame.view * model\n")
          == "(fun make_mvp (params (field model : mat4)) (uses frame) => "
             "(call:infix * (call:infix * (member frame proj) (member frame view)) model))");

    CHECK(ast_of("fun lerp[T](a: T, b: T, t: float = 0.5){frame, instance} -> T:\n    return a\n")
          == "(fun lerp (type-params (field T)) (params (field a : T) (field b : T) (field t : float = num:0.5)) "
             "(uses frame instance) -> T\n"
             "  (return a))");

    CHECK(ast_of("fun f() => 1\n") == "(fun f (params) => num:1)");
    CHECK(ast_of("fun f[N: int]() => N\n") == "(fun f (type-params (field N : int)) (params) => N)");
    CHECK(ast_of("fun f(x) => x\n") == "(fun f (params (field x)) => x)");
}

TEST("sgl ast - a function without a body is a valid signature")
{
    CHECK(ast_of("fun f(x: int) -> int\n") == "(fun f (params (field x : int)) -> int)");
    CHECK(ast_of("@builtin fun sin(x: float) -> float\n") == "(fun{@builtin} sin (params (field x : float)) -> float)");
}

TEST("sgl ast - a binding entry keeps its whole element")
{
    CHECK(ast_of("fun f(){frame as f, lights = scene_lights, @slot(2) extra}\n")
          == "(fun f (params) (uses (cast frame : f) lights=scene_lights extra{@slot(num:2)}))");
}

TEST("sgl ast - the signature lists come in one order, each at most once")
{
    CHECK(ast_of("fun f{frame}(x: int) => x\n")
          == "(fun f (params (field x : int)) (uses frame) => x) !! signature-out-of-order @12+8\n");
    CHECK(ast_of("fun f(x: int)[T] => x\n")
          == "(fun f (type-params (field T)) (params (field x : int)) => x) !! signature-out-of-order @13+3\n");
    CHECK(ast_of("fun f(a)(b) => a\n") == "(fun f (params (field a)) => a) !! duplicate-signature-list @8+3\n");
    CHECK(ast_of("fun f(){a}{b}\n") == "(fun f (params) (uses a)) !! duplicate-signature-list @10+3\n");
}

TEST("sgl ast - the parameter list is mandatory")
{
    CHECK(ast_of("fun f => 1\n") == "(fun f (params) => num:1) !! missing-parameter-list @4+1\n");
    CHECK(ast_of("fun f{frame} -> int\n") == "(fun f (params) (uses frame) -> int) !! missing-parameter-list @4+1\n");
    CHECK(ast_of("fun f[T]:\n    return 1\n")
          == "(fun f (type-params (field T)) (params)\n  (return num:1)) !! missing-parameter-list @4+1\n");
}

TEST("sgl ast - a function that lost its name or its shape still has its body read")
{
    // Without a name the first list stands where the name belongs, and it is still the parameter list.
    CHECK(ast_of("fun (x) => x\n") == "(fun <missing> (params (field x)) => x) !! expected-name @4+3\n");
    CHECK(ast_of("fun:\n    return 1\n") == "(fun <missing> (params)\n  (return num:1)) !! expected-name @0+3\n");
    CHECK(ast_of("fun f(1 + 2) => 0\n")
          == "(fun f (params (field : (invalid \"1 + 2\"))) => num:0) !! expected-parameter @6+5\n");
    CHECK(ast_of("fun f() = 5\n") == "(fun f (params)) !! expected-body @8+1\n");
    CHECK(ast_of("fun f() : int => 1\n") == "(fun f (params) -> int => num:1) !! unexpected-token @8+1\n");
    CHECK(ast_of("fun f(), g()\n") == "(fun f (params)) !! too-many-arguments @9+3\n");
}

TEST("sgl ast - a return type is a type position and takes attributes")
{
    CHECK(ast_of("fun f(@a x: int, y: float @b) -> @c vec4\n")
          == "(fun f (params (field{@a} x : int) (field{@b} y : float)) -> vec4{@c})");
    CHECK(ast_of("fun vs(v: vertex) -> {\n    @position pos: hpos4\n    uv: vec2\n}:\n    return {pos = v.pos, uv = "
                 "v.uv}\n")
          == "(fun vs (params (field v : vertex)) -> (struct-type (field{@position} pos : hpos4) (field uv : vec2))\n"
             "  (return (object pos=(member v pos) uv=(member v uv))))");
    CHECK(ast_of("fun f() -> (int) -> int\n") == "(fun f (params) -> (function-type (params (field : int)) -> int))");
    // Only the first arrow of a signature is the return arrow; every other one makes a function type.
    CHECK(ast_of("fun apply(f: (float) -> float, x: float) => f x\n")
          == "(fun apply (params (field f : (function-type (params (field : float)) -> float)) (field x : float)) => "
             "(call:juxt f x))");
    CHECK(ast_of("fun f() : int\n") == "(fun f (params) -> int) !! unexpected-token @8+1\n");

    // The value right of `=>` is no type position.
    CHECK(ast_of("fun f() => @a x\n") == "(fun f (params) => x{@a}) !! misplaced-attribute-on-expression @14+1\n");
}

TEST("sgl ast - functions nest, and a body reads its declarations in source order")
{
    CHECK(ast_of("fun outer(x: float) -> float:\n    fun twice(y: float) => y * k\n    const k = 2\n    return twice "
                 "x\n")
          == "(fun outer (params (field x : float)) -> float\n"
             "  (fun twice (params (field y : float)) => (call:infix * y k))\n"
             "  (const k = num:2)\n"
             "  (return (call:juxt twice x)))");
}

TEST("sgl ast - type, const and notation")
{
    CHECK(ast_of("type color = vec4\n") == "(type color : vec4)");
    CHECK(ast_of("type grid = array[float, 16]\n") == "(type grid : (index array float num:16))");
    CHECK(ast_of("const bias = 0.5 @range(0, 1)\n") == "(const{@range(num:0 num:1)} bias = num:0.5)");
    CHECK(ast_of("const limit : int = 10\n") == "(const limit : int = num:10)");
    CHECK(ast_of("notation a dot b => dot(a, b)\n") == "(notation (call:juxt a dot b) => (call:paren dot a b))");
    CHECK(body_of("type t = int\nconst c = 1\nnotation a x b => cross(a, b)\n")
          == "(type t : int)\n(const c = num:1)\n(notation (call:juxt a x b) => (call:paren cross a b))");

    CHECK(ast_of("type color\n") == "(type color : (invalid \"type color\")) !! expected-expression @0+4\n");
    CHECK(ast_of("type = vec4\n") == "(type <missing> : vec4) !! expected-name @0+4\n");
    CHECK(ast_of("const x\n") == "(const x = (invalid \"const x\")) !! expected-expression @0+5\n");
    CHECK(ast_of("const 5 = 1\n") == "(const <missing> = num:1) !! expected-name @6+1\n");
    CHECK(ast_of("notation a\n") == "(notation a => (invalid \"notation a\")) !! expected-expression @0+8\n");
    CHECK(ast_of("type t => int\n").contains("unexpected-token"));
}

TEST("sgl ast - the right side of a type declaration is a type position")
{
    CHECK(ast_of("type callback = (float) -> float\n")
          == "(type callback : (function-type (params (field : float)) -> float))");
    CHECK(ast_of("type radiance_sample = (vec3, float)\n") == "(type radiance_sample : (tuple vec3 float))");
}

TEST("sgl ast - binding as a block and as a composition")
{
    CHECK(ast_of("binding frame:\n    view: mat4\n    tex: texture2d[rgba8]\n")
          == "(binding frame\n"
             "  (field view : mat4)\n"
             "  (field tex : (index texture2d rgba8)))");
    CHECK(ast_of("binding scene = frame\n") == "(binding scene = frame)");
    CHECK(ast_of("binding scene = (frame, instance)\n") == "(binding scene = (tuple frame instance))");
    CHECK(ast_of("binding empty\n") == "(binding empty)");
    CHECK(ast_of("binding scene += frame\n") == "(binding scene = frame) !! unexpected-token @14+2\n");
}

TEST("sgl ast - a local binding supplies a library's binding and may reach local variables")
{
    CHECK(body_of("let t = now()\nbinding timing:\n    time => t + 1.5\n    scale: float\nrender(){timing}\n")
          == "(let t = (call:paren now))\n"
             "(binding timing\n"
             "  (property time => (call:infix + t num:1.5))\n"
             "  (field scale : float))\n"
             "(with-bindings (call:paren render) timing=<shorthand>)"
             " !! unsupported-syntax @104+8\n");
}

TEST("sgl ast - a sampler is a list of settings, at file level only")
{
    CHECK(ast_of("sampler bilinear:\n    filter = .linear\n    address = .clamp\n    anisotropy = 16\n")
          == "(sampler bilinear\n"
             "  filter=.linear\n"
             "  address=.clamp\n"
             "  anisotropy=num:16)");
    CHECK(ast_of("sampler bare\n") == "(sampler bare)");

    CHECK(ast_of("sampler s:\n    filter: linear\n")
          == "(sampler s\n  (invalid \"filter: linear\")) !! expected-member @15+14\n");
    CHECK(body_of("sampler s:\n    filter = .linear\n")
          == "(sampler s\n  filter=.linear) !! declaration-not-allowed-here @13+7\n");
}

TEST("sgl ast - what is no declaration at file level")
{
    CHECK(ast_of("let x = 5\n") == "(invalid-decl \"let x = 5\") !! declaration-not-allowed-here @0+3\n");
    CHECK(ast_of("f(x)\n") == "(invalid-decl \"f(x)\") !! expected-declaration @0+4\n");
    CHECK(ast_of("x = 5\n") == "(invalid-decl \"x = 5\") !! expected-declaration @0+5\n");
    CHECK(ast_of("if a:\n    f()\n") == "(invalid-decl \"if a:\") !! expected-declaration @0+2\n");
    CHECK(ast_of("mut x\n") == "(invalid-decl \"mut x\") !! unexpected-keyword @0+3\n");
    CHECK(ast_of("struct enum a\n") == "(invalid-decl \"struct enum a\") !! unexpected-keyword @7+4\n");

    // The rest of the file is untouched.
    CHECK(ast_of("f(x)\nconst k = 1\n") == "(invalid-decl \"f(x)\")\n(const k = num:1) !! expected-declaration @0+4\n");
}

TEST("sgl ast - attributes on declarations are an open set")
{
    CHECK(ast_of("@vertex struct v:\n    pos: pos3\n") == "(struct{@vertex} v\n  (field pos : pos3))");
    CHECK(ast_of("@vertex\n@inline fun vs()\n") == "(fun{@vertex @inline} vs (params))");
    CHECK(ast_of("@nobody_knows_this(1, \"two\") const k = 1\n")
          == "(const{@nobody_knows_this(num:1 str:\"two\")} k = num:1)");
}

TEST("sgl ast - the arguments of an attribute are list elements like any other")
{
    CHECK(ast_of("@slider(0, max = 1, step = 1 / 8) const k = 1\n")
          == "(const{@slider(num:0 max=num:1 step=(call:infix / num:1 num:8))} k = num:1)");
    CHECK(ast_of("@flags(..defaults, .srgb) const k = 1\n") == "(const{@flags(..defaults .srgb)} k = num:1)");
    CHECK(ast_of("@empty() const k = 1\n") == "(const{@empty()} k = num:1)");
    CHECK(ast_of("@a(f(x)) @b const k = 1\n") == "(const{@a((call:paren f x)) @b} k = num:1)");

    // What is wrong inside an argument is found like anywhere else.
    CHECK(ast_of("@a(x.y = 1) const k = 1\n") == "(const{@a((invalid \"x.y = 1\"))} k = num:1) !! expected-name @3+7\n");

    auto const file = sgl::parse("@range(0, hi = 1) const k = 1\n@bare const j = 2\n");
    auto const ast = sgl::ast::build(file);
    REQUIRE(ast.attributes.size() == 2);
    CHECK(file.text_of(ast.attributes[0].name) == "range");
    CHECK(file.at(ast.attributes[0].list).kind == sgl::form_kind::round_list);
    auto const arguments = ast.at(ast.attributes[0].arguments);
    REQUIRE(arguments.size() == 2);
    CHECK(arguments[0].name.empty());
    CHECK(file.text_of(arguments[1].name) == "hi");
    CHECK(!sgl::is_valid(ast.attributes[1].list));
    CHECK(ast.attributes[1].arguments.empty());
}

TEST("sgl ast - the value copies and compares whole, and the parsed file is left alone")
{
    auto const source = cc::string_view("module m\nfun f(x: int) -> int:\n    if x > 0 => return x\n    return -x\n");
    auto const file = sgl::parse(source);
    auto const forms_before = file.forms.size();
    auto const ast = sgl::ast::build(file);
    CHECK(file.forms.size() == forms_before);

    // A copy is equal until it is changed, and the original does not follow it: the value holds no pointer into itself.
    auto copy = ast;
    CHECK(copy == ast);
    copy.diagnostics.push_back({.kind = sgl::diagnostic_kind::no_effect, .level = sgl::severity::warning, .where = {}});
    CHECK(!(copy == ast));
    CHECK(ast.diagnostics.empty());
    CHECK(sgl::ast::build(sgl::parse(source)) == ast);
    CHECK(!(sgl::ast::build(sgl::parse("module n\n")) == ast));

    // Every node names the form it came from.
    for (auto const& e : ast.exprs)
        CHECK(sgl::is_valid(e.form));
    for (auto const& s : ast.stmts)
        CHECK(sgl::is_valid(s.form));
    for (auto const& d : ast.decls)
        CHECK(sgl::is_valid(d.form));
}
