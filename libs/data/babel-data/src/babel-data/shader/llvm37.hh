#pragma once

#include <babel-data/fwd.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>

// LLVM 3.7 module reader (shader/): what a bitstream's records mean in the LLVM version DXIL froze.
//
// LLVM keeps no stable bitcode across versions, so each version family is a reader of its own over the one
// babel::llvm bitstream; this is 3.7's, enough for DXIL from DXC and from SGL's own writer.
// It reads the type table, the globals and functions, the constants, the value names, and each function body as
// instructions: a code, a sub-opcode, a result type, the value ids of its operands, the block it stands in.
// Metadata is not read beyond what a debug location names.
//
// VALUE IDS ARE LLVM'S OWN NUMBERING, made absolute: the module's globals and functions in the order they are
// declared, then its constants; then, per function, its arguments, its own constants and its instructions' results.
// So an operand names a global, a constant or an earlier result alike, and `module::value_of` says which.
//
//   auto const m = babel::llvm37::read(bitstream).value();
//   for (auto const& f : m.functions)
//       if (f.body >= 0)
//           for (auto const& i : m.bodies[f.body].instructions)
//               if (i.code == babel::llvm37::inst_code::call)
//                   names.push_back(m.callee_name(m.bodies[f.body], i));

enum class babel::llvm37::type_kind : babel::u8
{
    void_,
    half,
    float_,
    double_,
    label,
    metadata,
    integer,
    pointer,
    function,
    array,
    vector,
    structure,
    opaque,
    other,
};

/// A type of the table: the kind, its width or count, its element, and its members as a range of `module::members`.
/// A function type's element is its return type and its members its parameters.
struct babel::llvm37::type
{
    type_kind kind = type_kind::other;
    /// An integer's bits.
    u32 width = 0;
    /// An array's or a vector's elements.
    u64 count = 0;
    u32 element = 0;
    u32 address_space = 0;
    u32 first_member = 0;
    u32 member_count = 0;
    cc::string name;
};

/// The function block's record codes, LLVM 3.7's `FUNC_CODE_INST_*`.
enum class babel::llvm37::inst_code : babel::u16
{
    binop = 2,
    cast = 3,
    gep_old = 4,
    select = 5,
    extract_element = 6,
    insert_element = 7,
    shuffle_vector = 8,
    cmp = 9,
    ret = 10,
    br = 11,
    switch_ = 12,
    unreachable = 15,
    phi = 16,
    alloca_ = 19,
    load = 20,
    extract_value = 26,
    insert_value = 27,
    cmp2 = 28,
    vselect = 29,
    call = 34,
    fence = 36,
    atomic_rmw = 38,
    gep = 43,
    store = 44,
    cmpxchg = 46,
};

/// What a value id names.
enum class babel::llvm37::value_kind : babel::u8
{
    global_variable,
    function,
    constant,
    argument,
    instruction,
};

struct babel::llvm37::value
{
    value_kind kind = value_kind::constant;
    /// Its type, an index into `module::types`.
    u32 type = 0;
    /// An index into the table its kind names: globals, functions, constants, or the body's instructions.
    u32 index = 0;
};

/// A constant, as its record states it: LLVM's `CST_CODE_*`, and its operands.
struct babel::llvm37::constant
{
    u32 type = 0;
    u16 code = 0;
    u32 first = 0;
    u32 count = 0;
};

struct babel::llvm37::global_variable
{
    /// The value's type, not its pointer's.
    u32 type = 0;
    u32 address_space = 0;
    bool is_constant = false;
    /// The initializer's value id, or 0xffffffff for none.
    u32 initializer = 0xffffffffu;
    u32 value_id = 0;
    cc::string name;
};

struct babel::llvm37::function
{
    /// Its function type.
    u32 type = 0;
    bool is_declaration = false;
    u32 value_id = 0;
    cc::string name;
    /// An index into `module::bodies`, or -1 for a declaration.
    i32 body = -1;
};

/// One instruction of a body.
struct babel::llvm37::instruction
{
    inst_code code = inst_code::unreachable;
    /// A binop's opcode, a cast's, a comparison's predicate, an atomic's operation.
    u32 sub = 0;
    /// A binop's or a comparison's flags.
    u32 flags = 0;
    /// The result's type, or the void type for an instruction that gives none.
    /// A getelementptr's is its base pointer's: the indexed element's pointer type is not looked up.
    u32 type = 0;
    /// The value id of its result, or 0xffffffff for none.
    u32 result = 0xffffffffu;
    /// A range of `body::operands`: value ids, but for a phi's blocks and a branch's, a switch's and the literals a
    /// record states, which `operand_roles` names.
    u32 first = 0;
    u32 count = 0;
    /// The block it stands in.
    u32 block = 0;
    /// Its debug location: a line and a column, 0 for none, and the scope's metadata id.
    u32 line = 0;
    u32 column = 0;
    u32 scope = 0;
};

/// What each operand of an instruction is, in the order `instruction::first` lists them.
enum class babel::llvm37::operand_role : babel::u8
{
    value,
    block,
    literal,
};

/// A function's body: its instructions in block order, and the value ids they read.
struct babel::llvm37::body
{
    u32 function = 0;
    u32 block_count = 0;
    /// The value id of its first argument; its constants and results follow its arguments.
    u32 first_value = 0;
    cc::vector<instruction> instructions;
    cc::vector<u32> operands;
    cc::vector<operand_role> roles;
    /// The function's own values, from `first_value` on.
    cc::vector<value> values;
};

/// A module, read once.
struct babel::llvm37::module
{
    cc::string triple;
    cc::string data_layout;
    cc::vector<type> types;
    cc::vector<u32> members;
    cc::vector<global_variable> globals;
    cc::vector<function> functions;
    cc::vector<constant> constants;
    cc::vector<u64> constant_operands;
    /// The module's values, from id 0: globals and functions as declared, then constants.
    cc::vector<value> values;
    cc::vector<body> bodies;

    /// What value id `id` names in `b`: the module's or the body's.
    [[nodiscard]] value value_of(body const& b, u32 id) const;
    /// The name of the function a call calls, or empty where its callee is no function.
    [[nodiscard]] cc::string_view callee_name(body const& b, instruction const& call) const;
    [[nodiscard]] cc::span<u32 const> operands_of(body const& b, instruction const& i) const
    {
        return cc::span<u32 const>(b.operands).subspan({.offset = isize(i.first), .size = isize(i.count)});
    }
    /// The integer a constant states, where it is one.
    [[nodiscard]] bool integer_of(u32 constant_index, i64& out) const;
};

namespace babel::llvm37
{
/// An error where the stream holds no module block, or a record states what 3.7 cannot mean.
[[nodiscard]] cc::result<module> read(llvm::bitstream const& stream);
} // namespace babel::llvm37
