#include <clean-core/math/random.hh>
#include <clean-core/string/format.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;
#include <shaped-graphics-language/driver/compile_to_text.hh>
#include <shaped-graphics-language/driver/test_source.hh>
#include <shaped-graphics-language/emit/emit.hh>

// PROTOTYPE (libs/graphics/shaped-graphics-language/docs/spec/incubator/autodiff.md): random programs, each function's two derivatives held to each other and to
// central differences on the interpreter.
// Forward and backward are both exact at a point, so they must agree even beside a kink; a central difference is
// compared only where two step sizes agree on it, which a kink within a step breaks.
// Every expression stays inside its builtins' domains, so a value is finite unless the transform is wrong.

namespace
{
struct program_writer
{
    cc::random& rng;
    cc::string text;

    /// The locals in scope by type, and of those the ones a statement may assign.
    cc::vector<cc::string> scalars;
    cc::vector<cc::string> vectors;
    cc::vector<cc::string> mut_scalars;
    cc::vector<cc::string> mut_vectors;
    cc::vector<cc::string> mut_blobs;
    cc::vector<cc::string> mut_matrices;
    /// The loop indices in scope, which an expression reads as a float.
    cc::vector<cc::string> indices;
    i32 next_local = 0;
    i32 loop_depth = 0;
    /// Whether the function being written holds a value constant, which a central difference does not see.
    bool is_held = false;

    i32 pick(i32 count) { return rng.uniform(0, count - 1); }
    bool chance(i32 percent) { return rng.uniform(0, 99) < percent; }
    /// A float literal, always with its decimals, so it never reads as an int.
    cc::string number()
    {
        auto const hundredths = rng.uniform(-150, 150);
        auto const magnitude = hundredths < 0 ? -hundredths : hundredths;
        return cc::format("{}{}.{}{}", hundredths < 0 ? "-" : "", magnitude / 100, magnitude / 10 % 10, magnitude % 10);
    }
    cc::string name(cc::string_view prefix) { return cc::format("{}{}", prefix, next_local++); }
    /// An int: a loop index in scope, or a literal.
    cc::string index()
    {
        if (!indices.empty() && chance(70))
            return indices[pick(i32(indices.size()))];
        return cc::format("{}", pick(3));
    }

    cc::string scalar(i32 depth)
    {
        if (depth <= 0 || chance(20))
        {
            switch (pick(5))
            {
            case 0:
                return "y";
            case 1:
                return cc::format("x.{}", "xyz"[pick(3)]);
            case 2:
                if (!scalars.empty())
                    return scalars[pick(i32(scalars.size()))];
                return "y";
            case 3:
                if (!indices.empty())
                    return cc::format("float({})", indices[pick(i32(indices.size()))]);
                return number();
            default:
                return number();
            }
        }
        auto const a = [&] { return scalar(depth - 1); };
        auto const v = [&] { return vector(depth - 1); };
        switch (pick(34))
        {
        case 31:
            return cc::format("smooth_sum({}, {})", v(), a());
        case 32:
            return cc::format("select({} < {}, {}, {})", a(), a(), a(), a());
        case 33:
            if (!mut_matrices.empty())
                return cc::format("dot({} * {}, float3(0.5, 1.0, -0.75))", mut_matrices[pick(i32(mut_matrices.size()))],
                                  v());
            return cc::format("ramp({})", a());
        case 23:
            return cc::format("ramp({})", a());
        case 24:
            return cc::format("component({}, {})", v(), index());
        case 25:
        {
            auto const e = v();
            return cc::format("distance(pos3({0}.x, {0}.y, {0}.z) + vec3({1}, {2}, {3}), pos3(0.5, -0.25, 1.0))", e,
                              a(), number(), a());
        }
        case 26:
        {
            auto const e = v();
            return cc::format("dot(pos3({0}.x, {0}.y, {0}.z) - pos3({1}, {2}, {3}), vec3({4}, {5}, {6}))", e, a(),
                              number(), a(), number(), a(), number());
        }
        case 27:
            is_held = true;
            return cc::format("no_diff({})", a());
        case 28:
        {
            auto const e = v();
            return cc::format("dot(float4({0}.x, {0}.y, {0}.z, {1}).wzyx, float4(0.5, -0.25, 0.75, 1.0))", e, a());
        }
        case 29:
            return cc::format("dot(float2({}, {}).yx, float2({}, {}))", a(), a(), a(), number());
        case 30:
        {
            auto const e = v();
            return cc::format("length(pos3({0}.x, {0}.y, {0}.z) - pos3(1.5, 0.5, -1.0))", e);
        }
        case 0:
            return cc::format("({} + {})", a(), a());
        case 1:
            return cc::format("({} - {})", a(), a());
        case 2:
            return cc::format("({} * {})", a(), a());
        case 3:
        {
            auto const e = a();
            return cc::format("({} / ({} * {} + 1.0))", a(), e, e);
        }
        case 4:
            return cc::format("sin({})", a());
        case 5:
            return cc::format("cos({})", a());
        case 6:
            return cc::format("tanh({})", a());
        case 7:
            return cc::format("exp(sin({}))", a());
        case 8:
        {
            auto const e = a();
            return cc::format("sqrt({} * {} + 1.0)", e, e);
        }
        case 9:
        {
            auto const e = a();
            return cc::format("log({} * {} + 1.0)", e, e);
        }
        case 10:
            return cc::format("atan({})", a());
        case 11:
            return cc::format("abs({})", a());
        case 12:
            return cc::format("min({}, {})", a(), a());
        case 13:
            return cc::format("max({}, {})", a(), a());
        case 14:
            return cc::format("clamp({}, -0.5, 0.5)", a());
        case 15:
            return cc::format("mix({}, {}, {})", a(), a(), a());
        case 16:
            return cc::format("pow(abs({}) + 1.0, 1.0 + 0.5 * sin({}))", a(), a());
        case 17:
        {
            auto const e = a();
            return cc::format("atan2({}, {} * {} + 1.0)", a(), e, e);
        }
        case 18:
            return cc::format("dot({}, {})", v(), v());
        case 19:
        {
            auto const e = v();
            return cc::format("length({} * {} + float3(1.0))", e, e);
        }
        case 20:
            return cc::format("{}.{}", v(), "xyz"[pick(3)]);
        case 21:
            return cc::format("asin(0.9 * tanh({}))", a());
        case 22:
            return cc::format("-({})", a());
        default:
        {
            auto const e = v();
            return cc::format("distance({}, float3(2.0, -1.0, 0.5))", e);
        }
        }
    }

    cc::string vector(i32 depth)
    {
        if (depth <= 0 || chance(20))
        {
            switch (pick(4))
            {
            case 0:
                return "x";
            case 1:
                if (!vectors.empty())
                    return vectors[pick(i32(vectors.size()))];
                return "x";
            case 2:
                return cc::format("float3({}, {}, {})", number(), number(), number());
            default:
                return cc::format("x.{}", cc::string_view(swizzles[pick(i32(sizeof(swizzles) / sizeof(swizzles[0])))]));
            }
        }
        auto const a = [&] { return scalar(depth - 1); };
        auto const v = [&] { return vector(depth - 1); };
        switch (pick(25))
        {
        case 23:
            return cc::format("select({} < {}, {}, {})", a(), a(), v(), v());
        case 24:
            if (!mut_matrices.empty())
                return cc::format("({} * {})", mut_matrices[pick(i32(mut_matrices.size()))], v());
            return cc::format("sin({})", v());
        case 15:
            return cc::format("abs({})", v());
        case 16:
            return cc::format("clamp({}, float3(-0.5), float3(0.5))", v());
        case 17:
            return cc::format("min({}, {})", v(), v());
        case 18:
            return cc::format("((mat3({}, {}, {}) * mat3({}, {}, {})) * {})", v(), v(), v(), v(), v(), v(), v());
        case 19:
            return cc::format("(({} * mat3({}, {}, {})) * {})", a(), v(), v(), v(), v());
        case 20:
            return cc::format("((mat3({}, {}, {}) - mat3({}, {}, {})) * {})", v(), v(), v(), v(), v(), v(), v());
        case 21:
        {
            auto const e = v();
            return cc::format("float4({0}.x, {0}.y, {0}.z, {1}).wzy", e, a());
        }
        case 22:
            return cc::format("exp(sin({}))", v());
        case 0:
            return cc::format("({} + {})", v(), v());
        case 1:
            return cc::format("({} - {})", v(), v());
        case 2:
            return cc::format("({} * {})", v(), v());
        case 3:
            return cc::format("({} * {})", v(), a());
        case 4:
            return cc::format("float3({}, {}, {})", a(), a(), a());
        case 5:
            return cc::format("cross({}, {})", v(), v());
        case 6:
        {
            auto const e = v();
            return cc::format("normalize({} * {} + float3(1.0))", e, e);
        }
        case 7:
            return cc::format("sin({})", v());
        case 8:
            return cc::format("tanh({})", v());
        case 9:
            return cc::format("(mat3({}, {}, {}) * {})", v(), v(), v(), v());
        case 10:
            return cc::format("(transpose(mat3({}, {}, {})) * {})", v(), v(), v(), v());
        case 11:
            return cc::format("mix({}, {}, {})", v(), v(), a());
        case 12:
            return cc::format("{}.{}", v(), cc::string_view(swizzles[pick(i32(sizeof(swizzles) / sizeof(swizzles[0])))]));
        case 13:
            return cc::format("max({}, {})", v(), v());
        default:
        {
            auto const e = a();
            return cc::format("({} / ({} * {} + 1.0))", v(), e, e);
        }
        }
    }

    static constexpr char const* swizzles[] = {"zxy", "yyx", "zzz", "xzx"};

    cc::string condition(i32 depth) { return cc::format("{} < {}", scalar(depth), scalar(depth)); }

    void line(i32 indent, cc::string_view s)
    {
        for (auto i = 0; i < indent; ++i)
            text += "    ";
        text += s;
        text += "\n";
    }

    /// `count` statements at `indent`, every local they declare out of scope after them.
    void block(i32 indent, i32 count, i32 nesting)
    {
        isize const scope[] = {scalars.size(),     vectors.size(),   mut_scalars.size(),
                               mut_vectors.size(), mut_blobs.size(), mut_matrices.size()};
        for (auto i = 0; i < count; ++i)
            statement(indent, nesting);
        scalars.resize_down_to(scope[0]);
        vectors.resize_down_to(scope[1]);
        mut_scalars.resize_down_to(scope[2]);
        mut_vectors.resize_down_to(scope[3]);
        mut_blobs.resize_down_to(scope[4]);
        mut_matrices.resize_down_to(scope[5]);
    }

    /// A value a loop assigns, whose growth over iterations stays bounded.
    cc::string bounded_scalar(cc::string_view self)
    {
        switch (pick(3))
        {
        case 0:
            return cc::format("0.5 * {} + tanh({})", self, scalar(2));
        case 1:
            return cc::format("{} * tanh({})", self, scalar(2));
        default:
            return cc::format("sin({} + {})", self, scalar(2));
        }
    }

    void statement(i32 indent, i32 nesting)
    {
        auto const kind = pick(20);
        if (kind <= 1)
        {
            auto const n = name("s");
            line(indent, cc::format("let {} = {}", n, scalar(3)));
            scalars.push_back(n);
        }
        else if (kind == 2)
        {
            auto const n = name("v");
            line(indent, cc::format("let {} = {}", n, vector(2)));
            vectors.push_back(n);
        }
        else if (kind == 3)
        {
            auto const n = name("m");
            line(indent, cc::format("let mut {} = {}", n, scalar(2)));
            scalars.push_back(n);
            mut_scalars.push_back(n);
        }
        else if (kind == 4)
        {
            auto const n = name("w");
            line(indent, cc::format("let mut {} = {}", n, vector(2)));
            vectors.push_back(n);
            mut_vectors.push_back(n);
        }
        else if (kind == 5 && !mut_scalars.empty())
        {
            auto const& n = mut_scalars[pick(i32(mut_scalars.size()))];
            if (loop_depth > 0)
                line(indent, cc::format("{} = {}", n, bounded_scalar(n)));
            else if (chance(50))
                line(indent, cc::format("{} += {}", n, scalar(2)));
            else
                line(indent, cc::format("{} = {}", n, scalar(3)));
        }
        else if (kind == 6 && !mut_vectors.empty())
        {
            auto const& n = mut_vectors[pick(i32(mut_vectors.size()))];
            auto const c = "xyz"[pick(3)];
            if (loop_depth > 0)
                line(indent, cc::format("{}.{} = {}", n, c, bounded_scalar(cc::format("{}.{}", n, c))));
            else if (chance(50))
                line(indent, cc::format("{}.{} = {}", n, c, scalar(2)));
            else
                line(indent, cc::format("{} = {}", n, vector(2)));
        }
        else if (kind == 7 && nesting < 3)
        {
            line(indent, cc::format("if {}:", condition(2)));
            block(indent + 1, rng.uniform(1, 3), nesting + 1);
            if (chance(50))
            {
                line(indent, "else:");
                block(indent + 1, rng.uniform(1, 3), nesting + 1);
            }
        }
        else if (kind == 8 && nesting < 3 && loop_depth < 2)
        {
            auto const index = name("i");
            auto const first = rng.uniform(0, 2);
            auto const count = rng.uniform(1, 4);
            auto const variant = pick(4);
            auto t = cc::string();
            if (variant == 0)
                line(indent, cc::format("for {} in {} ..< {}:", index, first, first + count));
            else if (variant == 1)
            {
                // an end only a `let` knows, so the stated bound is what keeps it a loop with a tape
                auto const end = name("e");
                line(indent, cc::format("let {} = {}", end, first + count));
                line(indent, cc::format("@max_iterations({})", rng.uniform(1, 5)));
                line(indent, cc::format("for {} in {} ..< {}:", index, first, end));
            }
            else if (variant == 2)
            {
                // a counter a `continue` may skip, so the bound is what ends it then
                line(indent, cc::format("let mut {} = {}", index, first));
                line(indent, cc::format("@max_iterations({})", rng.uniform(1, 5)));
                line(indent, cc::format("while {} < {}:", index, first + count));
                line(indent + 1, cc::format("{} += 1", index));
            }
            else
            {
                // a march whose step count follows the input
                t = name("t");
                line(indent, cc::format("let mut {} = 0.0", t));
                line(indent, cc::format("@max_iterations({})", rng.uniform(1, 6)));
                line(indent, cc::format("while {} < 1.0:", t));
                line(indent + 1, cc::format("{} += 0.3 + 0.1 * tanh({})", t, scalar(2)));
            }
            indices.push_back(index);
            if (!t.empty())
                indices.remove_back();
            ++loop_depth;
            block(indent + 1, rng.uniform(1, 4), nesting + 1);
            --loop_depth;
            if (t.empty())
                indices.remove_back();
            else
                scalars.push_back(t);
        }
        else if (kind == 9 && loop_depth > 0)
            line(indent, cc::format("if {} => {}", condition(1), chance(50) ? "break" : "continue"));
        else if (kind == 10 && nesting > 0)
            line(indent, cc::format("if {} => return {}", condition(1), scalar(2)));
        else if (kind == 11 && nesting < 3 && loop_depth < 2)
        {
            // an array whose elements a loop overwrites one by one, each slot an element of its own tape
            auto const n = name("a");
            auto const i = name("i");
            line(indent, cc::format("let mut {} = [{}, {}, {}]", n, scalar(2), scalar(2), scalar(2)));
            line(indent, cc::format("for {} in 0 ..< 3:", i));
            indices.push_back(i);
            line(indent + 1, cc::format("{0}[{1}] = 0.5 * {0}[{1}] + tanh({2})", n, i, scalar(2)));
            indices.remove_back();
            for (auto k = 0; k < 3; ++k)
                scalars.push_back(cc::format("{}[{}]", n, k));
        }
        else if (kind == 12)
        {
            // a struct of a position, a float and an int, which only some fields of takes a tangent
            auto const n = name("b");
            line(indent, cc::format("let mut {} = blob(at = pos3({}, {}, {}), weight = {}, count = 2)", n, scalar(1),
                                    scalar(1), scalar(1), scalar(2)));
            scalars.push_back(cc::format("{}.weight", n));
            scalars.push_back(cc::format("distance({}.at, pos3(0.25, 0.5, -0.5))", n));
            mut_blobs.push_back(n);
        }
        else if (kind == 14)
        {
            auto const n = name("q");
            line(indent, cc::format("let mut {} = mat3({}, {}, {})", n, vector(1), vector(1), vector(1)));
            mut_matrices.push_back(n);
        }
        else if (kind == 15 && !mut_matrices.empty())
        {
            // a matrix a loop overwrites, whose save is a matrix on the tape
            auto const& n = mut_matrices[pick(i32(mut_matrices.size()))];
            line(indent, cc::format("{0} = {0} * 0.5 + mat3({1}, {2}, {3}) * tanh({4})", n, vector(1), vector(1),
                                    vector(1), scalar(1)));
        }
        else if (kind == 16 && (!mut_scalars.empty() || !mut_vectors.empty()))
        {
            // a call that assigns its `mut` parameter: a local, a vector's component
            auto const place = !mut_vectors.empty() && chance(50)
                                 ? cc::format("{}.{}", mut_vectors[pick(i32(mut_vectors.size()))], "xyz"[pick(3)])
                                 : (!mut_scalars.empty() ? mut_scalars[pick(i32(mut_scalars.size()))]
                                                         : cc::format("{}.x", mut_vectors[0]));
            line(indent, cc::format("nudge(mut {}, {})", place, scalar(2)));
        }
        else if (kind == 13 && !mut_blobs.empty())
        {
            auto const& n = mut_blobs[pick(i32(mut_blobs.size()))];
            if (chance(50))
                line(indent, cc::format("{0}.weight = {1}", n,
                                        loop_depth > 0 ? bounded_scalar(cc::format("{}.weight", n)) : scalar(2)));
            else
                line(indent,
                     cc::format("{0}.at = {0}.at + vec3(tanh({1}), 0.5 * sin({2}), 0.25)", n, scalar(1), scalar(1)));
        }
        else if (kind == 18 && !mut_vectors.empty())
        {
            // an assignment through a swizzle, which writes two fields and keeps the third
            cc::string_view const pairs[] = {"xy", "zx", "yz", "xz"};
            line(indent, cc::format("{}.{} = float2(tanh({}), tanh({}))", mut_vectors[pick(i32(mut_vectors.size()))],
                                    pairs[pick(4)], scalar(2), scalar(2)));
        }
        else if (kind == 19 && nesting < 3)
        {
            // a case on an int, an arm of its own per value and the rest
            line(indent, cc::format("case {}:", index()));
            line(indent + 1, "0 =>:");
            block(indent + 2, rng.uniform(1, 2), nesting + 1);
            line(indent + 1, "1 or 2 =>:");
            block(indent + 2, rng.uniform(1, 2), nesting + 1);
            line(indent + 1, "_ =>:");
            block(indent + 2, rng.uniform(1, 2), nesting + 1);
        }
        else
        {
            auto const n = name("s");
            line(indent, cc::format("let {} = {}", n, scalar(2)));
            scalars.push_back(n);
        }
    }

    /// `fun f<k>(x: float3, y: float) -> float`, and the function that holds its derivatives to each other.
    void function(i32 k)
    {
        is_held = false;
        line(0, cc::format("fun f{}(x: float3, y: float) -> float:", k));
        block(1, rng.uniform(2, 7), 0);
        line(1, cc::format("return {}", scalar(3)));
        line(0, "");
        line(0, cc::format("fun agrees{}(x: float3, y: float, ax: float3, ay: float) -> bool:", k));
        line(1, cc::format("let forward = fwd_diff(f{}, x, ax, y, ay)", k));
        line(1, "let mut gx = float3(0.0)");
        line(1, "let mut gy = 0.0");
        line(1, cc::format("bwd_diff(f{}, x, mut gx, y, mut gy, 1.0)", k));
        line(1, "let backward = dot(gx, ax) + gy * ay");
        line(1, "let modes_agree = nearly_equal(forward, backward, within = 1e-3 * (1.0 + abs(forward) + "
                "abs(backward)))");
        line(1, "let h = 4e-3");
        line(1, cc::format("let wide = (f{0}(x + ax * h, y + ay * h) - f{0}(x - ax * h, y - ay * h)) / (2.0 * h)", k));
        line(1, cc::format("let narrow = (f{0}(x + ax * (0.5 * h), y + ay * (0.5 * h)) - f{0}(x - ax * (0.5 * h), y - "
                           "ay * (0.5 * h))) / h",
                           k));
        line(1, cc::format("let roundoff = 1e-6 * (1.0 + abs(f{}(x, y))) / h", k));
        line(1, "let tolerance = 2e-2 * (1.0 + abs(narrow)) + roundoff");
        // a kink exactly at the point leaves both central differences alike, and the two one-sided ones apart
        line(1, cc::format("let at = f{}(x, y)", k));
        line(1, cc::format("let left = (at - f{}(x - ax * (0.5 * h), y - ay * (0.5 * h))) / (0.5 * h)", k));
        line(1, cc::format("let right = (f{}(x + ax * (0.5 * h), y + ay * (0.5 * h)) - at) / (0.5 * h)", k));
        line(1, "let smooth = nearly_equal(wide, narrow, within = tolerance) and nearly_equal(left, right, within = "
                "4.0 * tolerance)");
        // `no_diff` changes the derivative and not the value, so only the two modes can be compared
        if (is_held)
            line(1, "return modes_agree");
        else
            line(1, "return modes_agree and (nearly_equal(forward, narrow, within = tolerance) or not smooth)");
        line(0, "");
        auto const point = [&] { return cc::format("float3({}, {}, {}), {}", number(), number(), number(), number()); };
        line(0, cc::format("test agrees{}({}, {})", k, point(), point()));
        line(0, "");
    }

    /// One compute stage that takes every function's derivatives, so each target writes them.
    void kernel(i32 count)
    {
        line(0, "binding io:");
        line(1, "values: mut buffer[float]");
        line(0, "");
        line(0, "@compute(1) fun kernel(){io}:");
        line(1, "let x = float3(io.values[0], io.values[1], io.values[2])");
        line(1, "let y = io.values[3]");
        line(1, "let mut total = 0.0");
        for (auto k = 0; k < count; ++k)
        {
            line(1, cc::format("let mut gx{} = float3(0.0)", k));
            line(1, cc::format("let mut gy{} = 0.0", k));
            line(1, cc::format("bwd_diff(f{0}, x, mut gx{0}, y, mut gy{0}, 1.0)", k));
            line(1, cc::format("total += dot(gx{0}, float3(1.0)) + gy{0} + fwd_diff(f{0}, x, x, y, y)", k));
        }
        line(1, "io.values[4] = total");
    }
};

/// A program of `count` random functions, each with its test, and a kernel over all of them.
cc::string random_program(u64 seed, i32 count)
{
    auto rng = cc::random(seed);
    auto w = program_writer{.rng = rng};
    w.line(0, cc::format("// written by autodiff-fuzz-test.cc from seed {}", seed));
    w.line(0, "");
    w.line(0, "struct blob:");
    w.line(1, "at: pos3");
    w.line(1, "weight: float");
    w.line(1, "count: int");
    w.line(0, "");
    // a call that leaves early, and one that picks by an index, each inlined into an expression
    w.line(0, "fun ramp(a: float) -> float:");
    w.line(1, "if a < 0.0 => return 0.25 * a");
    w.line(1, "return a * a * 0.5 + a");
    w.line(0, "");
    w.line(0, "fun nudge(s: mut float, by: float):");
    w.line(1, "s = 0.5 * s + tanh(by)");
    w.line(0, "");
    // a loop of its own, inlined into whatever loop calls it
    w.line(0, "fun smooth_sum(v: float3, n: float) -> float:");
    w.line(1, "let mut acc = 0.0");
    w.line(1, "for k in 0 ..< 3:");
    w.line(2, "acc = 0.5 * acc + sin(component(v, k) * n)");
    w.line(2, "if acc > 0.9 => break");
    w.line(1, "return acc");
    w.line(0, "");
    w.line(0, "fun component(v: float3, k: int) -> float:");
    w.line(1, "if k == 0 => return v.x");
    w.line(1, "if k == 1 => return v.y");
    w.line(1, "return v.z");
    w.line(0, "");
    for (auto k = 0; k < count; ++k)
        w.function(k);
    w.kernel(count);
    return w.text;
}
} // namespace

TEST("sgl autodiff - random programs: forward agrees with backward, and both with central differences")
{
    // about 0.2 s a program on the interpreter
    auto const programs = nx::is_thorough() ? 200 : nx::is_smoke() ? 2 : 6;
    auto const functions = 8;
    auto seeds = nx::test_random();
    for (auto p = 0; p < programs; ++p)
    {
        auto const seed = seeds.next_u64();
        auto const source = random_program(seed, functions);
        auto const tested = sgl::test_source(source, "autodiff-fuzz.sgl");
        CHECK(tested.errors == "").dump("seed", seed).dump("source", source);
        CHECK(tested.tests_passed == functions).dump("seed", seed);
        if (!tested.errors.empty())
            continue;
        auto const texts = sgl::compile_all_to_text(
            {.source = source, .source_name = "autodiff-fuzz.sgl", .targets = sgl::emit::all_targets()});
        REQUIRE(texts.has_value());
        for (auto const& e : texts.value())
            if (e.text.has_error())
                CHECK(e.text.error() == "")
                    .dump("seed", seed)
                    .dump("target", sgl::emit::to_string(e.target))
                    .dump("source", source);
    }
}
