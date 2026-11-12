#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/ir/ir.hh>
#include <shaped-graphics-language/ir/spirv/impl/spv.hh>

#include <initializer_list>

/// One `ir::program` as a SPIR-V module, each logical section of it a list of words until `assembled` lays them out.
///
/// writer.cc holds the walk, the control flow and the module's frame, writer_declarations.cc types, constants, globals
/// and the entry point, and writer_operations.cc the operations.
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
    void forget_line() { _line_file = -1; }
    /// Gives each file a line named its `OpString` and `OpSource`, the last ids of the module, so no other id moves.
    void declare_files();

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
    u32 _types = 0;
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
