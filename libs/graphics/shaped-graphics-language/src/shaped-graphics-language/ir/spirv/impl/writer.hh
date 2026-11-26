#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/ir/ir.hh>
#include <shaped-graphics-language/ir/spirv/impl/spv.hh>

#include <initializer_list>

/// One `ir::program` as a SPIR-V module, each logical section of it a list of words until `assembled` lays them out.
///
/// writer.cc holds the walk, the control flow and the module's frame, writer_declarations.cc types, constants, globals
/// and the entry point, writer_operations.cc the operations, writer_lines.cc and writer_variables.cc the debug info.
/// Every table a new case adds a row to stands in the file of its topic.
namespace sgl::ir::spirv::impl
{
class writer
{
public:
    explicit writer(program const& p) : _p(p) {}

    [[nodiscard]] cc::vector<u32> run();

private:
    void number_everything();

    [[nodiscard]] u32 id_of(type_ref t) const { return _type_ids[isize(t)]; }
    [[nodiscard]] u32 id_of(block_ref b) const { return _blocks + u32(b); }
    [[nodiscard]] u32 id_of(value_ref v) const
    {
        if (is_constant(v))
            return _constants + index_of(v);
        if (is_global(v))
            return _globals + index_of(v);
        if (is_local(v))
            return _locals + index_of(v);
        return _instruction_ids[u32(v)];
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

    static void string_into(cc::vector<u32>& out, cc::string_view text);
    [[nodiscard]] static isize string_words(cc::string_view text) { return text.size() / 4 + 1; }

    void name(u32 id, cc::string_view text);
    void require(u32 capability);
    void require_extension(cc::string_view text);

    /// A `uint` constant the writer itself needs, such as a scope or an array's length.
    [[nodiscard]] u32 literal_id(u32 value);
    /// A constant, or a `construct` of constants as the one `OpConstantComposite` an image operand must name.
    [[nodiscard]] u32 constant_composite(value_ref v);
    /// The constant of `type` a table's leaves state from `at` on, which moves past them.
    [[nodiscard]] u32 table_constant(type_ref type, cc::span<u32 const> leaves, isize& at);

    // ---- writer_lines.cc (EMIT-257)
    /// States the line of instruction `index` before it, where the line table has one and it changed.
    void line_before(u32 index);
    /// Forgets the line stated, which a block's label ends.
    void forget_line()
    {
        _line_file = -1;
        _debug_scope_stated = 0;
        _debug_inlined_stated = 0;
    }
    /// Gives each file a line named its `OpString` and `OpSource`, the last ids of the module, so no other id moves.
    void declare_files();

    // ---- writer_variables.cc (EMIT-267)
    /// At `full` the module carries `NonSemantic.Shader.DebugInfo.100` in place of `OpLine`: scopes, inlined calls,
    /// lines and variables, as DXC's `-fspv-debug=vulkan-with-source` writes them.
    [[nodiscard]] bool writes_debug_info() const { return _p.describes_variables && !_p.lines.is_empty(); }
    /// A debug id named before the module's other ids are all taken, which `declare_debug_info` gives its number.
    [[nodiscard]] u32 debug_handle() { return k_debug_handle | _debug_handles++; }
    /// The handle of the `uint` constant `value` a debug instruction names.
    [[nodiscard]] u32 debug_literal(u32 value);
    /// An `OpExtInst` of the debug set into `out`, its handles noted in `patches` to be numbered at the end.
    void debug_instruction(cc::vector<u32>& out,
                           cc::vector<isize>& patches,
                           u32 instruction,
                           std::initializer_list<u32> operands);
    void debug_instruction(cc::vector<u32>& out,
                           cc::vector<isize>& patches,
                           u32 result,
                           u32 instruction,
                           cc::span<u32 const> operands);
    /// An `OpString` in the debug section, by handle.
    [[nodiscard]] u32 debug_string(cc::string_view text);
    /// The handle of the scope statement place `place` stands in, and of the chain of calls it was inlined through.
    /// The innermost scope `place` stands in: its block, or its function's body.
    [[nodiscard]] u32 debug_scope_of(source_place const& place);
    [[nodiscard]] u32 debug_function_scope_of(source_place const& place);
    /// Block `block` of the line table, in the function scope `function`, made with its parents on first use.
    [[nodiscard]] u32 debug_block(i32 block, u32 function);
    [[nodiscard]] u32 debug_scope(i32 file, i32 function);
    [[nodiscard]] u32 debug_inlined_at(source_place const& place);
    /// `DebugScope` and `DebugLine` for instruction `index`, where they changed.
    void debug_line_before(u32 index);
    /// The function's scope and `DebugFunctionDefinition`, at the head of its first block.
    void debug_function_head();
    /// A `debug_value` as a `DebugValue`, or a `debug_declare` as a `DebugDeclare`, where the value is written.
    void describe(u32 index);
    /// The handle of `t`'s `DebugType*`; 0 for a type a debugger shows no value of.
    [[nodiscard]] u32 debug_type(type_ref t);
    [[nodiscard]] u32 leaves_of(type_ref t) const;
    /// The indexes from variable type `t` down to the part of type `part` that begins at leaf `leaf`; false where none.
    [[nodiscard]] bool debug_indexes(type_ref t, u32 leaf, type_ref part, cc::vector<u32>& indexes) const;
    /// Writes every global debug instruction, gives each handle its id past every other, and patches them in.
    void declare_debug_info();

    // ---- writer_declarations.cc
    [[nodiscard]] u32 storage_of(storage s) const;
    [[nodiscard]] bool holds_16_bit(type_ref t) const;
    void write_types();
    void write_structure(ir::type const& t, u32 id);
    void write_image(ir::type const& t, u32 id);
    void write_constants();
    [[nodiscard]] u32 space_of(stage_element const& e) const;
    void write_stage_elements();
    [[nodiscard]] u32 pointer_to(u32 space, u32 pointee);
    void write_primitive_modes();
    void write_interpolation(u32 id, interpolation mode);
    void write_globals();
    void write_entry();

    // ---- writer_operations.cc
    void ext_inst(instruction const& i, u32 id);
    void atomic(u32 opcode, instruction const& i, u32 id);
    void group(instruction const& i, u32 id);
    void ray_query(instruction const& i, u32 id);
    void hit_object(instruction const& i, u32 id);
    void instruction_of(u32 index);
    /// Finds each vector `fmul` of a splat of one scalar, which `OpVectorTimesScalar` writes without building the splat.
    void find_scalings();
    /// Tells the target that `i`, of id `id`, came through an index that differs between invocations.
    void nonuniform(instruction const& i, u32 id);
    /// A storage image of a format SPIR-V has no name for, `bgra8_unorm`, which is `Unknown` (EMIT-98).
    [[nodiscard]] bool is_unknown_format(value_ref image) const;
    /// What a load or a store of an element goes through, `index` naming a control point or a vertex where it has one.
    [[nodiscard]] u32 element_pointer(u32 element, u32 index = 0);
    /// `TessCoord`, which is three lanes where a parameter may take two.
    void tess_coord(instruction const& i, u32 id);
    /// `@position` in a pixel stage, whose `w` HLSL reads as the reciprocal of what `FragCoord` holds.
    void frag_coord(instruction const& i, u32 id);

    // ---- writer.cc
    void terminator(ir::block const& b, instruction const& i);
    void write_function();
    /// A control stage's entry point: hands each control point on, then runs the body once per patch.
    void write_control_entry();
    [[nodiscard]] cc::vector<u32> assembled();

    struct literal
    {
        u32 value = 0;
        u32 id = 0;
    };
    struct element_pointer_type
    {
        u32 space = 0;
        u32 pointee = 0;
        u32 id = 0;
    };

    program const& _p;
    /// Parallel to `program::types`: each one's id, 0 for one a debug variable alone names (EMIT-267).
    cc::vector<u32> _type_ids;
    u32 _constants = 0;
    u32 _elements = 0;
    /// Parallel to `program::stage_elements`: the variable each reads, one per element but a shared tessellation factor.
    cc::vector<u32> _element_variables;
    /// Parallel to it: the pointer to one lane of an element that stands in an array, or 0.
    cc::vector<u32> _element_lanes;
    /// Every pointer an element took, since SPIR-V declares a pointer type once.
    cc::vector<element_pointer_type> _pointer_types;
    /// A control stage's `InvocationId`, which no element holds.
    u32 _invocation = 0;
    /// A mesh stage's array of each primitive's vertices, which no element holds, and the pointer to one of them.
    u32 _primitive_indices = 0;
    u32 _primitive_lane = 0;
    u32 _globals = 0;
    u32 _locals = 0;
    u32 _instructions = 0;
    /// Parallel to `_p.instructions`: each one's id, counted past the debug operations, which take none, so describing
    /// variables moves no other id (EMIT-267).
    cc::vector<u32> _instruction_ids;
    u32 _blocks = 0;
    u32 _glsl = 0;
    /// The entry point, and the function the program is written as, which only a control stage tells apart.
    u32 _function = 0;
    u32 _body = 0;
    u32 _function_type = 0;
    u32 _bound = 0;
    type_ref _uint = type_ref::none;
    type_ref _void = type_ref::none;
    bool _uses_glsl = false;
    /// A quad vote was written, so a pixel entry point states `QuadDerivativesKHR` and `MaximallyReconvergesKHR`.
    bool _uses_quad_control = false;
    cc::vector<u32> _capabilities;
    cc::vector<cc::string_view> _extensions;
    cc::vector<literal> _literals;
    /// What holds `NonUniform` already, which an index shared by two chains would otherwise take twice.
    cc::vector<u32> _nonuniform;
    cc::vector<u32> _entry;
    cc::vector<u32> _modes;
    cc::vector<u32> _names;
    /// `OpString` and `OpSource` of every file a line names, which stand ahead of the names.
    cc::vector<u32> _sources;
    /// Where in `_code` each `OpLine` names its file, by the line table's position until `declare_files`.
    cc::vector<isize> _line_files;
    /// The line last stated in the block being written; -1 for none.
    i32 _line_file = -1;
    i32 _line = 0;
    // ---- EMIT-267, at `full`
    static constexpr u32 k_debug_handle = 0x80000000u;
    struct debug_scope_info
    {
        i32 file = -1;
        /// A position in `line_table::functions`; -1 for the entry point, -2 for an inlined body no function names.
        i32 function = -1;
        i32 line = 0;
        u32 handle = 0;
    };
    struct debug_inlined
    {
        i32 line = 0;
        u32 scope = 0;
        u32 parent = 0;
        u32 handle = 0;
    };
    struct debug_variable_info
    {
        u32 handle = 0;
        u32 scope = 0;
    };
    struct debug_type_info
    {
        u32 handle = 0;
        bool is_made = false;
    };
    u32 _debug_handles = 0;
    u32 _debug_set = 0;
    u32 _debug_expression = 0;
    /// Per file of the line table: its `DebugSource`, 0 until a line names it.
    cc::vector<u32> _debug_sources;
    cc::vector<debug_scope_info> _debug_scopes;
    /// Parallel to `line_table::blocks` once one is used: its `DebugLexicalBlock` and its parent scope, 0 before.
    struct debug_block_info
    {
        u32 handle = 0;
        u32 parent = 0;
    };
    cc::vector<debug_block_info> _debug_blocks;
    cc::vector<debug_inlined> _debug_inlined;
    /// Per `program::debug_variables`: its `DebugLocalVariable`, 0 until a value is stated of it.
    cc::vector<debug_variable_info> _debug_variables;
    cc::vector<debug_type_info> _debug_types;
    cc::vector<literal> _debug_literals;
    /// `OpString`s of the debug info, by handle, which `declare_debug_info` writes ahead of the names.
    cc::vector<cc::string_view> _debug_string_texts;
    cc::vector<u32> _debug_string_handles;
    /// The global debug instructions, written last, and where in them and in `_code` a handle stands.
    cc::vector<u32> _debug_declarations;
    cc::vector<isize> _debug_code_patches;
    /// Each type's instructions as `debug_type` makes them, which stand after the sources and the unit.
    cc::vector<u32> _debug_type_words;
    cc::vector<isize> _debug_type_patches;
    u32 _debug_unit = 0;
    /// The entry point's `DebugSource`, which a type names before the entry point's file is known.
    u32 _debug_unit_source = 0;
    /// The import's id, once handles have theirs.
    u32 _debug_set_id = 0;
    /// The scope and inlined call last stated in the block being written, and its line; 0 and -1 for none.
    u32 _debug_scope_stated = 0;
    u32 _debug_inlined_stated = 0;
    /// Debug operations among a block's phis, which stand after the last of them.
    cc::vector<u32> _debug_pending;

    cc::vector<u32> _annotations;
    cc::vector<u32> _declarations;
    cc::vector<u32> _code;
    /// Per instruction: which operand of a scaling `fmul` is the splat, `k_no_splat` for none.
    cc::vector<u8> _splat_operand;
    /// Per instruction: a splat only scalings read, which is never written.
    cc::vector<bool> _unwritten;
    static constexpr u8 k_no_splat = 2;
};
} // namespace sgl::ir::spirv::impl
