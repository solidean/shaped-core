#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/raster/raster_pipeline.hh>
#include <shaped-shader-library/fwd.hh>

/// What the generated `impl/pipeline_fields.hh` is written against: the shape of its tables, and the one helper its
/// setters share.
namespace slib::impl
{
/// How a field's value arrives from a reloaded setting.
enum class field_kind : u8
{
    boolean,
    integer,
    real,
    enum_case,
    /// `blend = .none`, the one setting that is no leaf.
    none,
};

/// One case of an sg enum, by the name the prelude mirrors it with.
struct enum_case
{
    cc::string_view name;
    int value = 0;
};

struct named_enum
{
    cc::string_view name;
    cc::span<enum_case const> cases;
};

/// A reloaded setting's value: 0 or 1 for a boolean, an integer, or an enum case's value; `real` for a float.
struct field_value
{
    i64 integer = 0;
    f64 real = 0;
};

/// One field by its path, `*` standing for a target, and how a value is written to it.
struct field
{
    cc::string_view path;
    field_kind kind = field_kind::boolean;
    cc::span<enum_case const> cases;
    /// `target` is the index into `color_targets` a `*` path names, and unused otherwise.
    void (*set)(sg::raster_pipeline_description& d, int target, field_value value) = nullptr;
};

/// The blend of a target, switched on with sg's defaults the first time one of its fields is written.
inline sg::blend_state& engaged(sg::color_target_state& target)
{
    if (!target.blend.has_value())
        target.blend = sg::blend_state{};
    return target.blend.value();
}
} // namespace slib::impl
