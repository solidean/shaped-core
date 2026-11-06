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

    // ---- writer_declarations.cc
    [[nodiscard]] static u32 storage_of(storage s);
    [[nodiscard]] bool holds_16_bit(type_ref t) const;
    void write_types();
    void write_structure(ir::type const& t, u32 id);
    void write_image(ir::type const& t, u32 id);
    void write_constants();
    void write_globals();
    void write_entry();

    // ---- writer_operations.cc
    void ext_inst(instruction const& i, u32 id);
    void atomic(u32 opcode, instruction const& i, u32 id);
    void group(instruction const& i, u32 id);
    void instruction_of(u32 index);

    // ---- writer.cc
    void terminator(ir::block const& b, instruction const& i);
    void write_function();
    [[nodiscard]] cc::vector<u32> assembled();

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
} // namespace sgl::ir::spirv::impl
