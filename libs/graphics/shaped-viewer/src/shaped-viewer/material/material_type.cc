#include "material_type.hh"

#include <clean-core/container/byte_stream_builder.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-viewer/impl/content_hash.hh>

namespace sv
{
namespace
{
/// Whether `name` is a plain C identifier — the only shape `shader_generator` can paste into HLSL as a local.
[[nodiscard]] bool is_identifier(cc::string_view name)
{
    if (name.empty())
        return false;
    if (!((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= 'a' && name[0] <= 'z') || name[0] == '_'))
        return false;
    for (auto const c : name)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    return true;
}

/// Whether `name` spells a builtin numeric type: a scalar name, optionally followed by `N` or `NxM`.
/// Derived rather than listed, so `float3` and `min16uint4x4` are covered without enumerating a few hundred spellings.
[[nodiscard]] bool is_builtin_type(cc::string_view name)
{
    constexpr cc::string_view scalars[]
        = {"bool",       "int",        "uint",     "dword",    "half",      "float",     "double",
           "min16float", "min10float", "min16int", "min12int", "min16uint", "float16_t", "float32_t",
           "float64_t",  "int16_t",    "int32_t",  "int64_t",  "uint16_t",  "uint32_t",  "uint64_t"};
    auto const digit = [](char c) { return c >= '1' && c <= '4'; };
    for (auto const s : scalars)
    {
        if (!name.starts_with(s))
            continue;
        auto const rest = name.subview(s.size());
        if (rest.empty() || (rest.size() == 1 && digit(rest[0]))
            || (rest.size() == 3 && digit(rest[0]) && rest[1] == 'x' && digit(rest[2])))
            return true;
    }
    return false;
}

/// Everything else HLSL will not let a local be called.
/// Object types are in here rather than in `is_builtin_type` because they take no dimension suffix.
[[nodiscard]] bool is_keyword(cc::string_view name)
{
    constexpr cc::string_view keywords[] = {"asm",
                                            "auto",
                                            "break",
                                            "case",
                                            "catch",
                                            "cbuffer",
                                            "centroid",
                                            "char",
                                            "class",
                                            "column_major",
                                            "compile",
                                            "const",
                                            "continue",
                                            "default",
                                            "delete",
                                            "discard",
                                            "do",
                                            "else",
                                            "enum",
                                            "explicit",
                                            "extern",
                                            "false",
                                            "for",
                                            "friend",
                                            "goto",
                                            "groupshared",
                                            "if",
                                            "in",
                                            "inline",
                                            "inout",
                                            "interface",
                                            "linear",
                                            "long",
                                            "matrix",
                                            "mutable",
                                            "namespace",
                                            "new",
                                            "nointerpolation",
                                            "noperspective",
                                            "operator",
                                            "out",
                                            "packoffset",
                                            "pass",
                                            "precise",
                                            "private",
                                            "protected",
                                            "public",
                                            "register",
                                            "return",
                                            "row_major",
                                            "sample",
                                            "sampler",
                                            "shared",
                                            "short",
                                            "signed",
                                            "sizeof",
                                            "snorm",
                                            "static",
                                            "string",
                                            "struct",
                                            "switch",
                                            "tbuffer",
                                            "technique",
                                            "template",
                                            "this",
                                            "throw",
                                            "true",
                                            "try",
                                            "typedef",
                                            "typename",
                                            "uniform",
                                            "union",
                                            "unorm",
                                            "unsigned",
                                            "using",
                                            "vector",
                                            "virtual",
                                            "void",
                                            "volatile",
                                            "while",
                                            "AppendStructuredBuffer",
                                            "Buffer",
                                            "ByteAddressBuffer",
                                            "ConsumeStructuredBuffer",
                                            "ConstantBuffer",
                                            "RaytracingAccelerationStructure",
                                            "RWBuffer",
                                            "RWByteAddressBuffer",
                                            "RWStructuredBuffer",
                                            "RWTexture1D",
                                            "RWTexture1DArray",
                                            "RWTexture2D",
                                            "RWTexture2DArray",
                                            "RWTexture3D",
                                            "SamplerComparisonState",
                                            "SamplerState",
                                            "StructuredBuffer",
                                            "Texture1D",
                                            "Texture1DArray",
                                            "Texture2D",
                                            "Texture2DArray",
                                            "Texture2DMS",
                                            "Texture2DMSArray",
                                            "Texture3D",
                                            "TextureCube",
                                            "TextureCubeArray"};
    for (auto const k : keywords)
        if (name == k)
            return true;
    return false;
}
} // namespace

material_type material_type::create(cc::string name, cc::vector<material_signature_entry> signature, cc::string shader)
{
    auto& b = cc::byte_stream_builder::thread_local_scratch();
    b.add_string(name);
    b.add_string(shader);
    b.add_pod(i64(signature.size()));
    for (auto const& d : signature)
    {
        b.add_string(d.name);
        b.add_pod(d.format);
        b.add_bool(d.is_final);
        b.add_pod(d.interpolation);
        b.add_pod_span_sized(cc::span<byte const>(d.default_value));
    }

    auto type = material_type{.name = cc::move(name),
                              .signature = cc::move(signature),
                              .shader = cc::move(shader),
                              .hash = cc::hash128::create(b.written_bytes(), impl::material_type_hash_seed)};

    for (auto i = 0; i < type.signature.size(); ++i)
        for (auto j = i + 1; j < type.signature.size(); ++j)
            CC_ASSERT(type.signature[i].name != type.signature[j].name, "a material type declares each attribute once");

    for (auto const& d : type.signature)
        CC_ASSERT(d.default_value.size() == d.format.size_bytes(), "a declaration's default must be exactly its "
                                                                   "format's size");

    for (auto const& d : type.signature)
        CC_ASSERT(d.interpolation != attribute_interpolation::rotation
                      || d.format == attribute_format::of_vector(scalar_type::f32, 4),
                  "an attribute interpolated as a rotation must be a 4-component f32 — a quaternion in xyzw order");

    // A name is pasted into generated HLSL as a local, and a material type's own fragment is written against it — so a name
    // the generator cannot emit is rejected rather than sanitized, since mangling it would silently break that fragment.
    for (auto const& d : type.signature)
    {
        CC_ASSERT(is_identifier(d.name),
                  "a material attribute name must be a plain identifier: a letter or underscore, "
                  "then letters, digits and underscores");
        CC_ASSERT(!is_builtin_type(d.name) && !is_keyword(d.name),
                  "a material attribute may not be named after an HLSL "
                  "keyword or builtin type");
        CC_ASSERT(!cc::string_view(d.name).starts_with("sv_"),
                  "the sv_ prefix belongs to the generator (sv_sampler_*, and "
                  "the entry function itself); a declared attribute may not "
                  "use it");
        CC_ASSERT(d.name != "surface" && d.name != "ctx", "'surface' and 'ctx' are the entry function's own locals");
    }

    return type;
}

material_signature_entry const* material_type::find(cc::string_view name) const
{
    for (auto const& d : signature)
        if (d.name == name)
            return &d;
    return nullptr;
}
} // namespace sv
