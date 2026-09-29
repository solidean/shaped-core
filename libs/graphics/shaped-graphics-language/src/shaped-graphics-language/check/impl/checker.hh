#pragma once

#include <clean-core/container/fixed_array.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/check/check.hh>
#include <shaped-graphics-language/check/features.hh>

namespace sgl::check::impl
{
/// How a number literal reads before any literal types exist.
enum class number_class : u8
{
    /// A decimal literal with a DOT or an exponent and no suffix.
    plain_float,
    /// Decimal digits, or hexadecimal or binary ones behind `0x` or `0b`, and nothing else.
    plain_integer,
    /// A suffix or a `p` exponent: spellings whose meaning needs literal types.
    other,
};

[[nodiscard]] number_class classify_number(cc::string_view text);

/// `text` must be a `plain_float`; nullopt when the value does not fit an f64.
[[nodiscard]] cc::optional<f64> parse_plain_float(cc::string_view text);

/// `text` must be a `plain_integer`; nullopt when the value does not fit an i32.
[[nodiscard]] cc::optional<i32> parse_plain_integer(cc::string_view text);

/// `text` must be a `plain_integer`; nullopt when the value does not fit an i64, which is what a literal is held in
/// until the type it meets is known (CHK-61).
[[nodiscard]] cc::optional<i64> parse_literal_integer(cc::string_view text);

/// A name of the function being checked: its parameters, then the locals in scope, in source order.
struct local_name
{
    cc::string_view name;
    /// `target_kind::parameter` or `target_kind::local`, as the side table records a use of it.
    target where;
    type_id type = type_id::none;
    /// Declared with `let mut`, so an assignment may name it.
    bool is_mut = false;
    /// How many blocks deep its declaration stands; the parameters and the function's own block are 0.
    int depth = 0;
    /// A name of the function a test stands in: visible, so it hides what the module has of that name, and unusable.
    bool is_captured = false;
};

/// A loop around the statement being checked.
struct loop_scope
{
    /// A `loop:` whose value somebody reads, so every `break` of it carries one.
    bool yields_value = false;
    /// The type of the first `break value`; `none` before one was seen.
    type_id value = type_id::none;
    bool has_break = false;
};

/// A value block around the statement being checked: the body of a `case` arm, which `yield` hands its value to.
struct value_block_scope
{
    /// The type of the first `yield`; `none` before one was seen.
    type_id value = type_id::none;
    bool has_yield = false;
    /// `value` was written, as a property's `-> T`, so a `yield` converts to it rather than setting it (CHK-82).
    bool is_expected = false;
};

/// How a statement list ends.
enum class flow : u8
{
    /// Some path reaches the end of the list.
    falls_through,
    /// No path does: each one ends in a `return`, a `break`, a `continue`, or a `loop:` nothing leaves.
    exits,
    /// A statement that did not build stands in the way, so nothing is reported about the paths through it.
    unknown,
};

/// The ordered scope of one function body, and what its expressions are checked against.
struct function_scope
{
    symbol_id function = symbol_id::none;
    i32 file = 0;
    type_id result = type_id::none;
    cc::vector<local_name> locals;
    int depth = 0;
    /// Innermost last.
    cc::vector<loop_scope> loops;
    /// The value blocks a `yield` can name, innermost last; a `case` arm that is a value pushes one.
    cc::vector<value_block_scope> value_blocks;
    /// The body of a `test`, which runs on its own: no parameter, no binding, and nothing of what stands around it.
    bool is_test = false;
    /// For a test in a function body: the parameters and locals visible where it stands, each `is_captured`.
    cc::vector<local_name> captured;
    /// For a test in a function body: that function.
    symbol_id enclosing = symbol_id::none;
    /// The type of `self` in a method or a property, where `self` is a name (CHK-245).
    type_id receiver = type_id::none;

    /// The newest visible local or parameter of that name, which hides every module-level symbol of it.
    /// In a test, a name of the function around it is found last, and is `is_captured`.
    [[nodiscard]] local_name const* find_local(cc::string_view name) const
    {
        for (auto i = locals.size() - 1; i >= 0; --i)
            if (locals[i].name == name)
                return &locals[i];
        for (auto i = captured.size() - 1; i >= 0; --i)
            if (captured[i].name == name)
                return &captured[i];
        return nullptr;
    }
};

/// One call of a function of the program, which is an edge of the graph recursion is looked for in.
struct call_edge
{
    symbol_id caller = symbol_id::none;
    symbol_id callee = symbol_id::none;
    i32 file = 0;
    source_span where;
};

/// What the pass knows about a function beyond its public `function_info`, parallel to `checked_module::functions`.
struct function_notes
{
    /// An arrow body without `-> T`: the result is the type of the body's expression.
    /// Its body is checked as part of compiling the symbol, and for every other function after all signatures are known.
    bool infers_result = false;
    /// `check_body` ran, so it does not run again.
    bool is_body_checked = false;
    /// An entry point whose signature passed every rule of its stage.
    bool is_valid_entry = false;
    /// The body checked without a single error, so its side tables are complete.
    bool is_body_sound = false;
    /// Every default of its parameters checked without an error; a call that leaves one out flattens it.
    bool are_defaults_sound = true;
    /// Stands on a loop of calls, which was reported.
    bool is_recursive = false;
    /// For an entry point: what its file, the bindings it lists and its body's `require`s declare (CHK-262).
    feature_set declared_features;
    /// 0 before anybody asked, 1 for a function that inlines whole, 2 for one that does not.
    /// It inlines whole when its body is sound, it is not recursive, and the same holds for every function it calls.
    u8 inlines_whole = 0;
};

/// Where a `require` stands, which decides what it grants and when it is unused (CHK-265).
enum class require_scope : u8
{
    file,
    binding,
    body,
};

/// One feature one `require` names, kept until the pass knows whether anything needed it.
struct require_line
{
    i32 file = 0;
    /// The feature's name in the line.
    source_span where;
    feature what = {};
    require_scope scope = require_scope::file;
    /// The binding, or the function whose body it stands in; `none` at file scope.
    symbol_id owner = symbol_id::none;
    bool is_used = false;
};

/// A number literal as an argument, which converts to any numeric type that holds it (CHK-253).
struct number_literal
{
    bool is_number = false;
    /// An integer literal, whose value is `integer`; a float literal's is `real`.
    bool is_integer = false;
    i64 integer = 0;
    f64 real = 0;
};

/// The arguments of one call as it wrote them, a splat spread into one per field, before any candidate is looked at.
struct call_arguments
{
    /// In the order written, which is the order they are evaluated in (EVAL-80).
    cc::vector<written_argument> written;
    /// Parallel to `written`.
    cc::vector<type_id> types;
    /// Parallel to `written`: the name of a named argument, empty for a positional one.
    cc::vector<cc::string_view> names;
    /// Parallel to `written`: what a number literal holds, for the conversions it may take.
    cc::vector<number_literal> numbers;
    /// Parallel to `written`: a tuple or object literal, as a position in `checker::literals`; -1 for anything else.
    /// Such a literal has no type of its own, so its entry in `types` is `none` (CHK-81).
    cc::vector<i32> literals;
    /// Parallel to `written`: a function's name or a lambda, as a position in `checker::function_arguments`; -1 for
    /// anything else, including a parameter of function type handed on, which has its type.
    cc::vector<i32> functions;
    /// Parallel to `written`: the prelude's `undefined()`, which has the type of the parameter it meets (CHK-341).
    cc::vector<bool> undefineds;
    /// An argument had the error type or was reported, so the call reports nothing about its arguments.
    bool is_poisoned = false;
};

/// A function's name or a lambda written as an argument, which has no type until a parameter of function type meets it.
struct function_argument
{
    ast::expr_id expr = ast::expr_id::none;
    bool is_lambda = false;
    /// The functions of the name, for a name.
    cc::vector<symbol_id> functions;
};

/// One candidate that takes a call's arguments: which argument fills which parameter, and at what cost.
struct candidate_match
{
    symbol_id candidate = symbol_id::none;
    cc::vector<i32> slots;
    /// What a generic candidate's type parameters were deduced as, two by two (CHK-340).
    cc::vector<type_id> bindings;
    /// Parallel to the call's written arguments: the length of the chain that converts each to its parameter (CHK-70).
    cc::vector<i32> chains;
};

/// Which written argument fills each parameter of one candidate (CHK-250 to CHK-252).
struct bound_arguments
{
    /// One per parameter: a position in `call_arguments::written`, or -1 where the parameter takes its default.
    cc::vector<i32> slots;
    miss_reason failure = miss_reason::none;
    /// The written argument, or the parameter, the failure is about; -1 where it is about neither.
    i32 argument = -1;
    i32 parameter = -1;
};

/// An integer literal beyond `int`, which is an error only where it keeps that default type.
struct wide_literal
{
    i32 file = 0;
    ast::expr_id expr = ast::expr_id::none;
    /// The function whose body or defaults it stands in; `none` outside one.
    symbol_id function = symbol_id::none;
};

/// `fun T.name`, whose type is known only once every file is declared.
struct pending_extension
{
    i32 file = 0;
    ast::decl_id declaration = ast::decl_id::none;
};

/// How a call was spelled, which its target is checked against once it is chosen (CHK-256).
enum class call_spelling : u8
{
    /// `foo(a)`, `a + b`, `T.foo(a)`: any function may be its target.
    free,
    /// `a.foo(…)`: a property may not be its target.
    dot_call,
    /// `a.foo`, or a bare name read through `self`: only a property may be its target.
    dot_read,
};

/// Which pipeline settings an attribute may be, by what it stands on.
enum class setting_scope : u8
{
    /// None: every attribute there is the compiler's own.
    none,
    /// An entry point or an edge struct, whose attributes may be any setting of the description.
    description,
    /// A member of a `@pixel struct`, whose attributes are settings of that one target.
    target,
};

/// An entry point's `@expect(footprint = "...")`, judged once its flat tree exists (CHK-267).
struct footprint_pin
{
    symbol_id function = symbol_id::none;
    i32 file = 0;
    cc::string expected;
    /// The argument, which an unmet pin is reported at.
    source_span where;
};

/// The one demand-driven pass; every member function only appends to `out` and flips symbol states.
struct checker
{
    cc::span<module_file const> files;
    builtins::registry const& builtins;
    checked_module out;

    /// The prelude's scope: every name its files declare but those of `@operator` functions.
    /// More than one symbol under a name means all of them are functions; the same holds for `file_names`.
    cc::map<cc::string, cc::vector<symbol_id>> prelude_names;
    /// The user file's own scope, the inner one.
    cc::map<cc::string, cc::vector<symbol_id>> file_names;
    /// What the user file sees: `file_names` over `prelude_names`, where two overload sets of one name merge.
    /// Built once every file is declared.
    cc::map<cc::string, cc::vector<symbol_id>> names;
    /// `@operator` functions by operator spelling.
    cc::map<cc::string, cc::vector<symbol_id>> operators;
    /// The functions of each struct's and enum's type scope, keyed by the index of the type's symbol, then by name.
    /// Members are declared with their type, and extensions once every file is declared (CHK-233, CHK-237).
    cc::map<i32, cc::map<cc::string, cc::vector<symbol_id>>> type_scopes;
    /// The types `resource_type` and `buffer_type` interned, by spelling, so a mention looks up only its equals.
    /// The function arguments of every call checked so far; `call_arguments::functions` indexes it.
    cc::vector<function_argument> function_arguments;
    /// True while a parameter's type is resolved, the one place a function type may stand (CHK-317).
    bool allows_function_type = false;
    cc::map<cc::string, cc::vector<type_id>> interned_types;
    cc::vector<pending_extension> pending_extensions;
    /// Integer literals that do not fit an `int`, judged once every literal has met the type it converts to (CHK-61).
    cc::vector<wide_literal> wide_literals;
    /// The elements of every tuple and object literal an argument or an expected type met, each as the arguments of the
    /// call it converts by (CHK-81); positions stay valid while it grows.
    cc::vector<call_arguments> literals;
    /// The symbols in compilation, outermost first, which is the loop a dependency cycle names.
    cc::vector<symbol_id> compiling;
    cc::vector<function_notes> notes;
    /// Parallel to the registry's functions: the prelude symbol declaring each, filled before any tree is flattened.
    cc::vector<symbol_id> symbol_of_builtin;
    /// Every call of a function that is no `@builtin`, in the order the bodies were checked.
    cc::vector<call_edge> calls;
    /// The object `check_index` is checking right now: the one place a buffer may stand as an expression.
    ast::expr_id subscripted = ast::expr_id::none;
    /// The index into a binding array `check_index` is checking right now: the one place `nonuniform i` may stand.
    ast::expr_id binding_index = ast::expr_id::none;
    /// The arguments of the call being checked, which a texture, an image or a sampler may stand as (CHK-206).
    cc::vector<ast::expr_id> handed;
    /// Parallel to `out.tests`: what a test in a function body sees of that function, and the function.
    cc::vector<cc::vector<local_name>> test_captures;
    cc::vector<symbol_id> test_enclosing;
    /// Parallel to `files`: what the `require` lines at file scope grant everything in that file (CHK-259).
    cc::vector<feature_set> file_features;
    /// What a type resolved right now may use beyond its file's features: a binding's own while its members compile.
    feature_set granted;
    /// Where a granted use is recorded; null where nobody collects one.
    feature_set* used_features = nullptr;
    /// Every `require` of a body, which is reported at the end where nothing needed it (CHK-265).
    cc::vector<require_line> require_lines;
    /// Every entry point's footprint pin, judged once the entry points are flattened (CHK-267).
    cc::vector<footprint_pin> footprint_pins;

    // ---- shared helpers (check.cc) ----------------------------------------------------------------------------------

    [[nodiscard]] parsed_file const& file_of(i32 file) const { return files[file].file; }
    [[nodiscard]] ast::file_ast const& ast_of(i32 file) const { return files[file].ast; }
    [[nodiscard]] cc::string_view text_of(i32 file, source_span where) const { return file_of(file).text_of(where); }
    /// The user file is the last one; every file before it is the prelude's.
    [[nodiscard]] bool is_prelude_file(i32 file) const { return file < i32(files.size()) - 1; }
    /// The module-level names a lookup from `file` finds: a prelude file never sees the user file's.
    [[nodiscard]] cc::map<cc::string, cc::vector<symbol_id>> const& names_seen_from(i32 file) const
    {
        return is_prelude_file(file) ? prelude_names : names;
    }
    [[nodiscard]] source_span span_of(i32 file, form_id form) const;
    [[nodiscard]] source_span span_of(i32 file, ast::expr_id expr) const;
    [[nodiscard]] source_span span_of(i32 file, ast::decl_id decl) const;
    [[nodiscard]] source_span span_of(i32 file, ast::stmt_id stmt) const;

    /// The diagnostic it appended, which a caller may give related notes.
    located_diagnostic& report(diagnostic_kind kind, i32 file, source_span where, cc::string detail);
    /// Where `expected` and `got` are two types of one name, `d` says so and notes where each is declared.
    /// Without it a mismatch would read `expected int, got int`.
    void tell_apart(located_diagnostic& d, type_id expected, type_id got);
    void unsupported(i32 file, source_span where, cc::string_view construct);
    /// `report`, unless a diagnostic of that kind already stands there: for what every tree inlining one body finds.
    void report_once(diagnostic_kind kind, i32 file, source_span where, cc::string_view detail);
    [[nodiscard]] isize error_count() const;

    [[nodiscard]] ast::attribute const* find_attribute(i32 file,
                                                       ast::range_of<ast::attribute> range,
                                                       cc::string_view name) const;
    /// Reports every attribute whose name is not in `known` as `unsupported-yet`, and arguments on a known one.
    /// Where `scope` allows pipeline settings, an attribute that names one is known and takes its value.
    void judge_attributes(i32 file,
                          ast::range_of<ast::attribute> range,
                          cc::span<cc::string_view const> known,
                          cc::string_view owner,
                          setting_scope scope = setting_scope::none);

    void set_type(i32 file, ast::expr_id expr, type_id type);
    void set_target(i32 file, ast::expr_id expr, target where);

    // ---- declarations (check.cc, check_decl.cc) ---------------------------------------------------------------------

    void run();
    void declare_file(i32 file);
    void declare(i32 file, ast::decl_id decl);
    void add_symbol(symbol s, source_span name_where);
    /// Lays the user file's scope over the prelude's into `names`.
    void merge_scopes();
    /// True where every symbol of `ids` is a function, so the name is an overload set (CHK-12, CHK-189).
    [[nodiscard]] bool is_all_functions(cc::span<symbol_id const> ids) const;
    /// True where `ids` are functions, or a struct in front of functions of its name (CHK-240).
    [[nodiscard]] bool is_overload_set(cc::span<symbol_id const> ids) const;
    /// The properties and functions of a struct's or an enum's block, into the type scope of `owner`.
    void declare_members(symbol_id owner, i32 file, ast::range_of<ast::decl_id> members);
    /// One function of a type scope, a member or an extension; reports a name its type already holds as another kind.
    void add_member(symbol s, source_span name_where);
    /// The kind of thing `name` is in the type scope of `owner`, from its block and what was added so far; empty for
    /// nothing, else "field", "case", "property" or "function".
    [[nodiscard]] cc::string_view member_kind_of(symbol_id owner, cc::string_view name) const;
    /// Every `fun T.name`, into the type scope of `T`; run once every file is declared.
    void attach_extensions();
    /// Reports each integer literal beyond `int` that no conversion took to a type holding it (CHK-61).
    void judge_wide_literals();
    /// Two functions of one overload set whose parameters agree in type, name and named-only mark (CHK-241).
    void judge_redeclarations();
    /// The functions a call of `name` from `file` may choose from, where `first` is its first argument's type:
    /// the functions of that name visible there, and those of the type scope of `first` (CHK-247).
    [[nodiscard]] cc::vector<symbol_id> candidates_of(i32 file, cc::string_view name, type_id first) const;
    /// True where a lookup from `file` sees a function of `from`: the prelude never sees the program's.
    [[nodiscard]] bool is_visible_from(i32 file, symbol_id from) const
    {
        return !is_prelude_file(file) || is_prelude_file(out.at(from).file);
    }
    /// Gives every struct with a block its synthesized constructor, a function of the struct's name (CHK-239).
    /// Run once every file is declared, so the symbols declared before keep their ids.
    void declare_constructors();

    /// Compiles the symbol when nobody has, and reports a cycle when somebody is.
    /// The state it returns is `checked` or `failed`, or `in_compilation` for a cycle, which was reported at `where`.
    symbol_state demand(symbol_id id, i32 file, source_span where);
    void compile(symbol_id id);
    void compile_struct(symbol_id id);
    void compile_enum(symbol_id id);
    /// A `const`: its value is a number literal, an enum case or another `const`, and anything else is `unsupported-yet`.
    void compile_const(symbol_id id);
    /// A file-scope `sampler name:`: its settings, and the sampler type they make (CHK-314).
    void compile_file_sampler(symbol_id id);
    /// False where `attributes` hold `@shadowable(false)`; a malformed one is reported when its declaration is compiled.
    [[nodiscard]] bool is_shadowable_by(i32 file, ast::range_of<ast::attribute> attributes) const;
    /// Reports `shadows-unshadowable` where a local or a parameter named `name` would hide a `@shadowable(false)` symbol.
    void judge_shadowing(i32 file, cc::string_view name, source_span where);
    void compile_binding(symbol_id id);
    /// The bindings a `{...}` list names, each checked; an entry that names none is reported and sets `is_failed`.
    [[nodiscard]] cc::vector<symbol_id> binding_list_of(i32 file, ast::range_of<ast::argument> entries, bool& is_failed);
    void compile_function(symbol_id id);
    /// The synthesized constructor `id`: one parameter per field of its struct, and the struct as its result.
    void compile_constructor(symbol_id id);
    /// A property, `name => value`: one parameter, `self`, and the value's type or the written one as its result.
    void compile_property(symbol_id id);
    /// The type of `self` for a function of a type scope; `none` for a free function and a static.
    [[nodiscard]] type_id receiver_of(symbol_id id);
    void judge_entry_point(symbol_id id);

    /// True for the prelude's `int3`, the type a dispatch reports a thread's id as.
    [[nodiscard]] bool is_int3(type_id type) const;
    /// The stages a `@stages` attribute names, as `function_info::stages`; every stage without one or after a bad one.
    [[nodiscard]] u16 stages_of(i32 file, ast::attribute const* a);
    [[nodiscard]] interpolation interpolation_of(i32 file, ast::attribute const* a);
    /// The grid of a `@compute` attribute; `{1, 1, 1}` without one, and after a bad argument it reports.
    [[nodiscard]] cc::fixed_array<i32, 3> workgroup_of(i32 file, ast::attribute const* a);
    /// `@geometry(max_vertices = N)`'s `N`, from 1 to 256; 1 after a bad argument, which it reports (CHK-301).
    [[nodiscard]] i32 max_vertices_of(i32 file, ast::attribute const& a);
    struct tessellation_mode
    {
        tessellation_partitioning partitioning = tessellation_partitioning::integer;
        bool is_clockwise = true;
    };
    /// `@tessellation_control(partitioning = …, winding = …)`, both named (CHK-304).
    [[nodiscard]] tessellation_mode tessellation_of(i32 file, ast::attribute const& a);
    /// `point_stream[T]`, `line_stream[T]` and `triangle_stream[T]` in a type position (CHK-302).
    [[nodiscard]] type_id resolve_stream(i32 file, ast::expr_id expr, ast::index const& node, function_scope const* scope);
    /// CHK-303: `s.emit(v)` or `s.end_strip()` on a geometry stage's stream, which `check_dot_call` hands on.
    type_id check_stream_call(function_scope& scope, ast::expr_id id, ast::call const& call, type_id stream);
    /// CHK-301 to CHK-306: an entry point of the geometry or a tessellation stage, which `judge_entry_point` hands on.
    void judge_primitive_stage(symbol_id id, cc::function_ref<void(cc::string_view)> invalid);
    /// The ray set a `rays` declaration made of `symbol`, and its ray types; null for any other symbol.
    [[nodiscard]] bool is_ray_set(symbol_id symbol) const;
    /// `set.ray` as an expression: the set and the ray's position, or `none` where `expr` names no ray set.
    /// A ray set without that ray type is reported, and its position is -1.
    [[nodiscard]] cc::optional<ray_trace> ray_type_of(i32 file, ast::expr_id expr);
    /// `trace(world, r, set.ray, mut payload)`, the trace of a ray-tracing stage (CHK-329).
    type_id check_pipeline_trace(function_scope& scope, ast::expr_id id, ast::call const& call, ray_trace ray);
    /// CHK-326: an entry point of a ray-tracing stage, which `judge_entry_point` hands on.
    void judge_ray_stage(symbol_id id, cc::function_ref<void(cc::string_view)> invalid);
    /// The one name of a `@stream(name)` or a `@sampler(name)`; empty without one, and after a bad argument it reports.
    [[nodiscard]] cc::string name_argument_of(i32 file, ast::attribute const* a);
    /// The members of a struct or a binding, collected locally and appended whole so the range stays contiguous.
    /// A `@pixel struct`'s members are targets, so their attributes may be a target's settings.
    /// A `@workgroup` binding's members are memory of the workgroup: values, arrays among them, and never a resource.
    [[nodiscard]] ast::range_of<member_info> compile_members(i32 file,
                                                             ast::range_of<ast::decl_id> members,
                                                             bool is_struct,
                                                             bool is_target_struct = false,
                                                             bool is_vertex_struct = false,
                                                             bool is_workgroup = false);
    /// The bytes a value of `type` takes in workgroup memory, laid out as WGSL lays out its workgroup variables.
    [[nodiscard]] i32 workgroup_size_of(type_id type) const;
    /// A `@workgroup` binding, whose members a shader writes and a test holds without listing it (CHK-292).
    [[nodiscard]] bool is_workgroup_binding(symbol_id id) const
    {
        return is_valid(id) && out.at(id).kind == symbol_kind::binding && out.at(id).info >= 0
            && out.bindings[out.at(id).info].is_workgroup;
    }
    /// A resource, or an array or a struct holding one at any depth.
    [[nodiscard]] bool holds_resource(type_id type) const;
    /// An atomic, or an array of them.
    [[nodiscard]] bool holds_atomic(type_id type) const;
    /// CHK-291: the path from `type` down to the first array it holds, through struct members, as `.corners: float2[3]`.
    /// Empty when it holds none; a tessellation factor member is skipped, since it is an array by design (CHK-305).
    [[nodiscard]] cc::string array_path(type_id type) const;
    /// CHK-291: refuses each array a struct crossing a stage edge holds, reported at `where` of `file`.
    void judge_edge_arrays(i32 file, source_span where, type_id type);
    /// `atomic[uint]` and `atomic[int]` in a type position.
    [[nodiscard]] type_id resolve_atomic(i32 file, ast::expr_id expr, ast::index const& node, function_scope const* scope);
    /// CHK-297: an expression of an atomic's type, which only a builtin's argument may be.
    [[nodiscard]] bool judge_atomic_use(i32 file, ast::expr_id id, type_id type);
    /// What every target gives a workgroup of memory: WebGPU's default limit, and vulkan's required minimum.
    static constexpr i32 k_portable_workgroup_bytes = 16384;
    /// A `@vertex struct` member's `@format(.case)`: the case's name, checked against the member's type (CHK-275).
    [[nodiscard]] cc::string vertex_format_of(i32 file, ast::attribute const* a, type_id member_type);
    /// A `@pixel struct` member's `@depth` or `@sample_mask`, checked against its type (CHK-276).
    [[nodiscard]] pixel_output pixel_output_of(i32 file,
                                               ast::range_of<ast::attribute> attributes,
                                               type_id member_type,
                                               cc::string_view name);
    /// The type an expression in a type position names; the error type when it names none.
    /// Inside a body, `scope` holds the locals, which hide a module-level type of their name.
    [[nodiscard]] type_id resolve_type(i32 file, ast::expr_id expr, function_scope const* scope = nullptr);
    /// `resolve_type` for the type of a value — a field, a parameter, a result, a local — where a buffer cannot stand.
    /// A buffer is a resource a binding member names, and is only ever read through a subscript.
    [[nodiscard]] type_id resolve_value_type(i32 file, ast::expr_id expr, function_scope const* scope = nullptr);
    /// The type of the prelude's `@builtin struct` named `name`; without one it reports at `where` and is the error type.
    [[nodiscard]] type_id type_of_builtin(cc::string_view name, i32 file, source_span where);
    /// `buffer[element]`, or its `mut` form, interned: two mentions of one buffer type share an id.
    [[nodiscard]] type_id buffer_type(type_id element, bool is_mut);
    /// `buffer[T]` in a type position, which is the `index` node `buffer` heads.
    [[nodiscard]] type_id resolve_buffer(i32 file, ast::expr_id expr, ast::index const& node, function_scope const* scope);
    /// `T[N]` and `T[a, b]` in a type position; the caller has ruled out `buffer` and every resource.
    [[nodiscard]] type_id resolve_array(i32 file, ast::expr_id expr, ast::index const& node, function_scope const* scope);
    /// The interned `element[count]`, spelled outermost first.
    [[nodiscard]] type_id array_type(type_id element, i32 count);
    /// Whether `expr` in a type position names a complete type, so that a group applied to it makes an array of it.
    /// Reports nothing, so a caller may still read the group as something else.
    [[nodiscard]] bool is_type_name(i32 file, ast::expr_id expr) const;
    /// The generic struct of the prelude `expr` names, `none` where it names no such struct (CHK-339).
    type_id generic_named(i32 file, ast::expr_id expr);
    /// An array's length as written: an int literal or a `const`; none for anything else.
    [[nodiscard]] cc::optional<i32> constant_count(i32 file, ast::expr_id expr);
    /// A checked index's value when it is an int literal or names a `const`; none for anything else.
    [[nodiscard]] cc::optional<i32> constant_index(i32 file, ast::expr_id expr) const;
    /// A texture, image or sampler type, interned like `buffer_type`; `info` needs no `spelled`.
    [[nodiscard]] type_id resource_type(type_info info);
    /// `acceleration_structure[.geometry]` (CHK-320).
    [[nodiscard]] type_id resolve_acceleration_structure(i32 file, ast::expr_id expr, ast::index const& node);
    /// `(parameters) -> result`, interned: two mentions of one signature share an id (CHK-317).
    [[nodiscard]] type_id function_type(cc::span<type_id const> parameters, type_id result);
    /// Of `candidates`, the function whose signature is exactly `type`'s; `none` where no single one is.
    [[nodiscard]] symbol_id function_of_type(cc::span<symbol_id const> candidates, type_id type);
    /// A lambda handed to a parameter of function type `type`, checked where it stands (CHK-318).
    /// `bindings`, where given, is what the call bound: a lambda's result binds what it left (CHK-340).
    type_id check_lambda(function_scope& scope, ast::expr_id expr, type_id type, cc::vector<type_id>* bindings = nullptr);
    /// A generic callee's result at this call: its parameters bound by the arguments, then by where the call stands.
    /// The error type after a report where a parameter is left unbound.
    type_id deduce_result(function_scope const& scope, ast::expr_id id, symbol_id callee, cc::vector<type_id>& bindings);
    /// A call through a parameter of function type (CHK-319).
    type_id check_function_call(function_scope& scope, ast::expr_id id, ast::call const& call, local_name const& local);
    /// The resource type a bare name in a type position names — a depth texture or a sampler — and `none` otherwise.
    [[nodiscard]] type_id resolve_resource_name(i32 file, ast::expr_id expr, cc::string_view text);
    /// `texture_2d[float4]` or `image_2d[.rgba8_unorm]`; `none` where `node` heads with no texture or image name.
    [[nodiscard]] type_id resolve_resource_applied(i32 file, ast::expr_id expr, ast::index const& node);
    /// `mut` or `out` in front of `inner`, which only a buffer and an image take.
    [[nodiscard]] type_id qualify_resource(i32 file, ast::expr_id expr, type_id inner, ast::type_access access);
    /// The settings of a `sampler name:` block; a setting that is wrong is reported and left at its default.
    [[nodiscard]] sampler_state compile_sampler(i32 file, ast::sampler_decl const& s);
    /// Reports a call that hands over an `@unfilterable` texture member together with a sampler member that filters.
    /// The sampler a sampling call reads against its texture: filtering (CHK-210, CHK-281), and a `@sampler` supplied
    /// where the call names none (CHK-279).
    void judge_filtering(i32 file, ast::expr_id id, source_span call, cc::span<written_argument const> arguments);
    /// CHK-280: a texel offset and a gather's component are constants, and a compare's level is the literal 0.0.
    void judge_constant_arguments(i32 file, ast::expr_id id);
    void judge_offset_range(i32 file, ast::expr_id expr);
    void index_builtin_symbols();
    /// CHK-282: every barrier and every call that takes derivatives stands where all invocations of its group arrive.
    void judge_uniformity(flat_entry_point const& structured);
    /// CHK-270, CHK-311 and CHK-312: every constant of the tree folded, and what WGSL would refuse of it reported.
    void judge_constants(flat_entry_point const& structured);
    /// The prelude symbol that declares `id`; `none` for a record no declaration names.
    [[nodiscard]] symbol_id symbol_declaring(builtin_id id) const;
    /// A literal, an enum case, a `const`, or a construction of those: what a target takes where it takes no value.
    [[nodiscard]] bool is_constant_argument(i32 file, ast::expr_id expr) const;
    /// A builtin's parameter type, where an image names the texel it reads or writes: `out image_2d[float4]`.
    [[nodiscard]] type_id resolve_pattern_type(i32 file, ast::expr_id expr);
    /// True where an argument of type `argument` may stand for a parameter of type `parameter` (CHK-70, CHK-207).
    [[nodiscard]] bool takes(type_id parameter, type_id argument) const;
    /// Reports `form` as `needs-feature` where neither its file nor `granted` grants `needed`, and records the use otherwise.
    void judge_feature(i32 file, source_span where, cc::string_view form, feature needed);

    // ---- features (check_features.cc) -------------------------------------------------------------------------------

    /// The features one `require` names, each appended to `require_lines`; a name that is no feature is reported.
    feature_set read_require(i32 file, ast::require_decl const& r, require_scope scope, symbol_id owner);
    /// Records which features entry point `id` needs and reports every one it does not declare (CHK-263, CHK-264).
    void judge_entry_features(symbol_id id);
    /// `unused-require` for every `require` of a body that nothing needed (CHK-265).
    void report_unused_requires();

    // ---- footprint pins (check_footprint.cc) ------------------------------------------------------------------------

    /// Records the `@expect(footprint = "...")` among the attributes of function `id`; any other `@expect` there is
    /// reported, since a function's only expectation is its footprint.
    void read_footprint_pin(symbol_id id, i32 file, ast::range_of<ast::attribute> attributes, bool is_entry_point);
    /// `unmet-expectation` for every pin whose entry point's footprint says otherwise (CHK-267).
    void judge_footprint_pins();
    /// True where `expr` is the bare name `name`, which is how a resource type is recognized before lookup.
    [[nodiscard]] bool is_named(i32 file, ast::expr_id expr, cc::string_view name) const;

    // ---- pipelines (check_pipeline.cc) ------------------------------------------------------------------------------

    void compile_pipeline(symbol_id id);
    // ---- generics (check_generics.cc) ----
    /// A fresh type parameter named `name`, of the function or the generic struct `owner` (CHK-338).
    type_id new_type_parameter(cc::string_view name, symbol_id owner);
    /// True where `type` names a type parameter at any depth: it stands for different types at different calls.
    [[nodiscard]] bool is_open(type_id type) const;
    /// Every type parameter `type` names, each once, into `into`.
    void collect_type_parameters(type_id type, cc::vector<type_id>& into) const;
    /// `generic[argument]`, interned; the template itself where `argument` is its own type parameter (CHK-339).
    type_id instance_of(type_id generic, type_id argument);
    /// `instance_of` where it exists already, `none` otherwise.
    [[nodiscard]] type_id existing_instance(type_id generic, type_id argument) const;
    /// `type` with every parameter `bindings` binds replaced, interning what that makes.
    type_id substitute(type_id type, cc::span<type_id const> bindings);
    /// `substitute` over what exists already, which flattening reads; `none` where an instance was never made.
    [[nodiscard]] type_id substitute_existing(type_id type, cc::span<type_id const> bindings) const;
    /// Whether `actual` is `pattern` with its open parameters bound, extending `bindings` with what that takes (CHK-340).
    [[nodiscard]] bool unify(type_id pattern, type_id actual, cc::vector<type_id>& bindings) const;
    /// Makes every instance a generic call's inlining will name, before any entry point is flattened.
    void instantiate_generics();
    /// The type parameters in scope, innermost last: a generic function's while its signature and body are checked,
    /// and a generic struct's while its members are.
    cc::vector<cc::pair<cc::string_view, type_id>> type_parameter_names;
    /// What a call stands where a type is expected, which a generic callee's result is deduced from where its
    /// arguments leave a parameter unbound (CHK-340); `none` elsewhere.
    type_id expected_result = type_id::none;

    /// CHK-330: a `hit_group`, one row of a ray-tracing pipeline's table.
    void compile_hit_group(symbol_id id);
    /// CHK-343: a `callables` table.
    void compile_callables(symbol_id id);
    /// The `callables` table `expr` names, `none` where it names none.
    symbol_id callables_named(i32 file, ast::expr_id expr);
    /// CHK-344: `table[i](mut p)`, a call of the callable at `i`.
    type_id check_callable_call(function_scope& scope,
                                ast::expr_id id,
                                ast::call const& call,
                                ast::index const& index,
                                symbol_id table);
    /// CHK-343: at most one table of a module takes the host's callables, and it is declared last.
    void judge_callables();
    /// CHK-331: a `@raytracing pipeline`.
    void compile_raytracing_pipeline(symbol_id id);
    /// The entry point of `wanted` that `value` names, or `none` after a report.
    symbol_id ray_entry_named(i32 file, ast::expr_id value, stage wanted);
    /// The ray set `name` names, or `none` after a report.
    symbol_id ray_set_named(i32 file, source_span name);
    /// The payload an entry point of a ray-tracing stage takes, `none` for a raygen.
    [[nodiscard]] type_id payload_of(symbol_id entry) const;
    /// CHK-332: every ray-tracing pipeline's trace graph, once each entry point says what it traces.
    void judge_trace_graphs();
    /// The prelude's `raster_pipeline_description`, compiled on first use; the error type where the prelude has none.
    [[nodiscard]] type_id pipeline_description_type();
    /// True when `name` alone is one field of the description, or of one target's part when `is_on_target`.
    /// A stage name never is: it marks an entry point.
    [[nodiscard]] bool is_setting_attribute(cc::string_view name, bool is_on_target);
    /// Every field an attribute of that name could set, as full paths; more than one means it is ambiguous.
    [[nodiscard]] cc::vector<cc::string> setting_attribute_paths(cc::string_view name, bool is_on_target);

    // ---- bodies and expressions (check_expr.cc) ---------------------------------------------------------------------

    void check_body(symbol_id id);
    void check_body_of(symbol_id id);
    /// The defaults of function `id`'s parameters, each in the function's own scope with the parameters before it
    /// visible, and of its parameter's type (CHK-243).
    /// Checked once, after every signature is known, since a default may call what is declared below it.
    void check_defaults(symbol_id id);

    // ---- tests (check_test.cc) --------------------------------------------------------------------------------------

    /// Records the `test` `decl` stands for, with the names of `enclosing` it can see and must not use.
    void add_test(i32 file, ast::decl_id decl, cc::string scope_path, function_scope const* enclosing = nullptr);
    /// Every `test` of every file that no other path found, checked as one at file scope with no names around it.
    void add_unregistered_tests();
    /// Every `test` a struct or an enum declares among its members.
    void add_member_tests(i32 file, ast::range_of<ast::decl_id> members, cc::string_view scope_path);
    /// Checks test `index` as a function of no parameter and no binding, and its last-line rule (CHK-226).
    void check_test(i32 index);
    /// The flat tree of test `index`, when it checked soundly and all it reaches inlines whole (flatten.cc).
    void flatten_test(i32 index);
    /// True where the parser or the AST pass reported an error inside `extent` of `file`.
    [[nodiscard]] bool has_syntax_error_in(i32 file, source_span extent) const;
    /// The statement that is the last code line of `statements`, through the last branch of an `if`, a loop's body and
    /// the last arm of a `case`; `none` for an empty list.
    [[nodiscard]] ast::stmt_id last_code_line(i32 file, ast::range_of<ast::stmt_id> statements) const;
    /// The arguments of every `@expect` among `attributes`; a malformed one is reported and left out.
    [[nodiscard]] cc::vector<test_expectation> expectations_of(i32 file, ast::range_of<ast::attribute> attributes);
    /// The text of a `//` comment on the line of `where`, or alone on the line above it; empty without one.
    [[nodiscard]] cc::string comment_of(i32 file, source_span where) const;
    /// Reports a use of what a test cannot reach: a name `local` of the function around it.
    void report_capture(function_scope const& scope, source_span where, local_name const& local);
    /// `values[i]`, which today is a buffer element and nothing else; the error type where it is not one.
    [[nodiscard]] type_id check_index(function_scope& scope, ast::expr_id id, ast::index const& node);
    /// `x as T`, which is the operator function of `as` that takes `x` and gives `T`; the error type where none does.
    [[nodiscard]] type_id check_cast(function_scope& scope, ast::expr_id id, ast::cast const& node);

    // ---- statements and control flow (check_stmt.cc) ----------------------------------------------------------------

    /// The statements of one list in order; what follows an exit is reported once as `unreachable-code`.
    [[nodiscard]] flow check_statements(function_scope& scope, ast::range_of<ast::stmt_id> statements);
    /// A body as a scope of its own: what it declares is gone behind it.
    [[nodiscard]] flow check_nested(function_scope& scope, ast::body const& body);
    [[nodiscard]] flow check_stmt(function_scope& scope, ast::stmt_id stmt);
    void check_let(function_scope& scope, ast::stmt_id id, ast::let_stmt const& let);
    void check_assign(function_scope& scope, ast::stmt_id id, ast::assign_stmt const& assign);
    [[nodiscard]] flow check_if(function_scope& scope, ast::if_stmt const& chain);
    void check_for(function_scope& scope, ast::stmt_id id, ast::for_stmt const& loop);
    void check_return(function_scope& scope, source_span where, ast::expr_id value);
    void check_break(function_scope& scope, source_span where, ast::expr_id value);
    void check_condition(function_scope& scope, ast::expr_id condition);
    /// Declares a local, which shadows every earlier local, parameter and module-level symbol of its name (CHK-53, CHK-54).
    void declare_local(function_scope& scope, local_name local);
    /// A `loop:`; the result is the type its breaks carry, and `nothing` for one that is a statement.
    /// `has_break` is false for a loop nothing leaves, which never ends.
    [[nodiscard]] type_id check_loop(function_scope& scope,
                                     ast::expr_id id,
                                     ast::loop_expr const& loop,
                                     bool yields_value,
                                     bool& has_break);
    /// A `case`; the result is the type of its arms, and `nothing` for one that is a statement.
    /// `ending`, where given, is how the `case` ends as a statement: it exits when it is exhaustive and every arm exits.
    [[nodiscard]] type_id check_case(function_scope& scope,
                                     ast::expr_id id,
                                     ast::case_expr const& node,
                                     bool yields_value,
                                     flow* ending = nullptr);
    /// One arm's pattern, against the scrutinee's type; appends the enum cases it names, and says whether all were cases.
    void check_pattern(function_scope& scope,
                       ast::expr_id pattern,
                       type_id scrutinee,
                       cc::vector<i32>& named_cases,
                       bool& is_all_constant);
    void check_yield(function_scope& scope, source_span where, ast::expr_id value);
    /// Reports every loop of calls once, and marks the functions on it.
    void find_recursion();
    /// True when `function` and every function it calls checked soundly and none is recursive, so all of it inlines.
    [[nodiscard]] bool inlines_whole(symbol_id function);

    [[nodiscard]] type_id check_expr(function_scope& scope, ast::expr_id expr);
    [[nodiscard]] type_id check_literal(function_scope& scope, ast::expr_id id, ast::literal const& literal);
    [[nodiscard]] type_id check_name(function_scope& scope, ast::expr_id id, ast::name const& name);
    [[nodiscard]] type_id check_member(function_scope& scope, ast::expr_id id, ast::member const& member);
    [[nodiscard]] type_id check_call(function_scope& scope, ast::expr_id id, ast::call const& call);
    [[nodiscard]] type_id check_logical(function_scope& scope, ast::expr_id id, ast::call const& call);
    [[nodiscard]] type_id check_chain(function_scope& scope, ast::expr_id id, ast::comparison_chain const& chain);
    /// The one `@operator` function of `spelling` that takes exactly `types`; `none` after a report at `where`.
    /// It fills no side table, so it serves an operator that is no expression: of a chain, of a compound assignment.
    [[nodiscard]] symbol_id resolve_operator(i32 file,
                                             source_span where,
                                             cc::string_view spelling,
                                             cc::span<type_id const> types,
                                             cc::span<ast::expr_id const> operands = {});
    /// True for a function whose inferred result is being compiled right now and whose parameters do not take `types`.
    /// Its parameters are known by then, so a call that could never choose it does not need its result.
    /// That keeps an overload set usable from inside one of its own inferred members, where demanding it would be a cycle.
    [[nodiscard]] bool is_out_of_the_running(i32 file, symbol_id candidate, call_arguments const& arguments);
    /// Which argument fills which parameter, by position and by name; says why where the arguments do not bind.
    [[nodiscard]] bound_arguments bind_arguments(cc::span<parameter const> parameters,
                                                 call_arguments const& arguments) const;
    /// How `candidate`, which is checked, takes the arguments: its slots and the chain of each argument.
    /// Nothing where the arguments do not bind or one does not convert.
    /// It reports nothing, so a literal can try every candidate.
    [[nodiscard]] cc::optional<candidate_match> match(i32 file, symbol_id candidate, call_arguments const& arguments);
    /// The length of the chain that converts written argument `i` to `parameter`; nothing where it does not convert.
    [[nodiscard]] cc::optional<i32> chain_of(i32 file, parameter const& taking, call_arguments const& arguments, isize i);
    /// Whether `expr`, already checked, names a place a body could assign, reporting why not; `what` names the use.
    bool judge_place(function_scope& scope, ast::expr_id expr, cc::string_view what);
    /// The chain of tuple or object literal `literal` to `to`: one more than its call's longest (CHK-84).
    [[nodiscard]] cc::optional<i32> literal_chain(i32 file, type_id to, i32 literal);
    /// The functions of the name of the struct `to` visible from `file`, its synthesized constructor among them.
    [[nodiscard]] cc::vector<symbol_id> functions_named_after(i32 file, type_id to) const;
    /// The matches that remain after CHK-192, CHK-254 and CHK-255; one is the target, more are ambiguous.
    [[nodiscard]] cc::vector<candidate_match> best_of(cc::vector<candidate_match> matches) const;
    /// True where number literal `n` holds exactly in the prelude's numeric type `to` (CHK-253).
    [[nodiscard]] bool holds(number_literal const& n, type_id to) const;
    /// The prelude's `float`, `int` or `uint` by name, as a literal's conversion knows them; `none` for another name.
    [[nodiscard]] type_id prelude_type(cc::string_view name) const;
    /// What a number literal `expr` holds; `is_number` is false for any other expression.
    [[nodiscard]] number_literal number_of(i32 file, ast::expr_id expr) const;
    /// The elements of tuple or object literal `expr`, checked where they are no literal themselves, into `literals`.
    [[nodiscard]] i32 shape_literal(function_scope& scope, ast::expr_id expr);
    /// Converts literal `expr`, shaped as `literal`, to `to` by a call of `to`'s name (CHK-81, CHK-85).
    type_id resolve_literal(function_scope& scope, ast::expr_id expr, type_id to, i32 literal);
    /// `expr` where the type `to` is expected, which a literal converts to (CHK-82); reports a value of another type.
    type_id check_expected(function_scope& scope, ast::expr_id expr, type_id to, cc::string_view what = {});
    /// A square literal: of the array type `to` where one is expected, and of its first element's type otherwise.
    type_id check_array_literal(function_scope& scope, ast::expr_id expr, type_id to);
    /// `T[N].filled(v)`, which `check_dot_call` hands over once its object names an array type.
    type_id check_filled(function_scope& scope, ast::expr_id id, ast::call const& call);
    /// Gives each literal argument of a chosen call the type of the parameter it fills.
    void commit_literals(function_scope& scope,
                         call_arguments const& arguments,
                         cc::span<parameter const> parameters,
                         cc::span<i32 const> slots);
    void commit_literals(function_scope& scope,
                         call_arguments const& arguments,
                         cc::span<parameter const> parameters,
                         cc::span<i32 const> slots,
                         cc::vector<type_id>& bindings);
    /// Why each of `candidates` did not match call `call`, kept for a later "did you mean".
    void note_near_misses(i32 file,
                          ast::expr_id call,
                          cc::span<symbol_id const> candidates,
                          call_arguments const& arguments);
    /// Remembers how call `id` fills the parameters of `callee`, which is what the flat tree is written from.
    void record_call(i32 file,
                     ast::expr_id id,
                     symbol_id callee,
                     call_arguments const& arguments,
                     cc::span<i32 const> slots,
                     cc::span<type_id const> bindings = {});
    /// The candidates of `spelling` that take exactly `types`, without a report; what the flat tree is written from.
    [[nodiscard]] symbol_id find_operator(cc::string_view spelling, cc::span<type_id const> types) const;
    [[nodiscard]] call_arguments check_arguments(function_scope& scope,
                                                 ast::range_of<ast::argument> range,
                                                 bool is_constructor);
    [[nodiscard]] type_id resolve_overload(function_scope& scope,
                                           ast::expr_id id,
                                           ast::expr_id callee,
                                           cc::span<symbol_id const> candidates,
                                           call_arguments const& arguments,
                                           cc::string_view spelling,
                                           call_spelling written_as = call_spelling::free);
    /// `a.foo(…)` or `T.foo(…)`, whose callee is the member `callee`.
    [[nodiscard]] type_id check_dot_call(function_scope& scope, ast::expr_id id, ast::call const& call);
    [[nodiscard]] cc::string signature_text(cc::string_view spelling, cc::span<type_id const> types) const;
    /// A call as it was written, each named argument with its name and a number literal as its text:
    /// `sub(int, b = 2.5)`; `file_of_call` is the file its arguments stand in.
    [[nodiscard]] cc::string call_text(i32 file_of_call, cc::string_view spelling, call_arguments const& arguments) const;
    /// Records a call of `callee`, a function of the program, as an edge of the call graph, and reports each binding it
    /// reads that the caller does not list.
    void note_program_call(function_scope const& scope, symbol_id callee, source_span where);

    // ---- the flat tree (flatten.cc) ---------------------------------------------------------------------------------

    /// True when every member of the type, at any depth, has a type.
    [[nodiscard]] bool is_sound(type_id type) const;
    void flatten_entry_point(symbol_id id);
};
} // namespace sgl::check::impl
