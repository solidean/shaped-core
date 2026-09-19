#include "ast-test-support.hh"

#include <nexus/test.hh>

using sgl_test::ast_of;
using sgl_test::body_of;

TEST("sgl ast - a struct holds fields, properties, methods and nested declarations")
{
    CHECK(ast_of("struct material:\n"
                 "    albedo: vec3\n"
                 "    roughness: float = 0.5\n"
                 "    is_rough => roughness > 0.5\n"
                 "    fun shade(self, n: vec3) -> vec3 => albedo * n\n"
                 "    fun reset(mut self):\n"
                 "        self.roughness = 0.5\n"
                 "    fun matte(albedo: vec3) -> material => {albedo, roughness = 1.0}\n"
                 "    const max_roughness = 1.0\n"
                 "    struct inner:\n"
                 "        x: int\n")
          == "(struct material\n"
             "  (field albedo : vec3)\n"
             "  (field roughness : float = num:0.5)\n"
             "  (property is_rough => (call:infix > roughness num:0.5))\n"
             "  (fun shade (params (field self) (field n : vec3)) -> vec3 => (call:infix * albedo n))\n"
             "  (fun reset (params (field mut self))\n"
             "    (assign = (member self roughness) num:0.5))\n"
             "  (fun matte (params (field albedo : vec3)) -> material => (object albedo=<shorthand> "
             "roughness=num:1.0))\n"
             "  (const max_roughness = num:1.0)\n"
             "  (struct inner\n"
             "    (field x : int)))");
}

TEST("sgl ast - self or mut self as the first parameter makes an instance method")
{
    auto const file = sgl::parse("struct s:\n"
                                 "    fun a(self) => 1\n"
                                 "    fun b(mut self, x: int) => 2\n"
                                 "    fun c(x: int) => 3\n"
                                 "    fun d(self: s) => 4\n");
    auto const ast = sgl::ast::build(file);
    CHECK(sgl::ast::dump_diagnostics(ast) == "");

    auto receivers = cc::vector<sgl::ast::receiver_kind>();
    for (auto const& d : ast.decls)
        if (auto const* f = d.node.try_as<sgl::ast::fun_decl>())
            receivers.push_back(f->receiver);
    REQUIRE(receivers.size() == 4);
    CHECK(receivers[0] == sgl::ast::receiver_kind::self);
    CHECK(receivers[1] == sgl::ast::receiver_kind::mut_self);
    // Without `self` a method is static, and a typed `self` is an ordinary parameter.
    CHECK(receivers[2] == sgl::ast::receiver_kind::none);
    CHECK(receivers[3] == sgl::ast::receiver_kind::none);
}

TEST("sgl ast - an enum holds cases, properties, methods and nested declarations")
{
    CHECK(ast_of("enum light_kind:\n"
                 "    point\n"
                 "    spot\n"
                 "    @deprecated area\n"
                 "    is_local => self != .area\n"
                 "    fun falloff(self, d: float) -> float => 1 / d\n"
                 "    const count = 3\n")
          == "(enum light_kind\n"
             "  (case point)\n"
             "  (case spot)\n"
             "  (case{@deprecated} area)\n"
             "  (property is_local => (call:infix != self .area))\n"
             "  (fun falloff (params (field self) (field d : float)) -> float => (call:infix / num:1 d))\n"
             "  (const count = num:3))");
}

TEST("sgl ast - a property may compute its value in a block")
{
    CHECK(ast_of("struct s:\n    area =>:\n        let w = size.x\n        w * size.y\n")
          == "(struct s\n"
             "  (property area\n"
             "    (let w = (member size x))\n"
             "    (call:infix * w (member size y))))");
}

TEST("sgl ast - a field carries its attributes")
{
    CHECK(ast_of("struct v:\n    @location(0) pos: pos3\n    uv: vec2 @location(1)\n")
          == "(struct v\n"
             "  (field{@location(0)} pos : pos3)\n"
             "  (field{@location(1)} uv : vec2))");
}

TEST("sgl ast - what each owner refuses")
{
    // A struct has no cases.
    CHECK(ast_of("struct s:\n    point\n") == "(struct s\n  (case point)) !! member-not-allowed-here @14+5\n");
    // An enum has no fields.
    CHECK(ast_of("enum e:\n    x: int\n") == "(enum e\n  (field x : int)) !! member-not-allowed-here @12+6\n");
    // A binding has no methods and no nested declarations.
    CHECK(ast_of("binding b:\n    fun f(self) => 1\n")
          == "(binding b\n  (fun f (params (field self)) => num:1)) !! member-not-allowed-here @15+3\n");
    CHECK(ast_of("binding b:\n    const k = 1\n")
          == "(binding b\n  (const k = num:1)) !! member-not-allowed-here @15+5\n");
    // A binding member has no default.
    CHECK(ast_of("binding b:\n    scale: float = 1.0\n")
          == "(binding b\n  (field scale : float = num:1.0)) !! default-not-allowed-here @30+3\n");
}

TEST("sgl ast - a line that is no member")
{
    CHECK(ast_of("struct s:\n    f(x)\n    x: int\n")
          == "(struct s\n  (invalid-decl \"f(x)\")\n  (field x : int)) !! expected-member @14+4\n");
    CHECK(ast_of("struct s:\n    x = 5\n") == "(struct s\n  (invalid-decl \"x = 5\")) !! expected-member @14+5\n");
    CHECK(ast_of("struct s:\n    if a => b\n") == "(struct s\n  (invalid-decl \"if a => b\")) !! expected-member @14+2\n");
    CHECK(ast_of("struct s:\n    let x = 5\n")
          == "(struct s\n  (invalid-decl \"let x = 5\")) !! declaration-not-allowed-here @14+3\n");
    CHECK(ast_of("struct s:\n    module m\n").contains("misplaced-module"));
    CHECK(ast_of("struct s:\n    sampler l\n").contains("declaration-not-allowed-here"));
}

TEST("sgl ast - struct, enum and binding are declared by one name")
{
    CHECK(ast_of("@builtin struct bool\n") == "(struct{@builtin} bool)");
    CHECK(ast_of("struct:\n    x: int\n") == "(struct <missing>\n  (field x : int)) !! expected-name @0+6\n");
    CHECK(ast_of("enum 5:\n    a\n") == "(enum <missing>\n  (case a)) !! expected-name @5+1\n");
    CHECK(ast_of("struct a, b\n") == "(struct a) !! too-many-arguments @10+1\n");
    CHECK(ast_of("struct a = b\n") == "(struct a) !! unexpected-token @9+1\n");
    CHECK(body_of("struct local:\n    x: int\nenum e:\n    a\n")
          == "(struct local\n  (field x : int))\n(enum e\n  (case a))");
}
