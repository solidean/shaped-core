#pragma once

#include <clean-core/container/set.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/memory/shared_ptr.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics-language/builtins/registry.hh>
#include <shaped-graphics-language/check/flat.hh>
#include <shaped-graphics-language/check/layered.hh>
#include <shaped-graphics-language/check/symbols.hh>
#include <shaped-graphics-language/source/diagnostic.hh>

/// A second place a diagnostic points at, with what to read there: the declaration a use conflicts with, a way to fix it.
struct sgl::check::related_note
{
    /// A position in the files `check` was given, as `located_diagnostic::file` is.
    i32 file = 0;
    source_span where;
    cc::string message;

    bool operator==(related_note const&) const = default;
};

/// One replacement of source text, in the file of the diagnostic that suggests it.
struct sgl::check::fix_edit
{
    source_span where;
    cc::string text;

    bool operator==(fix_edit const&) const = default;
};

/// An edit that makes a diagnostic go away, which an editor offers as a quick fix.
/// Its edits never overlap, and applying them all is the fix.
struct sgl::check::suggested_fix
{
    /// What the fix does, as a menu shows it: "Convert each operand first".
    cc::string title;
    cc::vector<fix_edit> edits;

    [[nodiscard]] bool operator==(suggested_fix const& rhs) const
    {
        return title == rhs.title && ast::impl::is_equal(edits, rhs.edits);
    }
};

/// A diagnostic of the check pass.
/// A module has several files, so the span alone does not say where it points.
struct sgl::check::located_diagnostic
{
    diagnostic what;
    /// The file `what.where` points into, as a position in the files `check` was given.
    i32 file = 0;
    /// What the kind alone cannot say: the construct that is unsupported, the name that is unknown, the loop of a cycle.
    /// For a reader; the kind and the span are what a tool keys on.
    cc::string detail;
    /// In the order they should be read, each printed as a `note:` line after the diagnostic.
    cc::vector<related_note> notes;
    /// The entry points whose inlined body it was judged of, by function, which a compile of another one leaves out
    /// (CHK-402).
    /// Empty for a diagnostic of the program, which every compile of it reports.
    cc::vector<symbol_id> entry_points;
    /// The edits that would fix it, where the checker knows them.
    cc::vector<suggested_fix> fixes;

    [[nodiscard]] bool operator==(located_diagnostic const& rhs) const
    {
        return what == rhs.what && file == rhs.file && detail == rhs.detail && ast::impl::is_equal(notes, rhs.notes)
            && ast::impl::is_equal(entry_points, rhs.entry_points) && ast::impl::is_equal(fixes, rhs.fixes);
    }
};

/// What an `@expect` argument says a test does.
enum class sgl::check::expectation_kind : sgl::u8
{
    /// `.fail`: the run fails, by a check or an assert.
    fail,
    /// `.assert`: the run stops at a false `assert`.
    assert_,
    /// `.discard`: the run ends at a `discard` (CHK-278).
    discard,
    /// `error = "kind"`: a diagnostic of that kind, a normal or a fatal error, stands in the test.
    error,
    /// `warning = "kind"`: the same, of a warning.
    warning,
};

struct sgl::check::test_expectation
{
    expectation_kind kind = expectation_kind::fail;
    /// The kind name a diagnostic must have, where `*` stands for any run of characters: `type-*`.
    cc::string pattern;
    /// The argument, which an unmet expectation is reported at.
    source_span where;

    bool operator==(test_expectation const&) const = default;
};

/// An entry point's `@expect(error = "kind")` or `@expect(warning = "kind")`, which takes the diagnostics judged of its
/// inlined body (CHK-403).
struct sgl::check::entry_expectation
{
    /// The entry point's function.
    symbol_id function = symbol_id::none;
    i32 file = 0;
    test_expectation expectation;

    bool operator==(entry_expectation const&) const = default;
};

/// One `test` of the module, wherever it stands.
struct sgl::check::test_info
{
    symbol_id symbol = symbol_id::none;
    i32 file = 0;
    ast::decl_id declaration = ast::decl_id::none;
    /// The `test` keyword, which is where the test is named in a report.
    source_span where;
    /// From the keyword to the end of the body: a diagnostic inside it is the test's, which `@expect` may name.
    source_span extent;
    /// What stands around it, for a reader: empty at file scope, `struct light`, `fun shade`.
    cc::string scope_path;
    /// The text of a `//` comment on the `test` line or on the line above it, which says what the test is for.
    cc::string comment;
    /// A position in `checked_module::test_units`, -1 for a test with no flat tree.
    i32 unit = -1;
    /// Its `@expect` arguments, in the order written.
    cc::vector<test_expectation> expectations;

    /// True where an expectation names a diagnostic, which makes the test one that is judged by them and never run.
    [[nodiscard]] bool expects_diagnostics() const
    {
        for (auto const& e : expectations)
            if (e.kind == expectation_kind::error || e.kind == expectation_kind::warning)
                return true;
        return false;
    }

    [[nodiscard]] bool operator==(test_info const& rhs) const
    {
        return symbol == rhs.symbol && file == rhs.file && declaration == rhs.declaration && where == rhs.where
            && extent == rhs.extent && scope_path == rhs.scope_path && comment == rhs.comment && unit == rhs.unit
            && ast::impl::is_equal(expectations, rhs.expectations);
    }
};

/// One module after name resolution, type checking and evaluation, as one value beside the ASTs it was checked from.
///
/// It has two readers.
/// An editor reads `files`, the side tables over the untouched AST, and `symbols`.
/// An emitter reads `entry_points` with `types`, `members` and `bindings`, and never the AST.
/// One alternative of a function body in a target's own text, a tagged string such as `"hlsl` (CHK-427).
/// Its run is the tagged strings that stand together as one body, one per language, and `first` is the first of them.
struct sgl::check::target_text
{
    symbol_id function = symbol_id::none;
    i32 file = 0;
    ast::stmt_id first = ast::stmt_id::none;
    shading_language language = shading_language::sgl;
    /// Where the text's first line stands in `file`, which a `#line` ahead of the text names (EMIT-182).
    u32 offset = 0;
    /// One more than `slots`: the text before each placeholder, then the text after the last.
    /// Its lines end in `\n`, one for each line of the source, so the text's lines stay the source's.
    cc::vector<cc::string> pieces;
    /// Parallel to the placeholders: the position of the parameter each names, and -1 for `$result`.
    cc::vector<i32> slots;

    [[nodiscard]] bool operator==(target_text const& rhs) const
    {
        return function == rhs.function && file == rhs.file && first == rhs.first && language == rhs.language
            && offset == rhs.offset && ast::impl::is_equal(pieces, rhs.pieces) && ast::impl::is_equal(slots, rhs.slots);
    }
};

/// A file's text in a target's own language, written once ahead of an entry point that writes a body of that file in
/// it (CHK-428).
struct sgl::check::target_preamble
{
    i32 file = 0;
    shading_language language = shading_language::sgl;
    /// Whole lines, each ending in `\n`.
    cc::string text;

    [[nodiscard]] bool operator==(target_preamble const&) const = default;
};

/// One node of a condition a compile decides from its target and its device alone, a text type's (CHK-432).
/// A condition is a list of them whose last is the root, and an operand is a position before its node.
struct sgl::check::target_condition
{
    enum class kind_t : u8
    {
        /// `target_api == .value`, a `graphics_api`.
        api_is,
        /// `target_language == .value`, a `shading_language`.
        language_is,
        /// `has(.value)`, a `feature`.
        has,
        /// `true`.
        always,
        negated,
        both,
        either,
    };
    kind_t kind = kind_t::always;
    i32 value = 0;
    i32 lhs = -1;
    i32 rhs = -1;

    constexpr bool operator==(target_condition const&) const = default;
};

/// Where one field of a struct with a target layout stands in a branch's record (CHK-526).
struct sgl::check::layout_place
{
    /// The member of the branch's struct that holds it; -1 for an `@absent` field, which none holds.
    i32 member = -1;
    /// For a `@bits` field, its lowest bit within that member's word and how many it takes; a `bits` of 0 is the whole
    /// member.
    i32 offset = 0;
    i32 bits = 0;

    constexpr bool operator==(layout_place const&) const = default;
};

/// One branch of a struct whose body is a `const if` chain, a text type (CHK-432), in the order the chain declares them.
/// An SGL branch is the struct's fields; any other spells the type in each language it is written in.
struct sgl::check::target_type_branch
{
    symbol_id type_symbol = symbol_id::none;
    /// Empty for the closing `else`.
    cc::vector<target_condition> condition;
    bool is_fields = false;
    /// Parallel: the languages it is spelled in, and the spelling in each.
    cc::vector<shading_language> languages;
    cc::vector<cc::string> spellings;
    /// Of a struct with a target layout (CHK-522): the struct of this branch's fields in its order, which a compile taking
    /// it writes wherever the type stands.
    type_id layout = type_id::none;
    /// Parallel to the fields the check judged: where each stands in `layout` (CHK-526).
    cc::vector<layout_place> places;

    [[nodiscard]] bool operator==(target_type_branch const& rhs) const
    {
        return type_symbol == rhs.type_symbol && ast::impl::is_equal(condition, rhs.condition)
            && is_fields == rhs.is_fields && ast::impl::is_equal(languages, rhs.languages)
            && ast::impl::is_equal(spellings, rhs.spellings) && layout == rhs.layout
            && ast::impl::is_equal(places, rhs.places);
    }
};

struct sgl::check::checked_module
{
    /// The module-level declarations of every file, in file order and then in source order.
    layered<symbol> symbols;
    /// Canonical and deduplicated; `types[0]` is the error type and `types[1]` is `void`.
    layered<type_info> types;
    /// The fields of every struct and the members of every binding.
    layered<member_info> members;
    /// The cases of every enum.
    layered<enum_case_info> enum_cases;
    layered<function_info> functions;
    layered<parameter> parameters;
    layered<binding_info> bindings;
    /// The binding lists of the functions, the layouts of the pipelines, and the shaders of the ray-tracing ones.
    layered<symbol_id> binding_lists;
    /// The type parameters of the generic functions, and what each generic call deduced them as.
    layered<type_id> type_lists;
    /// The static samplers a binding declares, which its members name by `member_info::static_sampler`.
    layered<sampler_state> samplers;
    /// Only the pipelines that checked without an error.
    layered<pipeline_info> pipelines;
    layered<pipeline_setting> pipeline_settings;
    /// The value of every `const` that checked.
    layered<constant_info> constants;
    /// Every place the program names an option, in the order the pass met them (CHK-355).
    layered<option_use> option_uses;
    /// How each resolved call fills its callee's parameters; `file_tables::call_of` points in here.
    layered<call_record> call_records;
    layered<written_argument> written_arguments;
    layered<i32> call_slots;
    /// Every instantiation of a generic function the check met (CHK-338).
    layered<function_instance> function_instances;
    /// Every candidate of every call that reported `no-matching-overload`, with the reason it did not match.
    layered<near_miss> near_misses;
    /// Every trace of a ray-tracing pipeline's ray type: the call, and which ray of which set (CHK-329).
    layered<ray_trace> ray_traces;
    /// Every call of a callable by its table's index (CHK-344).
    layered<callable_call> callable_calls;
    /// Every assignment to an image's texel (CHK-367).
    layered<texel_store> texel_stores;
    /// Every function body in a target's own text, one entry per language it is written in (CHK-427).
    layered<target_text> target_texts;
    /// Every file's preamble per language (CHK-428).
    layered<target_preamble> target_preambles;
    /// Every branch of every text type, each type's in the order its chain declares them (CHK-432).
    layered<target_type_branch> target_type_branches;

    /// One entry per file checked: the prelude's, then the library files the program reaches, then the program's.
    layered<file_tables> files;
    /// How many of `files` are the prelude's.
    i32 prelude_files = 0;
    /// Parallel to the files between the prelude and the program: each one's position in the library `check` was given.
    /// A library file no `use` reaches is not checked, and has no entry, unless the check continued from a layer of the
    /// whole library (`check_library`), which holds every module.
    cc::vector<i32> library_files;
    /// The library files the program reaches through `use`, as positions in the library, in the order they are checked.
    /// What an edit of a library file changes the program for; it is `library_files` unless the check continued from a
    /// layer of the whole library, whose other files are checked and are none of the program's.
    cc::vector<i32> reached_library_files;
    /// Parallel to `files`: the module each file belongs to; empty for the prelude's and for a program that names none.
    cc::vector<cc::string> file_modules;

    /// Only the entry points that checked without an error; a broken one has diagnostics and no flat tree.
    cc::vector<flat_entry_point> entry_points;
    /// The `@internal` entry points of the prelude, which no program lists and a polyfill compiles by name (CHK-527).
    /// Checked once with the prelude, so every module behind it shares them.
    layered<flat_entry_point> internal_entry_points;

    /// Every test, in the order the pass found them: file scope and type bodies first, then the tests of function bodies.
    layered<test_info> tests;
    /// The flat trees of the tests that checked without an error, each of stage `none` and without a parameter.
    layered<flat_entry_point> test_units;
    /// Every entry point's expectation of a diagnostic; such an entry point is judged by them, and has no flat tree.
    cc::vector<entry_expectation> entry_expectations;

    /// In the order the demand-driven pass found them, which is deterministic and not source order.
    cc::vector<located_diagnostic> diagnostics;

    /// Every module-level name, as each flat tree's mint takes it through `name_mint::also_taken` rather than a copy.
    /// A tree keeps pointing at the set it was made with, so a set is never changed and a copy of the module shares it;
    /// a symbol added later makes a new one, the last.
    cc::vector<cc::shared_ptr<cc::set<cc::string>>> module_names;

    /// The registry every `builtin_id` and `builtin_type_id` in here is a position in.
    /// It must outlive the module; `builtins::default_registry()` lives as long as the process.
    builtins::registry const* builtins = nullptr;

    static constexpr type_id error_type = type_id(0);
    /// `types[1]`: what a function without a return type returns.
    static constexpr type_id void_type = type_id(1);

    [[nodiscard]] i32 prelude_file_count() const { return prelude_files; }
    /// The program's file, which is always the last.
    [[nodiscard]] i32 program_file() const { return i32(files.size()) - 1; }
    /// Whether `file` is the prelude's, the program's or a library file the program reaches.
    /// Behind a layer of the whole library, a module the program never uses is checked too, and is nothing of its:
    /// a pass over every symbol, pipeline or require line asks this first, so the program reads as it does beside the
    /// modules it reaches alone.
    [[nodiscard]] bool is_reached_file(i32 file) const
    {
        if (file < prelude_files || file >= prelude_files + i32(library_files.size())
            || reached_library_files.size() == library_files.size())
            return true;
        auto const position = library_files[file - prelude_files];
        for (auto const r : reached_library_files)
            if (r == position)
                return true;
        return false;
    }
    /// The module the program names `id` through, as `m.name` (CHK-348): empty for a symbol of the prelude and of the
    /// program's own module.
    [[nodiscard]] cc::string_view foreign_module_of(symbol_id id) const
    {
        auto const file = at(id).file;
        if (file < 0 || file >= i32(file_modules.size()) || file_modules[file] == file_modules.back())
            return {};
        return file_modules[file];
    }
    /// The name the program names `id` by from outside any scope: bare for the prelude's and its own module's, and
    /// `m.name` for another module's (CHK-348), which is also how a compile sets a module's option (CHK-354).
    [[nodiscard]] cc::string qualified_name_of(symbol_id id) const;

    [[nodiscard]] symbol const& at(symbol_id id) const { return symbols[index_of(id)]; }
    [[nodiscard]] type_info const& at(type_id id) const { return types[index_of(id)]; }
    [[nodiscard]] cc::span<member_info const> at(ast::range_of<member_info> r) const
    {
        return ast::impl::slice(members, r);
    }
    [[nodiscard]] cc::span<enum_case_info const> at(ast::range_of<enum_case_info> r) const
    {
        return ast::impl::slice(enum_cases, r);
    }
    [[nodiscard]] cc::span<parameter const> at(ast::range_of<parameter> r) const
    {
        return ast::impl::slice(parameters, r);
    }
    [[nodiscard]] cc::span<symbol_id const> at(ast::range_of<symbol_id> r) const
    {
        return ast::impl::slice(binding_lists, r);
    }
    [[nodiscard]] cc::span<type_id const> at(ast::range_of<type_id> r) const { return ast::impl::slice(type_lists, r); }
    [[nodiscard]] cc::span<pipeline_setting const> at(ast::range_of<pipeline_setting> r) const
    {
        return ast::impl::slice(pipeline_settings, r);
    }
    [[nodiscard]] cc::span<written_argument const> at(ast::range_of<written_argument> r) const
    {
        return ast::impl::slice(written_arguments, r);
    }
    [[nodiscard]] cc::span<i32 const> at(ast::range_of<i32> r) const { return ast::impl::slice(call_slots, r); }

    /// The registry record behind a type the prelude declares `@builtin`; null for every other type and for an id that names none.
    [[nodiscard]] builtins::type_record const* builtin_type_of(type_id id) const
    {
        if (builtins == nullptr || !is_valid(id) || index_of(id) >= types.size())
            return nullptr;
        auto const& t = at(id);
        auto const is_declared = t.kind == type_kind::structure || t.kind == type_kind::enumeration;
        if (!is_declared || !is_valid(t.symbol) || index_of(t.symbol) >= symbols.size())
            return nullptr;
        auto const record = at(t.symbol).intrinsic_type;
        return builtins->is_known(record) ? &builtins->at(record) : nullptr;
    }

    /// An enum of the program or the prelude that is no `@builtin`: one whose values are the `int`s of its cases.
    /// `bool` is a builtin enum, so it is none, and it compares and is written as a bool.
    [[nodiscard]] bool is_plain_enum(type_id id) const
    {
        return is_valid(id) && index_of(id) < types.size() && at(id).kind == type_kind::enumeration
            && builtin_type_of(id) == nullptr;
    }

    /// The registry record behind a call; null when `id` names none.
    [[nodiscard]] builtins::function_record const* builtin_function(builtin_id id) const
    {
        return builtins != nullptr && builtins->is_known(id) ? &builtins->at(id) : nullptr;
    }

    /// DXR's cap on the attributes a procedural hit carries, which metal's two `uint4` of ray data match (CHK-342).
    static constexpr i32 max_attribute_bytes = 32;

    /// The bytes a payload or a hit's attributes take in a ray-tracing pipeline: a 32-bit word per scalar and per enum.
    /// The checker's attribute cap and `describe`'s sizes for sg both read this, so the two cannot disagree.
    [[nodiscard]] i32 ray_data_bytes(type_id id) const
    {
        if (auto const* const builtin = builtin_type_of(id))
            return 4 * builtin->leaf_count;
        auto const& t = at(id);
        if (t.kind == type_kind::enumeration)
            return 4;
        if (t.kind == type_kind::array)
            return t.count * ray_data_bytes(t.element);
        auto result = 0;
        for (auto const& member : at(t.members))
            result += ray_data_bytes(member.type);
        return result;
    }

    /// The name a type is written with; `<error>` for the error type.
    /// A resource, or a binding array of one: what takes slots of its group rather than a place in its constant block.
    [[nodiscard]] bool takes_slots(type_id id) const
    {
        auto const& t = at(id);
        return is_resource(t.kind) || (t.kind == type_kind::array && is_resource(at(t.element).kind));
    }

    [[nodiscard]] cc::string_view name_of(type_id id) const
    {
        auto const& t = at(id);
        if (t.kind == type_kind::void_)
            return "void";
        if (t.kind == type_kind::structure && (is_valid(t.generic) || is_valid(t.tangent_of)))
            return t.spelled;
        if (t.kind == type_kind::structure || t.kind == type_kind::enumeration)
            return at(t.symbol).name;
        if (!t.spelled.empty())
            return t.spelled;
        return "<error>";
    }

    /// Makes every table's entries the shared lower layer a program's check starts from and never writes.
    void freeze_tables()
    {
        symbols.freeze();
        types.freeze();
        members.freeze();
        enum_cases.freeze();
        functions.freeze();
        parameters.freeze();
        bindings.freeze();
        binding_lists.freeze();
        type_lists.freeze();
        samplers.freeze();
        pipelines.freeze();
        pipeline_settings.freeze();
        constants.freeze();
        option_uses.freeze();
        call_records.freeze();
        written_arguments.freeze();
        call_slots.freeze();
        function_instances.freeze();
        near_misses.freeze();
        ray_traces.freeze();
        callable_calls.freeze();
        texel_stores.freeze();
        target_texts.freeze();
        target_preambles.freeze();
        target_type_branches.freeze();
        files.freeze();
        tests.freeze();
        test_units.freeze();
        internal_entry_points.freeze();
    }

    [[nodiscard]] bool operator==(checked_module const& rhs) const
    {
        using ast::impl::is_equal;
        return is_equal(symbols, rhs.symbols) && is_equal(types, rhs.types) && is_equal(members, rhs.members)
            && is_equal(enum_cases, rhs.enum_cases) && is_equal(functions, rhs.functions)
            && is_equal(parameters, rhs.parameters) && is_equal(bindings, rhs.bindings)
            && is_equal(binding_lists, rhs.binding_lists) && is_equal(type_lists, rhs.type_lists)
            && is_equal(samplers, rhs.samplers) && is_equal(pipelines, rhs.pipelines)
            && is_equal(pipeline_settings, rhs.pipeline_settings) && is_equal(constants, rhs.constants)
            && is_equal(option_uses, rhs.option_uses) && is_equal(call_records, rhs.call_records)
            && is_equal(function_instances, rhs.function_instances)
            && is_equal(written_arguments, rhs.written_arguments) && is_equal(call_slots, rhs.call_slots)
            && is_equal(near_misses, rhs.near_misses) && is_equal(ray_traces, rhs.ray_traces)
            && is_equal(callable_calls, rhs.callable_calls) && is_equal(texel_stores, rhs.texel_stores)
            && is_equal(target_texts, rhs.target_texts) && is_equal(target_preambles, rhs.target_preambles)
            && is_equal(target_type_branches, rhs.target_type_branches) && is_equal(files, rhs.files)
            && is_equal(entry_points, rhs.entry_points) && is_equal(tests, rhs.tests)
            && is_equal(test_units, rhs.test_units) && is_equal(internal_entry_points, rhs.internal_entry_points)
            && is_equal(entry_expectations, rhs.entry_expectations) && is_equal(diagnostics, rhs.diagnostics)
            && builtins == rhs.builtins;
    }
};

/// One file's side tables as a body reads them: an instance's own entries over the file's (CHK-338).
/// It holds positions, not references, since checking may add instances while it is read.
struct sgl::check::table_view
{
    checked_module const* module = nullptr;
    i32 file = 0;
    /// A position in `checked_module::function_instances`, -1 for the file's tables alone.
    i32 instance = -1;

    [[nodiscard]] type_id type_at(ast::expr_id id) const
    {
        if (auto const* const own = own_tables(id); own != nullptr)
            if (auto const t = own->type_of[slot_of(id)]; is_valid(t))
                return t;
        return module->files[file].type_at(id);
    }
    /// By value: an instance's tables move when the check adds another instance.
    [[nodiscard]] target target_at(ast::expr_id id) const
    {
        if (auto const* const own = own_tables(id); own != nullptr)
            if (auto const& t = own->target_of[slot_of(id)]; t.kind != target_kind::none)
                return t;
        return module->files[file].target_at(id);
    }
    [[nodiscard]] i32 call_at(ast::expr_id id) const
    {
        if (auto const* const own = own_tables(id); own != nullptr)
            if (auto const c = own->call_of[slot_of(id)]; c != -2)
                return c;
        return module->files[file].call_at(id);
    }

private:
    [[nodiscard]] isize slot_of(ast::expr_id id) const
    {
        return isize(ast::index_of(id)) - module->function_instances[instance].first;
    }
    [[nodiscard]] file_tables const* own_tables(ast::expr_id id) const
    {
        if (instance < 0)
            return nullptr;
        auto const& tables = module->function_instances[instance].tables;
        auto const slot = slot_of(id);
        return slot >= 0 && slot < tables.type_of.size() ? &tables : nullptr;
    }
};

namespace sgl::check
{
/// The position in `m.target_type_branches` of the branch a compile for `target` takes of text type `type_symbol`, the
/// first whose condition holds; -1 where none does (CHK-432).
[[nodiscard]] i32 chosen_type_branch(checked_module const& m, symbol_id type_symbol, compile_target const& target);
} // namespace sgl::check
