#include "spirv.hh"

#include <clean-core/common/assert.hh>
#include <shaped-graphics-language/check/resources.hh>

using namespace sgl;
using namespace sgl::ir;

namespace
{
// The numbers are SPIR-V's own, from its unified headers; nothing here links them, so the compiler stays clean-core's alone.
namespace spv
{
enum op_code : u32
{
    Name = 5,
    MemberName = 6,
    Extension = 10,
    ExtInstImport = 11,
    ExtInst = 12,
    MemoryModel = 14,
    EntryPoint = 15,
    ExecutionMode = 16,
    Capability = 17,
    TypeVoid = 19,
    TypeBool = 20,
    TypeInt = 21,
    TypeFloat = 22,
    TypeVector = 23,
    TypeMatrix = 24,
    TypeImage = 25,
    TypeSampler = 26,
    TypeSampledImage = 27,
    TypeArray = 28,
    TypeRuntimeArray = 29,
    TypeStruct = 30,
    TypePointer = 32,
    TypeFunction = 33,
    ConstantTrue = 41,
    ConstantFalse = 42,
    Constant = 43,
    ConstantNull = 46,
    Function = 54,
    FunctionEnd = 56,
    Variable = 59,
    Undef = 1,
    Load = 61,
    Store = 62,
    AccessChain = 65,
    ArrayLength = 68,
    Decorate = 71,
    MemberDecorate = 72,
    VectorShuffle = 79,
    CompositeConstruct = 80,
    CompositeExtract = 81,
    CompositeInsert = 82,
    Transpose = 84,
    SampledImage = 86,
    ImageSampleExplicitLod = 88,
    ImageFetch = 95,
    ImageGather = 96,
    ImageRead = 98,
    ImageWrite = 99,
    ImageQuerySizeLod = 103,
    ImageQuerySize = 104,
    ImageQueryLevels = 106,
    ImageQuerySamples = 107,
    ConvertFToU = 109,
    ConvertFToS = 110,
    ConvertSToF = 111,
    ConvertUToF = 112,
    UConvert = 113,
    SConvert = 114,
    FConvert = 115,
    Bitcast = 124,
    SNegate = 126,
    FNegate = 127,
    IAdd = 128,
    FAdd = 129,
    ISub = 130,
    FSub = 131,
    IMul = 132,
    FMul = 133,
    UDiv = 134,
    SDiv = 135,
    FDiv = 136,
    UMod = 137,
    SRem = 138,
    FRem = 140,
    VectorTimesScalar = 142,
    MatrixTimesScalar = 143,
    MatrixTimesVector = 145,
    MatrixTimesMatrix = 146,
    Dot = 148,
    Any = 154,
    All = 155,
    IsNan = 156,
    IsInf = 157,
    LogicalEqual = 164,
    LogicalNotEqual = 165,
    LogicalOr = 166,
    LogicalAnd = 167,
    LogicalNot = 168,
    Select = 169,
    IEqual = 170,
    INotEqual = 171,
    UGreaterThan = 172,
    SGreaterThan = 173,
    UGreaterThanEqual = 174,
    SGreaterThanEqual = 175,
    ULessThan = 176,
    SLessThan = 177,
    ULessThanEqual = 178,
    SLessThanEqual = 179,
    FOrdEqual = 180,
    FUnordNotEqual = 183,
    FOrdLessThan = 184,
    FOrdGreaterThan = 186,
    FOrdLessThanEqual = 188,
    FOrdGreaterThanEqual = 190,
    ShiftRightLogical = 194,
    ShiftRightArithmetic = 195,
    ShiftLeftLogical = 196,
    BitwiseOr = 197,
    BitwiseXor = 198,
    BitwiseAnd = 199,
    Not = 200,
    BitReverse = 204,
    BitCount = 205,
    ControlBarrier = 224,
    AtomicLoad = 227,
    AtomicStore = 228,
    AtomicExchange = 229,
    AtomicCompareExchange = 230,
    AtomicIAdd = 234,
    AtomicISub = 235,
    AtomicSMin = 236,
    AtomicUMin = 237,
    AtomicSMax = 238,
    AtomicUMax = 239,
    AtomicAnd = 240,
    AtomicOr = 241,
    AtomicXor = 242,
    Phi = 245,
    LoopMerge = 246,
    SelectionMerge = 247,
    Label = 248,
    Branch = 249,
    BranchConditional = 250,
    Switch = 251,
    Return = 253,
    Unreachable = 255,
    GroupNonUniformAll = 334,
    GroupNonUniformAny = 335,
    GroupNonUniformBroadcast = 337,
    GroupNonUniformBroadcastFirst = 338,
    GroupNonUniformBallot = 339,
    GroupNonUniformShuffle = 345,
    GroupNonUniformShuffleXor = 346,
    GroupNonUniformShuffleUp = 347,
    GroupNonUniformShuffleDown = 348,
    GroupNonUniformIAdd = 349,
    GroupNonUniformFAdd = 350,
    GroupNonUniformIMul = 351,
    GroupNonUniformFMul = 352,
    GroupNonUniformSMin = 353,
    GroupNonUniformUMin = 354,
    GroupNonUniformFMin = 355,
    GroupNonUniformSMax = 356,
    GroupNonUniformUMax = 357,
    GroupNonUniformFMax = 358,
    GroupNonUniformBitwiseAnd = 359,
    GroupNonUniformBitwiseOr = 360,
    GroupNonUniformBitwiseXor = 361,
    GroupNonUniformQuadBroadcast = 365,
    GroupNonUniformQuadSwap = 366,
    CopyLogical = 400,
    AtomicFAddEXT = 6035,
};

enum capability : u32
{
    Shader = 1,
    Float16 = 9,
    Float64 = 10,
    Int64 = 11,
    Int64Atomics = 12,
    Int16 = 22,
    ImageCubeArray = 34,
    Sampled1D = 43,
    Image1D = 44,
    SampledCubeArray = 45,
    ImageMSArray = 48,
    StorageImageExtendedFormats = 49,
    ImageQuery = 50,
    GroupNonUniform = 61,
    GroupNonUniformVote = 62,
    GroupNonUniformArithmetic = 63,
    CapGroupNonUniformBallot = 64,
    CapGroupNonUniformShuffle = 65,
    GroupNonUniformShuffleRelative = 66,
    GroupNonUniformQuad = 68,
    StorageBuffer16BitAccess = 4433,
    UniformAndStorageBuffer16BitAccess = 4434,
    StoragePushConstant16 = 4435,
    DenormPreserve = 4464,
    SignedZeroInfNanPreserve = 4466,
    AtomicFloat32AddEXT = 6033,
    AtomicFloat64AddEXT = 6034,
};

enum decoration : u32
{
    Block = 2,
    ColMajor = 5,
    ArrayStride = 6,
    MatrixStride = 7,
    BuiltIn = 11,
    Coherent = 23,
    NonWritable = 24,
    NonReadable = 25,
    Binding = 33,
    DescriptorSet = 34,
    Offset = 35,
    NoContraction = 42,
};

enum storage_class : u32
{
    UniformConstant = 0,
    Input = 1,
    Uniform = 2,
    Workgroup = 4,
    FunctionStorage = 7,
    PushConstant = 9,
    StorageBuffer = 12,
};
} // namespace spv

constexpr u32 k_scope_device = 1;
constexpr u32 k_scope_workgroup = 2;
constexpr u32 k_scope_subgroup = 3;
constexpr u32 k_semantics_acquire_release = 0x8;
constexpr u32 k_semantics_uniform = 0x40;
constexpr u32 k_semantics_workgroup = 0x100;
constexpr u32 k_semantics_image = 0x800;

/// GLSL.std.450's number of each `math`, parallel to it.
constexpr u32 k_glsl_numbers[] = {
    2,  3,  4,  5,  6,  7,  8,  9,  10, // round_even to fract
    13, 14, 15, 16, 17, 18, 19, 20, 21, // sin to tanh
    25, 26, 27, 28, 29, 30, 31, 32, 33, // atan2 to determinant
    37, 38, 39, 40, 41, 42, 43, 44, 45, // fmin to sclamp
    46, 48, 49, 50,                     // fmix to fma
    54, 55, 58, 62, 63, 64,             // the packs
    66, 67, 68, 69, 71, 72,             // length to refract
    73, 74, 75,                         // find_ilsb to find_umsb
};
static_assert(sizeof(k_glsl_numbers) / sizeof(k_glsl_numbers[0]) == sizeof(char[isize(math::find_umsb) + 1]));

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

/// The opcode of an operation whose words are only its result and its operands, or 0.
[[nodiscard]] constexpr u32 plain_opcode(ir::op code)
{
    switch (code)
    {
    case op::fadd:
        return spv::FAdd;
    case op::fsub:
        return spv::FSub;
    case op::fmul:
        return spv::FMul;
    case op::fdiv:
        return spv::FDiv;
    case op::frem:
        return spv::FRem;
    case op::fneg:
        return spv::FNegate;
    case op::iadd:
        return spv::IAdd;
    case op::isub:
        return spv::ISub;
    case op::imul:
        return spv::IMul;
    case op::sdiv:
        return spv::SDiv;
    case op::udiv:
        return spv::UDiv;
    case op::srem:
        return spv::SRem;
    case op::urem:
        return spv::UMod;
    case op::ineg:
        return spv::SNegate;
    case op::bit_and:
        return spv::BitwiseAnd;
    case op::bit_or:
        return spv::BitwiseOr;
    case op::bit_xor:
        return spv::BitwiseXor;
    case op::bit_not:
        return spv::Not;
    case op::bit_reverse:
        return spv::BitReverse;
    case op::bit_count:
        return spv::BitCount;
    case op::shift_left:
        return spv::ShiftLeftLogical;
    case op::shift_right_logical:
        return spv::ShiftRightLogical;
    case op::shift_right_arithmetic:
        return spv::ShiftRightArithmetic;
    case op::feq:
        return spv::FOrdEqual;
    case op::fne:
        return spv::FUnordNotEqual;
    case op::flt:
        return spv::FOrdLessThan;
    case op::fle:
        return spv::FOrdLessThanEqual;
    case op::fgt:
        return spv::FOrdGreaterThan;
    case op::fge:
        return spv::FOrdGreaterThanEqual;
    case op::is_nan:
        return spv::IsNan;
    case op::is_inf:
        return spv::IsInf;
    case op::ieq:
        return spv::IEqual;
    case op::ine:
        return spv::INotEqual;
    case op::slt:
        return spv::SLessThan;
    case op::sle:
        return spv::SLessThanEqual;
    case op::sgt:
        return spv::SGreaterThan;
    case op::sge:
        return spv::SGreaterThanEqual;
    case op::ult:
        return spv::ULessThan;
    case op::ule:
        return spv::ULessThanEqual;
    case op::ugt:
        return spv::UGreaterThan;
    case op::uge:
        return spv::UGreaterThanEqual;
    case op::logical_and:
        return spv::LogicalAnd;
    case op::logical_or:
        return spv::LogicalOr;
    case op::logical_not:
        return spv::LogicalNot;
    case op::logical_eq:
        return spv::LogicalEqual;
    case op::logical_ne:
        return spv::LogicalNotEqual;
    case op::any:
        return spv::Any;
    case op::all:
        return spv::All;
    case op::select:
        return spv::Select;
    case op::f_to_s:
        return spv::ConvertFToS;
    case op::f_to_u:
        return spv::ConvertFToU;
    case op::s_to_f:
        return spv::ConvertSToF;
    case op::u_to_f:
        return spv::ConvertUToF;
    case op::f_resize:
        return spv::FConvert;
    case op::s_resize:
        return spv::SConvert;
    case op::u_resize:
        return spv::UConvert;
    case op::bitcast:
        return spv::Bitcast;
    case op::copy_logical:
        return spv::CopyLogical;
    case op::dot:
        return spv::Dot;
    case op::vector_times_scalar:
        return spv::VectorTimesScalar;
    case op::matrix_times_scalar:
        return spv::MatrixTimesScalar;
    case op::matrix_times_vector:
        return spv::MatrixTimesVector;
    case op::matrix_times_matrix:
        return spv::MatrixTimesMatrix;
    case op::transpose:
        return spv::Transpose;
    case op::construct:
        return spv::CompositeConstruct;
    case op::load:
        return spv::Load;
    case op::access_chain:
        return spv::AccessChain;
    case op::combine_sampler:
        return spv::SampledImage;
    case op::image_read:
        return spv::ImageRead;
    case op::image_levels:
        return spv::ImageQueryLevels;
    case op::image_samples:
        return spv::ImageQuerySamples;
    default:
        return 0;
    }
}

[[nodiscard]] constexpr bool keeps_contraction(ir::op code)
{
    switch (code)
    {
    case op::fadd:
    case op::fsub:
    case op::fmul:
    case op::fdiv:
    case op::frem:
    case op::fneg:
    case op::vector_times_scalar:
    case op::matrix_times_scalar:
    case op::matrix_times_vector:
    case op::matrix_times_matrix:
    case op::dot:
        return true;
    default:
        return false;
    }
}

class writer
{
public:
    explicit writer(program const& p) : _p(p) {}

    cc::vector<u32> run()
    {
        number_everything();
        write_types();
        write_constants();
        write_globals();
        write_function();
        write_entry();
        return assembled();
    }

private:
    void number_everything()
    {
        auto next = u32(1);
        auto const take = [&](isize count)
        {
            auto const first = next;
            next += u32(count);
            return first;
        };
        _types = take(_p.types.size());
        _constants = take(_p.constants.size());
        _globals = take(_p.globals.size());
        _locals = take(_p.locals.size());
        _instructions = take(_p.instructions.size());
        _blocks = take(_p.blocks.size());
        _glsl = take(1);
        _function = take(1);
        _function_type = take(1);
        _bound = next;
    }

    [[nodiscard]] u32 id_of(type_ref t) const { return _types + u32(t); }
    [[nodiscard]] u32 id_of(block_ref b) const { return _blocks + u32(b); }
    [[nodiscard]] u32 id_of(value_ref v) const
    {
        if (is_constant(v))
            return _constants + index_of(v);
        if (is_global(v))
            return _globals + index_of(v);
        if (is_local(v))
            return _locals + index_of(v);
        return _instructions + u32(v);
    }
    [[nodiscard]] u32 id_of_raw(u32 raw) const { return id_of(value_ref(raw)); }

    static void emit(cc::vector<u32>& out, u32 opcode, std::initializer_list<u32> words)
    {
        out.push_back((u32(words.size() + 1) << 16) | opcode);
        for (auto const w : words)
            out.push_back(w);
    }
    /// An instruction's first word, for one written word by word; `words` counts it.
    static void head(cc::vector<u32>& out, u32 opcode, isize words) { out.push_back((u32(words) << 16) | opcode); }

    static void string_into(cc::vector<u32>& out, cc::string_view text)
    {
        auto word = u32(0);
        auto shift = 0;
        for (auto const c : text)
        {
            word |= u32(u8(c)) << shift;
            shift += 8;
            if (shift == 32)
            {
                out.push_back(word);
                word = 0;
                shift = 0;
            }
        }
        out.push_back(word);
    }
    [[nodiscard]] static isize string_words(cc::string_view text) { return text.size() / 4 + 1; }

    void name(u32 id, cc::string_view text)
    {
        if (text.empty())
            return;
        head(_names, spv::Name, 2 + string_words(text));
        _names.push_back(id);
        string_into(_names, text);
    }

    void require(u32 capability)
    {
        for (auto const c : _capabilities)
            if (c == capability)
                return;
        _capabilities.push_back(capability);
    }
    void require_extension(cc::string_view text)
    {
        for (auto const e : _extensions)
            if (e == text)
                return;
        _extensions.push_back(text);
    }

    /// A `uint` constant the writer itself needs, such as a scope or an array's length.
    [[nodiscard]] u32 literal_id(u32 value)
    {
        for (auto const& known : _literals)
            if (known.value == value)
                return known.id;
        auto const id = _bound++;
        _literals.push_back({.value = value, .id = id});
        emit(_declarations, spv::Constant, {id_of(_uint), id, value});
        return id;
    }

    [[nodiscard]] static u32 storage_of(storage s)
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
        case storage::input:
            return spv::Input;
        }
        return spv::FunctionStorage;
    }

    [[nodiscard]] bool holds_16_bit(type_ref t) const
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

    void write_types()
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
                emit(_annotations, spv::Decorate, {id, spv::ArrayStride, u32(t.stride)});
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

    void write_structure(ir::type const& t, u32 id)
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

    void write_image(ir::type const& t, u32 id)
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

    void write_constants()
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

    void write_globals()
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
            if (g.built_in >= 0)
            {
                emit(_annotations, spv::Decorate, {id, spv::BuiltIn, u32(g.built_in)});
                if (g.built_in == 36 || g.built_in == 41)
                    require(spv::GroupNonUniform);
            }
            auto const space = _p.at(g.type).space;
            auto const is_storage = space == storage::storage_buffer
                                 || (space == storage::resource && _p.at(_p.at(g.type).element).is_storage_image);
            if (g.is_read_only && is_storage)
                emit(_annotations, spv::Decorate, {id, spv::NonWritable});
            if (g.is_write_only)
                emit(_annotations, spv::Decorate, {id, spv::NonReadable});
            if (g.is_coherent)
                emit(_annotations, spv::Decorate, {id, spv::Coherent});
        }
    }

    void ext_inst(instruction const& i, u32 id)
    {
        _uses_glsl = true;
        auto const operands = _p.operands_of(i);
        head(_code, spv::ExtInst, 5 + operands.size());
        _code.push_back(id_of(i.type));
        _code.push_back(id);
        _code.push_back(_glsl);
        _code.push_back(k_glsl_numbers[i.sub]);
        for (auto const v : operands)
            _code.push_back(id_of_raw(v));
    }

    void atomic(u32 opcode, instruction const& i, u32 id)
    {
        auto const operands = _p.operands_of(i);
        auto const pointer = id_of_raw(operands[0]);
        auto const scope = literal_id(k_scope_device);
        auto const none = literal_id(0);
        auto const held = _p.at(_p.at(_p.type_of(value_ref(operands[0]))).element);
        if (held.width == 64)
            require(spv::Int64Atomics);
        if (i.code == op::atomic_store)
            return emit(_code, opcode, {pointer, scope, none, id_of_raw(operands[1])});
        if (i.code == op::atomic_load)
            return emit(_code, opcode, {id_of(i.type), id, pointer, scope, none});
        if (i.code == op::atomic_compare_exchange)
            return emit(_code, opcode,
                        {id_of(i.type), id, pointer, scope, none, none, id_of_raw(operands[1]), id_of_raw(operands[2])});
        emit(_code, opcode, {id_of(i.type), id, pointer, scope, none, id_of_raw(operands[1])});
    }

    void group(instruction const& i, u32 id)
    {
        auto const operands = _p.operands_of(i);
        auto const scope = literal_id(k_scope_subgroup);
        auto const type = id_of(i.type);
        auto const value = id_of_raw(operands[0]);
        auto const what = subgroup(i.sub);
        require(spv::GroupNonUniform);
        auto const plain = [&](u32 opcode) { emit(_code, opcode, {type, id, scope, value}); };
        auto const with_id = [&](u32 opcode)
        {
            auto const second = id_of_raw(operands[1]);
            emit(_code, opcode, {type, id, scope, value, second});
        };
        auto const folded = [&](u32 opcode)
        {
            require(spv::GroupNonUniformArithmetic);
            emit(_code, opcode, {type, id, scope, u32(i.aux) - 1, value});
        };
        switch (what)
        {
        case subgroup::all:
            require(spv::GroupNonUniformVote);
            return plain(spv::GroupNonUniformAll);
        case subgroup::any:
            require(spv::GroupNonUniformVote);
            return plain(spv::GroupNonUniformAny);
        case subgroup::broadcast:
            require(spv::CapGroupNonUniformBallot);
            return with_id(spv::GroupNonUniformBroadcast);
        case subgroup::broadcast_first:
            require(spv::CapGroupNonUniformBallot);
            return plain(spv::GroupNonUniformBroadcastFirst);
        case subgroup::ballot:
            require(spv::CapGroupNonUniformBallot);
            return plain(spv::GroupNonUniformBallot);
        case subgroup::shuffle:
            require(spv::CapGroupNonUniformShuffle);
            return with_id(spv::GroupNonUniformShuffle);
        case subgroup::shuffle_xor:
            require(spv::CapGroupNonUniformShuffle);
            return with_id(spv::GroupNonUniformShuffleXor);
        case subgroup::shuffle_up:
            require(spv::GroupNonUniformShuffleRelative);
            return with_id(spv::GroupNonUniformShuffleUp);
        case subgroup::shuffle_down:
            require(spv::GroupNonUniformShuffleRelative);
            return with_id(spv::GroupNonUniformShuffleDown);
        case subgroup::iadd:
            return folded(spv::GroupNonUniformIAdd);
        case subgroup::fadd:
            return folded(spv::GroupNonUniformFAdd);
        case subgroup::imul:
            return folded(spv::GroupNonUniformIMul);
        case subgroup::fmul:
            return folded(spv::GroupNonUniformFMul);
        case subgroup::smin:
            return folded(spv::GroupNonUniformSMin);
        case subgroup::umin:
            return folded(spv::GroupNonUniformUMin);
        case subgroup::fmin:
            return folded(spv::GroupNonUniformFMin);
        case subgroup::smax:
            return folded(spv::GroupNonUniformSMax);
        case subgroup::umax:
            return folded(spv::GroupNonUniformUMax);
        case subgroup::fmax:
            return folded(spv::GroupNonUniformFMax);
        case subgroup::bit_and:
            return folded(spv::GroupNonUniformBitwiseAnd);
        case subgroup::bit_or:
            return folded(spv::GroupNonUniformBitwiseOr);
        case subgroup::bit_xor:
            return folded(spv::GroupNonUniformBitwiseXor);
        case subgroup::quad_broadcast:
            require(spv::GroupNonUniformQuad);
            return with_id(spv::GroupNonUniformQuadBroadcast);
        case subgroup::quad_swap:
            require(spv::GroupNonUniformQuad);
            return with_id(spv::GroupNonUniformQuadSwap);
        }
    }

    void instruction_of(u32 index)
    {
        auto const& i = _p.instructions[index];
        auto const id = _instructions + index;
        auto const operands = _p.operands_of(i);
        if (i.guarantee != check::float_level::fast && keeps_contraction(i.code))
            emit(_annotations, spv::Decorate, {id, spv::NoContraction});

        if (auto const opcode = plain_opcode(i.code); opcode != 0)
        {
            head(_code, opcode, 3 + operands.size());
            _code.push_back(id_of(i.type));
            _code.push_back(id);
            for (auto const v : operands)
                _code.push_back(id_of_raw(v));
            return;
        }

        switch (i.code)
        {
        case op::math:
            return ext_inst(i, id);
        case op::extract:
        case op::insert:
        {
            auto const values = i.code == op::insert ? isize(2) : isize(1);
            head(_code, i.code == op::insert ? spv::CompositeInsert : spv::CompositeExtract, 3 + operands.size());
            _code.push_back(id_of(i.type));
            _code.push_back(id);
            for (auto k = isize(0); k < operands.size(); ++k)
                _code.push_back(k < values ? id_of_raw(operands[k]) : operands[k]);
            return;
        }
        case op::shuffle:
        {
            head(_code, spv::VectorShuffle, 3 + operands.size());
            _code.push_back(id_of(i.type));
            _code.push_back(id);
            for (auto k = isize(0); k < operands.size(); ++k)
                _code.push_back(k < 2 ? id_of_raw(operands[k]) : operands[k]);
            return;
        }
        case op::store:
            return emit(_code, spv::Store, {id_of_raw(operands[0]), id_of_raw(operands[1])});
        case op::array_length:
            return emit(_code, spv::ArrayLength, {id_of(i.type), id, id_of_raw(operands[0]), operands[1]});
        case op::atomic_load:
            return atomic(spv::AtomicLoad, i, id);
        case op::atomic_store:
            return atomic(spv::AtomicStore, i, id);
        case op::atomic_exchange:
            return atomic(spv::AtomicExchange, i, id);
        case op::atomic_compare_exchange:
            return atomic(spv::AtomicCompareExchange, i, id);
        case op::atomic_iadd:
            return atomic(spv::AtomicIAdd, i, id);
        case op::atomic_isub:
            return atomic(spv::AtomicISub, i, id);
        case op::atomic_smin:
            return atomic(spv::AtomicSMin, i, id);
        case op::atomic_umin:
            return atomic(spv::AtomicUMin, i, id);
        case op::atomic_smax:
            return atomic(spv::AtomicSMax, i, id);
        case op::atomic_umax:
            return atomic(spv::AtomicUMax, i, id);
        case op::atomic_and:
            return atomic(spv::AtomicAnd, i, id);
        case op::atomic_or:
            return atomic(spv::AtomicOr, i, id);
        case op::atomic_xor:
            return atomic(spv::AtomicXor, i, id);
        case op::atomic_fadd:
            require(_p.at(i.type).width == 64 ? spv::AtomicFloat64AddEXT : spv::AtomicFloat32AddEXT);
            require_extension("SPV_EXT_shader_atomic_float_add");
            return atomic(spv::AtomicFAddEXT, i, id);
        case op::control_barrier:
        {
            auto const is_workgroup = barrier(i.sub) == barrier::workgroup;
            auto const semantics = k_semantics_acquire_release
                                 | (is_workgroup ? k_semantics_workgroup : k_semantics_uniform | k_semantics_image);
            return emit(_code, spv::ControlBarrier,
                        {literal_id(k_scope_workgroup), literal_id(is_workgroup ? k_scope_workgroup : k_scope_device),
                         literal_id(semantics)});
        }
        case op::sample_lod:
            return emit(
                _code, spv::ImageSampleExplicitLod,
                {id_of(i.type), id, id_of_raw(operands[0]), id_of_raw(operands[1]), 0x2u, id_of_raw(operands[2])});
        case op::image_fetch:
            return emit(_code, spv::ImageFetch,
                        {id_of(i.type), id, id_of_raw(operands[0]), id_of_raw(operands[1]), i.aux != 0 ? 0x40u : 0x2u,
                         id_of_raw(operands[2])});
        case op::image_gather:
            return emit(_code, spv::ImageGather,
                        {id_of(i.type), id, id_of_raw(operands[0]), id_of_raw(operands[1]), id_of_raw(operands[2])});
        case op::image_write:
            return emit(_code, spv::ImageWrite, {id_of_raw(operands[0]), id_of_raw(operands[1]), id_of_raw(operands[2])});
        case op::image_size:
            require(spv::ImageQuery);
            if (i.aux != 0)
                return emit(_code, spv::ImageQuerySizeLod,
                            {id_of(i.type), id, id_of_raw(operands[0]), id_of_raw(operands[1])});
            return emit(_code, spv::ImageQuerySize, {id_of(i.type), id, id_of_raw(operands[0])});
        case op::subgroup:
            return group(i, id);
        case op::phi:
        {
            head(_code, spv::Phi, 3 + operands.size());
            _code.push_back(id_of(i.type));
            _code.push_back(id);
            for (auto k = isize(0); k < operands.size(); k += 2)
            {
                _code.push_back(id_of_raw(operands[k]));
                _code.push_back(id_of(block_ref(operands[k + 1])));
            }
            return;
        }
        default:
            CC_UNREACHABLE("an operation with no words");
        }
    }

    void terminator(ir::block const& b, instruction const& i)
    {
        auto const operands = _p.operands_of(i);
        if (b.merges == merge_kind::selection)
            emit(_code, spv::SelectionMerge, {id_of(b.merge), 0u});
        else if (b.merges == merge_kind::loop)
            emit(_code, spv::LoopMerge, {id_of(b.merge), id_of(b.continue_to), 0u});

        switch (i.code)
        {
        case op::branch:
            return emit(_code, spv::Branch, {id_of(block_ref(operands[0]))});
        case op::branch_conditional:
            return emit(_code, spv::BranchConditional,
                        {id_of_raw(operands[0]), id_of(block_ref(operands[1])), id_of(block_ref(operands[2]))});
        case op::switch_:
        {
            head(_code, spv::Switch, 1 + operands.size());
            _code.push_back(id_of_raw(operands[0]));
            _code.push_back(id_of(block_ref(operands[1])));
            for (auto k = isize(2); k < operands.size(); k += 2)
            {
                _code.push_back(operands[k]);
                _code.push_back(id_of(block_ref(operands[k + 1])));
            }
            return;
        }
        case op::return_:
            return emit(_code, spv::Return, {});
        default:
            return emit(_code, spv::Unreachable, {});
        }
    }

    void write_function()
    {
        emit(_code, spv::Function, {id_of(_void), _function, 0u, _function_type});
        auto const order = blocks_in_order(_p);
        for (auto k = isize(0); k < order.size(); ++k)
        {
            auto const& b = _p.blocks[isize(order[k])];
            emit(_code, spv::Label, {id_of(order[k])});
            if (k == 0)
                for (auto i = isize(0); i < _p.locals.size(); ++i)
                {
                    emit(_code, spv::Variable, {id_of(_p.locals[i].type), _locals + u32(i), spv::FunctionStorage});
                    name(_locals + u32(i), _p.name_at(_p.locals[i].name_first, _p.locals[i].name_size));
                }
            for (auto i = b.first; i + 1 < b.first + b.count; ++i)
                instruction_of(i);
            terminator(b, _p.instructions[b.first + b.count - 1]);
        }
        emit(_code, spv::FunctionEnd, {});
    }

    void write_entry()
    {
        auto const words = 3 + string_words(_p.entry_name) + _p.globals.size();
        head(_entry, spv::EntryPoint, words);
        _entry.push_back(5); // GLCompute
        _entry.push_back(_function);
        string_into(_entry, _p.entry_name);
        for (auto i = isize(0); i < _p.globals.size(); ++i)
            _entry.push_back(_globals + u32(i));
        emit(_modes, spv::ExecutionMode,
             {_function, 17u, u32(_p.workgroup[0]), u32(_p.workgroup[1]), u32(_p.workgroup[2])});
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

    [[nodiscard]] cc::vector<u32> assembled()
    {
        auto out = cc::vector<u32>();
        out.push_back(0x07230203u);
        out.push_back(0x00010600u); // SPIR-V 1.6, which Vulkan 1.3 takes
        out.push_back(0x00530047u); // a generator number of our own, unregistered
        out.push_back(_bound);
        out.push_back(0);
        for (auto const c : _capabilities)
            emit(out, spv::Capability, {c});
        for (auto const e : _extensions)
        {
            head(out, spv::Extension, 1 + string_words(e));
            string_into(out, e);
        }
        if (_uses_glsl)
        {
            constexpr auto text = cc::string_view("GLSL.std.450");
            head(out, spv::ExtInstImport, 2 + string_words(text));
            out.push_back(_glsl);
            string_into(out, text);
        }
        emit(out, spv::MemoryModel, {0u, 1u}); // Logical, GLSL450
        out.push_back_range(_entry);
        out.push_back_range(_modes);
        out.push_back_range(_names);
        out.push_back_range(_annotations);
        out.push_back_range(_declarations);
        out.push_back_range(_code);
        return out;
    }

    struct literal
    {
        u32 value = 0;
        u32 id = 0;
    };

    program const& _p;
    u32 _types = 0;
    u32 _constants = 0;
    u32 _globals = 0;
    u32 _locals = 0;
    u32 _instructions = 0;
    u32 _blocks = 0;
    u32 _glsl = 0;
    u32 _function = 0;
    u32 _function_type = 0;
    u32 _bound = 0;
    type_ref _uint = type_ref::none;
    type_ref _void = type_ref::none;
    bool _uses_glsl = false;
    cc::vector<u32> _capabilities;
    cc::vector<cc::string_view> _extensions;
    cc::vector<literal> _literals;
    cc::vector<u32> _entry;
    cc::vector<u32> _modes;
    cc::vector<u32> _names;
    cc::vector<u32> _annotations;
    cc::vector<u32> _declarations;
    cc::vector<u32> _code;
};
} // namespace

cc::vector<u32> sgl::ir::write_spirv(program const& p)
{
    return writer(p).run();
}
