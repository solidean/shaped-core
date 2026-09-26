#include "../check/source-flat-test-support.hh"

#include <shaped-graphics-language/emit/emit.hh>

using namespace sgl_test;
using namespace sgl::check;

namespace
{
constexpr auto noisy = cc::string_view("fun noisy(x: float) -> float:\n"
                                       "    print x\n"
                                       "    return x * 2.0\n");

/// Guard clauses, and a second helper called from the first.
constexpr auto guard_clauses = cc::string_view("fun lifted(x: float) -> float => x + 0.125\n"
                                               "fun grade(x: float) -> float:\n"
                                               "    if x < 0.25 => return 0.0\n"
                                               "    if x < 0.5 => return lifted x\n"
                                               "    return 1.0\n");
constexpr auto guard_clauses_body = cc::string_view("let x = grade(p.a) + grade(p.b)\n");

/// An early return from inside a loop, through an inlined helper.
constexpr auto early_return = cc::string_view("fun falloff(d: float, steps: int) -> float:\n"
                                              "    if d <= 0.0 => return 0.0\n"
                                              "    let mut w = 1.0\n"
                                              "    for i in 0 ..< steps:\n"
                                              "        w *= d\n"
                                              "        if w < 0.125 => return 0.0\n"
                                              "    return w\n");
constexpr auto early_return_body = cc::string_view("let x = falloff(p.a + p.b, 3)\n");

/// A `continue` inside a helper that is inlined into a loop of its caller.
constexpr auto continue_in_helper = cc::string_view("fun skipping(n: int, skipped: int) -> int:\n"
                                                    "    let mut total = 0\n"
                                                    "    for i in 0 ..< n:\n"
                                                    "        if i == skipped => continue\n"
                                                    "        total += i\n"
                                                    "        print total\n"
                                                    "    return total\n");
constexpr auto continue_in_helper_body = cc::string_view("let mut x = p.a\n"
                                                         "for k in 0 ..< 4:\n"
                                                         "    if k == 1 => continue\n"
                                                         "    if skipping(k + 2, k) > 5 => x += 1.0\n");

/// Helper calls nested in one expression, whose prints say in which order they ran.
constexpr auto nested_calls_body = cc::string_view("let x = noisy(p.a) + noisy(noisy(p.b)) * noisy(0.5)\n");

/// `and` and `or` with a call on the right, which runs only when the left side has not decided.
constexpr auto short_circuit_body = cc::string_view("let mut x = 0.0\n"
                                                    "if p.a < 0.5 and noisy(p.b) > 1.0 => x += 1.0\n"
                                                    "if p.a < 0.5 or noisy(p.a) > 1.0 => x += 2.0\n"
                                                    "if 0.25 <= noisy(p.a) < noisy(p.b) => x += 4.0\n");

/// A `while` whose condition holds a call, which runs before every iteration.
constexpr auto while_call_body = cc::string_view("let mut x = p.a\n"
                                                 "let mut turns = 0\n"
                                                 "while turns < 6 and noisy(x) < 4.0:\n"
                                                 "    turns += 1\n"
                                                 "    x = x * 2.0 + p.b\n"
                                                 "    if x > 3.0 => continue\n"
                                                 "    x += 0.125\n");

/// `break value`, with a helper in the loop.
constexpr auto break_value_body = cc::string_view("let mut w = p.a\n"
                                                  "let x = loop:\n"
                                                  "    w = w * 2.0 + 0.125\n"
                                                  "    if grade(w) > 0.5 => break w\n"
                                                  "    if w > 100.0 or w < -100.0 => break 0.0\n");

/// Calls whose value is dropped, and a helper whose return type is inferred from its arrow body.
constexpr auto dropped_values = cc::string_view("fun noisy(x: float) -> float:\n"
                                                "    print x\n"
                                                "    return x * 2.0\n"
                                                "fun twice(v: float) => noisy(v) + noisy(v * 0.5)\n"
                                                "fun careful(v: float) -> float:\n"
                                                "    if v < 0.25 => return 0.0\n"
                                                "    noisy v\n"
                                                "    return v\n");
constexpr auto dropped_values_body = cc::string_view("let mut x = p.a\n"
                                                     "noisy x\n"
                                                     "if p.a < p.b => twice(p.b)\n"
                                                     "careful(p.a)\n"
                                                     "saturate x\n"
                                                     "x += twice(careful p.b)\n");

/// The same without a print, so every target writes it.
constexpr auto quiet_drops = cc::string_view("fun half(v: float) => v * 0.5\n"
                                             "fun graded(v: float) -> float:\n"
                                             "    if v < 0.25 => return 0.0\n"
                                             "    return half v\n");
/// A `case` in each of its shapes: a switch over an enum, a chain-free `int` switch, and an arm that leaves.
constexpr auto cases = cc::string_view("enum mode:\n"
                                       "    low\n"
                                       "    mid\n"
                                       "    high\n"
                                       "\n"
                                       "fun mode_of(v: float) -> mode:\n"
                                       "    if v < 0.25 => return mode.low\n"
                                       "    if v < 0.75 => return mode.mid\n"
                                       "    return mode.high\n"
                                       "\n"
                                       "fun weight(m: mode) -> float:\n"
                                       "    return case m:\n"
                                       "        .low => 0.25\n"
                                       "        .mid => 0.5\n"
                                       "        .high => 1.0\n"
                                       "\n"
                                       "fun stepped(n: int) -> float:\n"
                                       "    return case n:\n"
                                       "        0 => 0.0\n"
                                       "        1 or 2 => 0.5\n"
                                       "        _ => 1.0\n"
                                       "\n"
                                       "fun guarded(m: mode, base: float) -> float:\n"
                                       "    let w = case m:\n"
                                       "        .low => return 0.0\n"
                                       "        _ => 1.0\n"
                                       "    return base * w\n");
constexpr auto cases_body = cc::string_view("let m = mode_of p.a\n"
                                            "let w = weight m\n"
                                            "let y = stepped 1\n"
                                            "let z = guarded(m, p.b)\n"
                                            "let x = w + y + z\n");

constexpr auto quiet_drops_body = cc::string_view("let x = half p.a\n"
                                                  "graded p.b\n"
                                                  "saturate(x + graded(p.a))\n");

/// void as a value: a local, a parameter, a field, a result and an operand of `==`, each written nowhere (LEGAL-52).
constexpr auto void_values = cc::string_view("struct tagged:\n"
                                             "    tag: void\n"
                                             "    v: float\n"
                                             "fun note(x: float):\n"
                                             "    print x\n"
                                             "fun unit(x: float) -> void => note x\n"
                                             "fun keep(t: tagged, extra: void) -> float => t.v\n");
constexpr auto quiet_void_values = cc::string_view("struct tagged:\n"
                                                   "    tag: void\n"
                                                   "    v: float\n"
                                                   "fun note(x: float):\n"
                                                   "    let doubled = x * 2.0\n"
                                                   "fun unit(x: float) -> void => note x\n"
                                                   "fun keep(t: tagged, extra: void) -> float => t.v\n");
constexpr auto void_values_body = cc::string_view("let u = unit p.a\n"
                                                  "let w: void = u\n"
                                                  "let mut y = p.b\n"
                                                  "if w == void => y += 1.0\n"
                                                  "let t = tagged(unit(p.b), p.a + y)\n"
                                                  "let x = keep(t, t.tag)\n");

/// `bool` is a builtin enum: a `case` over one names its cases, and `bool.false` is a value like `mode.low`.
constexpr auto bool_cases = cc::string_view("fun pick(b: bool) -> float:\n"
                                            "    return case b:\n"
                                            "        .true => 1.0\n"
                                            "        .false => 2.0\n");
constexpr auto bool_cases_body
    = cc::string_view("let x = pick(p.a < 0.5) + pick(bool.false) + pick(bool.true == (p.b > 0.5))\n");

/// A const is its value where it is named, `true` and `false` included.
constexpr auto consts = cc::string_view("const scale = 2.0\n"
                                        "const limit = 3\n"
                                        "const also_limit = limit\n");
constexpr auto consts_body = cc::string_view("let mut x = p.a * scale\n"
                                             "for i in 0 ..< also_limit:\n"
                                             "    if (i < limit) == true and not false => x += 1.0\n");

struct program
{
    cc::string_view name;
    cc::string_view helpers;
    cc::string_view body;
};

constexpr program programs[] = {
    {.name = "guard clauses", .helpers = guard_clauses, .body = guard_clauses_body},
    {.name = "early return from a loop", .helpers = early_return, .body = early_return_body},
    {.name = "continue in a helper", .helpers = continue_in_helper, .body = continue_in_helper_body},
    {.name = "nested calls", .helpers = noisy, .body = nested_calls_body},
    {.name = "short circuit", .helpers = noisy, .body = short_circuit_body},
    {.name = "while with a call", .helpers = noisy, .body = while_call_body},
    {.name = "break value", .helpers = guard_clauses, .body = break_value_body},
    {.name = "dropped values", .helpers = dropped_values, .body = dropped_values_body},
    {.name = "quiet drops", .helpers = quiet_drops, .body = quiet_drops_body},
    {.name = "cases", .helpers = cases, .body = cases_body},
    {.name = "void values", .helpers = void_values, .body = void_values_body},
    {.name = "quiet void values", .helpers = quiet_void_values, .body = void_values_body},
    {.name = "bool cases", .helpers = bool_cases, .body = bool_cases_body},
    {.name = "consts", .helpers = consts, .body = consts_body},
};

run_inputs inputs_of(checked_module const& m, f32 a, f32 b)
{
    auto inputs = test_inputs(m);
    inputs.parameter.leaves[0] = scalar::of(a);
    inputs.parameter.leaves[1] = scalar::of(b);
    return inputs;
}

/// Everything after the declarations: the function alone.
cc::string function_text(cc::string_view helpers, cc::string_view body, sgl::emit::target t)
{
    auto const checked = check_program(grey_source(helpers, body));
    auto const emitted = sgl::emit::emit(checked.module, 0, t);
    CHECK(sgl::emit::dump_errors(emitted) == "");
    auto at
        = emitted.text.find(t == sgl::emit::target::wgsl ? cc::string_view("@fragment") : cc::string_view("main_ps("));
    // the C-like targets put the result type in front of the name, on the same line
    while (at > 0 && emitted.text[at - 1] != '\n')
        --at;
    return at < 0 ? emitted.text
                  : cc::string(cc::string_view(emitted.text).subview({.start = at, .end = emitted.text.size()}));
}
} // namespace

TEST("sgl source - both forms of a program behave the same, on every input")
{
    for (auto const& p : programs)
    {
        auto const checked = check_program(grey_source(p.helpers, p.body));
        REQUIRE(reports_of(checked) == "");
        REQUIRE(checked.module.entry_points.size() == 1);
        auto const& m = checked.module;
        auto const& structured = m.entry_points[0];
        auto const core = legalize(m, structured);
        CHECK(!find_core_violation(core).has_value());

        for (auto const a : {-0.5f, 0.0f, 0.125f, 0.25f, 0.375f, 0.5f, 0.75f, 1.5f})
            for (auto const b : {-1.0f, 0.125f, 0.5f, 0.625f, 2.0f})
            {
                auto const before = interpret(m, structured, inputs_of(m, a, b));
                auto const after = interpret(m, core, inputs_of(m, a, b));
                CHECK(before.status == run_status::ok);
                // the name in front says which program a red line is about
                CHECK(cc::format("{}: {}", p.name, dump(before)) == cc::format("{}: {}", p.name, dump(after)));
            }
    }
}

TEST("sgl source - the prints of nested calls run left to right, inner before outer")
{
    auto const checked = check_program(grey_source(noisy, nested_calls_body));
    REQUIRE(checked.module.entry_points.size() == 1);
    auto const& m = checked.module;
    // noisy(a), then noisy(b), then noisy of that, then noisy(0.5); a = 0.25 and b = 0.75
    CHECK(dump(interpret(m, m.entry_points[0], test_inputs(m)))
          == "ok 3.5 3.5 3.5 1 | print 0.25 | print 0.75 | print 1.5 | print 0.5");
    CHECK(dump(interpret(m, legalize(m, m.entry_points[0]), test_inputs(m)))
          == "ok 3.5 3.5 3.5 1 | print 0.25 | print 0.75 | print 1.5 | print 0.5");
}

TEST("sgl source - the right side of and / or runs only when the left side has not decided")
{
    auto const checked = check_program(grey_source(noisy, short_circuit_body));
    REQUIRE(checked.module.entry_points.size() == 1);
    auto const& m = checked.module;
    // a = 0.25: `and` runs its call, `or` does not; the chain runs its middle once and then its end
    CHECK(dump(interpret(m, m.entry_points[0], test_inputs(m))) == "ok 7 7 7 1 | print 0.75 | print 0.25 | print 0.75");
    // a = 0.75: `and` is decided, `or` is not, and the chain runs both again
    CHECK(dump(interpret(m, legalize(m, m.entry_points[0]), inputs_of(m, 0.75f, 0.25f)))
          == "ok 2 2 2 1 | print 0.75 | print 0.75 | print 0.25");
}

TEST("sgl source - guard clauses: the structured form, the core form, and the text")
{
    CHECK(structured_dump(guard_clauses, guard_clauses_body)
          == "  (let x_2 : float = (call add (block $grade\n"
             "      (let x : float = (member (local p : frag) a : float))\n"
             "      (if (call less (local x : float) (lit 0.25 : float) : bool)\n"
             "        (then\n"
             "          (leave $grade (lit 0.0 : float))))\n"
             "      (if (call less (local x : float) (lit 0.5 : float) : bool)\n"
             "        (then\n"
             "          (leave $grade (block $lifted\n"
             "              (leave $lifted (call add (local x : float) (lit 0.125 : float) : float)) : float))))\n"
             "      (leave $grade (lit 1.0 : float)) : float) (block $grade_1\n"
             "      (let x_1 : float = (member (local p : frag) b : float))\n"
             "      (if (call less (local x_1 : float) (lit 0.25 : float) : bool)\n"
             "        (then\n"
             "          (leave $grade_1 (lit 0.0 : float))))\n"
             "      (if (call less (local x_1 : float) (lit 0.5 : float) : bool)\n"
             "        (then\n"
             "          (leave $grade_1 (block $lifted_1\n"
             "              (leave $lifted_1 (call add (local x_1 : float) (lit 0.125 : float) : float)) : float))))\n"
             "      (leave $grade_1 (lit 1.0 : float)) : float) : float))\n"
             "  (return (construct\n"
             "    (construct (local x_2 : float) (local x_2 : float) (local x_2 : float) (lit 1.0 : float) : float4) : "
             "target))\n");
    CHECK(core_dump(guard_clauses, guard_clauses_body)
          == "  (var:temporary grade_result : float)\n"
             "  (let x : float = (member (local p : frag) a : float))\n"
             "  (if (call less (local x : float) (lit 0.25 : float) : bool)\n"
             "    (then\n"
             "      (assign (local grade_result : float) = (lit 0.0 : float)))\n"
             "    (else\n"
             "      (if (call less (local x : float) (lit 0.5 : float) : bool)\n"
             "        (then\n"
             "          (assign (local grade_result : float) = (call add (local x : float) (lit 0.125 : float) : "
             "float)))\n"
             "        (else\n"
             "          (assign (local grade_result : float) = (lit 1.0 : float))))))\n"
             "  (var:temporary grade_1_result : float)\n"
             "  (let x_1 : float = (member (local p : frag) b : float))\n"
             "  (if (call less (local x_1 : float) (lit 0.25 : float) : bool)\n"
             "    (then\n"
             "      (assign (local grade_1_result : float) = (lit 0.0 : float)))\n"
             "    (else\n"
             "      (if (call less (local x_1 : float) (lit 0.5 : float) : bool)\n"
             "        (then\n"
             "          (assign (local grade_1_result : float) = (call add (local x_1 : float) (lit 0.125 : float) : "
             "float)))\n"
             "        (else\n"
             "          (assign (local grade_1_result : float) = (lit 1.0 : float))))))\n"
             "  (let x_2 : float = (call add (local grade_result : float) (local grade_1_result : float) : float))\n"
             "  (return (construct\n"
             "    (construct (local x_2 : float) (local x_2 : float) (local x_2 : float) (lit 1.0 : float) : float4) : "
             "target))\n");

    CHECK(function_text(guard_clauses, guard_clauses_body, sgl::emit::target::wgsl)
          == "@fragment\n"
             "fn main_ps(p: frag) -> target_ {\n"
             "    var grade_result: f32;\n"
             "    let x: f32 = p.a;\n"
             "    if x < 0.25 {\n"
             "        grade_result = 0.0;\n"
             "    } else if x < 0.5 {\n"
             "        grade_result = x + 0.125;\n"
             "    } else {\n"
             "        grade_result = 1.0;\n"
             "    }\n"
             "    var grade_1_result: f32;\n"
             "    let x_1: f32 = p.b;\n"
             "    if x_1 < 0.25 {\n"
             "        grade_1_result = 0.0;\n"
             "    } else if x_1 < 0.5 {\n"
             "        grade_1_result = x_1 + 0.125;\n"
             "    } else {\n"
             "        grade_1_result = 1.0;\n"
             "    }\n"
             "    let x_2: f32 = grade_result + grade_1_result;\n"
             "    return target_(vec4f(x_2, x_2, x_2, 1.0));\n"
             "}\n");
    CHECK(function_text(guard_clauses, guard_clauses_body, sgl::emit::target::hlsl_dx12)
          == "target main_ps(frag p)\n"
             "{\n"
             "    float grade_result;\n"
             "    const float x = p.a;\n"
             "    if (x < 0.25)\n"
             "    {\n"
             "        grade_result = 0.0;\n"
             "    }\n"
             "    else if (x < 0.5)\n"
             "    {\n"
             "        grade_result = x + 0.125;\n"
             "    }\n"
             "    else\n"
             "    {\n"
             "        grade_result = 1.0;\n"
             "    }\n"
             "    float grade_1_result;\n"
             "    const float x_1 = p.b;\n"
             "    if (x_1 < 0.25)\n"
             "    {\n"
             "        grade_1_result = 0.0;\n"
             "    }\n"
             "    else if (x_1 < 0.5)\n"
             "    {\n"
             "        grade_1_result = x_1 + 0.125;\n"
             "    }\n"
             "    else\n"
             "    {\n"
             "        grade_1_result = 1.0;\n"
             "    }\n"
             "    const float x_2 = grade_result + grade_1_result;\n"
             "    target result;\n"
             "    result.color = float4(x_2, x_2, x_2, 1.0);\n"
             "    return result;\n"
             "}\n");
}

TEST("sgl source - an early return from a loop in a helper: the structured form, the core form, and the text")
{
    CHECK(structured_dump(early_return, early_return_body)
          == "  (let x : float = (block $falloff\n"
             "      (let d : float = (call add (member (local p : frag) a : float) (member (local p : frag) b : float) "
             ": float))\n"
             "      (if (call less_equal (local d : float) (lit 0.0 : float) : bool)\n"
             "        (then\n"
             "          (leave $falloff (lit 0.0 : float))))\n"
             "      (var w : float = (lit 1.0 : float))\n"
             "      (for $for i in (lit 0 : int) ..< (lit 3 : int)\n"
             "        (assign (local w : float) = (call multiply (local w : float) (local d : float) : float))\n"
             "        (if (call less (local w : float) (lit 0.125 : float) : bool)\n"
             "          (then\n"
             "            (leave $falloff (lit 0.0 : float)))))\n"
             "      (leave $falloff (local w : float)) : float))\n"
             "  (return (construct\n"
             "    (construct (local x : float) (local x : float) (local x : float) (lit 1.0 : float) : float4) : "
             "target))\n");
    CHECK(core_dump(early_return, early_return_body)
          == "  (var:temporary falloff_result : float)\n"
             "  (var:temporary falloff_left : bool = (lit false : bool))\n"
             "  (once\n"
             "    (let d : float = (call add (member (local p : frag) a : float) (member (local p : frag) b : float) : "
             "float))\n"
             "    (if (call less_equal (local d : float) (lit 0.0 : float) : bool)\n"
             "      (then\n"
             "        (assign (local falloff_result : float) = (lit 0.0 : float))\n"
             "        (break)))\n"
             "    (var w : float = (lit 1.0 : float))\n"
             "    (for $for i in (lit 0 : int) ..< (lit 3 : int)\n"
             "      (assign (local w : float) = (call multiply (local w : float) (local d : float) : float))\n"
             "      (if (call less (local w : float) (lit 0.125 : float) : bool)\n"
             "        (then\n"
             "          (assign (local falloff_result : float) = (lit 0.0 : float))\n"
             "          (assign (local falloff_left : bool) = (lit true : bool))\n"
             "          (break))))\n"
             "    (if (local falloff_left : bool)\n"
             "      (then\n"
             "        (break)))\n"
             "    (assign (local falloff_result : float) = (local w : float)))\n"
             "  (let x : float = (local falloff_result : float))\n"
             "  (return (construct\n"
             "    (construct (local x : float) (local x : float) (local x : float) (lit 1.0 : float) : float4) : "
             "target))\n");

    CHECK(function_text(early_return, early_return_body, sgl::emit::target::wgsl)
          == "@fragment\n"
             "fn main_ps(p: frag) -> target_ {\n"
             "    var falloff_result: f32;\n"
             "    var falloff_left: bool = false;\n"
             "    loop {\n"
             "        let d: f32 = p.a + p.b;\n"
             "        if d <= 0.0 {\n"
             "            falloff_result = 0.0;\n"
             "            break;\n"
             "        }\n"
             "        var w: f32 = 1.0;\n"
             "        for (var i: i32 = 0; i < 3; i++) {\n"
             "            w = w * d;\n"
             "            if w < 0.125 {\n"
             "                falloff_result = 0.0;\n"
             "                falloff_left = true;\n"
             "                break;\n"
             "            }\n"
             "        }\n"
             "        if falloff_left {\n"
             "            break;\n"
             "        }\n"
             "        falloff_result = w;\n"
             "        break;\n"
             "    }\n"
             "    let x: f32 = falloff_result;\n"
             "    return target_(vec4f(x, x, x, 1.0));\n"
             "}\n");
    CHECK(function_text(early_return, early_return_body, sgl::emit::target::hlsl_dx12)
          == "target main_ps(frag p)\n"
             "{\n"
             "    float falloff_result;\n"
             "    bool falloff_left = false;\n"
             "    do\n"
             "    {\n"
             "        const float d = p.a + p.b;\n"
             "        if (d <= 0.0)\n"
             "        {\n"
             "            falloff_result = 0.0;\n"
             "            break;\n"
             "        }\n"
             "        float w = 1.0;\n"
             "        for (int i = 0; i < 3; ++i)\n"
             "        {\n"
             "            w = w * d;\n"
             "            if (w < 0.125)\n"
             "            {\n"
             "                falloff_result = 0.0;\n"
             "                falloff_left = true;\n"
             "                break;\n"
             "            }\n"
             "        }\n"
             "        if (falloff_left)\n"
             "        {\n"
             "            break;\n"
             "        }\n"
             "        falloff_result = w;\n"
             "    } while (false);\n"
             "    const float x = falloff_result;\n"
             "    target result;\n"
             "    result.color = float4(x, x, x, 1.0);\n"
             "    return result;\n"
             "}\n");
}

TEST("sgl source - break value through a helper: the text")
{
    CHECK(function_text(guard_clauses, break_value_body, sgl::emit::target::wgsl)
          == "@fragment\n"
             "fn main_ps(p: frag) -> target_ {\n"
             "    var w: f32 = p.a;\n"
             "    var loop_value_result: f32;\n"
             "    loop {\n"
             "        w = w * 2.0 + 0.125;\n"
             "        var grade_result: f32;\n"
             "        let x: f32 = w;\n"
             "        if x < 0.25 {\n"
             "            grade_result = 0.0;\n"
             "        } else if x < 0.5 {\n"
             "            grade_result = x + 0.125;\n"
             "        } else {\n"
             "            grade_result = 1.0;\n"
             "        }\n"
             "        if grade_result > 0.5 {\n"
             "            loop_value_result = w;\n"
             "            break;\n"
             "        }\n"
             "        if w > 100.0 || w < -100.0 {\n"
             "            loop_value_result = 0.0;\n"
             "            break;\n"
             "        }\n"
             "    }\n"
             "    let x_1: f32 = loop_value_result;\n"
             "    return target_(vec4f(x_1, x_1, x_1, 1.0));\n"
             "}\n");
    CHECK(function_text(guard_clauses, break_value_body, sgl::emit::target::hlsl_dx12)
          == "target main_ps(frag p)\n"
             "{\n"
             "    float w = p.a;\n"
             "    float loop_value_result;\n"
             "    while (true)\n"
             "    {\n"
             "        w = w * 2.0 + 0.125;\n"
             "        float grade_result;\n"
             "        const float x = w;\n"
             "        if (x < 0.25)\n"
             "        {\n"
             "            grade_result = 0.0;\n"
             "        }\n"
             "        else if (x < 0.5)\n"
             "        {\n"
             "            grade_result = x + 0.125;\n"
             "        }\n"
             "        else\n"
             "        {\n"
             "            grade_result = 1.0;\n"
             "        }\n"
             "        if (grade_result > 0.5)\n"
             "        {\n"
             "            loop_value_result = w;\n"
             "            break;\n"
             "        }\n"
             "        if (w > 100.0 || w < -100.0)\n"
             "        {\n"
             "            loop_value_result = 0.0;\n"
             "            break;\n"
             "        }\n"
             "    }\n"
             "    const float x_1 = loop_value_result;\n"
             "    target result;\n"
             "    result.color = float4(x_1, x_1, x_1, 1.0);\n"
             "    return result;\n"
             "}\n");
}

TEST("sgl source - a return whose value inlines a call leaves its own block, however deep the nesting")
{
    // Each helper returns a call, so flattening a `return` value pushes a frame while that return's frame is live.
    // A frame held by reference across that push is a dangling read, and it showed only where the vector moved.
    constexpr auto chain = cc::string_view("fun f1(v: float) -> float => v * 0.5\n"
                                           "fun f2(v: float) -> float:\n"
                                           "    return f1 v\n"
                                           "fun f3(v: float) -> float:\n"
                                           "    return f2 v\n"
                                           "fun f4(v: float) -> float:\n"
                                           "    return f3 v\n"
                                           "fun f5(v: float) -> float:\n"
                                           "    return f4 v\n"
                                           "fun f6(v: float) -> float:\n"
                                           "    return f5 v\n"
                                           "fun f7(v: float) -> float:\n"
                                           "    return f6 v\n"
                                           "fun f8(v: float) -> float:\n"
                                           "    return f7 v\n");
    constexpr auto body = cc::string_view("let x = f8 p.a\n");

    // Every inlined `return` is a `leave` of its own block; the entry point owns the only `return` in the tree.
    auto const dump = structured_dump(chain, body);
    CHECK(!dump.contains("(return (block"));
    CHECK(dump.contains("(leave $f1 "));
    CHECK(dump.contains("(leave $f8 "));

    // And eight blocks that all dissolved leave two lines: the bound argument and the one computation.
    auto const text = function_text(chain, body, sgl::emit::target::wgsl);
    CHECK(text.contains("    let v: f32 = p.a;\n    let x: f32 = v * 0.5;\n"));
    CHECK(!text.contains("_result"));
}

TEST("sgl source - a dropped value: the call's statements stay, and a rest that is only a local is gone")
{
    // `graded p.b` leaves nothing behind its statements, and `saturate(...)` stays a statement of its own.
    CHECK(function_text(quiet_drops, quiet_drops_body, sgl::emit::target::wgsl)
          == "@fragment\n"
             "fn main_ps(p: frag) -> target_ {\n"
             "    let v: f32 = p.a;\n"
             "    let x: f32 = v * 0.5;\n"
             "    var graded_result: f32;\n"
             "    let v_1: f32 = p.b;\n"
             "    if v_1 < 0.25 {\n"
             "        graded_result = 0.0;\n"
             "    } else {\n"
             "        graded_result = v_1 * 0.5;\n"
             "    }\n"
             "    var graded_1_result: f32;\n"
             "    let v_2: f32 = p.a;\n"
             "    if v_2 < 0.25 {\n"
             "        graded_1_result = 0.0;\n"
             "    } else {\n"
             "        graded_1_result = v_2 * 0.5;\n"
             "    }\n"
             "    _ = saturate(x + graded_1_result);\n"
             "    return target_(vec4f(x, x, x, 1.0));\n"
             "}\n");
    CHECK(function_text(quiet_drops, quiet_drops_body, sgl::emit::target::hlsl_dx12)
              .contains("    saturate(x + graded_1_result);\n"));
    CHECK(function_text(quiet_drops, quiet_drops_body, sgl::emit::target::msl)
              .contains("    (void)(saturate(x + graded_1_result));\n"));
}

TEST("sgl source - void is written nowhere: no local, no field and no argument holds it")
{
    for (auto const t : sgl::emit::all_targets())
    {
        auto const text = function_text(quiet_void_values, void_values_body, t);
        CHECK(!text.contains(" u"));
        CHECK(!text.contains(" w"));
        CHECK(!text.contains(".tag"));
    }
    CHECK(function_text(quiet_void_values, void_values_body, sgl::emit::target::wgsl)
              .contains("let t: tagged = tagged(p.a + y);\n"));
}

TEST("sgl source - a case of bool is the target's bool, never the int of an enum")
{
    for (auto const t : sgl::emit::all_targets())
    {
        auto const text = function_text(bool_cases, bool_cases_body, t);
        CHECK(text.contains("false"));
        CHECK(!text.contains("bool_false"));
        CHECK(!text.contains("switch"));
    }
}

TEST("sgl source - every program without a print is written for every target")
{
    for (auto const& p : programs)
    {
        auto const checked = check_program(grey_source(p.helpers, p.body));
        auto const has_print = p.helpers.contains("print");
        for (auto const t : sgl::emit::all_targets())
        {
            auto const emitted = sgl::emit::emit(checked.module, 0, t);
            CHECK(sgl::emit::dump_errors(emitted)
                  == (has_print ? "unsupported a print, which no target writes yet\n" : ""));
        }
    }
}
