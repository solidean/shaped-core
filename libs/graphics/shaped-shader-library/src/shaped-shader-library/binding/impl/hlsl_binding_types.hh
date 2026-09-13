#pragma once

#include <clean-core/error/optional.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-shader-library/fwd.hh>

namespace slib::impl
{
/// What one HLSL resource type means to the pass: where its register lives, and what sg calls the thing.
///
/// This table is the single most important piece of shared state in the design, because the rewriter and the
/// build-time generator must agree on it exactly — a divergence binds a resource to the wrong descriptor, and
/// nothing downstream can notice.
/// It is checked against DXC rather than trusted: a generated layout compares itself against the compiled
/// shader's reflected bindings.
struct hlsl_binding_type
{
    char register_class = 't'; ///< the DXIL register class letter: 't', 'u', 'b' or 's'
    sg::binding_type type = sg::binding_type::readonly_texture;
    cc::optional<sg::texture_view_dimension> dimension; ///< set for texture types, absent for everything else
};

/// The table entry for one HLSL type name, or nothing when the pass does not know the type.
/// `hlsl_type` is the bare name — `Texture2D`, not `Texture2D<float4>` — since the template arguments say what
/// the resource holds, never where it is bound.
[[nodiscard]] cc::optional<hlsl_binding_type> binding_type_of(cc::string_view hlsl_type);
} // namespace slib::impl
