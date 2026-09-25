#include <clean-core/string/format.hh>
#include <shaped-graphics-language/builtins/register.hh>

using namespace sgl;
using namespace sgl::builtins;

namespace
{
using check::value_kind;

/// How each target spells one scalar family, and what its values are to the interpreter.
struct scalar_family
{
    cc::string_view name;               ///< `float`, and the stem of `float2` to `float4`
    cc::string_view wgsl;               ///< `f32`
    cc::string_view wgsl_vector_suffix; ///< `f` of `vec3f`, or empty where WGSL has no alias and writes `vec3<bool>`
    value_kind leaf_kind;
    bool has_layout;    ///< a bool has a different size in every target's block, so it has no place in one
    bool crosses_edges; ///< an int would need a flat interpolation nothing states yet, and WGSL passes no bool
};

constexpr scalar_family k_float_family = {"float", "f32", "f", value_kind::scalar_float, true, true};
constexpr scalar_family k_int_family = {"int", "i32", "i", value_kind::scalar_int, true, false};
constexpr scalar_family k_uint_family = {"uint", "u32", "u", value_kind::scalar_uint, true, false};
constexpr scalar_family k_bool_family = {"bool", "bool", "", value_kind::boolean, false, false};

/// A vector of `width` scalars of `family` with the fields `x y [z [w]]`, which every target spells as its own vector.
type_record vector_of(scalar_family const& family, cc::string_view name, i32 width, cc::string_view doc)
{
    auto declaration = cc::format("struct {}:", name);
    cc::string_view const fields[] = {"x", "y", "z", "w"};
    for (auto i = 0; i < width; ++i)
        declaration.appendf("\n    {}: {}", fields[i], family.name);

    auto const native = cc::format("{}{}", family.name, width);
    auto record = type_record{
        .declaration = cc::move(declaration),
        .doc = doc,
        .hlsl = native,
        .wgsl = family.wgsl_vector_suffix.empty() ? cc::format("vec{}<{}>", width, family.wgsl)
                                                  : cc::format("vec{}{}", width, family.wgsl_vector_suffix),
        .msl = native,
        .leaf_kind = family.leaf_kind,
        .leaf_count = width,
        .crosses_edges = family.crosses_edges,
    };
    if (family.has_layout)
    {
        // WGSL and MSL align a two-vector at 8 and a wider one at 16, and MSL also sizes a three-vector 16:
        // nothing fits into its tail.
        auto const aligned = width == 2 ? 8 : 16;
        record.hlsl_layout = {.size = width * 4, .alignment = 4};
        record.wgsl_layout = {.size = width * 4, .alignment = aligned};
        record.msl_layout = {.size = width == 3 ? 16 : width * 4, .alignment = aligned};
    }
    return record;
}

/// The scalar of `family`, which every target spells by its own name.
type_record scalar_of(scalar_family const& family, cc::string_view doc)
{
    auto record = type_record{
        .declaration = cc::format("struct {}", family.name),
        .doc = doc,
        .hlsl = cc::string(family.name),
        .wgsl = cc::string(family.wgsl),
        .msl = cc::string(family.name),
        .leaf_kind = family.leaf_kind,
        .leaf_count = 1,
        .crosses_edges = family.crosses_edges,
    };
    if (family.has_layout)
    {
        record.hlsl_layout = {.size = 4, .alignment = 4};
        record.wgsl_layout = {.size = 4, .alignment = 4};
        record.msl_layout = {.size = 4, .alignment = 4};
    }
    return record;
}

/// The vectors of a family at widths 2 to 4, each named after the family and its width.
void add_vectors(registry& r, scalar_family const& family)
{
    for (auto width = 2; width <= 4; ++width)
        r.add(vector_of(family, cc::format("{}{}", family.name, width), width, ""));
}
} // namespace

void sgl::builtins::register_types(registry& r)
{
    r.add_comment("// A struct line without a block is opaque: there is no member to name.");
    r.add(scalar_of(k_float_family, ""));
    add_vectors(r, k_float_family);
    r.add(vector_of(k_float_family, "vec3", 3, "/// A direction: it has a length, and a translation leaves it alone."));
    r.add(vector_of(k_float_family, "pos3", 3, "/// A position: a translation moves it."));
    r.add(vector_of(k_float_family, "hpos4", 4, "/// A position in clip space, before the divide."));

    r.add(type_record{
        .declaration = "struct mat4",
        .doc = "/// Column-major, and a vector stands to its right.",
        .hlsl = "float4x4",
        .wgsl = "mat4x4f",
        .msl = "float4x4",
        // HLSL starts a matrix on a fresh 16-byte row, which is what an alignment of 16 says.
        .hlsl_layout = {.size = 64, .alignment = 16},
        .wgsl_layout = {.size = 64, .alignment = 16},
        .msl_layout = {.size = 64, .alignment = 16},
        .leaf_kind = value_kind::scalar_float,
        .leaf_count = 16,
    });

    r.add(scalar_of(k_int_family, "/// 32 bits, signed; its arithmetic wraps."));
    add_vectors(r, k_int_family);
    r.add(scalar_of(k_uint_family, "/// 32 bits, unsigned; its arithmetic wraps."));
    add_vectors(r, k_uint_family);
    // A builtin enum: `bool.true` is a case like any other, and the record says the targets write it as their bool.
    // `false` comes first, so the zero value of a bool is false.
    auto boolean = scalar_of(k_bool_family, "");
    boolean.declaration = "enum bool:\n    false\n    true";
    r.add(cc::move(boolean));
    add_vectors(r, k_bool_family);
}
