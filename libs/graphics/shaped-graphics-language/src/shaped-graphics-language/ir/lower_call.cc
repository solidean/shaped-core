#include <clean-core/string/format.hh>
#include <shaped-graphics-language/builtins/subgroup_calls.hh>
#include <shaped-graphics-language/builtins/texture_calls.hh>
#include <shaped-graphics-language/ir/impl/lowering.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::ir;
using namespace sgl::ir::impl;

namespace
{
/// What a builtin's name says it does; a record's name is its family, then what tells the overloads apart.
enum class family : u8
{
    unknown,
    add,
    subtract,
    multiply,
    divide,
    remainder,
    negate,
    identity,
    bit_and,
    bit_or,
    bit_xor,
    bit_not,
    shift_left,
    shift_right,
    less,
    less_equal,
    greater,
    greater_equal,
    equal,
    not_equal,
    same,
    differs,
    any,
    all,
    select,
    dot,
    transpose,
    is_nan,
    is_inf,
    saturate,
    count_bits,
    reverse_bits,
    first_bit_high,
    matrix_column,
    matrix_row,
    transform,
    transform_position,
    transform_direction,
    constructor,
    convert,
    reinterpret,
    math_float,
    math_any_sign,
    derivative_x,
    derivative_y,
    derivative_width,
};

struct family_name
{
    cc::string_view stem;
    family what;
    math fn = math::fabs;
};

// Longest stems first where one starts another; a record's name is its stem, alone or followed by `_`.
constexpr family_name k_families[] = {
    {"add", family::add},
    {"translate_back", family::subtract},
    {"translate", family::add},
    {"subtract", family::subtract},
    {"multiply", family::multiply},
    {"prescale", family::multiply},
    {"unscale", family::divide},
    {"scale", family::multiply},
    {"divide", family::divide},
    {"remainder", family::remainder},
    {"negate", family::negate},
    {"identity", family::identity},
    {"uniform", family::identity},
    {"nonuniform", family::identity},
    {"enum_as_int", family::identity},
    {"bit_and", family::bit_and},
    {"bit_or", family::bit_or},
    {"bit_xor", family::bit_xor},
    {"bit_not", family::bit_not},
    {"shift_left", family::shift_left},
    {"shift_right", family::shift_right},
    {"less_equal", family::less_equal},
    {"less", family::less},
    {"greater_equal", family::greater_equal},
    {"greater", family::greater},
    {"equal", family::equal},
    {"not_equal", family::not_equal},
    {"same", family::same},
    {"differs", family::differs},
    {"any", family::any},
    {"all", family::all},
    {"select", family::select},
    {"dot", family::dot},
    {"transpose", family::transpose},
    {"is_nan", family::is_nan},
    {"is_inf", family::is_inf},
    {"saturate", family::saturate},
    {"count_bits", family::count_bits},
    {"reverse_bits", family::reverse_bits},
    {"first_bit_high", family::first_bit_high},
    {"matrix_column", family::matrix_column},
    {"matrix_row", family::matrix_row},
    {"transform_position", family::transform_position},
    {"transform_direction3", family::transform},
    {"transform_direction", family::transform_direction},
    {"transform", family::transform},
    {"convert", family::convert},
    {"reinterpret_as", family::reinterpret},
    {"abs", family::math_any_sign, math::fabs},
    {"sign", family::math_any_sign, math::fsign},
    {"min", family::math_any_sign, math::fmin},
    {"max", family::math_any_sign, math::fmax},
    {"clamp", family::math_any_sign, math::fclamp},
    {"round", family::math_float, math::round_even},
    {"trunc", family::math_float, math::trunc},
    {"floor", family::math_float, math::floor},
    {"ceil", family::math_float, math::ceil},
    {"fract", family::math_float, math::fract},
    {"sinh", family::math_float, math::sinh},
    {"cosh", family::math_float, math::cosh},
    {"ddx", family::derivative_x},
    {"ddy", family::derivative_y},
    {"fwidth", family::derivative_width},
    {"tanh", family::math_float, math::tanh},
    {"asin", family::math_float, math::asin},
    {"acos", family::math_float, math::acos},
    {"atan2", family::math_float, math::atan2},
    {"atan", family::math_float, math::atan},
    {"sin", family::math_float, math::sin},
    {"cos", family::math_float, math::cos},
    {"tan", family::math_float, math::tan},
    {"pow", family::math_float, math::pow},
    {"exp2", family::math_float, math::exp2},
    {"exp", family::math_float, math::exp},
    {"log2", family::math_float, math::log2},
    {"log", family::math_float, math::log},
    {"inverse_sqrt", family::math_float, math::inverse_sqrt},
    {"sqrt", family::math_float, math::sqrt},
    {"determinant", family::math_float, math::determinant},
    {"mix", family::math_float, math::fmix},
    {"step", family::math_float, math::step},
    {"smoothstep", family::math_float, math::smooth_step},
    {"fma", family::math_float, math::fma},
    {"mul_add", family::math_float, math::fma},
    {"pack_snorm4x8", family::math_float, math::pack_snorm4x8},
    {"pack_unorm4x8", family::math_float, math::pack_unorm4x8},
    {"pack_half2x16", family::math_float, math::pack_half2x16},
    {"unpack_half2x16", family::math_float, math::unpack_half2x16},
    {"unpack_snorm4x8", family::math_float, math::unpack_snorm4x8},
    {"unpack_unorm4x8", family::math_float, math::unpack_unorm4x8},
    {"length", family::math_float, math::length},
    {"distance", family::math_float, math::distance},
    {"cross", family::math_float, math::cross},
    {"normalize", family::math_float, math::normalize},
    {"reflect", family::math_float, math::reflect},
    {"refract", family::math_float, math::refract},
    {"first_bit_low", family::math_float, math::find_ilsb},
    // EMIT-256: what core.sgl calls for a double where the API is vulkan
    {"spirv_trunc", family::math_float, math::trunc},
    {"spirv_floor", family::math_float, math::floor},
    {"spirv_ceil", family::math_float, math::ceil},
    {"spirv_round", family::math_float, math::round_even},
    {"spirv_fract", family::math_float, math::fract},
    {"spirv_sqrt", family::math_float, math::sqrt},
    {"spirv_inverse_sqrt", family::math_float, math::inverse_sqrt},
};

/// The position in `k_families` of the record called `name`, or -1.
[[nodiscard]] i32 find_family(cc::string_view name)
{
    auto best = -1;
    auto best_size = isize(-1);
    for (auto i = 0; i < i32(sizeof(k_families) / sizeof(k_families[0])); ++i)
    {
        auto const stem = k_families[i].stem;
        if (stem.size() <= best_size || !name.starts_with(stem))
            continue;
        if (name.size() == stem.size() || name[stem.size()] == '_')
        {
            best = i;
            best_size = stem.size();
        }
    }
    return best;
}

/// Parallel to the registry's functions: what each record's name says, found once for the life of the process.
[[nodiscard]] cc::vector<i16> const& families_of(builtins::registry const& registry)
{
    static auto const table = [&]
    {
        auto result = cc::vector<i16>::create_filled(registry.functions.size(), i16(-1));
        for (auto i = isize(0); i < registry.functions.size(); ++i)
            result[i] = i16(find_family(registry.functions[i].name));
        return result;
    }();
    return table;
}

/// A record whose name is its result type's: `float3(x, y, z)`, `uint(x)`.
[[nodiscard]] bool is_constructor(builtins::registry const& registry, builtins::function_record const& record)
{
    return sgl::is_valid(record.result) && registry.at(record.result).name == record.name;
}
} // namespace

type_ref lowering::scalar_of(type_ref t) const
{
    auto const info = _out.at(t);
    return info.kind == type_kind::vector ? info.element
         : info.kind == type_kind::matrix ? _out.at(info.element).element
                                          : t;
}

u32 lowering::lanes_of(type_ref t) const
{
    auto const info = _out.at(t);
    return info.kind == type_kind::vector ? info.count : 1;
}

value_ref lowering::column_wise(op code, value_ref a, value_ref b, type_ref matrix)
{
    auto const info = _out.at(matrix);
    auto const column = info.element;
    auto columns = cc::vector<u32>();
    for (auto i = u32(0); i < info.count; ++i)
    {
        auto const left = extracted(a, column, i);
        columns.push_back(
            u32(is_valid(b) ? add(code, column, {left, extracted(b, column, i)}) : add(code, column, {left})));
    }
    return add(op::construct, matrix, columns);
}

value_ref lowering::converted(value_ref value, type_ref from, type_ref to)
{
    if (from == to)
        return value;
    auto const source = _out.at(scalar_of(from));
    auto const target = _out.at(scalar_of(to));
    auto const target_scalar = scalar_of(to);
    if (source.kind == type_kind::bool_)
    {
        auto const one
            = target.kind == type_kind::float_ ? float_constant(target_scalar, 1.0) : constant_of(target_scalar, 1u);
        auto const zero
            = target.kind == type_kind::float_ ? float_constant(target_scalar, 0.0) : constant_of(target_scalar, 0u);
        return add(op::select, to, {value, splat(one, to), splat(zero, to)});
    }
    if (target.kind == type_kind::bool_)
    {
        auto const zero = source.kind == type_kind::float_ ? float_constant(scalar_of(from), 0.0)
                                                           : constant_of(scalar_of(from), 0u);
        return add(source.kind == type_kind::float_ ? op::fne : op::ine, to, {value, splat(zero, from)});
    }
    if (source.kind == type_kind::float_ && target.kind == type_kind::float_)
        return add(op::f_resize, to, {value});
    if (source.kind == type_kind::float_)
        return add(target.is_signed ? op::f_to_s : op::f_to_u, to, {value});
    if (target.kind == type_kind::float_)
        return add(source.is_signed ? op::s_to_f : op::u_to_f, to, {value});
    if (source.width == target.width)
        return add(op::bitcast, to, {value});
    if (source.is_signed)
        return add(op::s_resize, to, {value});
    // SPIR-V's zero extension gives an unsigned integer alone
    if (!target.is_signed)
        return add(op::u_resize, to, {value});
    auto const unsigned_scalar = intern({.kind = type_kind::int_, .width = target.width});
    auto const widened = add(op::u_resize, vector_type(unsigned_scalar, lanes_of(to)), {value});
    return add(op::bitcast, to, {widened});
}

value_ref lowering::constructed(cc::span<value_ref const> args, type_ref result)
{
    auto const info = _out.at(result);
    if (args.size() == 1)
    {
        auto const from = _out.type_of(args[0]);
        if (info.kind == type_kind::vector && _out.at(from).kind != type_kind::vector)
            return splat(converted(args[0], from, info.element), result);
        return converted(args[0], from, result);
    }
    auto operands = cc::vector<u32>();
    for (auto const a : args)
        operands.push_back(u32(a));
    return add(op::construct, result, operands);
}

value_ref lowering::arithmetic(u32 shape, cc::span<value_ref const> args, type_ref result)
{
    auto const what = family(shape);
    auto a = args[0];
    auto b = args.size() > 1 ? args[1] : value_ref::none;
    auto const a_type = _out.type_of(a);
    auto const b_type = is_valid(b) ? _out.type_of(b) : a_type;
    auto const is_a_matrix = _out.at(a_type).kind == type_kind::matrix;
    auto const is_b_matrix = _out.at(b_type).kind == type_kind::matrix;

    // the matrix cases, which SPIR-V's componentwise operations do not take
    if (what == family::multiply && (is_a_matrix || is_b_matrix))
    {
        if (is_a_matrix && is_b_matrix)
            return add(op::matrix_times_matrix, result, {a, b});
        if (is_a_matrix)
            return add(op::matrix_times_scalar, result, {a, b});
        return add(op::matrix_times_scalar, result, {b, a});
    }
    if (is_a_matrix && (what == family::add || what == family::subtract))
        return column_wise(what == family::add ? op::fadd : op::fsub, a, b, result);
    if (is_a_matrix && what == family::negate)
        return column_wise(op::fneg, a, value_ref::none, result);

    // a scalar beside a vector stands for that vector of it
    auto const wide = lanes_of(a_type) >= lanes_of(b_type) ? a_type : b_type;
    if (lanes_of(a_type) < lanes_of(wide))
        a = splat(a, wide);
    if (is_valid(b) && lanes_of(b_type) < lanes_of(wide))
        b = splat(b, wide);

    auto const scalar = _out.at(scalar_of(wide));
    auto const is_float = scalar.kind == type_kind::float_;
    auto const is_bool = scalar.kind == type_kind::bool_;
    auto const is_signed = scalar.is_signed;
    auto const bools = vector_type(_bool, lanes_of(wide));
    auto const compared = [&](op f, op s, op u) { return add(is_float ? f : is_signed ? s : u, bools, {a, b}); };
    auto const every = [&](value_ref v, op folded) { return lanes_of(wide) == 1 ? v : add(folded, _bool, {v}); };
    auto const equals
        = [&]() { return is_bool ? add(op::logical_eq, bools, {a, b}) : compared(op::feq, op::ieq, op::ieq); };
    auto const differs
        = [&]() { return is_bool ? add(op::logical_ne, bools, {a, b}) : compared(op::fne, op::ine, op::ine); };
    auto const shift_count = [&]
    {
        // a count past the width is masked (EVAL-84), where SPIR-V leaves it undefined
        auto const count_scalar = scalar_of(wide == result ? _out.type_of(b) : wide);
        auto const mask = constant_of(count_scalar, u32(scalar.width - 1));
        return add(op::bit_and, _out.type_of(b), {b, splat(mask, _out.type_of(b))});
    };

    switch (what)
    {
    case family::add:
        return add(is_float ? op::fadd : op::iadd, result, {a, b});
    case family::subtract:
        return add(is_float ? op::fsub : op::isub, result, {a, b});
    case family::multiply:
        return add(is_float ? op::fmul : op::imul, result, {a, b});
    case family::divide:
        return add(is_float ? op::fdiv : is_signed ? op::sdiv : op::udiv, result, {a, b});
    case family::remainder:
        return add(is_float ? op::frem : is_signed ? op::srem : op::urem, result, {a, b});
    case family::negate:
        return add(is_float ? op::fneg : op::ineg, result, {a});
    case family::bit_and:
        return add(is_bool ? op::logical_and : op::bit_and, result, {a, b});
    case family::bit_or:
        return add(is_bool ? op::logical_or : op::bit_or, result, {a, b});
    case family::bit_xor:
        return add(is_bool ? op::logical_ne : op::bit_xor, result, {a, b});
    case family::bit_not:
        return add(is_bool ? op::logical_not : op::bit_not, result, {a});
    case family::shift_left:
        return add(op::shift_left, result, {a, shift_count()});
    case family::shift_right:
        return add(is_signed ? op::shift_right_arithmetic : op::shift_right_logical, result, {a, shift_count()});
    case family::less:
        return compared(op::flt, op::slt, op::ult);
    case family::less_equal:
        return compared(op::fle, op::sle, op::ule);
    case family::greater:
        return compared(op::fgt, op::sgt, op::ugt);
    case family::greater_equal:
        return compared(op::fge, op::sge, op::uge);
    case family::equal:
        return equals();
    case family::not_equal:
        return differs();
    case family::same:
        return every(equals(), op::all);
    case family::differs:
        return every(differs(), op::any);
    default:
        break;
    }
    return value_ref::none;
}

value_ref lowering::atomic(flat_call const& c, flat_expr const& x)
{
    auto const arguments = cc::vector<flat_expr_id>::create_copy_of(cc::span<flat_expr_id const>(_e.at(c.arguments)));
    auto const& record = *_m.builtin_function(c.intrinsic);
    // an `@atomic` image's `texel_max(i, xy, v)`, which the flat tree writes for `i[xy].max(v)` (CHK-373)
    auto const is_texel = record.is_atomic && cc::string_view(record.name).starts_with("texel_");
    auto const texel_shape = check::texture_shape((record.write.data >> 12) & 15u);
    auto const is_layered
        = texel_shape == check::texture_shape::d1_array || texel_shape == check::texture_shape::d2_array;
    // where the atomic's own operands begin: after the image, the coordinate and an array's layer
    auto const first = !is_texel ? isize(1) : is_layered ? isize(3) : isize(2);
    auto const texel_coordinate = [&]() -> value_ref
    {
        auto const xy = expr(arguments[1]);
        if (!is_layered)
            return xy;
        auto const xy_type = _out.type_of(xy);
        return add(op::construct, vector_type(scalar_of(xy_type), lanes_of(xy_type) + 1), {xy, expr(arguments[2])});
    };
    auto const texel = is_texel ? texel_of(arguments[0], texel_coordinate()) : cc::optional<place>();
    auto const target = is_texel ? texel : place_of(arguments[0]);
    if (!target.has_value())
    {
        unsupported(cc::format("the atomic {} of something that is no place", record.name));
        return value_ref::none;
    }
    auto const pointer = target.value().pointer;
    auto const held = _out.at(_out.type_of(pointer)).element;
    // by value: lowering an argument below interns types, which moves `types`
    auto const scalar = _out.at(held);
    auto const is_float = scalar.kind == type_kind::float_;
    // a texel is its image's scalar, where a buffer's element is an `atomic[T]` that may hold a float as bits
    auto const declared_float
        = !is_texel && _m.at(_m.at(_e.at(arguments[0]).type).element).kind == check::type_kind::structure
       && _m.builtin_type_of(_m.at(_e.at(arguments[0]).type).element) != nullptr
       && _m.builtin_type_of(_m.at(_e.at(arguments[0]).type).element)->leaf_kind == value_kind::scalar_float;
    auto const name = cc::string_view(record.name).subview(is_texel ? 6 : 0);
    auto const value = arguments.size() > first ? expr(arguments[first]) : value_ref::none;
    // a float held as a uint's bits, which every target but vulkan's with a device float add keeps it as
    if (declared_float && !is_float)
    {
        if (name == "load")
            return add(op::bitcast, _float, {add(op::atomic_load, held, {pointer})});
        if (name == "store")
            return add(op::atomic_store, type_ref::none, {pointer, add(op::bitcast, held, {value})});
        if (name == "exchange")
            return add(op::bitcast, _float, {add(op::atomic_exchange, held, {pointer, add(op::bitcast, held, {value})})});
        if (name != "add")
        {
            unsupported(cc::format("the float atomic {} on a device that has no float add", name));
            return value_ref::none;
        }
        // the add retries a compare-exchange of the bits until no other update came between, as the text's does
        auto const expected = add_local(held, "atomic_expected");
        add(op::store, type_ref::none, {expected, add(op::atomic_load, held, {pointer})});
        auto const header = new_block();
        auto const body = new_block();
        auto const next = new_block();
        auto const merge = new_block();
        branch(header);
        open_block(header);
        set_merge(merge_kind::loop, merge, next);
        branch(body);
        open_block(body);
        auto const guess = add(op::load, held, {expected});
        auto const sum = add(op::fadd, _float, {add(op::bitcast, _float, {guess}), value});
        auto const before = add(op::atomic_compare_exchange, held, {pointer, add(op::bitcast, held, {sum}), guess});
        add(op::store, type_ref::none, {expected, before});
        branch_if(add(op::ieq, _bool, {before, guess}), merge, next);
        open_block(next);
        branch(header);
        open_block(merge);
        return add(op::bitcast, _float, {add(op::load, held, {expected})});
    }
    auto const sign = scalar.is_signed;
    if (name == "load")
        return add(op::atomic_load, held, {pointer});
    if (name == "store")
        return add(op::atomic_store, type_ref::none, {pointer, value});
    if (name == "exchange")
        return add(op::atomic_exchange, held, {pointer, value});
    if (name.starts_with("compare_exchange"))
    {
        auto const desired = expr(arguments[first + 1]);
        auto const old = add(op::atomic_compare_exchange, held, {pointer, desired, value});
        auto const result = value_type(x.type);
        if (result == held)
            return old;
        auto const stored = add(is_float ? op::feq : op::ieq, _bool, {old, value});
        return add(op::construct, result, {old, stored});
    }
    if (is_float)
    {
        if (name == "add")
            return add(op::atomic_fadd, held, {pointer, value});
        if (name == "subtract")
            return add(op::atomic_fadd, held, {pointer, add(op::fneg, held, {value})});
        unsupported(cc::format("the float atomic {}", name));
        return value_ref::none;
    }
    auto const code = name == "add"      ? op::atomic_iadd
                    : name == "subtract" ? op::atomic_isub
                    : name == "min"      ? (sign ? op::atomic_smin : op::atomic_umin)
                    : name == "max"      ? (sign ? op::atomic_smax : op::atomic_umax)
                    : name == "bit_and"  ? op::atomic_and
                    : name == "bit_or"   ? op::atomic_or
                                         : op::atomic_xor;
    if (name != "add" && name != "subtract" && name != "min" && name != "max" && name != "bit_and" && name != "bit_or"
        && name != "bit_xor")
    {
        unsupported(cc::format("the atomic {}", name));
        return value_ref::none;
    }
    return add(code, held, {pointer, value});
}

value_ref lowering::bytes_call(flat_call const& c, builtins::function_record const& record)
{
    auto const arguments = cc::vector<flat_expr_id>::create_copy_of(cc::span<flat_expr_id const>(_e.at(c.arguments)));
    auto const* const bound = _e.at(arguments[0]).node.try_as<flat_binding_member>();
    auto const memory = bound != nullptr ? bound_resource(emit::impl::resource_of(_plan, bound->binding, bound->member))
                                         : bound_element(arguments[0]);
    if (!is_valid(memory))
    {
        unsupported("bytes of a binding array");
        return value_ref::none;
    }
    auto const uniformity = nonuniform_of(memory);
    auto const offset = expr(arguments[1]);
    auto const first = add(op::shift_right_logical, _uint, {offset, uint_constant(2)});
    auto const pointer_to = pointer_type(_uint, storage::storage_buffer);
    auto const word_at = [&](i32 i)
    {
        auto const index = i == 0 ? first : add(op::iadd, _uint, {first, uint_constant(u32(i))});
        return add(op::access_chain, pointer_to, {memory, uint_constant(0), index}, 0, uniformity);
    };
    if (record.is_bytes_store)
    {
        auto const value = expr(arguments[2]);
        for (auto i = 0; i < record.bytes_words; ++i)
            add(op::store, type_ref::none, {word_at(i), extracted(value, _uint, u32(i))});
        return value_ref::none;
    }
    if (record.bytes_words == 1)
        return add(op::load, _uint, {word_at(0)});
    auto words = cc::vector<u32>();
    for (auto i = 0; i < record.bytes_words; ++i)
        words.push_back(u32(add(op::load, _uint, {word_at(i)})));
    return add(op::construct, vector_type(_uint, u32(record.bytes_words)), words);
}

value_ref lowering::texture_call(flat_call const& c, builtins::function_record const& record, type_ref result)
{
    using builtins::texture::op;
    auto const arguments = cc::vector<flat_expr_id>::create_copy_of(cc::span<flat_expr_id const>(_e.at(c.arguments)));
    auto const shape = builtins::texture::call::unpack(record.write.data);
    auto const layout = builtins::texture::layout_of(shape);
    auto const axes = builtins::texture::axes_of(shape.where);

    auto const resource_at = [&](flat_expr_id id) -> value_ref
    {
        auto const& node = _e.at(id).node;
        if (auto const* const smp = node.try_as<flat_file_sampler>())
            return bound_sampler(emit::impl::sampler_of(_plan, smp->sampler));
        auto const* const bound = node.try_as<flat_binding_member>();
        auto const pointer = bound != nullptr
                               ? bound_resource(emit::impl::resource_of(_plan, bound->binding, bound->member))
                               : bound_element(id);
        if (!is_valid(pointer))
            unsupported("a texture of a binding array");
        return pointer;
    };
    auto const texture = resource_at(arguments[0]);
    if (!is_valid(texture))
        return value_ref::none;
    auto const image_ref = _out.at(_out.type_of(texture)).element;
    auto const texture_uniformity = nonuniform_of(texture);
    auto const image = add(ir::op::load, image_ref, {texture}, 0, texture_uniformity);
    auto const scalar = _out.at(image_ref).element;
    auto const texels = vector_type(scalar, 4);

    // what a read gives is four lanes, of which the call wants its own
    auto const narrowed = [&](value_ref four) -> value_ref
    {
        auto const lanes = lanes_of(result);
        if (lanes == 4)
            return four;
        if (lanes == 1)
            return extracted(four, result, 0);
        auto operands = cc::vector<u32>();
        operands.push_back(u32(four));
        operands.push_back(u32(four));
        for (auto i = u32(0); i < lanes; ++i)
            operands.push_back(i);
        return add(ir::op::shuffle, result, operands);
    };
    // a coordinate with its layer, as SPIR-V takes one vector of both
    auto const coordinate = [&](value_ref xy) -> value_ref
    {
        if (!axes.is_array || layout.layer < 0)
            return xy;
        auto const xy_type = _out.type_of(xy);
        auto layer = expr(arguments[layout.layer]);
        layer = converted(layer, _out.type_of(layer), scalar_of(xy_type));
        return add(ir::op::construct, vector_type(scalar_of(xy_type), lanes_of(xy_type) + 1), {xy, layer});
    };

    // EMIT-253: a `sample` or a `gather` takes its offset last, and nothing else states one
    auto const takes_offset = shape.what == op::sample || shape.what == op::gather;
    auto const with_offset = [&](std::initializer_list<value_ref> operands)
    {
        auto list = cc::vector<u32>();
        for (auto const v : operands)
            list.push_back(u32(v));
        if (layout.offset < 0)
            return list;
        // CHK-280 makes it a constant, which SPIR-V names as one and DXIL takes as immediates
        auto const offset = expr(arguments[layout.offset]);
        auto is_known = is_constant(offset);
        if (!is_known && is_operation(offset) && _out.instructions[u32(offset)].code == ir::op::construct)
        {
            is_known = true;
            for (auto const part : _out.operands_of(_out.instructions[u32(offset)]))
                is_known = is_known && is_constant(value_ref(part));
        }
        if (!is_known)
            unsupported("an offset that lowers to no constant");
        list.push_back(u32(offset));
        return list;
    };
    auto const sampler_handle = [&]() -> value_ref
    {
        if (layout.offset >= 0 && !takes_offset)
        {
            unsupported("a sample with an offset");
            return value_ref::none;
        }
        auto const sampler = resource_at(arguments[layout.sampler]);
        if (!is_valid(sampler))
            return value_ref::none;
        return add(ir::op::load, _out.at(_out.type_of(sampler)).element, {sampler}, 0, nonuniform_of(sampler));
    };
    // what every call through a sampler reads, combined last as SPIR-V has it next to its use
    auto const with_image = [&](value_ref sampler)
    {
        auto const& loaded = _out.instructions[index_of(sampler)];
        return add(ir::op::combine_sampler, intern({.kind = type_kind::sampled_image, .element = image_ref}),
                   {image, sampler}, 0, u8(texture_uniformity | loaded.aux));
    };

    switch (shape.what)
    {
    case op::load:
    {
        auto const at = coordinate(expr(arguments[1]));
        auto const level = expr(arguments[layout.first]);
        return narrowed(add(ir::op::image_fetch, texels, {image, at, level}, 0, axes.is_ms ? 1 : 0));
    }
    case op::sample_level:
    {
        auto const sampler = sampler_handle();
        if (!is_valid(sampler))
            return value_ref::none;
        auto const at = coordinate(expr(arguments[1]));
        auto level = expr(arguments[layout.first]);
        level = converted(level, _out.type_of(level), _float);
        return narrowed(add(ir::op::sample_lod, texels, {with_image(sampler), at, level}));
    }
    case op::sample:
    {
        auto const sampler = sampler_handle();
        if (!is_valid(sampler))
            return value_ref::none;
        auto const at = coordinate(expr(arguments[1]));
        return narrowed(add(ir::op::sample_implicit, texels, with_offset({with_image(sampler), at})));
    }
    case op::sample_bias:
    {
        auto const sampler = sampler_handle();
        if (!is_valid(sampler))
            return value_ref::none;
        auto const at = coordinate(expr(arguments[1]));
        auto const bias = expr(arguments[layout.first]);
        return narrowed(add(ir::op::sample_bias, texels, {with_image(sampler), at, bias}));
    }
    case op::sample_grad:
    {
        auto const sampler = sampler_handle();
        if (!is_valid(sampler))
            return value_ref::none;
        auto const at = coordinate(expr(arguments[1]));
        auto const x = expr(arguments[layout.first]);
        auto const y = expr(arguments[layout.second]);
        return narrowed(add(ir::op::sample_grad, texels, {with_image(sampler), at, x, y}));
    }
    case op::gather:
    {
        auto const sampler = sampler_handle();
        if (!is_valid(sampler))
            return value_ref::none;
        auto const at = coordinate(expr(arguments[1]));
        auto const component = layout.first >= 0 ? expr(arguments[layout.first]) : int_constant(0);
        return add(ir::op::image_gather, texels, with_offset({with_image(sampler), at, component}));
    }
    case op::sample_compare:
    {
        auto const sampler = sampler_handle();
        if (!is_valid(sampler))
            return value_ref::none;
        auto const at = coordinate(expr(arguments[1]));
        auto const reference = expr(arguments[layout.first]);
        return add(ir::op::sample_compare, result, {with_image(sampler), at, reference});
    }
    case op::sample_compare_level:
    {
        auto const sampler = sampler_handle();
        if (!is_valid(sampler))
            return value_ref::none;
        // HLSL's `SampleCmpLevelZero`, whatever the level's constant says (CHK-280)
        auto const at = coordinate(expr(arguments[1]));
        auto const reference = expr(arguments[layout.first]);
        return add(ir::op::sample_compare_zero, result,
                   {with_image(sampler), at, reference, float_constant(_float, 0.0)});
    }
    case op::gather_compare:
    {
        auto const sampler = sampler_handle();
        if (!is_valid(sampler))
            return value_ref::none;
        auto const at = coordinate(expr(arguments[1]));
        auto const reference = expr(arguments[layout.first]);
        return add(ir::op::image_gather_compare, texels, {with_image(sampler), at, reference});
    }
    case op::image_load:
        return narrowed(add(ir::op::image_read, texels, {image, coordinate(expr(arguments[1]))}));
    case op::image_store:
    {
        auto const at = coordinate(expr(arguments[1]));
        auto value = expr(arguments[layout.first]);
        auto const lanes = lanes_of(_out.type_of(value));
        if (lanes != 4)
        {
            auto const zero
                = _out.at(scalar).kind == type_kind::float_ ? float_constant(scalar, 0.0) : constant_of(scalar, 0u);
            auto parts = cc::vector<u32>();
            for (auto i = u32(0); i < 4; ++i)
                parts.push_back(u32(i < lanes ? extracted(value, scalar, i) : zero));
            value = add(ir::op::construct, texels, parts);
        }
        add(ir::op::image_write, type_ref::none, {image, at, value});
        return value_ref::none;
    }
    case op::size:
    case op::layer_count:
    case op::image_size:
    case op::image_layer_count:
    {
        auto const is_storage = shape.what == op::image_size || shape.what == op::image_layer_count;
        auto const lanes = u32(axes.is_cube ? 2 : axes.dim) + (axes.is_array ? 1u : 0u);
        auto const whole = vector_type(_int, lanes);
        auto const asks_level = !is_storage && !axes.is_ms;
        auto const level = !asks_level                                 ? value_ref::none
                         : shape.what == op::size && layout.first >= 0 ? expr(arguments[layout.first])
                                                                       : int_constant(0);
        auto const queried = asks_level ? add(ir::op::image_size, whole, {image, level}, 0, 1)
                                        : add(ir::op::image_size, whole, {image});
        auto const is_layers = shape.what == op::layer_count || shape.what == op::image_layer_count;
        if (is_layers)
            return extracted(queried, _int, lanes - 1);
        auto const wanted = lanes_of(result);
        if (wanted == lanes)
            return queried;
        if (wanted == 1)
            return extracted(queried, _int, 0);
        auto operands = cc::vector<u32>();
        operands.push_back(u32(queried));
        operands.push_back(u32(queried));
        for (auto i = u32(0); i < wanted; ++i)
            operands.push_back(i);
        return add(ir::op::shuffle, result, operands);
    }
    case op::level_count:
        return add(ir::op::image_levels, _int, {image});
    case op::sample_count:
        return add(ir::op::image_samples, _int, {image});
    default:
        unsupported(cc::format("the texture call {}", record.name));
        return value_ref::none;
    }
}

value_ref lowering::subgroup_call(flat_call const& c, builtins::function_record const& record, type_ref result)
{
    using builtins::subgroup_op;
    auto const arguments = cc::vector<flat_expr_id>::create_copy_of(cc::span<flat_expr_id const>(_e.at(c.arguments)));
    auto const what = subgroup_op(record.write.data);
    auto const value = expr(arguments[0]);
    auto const type = _out.type_of(value);
    auto const scalar = _out.at(scalar_of(type));
    auto const is_float = scalar.kind == type_kind::float_;
    auto const sign = scalar.is_signed;
    // an id SPIR-V reads as unsigned
    auto const as_uint = [&](value_ref v)
    {
        if (is_constant(v))
            return uint_constant(_out.constants[index_of(v)].low);
        return _out.type_of(v) == _uint ? v : add(op::bitcast, _uint, {v});
    };
    auto const reduce = [&](subgroup f, subgroup i, group_fold fold)
    { return add(op::subgroup, result, {value}, u8(is_float ? f : i), u8(fold)); };
    auto const with = [&](subgroup s, value_ref id) { return add(op::subgroup, result, {value, as_uint(id)}, u8(s)); };

    switch (what)
    {
    case subgroup_op::all:
        return add(op::subgroup, result, {value}, u8(subgroup::all));
    case subgroup_op::any:
        return add(op::subgroup, result, {value}, u8(subgroup::any));
    case subgroup_op::ballot:
        return add(op::subgroup, result, {value}, u8(subgroup::ballot));
    case subgroup_op::add:
        return reduce(subgroup::fadd, subgroup::iadd, group_fold::reduce);
    case subgroup_op::mul:
        return reduce(subgroup::fmul, subgroup::imul, group_fold::reduce);
    case subgroup_op::min:
        return reduce(subgroup::fmin, sign ? subgroup::smin : subgroup::umin, group_fold::reduce);
    case subgroup_op::max:
        return reduce(subgroup::fmax, sign ? subgroup::smax : subgroup::umax, group_fold::reduce);
    case subgroup_op::bit_and:
        return reduce(subgroup::bit_and, subgroup::bit_and, group_fold::reduce);
    case subgroup_op::bit_or:
        return reduce(subgroup::bit_or, subgroup::bit_or, group_fold::reduce);
    case subgroup_op::bit_xor:
        return reduce(subgroup::bit_xor, subgroup::bit_xor, group_fold::reduce);
    case subgroup_op::exclusive_add:
        return reduce(subgroup::fadd, subgroup::iadd, group_fold::exclusive);
    case subgroup_op::exclusive_mul:
        return reduce(subgroup::fmul, subgroup::imul, group_fold::exclusive);
    case subgroup_op::inclusive_add:
        return reduce(subgroup::fadd, subgroup::iadd, group_fold::inclusive);
    case subgroup_op::inclusive_mul:
        return reduce(subgroup::fmul, subgroup::imul, group_fold::inclusive);
    case subgroup_op::broadcast:
        return with(subgroup::broadcast, expr(arguments[1]));
    case subgroup_op::broadcast_first:
        return add(op::subgroup, result, {value}, u8(subgroup::broadcast_first));
    case subgroup_op::shuffle:
        return with(subgroup::shuffle, expr(arguments[1]));
    case subgroup_op::shuffle_xor:
        return with(subgroup::shuffle_xor, expr(arguments[1]));
    case subgroup_op::shuffle_up:
        return with(subgroup::shuffle_up, expr(arguments[1]));
    case subgroup_op::shuffle_down:
        return with(subgroup::shuffle_down, expr(arguments[1]));
    case subgroup_op::quad_swap_x:
        return with(subgroup::quad_swap, uint_constant(0));
    case subgroup_op::quad_swap_y:
        return with(subgroup::quad_swap, uint_constant(1));
    case subgroup_op::quad_swap_diagonal:
        return with(subgroup::quad_swap, uint_constant(2));
    case subgroup_op::quad_broadcast:
        return with(subgroup::quad_broadcast, expr(arguments[1]));
    }
    return value_ref::none;
}

value_ref lowering::affine_call(builtins::function_record const& record, cc::span<value_ref const> args, type_ref result)
{
    if (record.name == "affine3_of_rows")
        return add(op::construct, result, {args[0], args[1], args[2]});
    auto const four = vector_type(_float, 4);
    if (record.name == "affine3_row")
    {
        if (is_constant(args[1]))
            return extracted(args[0], four, _out.constants[index_of(args[1])].low);
        auto const variable = add_local(_out.type_of(args[0]), "affine");
        add(op::store, type_ref::none, {variable, args[0]});
        u32 const chain[] = {u32(variable), u32(args[1])};
        return add(op::load, four, {add(op::access_chain, pointer_type(four, storage::function), chain)});
    }
    // `a * v`: each row dotted with `v`, with its w where the call gives none
    auto v = args[1];
    if (lanes_of(_out.type_of(v)) == 3)
        v = add(op::construct, four, {v, float_constant(_float, record.name == "transform_position_affine" ? 1.0 : 0.0)});
    u32 lanes[3];
    for (auto row = u32(0); row < 3; ++row)
        lanes[row] = u32(add(op::dot, _float, {extracted(args[0], four, row), v}));
    return add(op::construct, result, lanes);
}

value_ref lowering::call(flat_expr const& x, flat_call const& c)
{
    if (c.text >= 0)
    {
        unsupported("a body in a target's own text");
        return value_ref::none;
    }
    auto const* const found = _m.builtin_function(c.intrinsic);
    if (found == nullptr)
    {
        unsupported("a call of something that is no builtin");
        return value_ref::none;
    }
    auto const& record = *found;
    if (_e.entry_stage == stage::geometry && (record.name == "emit" || record.name == "end_strip"))
    {
        stream_call(c, record.name == "emit");
        return value_ref::none;
    }
    if (_e.entry_stage == stage::mesh && !_e.at(c.arguments).empty()
        && _m.at(_e.at(_e.at(c.arguments)[0]).type).kind == check::type_kind::stream)
    {
        mesh_call(c, record.name);
        return value_ref::none;
    }
    if (_e.entry_stage == stage::task && record.name == "dispatch_mesh")
    {
        dispatch_mesh(c);
        return value_ref::none;
    }
    if (record.name == "trace_ray")
    {
        trace_ray(c);
        return value_ref::none;
    }
    if (record.name == "call_callable")
    {
        call_callable(c);
        return value_ref::none;
    }
    // `hit_object_to_world_row` and `hit_object_ray_origin` are the hit's own, which a stage reads with no hit object
    auto const is_hit_stage_read = record.name.ends_with("_row") || record.name.starts_with("hit_object_ray_");
    if ((record.name.starts_with("hit_object_") && !is_hit_stage_read) || record.name.starts_with("trace_hit_ray")
        || record.name.starts_with("trace_ray_reordered") || record.name == "reorder_thread_by_hint")
        return hit_object_call(x, c, record);
    if (record.name == "convert_bool_to_bool32")
        return add(op::select, _uint, {expr(_e.at(c.arguments)[0]), uint_constant(1), uint_constant(0)});
    if (record.name == "convert_bool32_to_bool")
        return add(op::ine, _bool, {expr(_e.at(c.arguments)[0]), uint_constant(0)});
    auto const result = x.type == checked_module::void_type ? _void : value_type(x.type);
    if (record.is_atomic)
        return atomic(c, x);
    if (record.bytes_words > 0)
        return bytes_call(c, record);
    if (record.is_texture_call)
        return texture_call(c, record, result);
    if (record.is_subgroup_operation)
        return subgroup_call(c, record, result);
    if (record.is_ray_query)
        return ray_query_call(c, record);
    if (record.is_barrier || record.is_uniform_load)
    {
        auto const kind = record.is_uniform_load || record.name == "workgroup_barrier" ? barrier::workgroup
                        : record.name == "storage_barrier"                             ? barrier::storage
                        : record.name == "texture_barrier"                             ? barrier::texture
                                                                                       : barrier(0xff);
        if (u8(kind) == 0xff)
        {
            unsupported(cc::format("the barrier {}", record.name));
            return value_ref::none;
        }
        add(op::control_barrier, type_ref::none, cc::span<u32 const>(), u8(kind));
        if (!record.is_uniform_load)
            return value_ref::none;
        // EMIT-149: no thread stores to the memory before every one has read it
        auto const memory = place_of(_e.at(c.arguments)[0]);
        if (!memory.has_value())
        {
            unsupported("a uniform load of something that is no place");
            return value_ref::none;
        }
        auto const value = load(memory.value());
        add(op::control_barrier, type_ref::none, cc::span<u32 const>(), u8(barrier::workgroup));
        return value;
    }

    auto args = cc::vector<value_ref>();
    for (auto const a : cc::vector<flat_expr_id>::create_copy_of(cc::span<flat_expr_id const>(_e.at(c.arguments))))
        args.push_back(expr(a));
    if (!_failure.empty())
        return value_ref::none;

    auto const& registry = *_m.builtins;
    if (is_constructor(registry, record))
        return constructed(args, result);

    if (record.name.starts_with("affine3_") || record.name.ends_with("_affine"))
        return affine_call(record, args, result);
    if (is_ray_stage() && record.features.has(check::feature::raytracing_pipeline))
        return ray_stage_call(record, args, result);

    auto const position = families_of(registry)[index_of(c.intrinsic)];
    if (position < 0)
    {
        unsupported(cc::format("the builtin {}", record.name));
        return value_ref::none;
    }
    auto const& name = k_families[position];
    switch (name.what)
    {
    case family::identity:
        return args[0];
    case family::convert:
        return converted(args[0], _out.type_of(args[0]), result);
    case family::reinterpret:
    {
        auto const from = _out.type_of(args[0]);
        auto const from_width = _out.at(scalar_of(from)).width;
        auto const to_width = _out.at(scalar_of(result)).width;
        if (from_width == to_width)
            return add(op::bitcast, result, {args[0]});
        // CHK-517: a double and its two words, as many bits either way
        if (from_width * u32(lanes_of(from)) == to_width * u32(lanes_of(result)))
            return add(op::bitcast, result, {args[0]});
        // a half's bits in the low half of a uint, and back
        auto const narrow = intern({.kind = type_kind::int_, .width = 16});
        if (from_width == 16 && to_width == 32)
            return add(op::u_resize, _uint, {add(op::bitcast, narrow, {args[0]})});
        if (from_width == 32 && to_width == 16)
            return add(op::bitcast, result, {add(op::u_resize, narrow, {args[0]})});
        unsupported(cc::format("the builtin {}", record.name));
        return value_ref::none;
    }
    case family::any:
        return lanes_of(_out.type_of(args[0])) == 1 ? args[0] : add(op::any, _bool, {args[0]});
    case family::all:
        return lanes_of(_out.type_of(args[0])) == 1 ? args[0] : add(op::all, _bool, {args[0]});
    case family::select:
        return add(op::select, result, {args[0], args[1], args[2]});
    case family::dot:
        return add(op::dot, result, {args[0], args[1]});
    case family::transpose:
        return add(op::transpose, result, {args[0]});
    case family::is_nan:
        return add(op::is_nan, result, {args[0]});
    case family::is_inf:
        return add(op::is_inf, result, {args[0]});
    case family::count_bits:
        return add(op::bit_count, result, {args[0]});
    case family::reverse_bits:
        return add(op::bit_reverse, result, {args[0]});
    case family::first_bit_high:
        return add(op::math, result, {args[0]},
                   u8(_out.at(scalar_of(result)).is_signed ? math::find_smsb : math::find_umsb));
    case family::saturate:
    {
        auto const scalar = scalar_of(result);
        auto const zero = splat(float_constant(scalar, 0.0), result);
        auto const one = splat(float_constant(scalar, 1.0), result);
        return add(op::math, result, {args[0], zero, one}, u8(math::fclamp));
    }
    case family::matrix_column:
    case family::matrix_row:
    {
        auto matrix = args[0];
        auto const matrix_type = _out.type_of(matrix);
        if (name.what == family::matrix_row)
            matrix = add(op::transpose, matrix_type, {matrix});
        if (is_constant(args[1]))
            return extracted(matrix, result, _out.constants[index_of(args[1])].low);
        auto const variable = add_local(matrix_type, "matrix");
        add(op::store, type_ref::none, {variable, matrix});
        u32 const chain[] = {u32(variable), u32(args[1])};
        return add(op::load, result, {add(op::access_chain, pointer_type(result, storage::function), chain)});
    }
    case family::derivative_x:
        return add(op::derivative, result, {args[0]}, u8(derivative::x));
    case family::derivative_y:
        return add(op::derivative, result, {args[0]}, u8(derivative::y));
    case family::derivative_width:
        return add(op::derivative, result, {args[0]}, u8(derivative::width));
    case family::transform:
        if (_out.at(_out.type_of(args[0])).kind != type_kind::matrix)
            break;
        return add(op::matrix_times_vector, result, {args[0], args[1]});
    case family::transform_position:
    case family::transform_direction:
    {
        if (_out.at(_out.type_of(args[0])).kind != type_kind::matrix)
            break;
        auto const lifted = vector_type(_float, 4);
        auto const w = float_constant(_float, name.what == family::transform_position ? 1.0 : 0.0);
        auto const product = add(op::matrix_times_vector, lifted, {args[0], add(op::construct, lifted, {args[1], w})});
        if (result == lifted)
            return product;
        u32 const operands[] = {u32(product), u32(product), 0u, 1u, 2u};
        return add(op::shuffle, result, operands);
    }
    case family::math_float:
    case family::math_any_sign:
    {
        auto fn = name.fn;
        if (name.what == family::math_any_sign)
        {
            auto const scalar = _out.at(scalar_of(_out.type_of(args[0])));
            auto const step = scalar.kind == type_kind::float_ ? 0 : scalar.is_signed ? 1 : 2;
            if (fn == math::fabs || fn == math::fsign)
            {
                if (step == 2)
                {
                    unsupported(cc::format("the builtin {} of an unsigned value", record.name));
                    return value_ref::none;
                }
                fn = math(u8(fn) + step);
            }
            else
                fn = fn == math::fmin ? (step == 0   ? math::fmin
                                         : step == 1 ? math::smin
                                                     : math::umin)
                   : fn == math::fmax ? (step == 0   ? math::fmax
                                         : step == 1 ? math::smax
                                                     : math::umax)
                                      : (step == 0   ? math::fclamp
                                         : step == 1 ? math::sclamp
                                                     : math::uclamp);
        }
        // a scalar beside vectors stands for a vector of it, where the call gives one;
        // an unpack's word is one scalar whatever its result, and refract's ratio is one too
        auto const takes_scalar = fn == math::refract || fn == math::unpack_half2x16 || fn == math::unpack_snorm4x8
                               || fn == math::unpack_unorm4x8;
        auto operands = cc::vector<u32>();
        auto const lanes = lanes_of(result);
        for (auto const a : args)
        {
            auto const is_short = lanes > 1 && lanes_of(_out.type_of(a)) == 1 && !takes_scalar;
            operands.push_back(u32(is_short ? splat(a, result) : a));
        }
        return add(op::math, result, operands, u8(fn));
    }
    default:
        return arithmetic(u32(name.what), args, result);
    }
    unsupported(cc::format("the builtin {}", record.name));
    return value_ref::none;
}
