#include <clean-core/container/span.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/from_string.hh>
#include <shaped-shader-library/binding/impl/hlsl_sampler_state.hh>

using namespace cc::primitive_defines;

namespace
{
template <class T>
struct named_value
{
    cc::string_view name;
    T value;
};

constexpr named_value<sg::sampler_filter> k_filters[] = {
    {"nearest", sg::sampler_filter::nearest},
    {"linear", sg::sampler_filter::linear},
};

constexpr named_value<sg::sampler_address_mode> k_address_modes[] = {
    {"repeat", sg::sampler_address_mode::repeat},
    {"mirror_repeat", sg::sampler_address_mode::mirror_repeat},
    {"clamp_edge", sg::sampler_address_mode::clamp_edge},
    {"clamp_border", sg::sampler_address_mode::clamp_border},
    {"mirror_clamp_edge", sg::sampler_address_mode::mirror_clamp_edge},
};

constexpr named_value<sg::sampler_border_color> k_border_colors[] = {
    {"transparent_black", sg::sampler_border_color::transparent_black},
    {"opaque_black", sg::sampler_border_color::opaque_black},
    {"opaque_white", sg::sampler_border_color::opaque_white},
};

constexpr named_value<sg::compare_op> k_compare_ops[] = {
    {"never", sg::compare_op::never},
    {"less", sg::compare_op::less},
    {"equal", sg::compare_op::equal},
    {"less_equal", sg::compare_op::less_equal},
    {"greater", sg::compare_op::greater},
    {"not_equal", sg::compare_op::not_equal},
    {"greater_equal", sg::compare_op::greater_equal},
    {"always", sg::compare_op::always},
};

template <class T>
[[nodiscard]] cc::optional<T> value_of(cc::span<named_value<T> const> table, cc::string_view name)
{
    for (auto const& entry : table)
        if (entry.name == name)
            return entry.value;
    return cc::nullopt;
}
} // namespace

cc::result<sg::sampler> slib::impl::parse_sampler_state(annotation const& attribute)
{
    auto sampler = sg::sampler();
    auto const where = to_string(attribute.location);

    for (auto const& argument : attribute.arguments)
    {
        if (argument.key.empty())
            return cc::error(cc::format("{}: 'static' takes key=value arguments, not '{}'", where, argument.values[0]));

        auto const& key = argument.key;
        auto const& values = argument.values;

        // The two shorthands, and the tuple form that addresses their fields individually.
        // A tuple's order is the order sg::sampler declares the fields, which is why nothing here names them.
        auto const assign_triple
            = [&]<class T>(cc::span<named_value<T> const> table, T& a, T& b, T& c) -> cc::result<cc::unit>
        {
            if (values.size() != 1 && values.size() != 3)
                return cc::error(cc::format("{}: '{}' takes one value or a tuple of three", where, key));

            T parsed[3] = {a, b, c};
            for (isize i = 0; i < values.size(); ++i)
            {
                auto const value = value_of(table, values[i]);
                if (!value.has_value())
                    return cc::error(cc::format("{}: '{}' is not a value of '{}'", where, values[i], key));
                parsed[values.size() == 1 ? 0 : i] = value.value();
            }

            a = parsed[0];
            b = values.size() == 1 ? parsed[0] : parsed[1];
            c = values.size() == 1 ? parsed[0] : parsed[2];
            return cc::unit();
        };

        auto const assign_one = [&]<class T>(cc::span<named_value<T> const> table, T& field) -> cc::result<cc::unit>
        {
            if (values.size() != 1)
                return cc::error(cc::format("{}: '{}' takes exactly one value", where, key));

            auto const value = value_of(table, values[0]);
            if (!value.has_value())
                return cc::error(cc::format("{}: '{}' is not a value of '{}'", where, values[0], key));
            field = value.value();
            return cc::unit();
        };

        auto const assign_float = [&](float& field) -> cc::result<cc::unit>
        {
            if (values.size() != 1)
                return cc::error(cc::format("{}: '{}' takes exactly one value", where, key));

            auto const value = cc::from_string<float>(values[0]);
            if (!value.has_value())
                return cc::error(cc::format("{}: '{}' is not a number", where, values[0]));
            field = value.value();
            return cc::unit();
        };

        // Every branch is braced: CC_RETURN_IF_ERROR expands to several statements, and an unbraced `else if`
        // body would take only the first of them and break the chain after it.
        if (key == "filter")
        {
            CC_RETURN_IF_ERROR(assign_triple(cc::span<named_value<sg::sampler_filter> const>(k_filters),
                                             sampler.min_filter, sampler.mag_filter, sampler.mip_filter));
        }
        else if (key == "address")
        {
            CC_RETURN_IF_ERROR(assign_triple(cc::span<named_value<sg::sampler_address_mode> const>(k_address_modes),
                                             sampler.address_u, sampler.address_v, sampler.address_w));
        }
        else if (key == "min_filter")
        {
            CC_RETURN_IF_ERROR(assign_one(cc::span<named_value<sg::sampler_filter> const>(k_filters), sampler.min_filter));
        }
        else if (key == "mag_filter")
        {
            CC_RETURN_IF_ERROR(assign_one(cc::span<named_value<sg::sampler_filter> const>(k_filters), sampler.mag_filter));
        }
        else if (key == "mip_filter")
        {
            CC_RETURN_IF_ERROR(assign_one(cc::span<named_value<sg::sampler_filter> const>(k_filters), sampler.mip_filter));
        }
        else if (key == "address_u")
        {
            CC_RETURN_IF_ERROR(
                assign_one(cc::span<named_value<sg::sampler_address_mode> const>(k_address_modes), sampler.address_u));
        }
        else if (key == "address_v")
        {
            CC_RETURN_IF_ERROR(
                assign_one(cc::span<named_value<sg::sampler_address_mode> const>(k_address_modes), sampler.address_v));
        }
        else if (key == "address_w")
        {
            CC_RETURN_IF_ERROR(
                assign_one(cc::span<named_value<sg::sampler_address_mode> const>(k_address_modes), sampler.address_w));
        }
        else if (key == "border_color")
        {
            CC_RETURN_IF_ERROR(assign_one(cc::span<named_value<sg::sampler_border_color> const>(k_border_colors),
                                          sampler.border_color));
        }
        else if (key == "compare")
        {
            sg::compare_op op = sg::compare_op::never;
            CC_RETURN_IF_ERROR(assign_one(cc::span<named_value<sg::compare_op> const>(k_compare_ops), op));
            sampler.compare = op;
        }
        else if (key == "mip_lod_bias")
        {
            CC_RETURN_IF_ERROR(assign_float(sampler.mip_lod_bias));
        }
        else if (key == "min_lod")
        {
            CC_RETURN_IF_ERROR(assign_float(sampler.min_lod));
        }
        else if (key == "max_lod")
        {
            CC_RETURN_IF_ERROR(assign_float(sampler.max_lod));
        }
        else if (key == "max_anisotropy")
        {
            if (values.size() != 1)
                return cc::error(cc::format("{}: '{}' takes exactly one value", where, key));

            auto const value = cc::from_string<u32>(values[0]);
            if (!value.has_value() || value.value() == 0)
                return cc::error(cc::format("{}: '{}' is not an anisotropy", where, values[0]));
            sampler.max_anisotropy = value.value();
        }
        else
        {
            return cc::error(cc::format("{}: '{}' is not a field of sg::sampler", where, key));
        }
    }

    return sampler;
}
