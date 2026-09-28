#include "check-test-support.hh"

using namespace sgl_test;

// Fixed-size arrays, CHK-285 to CHK-291: what the checker refuses, since the corpus shows what it takes.

namespace
{
/// `body` as the statements of a function, with `head` above it at file scope.
cc::string in_function(cc::string_view body, cc::string_view head = "")
{
    return cc::format("{}fun f() -> int:\n{}    return 0\n", head, body);
}
} // namespace

TEST("sgl check - an array's lengths are one group of int constants of at least 1")
{
    CHECK(reports_for(in_function("    let xs: float[4] = [1.0, 2.0, 3.0, 4.0]\n")) == "");
    CHECK(reports_for(in_function("    let xs: float[n] = float[n].filled(1.0)\n", "const n = 3\n\n")) == "");

    auto const twice = reports_for(in_function("    let xs = float[3][5].filled(1.0)\n"));
    CHECK(twice.contains("wrong-kind-of-name"));
    CHECK(twice.contains("one group of dimensions, outermost first: `float[3, 5]`"));

    constexpr auto length = "an array's length is an int constant of at least 1";
    CHECK(reports_for(in_function("    let xs = float[0].filled(1.0)\n")).contains(length));
    CHECK(reports_for(in_function("    let n = 3\n    let xs = float[n].filled(1.0)\n")).contains(length));
    CHECK(reports_for(in_function("    let xs = float[2.0].filled(1.0)\n")).contains(length));

    // CHK-286: `T[]` has the length the host binds, which no value has
    CHECK(reports_for(in_function("    let xs: float[] = [1.0]\n")).contains("only a binding member may be"));
}

TEST("sgl check - an array literal is as long as its array, and its elements all one type")
{
    CHECK(reports_for(in_function("    let xs: int[3] = [1, 2]\n"))
              .contains("int[3] holds 3 elements, and this literal has 2"));
    CHECK(reports_for(in_function("    let xs = [1, 2.0]\n")).contains("int does not hold 2.0 exactly"));
    CHECK(reports_for(in_function("    let a = 2.0\n    let xs = [1, a]\n"))
              .contains("an element of this array is int, got float"));
    CHECK(reports_for(in_function("    let xs = []\n")).contains("an array holds at least one element"));
    CHECK(reports_for(in_function("    let xs = [a = 1]\n")).contains("an array literal holds values alone"));
    // a number literal converts to the element it meets (CHK-253)
    CHECK(reports_for(in_function("    let xs: float[2] = [1, 2]\n")) == "");
}

TEST("sgl check - an array is read by as many int indices as it has dimensions, and has a length and nothing else")
{
    constexpr auto grid = "    let g: int[2, 3] = [[1, 2, 3], [4, 5, 6]]\n";
    CHECK(reports_for(in_function(cc::format("{}    let a = g[1, 2] + g[1][2] + g.length + g[0].length\n", grid))) == "");
    CHECK(reports_for(in_function(cc::format("{}    let a = g[1, 2, 0]\n", grid)))
              .contains("int[2, 3] takes one to 2 indices"));
    CHECK(reports_for(in_function(cc::format("{}    let a = g[1.0]\n", grid)))
              .contains("an array is indexed by an int, and this is a float"));
    CHECK(reports_for(in_function(cc::format("{}    let a = g.count\n", grid)))
              .contains("int[2, 3] has no member count; an array's one member is its length"));
    CHECK(reports_for(in_function("    let a = float[3].fill(0.0)\n"))
              .contains("float[3] has no function fill; an array type's one function is `filled`"));
    CHECK(reports_for(in_function("    let a = float[3].filled(1)\n")) == "");
    CHECK(reports_for(in_function("    let a = float[3].filled(true)\n"))
              .contains("the value it is filled with is float, got bool"));
}

TEST("sgl check - a constant index names an element of its array")
{
    constexpr auto grid = "    let g: int[2, 3] = [[1, 2, 3], [4, 5, 6]]\n";
    CHECK(reports_for(in_function(cc::format("{}    let a = g[1, 2] + g[k, 0]\n", grid), "const k = 1\n\n")) == "");

    // CHK-309: one past the end is refused by DXC and by WGSL, so the checker refuses it first
    auto const literal = reports_for(in_function(cc::format("{}    let a = g[1, 3]\n", grid)));
    CHECK(literal.contains("invalid-constant-argument"));
    CHECK(literal.contains("3 is no index into int[3], whose elements are 0 ..< 3"));
    CHECK(reports_for(in_function(cc::format("{}    let a = g[k]\n", grid), "const k = 2\n\n"))
              .contains("2 is no index into int[2, 3], whose elements are 0 ..< 2"));
    CHECK(reports_for(in_function(cc::format("{}    let a = g[k]\n", grid), "const k = -1\n\n"))
              .contains("-1 is no index into int[2, 3]"));

    // a value computed at run time is the target's to bound
    CHECK(reports_for(in_function(cc::format("{}    let k = 2\n    let a = g[k]\n", grid))) == "");
}

TEST("sgl check - an element of a mutable array is assigned, and its length never is")
{
    CHECK(reports_for(in_function("    let mut xs = [1, 2]\n    xs[0] = 3\n    xs[1] += 1\n")) == "");
    CHECK(reports_for(in_function("    let xs = [1, 2]\n    xs[0] = 3\n")).contains("not-assignable"));
    CHECK(reports_for(in_function("    let mut xs = [1, 2]\n    xs.length = 3\n"))
              .contains("an array's length is part of its type, and never assigned"));
}

TEST("sgl check - an array in a constant block waits for a layout rule, and a binding array for its feature")
{
    CHECK(reports_for("binding work:\n    weights: float[4]\n")
              .contains("unsupported-yet user:[float[4]] an array in a constant block, whose layout no rule settles "
                        "yet"));
    // an array of arrays of values is values still, and no binding array
    CHECK(reports_for("binding work:\n    grid: float[2, 3]\n")
              .contains("unsupported-yet user:[float[2, 3]] an array in a constant block, whose layout no rule "
                        "settles yet"));
    // a binding array is a feature a device grants (CHK-299)
    CHECK(reports_for("binding work:\n    maps: texture_2d[float4][4]\n").contains("needs binding_arrays"));
}

TEST("sgl check - an array anywhere in GPU memory or across a stage edge is refused at its line")
{
    // CHK-291: the checker says it where the author wrote it, rather than the emitter with no line at all
    constexpr auto poly = "struct poly:\n    count: int\n    corners: float2[3]\n\n";
    CHECK(reports_for(cc::format("{}binding work:\n    p: poly\n", poly))
              .contains("unsupported-yet user:[poly] an array in a constant block, poly.corners: float2[3], whose "
                        "layout no rule places yet"));
    CHECK(reports_for(cc::format("{}binding work:\n    ps: buffer[poly]\n", poly))
              .contains("an array in a buffer's element, poly.corners: float2[3], whose layout no rule places yet"));
    CHECK(reports_for("binding work:\n    rows: mut buffer[float[4]]\n")
              .contains("an array in a buffer's element, float[4], whose layout no rule places yet"));

    // a stage link, and the struct a patch or a stream carries, cross an edge too
    auto const linked = reports_for("struct varyings:\n"
                                    "    @position position: hpos4\n"
                                    "    weights: float[2]\n"
                                    "\n"
                                    "@vertex fun vs() -> varyings:\n"
                                    "    return {position = hpos4(0.0, 0.0, 0.0, 1.0), weights = [1.0, 2.0]}\n");
    CHECK(linked.contains("an array crossing a stage edge, in varyings.weights: float[2]"));

    // workgroup memory is laid out by each target alone, so an array there stays fine
    CHECK(reports_for("@workgroup binding tile:\n    rows: poly[4]\nstruct poly:\n    corners: float2[3]\n") == "");
}
