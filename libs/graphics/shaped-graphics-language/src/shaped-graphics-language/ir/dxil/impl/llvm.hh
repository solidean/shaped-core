#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/fwd.hh>

/// An LLVM 3.7 module as flat tables, and its bitcode: all of LLVM DXIL needs, with no LLVM in the build.
///
/// The tables know LLVM and nothing of DXIL, so a new operation of DXIL is a table entry above this rather than a
/// change here, and only a new kind of LLVM instruction touches it.
namespace sgl::ir::dxil::impl
{
enum class ll_type : u32
{
    none = 0xffffffffu
};

/// A global, a constant or an instruction's result, told apart by its top bits until the writer numbers them.
enum class ll_value : u32
{
    none = 0xffffffffu
};

enum class ll_block : u32
{
    none = 0xffffffffu
};

/// LLVM's own numbers, as its bitcode states them.
enum class binary_op : u8
{
    add = 0,
    sub = 1,
    mul = 2,
    udiv = 3,
    sdiv = 4,
    urem = 5,
    srem = 6,
    shl = 7,
    lshr = 8,
    ashr = 9,
    bit_and = 10,
    bit_or = 11,
    bit_xor = 12,
};

enum class cast_op : u8
{
    trunc = 0,
    zext = 1,
    sext = 2,
    fptoui = 3,
    fptosi = 4,
    uitofp = 5,
    sitofp = 6,
    fptrunc = 7,
    fpext = 8,
    bitcast = 11,
};

enum class predicate : u8
{
    f_oeq = 1,
    f_ogt = 2,
    f_oge = 3,
    f_olt = 4,
    f_ole = 5,
    f_one = 6,
    f_ord = 7,
    f_uno = 8,
    f_ueq = 9,
    f_ugt = 10,
    f_uge = 11,
    f_ult = 12,
    f_ule = 13,
    f_une = 14,
    i_eq = 32,
    i_ne = 33,
    i_ugt = 34,
    i_uge = 35,
    i_ult = 36,
    i_ule = 37,
    i_sgt = 38,
    i_sge = 39,
    i_slt = 40,
    i_sle = 41,
};

enum class rmw_op : u8
{
    exchange = 0,
    add = 1,
    sub = 2,
    bit_and = 3,
    bit_or = 5,
    bit_xor = 6,
    max = 7,
    min = 8,
    umax = 9,
    umin = 10,
};

/// Fast-math flags, as a binary operation or a comparison carries them.
struct fp_flags
{
    static constexpr u8 unsafe_algebra = 1;
    static constexpr u8 no_nans = 2;
    static constexpr u8 no_infs = 4;
    static constexpr u8 no_signed_zeros = 8;
    static constexpr u8 allow_reciprocal = 16;
};

/// An attribute of the kinds DXIL declares its functions with: an enum of LLVM's, or a string pair.
struct ll_attribute
{
    /// LLVM's attribute number, or 0 for a string pair.
    u32 kind = 0;
    cc::string_view key;
    cc::string_view value;
};

inline constexpr u32 k_attr_noduplicate = 12;
inline constexpr u32 k_attr_noreturn = 17;
inline constexpr u32 k_attr_nounwind = 18;
inline constexpr u32 k_attr_readnone = 20;
inline constexpr u32 k_attr_readonly = 21;

/// A value's tag, which the writer's numbering and every pass read.
inline constexpr u32 k_ll_constant_bit = 0x80000000u;
/// The `sub` of an element pointer that `dx.nonuniform` marks.
inline constexpr u8 k_ll_nonuniform = 1;
inline constexpr u32 k_ll_global_bit = 0x40000000u;
inline constexpr u32 k_ll_index_mask = 0x3fffffffu;

[[nodiscard]] constexpr bool ll_is_constant(ll_value v)
{
    return v != ll_value::none && (u32(v) & k_ll_constant_bit) != 0;
}
[[nodiscard]] constexpr bool ll_is_global(ll_value v)
{
    return v != ll_value::none && (u32(v) & (k_ll_constant_bit | k_ll_global_bit)) == k_ll_global_bit;
}
/// A parameter of the module's one function, which carries both bits, so a pass takes it for a constant.
[[nodiscard]] constexpr bool ll_is_argument(ll_value v)
{
    return v != ll_value::none
        && (u32(v) & (k_ll_constant_bit | k_ll_global_bit)) == (k_ll_constant_bit | k_ll_global_bit);
}
[[nodiscard]] constexpr u32 ll_index(ll_value v)
{
    return u32(v) & k_ll_index_mask;
}
/// How LLVM states an alignment.
[[nodiscard]] constexpr u64 log2_plus_one(u32 alignment)
{
    auto result = u64(1);
    while (alignment > 1)
    {
        alignment >>= 1;
        ++result;
    }
    return result;
}

class module_builder
{
public:
    module_builder();

    // ---- types
    [[nodiscard]] ll_type void_type() const { return _void; }
    [[nodiscard]] ll_type bool_type() const { return _i1; }
    [[nodiscard]] ll_type byte_type() const { return _i8; }
    [[nodiscard]] ll_type int_type() const { return _i32; }
    [[nodiscard]] ll_type long_type() const { return _i64; }
    [[nodiscard]] ll_type float_type() const { return _f32; }
    [[nodiscard]] ll_type integer(u32 width);
    [[nodiscard]] ll_type floating(u32 width);
    [[nodiscard]] ll_type pointer(ll_type element, u32 address_space = 0);
    [[nodiscard]] ll_type array(ll_type element, u64 count);
    [[nodiscard]] ll_type function(ll_type result, cc::span<ll_type const> params);
    /// A struct by name; the same name is the same struct.
    [[nodiscard]] ll_type structure(cc::string_view name, cc::span<ll_type const> members);
    [[nodiscard]] bool is_float(ll_type t) const;
    [[nodiscard]] u32 bits_of(ll_type t) const;

    // ---- constants
    [[nodiscard]] ll_value constant_int(ll_type t, u64 bits);
    [[nodiscard]] ll_value constant_i32(u32 value) { return constant_int(_i32, value); }
    [[nodiscard]] ll_value constant_i8(u32 value) { return constant_int(_i8, value); }
    [[nodiscard]] ll_value constant_bool(bool value) { return constant_int(_i1, value ? 1u : 0u); }
    [[nodiscard]] ll_value constant_float(ll_type t, u64 bits);
    [[nodiscard]] ll_value constant_undef(ll_type t);
    [[nodiscard]] ll_value constant_null(ll_type t);
    [[nodiscard]] ll_value constant_struct(ll_type t, cc::span<ll_value const> fields);
    /// `bitcast (value to t)`, a constant of a global's address.
    [[nodiscard]] ll_value constant_bitcast(ll_type t, ll_value value);
    /// The bits of a constant int or float, for a pass that folds on sight.
    [[nodiscard]] bool constant_bits(ll_value v, u64& bits) const;

    // ---- globals
    /// An attribute list, by the position `add_function` takes; 0 is none.
    [[nodiscard]] u32 attributes(cc::span<ll_attribute const> list);
    [[nodiscard]] ll_value add_function(cc::string_view name, ll_type type, u32 attributes, bool is_declaration);
    [[nodiscard]] ll_value add_variable(cc::string_view name, ll_type value, u32 address_space, u32 alignment);
    /// `external constant`: what a library's resource is until the runtime binds it.
    [[nodiscard]] ll_value add_external_constant(cc::string_view name, ll_type value, u32 alignment);
    /// `internal constant`, holding `initializer`: a table the module states in full.
    [[nodiscard]] ll_value add_constant_table(cc::string_view name, ll_type value, ll_value initializer, u32 alignment);
    /// States the parameters of the module's one function, which precede every value of its body.
    void set_arguments(cc::span<ll_type const> types);
    [[nodiscard]] ll_value argument(u32 index) const { return ll_value(index | k_ll_constant_bit | k_ll_global_bit); }

    // ---- metadata; every id is a position plus one, 0 standing for a null operand
    [[nodiscard]] u32 md_string(cc::string_view text);
    [[nodiscard]] u32 md_value(ll_type t, ll_value v);
    [[nodiscard]] u32 md_i32(u32 value) { return md_value(_i32, constant_i32(value)); }
    [[nodiscard]] u32 md_i1(bool value) { return md_value(_i1, constant_bool(value)); }
    [[nodiscard]] u32 md_node(cc::span<u32 const> operands);
    void md_named(cc::string_view name, cc::span<u32 const> nodes);
    /// A specialized node, such as a `DIFile`: LLVM's record `code` and its fields as LLVM 3.7 writes them.
    [[nodiscard]] u32 md_record(u32 code, cc::span<u64 const> fields);

    // ---- debug info (EMIT-257), which only `write(..., with_debug_info)` states
    /// Where an instruction's debug location stands, and in what scope; ids are metadata ids, 0 for none.
    struct debug_location
    {
        u32 line = 0;
        u32 column = 0;
        u32 scope = 0;
        u32 inlined_at = 0;
    };
    /// The key every instruction added from here on carries, 0 for none: a position in `set_debug_locations` plus one.
    void set_location(u32 key) { _location = key; }
    /// Which keys an instruction still carries, after the passes, by key minus one.
    [[nodiscard]] cc::vector<bool> locations_used(u32 count) const;
    /// What each key stands for, by key minus one.
    void set_debug_locations(cc::vector<debug_location> locations) { _debug_locations = cc::move(locations); }
    /// The metadata and named metadata added from here on are the debug info's, which a write without it leaves out.
    /// So are the constants added from here on, which only the debug info's metadata may name.
    void begin_debug_metadata()
    {
        _debug_metadata_from = u32(_metadata.size());
        _debug_named_from = u32(_named.size());
        _debug_constants_from = u32(_constants.size());
    }

    // ---- the one function body; blocks are written in the order they are added
    /// Marks where the next function body starts: the blocks added from here on are its own.
    /// Bodies stand in the order of their functions' `add_function`; a module of one need not call it.
    void begin_function();
    [[nodiscard]] ll_block add_block();
    void open_block(ll_block b);
    [[nodiscard]] ll_value binary(binary_op op, ll_value a, ll_value b, u8 flags = 0);
    [[nodiscard]] ll_value cast(cast_op op, ll_value a, ll_type to);
    [[nodiscard]] ll_value compare(predicate p, ll_value a, ll_value b, u8 flags = 0);
    [[nodiscard]] ll_value select(ll_value condition, ll_value yes, ll_value no);
    [[nodiscard]] ll_value extract_value(ll_value aggregate, u32 index);
    /// `is_stable` says a call that reads memory reads what never changes, so two of it are one.
    [[nodiscard]] ll_value call(ll_value callee, ll_type type, cc::span<ll_value const> args, bool is_stable = false);
    /// `count` elements of `element`, whose size operand is an i32 constant.
    [[nodiscard]] ll_value alloca_of(ll_type allocated, u32 alignment);
    [[nodiscard]] ll_value load(ll_type t, ll_value pointer, u32 alignment);
    void store(ll_value pointer, ll_value value, u32 alignment);
    /// `is_nonuniform` attaches `dx.nonuniform`, which a library's index into an array of resources says it differs by.
    [[nodiscard]] ll_value element_pointer(ll_type source,
                                           ll_value base,
                                           cc::span<ll_value const> indices,
                                           bool is_nonuniform = false);
    [[nodiscard]] ll_value atomic_rmw(rmw_op op, ll_value pointer, ll_value value);
    [[nodiscard]] ll_value atomic_exchange(ll_value pointer, ll_value compare, ll_value desired);
    /// An empty phi of `t`, its incoming edges added once their values exist.
    [[nodiscard]] ll_value phi(ll_type t, isize edges);
    void set_phi_edge(ll_value phi, isize edge, ll_value value, ll_block from);
    void branch(ll_block to);
    void branch_if(ll_value condition, ll_block yes, ll_block no);
    void switch_on(ll_value selector, ll_block fallback, cc::span<u32 const> cases);
    void return_void();
    void unreachable();

    /// Gives two equal computations one value, where the first dominates the second.
    /// What reads memory counts only where `call` said that memory never changes, a block of constants above all.
    void share_common_values();
    /// Merges loads of one buffer whose lanes follow on in one block into one load of up to four lanes, as DXC does.
    /// Only a load of a buffer the shader cannot write merges, `call`'s `is_stable`, so no store stands between two.
    /// `opcode` is `rawBufferLoad`'s.
    void merge_buffer_loads(u32 opcode);

    /// Drops every instruction nothing with an effect reads, a loop of phis among them.
    /// Every `ll_value` of an instruction taken before this is void afterwards, and blocks keep their numbers.
    /// A declaration nothing calls any more keeps its value, and `write` leaves it out of the module.
    void remove_unused();

    [[nodiscard]] ll_type type_of(ll_value v) const;
    [[nodiscard]] ll_type element_of(ll_type pointer_or_array) const;
    [[nodiscard]] bool is_structure(ll_type t) const { return _types[isize(t)].code == type_code::structure; }
    /// A struct's name.
    [[nodiscard]] cc::string_view name_of(ll_type structure) const;
    [[nodiscard]] isize instruction_count() const { return _instructions.size(); }
    /// How many instructions call into DXIL's operations, and how many do anything else.
    [[nodiscard]] isize call_count() const { return _call_count; }
    /// Hands `visit` the arguments of every call of `callee` still in the module, as raw `ll_value`s.
    template <class Visit>
    void calls_of(ll_value callee, Visit&& visit) const
    {
        for (auto const& inst : _instructions)
            if (inst.code == inst_code::call && _args[inst.first] == u32(callee))
                visit(cc::span<u32 const>(_args.data() + inst.first + 1, isize(inst.count) - 1));
    }

    /// The module as bitcode, its magic first, `triple` and `layout` as LLVM's module states them.
    /// Without `with_debug_info` it is the same bytes whether or not the debug info was built.
    [[nodiscard]] cc::vector<byte> write(cc::string_view triple, cc::string_view layout, bool with_debug_info = false) const;

private:
    enum class type_code : u8
    {
        void_,
        integer,
        half,
        float_,
        double_,
        label,
        metadata,
        pointer,
        array,
        function,
        structure,
    };
    struct type_entry
    {
        type_code code = type_code::void_;
        /// An integer's width, a pointer's address space, an array's count.
        u64 number = 0;
        ll_type element = ll_type::none;
        /// A range of `_type_args`: a function's result then parameters, a struct's members.
        u32 first = 0;
        u32 count = 0;
        /// A range of `_names`, of a struct.
        u32 name_first = 0;
        u32 name_size = 0;
    };
    enum class constant_code : u8
    {
        integer,
        floating,
        null_,
        undefined,
        aggregate,
        bitcast,
    };
    struct constant_entry
    {
        ll_type type = ll_type::none;
        constant_code code = constant_code::integer;
        u64 bits = 0;
        /// A range of `_constant_args`, of an aggregate.
        u32 first = 0;
        u32 count = 0;
    };
    struct global_entry
    {
        /// The function's type, or the variable's value type.
        ll_type type = ll_type::none;
        ll_type pointer = ll_type::none;
        bool is_function = false;
        bool is_declaration = false;
        u32 address_space = 0;
        u32 alignment = 0;
        bool is_constant = false;
        /// A variable's value, where the module states it, which makes it internal.
        ll_value initializer = ll_value::none;
        u32 attributes = 0;
        u32 name_first = 0;
        u32 name_size = 0;
    };
    enum class md_code : u8
    {
        string,
        value,
        node,
        record,
    };
    struct md_entry
    {
        md_code code = md_code::string;
        /// A record's LLVM code.
        u32 record = 0;
        ll_type type = ll_type::none;
        ll_value value = ll_value::none;
        u32 first = 0;
        u32 count = 0;
    };
    struct named_md
    {
        u32 name_first = 0;
        u32 name_size = 0;
        u32 first = 0;
        u32 count = 0;
    };
    enum class inst_code : u8
    {
        binary,
        cast,
        compare,
        select,
        extract_value,
        call,
        alloca_,
        load,
        store,
        element_pointer,
        atomic_rmw,
        atomic_exchange,
        phi,
        branch,
        branch_if,
        switch_,
        return_void,
        unreachable,
    };
    struct inst_entry
    {
        inst_code code = inst_code::unreachable;
        /// What the code says of it: an opcode, a predicate.
        u8 sub = 0;
        /// A binary's or a comparison's flags, a memory access's alignment.
        u8 flags = 0;
        /// The result's, `_void` for none.
        ll_type type = ll_type::none;
        /// A range of `_args`.
        u32 first = 0;
        u32 count = 0;
        /// The debug location's key, 0 for none.
        u32 location = 0;
    };
    struct block_entry
    {
        u32 first = 0;
        u32 count = 0;
    };
    struct attribute_list
    {
        u32 first = 0;
        u32 count = 0;
    };

    [[nodiscard]] ll_type add_type(type_entry const& t, cc::span<ll_type const> args);
    [[nodiscard]] ll_value add_constant(constant_entry const& c, cc::span<ll_value const> args);
    [[nodiscard]] ll_value push(inst_code code, ll_type type, cc::span<u32 const> args, u8 sub = 0, u8 flags = 0);
    [[nodiscard]] u32 add_name(cc::string_view name);
    [[nodiscard]] bool has_effect(inst_entry const& inst) const;
    [[nodiscard]] bool may_share(inst_entry const& inst) const;
    /// What `binary` folds to for two known operands, or `none`.
    /// `v` multiplied, or divided when `op` is `sdiv`, by the constant `c` on the side `is_right` says, under `fast`'s
    /// reassociation; none where nothing simpler exists.
    /// Of a product's two operands, the one another recent product reads beside some other value; none where neither.
    [[nodiscard]] ll_value shared_factor(ll_value left, ll_value right) const;
    [[nodiscard]] ll_value reassociated(binary_op op, ll_value v, f32 c, bool is_right, u8 flags);
    [[nodiscard]] ll_value folded_binary(binary_op op, ll_value a, ll_value b, u8 flags);
    /// Calls `visit` with a reference to each of the instruction's operands that is a value, not a type or a block.
    template <class Visit>
    void for_each_value(inst_entry const& inst, Visit&& visit);
    [[nodiscard]] cc::string_view name_at(u32 first, u32 size) const;

    cc::vector<type_entry> _types;
    cc::vector<u32> _type_args;
    cc::vector<constant_entry> _constants;
    cc::vector<u32> _constant_args;
    /// An open table of positions in `_constants`, plus one.
    cc::vector<u32> _constant_slots;
    cc::vector<global_entry> _globals;
    cc::vector<ll_type> _arguments;
    /// The `!{i32 1}` of `dx.nonuniform`, made on first use.
    u32 _nonuniform_node = 0;
    cc::vector<ll_attribute> _attribute_items;
    cc::vector<attribute_list> _attribute_lists;
    cc::vector<md_entry> _metadata;
    cc::vector<u32> _md_args;
    /// The fields of every record, in `_metadata`'s order.
    cc::vector<u64> _md_fields;
    cc::vector<named_md> _named;
    /// Where the debug info's metadata starts; a write without it stops there.
    u32 _debug_metadata_from = 0xffffffffu;
    u32 _debug_named_from = 0xffffffffu;
    u32 _debug_constants_from = 0xffffffffu;
    cc::vector<debug_location> _debug_locations;
    u32 _location = 0;
    cc::vector<inst_entry> _instructions;
    cc::vector<u32> _args;
    /// Parallel to the instructions that give a value: their position.
    cc::vector<u32> _results;
    cc::vector<block_entry> _blocks;
    /// Where each function body after the first starts in `_blocks`.
    cc::vector<u32> _later_bodies;
    /// Blocks are filled in the order they open, so each one's run ends where the next opens.
    ll_block _open = ll_block::none;
    isize _call_count = 0;
    cc::string _names;

    ll_type _void = ll_type::none;
    ll_type _i1 = ll_type::none;
    ll_type _i8 = ll_type::none;
    ll_type _i32 = ll_type::none;
    ll_type _i64 = ll_type::none;
    ll_type _f32 = ll_type::none;
    ll_type _label = ll_type::none;
    ll_type _metadata_type = ll_type::none;
};
} // namespace sgl::ir::dxil::impl
