#include "check-test-support.hh"

using namespace sgl_test;

TEST("sgl check - a test is checked on its own, wherever it stands")
{
    // CHK-224: at file scope, in a struct, in an enum and in a function body
    auto const checked
        = check_sources(read_prelude(), "fun square(x: float) -> float => x * x\n"
                                        "test square 3.0 == 9.0\n"
                                        "struct light:\n"
                                        "    power: float\n"
                                        "    test light(2.0).power == 2.0\n"
                                        "enum mode:\n"
                                        "    on\n"
                                        "    off\n"
                                        "    test mode.on != mode.off\n"
                                        "fun shade(k: float) -> float:\n"
                                        "    return k\n"
                                        "    // a test after the return is no unreachable code: it never runs there\n"
                                        "    test square(2.0) == 4.0\n");
    CHECK(reports_of(checked) == "");
    REQUIRE(checked.module.tests.size() == 4);
    CHECK(checked.module.tests[0].scope_path == "");
    CHECK(checked.module.tests[1].scope_path == "struct light");
    CHECK(checked.module.tests[2].scope_path == "enum mode");
    CHECK(checked.module.tests[3].scope_path == "fun shade");
    CHECK(checked.module.tests[3].comment == "a test after the return is no unreachable code: it never runs there");
    CHECK(checked.user.text_of(checked.module.tests[0].where) == "test");
}

TEST("sgl check - an entry point that holds a test or an assert still has a flat tree")
{
    auto const checked
        = check_sources(read_prelude(), "struct frag:\n    a: float\n@pixel struct target:\n    color: float4\n"
                                        "@pixel fun main_ps(p: frag) -> target:\n"
                                        "    assert p.a >= 0.0\n"
                                        "    test 1 < 2\n"
                                        "    return target(float4(p.a, p.a, p.a, 1.0))\n");
    CHECK(reports_of(checked) == "");
    CHECK(checked.module.entry_points.size() == 1);
}

TEST("sgl check - a line of type bool is a check, and any other line that computes nothing is no-effect")
{
    // CHK-225
    CHECK(reports_for("test:\n    1 + 2 == 3\n    let x = 10\n    x * x > 50\n") == "");
    CHECK(reports_for("test:\n    1 + 2\n    true\n") == "no-effect user:[1 + 2]\n");
    CHECK(reports_for("fun note(x: float):\n    print x\ntest:\n    note 2.0\n    true\n") == "");
}

TEST("sgl check - a test ends in a check")
{
    // CHK-226: the last code line, through the last branch of an if, the body of a loop and the last arm of a case
    CHECK(reports_for("test:\n    let x = 1\n")
          == "test-must-end-in-check user:[let x = 1] the last line of a test is a check; end in `true // why` where "
             "its "
             "asserts are what it checks\n");
    CHECK(reports_for("test:\n    for i in 0 ..< 3:\n        i < 3\n") == "");
    CHECK(reports_for("test:\n    let b = 1 < 2\n    if b:\n        b\n    else:\n        let y = 2\n")
              .starts_with("test-must-end-in-check user:[let y = 2]"));
    CHECK(reports_for("test:\n    let b = 1 < 2\n    if b:\n        let y = 2\n    else:\n        not b\n") == "");
    // a test that is to fail or to stop at an assert cannot pass vacuously, and an empty one was reported already
    CHECK(reports_for("fun f(x: float) -> float:\n    assert x > 0.0\n    return x\n@expect(.assert) test:\n    f -1.0\n")
          == "");
    CHECK(reports_for("test\n") == ""); // `expected-body` is the AST pass's, and the check pass adds nothing
    // a line that does not check is reported once, as what it is
    CHECK(reports_for("test missing == 1\n") == "unknown-name user:[missing] missing\n");
}

TEST("sgl check - a test reads nothing of the function it stands in")
{
    // CHK-228: a parameter, a local and a binding member of the function are values of its run, and a const is none
    CHECK(reports_for("fun f(k: float) -> float:\n    let x = k\n    test x > 0.0\n    return k\n")
          == "test-captures-runtime-value user:[x] x is a value of f when it runs, and a test runs on its own\n"
             "  note user:[let x = k] x is declared here\n");
    CHECK(reports_for("fun f(k: float) -> float:\n    test k > 0.0\n    return k\n")
          == "test-captures-runtime-value user:[k] k is a value of f when it runs, and a test runs on its own\n"
             "  note user:[k] k is declared here\n");
    // a local of the test itself hides one of the function
    CHECK(reports_for("fun f(k: float) -> float:\n    let x = k\n    test:\n        let x = 2.0\n        x > 1.0\n    "
                      "return k\n")
          == "");
    CHECK(reports_for("const c = 2\nfun f(k: float) -> float:\n    test c == 2\n    return k\n") == "");
    CHECK(reports_for("binding frame:\n    e: float\nfun f(k: float){frame} -> float:\n    test frame.e > 0.0\n    "
                      "return k\n")
          == "test-captures-runtime-value user:[frame] frame is a binding of f, and a test runs on its own\n");
}

TEST("sgl check - a test lists no binding, and a callee that needs one is told how it will get it")
{
    // CHK-228
    CHECK(reports_for("binding frame:\n    e: float\nfun g(){frame} -> float => frame.e\ntest g() > 0.0\n")
          == "binding-not-listed user:[g()] g needs frame, and a test lists no binding\n"
             "  note user:[g()] a `binding frame:` declared in the test gives g its values, once local bindings are "
             "carried\n");
    CHECK(reports_for("binding frame:\n    e: float\ntest frame.e > 0.0\n")
          == "binding-not-listed user:[frame] frame is a binding, and a test lists none\n");
}

TEST("sgl check - assert takes a bool, anywhere")
{
    // CHK-227
    CHECK(reports_for("fun f(k: float) -> float:\n    assert k > 0.0\n    return k\n") == "");
    CHECK(reports_for("fun f(k: float) -> float:\n    assert k\n    return k\n")
          == "type-mismatch user:[k] a condition is a bool, got float\n");
    CHECK(reports_for("fun half(x: float) -> float:\n    assert x >= 0.0\n    return x * 0.5\ntest:\n    half 4.0\n    "
                      "true\n")
          == "");
}

TEST("sgl check - a test is named at its keyword, whatever its attributes spell")
{
    // an @expect pattern holding "test" must not move the test's place, nor where its extent starts (CHK-232)
    auto const checked = check_sources(read_prelude(), "@expect(error = \"test-*\") test missing < 1\n");
    REQUIRE(checked.module.tests.size() == 1);
    auto const& t = checked.module.tests[0];
    CHECK(checked.user.text_of(t.where) == "test");
    CHECK(t.extent.offset == t.where.offset);
    CHECK(checked.user.text_of(t.extent) == "test missing < 1");
}
