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

    /// Which column of a builtin's record this target reads: how a type is named and how a call is written.
    [[nodiscard]] virtual builtins::language language() const = 0;

    /// True when `name(a, b)` builds a value of a struct; false when a local must be declared and its members assigned.
    [[nodiscard]] virtual bool has_struct_constructor() const = 0;

    /// One line without indentation and without a line break: `let n: vec3f = normalize(p.normal);`.
    /// A mutable local without a value is declared and nothing else: `float x;`, and `var x: f32;`, which WGSL zeroes.
    virtual void write_local(cc::string& out, local_declaration const& local) const = 0;

    /// One line without indentation and without a line break, for a value that is evaluated and dropped: `saturate(x);`.
    /// WGSL refuses a bare call of a function whose value must be used, so there it is `_ = saturate(x);`.
    virtual void write_eval(cc::string& out, cc::string_view value) const = 0;

    /// True for a target with C's control flow: `if (c)` with the brace on a line of its own, `while (true)`, and
    /// `do { … } while (false);` for a `once`.
    /// False for WGSL: `if c {`, `loop {`, and a `once` that is `loop { … break; }`.
    [[nodiscard]] virtual bool is_c_like() const = 0;

    /// The head of a `for` over an int range, without the brace: `for (int i = 0; i < n; ++i)`.
    virtual void write_for_head(cc::string& out, cc::string_view index, cc::string_view first, cc::string_view end) const
        = 0;

    /// One constant of an enum, without indentation and with its line break: `static const int light_kind_point = 0;`.
    virtual void write_enum_constant(cc::string& out, cc::string_view name, i32 value) const = 0;

    /// The structs of `p.structs` and the constant block, each followed by an empty line.
    virtual void write_declarations(cc::string& out, plan const& p) const = 0;

    /// How the body names a resource, which is the bare global everywhere but HLSL, where it stands in a namespace.
    [[nodiscard]] virtual cc::string resource_reference(planned_resource const& b) const { return b.name; }

    /// How a group's constant block is named where a member is read through it.
    [[nodiscard]] virtual cc::string block_reference(planned_constants const& b) const { return b.name; }

    /// The resources of one binding, which is one group: HLSL wraps them, and WGSL writes each with its own address.
    /// One group: its constant block when it has one, then its resources; never called for a group with neither.
    virtual void write_group(cc::string& out,
                             plan const& p,
                             planned_constants const* block,
                             cc::span<planned_resource const> buffers) const = 0;

    /// How the target spells a texture, an image or a sampler type, as a helper's parameter declares it.
    [[nodiscard]] virtual cc::string resource_text(plan const& p, check::type_id type) const = 0;

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

/// The constants of every enum of `p`, each set followed by an empty line; a dialect calls it from its declarations.
void write_enum_constants(cc::string& out, plan const& p, dialect const& d);
/// The helpers the entry point's builtin calls need, each once, ahead of the function.
void write_helpers(cc::string& out, plan const& p, dialect const& d);
/// Every resource of the entry point, handed to the dialect one binding at a time.
void write_buffers(cc::string& out, plan const& p, dialect const& d);
/// True when the entry point calls a builtin that takes derivatives implicitly: a sample that picks its own level.
[[nodiscard]] bool uses_derivatives(plan const& p);

/// The whole text of the planned entry point: a header comment, the declarations, and the function.
/// Mints what the body still needs from `p.names`.
[[nodiscard]] cc::string write_text(plan& p, dialect const& d);

// Each defined by its target's file.
[[nodiscard]] dialect const& hlsl_dx12_dialect();
[[nodiscard]] dialect const& hlsl_vulkan_dialect();
[[nodiscard]] dialect const& wgsl_dialect();
[[nodiscard]] dialect const& msl_dialect();
} // namespace sgl::emit::impl
