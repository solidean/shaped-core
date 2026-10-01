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
    bool crosses_edges; ///< WGSL passes no bool; an int crosses only flat, which the check pass holds it to
};

constexpr scalar_family k_float_family = {"float", "f32", "f", value_kind::scalar_float, true, true};
constexpr scalar_family k_int_family = {"int", "i32", "i", value_kind::scalar_int, true, true};
constexpr scalar_family k_uint_family = {"uint", "u32", "u", value_kind::scalar_uint, true, true};
constexpr scalar_family k_bool_family = {"bool", "bool", "", value_kind::boolean, false, false};

/// A vector of `width` scalars of `family` with the fields `x y [z [w]]`, which every target spells as its own vector.
/// Its swizzles are the target's own too (EMIT-142).
type_record vector_of(scalar_family const& family, cc::string_view name, i32 width, cc::string_view doc)
{
    auto declaration = cc::format("@swizzle struct {}:", name);
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

/// A vector of the prelude, which takes a one-value constructor once every vector is registered.
struct registered_vector
{
    builtin_type_id id;
    cc::string_view element;
    i32 width;
};

/// The vectors of a family at widths 2 to 4, each named after the family and its width.
void add_vectors(registry& r, scalar_family const& family, cc::vector<registered_vector>& vectors)
{
    for (auto width = 2; width <= 4; ++width)
        vectors.push_back({.id = r.add(vector_of(family, cc::format("{}{}", family.name, width), width, "")),
                           .element = family.name,
                           .width = width});
}

template <int Width>
void fill(cc::span<check::scalar const> in, cc::vector<check::scalar>& out)
{
    for (auto i = 0; i < Width; ++i)
        out.push_back(in[0]);
}

/// The target's own vector of one value, `(float3)x`, `vec3f(x)` and `float3(x)`; `data` names the vector.
/// HLSL's constructor wants every component, so it spreads a scalar by a cast.
written write_filled(call_context const& c)
{
    auto const type = c.builtins.at(builtin_type_id(i32(c.data))).spelled_in(c.target);
    if (c.target == language::hlsl)
        return {.text = cc::format("({}){}", type, wrapped(c.arguments[0], precedence::unary)),
                .binds = precedence::unary};
    return {.text = cc::format("{}({})", type, c.arguments[0].text)};
}
} // namespace

void sgl::builtins::register_types(registry& r)
{
    r.add_comment("// A struct line without a block is opaque: there is no member to name.\n"
                  "// A builtin type is @shadowable(false): a program's own `int` would be a second type that reads "
                  "the same.");
    auto vectors = cc::vector<registered_vector>();
    r.add(scalar_of(k_float_family, ""));
    add_vectors(r, k_float_family, vectors);
    vectors.push_back({.id = r.add(vector_of(k_float_family, "vec3", 3,
                                             "/// A direction: it has a length, and a translation leaves it alone.")),
                       .element = "float",
                       .width = 3});
    vectors.push_back({.id = r.add(vector_of(k_float_family, "pos3", 3, "/// A position: a translation moves it.")),
                       .element = "float",
                       .width = 3});
    vectors.push_back(
        {.id = r.add(vector_of(k_float_family, "hpos4", 4, "/// A position in clip space, before the divide.")),
         .element = "float",
         .width = 4});

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

    r.add(type_record{
        .declaration = "struct bool32",
        .doc = "/// A bool as GPU memory holds one: four bytes, 0 for false and anything else for true.\n"
               "/// `x as bool32` and `x as bool` convert; a bool itself has no layout.",
        .hlsl = "uint",
        .wgsl = "u32",
        .msl = "uint",
        .hlsl_layout = {.size = 4, .alignment = 4},
        .wgsl_layout = {.size = 4, .alignment = 4},
        .msl_layout = {.size = 4, .alignment = 4},
        .leaf_kind = value_kind::scalar_uint,
        .leaf_count = 1,
    });

    r.add(scalar_of(k_int_family, "/// 32 bits, signed; its arithmetic wraps."));
    add_vectors(r, k_int_family, vectors);
    r.add(scalar_of(k_uint_family, "/// 32 bits, unsigned; its arithmetic wraps."));
    add_vectors(r, k_uint_family, vectors);
    // A builtin enum: `bool.true` is a case like any other, and the record says the targets write it as their bool.
    // `false` comes first, so the zero value of a bool is false.
    auto boolean = scalar_of(k_bool_family, "");
    boolean.declaration = "enum bool:\n    false\n    true";
    r.add(cc::move(boolean));
    add_vectors(r, k_bool_family, vectors);

    r.add_comment("// a vector of one value, beside the constructor of its fields (CHK-360)");
    evaluator const fills[] = {nullptr, nullptr, fill<2>, fill<3>, fill<4>};
    for (auto const& v : vectors)
    {
        auto const name = r.types[index_of(v.id)].declaration;
        auto const start = name.find("struct ") + 7;
        auto const end = name.find(':');
        auto const type = cc::string_view(name).subview({.offset = start, .size = end - start});
        r.add(function_record{
            .signature = cc::format("@pure fun {0}(x: {1}) -> {0}", type, v.element),
            .doc = "/// Every component `x`.",
            .evaluate = fills[v.width],
            .write = {.kind = spelling_kind::custom, .custom = write_filled, .data = u32(index_of(v.id))},
        });
    }
}
