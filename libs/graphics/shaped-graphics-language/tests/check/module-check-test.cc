#include "check-test-support.hh"

using namespace sgl_test;

namespace
{
/// A module of the shape the viewer's raster route wants: a binding, a type with a function, a const and an enum.
constexpr cc::string_view k_view = "module view\n"
                                   "\n"
                                   "binding frame:\n"
                                   "    exposure: float\n"
                                   "\n"
                                   "struct sample:\n"
                                   "    value: float\n"
                                   "\n"
                                   "fun sample.doubled(self) -> float => self.value * 2.0\n"
                                   "fun brighten(s: sample) -> float => s.value + 1.0\n"
                                   "fun exposed(x: float){frame} -> float => x * frame.exposure\n"
                                   "\n"
                                   "const levels = 4\n"
                                   "\n"
                                   "enum tone:\n"
                                   "    linear\n"
                                   "    aces\n";
} // namespace

TEST("sgl check - a qualified name reaches what a module declares, wherever a bare name could stand")
{
    cc::string_view const library[] = {k_view};
    // a type, a constructor, a function, a method, a const, an enum case, a binding listed and read
    CHECK(reports_with(library, "use view\n"
                                "fun f(x: float){view.frame} -> float:\n"
                                "    let s: view.sample = view.sample(x)\n"
                                "    let t = view.tone.aces\n"
                                "    let n = view.levels\n"
                                "    return view.exposed(s.doubled()) + view.frame.exposure\n")
          == "");
    // `use … as` names the module by its alias alone
    CHECK(reports_with(library, "use view as v\nfun f(x: float) -> float => v.brighten(v.sample(x))\n") == "");
    CHECK(reports_with(library, "use view as v\nfun f(x: float) -> float => view.brighten(v.sample(x))\n")
          == "unknown-name user:[view] view\n");
}

TEST("sgl check - a foreign name is always qualified, and a module is no value")
{
    cc::string_view const library[] = {k_view};
    CHECK(reports_with(library, "use view\nfun f(x: float){view.frame} -> float => exposed(x)\n")
          == "unknown-name user:[exposed] exposed\n");
    CHECK(reports_with(library, "use view\nfun f() -> float:\n    let v = view\n    return 0.0\n")
          == "wrong-kind-of-name user:[view] view is a module, and only `view.name` reaches what it declares\n");
    // a module's own names are what it declares, and the prelude's are not among them
    CHECK(
        reports_with(library, "use view\nfun f(x: view.float) -> float => x\n").contains("unknown-name user:[view.float]"));
    // a local hides the module, as it hides any module-level name
    CHECK(reports_with(library, "use view\nfun f(view: float) -> float => view\n") == "");
    // a qualified call too: the type of its argument does not widen it to the prelude's functions
    CHECK(reports_with(library, "use view\nfun f(x: float) -> float => view.saturate(x)\n")
          == "unknown-name user:[view.saturate] view.saturate\n");
}

TEST("sgl check - a call on a value of a module's type finds that module's functions (CHK-247)")
{
    cc::string_view const library[] = {k_view};
    // `brighten` is declared where `sample` is, so the dot call and the free call both find it unqualified
    CHECK(reports_with(library, "use view\nfun f(s: view.sample) -> float => s.brighten()\n") == "");
}

TEST("sgl check - a binding of a module is listed and read through its qualified name")
{
    cc::string_view const library[] = {k_view};
    CHECK(reports_with(library, "use view\nfun f(x: float) -> float => x * view.frame.exposure\n")
          == "binding-not-listed user:[view.frame] view.frame is not in the binding list of f\n");
    // a call of a module's function needs what it lists, by the same rule as a call of the file's own
    CHECK(reports_with(library, "use view\nfun f(x: float) -> float => view.exposed(x)\n").contains("binding-not-listed"));
    CHECK(reports_with(library, "use view\ntest {view.frame}:\n    view.frame.exposure == 0.0\n") == "");

    // CHK-351: two bindings of one name from different modules are two bindings, which one list may name
    cc::string_view const two[] = {k_view, "module post\nbinding frame:\n    gain: float\n"};
    CHECK(reports_with(two, "use view\nuse post\nfun f(x: float){view.frame, post.frame} -> float => x\n") == "");

    // CHK-351: one binding listed twice, by its name or through two aliases of its module
    CHECK(reports_with(library, "use view\nuse view as v\nfun f(x: float){view.frame, v.frame} -> float => x\n")
          == "duplicate-declaration user:[v.frame] frame is listed already\n");
    CHECK(reports_with({}, "binding frame:\n    e: float\nfun f(x: float){frame, frame} -> float => x\n")
          == "duplicate-declaration user:[frame] frame is listed already\n");
}

TEST("sgl check - what a `use` names must be a module of the library, and not the file's own")
{
    cc::string_view const library[] = {k_view, "module lights\nuse lights\n"};
    CHECK(reports_with(library, "use nowhere\n") == "unknown-module user:[use nowhere] nowhere\n");
    CHECK(reports_with(library, "use lights\n") == "use-of-own-module lib.1:[use lights] this file is of module lights\n");
    // a dotted path is one module's whole name, so `shaped.view` is no name of `view`
    CHECK(reports_with(library, "use shaped.view\n") == "unknown-module user:[use shaped.view] shaped.view\n");
    CHECK(reports_with(library, "use view\nuse lights as view\n")
          == "duplicate-declaration user:[use lights as view] view names a module of this file already\n");
    CHECK(reports_with(library, "use view\nstruct view:\n    x: float\n")
          == "duplicate-declaration user:[use view] view names a module here, and a declaration of this module\n");
    // a prelude name too, which then keeps meaning the prelude's declaration
    CHECK(reports_with(library, "use view as float\nfun f(x: float) -> float => x\n")
          == "duplicate-declaration user:[use view as float] float names a module here, and a declaration of the "
             "prelude\n");
}

TEST("sgl check - a dotted module name is the whole name, and its `use` binds the last part (CHK-346, CHK-347)")
{
    cc::string_view const library[]
        = {"module palette.warm\nfun tone() -> float => 2.0\n", "module heat.warm\nfun tone() -> float => 3.0\n",
           "module heat\nuse heat.warm\nfun twice() -> float => warm.tone() * 2.0\n"};
    CHECK(reports_with(library, "use palette.warm\nfun f() -> float => warm.tone()\n") == "");
    CHECK(reports_with(library, "use palette.warm as p\nfun f() -> float => p.tone()\n") == "");
    // the path binds nothing but its last part, and a module of its first part alone is another module
    CHECK(reports_with(library, "use palette.warm\nfun f() -> float => palette.warm.tone()\n")
          == "unknown-name user:[palette] palette\n");
    CHECK(reports_with(library, "use heat\nuse heat.warm as w\nfun f() -> float => heat.twice() + w.tone()\n") == "");
    // two paths ending alike bind one name, so the second needs `as`
    CHECK(reports_with(library, "use palette.warm\nuse heat.warm\n")
          == "duplicate-declaration user:[use heat.warm] warm names a module of this file already\n");
    CHECK(reports_with(library, "use palette\n") == "unknown-module user:[use palette] palette\n");
    // a program joining a dotted module sees its names unqualified
    CHECK(reports_with(library, "module palette.warm\nfun f() -> float => tone()\n") == "");
}

TEST("sgl check - modules that use each other in a loop are module-cycle")
{
    cc::string_view const library[] = {"module a\nuse b\n", "module b\nuse a\n"};
    CHECK(reports_with(library, "use a\n") == "module-cycle lib.1:[use a] a -> b -> a\n");
}

TEST("sgl check - a use is the file's own, and a module's files see each other unqualified")
{
    // `b.sgl` of module `lights` sees `a.sgl`'s `key`, and not `a.sgl`'s `use view`
    cc::string_view const library[] = {k_view, "module lights\nuse view\nfun key() -> float => 1.0\n",
                                       "module lights\nfun fill() -> float => key() * 0.5\n"
                                       "fun probe(s: view.sample) -> float => s.value\n"};
    CHECK(reports_with(library, "use lights\nfun f() -> float => lights.fill()\n") == "unknown-name lib.2:[view] view\n");
}

TEST("sgl check - only the modules the program reaches are checked")
{
    // the broken module is never named, so it is never read past its `module` line
    cc::string_view const library[] = {k_view, "module broken\nfun f( -> \n", "fun loose() -> float => nope\n"};
    auto const s = check_with_library(library, "use view\n");
    CHECK(reports_of(s) == "");
    CHECK(s.module.library_files.size() == 1);
    CHECK(s.module.library_files[0] == 0);
}

TEST("sgl check - a program whose module line names a library module joins it")
{
    cc::string_view const library[] = {k_view};
    CHECK(reports_with(library, "module view\nfun f(x: float) -> float => brighten(sample(x))\n") == "");
}

TEST("sgl check - a module's operator applies where the module is used, and its file sampler too")
{
    cc::string_view const library[] = {"module ops\nstruct v:\n    x: float\n"
                                       "@operator(\"+\") fun plus(a: v, b: v) -> v => v(a.x + b.x)\n",
                                       "module samplers\nsampler bilinear:\n    filter = .linear\n",
                                       "module maker\nuse ops\nfun make() -> ops.v => ops.v(1.0)\n"};
    // CHK-350: an operator has no qualified spelling, so a `use` brings it with the module
    CHECK(reports_with(library, "use ops\nfun f(a: ops.v) -> ops.v => a + a\n") == "");
    // a file that does not use the module sees none of its operators, as it sees none of its names
    CHECK(reports_with(library, "use maker\nfun f() => maker.make() + maker.make()\n")
              .starts_with("no-matching-overload user:[maker.make() + maker.make()]"));
    // EMIT-133: a module's file-scope sampler shares the program's index space
    CHECK(reports_with(library, "use samplers\n") == "");
}

TEST("sgl check - a struct named as an instance of its own module's generic struct the host sees is named is refused "
     "(CHK-530)")
{
    constexpr auto pair = cc::string_view("struct pair[T]:\n    a: T\n");
    constexpr auto refusal
        = cc::string_view("struct pair_float is named as the instance pair[float] of a generic struct "
                          "is, which the host names pair_float too\n");
    // the description names `pair[float]` `pair_float` (EMIT-179), so a struct of that name beside a binding holding it
    // would be named alike
    CHECK(reports_with(
              {}, cc::format("{}struct pair_float:\n    x: int\nbinding data:\n    xs: buffer[pair[float]]\n", pair))
          == cc::format("duplicate-declaration user:[pair_float] {}", refusal));
    // and so through a struct the binding holds, and in a constant block
    CHECK(reports_with({}, cc::format("{}struct pair_float:\n    x: int\nstruct holder:\n    p: pair[float]\n"
                                      "binding data:\n    xs: buffer[holder]\n",
                                      pair))
          == cc::format("duplicate-declaration user:[pair_float] {}", refusal));
    CHECK(reports_with({}, cc::format("{}struct pair_float:\n    x: int\nbinding data:\n    p: pair[float]\n", pair))
          == cc::format("duplicate-declaration user:[pair_float] {}", refusal));
    // a ray-tracing stage's payload is the host's too (EMIT-269)
    CHECK(reports_with({}, cc::format("require raytracing_pipeline\n{}struct pair_float:\n    x: int\n"
                                      "@miss fun sky(p: mut pair[float]):\n    p.a = 1.0\n",
                                      pair))
          == cc::format("duplicate-declaration user:[pair_float] {}", refusal));
    // without the instance nothing names it twice
    CHECK(reports_with({},
                       cc::format("{}struct pair_float:\n    x: int\nbinding data:\n    xs: buffer[pair[int]]\n", pair))
          == "");

    // a module's own pair, inside the module
    cc::string_view const both[] = {"module view\nstruct pair[T]:\n    a: T\nstruct pair_float:\n    x: int\n"
                                    "binding data:\n    xs: buffer[pair[float]]\n"};
    CHECK(reports_with(both, "use view\n") == cc::format("duplicate-declaration lib.0:[pair_float] {}", refusal));
    // a module's `view.pair_float` beside the program's own `pair[float]` is named apart, and stays allowed
    cc::string_view const theirs[] = {"module view\nstruct pair_float:\n    x: int\n"};
    CHECK(reports_with(theirs, cc::format("use view\n{}binding data:\n    xs: buffer[pair[float]]\n"
                                          "    ys: buffer[view.pair_float]\n",
                                          pair))
          == "");
}

TEST("sgl check - two instances of one module's generic structs the host names alike are refused (CHK-530)")
{
    // `pair[a_b]` and `pair_a[b]` are both `pair_a_b` to the host, which the target text tells apart by minting
    CHECK(reports_with({}, "struct pair[T]:\n    a: T\nstruct pair_a[T]:\n    b: T\nstruct a_b:\n    x: float\n"
                           "struct b:\n    y: float\nbinding data:\n    xs: buffer[pair[a_b]]\n    ys: "
                           "buffer[pair_a[b]]\n")
          == "duplicate-declaration user:[pair_a] the instances pair[a_b] and pair_a[b] of generic structs are both "
             "named pair_a_b by the host\n");
    // one the host never sees names nothing
    CHECK(reports_with({}, "struct pair[T]:\n    a: T\nstruct pair_a[T]:\n    b: T\nstruct a_b:\n    x: float\n"
                           "struct b:\n    y: float\nbinding data:\n    xs: buffer[pair[a_b]]\n"
                           "fun f(p: pair_a[b]) -> float => p.b.y\n")
          == "");
}

TEST("sgl check - an instance the host never sees takes no name a struct could clash with (CHK-530)")
{
    // the prelude's atomics make `exchange_result[int]`, which no binding of this program holds
    CHECK(reports_with({}, "struct exchange_result_int:\n    x: int\nbinding data:\n    ys: mut "
                           "buffer[exchange_result_int]\n")
          == "");
    // an open instance, `pair[T]` in a generic function, is no type the host meets
    CHECK(reports_with({}, "struct pair[T]:\n    a: T\nstruct pair_T:\n    x: int\nfun first[T](p: pair[T]) -> T => "
                           "p.a\n"
                           "binding data:\n    ys: mut buffer[pair_T]\n"
                           "@compute(1) fun run(@thread_id id: int3){data}:\n"
                           "    data.ys[id.x] = pair_T(first(pair[int](3)))\n")
          == "");
    // nor is one a body holds only in a local
    CHECK(reports_with({}, "struct pair[T]:\n    a: T\nstruct pair_float:\n    x: int\n"
                           "binding data:\n    ys: mut buffer[pair_float]\n"
                           "@compute(1) fun run(@thread_id id: int3){data}:\n"
                           "    let p = pair[float](1.0)\n"
                           "    data.ys[id.x] = pair_float(p.a as int)\n")
          == "");
}
