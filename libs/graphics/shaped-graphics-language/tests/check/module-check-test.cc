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

    // CHK-350: two bindings of one name in one list would be one name twice in the target text
    cc::string_view const two[] = {k_view, "module post\nbinding frame:\n    gain: float\n"};
    CHECK(reports_with(two, "use view\nuse post\nfun f(x: float){view.frame, post.frame} -> float => x\n")
          == "unsupported-yet user:[post.frame] two bindings named frame in one list, from different modules\n");

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
    CHECK(reports_with(library, "use shaped.view\n") == "unsupported-yet user:[use shaped.view] a dotted module path\n");
    CHECK(reports_with(library, "module shaped.view\n")
          == "unsupported-yet user:[module shaped.view] a dotted module name\n");
    CHECK(reports_with(library, "use view\nuse lights as view\n")
          == "duplicate-declaration user:[use lights as view] view names a module of this file already\n");
    CHECK(reports_with(library, "use view\nstruct view:\n    x: float\n")
          == "duplicate-declaration user:[use view] view names a module here, and a declaration of this module\n");
    // a prelude name too, which then keeps meaning the prelude's declaration
    CHECK(reports_with(library, "use view as float\nfun f(x: float) -> float => x\n")
          == "duplicate-declaration user:[use view as float] float names a module here, and a declaration of the "
             "prelude\n");
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

TEST("sgl check - a module the program uses is a library: no operator and no file sampler of its own yet")
{
    cc::string_view const library[] = {"module ops\n@operator(\"-\") fun minus(a: float, b: float) -> float => a\n",
                                       "module samplers\nsampler bilinear:\n    filter = .linear\n"};
    CHECK(reports_with(library, "use ops\n")
          == "unsupported-yet lib.0:[operator] an @operator function in a module the program uses\n");
    CHECK(reports_with(library, "use samplers\n")
              .contains("unsupported-yet lib.1:[sampler bilinear:] a file-scope sampler in a module the program uses"));
}
