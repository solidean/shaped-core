#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh> // cc::memcpy
#include <clean-core/string/string.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/scene/mesh_attribute.hh> // attribute_format + attribute_format_of

#include <type_traits>

/// One value a mesh parameter carries — a scalar or a vector, stored inline, never an allocation.
///
/// It is typed by the same `attribute_format` an attribute's elements are, so a material describes what it wants once and reads either the same way.
/// Construct with `of(value)` and read with `as<T>()`; reading a type the value does not hold asserts, and `holds<T>()` is the question a material asks first.
struct sv::parameter_value
{
    /// Inline payload budget for one value: 4 components of 8 bytes.
    ///
    /// That covers every scalar and every vector, whatever their scalar type.
    /// A matrix does not fit and is rejected at construction — raising the budget is all that stands in the way, since `attribute_format` already describes one.
    static constexpr i32 max_bytes = 32;

    attribute_format format = attribute_format::of_scalar(scalar_type::f32);

    /// the value's bytes, in its own layout — only the leading `format.size_bytes()` of them are live
    byte storage[max_bytes] = {};

    /// The value of any scalar or vector element type — whatever `attribute_format_of` names.
    /// A matrix is rejected: it does not fit the inline budget.
    template <class T>
    [[nodiscard]] static parameter_value of(T const& value)
    {
        static_assert(std::is_trivially_copyable_v<T>, "a parameter value is stored by copying its bytes");
        static_assert(sizeof(T) <= max_bytes, "parameter value is too large to store inline");
        CC_ASSERT(!attribute_format_of<T>.is_matrix(), "matrix parameters need out-of-line storage, which does not "
                                                       "exist yet");

        auto r = parameter_value{.format = attribute_format_of<T>};
        cc::memcpy(r.storage, &value, sizeof(T));
        return r;
    }

    /// Does this value hold a T? What a material checks before reading one.
    template <class T>
    [[nodiscard]] bool holds() const
    {
        return format == attribute_format_of<T>;
    }

    /// The value read back as T, which must be the type `format` names.
    template <class T>
    [[nodiscard]] T as() const
    {
        CC_ASSERT(holds<T>(), "parameter holds a different type");
        auto value = T{};
        cc::memcpy(&value, storage, sizeof(T));
        return value;
    }
};

/// A per-mesh (per-instance) value the material reads by name — a tint, a fade, a slice plane's offset.
///
/// This is how two meshes share one material and still draw differently, without either owning a material of its own.
/// The material decides which names it knows; an unknown one is ignored, and a missing one takes the material's default.
struct sv::mesh_parameter
{
    cc::string name;
    parameter_value value;
};
