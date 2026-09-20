#include "../legalize/flat-test-support.hh"

#include <shaped-graphics-language/emit/emit.hh>

using namespace sgl_test;
using namespace sgl::check;

namespace
{
/// A builder for `@pixel fun shade(p: frag) -> target`, which is an entry point every target can write.
flat_builder pixel_function(checked_module const& m)
{
    auto const types = flat_builder{.m = m};
    return flat_builder::create(
        m, {.name = "shade", .input = types.type_named("frag"), .result = types.type_named("target")});
}

/// Everything after the declarations: the function alone, which is all a test of control flow is about.
cc::string function_text(checked_module const& m, flat_entry_point const& e, sgl::emit::target t)
{
    auto const emitted = sgl::emit::emit_entry_point(m, e, t);
    CHECK(sgl::emit::dump_errors(emitted) == "");
    auto const head = t == sgl::emit::target::wgsl ? cc::string_view("@fragment") : cc::string_view("shade(");
    auto at = emitted.text.find(head);
    if (at < 0)
        return emitted.text;
    // back to the start of the line the head stands in
    while (at > 0 && emitted.text[at - 1] != '\n')
        --at;
    return cc::string(cc::string_view(emitted.text).subview({.start = at, .end = emitted.text.size()}));
}

/// Every core construct once: both kinds of `var`, the three loops, a `once`, an `else if`, and the logical operators.
flat_entry_point every_core_construct(checked_module const& m)
{
    auto b = pixel_function(m);
    auto const float_type = b.type_named("float");
    auto const int_type = b.type_named("int");
    auto const bool_type = b.type_named("bool");
    auto const a = [&] { return b.member(b.local(local_id(0)), "a"); };

    auto const acc = b.var("acc", float_type, b.literal(0.0));
    auto const index = b.add_local(local_kind::index, "i", int_type);
    auto const rows = b.add_label("rows");
    auto const n = b.var("n", int_type, b.int_literal(-1));
    auto const counting = b.add_label("counting");
    auto const doubling = b.add_label("doubling");
    auto const found = b.var("found", bool_type);
    auto const get = [&](flat_builder::declared const& d) { return b.local(d.local); };
    auto const color = [&](flat_expr_id value)
    { return b.construct(b.e.result, {b.construct(b.type_named("float4"), {value, value, value, b.literal(1.0)})}); };

    b.set_body({
        acc.stmt,
        b.for_(rows, index, b.int_literal(0), b.int_literal(4),
               {
                   b.if_(b.call("equal_int", {b.local(index), b.int_literal(2)}), {b.continue_(rows)}),
                   b.assign(get(acc), b.call("add", {get(acc), a()})),
               }),
        n.stmt,
        b.while_(counting, b.call("less_int", {get(n), b.int_literal(3)}),
                 {b.assign(get(n), b.call("add_int", {get(n), b.int_literal(1)}))}),
        b.loop(doubling,
               {
                   b.if_(b.or_(b.and_(b.call("less", {b.literal(10.0), get(acc)}),
                                      b.not_(b.call("equal_int", {get(n), b.int_literal(7)}))),
                               b.call("less", {a(), b.literal(0.0)})),
                         {b.break_()}),
                   b.assign(get(acc), b.call("multiply", {get(acc), b.call("subtract", {b.literal(3.0), a()})})),
               }),
        found.stmt,
        b.assign(get(found), b.bool_literal(false)),
        b.once({
            b.if_(b.call("less", {a(), b.literal(0.5)}), {b.assign(get(found), b.bool_literal(true)), b.break_()}),
            b.assign(get(acc), b.call("subtract", {get(acc), b.literal(1.0)})),
        }),
        b.if_(get(found), {b.return_(color(get(acc)))},
              {b.if_(b.call("less_int", {get(n), b.int_literal(0)}), {b.return_(color(b.literal(0.0)))},
                     {b.assign(get(acc), b.literal(0.5))})}),
        b.return_(color(get(acc))),
    });
    return cc::move(b.e);
}

/// The search that needs rule X5: the first row whose weight is small enough leaves both the loop and the block.
flat_entry_point structured_search(checked_module const& m)
{
    auto b = pixel_function(m);
    auto const float_type = b.type_named("float");
    auto const int_type = b.type_named("int");
    auto const search = b.add_label("search");
    auto const rows = b.add_label("rows");
    auto const index = b.add_local(local_kind::index, "i", int_type);
    auto const weight = b.var("weight", float_type, b.literal(1.0));
    auto const a = [&] { return b.member(b.local(local_id(0)), "a"); };

    auto const value
        = b.block_expr(search, float_type,
                       {
                           b.for_(rows, index, b.int_literal(0), b.int_literal(8),
                                  {
                                      b.assign(b.local(weight.local), b.call("multiply", {b.local(weight.local), a()})),
                                      b.if_(b.call("less", {b.local(weight.local), b.literal(0.125)}),
                                            {b.leave(search, b.local(weight.local))}),
                                  }),
                           b.leave(search, b.literal(0.0)),
                       });
    auto const shade = b.let("shade", value);
    b.set_body({
        weight.stmt,
        shade.stmt,
        b.leave(b.e.root,
                b.construct(b.e.result, {b.construct(b.type_named("float4"), {b.local(shade.local), b.local(shade.local),
                                                                              b.local(shade.local), b.literal(1.0)})})),
    });
    return cc::move(b.e);
}
} // namespace

TEST("sgl emit - every core construct is written in every target")
{
    auto const checked = flat_test_module();
    auto const& m = checked.module;
    auto const e = every_core_construct(m);
    REQUIRE(is_core(e));

    CHECK(function_text(m, e, sgl::emit::target::hlsl_dx12)
          == "target shade(frag p)\n"
             "{\n"
             "    float acc = 0.0;\n"
             "    for (int i = 0; i < 4; ++i)\n"
             "    {\n"
             "        if (i == 2)\n"
             "        {\n"
             "            continue;\n"
             "        }\n"
             "        acc = acc + p.a;\n"
             "    }\n"
             "    int n = -1;\n"
             "    while (n < 3)\n"
             "    {\n"
             "        n = n + 1;\n"
             "    }\n"
             "    while (true)\n"
             "    {\n"
             "        if ((10.0 < acc && !(n == 7)) || p.a < 0.0)\n"
             "        {\n"
             "            break;\n"
             "        }\n"
             "        acc = acc * (3.0 - p.a);\n"
             "    }\n"
             "    bool found;\n"
             "    found = false;\n"
             "    do\n"
             "    {\n"
             "        if (p.a < 0.5)\n"
             "        {\n"
             "            found = true;\n"
             "            break;\n"
             "        }\n"
             "        acc = acc - 1.0;\n"
             "    } while (false);\n"
             "    if (found)\n"
             "    {\n"
             "        target result;\n"
             "        result.color = float4(acc, acc, acc, 1.0);\n"
             "        return result;\n"
             "    }\n"
             "    else if (n < 0)\n"
             "    {\n"
             "        target result_1;\n"
             "        result_1.color = float4(0.0, 0.0, 0.0, 1.0);\n"
             "        return result_1;\n"
             "    }\n"
             "    else\n"
             "    {\n"
             "        acc = 0.5;\n"
             "    }\n"
             "    target result_2;\n"
             "    result_2.color = float4(acc, acc, acc, 1.0);\n"
             "    return result_2;\n"
             "}\n");
    // one language: what differs between the two HLSL targets stands in the declarations
    CHECK(function_text(m, e, sgl::emit::target::hlsl_vulkan) == function_text(m, e, sgl::emit::target::hlsl_dx12));
    CHECK(function_text(m, e, sgl::emit::target::wgsl)
          == "@fragment\n"
             "fn shade(p: frag) -> target_ {\n"
             "    var acc: f32 = 0.0;\n"
             "    for (var i: i32 = 0; i < 4; i++) {\n"
             "        if i == 2 {\n"
             "            continue;\n"
             "        }\n"
             "        acc = acc + p.a;\n"
             "    }\n"
             "    var n: i32 = -1;\n"
             "    while n < 3 {\n"
             "        n = n + 1;\n"
             "    }\n"
             "    loop {\n"
             "        if (10.0 < acc && !(n == 7)) || p.a < 0.0 {\n"
             "            break;\n"
             "        }\n"
             "        acc = acc * (3.0 - p.a);\n"
             "    }\n"
             "    var found: bool;\n"
             "    found = false;\n"
             "    loop {\n"
             "        if p.a < 0.5 {\n"
             "            found = true;\n"
             "            break;\n"
             "        }\n"
             "        acc = acc - 1.0;\n"
             "        break;\n"
             "    }\n"
             "    if found {\n"
             "        return target_(vec4f(acc, acc, acc, 1.0));\n"
             "    } else if n < 0 {\n"
             "        return target_(vec4f(0.0, 0.0, 0.0, 1.0));\n"
             "    } else {\n"
             "        acc = 0.5;\n"
             "    }\n"
             "    return target_(vec4f(acc, acc, acc, 1.0));\n"
             "}\n");
    CHECK(function_text(m, e, sgl::emit::target::msl)
          == "fragment target shade(frag p [[stage_in]])\n"
             "{\n"
             "    float acc = 0.0;\n"
             "    for (int i = 0; i < 4; ++i)\n"
             "    {\n"
             "        if (i == 2)\n"
             "        {\n"
             "            continue;\n"
             "        }\n"
             "        acc = acc + p.a;\n"
             "    }\n"
             "    int n = -1;\n"
             "    while (n < 3)\n"
             "    {\n"
             "        n = n + 1;\n"
             "    }\n"
             "    while (true)\n"
             "    {\n"
             "        if ((10.0 < acc && !(n == 7)) || p.a < 0.0)\n"
             "        {\n"
             "            break;\n"
             "        }\n"
             "        acc = acc * (3.0 - p.a);\n"
             "    }\n"
             "    bool found;\n"
             "    found = false;\n"
             "    do\n"
             "    {\n"
             "        if (p.a < 0.5)\n"
             "        {\n"
             "            found = true;\n"
             "            break;\n"
             "        }\n"
             "        acc = acc - 1.0;\n"
             "    } while (false);\n"
             "    if (found)\n"
             "    {\n"
             "        target result;\n"
             "        result.color = float4(acc, acc, acc, 1.0);\n"
             "        return result;\n"
             "    }\n"
             "    else if (n < 0)\n"
             "    {\n"
             "        target result_1;\n"
             "        result_1.color = float4(0.0, 0.0, 0.0, 1.0);\n"
             "        return result_1;\n"
             "    }\n"
             "    else\n"
             "    {\n"
             "        acc = 0.5;\n"
             "    }\n"
             "    target result_2;\n"
             "    result_2.color = float4(acc, acc, acc, 1.0);\n"
             "    return result_2;\n"
             "}\n");
}

TEST("sgl emit - a structured tree is legalized and then written: a value block that a loop leaves early")
{
    auto const checked = flat_test_module();
    auto const& m = checked.module;
    auto const structured = structured_search(m);

    auto const refused = sgl::emit::emit_entry_point(m, structured, sgl::emit::target::wgsl);
    CHECK(sgl::emit::dump_errors(refused)
          == "not-core the block expression $search, since a core expression holds no statement\n");
    CHECK(refused.text.empty());

    auto const core = legalize(m, structured);
    REQUIRE(is_core(core));
    CHECK(dump(interpret(m, core, test_inputs(m))) == dump(interpret(m, structured, test_inputs(m))));
    CHECK(function_text(m, core, sgl::emit::target::wgsl)
          == "@fragment\n"
             "fn shade(p: frag) -> target_ {\n"
             "    var weight: f32 = 1.0;\n"
             "    var search_result: f32;\n"
             "    var search_left: bool = false;\n"
             "    loop {\n"
             "        for (var i: i32 = 0; i < 8; i++) {\n"
             "            weight = weight * p.a;\n"
             "            if weight < 0.125 {\n"
             "                search_result = weight;\n"
             "                search_left = true;\n"
             "                break;\n"
             "            }\n"
             "        }\n"
             "        if search_left {\n"
             "            break;\n"
             "        }\n"
             "        search_result = 0.0;\n"
             "        break;\n"
             "    }\n"
             "    let shade_1: f32 = search_result;\n"
             "    return target_(vec4f(shade_1, shade_1, shade_1, 1.0));\n"
             "}\n");
    CHECK(function_text(m, core, sgl::emit::target::hlsl_dx12)
          == "target shade(frag p)\n"
             "{\n"
             "    float weight = 1.0;\n"
             "    float search_result;\n"
             "    bool search_left = false;\n"
             "    do\n"
             "    {\n"
             "        for (int i = 0; i < 8; ++i)\n"
             "        {\n"
             "            weight = weight * p.a;\n"
             "            if (weight < 0.125)\n"
             "            {\n"
             "                search_result = weight;\n"
             "                search_left = true;\n"
             "                break;\n"
             "            }\n"
             "        }\n"
             "        if (search_left)\n"
             "        {\n"
             "            break;\n"
             "        }\n"
             "        search_result = 0.0;\n"
             "    } while (false);\n"
             "    const float shade_1 = search_result;\n"
             "    target result;\n"
             "    result.color = float4(shade_1, shade_1, shade_1, 1.0);\n"
             "    return result;\n"
             "}\n");
}

TEST("sgl emit - a print is unsupported in every target, and the error does not depend on the target")
{
    auto const checked = flat_test_module();
    auto b = pixel_function(checked.module);
    b.set_body(
        {b.print(b.literal(1.0)),
         b.return_(b.construct(b.e.result, {b.construct(b.type_named("float4"), {b.literal(0.0), b.literal(0.0),
                                                                                 b.literal(0.0), b.literal(1.0)})}))});
    for (auto const t : sgl::emit::all_targets())
        CHECK(sgl::emit::dump_errors(sgl::emit::emit_entry_point(checked.module, b.e, t))
              == "unsupported a print, which no target writes yet\n");
}

TEST("sgl emit - an int that does not fit behind a minus is written as an expression")
{
    auto const checked = flat_test_module();
    auto b = pixel_function(checked.module);
    auto const lowest = b.let("lowest", b.int_literal(-2147483647 - 1));
    auto const negative = b.let("negative", b.call("subtract_int", {b.int_literal(1), b.int_literal(-2)}));
    b.set_body(
        {lowest.stmt, negative.stmt,
         b.return_(b.construct(b.e.result, {b.construct(b.type_named("float4"), {b.literal(0.0), b.literal(0.0),
                                                                                 b.literal(0.0), b.literal(1.0)})}))});
    auto const text = function_text(checked.module, b.e, sgl::emit::target::wgsl);
    CHECK(text.contains("let lowest: i32 = (-2147483647 - 1);"));
    CHECK(text.contains("let negative: i32 = 1 - -2;"));
}
