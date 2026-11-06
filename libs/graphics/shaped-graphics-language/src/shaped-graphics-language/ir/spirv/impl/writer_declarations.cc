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
};
/// A pixel stage reads `@position` as what the rasterizer made of it, where a vertex stage's is `Position`.
constexpr u32 k_frag_coord = 15;

constexpr u32 k_model_vertex = 0;
constexpr u32 k_model_fragment = 4;
constexpr u32 k_model_compute = 5;
constexpr u32 k_mode_origin_upper_left = 7;
constexpr u32 k_mode_depth_replacing = 12;
constexpr u32 k_mode_depth_greater = 14;
constexpr u32 k_mode_depth_less = 15;
constexpr u32 k_mode_local_size = 17;
} // namespace

u32 writer::storage_of(storage s)
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
    auto const is_input
        = e.part == stage_part::input || (e.part == stage_part::primitive && _p.stage == check::stage::pixel);
    return is_input ? u32(spv::Input) : u32(spv::Output);
}

void writer::write_stage_elements()
{
    for (auto i = isize(0); i < _p.stage_elements.size(); ++i)
    {
        auto const& e = _p.stage_elements[i];
        auto const id = _elements + u32(i);
        auto const space = space_of(e);
        auto const is_mask = e.system == built_in::sample_mask;
        // a pointer type is declared once, and a program's own table holds none of these
        auto pointer = u32(0);
        for (auto k = isize(0); k < i; ++k)
            if (_p.stage_elements[k].type == e.type && space_of(_p.stage_elements[k]) == space
                && (_p.stage_elements[k].system == built_in::sample_mask) == is_mask)
                pointer = _element_pointers[k];
        if (pointer == 0)
        {
            pointer = _bound++;
            auto held = id_of(e.type);
            if (is_mask)
            {
                auto const array = _bound++;
                auto const one = literal_id(1);
                emit(_declarations, spv::TypeArray, {array, held, one});
                held = array;
            }
            emit(_declarations, spv::TypePointer, {pointer, space, held});
        }
        _element_pointers.push_back(pointer);
        auto lane = u32(0);
        if (is_mask)
        {
            lane = _bound++;
            emit(_declarations, spv::TypePointer, {lane, space, id_of(e.type)});
        }
        _element_lanes.push_back(lane);
        emit(_declarations, spv::Variable, {pointer, id, space});
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
                if (e.system == built_in::primitive_id)
                    require(spv::Geometry);
                if (e.system == built_in::layer)
                    require(_p.stage == check::stage::pixel ? spv::Geometry : spv::ShaderLayer);
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

void writer::write_globals()
{
    for (auto i = isize(0); i < _p.globals.size(); ++i)
    {
        auto const& g = _p.globals[i];
        auto const id = _globals + u32(i);
        emit(_declarations, spv::Variable, {id_of(g.type), id, storage_of(_p.at(g.type).space)});
        name(id, _p.name_at(g.name_first, g.name_size));
        if (g.set >= 0)
            emit(_annotations, spv::Decorate, {id, spv::DescriptorSet, u32(g.set)});
        if (g.binding >= 0)
            emit(_annotations, spv::Decorate, {id, spv::Binding, u32(g.binding)});
        auto const space = _p.at(g.type).space;
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

void writer::write_entry()
{
    auto const words = 3 + string_words(_p.entry_name) + _p.stage_elements.size() + _p.globals.size();
    head(_entry, spv::EntryPoint, words);
    _entry.push_back(_p.stage == check::stage::vertex  ? k_model_vertex
                     : _p.stage == check::stage::pixel ? k_model_fragment
                                                       : k_model_compute);
    _entry.push_back(_function);
    string_into(_entry, _p.entry_name);
    for (auto i = isize(0); i < _p.stage_elements.size(); ++i)
        _entry.push_back(_elements + u32(i));
    for (auto i = isize(0); i < _p.globals.size(); ++i)
        _entry.push_back(_globals + u32(i));
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
    else if (_p.stage != check::stage::vertex)
        emit(_modes, spv::ExecutionMode,
             {_function, k_mode_local_size, u32(_p.workgroup[0]), u32(_p.workgroup[1]), u32(_p.workgroup[2])});
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
