#include "writer.hh"

#include <clean-core/common/assert.hh>
#include <shaped-graphics-language/check/resources.hh>

using namespace sgl;
using namespace sgl::ir;
using namespace sgl::ir::spirv::impl;

namespace
{
/// SPIR-V's name of the `check::k_image_formats` it names, as `spirv` of that table spells it.
struct format_number
{
    cc::string_view spelled;
    u32 number;
    bool is_extended;
};
constexpr format_number k_formats[] = {
    {"rgba32f", 1, false},    {"rgba16f", 2, false},  {"r32f", 3, false},     {"rgba8", 4, false},
    {"rgba8snorm", 5, false}, {"rg32f", 6, true},     {"rg16f", 7, true},     {"r11g11b10f", 8, true},
    {"r16f", 9, true},        {"rgb10a2", 11, true},  {"rg8", 13, true},      {"r8", 15, true},
    {"rg8snorm", 18, true},   {"r8snorm", 20, true},  {"rgba32i", 21, false}, {"rgba16i", 22, false},
    {"rgba8i", 23, false},    {"r32i", 24, false},    {"rg32i", 25, true},    {"rg16i", 26, true},
    {"rg8i", 27, true},       {"r16i", 28, true},     {"r8i", 29, true},      {"rgba32ui", 30, false},
    {"rgba16ui", 31, false},  {"rgba8ui", 32, false}, {"r32ui", 33, false},   {"rg32ui", 35, true},
    {"rg16ui", 36, true},     {"rg8ui", 37, true},    {"r16ui", 38, true},    {"r8ui", 39, true},
};

/// SPIR-V's `BuiltIn` of each stage input.
struct built_in_number
{
    built_in input = built_in::none;
    u32 number = 0;
    bool is_subgroup = false;
};
constexpr built_in_number k_built_ins[] = {
    {built_in::thread_id, 28},
    {built_in::local_thread_id, 27},
    {built_in::local_thread_index, 29},
    {built_in::workgroup_id, 26},
    {built_in::subgroup_size, 36, true},
    {built_in::subgroup_invocation_id, 41, true},
    {built_in::position, 0},
    {built_in::vertex_index, 42},
    {built_in::instance_index, 43},
    {built_in::first_instance, 4425},
    {built_in::is_front_facing, 17},
    {built_in::sample_index, 18},
    {built_in::sample_mask, 20},
    {built_in::primitive_id, 7},
    {built_in::view_index, 4440},
    {built_in::layer, 9},
    {built_in::depth, 22},
    {built_in::depth_greater_equal, 22},
    {built_in::depth_less_equal, 22},
    {built_in::tess_edge, 11},
    {built_in::tess_inside, 12},
    {built_in::domain_location, 13},
    {built_in::launch_id, 5319},
    {built_in::launch_size, 5320},
    {built_in::world_ray_origin, 5321},
    {built_in::world_ray_direction, 5322},
    {built_in::object_ray_origin, 5323},
    {built_in::object_ray_direction, 5324},
    {built_in::ray_t_min, 5325},
    {built_in::ray_t_current, 5326},
    {built_in::ray_instance_id, 5327},
    {built_in::ray_instance_index, 6},
    {built_in::ray_geometry_index, 5352},
    {built_in::ray_primitive_index, 7},
    {built_in::hit_kind, 5333},
    {built_in::object_to_world, 5330},
    {built_in::world_to_object, 5331},
    {built_in::ray_cluster_id, 5436},
};
/// A pixel stage reads `@position` as what the rasterizer made of it, where a vertex stage's is `Position`.
constexpr u32 k_frag_coord = 15;

/// `InvocationId`.
constexpr u32 k_invocation_id = 8;
/// `TessLevelOuter` and `TessLevelInner` are always this long, whatever the domain writes of them.
constexpr u32 k_outer_factors = 4;
constexpr u32 k_inner_factors = 2;

constexpr u32 k_model_vertex = 0;
constexpr u32 k_model_tessellation_control = 1;
constexpr u32 k_model_tessellation_evaluation = 2;
constexpr u32 k_model_geometry = 3;
constexpr u32 k_model_fragment = 4;
constexpr u32 k_model_compute = 5;
constexpr u32 k_model_raygen = 5313;
constexpr u32 k_model_intersection = 5314;
constexpr u32 k_model_any_hit = 5315;
constexpr u32 k_model_closest_hit = 5316;
constexpr u32 k_model_miss = 5317;
constexpr u32 k_model_callable = 5318;
constexpr u32 k_callable_data = 5328;
constexpr u32 k_incoming_callable_data = 5329;
constexpr u32 k_ray_payload = 5338;
constexpr u32 k_hit_attribute = 5339;
constexpr u32 k_incoming_ray_payload = 5342;
constexpr u32 k_model_task = 5364;
constexpr u32 k_model_mesh = 5365;
constexpr u32 k_mode_origin_upper_left = 7;
constexpr u32 k_mode_depth_replacing = 12;
constexpr u32 k_mode_depth_greater = 14;
constexpr u32 k_mode_depth_less = 15;
constexpr u32 k_mode_local_size = 17;
constexpr u32 k_mode_invocations = 0;
constexpr u32 k_mode_spacing_equal = 1;
constexpr u32 k_mode_spacing_fractional_even = 2;
constexpr u32 k_mode_spacing_fractional_odd = 3;
constexpr u32 k_mode_vertex_order_cw = 4;
constexpr u32 k_mode_vertex_order_ccw = 5;
constexpr u32 k_mode_input_points = 19;
constexpr u32 k_mode_input_lines = 20;
constexpr u32 k_mode_input_lines_adjacency = 21;
constexpr u32 k_mode_triangles = 22;
constexpr u32 k_mode_input_triangles_adjacency = 23;
constexpr u32 k_mode_quads = 24;
constexpr u32 k_mode_isolines = 25;
constexpr u32 k_mode_output_vertices = 26;
constexpr u32 k_mode_output_points = 27;
constexpr u32 k_mode_output_line_strip = 28;
constexpr u32 k_mode_output_triangle_strip = 29;
constexpr u32 k_mode_output_lines = 5269;
constexpr u32 k_mode_output_primitives = 5270;
constexpr u32 k_mode_output_triangles = 5298;
/// `PrimitiveLineIndicesEXT` and `PrimitiveTriangleIndicesEXT`.
constexpr u32 k_primitive_line_indices = 5295;
constexpr u32 k_primitive_triangle_indices = 5296;

[[nodiscard]] constexpr bool is_factor(built_in b)
{
    return b == built_in::tess_edge || b == built_in::tess_inside;
}
} // namespace

u32 writer::storage_of(storage s) const
{
    switch (s)
    {
    case storage::function:
        return spv::FunctionStorage;
    case storage::workgroup:
        return spv::Workgroup;
    case storage::uniform:
        return spv::Uniform;
    case storage::push_constant:
        return spv::PushConstant;
    case storage::storage_buffer:
        return spv::StorageBuffer;
    case storage::resource:
        return spv::UniformConstant;
    case storage::task_payload:
        return spv::TaskPayloadWorkgroupEXT;
    case storage::ray_input:
        return _p.stage == check::stage::callable ? k_incoming_callable_data : k_incoming_ray_payload;
    case storage::hit_attribute:
        return k_hit_attribute;
    case storage::ray_output:
        return k_ray_payload;
    case storage::callable_output:
        return k_callable_data;
    case storage::image:
        return spv::Image;
    case storage::table:
        return spv::Private;
    }
    return spv::FunctionStorage;
}

bool writer::holds_16_bit(type_ref t) const
{
    auto const& info = _p.at(t);
    switch (info.kind)
    {
    case type_kind::int_:
    case type_kind::float_:
        return info.width == 16;
    case type_kind::vector:
    case type_kind::matrix:
    case type_kind::array:
    case type_kind::runtime_array:
        return holds_16_bit(info.element);
    case type_kind::structure:
        for (auto i = u32(0); i < info.member_count; ++i)
            if (holds_16_bit(_p.members[info.first_member + i].type))
                return true;
        return false;
    default:
        return false;
    }
}

void writer::write_types()
{
    require(spv::Shader);
    for (auto i = isize(0); i < _p.types.size(); ++i)
        if (_p.types[i].kind == type_kind::int_ && _p.types[i].width == 32 && !_p.types[i].is_signed)
            _uint = type_ref(i);

    for (auto i = isize(0); i < _p.types.size(); ++i)
    {
        auto const& t = _p.types[i];
        auto const id = _types + u32(i);
        switch (t.kind)
        {
        case type_kind::void_:
            emit(_declarations, spv::TypeVoid, {id});
            _void = type_ref(i);
            break;
        case type_kind::bool_:
            emit(_declarations, spv::TypeBool, {id});
            break;
        case type_kind::int_:
            if (t.width == 16)
                require(spv::Int16);
            if (t.width == 64)
                require(spv::Int64);
            emit(_declarations, spv::TypeInt, {id, u32(t.width), t.is_signed ? 1u : 0u});
            break;
        case type_kind::float_:
            if (t.width == 16)
                require(spv::Float16);
            if (t.width == 64)
                require(spv::Float64);
            emit(_declarations, spv::TypeFloat, {id, u32(t.width)});
            break;
        case type_kind::vector:
            emit(_declarations, spv::TypeVector, {id, id_of(t.element), t.count});
            break;
        case type_kind::matrix:
            emit(_declarations, spv::TypeMatrix, {id, id_of(t.element), t.count});
            break;
        case type_kind::array:
            emit(_declarations, spv::TypeArray, {id, id_of(t.element), literal_id(t.count)});
            if (t.stride > 0)
                emit(_annotations, spv::Decorate, {id, spv::ArrayStride, u32(t.stride)});
            break;
        case type_kind::runtime_array:
            emit(_declarations, spv::TypeRuntimeArray, {id, id_of(t.element)});
            // none is an unbounded binding array, of which the descriptors are the elements
            if (t.stride > 0)
                emit(_annotations, spv::Decorate, {id, spv::ArrayStride, u32(t.stride)});
            else
            {
                require(spv::RuntimeDescriptorArray);
                require_extension("SPV_EXT_descriptor_indexing");
            }
            break;
        case type_kind::structure:
            write_structure(t, id);
            break;
        case type_kind::pointer:
            if (holds_16_bit(t.element))
                require(t.space == storage::uniform          ? spv::UniformAndStorageBuffer16BitAccess
                        : t.space == storage::push_constant  ? spv::StoragePushConstant16
                        : t.space == storage::storage_buffer ? spv::StorageBuffer16BitAccess
                                                             : spv::Shader);
            emit(_declarations, spv::TypePointer, {id, storage_of(t.space), id_of(t.element)});
            break;
        case type_kind::image:
            write_image(t, id);
            break;
        case type_kind::sampler:
            emit(_declarations, spv::TypeSampler, {id});
            break;
        case type_kind::sampled_image:
            emit(_declarations, spv::TypeSampledImage, {id, id_of(t.element)});
            break;
        case type_kind::acceleration_structure:
            emit(_declarations, spv::TypeAccelerationStructureKHR, {id});
            break;
        case type_kind::hit_object:
            require(spv::ShaderInvocationReorderEXT);
            require_extension("SPV_EXT_shader_invocation_reorder");
            emit(_declarations, spv::TypeHitObjectEXT, {id});
            break;
        case type_kind::ray_query:
            require(spv::CapRayQueryKHR);
            require_extension("SPV_KHR_ray_query");
            emit(_declarations, spv::TypeRayQueryKHR, {id});
            break;
        }
    }
    emit(_declarations, spv::TypeFunction, {_function_type, id_of(_void)});
}

void writer::write_structure(ir::type const& t, u32 id)
{
    head(_declarations, spv::TypeStruct, 2 + isize(t.member_count));
    _declarations.push_back(id);
    for (auto i = u32(0); i < t.member_count; ++i)
        _declarations.push_back(id_of(_p.members[t.first_member + i].type));
    name(id, _p.name_at(t.name_first, t.name_size));
    if (t.is_block)
        emit(_annotations, spv::Decorate, {id, spv::Block});
    for (auto i = u32(0); i < t.member_count; ++i)
    {
        auto const& m = _p.members[t.first_member + i];
        auto const text = _p.name_at(m.name_first, m.name_size);
        head(_names, spv::MemberName, 3 + string_words(text));
        _names.push_back(id);
        _names.push_back(i);
        string_into(_names, text);
        if (m.offset >= 0)
            emit(_annotations, spv::MemberDecorate, {id, i, spv::Offset, u32(m.offset)});
        if (m.matrix_stride > 0)
        {
            emit(_annotations, spv::MemberDecorate, {id, i, spv::ColMajor});
            emit(_annotations, spv::MemberDecorate, {id, i, spv::MatrixStride, u32(m.matrix_stride)});
        }
    }
}

void writer::write_image(ir::type const& t, u32 id)
{
    auto format = u32(0);
    if (t.is_storage_image)
    {
        auto const spelled = check::k_image_formats[t.format].spirv;
        for (auto const& f : k_formats)
            if (f.spelled == spelled)
            {
                format = f.number;
                if (f.is_extended)
                    require(spv::StorageImageExtendedFormats);
            }
        CC_ASSERT(format != 0, "every format the lowering takes has a number");
    }
    if (t.dimension == 0)
        require(t.is_storage_image ? spv::Image1D : spv::Sampled1D);
    if (t.dimension == 3 && t.is_arrayed)
        require(t.is_storage_image ? spv::ImageCubeArray : spv::SampledCubeArray);
    if (t.is_multisampled && t.is_arrayed)
        require(spv::ImageMSArray);
    emit(_declarations, spv::TypeImage,
         {id, id_of(t.element), u32(t.dimension), t.is_depth ? 1u : 0u, t.is_arrayed ? 1u : 0u,
          t.is_multisampled ? 1u : 0u, t.is_storage_image ? 2u : 1u, format});
}

void writer::write_constants()
{
    for (auto i = isize(0); i < _p.constants.size(); ++i)
    {
        auto const& c = _p.constants[i];
        auto const id = _constants + u32(i);
        auto const& t = _p.at(c.type);
        if (c.is_undefined)
            emit(_declarations, spv::Undef, {id_of(c.type), id});
        else if (c.is_null)
            emit(_declarations, spv::ConstantNull, {id_of(c.type), id});
        else if (t.kind == type_kind::bool_)
            emit(_declarations, c.low != 0 ? spv::ConstantTrue : spv::ConstantFalse, {id_of(c.type), id});
        else if (t.width == 64)
            emit(_declarations, spv::Constant, {id_of(c.type), id, c.low, c.high});
        else
            emit(_declarations, spv::Constant, {id_of(c.type), id, c.low});
    }
}

u32 writer::space_of(stage_element const& e) const
{
    // what a stage before wrote per primitive or per patch, this one reads
    auto const is_input = e.part == stage_part::input
                       || (e.part == stage_part::primitive && _p.stage == check::stage::pixel)
                       || (e.part == stage_part::patch && _p.stage == check::stage::tessellation_evaluation);
    return is_input ? u32(spv::Input) : u32(spv::Output);
}

u32 writer::pointer_to(u32 space, u32 pointee)
{
    for (auto const& known : _pointer_types)
        if (known.space == space && known.pointee == pointee)
            return known.id;
    auto const id = _bound++;
    emit(_declarations, spv::TypePointer, {id, space, pointee});
    _pointer_types.push_back({.space = space, .pointee = pointee, .id = id});
    return id;
}

void writer::write_stage_elements()
{
    auto const is_tessellation
        = _p.stage == check::stage::tessellation_control || _p.stage == check::stage::tessellation_evaluation;
    if (is_tessellation)
        require(spv::Tessellation);
    if (_p.stage == check::stage::geometry)
        require(spv::Geometry);
    if (_p.stage == check::stage::tessellation_control)
    {
        _invocation = _bound++;
        emit(_declarations, spv::Variable, {pointer_to(spv::Input, id_of(_uint)), _invocation, spv::Input});
        emit(_annotations, spv::Decorate, {_invocation, spv::BuiltIn, k_invocation_id});
    }
    if (_p.stage == check::stage::mesh || _p.stage == check::stage::task)
    {
        require(spv::MeshShadingEXT);
        require_extension("SPV_EXT_mesh_shader");
    }
    if (_p.stage == check::stage::mesh)
    {
        auto const held = id_of(_p.primitive.primitive_indices);
        auto const array = _bound++;
        emit(_declarations, spv::TypeArray, {array, held, literal_id(_p.primitive.max_primitives)});
        _primitive_lane = pointer_to(spv::Output, held);
        _primitive_indices = _bound++;
        emit(_declarations, spv::Variable, {pointer_to(spv::Output, array), _primitive_indices, spv::Output});
        emit(_annotations, spv::Decorate,
             {_primitive_indices, spv::BuiltIn,
              _p.primitive.writes_lines ? k_primitive_line_indices : k_primitive_triangle_indices});
    }
    for (auto i = isize(0); i < _p.stage_elements.size(); ++i)
    {
        auto const& e = _p.stage_elements[i];
        auto const id = _elements + u32(i);
        auto const space = space_of(e);
        // a factor is one element of an array of them, and the first one declares that array
        if (is_factor(e.system) && e.semantic_index != 0)
        {
            _element_variables.push_back(_element_variables[i - isize(e.semantic_index)]);
            _element_lanes.push_back(_element_lanes[i - isize(e.semantic_index)]);
            continue;
        }
        auto held = id_of(e.type);
        // `TessCoord` is three lanes, though a parameter of a quad or an isoline takes two
        if (e.system == built_in::domain_location)
            for (auto t = isize(0); t < _p.types.size(); ++t)
                if (_p.types[t].kind == type_kind::vector && _p.types[t].count == 3
                    && _p.types[t].element == _p.at(e.type).element)
                    held = _types + u32(t);
        auto const count = e.system == built_in::sample_mask ? u32(1)
                         : e.system == built_in::tess_edge   ? k_outer_factors
                         : e.system == built_in::tess_inside ? k_inner_factors
                                                             : e.array_size;
        auto lane = u32(0);
        if (count > 0)
        {
            lane = pointer_to(space, held);
            auto const array = _bound++;
            emit(_declarations, spv::TypeArray, {array, held, literal_id(count)});
            held = array;
        }
        _element_variables.push_back(id);
        _element_lanes.push_back(lane);
        emit(_declarations, spv::Variable, {pointer_to(space, held), id, space});
        name(id, _p.name_at(e.name_first, e.name_size));
        for (auto const& known : k_built_ins)
            if (known.input == e.system)
            {
                auto const is_frag_coord = e.system == built_in::position && _p.stage == check::stage::pixel;
                emit(_annotations, spv::Decorate, {id, spv::BuiltIn, is_frag_coord ? k_frag_coord : known.number});
                if (known.is_subgroup)
                    require(spv::GroupNonUniform);
                if (e.system == built_in::first_instance)
                    require(spv::DrawParameters);
                if (e.system == built_in::sample_index)
                    require(spv::SampleRateShading);
                if (e.system == built_in::view_index)
                    require(spv::MultiView);
                if (e.system == built_in::ray_cluster_id)
                {
                    require(spv::RayTracingClusterAccelerationStructureNV);
                    require_extension("SPV_NV_cluster_acceleration_structure");
                }
                if (e.system == built_in::primitive_id && !is_tessellation)
                    require(spv::Geometry);
                if (e.system == built_in::layer && _p.stage != check::stage::mesh)
                    require(_p.stage == check::stage::pixel || _p.stage == check::stage::geometry ? spv::Geometry
                                                                                                  : spv::ShaderLayer);
            }
        if (e.system == built_in::none && e.location >= 0)
            emit(_annotations, spv::Decorate, {id, spv::Location, u32(e.location)});
        if (space == spv::Input && _p.stage == check::stage::pixel)
        {
            if (e.system == built_in::none)
                write_interpolation(id, e.interpolate);
            else if (e.system != built_in::position)
                emit(_annotations, spv::Decorate, {id, spv::Flat});
        }
        // DXC marks an evaluation stage's primitive id as per patch, which it is
        if (e.part == stage_part::patch
            || (e.system == built_in::primitive_id && _p.stage == check::stage::tessellation_evaluation))
            emit(_annotations, spv::Decorate, {id, spv::Patch});
        if (e.part == stage_part::primitive)
        {
            require(spv::MeshShadingEXT);
            require_extension("SPV_EXT_mesh_shader");
            emit(_annotations, spv::Decorate, {id, spv::PerPrimitiveEXT});
        }
    }
}

void writer::write_interpolation(u32 id, interpolation mode)
{
    auto const is_noperspective = mode == interpolation::noperspective || mode == interpolation::noperspective_centroid
                               || mode == interpolation::noperspective_sample;
    if (mode == interpolation::flat)
        emit(_annotations, spv::Decorate, {id, spv::Flat});
    if (is_noperspective)
        emit(_annotations, spv::Decorate, {id, spv::NoPerspective});
    if (mode == interpolation::centroid || mode == interpolation::noperspective_centroid)
        emit(_annotations, spv::Decorate, {id, spv::Centroid});
    if (mode == interpolation::sample || mode == interpolation::noperspective_sample)
    {
        require(spv::SampleRateShading);
        emit(_annotations, spv::Decorate, {id, spv::Sample});
    }
}

u32 writer::table_constant(type_ref type, cc::span<u32 const> leaves, isize& at)
{
    auto const& info = _p.at(type);
    auto parts = cc::vector<u32>();
    switch (info.kind)
    {
    case type_kind::vector:
    case type_kind::matrix:
    case type_kind::array:
        for (auto k = u32(0); k < info.count; ++k)
            parts.push_back(table_constant(info.element, leaves, at));
        break;
    case type_kind::structure:
        for (auto m = u32(0); m < info.member_count; ++m)
            parts.push_back(table_constant(_p.members[info.first_member + m].type, leaves, at));
        break;
    default:
        return id_of(value_ref(leaves[at++]));
    }
    auto const id = _bound++;
    head(_declarations, spv::ConstantComposite, 3 + parts.size());
    _declarations.push_back(id_of(type));
    _declarations.push_back(id);
    _declarations.push_back_range(parts);
    return id;
}

void writer::write_globals()
{
    for (auto i = isize(0); i < _p.globals.size(); ++i)
    {
        auto const& g = _p.globals[i];
        auto const id = _globals + u32(i);
        if (_p.at(g.type).space == storage::table)
        {
            auto const leaves = cc::span<u32 const>(_p.initializers.data() + g.initializer_first, g.initializer_count);
            auto at = isize(0);
            auto const value = table_constant(_p.at(g.type).element, leaves, at);
            emit(_declarations, spv::Variable, {id_of(g.type), id, spv::Private, value});
            name(id, _p.name_at(g.name_first, g.name_size));
            continue;
        }
        emit(_declarations, spv::Variable, {id_of(g.type), id, storage_of(_p.at(g.type).space)});
        name(id, _p.name_at(g.name_first, g.name_size));
        if (g.set >= 0)
            emit(_annotations, spv::Decorate, {id, spv::DescriptorSet, u32(g.set)});
        auto const space = _p.at(g.type).space;
        if (g.binding >= 0 && (space == storage::ray_output || space == storage::callable_output))
            emit(_annotations, spv::Decorate, {id, spv::Location, u32(g.binding)});
        else if (g.binding >= 0)
            emit(_annotations, spv::Decorate, {id, spv::Binding, u32(g.binding)});
        auto const& pointee = _p.at(_p.at(g.type).element);
        auto const& held = pointee.kind == type_kind::array || pointee.kind == type_kind::runtime_array
                             ? _p.at(pointee.element)
                             : pointee;
        auto const is_storage = space == storage::storage_buffer || (space == storage::resource && held.is_storage_image);
        if (g.is_read_only && is_storage)
            emit(_annotations, spv::Decorate, {id, spv::NonWritable});
        if (g.is_write_only)
            emit(_annotations, spv::Decorate, {id, spv::NonReadable});
        if (g.is_coherent)
            emit(_annotations, spv::Decorate, {id, spv::Coherent});
    }
}

void writer::write_primitive_modes()
{
    auto const mode = [&](u32 number) { emit(_modes, spv::ExecutionMode, {_function, number}); };
    auto const& shape = _p.primitive;
    if (_p.stage == check::stage::mesh)
    {
        mode(shape.writes_lines ? k_mode_output_lines : k_mode_output_triangles);
        emit(_modes, spv::ExecutionMode, {_function, k_mode_output_vertices, shape.max_vertices});
        emit(_modes, spv::ExecutionMode, {_function, k_mode_output_primitives, shape.max_primitives});
        return;
    }
    if (_p.stage == check::stage::geometry)
    {
        emit(_modes, spv::ExecutionMode, {_function, k_mode_output_vertices, shape.max_vertices});
        emit(_modes, spv::ExecutionMode, {_function, k_mode_invocations, 1u});
        mode(shape.input_vertices == 1   ? k_mode_input_points
             : shape.input_vertices == 2 ? k_mode_input_lines
             : shape.input_vertices == 4 ? k_mode_input_lines_adjacency
             : shape.input_vertices == 6 ? k_mode_input_triangles_adjacency
                                         : k_mode_triangles);
        mode(shape.output_vertices == 1   ? k_mode_output_points
             : shape.output_vertices == 2 ? k_mode_output_line_strip
                                          : k_mode_output_triangle_strip);
        return;
    }
    mode(shape.domain == tessellation_domain::isoline    ? k_mode_isolines
         : shape.domain == tessellation_domain::triangle ? k_mode_triangles
                                                         : k_mode_quads);
    if (_p.stage != check::stage::tessellation_control)
        return;
    using partitioning = check::tessellation_partitioning;
    mode(shape.partitioning == partitioning::integer           ? k_mode_spacing_equal
         : shape.partitioning == partitioning::fractional_even ? k_mode_spacing_fractional_even
                                                               : k_mode_spacing_fractional_odd);
    if (shape.domain != tessellation_domain::isoline)
        mode(shape.is_output_clockwise ? k_mode_vertex_order_cw : k_mode_vertex_order_ccw);
    emit(_modes, spv::ExecutionMode, {_function, k_mode_output_vertices, shape.input_vertices});
}

void writer::write_entry()
{
    auto interface = cc::vector<u32>();
    for (auto i = isize(0); i < _p.stage_elements.size(); ++i)
        if (_element_variables[i] == _elements + u32(i))
            interface.push_back(_elements + u32(i));
    if (_invocation != 0)
        interface.push_back(_invocation);
    if (_primitive_indices != 0)
        interface.push_back(_primitive_indices);
    for (auto i = isize(0); i < _p.globals.size(); ++i)
        interface.push_back(_globals + u32(i));

    auto model = k_model_compute;
    switch (_p.stage)
    {
    case check::stage::vertex:
        model = k_model_vertex;
        break;
    case check::stage::pixel:
        model = k_model_fragment;
        break;
    case check::stage::geometry:
        model = k_model_geometry;
        break;
    case check::stage::tessellation_control:
        model = k_model_tessellation_control;
        break;
    case check::stage::tessellation_evaluation:
        model = k_model_tessellation_evaluation;
        break;
    case check::stage::task:
        model = k_model_task;
        break;
    case check::stage::mesh:
        model = k_model_mesh;
        break;
    case check::stage::raygen:
        model = k_model_raygen;
        break;
    case check::stage::miss:
        model = k_model_miss;
        break;
    case check::stage::closest_hit:
        model = k_model_closest_hit;
        break;
    case check::stage::any_hit:
        model = k_model_any_hit;
        break;
    case check::stage::intersection:
        model = k_model_intersection;
        break;
    case check::stage::callable:
        model = k_model_callable;
        break;
    default:
        break;
    }
    // what a trace's flags may name, and what DXC's own text leaves out (EMIT-184)
    if (_p.sees_micromaps)
    {
        require(spv::RayTracingOpacityMicromapEXT);
        require_extension("SPV_EXT_opacity_micromap");
    }
    if (_p.stage >= check::stage::raygen)
    {
        require(spv::RayTracingKHR);
        require_extension("SPV_KHR_ray_tracing");
    }
    head(_entry, spv::EntryPoint, 3 + string_words(_p.entry_name) + interface.size());
    _entry.push_back(model);
    _entry.push_back(_function);
    string_into(_entry, _p.entry_name);
    _entry.push_back_range(interface);
    if (_p.stage == check::stage::pixel)
    {
        emit(_modes, spv::ExecutionMode, {_function, k_mode_origin_upper_left});
        for (auto const& e : _p.stage_elements)
        {
            if (e.system == built_in::depth || e.system == built_in::depth_greater_equal
                || e.system == built_in::depth_less_equal)
                emit(_modes, spv::ExecutionMode, {_function, k_mode_depth_replacing});
            if (e.system == built_in::depth_greater_equal)
                emit(_modes, spv::ExecutionMode, {_function, k_mode_depth_greater});
            if (e.system == built_in::depth_less_equal)
                emit(_modes, spv::ExecutionMode, {_function, k_mode_depth_less});
        }
    }
    else if (model == k_model_compute || model == k_model_task || model == k_model_mesh)
    {
        emit(_modes, spv::ExecutionMode,
             {_function, k_mode_local_size, u32(_p.workgroup[0]), u32(_p.workgroup[1]), u32(_p.workgroup[2])});
        if (model == k_model_mesh)
            write_primitive_modes();
    }
    else if (model != k_model_vertex && _p.stage < check::stage::raygen)
        write_primitive_modes();
    name(_function, _p.entry_name);

    // what no operation can say: a whole entry point's float behaviour, by width
    if (_p.guarantee != check::float_level::fast)
        for (auto const& t : _p.types)
            if (t.kind == type_kind::float_)
            {
                require(spv::SignedZeroInfNanPreserve);
                emit(_modes, spv::ExecutionMode, {_function, 4461u, u32(t.width)});
                if (_p.guarantee == check::float_level::ieee)
                {
                    require(spv::DenormPreserve);
                    emit(_modes, spv::ExecutionMode, {_function, 4459u, u32(t.width)});
                }
            }
}
