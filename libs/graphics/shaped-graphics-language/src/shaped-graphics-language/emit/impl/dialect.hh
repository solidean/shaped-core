#pragma once

#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/emit/impl/plan.hh>

namespace sgl::emit::impl
{
/// One level of indentation in every target's text.
constexpr cc::string_view k_indent = "    ";

/// A local as its declaration needs it; every name and type is already spelled for the target.
struct local_declaration
{
    cc::string_view name;
    cc::string_view type;
    /// Empty for a local that is declared now and filled member by member afterwards.
    cc::string_view value;
    bool is_mut = false;
};

/// What one target spells differently from the others.
///
/// The walk over the flat tree is `write_text`, and it is the same for every target.
/// A new target is one more dialect: its declarations, its signature, and the few answers below.
/// A dialect holds no state, so one value serves every call.
class dialect
{
public:
    /// As the header comment names the target: "HLSL for dx12".
    [[nodiscard]] virtual cc::string_view description() const = 0;

    /// `b` must be a builtin type.
    [[nodiscard]] virtual cc::string_view type_name(check::builtin b) const = 0;

    /// `b` must be a builtin function that every target writes as a plain call: `normalize`, `dot`, `saturate`.
    [[nodiscard]] virtual cc::string_view function_name(check::builtin b) const = 0;

    /// True when a matrix times a vector is `mul(m, v)`; false when it is `m * v`.
    [[nodiscard]] virtual bool has_mul_function() const = 0;

    /// True when `name(a, b)` builds a value of a struct; false when a local must be declared and its members assigned.
    [[nodiscard]] virtual bool has_struct_constructor() const = 0;

    /// One line without indentation and without a line break: `let n: vec3f = normalize(p.normal);`.
    virtual void write_local(cc::string& out, local_declaration const& local) const = 0;

    /// The structs of `p.structs` and the constant block, each followed by an empty line.
    virtual void write_declarations(cc::string& out, plan const& p) const = 0;

    /// Everything of the function up to and including the line that opens its body.
    virtual void write_function_head(cc::string& out, plan const& p) const = 0;

protected:
    ~dialect() = default;
};

[[nodiscard]] dialect const& dialect_of(target t);

/// The name of a type as `d` writes it: a builtin's spelling, or the planned name of a struct of the program.
[[nodiscard]] cc::string_view type_text(plan const& p, dialect const& d, check::type_id type);

/// "vertex" or "pixel", as SGL names the stage.
[[nodiscard]] cc::string_view stage_name(check::stage s);

/// The whole text of the planned entry point: a header comment, the declarations, and the function.
/// Mints what the body still needs from `p.names`.
[[nodiscard]] cc::string write_text(plan& p, dialect const& d);

// Each defined by its target's file.
[[nodiscard]] dialect const& hlsl_dx12_dialect();
[[nodiscard]] dialect const& hlsl_vulkan_dialect();
[[nodiscard]] dialect const& wgsl_dialect();
[[nodiscard]] dialect const& msl_dialect();
} // namespace sgl::emit::impl
