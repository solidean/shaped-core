#include "register.hh"

#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/features.hh>

using namespace sgl;
using namespace sgl::builtins;

namespace
{
/// `enum name:` and one case per name, in the order of the C++ enum the cases mirror.
cc::string enum_of(cc::string_view doc, cc::string_view name, cc::span<cc::string_view const> cases)
{
    auto result = cc::format("{}\nenum {}:", doc, name);
    for (auto const c : cases)
        result.appendf("\n    {}", c);
    return result;
}
} // namespace

void sgl::builtins::register_target(registry& r)
{
    r.add_comment("// What a compile writes for, which a `const if` branches on (CHK-426).\n"
                  "// Each compile stands for its own target and device where an inlined body reads them.\n"
                  "// So the values below are a test's, which writes no text and runs on no device.");

    r.add_declaration(enum_of("/// An API a compile writes text for; `none` where no text is written.", "graphics_api",
                              check::k_graphics_api_names));
    r.add_declaration(enum_of("/// A language a compile writes; `sgl` where no text is written, since the interpreter "
                              "runs SGL itself.",
                              "shading_language", check::k_shading_language_names));
    r.add_declaration(
        enum_of("/// A feature a device may have, named as sg names it.", "feature", check::k_feature_names));

    r.add_declaration("/// The API this compile writes for, known once a body is inlined; no const holds it.\n"
                      "const target_api: graphics_api = .none");
    r.add_declaration("/// The language this compile writes, known once a body is inlined; no const holds it.\n"
                      "const target_language: shading_language = .sgl");
    r.add_declaration("/// Whether the device this compile writes for has `f`, known once a body is inlined.\n"
                      "/// A branch that `has` takes may use `f` without a `require`.\n"
                      "/// The body is a test's answer; `1 < 0`, since this file holds no `bool` case to name.\n"
                      "fun has(f: feature) -> bool => 1 < 0");
}
