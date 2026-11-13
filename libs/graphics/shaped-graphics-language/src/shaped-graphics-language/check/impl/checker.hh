#pragma once

#include <clean-core/bytes/hash128.hh>
#include <clean-core/container/fixed_array.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/check/check.hh>
#include <shaped-graphics-language/check/features.hh>
#include <shaped-graphics-language/check/layered_map.hh>

namespace sgl::check::impl
{
/// How a number literal reads before any literal types exist.
enum class number_class : u8
{
    /// A decimal literal with a DOT or an exponent and no suffix.
    plain_float,
    /// Decimal digits, or hexadecimal or binary ones behind `0x` or `0b`, and nothing else.
    plain_integer,
    /// One of the two above with a suffix, which names its type (CHK-357): `1u`, `0.5f`, `7i32`.
    suffixed,
    /// A `p` exponent, or anything else that reads as no number of the ones above.
    other,
};

[[nodiscard]] number_class classify_number(cc::string_view text);

/// A `suffixed` number taken apart.
struct suffixed_number
{
    /// The number without its suffix, a `plain_integer` or a `plain_float`.
    cc::string_view body;
    /// `i`, `u` or `f`.
    char letter = 0;
    /// 32 where the suffix writes none.
    i32 width = 32;
};

/// Nothing where `text` is no `suffixed` number.
[[nodiscard]] cc::optional<suffixed_number> split_suffix(cc::string_view text);

/// `text` must be a `plain_float`; nullopt when the value does not fit an f64.
[[nodiscard]] cc::optional<f64> parse_plain_float(cc::string_view text);

/// `text` must be a `plain_integer`; nullopt when the value does not fit an i32.
[[nodiscard]] cc::optional<i32> parse_plain_integer(cc::string_view text);

/// `text` must be a `plain_integer`; nullopt when the value does not fit an i64, which is what a literal is held in
/// until the type it meets is known (CHK-61).
[[nodiscard]] cc::optional<i64> parse_literal_integer(cc::string_view text);
/// The value of an unsigned integer literal from 2^63 to 2^64 - 1, which `parse_literal_integer` does not hold.
[[nodiscard]] cc::optional<u64> parse_literal_past_long(cc::string_view text);

/// Whether the number literal `text` is written in hex, suffixed or not.
[[nodiscard]] bool is_hex_literal(cc::string_view text);

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
    /// `integer` is the bits of a value past 2^63 - 1, which a `ulong` alone holds (CHK-450).
    bool is_past_long = false;
};

/// A generic argument a call states: by position where `name` is empty, else by the parameter's name (CHK-340).
struct stated_argument
{
    cc::string_view name;
    type_id type = type_id::none;
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
    /// Parallel to `written`: the case a leading dot names, which meets a parameter of an enum holding it (CHK-152);
    /// empty for anything else, whose type is in `types`.
    cc::vector<cc::string_view> leading_dots;
    /// An argument had the error type or was reported, so the call reports nothing about its arguments.
    bool is_poisoned = false;
    /// The generic arguments the call states, `f[float, .B = int](…)`, in the order written (CHK-340).
    cc::vector<stated_argument> stated;
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
    /// The instance whose body it stands in, whose tables hold its type; -1 outside one.
    i32 instance = -1;
};

/// A generic function being instantiated, innermost last: what an error in its body is noted under (CHK-338).
struct instantiation
{
    /// A position in `checked_module::function_instances`.
    i32 instance = -1;
    /// The call that asked for it.
    i32 file = 0;
    source_span where;
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

/// One access of a binding array by index in one entry point's inlined body, which `judge_indices` judges with every
/// other access the same index text makes (CHK-300, CHK-401).
struct index_use
{
    /// The entry point's function.
    symbol_id entry = symbol_id::none;
    /// The index as written, its mark included: what every inlined copy of one access shares.
    origin index;
    /// The marked argument where there is a mark, and the index itself where there is none.
    origin written;
    builtins::index_mark mark = {};
    /// Proven the same in every invocation; proven to vary, by what each invocation is handed (CHK-399); or neither.
    bool is_uniform = false;
    bool varies = false;
    /// What made it not uniform, for a note.
    related_note why;
    /// The calls this copy was inlined through, outermost first.
    cc::vector<call_site> through;
};

/// One `use` line of a file: the name it binds, and the module that name stands for.
struct module_use
{
    cc::string name;
    i32 module = -1;
    /// The whole line, which a clash with the name is reported at.
    source_span where;
};

/// How the files behind the prelude fall into modules, read from their `module` and `use` lines before anything is
/// declared.
struct module_plan
{
    /// The library files the program reaches, as positions in the library, in the order they are checked.
    cc::vector<i32> library_order;
    /// Each module's name; module 0 is the program's own, whose name is empty where its file declares none.
    cc::vector<cc::string> modules;
    /// Parallel to `library_order` and then the program: the module of each file.
    cc::vector<i32> file_module;
    /// Parallel to `file_module`: the `use` lines of each file that name a module.
    cc::vector<cc::vector<module_use>> uses;
    /// What the plan found wrong, each `file` a position in `library_order`, and the program's one past its end.
    cc::vector<located_diagnostic> diagnostics;
};

/// The modules `program` reaches through `use`, from the files of `library` grouped by their `module` line (CHK-346).
/// A library file without a `module` line is no module's, and is left out.
[[nodiscard]] module_plan plan_modules(cc::span<module_file const> library, module_file program);

/// The module `f`'s `module` line names, `a.b` as one name; empty without one.
[[nodiscard]] cc::string module_name_of(module_file const& f);

/// Whether `plan` reaches library modules a layer may hold: some, and none of them the program's own module.
/// A library file of the program's module shares the program's scope, which a program's check starts afresh.
[[nodiscard]] bool may_layer(module_plan const& plan);

/// How long each list is that an id or a position of the check pass names, at one moment of a check.
/// A chain's link is relocated by these: an id below a link's start stays, one in a later link's part moves with it.
struct table_sizes
{
    isize symbols = 0;
    isize types = 0;
    isize members = 0;
    isize enum_cases = 0;
    isize functions = 0;
    isize parameters = 0;
    isize bindings = 0;
    isize binding_lists = 0;
    isize type_lists = 0;
    isize samplers = 0;
    isize pipelines = 0;
    isize pipeline_settings = 0;
    isize constants = 0;
    isize call_records = 0;
    isize written_arguments = 0;
    isize call_slots = 0;
    isize function_instances = 0;
    isize target_texts = 0;
    isize literals = 0;
    isize function_arguments = 0;

    constexpr bool operator==(table_sizes const&) const = default;
};

/// The one demand-driven pass; every member function only appends to `out` and flips symbol states.
struct checker
{
    cc::span<module_file const> files;
    builtins::registry const& builtins;
    checked_module out;
    /// The values this compile gives the options, by the name each is set by (CHK-354).
    cc::span<option_value const> options;
    /// What this compile writes for, which the flattener substitutes for `target_api`, `target_language` and `has`
    /// (CHK-426).
    compile_target compiled_target;

    /// A statement of a body in a target's own text that stands where one may (CHK-427).
    struct text_statement
    {
        i32 file = 0;
        ast::stmt_id stmt = ast::stmt_id::none;
        /// The last of its run, which is where the body ends.
        bool ends_run = false;
    };
    cc::vector<text_statement> text_statements;
    /// The functions whose bodies `plan_target_text` read already, which an instance does not read again.
    cc::vector<symbol_id> planned_text_functions;

    /// The token index of `stmt`'s opening quote where it is a tagged string, a body in a target's text; -1 otherwise.
    [[nodiscard]] i32 tagged_text_token(i32 file, ast::stmt_id stmt) const;
    /// Finds the runs of tagged strings `function`'s body may hold, and registers each in `out.target_texts`.
    /// A run is the whole of `statements`, or of a branch of a `const if` that is the whole of them, at any depth.
    void plan_target_text(symbol_id function, i32 file, ast::range_of<ast::stmt_id> statements);
    void register_text_run(symbol_id function, i32 file, cc::span<ast::stmt_id const> run);
    /// A tagged string's language and its text, whole lines each ending in a newline, where it has no placeholder.
    /// `what` names what it is in a diagnostic: `a preamble`.
    struct plain_text
    {
        shading_language language = shading_language::sgl;
        cc::string text;
    };
    [[nodiscard]] cc::optional<plain_text> plain_target_text(i32 file, ast::expr_id expr, cc::string_view what);
    /// CHK-432: the branches of a struct whose body is a `const if` chain, into `target_type_branches`.
    /// False where it has no SGL branch, which leaves it opaque.
    bool compile_type_branches(symbol_id id, i32 file, ast::struct_decl const& s);
    /// CHK-522: the branches of a `@target_layout` struct, each a struct of the same fields in its own order, into
    /// `target_type_branches`; `fields` are its first branch's, which the check judges it by.
    void compile_layout_branches(symbol_id id,
                                 type_id type,
                                 i32 file,
                                 ast::struct_decl const& s,
                                 ast::range_of<member_info> fields);
    /// Whether `file` belongs to a module SGL ships, the only place a `@target_layout` struct is declared (CHK-522).
    [[nodiscard]] bool is_shipped_module_file(i32 file) const;
    /// Whether `type` is a struct with a target layout (CHK-522).
    [[nodiscard]] bool has_target_layout(type_id type) const;
    /// CHK-522: a struct with a target layout stands as a buffer's element and a value, so the field or the array
    /// element `type` is reports it at `where`; true where it did.
    bool refuse_held_layout(i32 file, source_span where, type_id type, cc::string_view holder);
    /// CHK-522: what each type holding a struct with a target layout is on this compile, the struct of the branch it
    /// takes or a buffer of one, made before any entry point is flattened, since flattening interns no type.
    void prepare_target_layouts();
    /// CHK-522: each binding of the symbols from `first` on, its buffers of a struct with a target layout made buffers of
    /// the struct this compile writes, once every tree is flattened against the type the check judged.
    void place_layout_bindings(isize first);
    /// Pairs of a type and what this compile writes in its place (CHK-522); empty where no compile reaches one.
    cc::vector<cc::pair<type_id, type_id>> target_layouts;
    /// The structs with a target layout that no branch of holds on this compile, which a flattened tree reaching one
    /// reports.
    cc::vector<type_id> unplaced_layouts;
    /// A text type's condition as nodes a compile decides alone, appended to `into`; false where it reads anything else.
    bool target_condition_of(i32 file, ast::expr_id expr, cc::vector<target_condition>& into);
    /// CHK-431: `@barrier`, `@subgroup` and `@derivatives` declare what a body in a target's text does, and stand on
    /// such a function alone; a barrier is an effect, so it is never `@pure` as well.
    void judge_text_effects(symbol_id function, i32 file, ast::range_of<ast::attribute> attributes);
    /// How a tagged string ends its block, where `stmt` is one; none for any other statement.
    [[nodiscard]] cc::optional<flow> target_text_flow(i32 file, ast::stmt_id stmt);
    /// A file's tagged string at file scope, registered in `out.target_preambles` (CHK-428).
    /// A chain's branches register theirs under `within` and each branch's condition (CHK-528).
    void declare_preamble(i32 file, ast::preamble_decl const& preamble, cc::span<target_condition const> within = {});

    /// How many of `files` are the prelude's; every other file is a module's.
    i32 prelude_count = 0;
    /// The modules of the files behind the prelude: 0 is the program's, every other one a module it reaches by `use`.
    cc::vector<cc::string> modules;
    /// Parallel to `files`: the module of each, and -1 for a file of the prelude.
    cc::vector<i32> file_module;
    /// Parallel to `files`: the `use` lines of each, which name a module and nothing else (CHK-347).
    cc::vector<cc::vector<module_use>> uses;

    /// The prelude's scope: every name its files declare but those of `@operator` functions.
    /// More than one symbol under a name means all of them are functions; the same holds for `module_scopes`.
    layered_map<cc::string, cc::vector<symbol_id>> prelude_names;
    /// Parallel to `modules`: the scope every file of the module shares, the inner one.
    cc::vector<cc::map<cc::string, cc::vector<symbol_id>>> module_scopes;
    /// Parallel to `modules`: each name the module declares merged with the prelude's of that name, where two overload
    /// sets of one name merge (CHK-189); a name it does not declare is found in `prelude_visible`.
    /// Built once every file is declared, from the module's names alone, so it costs what the program declares.
    cc::vector<cc::map<cc::string, cc::vector<symbol_id>>> merged_scopes;
    /// What a module's file finds of the prelude: `prelude_names` without the `@internal` symbols (CHK-323).
    /// Built once, with the prelude, and shared by every program's check behind it.
    layered_map<cc::string, cc::vector<symbol_id>> prelude_visible;
    bool has_prelude_visible = false;
    /// `@operator` functions by operator spelling.
    layered_map<cc::string, cc::vector<symbol_id>> operators;
    /// The prelude's alone, which is all a prelude file sees (CHK-190).
    layered_map<cc::string, cc::vector<symbol_id>> prelude_operators;
    /// The functions of each struct's and enum's type scope, keyed by the index of the type's symbol, then by name.
    /// Members are declared with their type, and extensions once every file is declared (CHK-233, CHK-237).
    layered_map<i32, cc::map<cc::string, cc::vector<symbol_id>>> type_scopes;
    /// The types `resource_type` and `buffer_type` interned, by spelling, so a mention looks up only its equals.
    /// The function arguments of every call checked so far; `call_arguments::functions` indexes it.
    cc::vector<function_argument> function_arguments;
    /// True while a parameter's type is resolved, the one place a function type may stand (CHK-317).
    bool allows_function_type = false;
    /// The type being resolved is a pixel entry point's result, whose attributes are its target's settings (CHK-418).
    bool allows_type_attributes = false;
    layered_map<cc::string, cc::vector<type_id>> interned_types;
    /// Every instance of a generic struct, by its generic and its argument: what `existing_instance` finds.
    layered_map<u64, type_id> instances;
    /// The open types and the generic calls of the layers below, which `instantiate_generics` substitutes into.
    cc::vector<type_id> open_types_below;
    cc::vector<isize> open_calls_below;
    cc::vector<pending_extension> pending_extensions;
    /// Integer literals that do not fit an `int`, judged once every literal has met the type it converts to (CHK-61).
    cc::vector<wide_literal> wide_literals;
    /// The elements of every tuple and object literal an argument or an expected type met, each as the arguments of the
    /// call it converts by (CHK-81); positions stay valid while it grows.
    cc::vector<call_arguments> literals;
    /// The symbols in compilation, outermost first, which is the loop a dependency cycle names.
    cc::vector<symbol_id> compiling;
    cc::vector<function_notes> notes;

    /// PROTOTYPE (autodiff): the concrete overload `fwd_diff` or `bwd_diff` takes for one function, which check_diff.cc
    /// writes when a call names that function: its parameters, and which of them is which.
    struct derivative_signature
    {
        symbol_id function = symbol_id::none;
        bool is_reverse = false;
        /// Per parameter of `function`: where its value and its tangent or adjoint stand, -1 where it takes none.
        cc::vector<i32> value_slots;
        cc::vector<i32> tangent_slots;
        /// Backward: where the result's adjoint stands.
        i32 result_slot = -1;
    };
    /// By the overload's symbol.
    cc::map<i32, derivative_signature> derivative_signatures;
    /// The overload of each function and mode, by `2 * symbol + is_reverse`.
    cc::map<i64, symbol_id> derivative_overloads;
    /// A `const` in a function body (CHK-413): a symbol no file scope sees, with the earlier ones of its body it reads.
    struct local_constant
    {
        i32 file = 0;
        ast::decl_id declaration = ast::decl_id::none;
        symbol_id symbol = symbol_id::none;
        cc::vector<local_name> visible;
    };
    cc::vector<local_constant> local_constants;
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
    /// The place of the assignment `check_assign` is checking right now, and whether it is a compound one.
    ast::expr_id assigned = ast::expr_id::none;
    bool is_compound_assigned = false;
    /// What `check_texel` left of a texel that is that place: the subscript's arguments, the image first, which the
    /// assignment's `store` takes with its value; and the `load` a compound one reads with.
    ast::expr_id texel_place = ast::expr_id::none;
    call_arguments texel_arguments;
    i32 texel_load = -1;
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
    /// Every access of a binding array by index, in each entry point that reaches it.
    cc::vector<index_use> index_uses;

    // ---- what a link of a chain records for its relocation (relocate.cc) --------------------------------------------

    /// Set while a link of a library's chain is checked.
    bool records_interface = false;
    /// The files the layers below declared, which a body read from is another layer's.
    i32 run_from_files = 0;
    /// The lists' sizes when this check began, and once every signature and default was checked, before any body:
    /// what lies between is the link's interface, which `interface_digest` hashes with no place in a file in it.
    table_sizes run_start;
    table_sizes interface_end;
    cc::hash128 interface_digest;
    /// The same over the link's whole part once it is checked: a link checked again into the same is kept whole.
    cc::hash128 part_digest;
    /// The functions of lower layers whose bodies this layer inlined or instantiated: an edit of one checks it again.
    mutable cc::vector<symbol_id> bodies_read;
    /// Where this check's own entries of `bodies_read` begin.
    isize bodies_read_from = 0;
    void note_body_read(symbol_id function) const;

    /// One link of a chain as the links after it were checked over it.
    struct chain_part
    {
        cc::string module;
        table_sizes start;
        table_sizes interface_end;
        table_sizes end;
        /// How many files the layers up to this one declared.
        i32 files = 0;
        cc::hash128 interface_digest;
        cc::hash128 part_digest;
        /// The sources of the link's own files: an inlined body is read from its AST, which no table holds.
        cc::hash128 sources_digest;
    };
    /// Every link of the chain up to this one, the first first: what a later link is moved past an edit by.
    cc::vector<chain_part> chain;

    // ---- shared helpers (check.cc) ----------------------------------------------------------------------------------

    [[nodiscard]] parsed_file const& file_of(i32 file) const { return files[file].file; }
    [[nodiscard]] ast::file_ast const& ast_of(i32 file) const { return files[file].ast; }
    [[nodiscard]] cc::string_view text_of(i32 file, source_span where) const { return file_of(file).text_of(where); }
    [[nodiscard]] bool is_prelude_file(i32 file) const { return file < prelude_count; }
    /// `target_api` or `target_language`, the prelude's consts each compile stands for once inlined (CHK-426).
    [[nodiscard]] bool is_target_constant(symbol_id s) const
    {
        auto const& named = out.at(s);
        return named.kind == symbol_kind::constant && is_prelude_file(named.file)
            && (named.name == "target_api" || named.name == "target_language");
    }
    /// The program is the last file, behind the prelude and the library files it reaches.
    [[nodiscard]] i32 program_file() const { return i32(files.size()) - 1; }
    /// The module-level names a lookup from `file` finds: a prelude file sees the prelude alone, and a module's file sees
    /// its module's names over the prelude's, and no other module's.
    [[nodiscard]] cc::vector<symbol_id> const* seen_from(i32 file, cc::string_view name) const
    {
        if (is_prelude_file(file))
            return prelude_names.get_ptr(name);
        if (auto const* const merged = merged_scopes[file_module[file]].get_ptr(name))
            return merged;
        return prelude_visible.get_ptr(name);
    }
    /// The module a `use` line of `file` binds to `name`; -1 for none.
    [[nodiscard]] i32 used_module(i32 file, cc::string_view name) const
    {
        for (auto const& u : uses[file])
            if (u.name == name)
                return u.module;
        return -1;
    }
    /// How a diagnostic in `file` names `id`: `m.name` for a symbol of another module, by that module's own name.
    [[nodiscard]] cc::string name_seen_from(i32 file, symbol_id id) const;
    /// The module `expr` names: a bare name a `use` of `file` binds, which no local of `scope` and no type parameter
    /// hides; -1 for anything else.
    [[nodiscard]] i32 module_named(i32 file, ast::expr_id expr, function_scope const* scope) const;
    /// The module-level symbols `expr` names from `file`: a bare name, or `m.name` where `m` is a module (CHK-348).
    /// Null for any other expression, for a name a local of `scope` or a type parameter hides, and for a name that
    /// finds nothing.
    /// `is_qualified`, where given, says whether it was `m.name`.
    [[nodiscard]] cc::vector<symbol_id> const* symbols_named(i32 file,
                                                             ast::expr_id expr,
                                                             function_scope const* scope,
                                                             bool* is_qualified = nullptr) const;
    /// The `@operator` functions a use of an operator in `file` may choose from, by spelling.
    [[nodiscard]] layered_map<cc::string, cc::vector<symbol_id>> const& operators_seen_from(i32 file) const
    {
        return is_prelude_file(file) ? prelude_operators : operators;
    }
    /// The `@operator` functions of `spelling` that `file` sees: the prelude's, its module's, and those of every module
    /// it uses, since an operator has no qualified spelling (CHK-350).
    [[nodiscard]] cc::vector<symbol_id> operators_of(i32 file, cc::string_view spelling) const
    {
        auto result = cc::vector<symbol_id>();
        auto const* const found = operators_seen_from(file).get_ptr(spelling);
        if (found == nullptr)
            return result;
        for (auto const candidate : *found)
        {
            auto const from = out.at(candidate).file;
            auto is_seen = is_prelude_file(from) || file_module[from] == file_module[file];
            for (auto const& u : uses[file])
                is_seen = is_seen || u.module == file_module[from];
            if (is_seen)
                result.push_back(candidate);
        }
        return result;
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
    /// Gives the one standing there or the one made.
    located_diagnostic& report_once(diagnostic_kind kind, i32 file, source_span where, cc::string_view detail);
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

    /// The whole pass, or what remains of it behind `from`, the state a checked prelude left.
    /// Every step past the declarations runs only over what `from` does not hold, or is a whole-module step that reads
    /// the prelude's part without changing it: so a program's check behind a checked prelude is the check of both.
    void run(resume_point from = {});
    void declare_file(i32 file);
    void declare(i32 file, ast::decl_id decl);
    void add_symbol(symbol s, source_span name_where);
    /// Lays each module's scope over the prelude's into `merged_scopes`, and reports a `use` whose name the file's module
    /// declares too.
    void merge_scopes();
    /// Lays `scope` over `names`: two overload sets of one name merge, and anything else of `scope` hides what `names`
    /// has of its name, unless that is `@shadowable(false)` (CHK-188, CHK-220).
    void merge_scope(cc::map<cc::string, cc::vector<symbol_id>>& names,
                     cc::map<cc::string, cc::vector<symbol_id>> const& scope);
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
    /// Every `fun T.name` from `pending_extensions[first]` on, into the type scope of `T`; run once every file is declared.
    void attach_extensions(isize first);
    /// Reports each integer literal from `wide_literals[first]` on that no conversion took to a type holding it (CHK-61).
    void judge_wide_literals(isize first);
    /// Two functions of one overload set whose parameters agree in type, name and named-only mark (CHK-241).
    /// Only the sets that hold a symbol from `first` on, since the others were judged with the prelude.
    void judge_redeclarations(isize first);
    /// The functions a call of `name` from `file` may choose from, where `first` is its first argument's type:
    /// the functions of that name visible there, and those of the type scope of `first` (CHK-247).
    [[nodiscard]] cc::vector<symbol_id> candidates_of(i32 file, cc::string_view name, type_id first) const;
    /// A declaration of the prelude marked `@internal`, which no lookup from the program's file finds (CHK-323).
    [[nodiscard]] bool is_internal(symbol_id id) const;
    /// True where a lookup from `file` sees a function of `from`: the prelude never sees the program's.
    /// The program never sees what the prelude keeps internal.
    [[nodiscard]] bool is_visible_from(i32 file, symbol_id from) const
    {
        return is_prelude_file(file) ? is_prelude_file(out.at(from).file) : !is_internal(from);
    }
    /// Gives every struct from symbol `first` on that has a block its synthesized constructor (CHK-239).
    /// Run once every file is declared, so the symbols declared before keep their ids.
    void declare_constructors(isize first);

    /// Compiles the symbol when nobody has, and reports a cycle when somebody is.
    /// The state it returns is `checked` or `failed`, or `in_compilation` for a cycle, which was reported at `where`.
    symbol_state demand(symbol_id id, i32 file, source_span where);
    void compile(symbol_id id);
    void compile_struct(symbol_id id);
    void compile_enum(symbol_id id);
    /// A `type` alias: the type its value names, which a function type may be as a parameter's whole type (CHK-14).
    void compile_alias(symbol_id id);
    /// The struct or enum `id` names: itself for one, and for an alias the one whose type it names.
    /// `none` for every other symbol, and for an alias of any other type or one that failed.
    [[nodiscard]] symbol_id declared_type_of(symbol_id id, i32 file, source_span where);
    /// A `const`: its value is a number literal, an enum case or another `const`, and anything else is `unsupported-yet`.
    void compile_const(symbol_id id);
    /// A function of no parameter and no binding whose body is a value computed before anything runs, a const's or a
    /// case's, named `name` and standing at `decl` of `file`; it is checked as a test's body is (CHK-224).
    [[nodiscard]] symbol_id add_value_scope(i32 file, ast::decl_id decl, cc::string name);
    /// CHK-146: the `int` a case's written `value` computes, once and before anything runs, as a const's is (CHK-219).
    /// None where it is no `int`, does not compute, or reaches an option, each reported.
    [[nodiscard]] cc::optional<i32> compute_case_value(i32 file, ast::decl_id member, ast::expr_id value);
    /// CHK-354: `info` as the compile's value for option `id` sets it, or as written where it sets none.
    /// A value of another type than the option's is `invalid-option`, and leaves the default.
    void apply_option_value(symbol_id id, constant_info& info);
    /// CHK-354: a value for a name no option is set by, or for a name given twice, is `invalid-option`.
    void judge_option_values();
    /// Records that `where` of `file` names the option `c` comes from, if it comes from one (CHK-355).
    void note_option(i32 file, source_span where, constant_info const& c);
    /// CHK-355: the options named by `function`, every function it calls, the bindings it lists, and the structs any of
    /// those name, in declaration order.
    /// `also` is a further function the tree inlines without a call of the source, such as a fused any hit; or `none`.
    [[nodiscard]] cc::vector<symbol_id> options_reached(symbol_id function, symbol_id also);
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
    /// `@preferred_subgroup_size(n)`'s `n` (CHK-371); 0 without the attribute, and after a bad one, which it reports.
    [[nodiscard]] i32 preferred_subgroup_size_of(i32 file, ast::attribute const* a, bool is_compute);
    /// `@padding(.none)` on a binding or a buffer member; anything else reports and leaves the natural padding.
    [[nodiscard]] padding_kind padding_of(i32 file, ast::attribute const* a);
    /// CHK-435: `@float(.specials)` and the other levels; `fast` without the attribute, and after a bad one, which it reports.
    [[nodiscard]] float_level float_level_of(i32 file, ast::attribute const* a);
    /// `@geometry(max_vertices = N)`'s `N`, from 1 to 256; 1 after a bad argument, which it reports (CHK-301).
    [[nodiscard]] i32 max_vertices_of(i32 file, ast::attribute const& a);
    /// `@mesh(x, y, z, vertices = N, triangles = M)`: its workgroup and its output's bounds (CHK-480).
    struct mesh_shape
    {
        cc::fixed_array<i32, 3> workgroup = {1, 1, 1};
        i32 vertices = 0;
        /// Triangles, or lines where `is_lines`.
        i32 triangles = 0;
        bool is_lines = false;
    };
    /// A bad argument reports and leaves a shape of one vertex and one triangle.
    [[nodiscard]] mesh_shape mesh_of(i32 file, ast::attribute const& a);
    /// `@task(x, y, z, meshes = N)`: its workgroup, and the mesh workgroups one of them launches at most, 0 where it
    /// states none (CHK-484); a bad argument reports and leaves a workgroup of one and no bound.
    struct task_shape
    {
        cc::fixed_array<i32, 3> workgroup = {1, 1, 1};
        i32 meshes = 0;
    };
    [[nodiscard]] task_shape task_of(i32 file, ast::attribute const& a);
    struct tessellation_mode
    {
        tessellation_partitioning partitioning = tessellation_partitioning::integer;
        bool is_clockwise = true;
        i32 max_factor = k_max_tessellation_factor;
    };
    /// `@tessellation_control(partitioning = …, winding = …, max_factor = N)`, the first two named (CHK-304).
    [[nodiscard]] tessellation_mode tessellation_of(i32 file, ast::attribute const& a);
    /// `point_stream[T]`, `line_stream[T]` and `triangle_stream[T]` in a type position (CHK-302).
    [[nodiscard]] type_id resolve_stream(i32 file, ast::expr_id expr, ast::index const& node, function_scope const* scope);
    /// CHK-303: `s.emit(v)` or `s.end_strip()` on a geometry stage's stream, which `check_dot_call` hands on.
    type_id check_stream_call(function_scope& scope, ast::expr_id id, ast::call const& call, type_id stream);
    /// `out.vertex(i, v)` of a mesh output, whose `v` is of the output's own type (CHK-482).
    type_id check_mesh_vertex(function_scope& scope, ast::expr_id id, ast::call const& call, type_id output);
    /// `dispatch_mesh(x, y, z, payload)` of a task stage, whose payload is any struct of the program (CHK-485).
    type_id check_dispatch_mesh(function_scope& scope, ast::expr_id id, ast::call const& call);
    /// CHK-487: each pipeline's mesh stage takes the payload its task stage hands on, once every body is checked.
    void judge_mesh_payloads();
    /// CHK-301 to CHK-306: an entry point of the geometry or a tessellation stage, which `judge_entry_point` hands on.
    void judge_primitive_stage(symbol_id id, cc::function_ref<void(cc::string_view)> invalid);
    /// The ray set a `rays` declaration made of `symbol`, and its ray types; null for any other symbol.
    [[nodiscard]] bool is_ray_set(symbol_id symbol) const;
    /// `set.ray` as an expression: the set and the ray's position, or `none` where `expr` names no ray set.
    /// A ray set without that ray type is reported, and its position is -1.
    [[nodiscard]] cc::optional<ray_trace> ray_type_of(i32 file, ast::expr_id expr);
    /// `trace(world, r, set.ray, mut payload)`, the trace of a ray-tracing stage (CHK-329).
    type_id check_pipeline_trace(function_scope& scope, ast::expr_id id, ast::call const& call, ray_trace ray);
    /// `h.invoke(mut p)` of a hit object a `trace_hit` let holds (CHK-434).
    type_id check_hit_invoke(function_scope& scope, ast::expr_id id, ast::call const& call);
    /// A hit object's let, and the ray type its trace traced.
    struct hit_let
    {
        i32 file = 0;
        i32 statement = -1;
        symbol_id set = symbol_id::none;
        i32 ray = 0;
    };
    cc::vector<hit_let> hit_lets;
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
    /// The bytes one element of a `buffer[type]` takes by EMIT-111, before any stride rounding: each value aligned to
    /// its scalar's size, and a struct sized to its largest scalar.
    [[nodiscard]] i32 storage_size_of(type_id type) const;
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
    /// The bytes one element of an array of `type` strides by at natural alignment (EMIT-110), outside a constant block.
    [[nodiscard]] i32 natural_stride_of(type_id type) const;
    /// CHK-291: the path from `type` down to the first array in it whose element strides by less than whole 16-byte rows,
    /// which a constant block cannot index without `@padding(.every_element)`; empty when there is none.
    [[nodiscard]] cc::string sub_row_array_path(type_id type) const;
    /// CHK-291: refuses each array a struct crossing a stage edge holds, reported at `where` of `file`.
    /// `is_link`: the edge is a stage link, where a one-dimensional array of builtins crosses an element per location.
    void judge_edge_arrays(i32 file, source_span where, type_id type, bool is_link = false);
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
    /// `allows_resource` takes a texture, an image or a sampler, as a parameter of a function of the program (CHK-366).
    /// `allows_atomic` takes an atomic, as a `mut` parameter does: the caller's atomic, which the body updates (CHK-297).
    [[nodiscard]] type_id resolve_value_type(i32 file,
                                             ast::expr_id expr,
                                             function_scope const* scope = nullptr,
                                             bool allows_resource = false,
                                             bool allows_atomic = false);
    /// The type of the prelude's `@builtin struct` named `name`; without one it reports at `where` and is the error type.
    [[nodiscard]] type_id type_of_builtin(cc::string_view name, i32 file, source_span where);
    /// The name of the plain vector of `count` values of `element`, `float3`; empty for an element without vectors.
    [[nodiscard]] cc::string vector_name_of(type_id element, isize count) const;
    /// The swizzle `name` is of a value of `object`, a count of zero where it is none (CHK-385).
    [[nodiscard]] swizzle swizzle_of(type_id object, cc::string_view name) const;
    /// The type of one lane of `object` where it is a vector a subscript reads a lane of: a swizzled struct whose every
    /// field has one type (CHK-395); `none` for any other type.
    [[nodiscard]] type_id lane_type_of(type_id object) const;
    /// What an unknown member of a `@swizzle` struct says about its letters; empty for any other type.
    [[nodiscard]] cc::string why_no_swizzle(type_id object, cc::string_view name) const;
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
    /// The position of the case `name` among `type`'s, -1 where `type` is no enum or has no such case.
    [[nodiscard]] i32 case_index_of(type_id type, cc::string_view name) const;
    [[nodiscard]] cc::optional<i32> constant_count(i32 file, ast::expr_id expr, function_scope const* scope = nullptr);
    /// A checked index's value when it is an int literal or names a `const`; none for anything else.
    [[nodiscard]] cc::optional<i32> constant_index(i32 file, ast::expr_id expr) const;
    /// The same of a `uint` index: a literal with its `u` suffix, or a `const` of `uint`.
    [[nodiscard]] cc::optional<i64> constant_unsigned_index(i32 file, ast::expr_id expr) const;
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
    /// `texture_2d[float4]` or `image_2d[.rgba8_unorm]`, and the value type `texel[.rgba8_unorm]` (CHK-390); `none`
    /// where `node` heads with none of those names.
    [[nodiscard]] type_id resolve_resource_applied(i32 file, ast::expr_id expr, ast::index const& node);
    /// The position in `k_image_formats` an image's argument `argument` names, an enum case or a const of one
    /// (CHK-200); -1 where it names none, which the caller reports, and `k_format_failed` where the const it names
    /// failed, which was reported.
    [[nodiscard]] i32 resolve_image_format(i32 file, ast::expr_id argument);
    static constexpr i32 k_format_failed = -2;
    /// `texel[F]`, the scalar or plain vector an image of format `F` loads and stores (CHK-390).
    [[nodiscard]] type_id resolve_texel(i32 file, ast::expr_id expr, ast::index const& node);
    /// `mut` or `out` in front of `inner`, which only a buffer and an image take.
    [[nodiscard]] type_id qualify_resource(i32 file, ast::expr_id expr, type_id inner, ast::type_access access);
    /// The settings of a `sampler name:` block; a setting that is wrong is reported and left at its default.
    [[nodiscard]] sampler_state compile_sampler(i32 file, ast::sampler_decl const& s);
    /// Reports a call that hands over an `@unfilterable` texture member together with a sampler member that filters.
    /// The sampler a sampling call reads against its texture: filtering (CHK-210, CHK-281), and a `@sampler` supplied
    /// where the call names none (CHK-279).
    void judge_filtering(i32 file, ast::expr_id id, source_span call, cc::span<written_argument const> arguments);
    /// CHK-280: a texel offset and a gather's component are constants, and a compare's level is the literal 0.0.
    /// CHK-378: so is a subgroup operation's lane.
    void judge_constant_arguments(i32 file, ast::expr_id id);
    /// CHK-374: false, having reported, where `candidates` are the uniform load's and its argument is no member of
    /// workgroup memory; true for every other call, which overload resolution goes on to judge.
    [[nodiscard]] bool judge_uniform_load(i32 file,
                                          cc::span<symbol_id const> candidates,
                                          ast::range_of<ast::argument> arguments);
    void judge_offset_range(i32 file, ast::expr_id expr);
    /// Indexes the symbols from `first`, the ones before it indexed when their layer was checked.
    void index_builtin_symbols(isize first);
    /// CHK-282: every barrier and every call that takes derivatives stands where all invocations of its group arrive.
    /// Gives the entry point legalized with the default options where judging it needed that, and none where not.
    [[nodiscard]] cc::optional<flat_entry_point> judge_uniformity(flat_entry_point const& structured);
    /// CHK-300, CHK-401: every index into a binding array, judged over the copies the entry points inlined of it.
    void judge_indices();
    /// CHK-402: one diagnostic for each that several entry points were judged to have alike, which names them all.
    void merge_entry_diagnostics();
    /// CHK-403: an entry point that expects a diagnostic is judged by it alone, and is never built.
    void drop_expecting_entry_points();
    /// CHK-403: so no pipeline, hit group or callables table names one.
    void judge_expecting_in_pipelines();
    /// CHK-270, CHK-311 and CHK-312: every constant of the tree folded, and what WGSL would refuse of it reported.
    void judge_constants(flat_entry_point const& structured);
    /// CHK-55: every read of a `let mut` declared without a value, on a path where nothing assigned it, is
    /// `read-before-assignment` (check_assignments.cc).
    void judge_assignments(flat_entry_point const& structured);
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
    /// What a value of `type` needs of a device: its builtin's, or what any member or element holds (CHK-382).
    /// A resource holds its element, so `buffer[half]` needs what `half` needs; a texture holds nothing.
    [[nodiscard]] feature_set features_of_type(type_id type) const;
    /// The path from `type` down to the first value of `bits` bits it holds, 16 or 64, through struct members, as
    /// `.inner.gain: half`.
    /// Empty when it holds none.
    [[nodiscard]] cc::string sized_value_path(type_id type, i32 bits) const;
    /// The path from `type` down to the first 64-bit vector of three or four components it holds, as `sized_value_path`.
    [[nodiscard]] cc::string wide_vector_path(type_id type) const;
    /// CHK-383: refuses each 16-bit and 64-bit value a struct crossing a stage edge holds, reported at `where` of `file`.
    void judge_edge_16_bit(i32 file, source_span where, type_id type, bool is_link);
    /// The first value of function bodies alone that `type` holds, such as a `mat3`, through struct members, arrays and
    /// a buffer's element; `path` is empty when it holds none (CHK-397).
    struct local_only_found
    {
        /// `: mat3` for `type` itself, `.inner.basis: mat3` for a member.
        cc::string path;
        /// Why no memory holds it yet, from its record.
        cc::string_view reason;
        type_id type = type_id::none;
    };
    /// `in_memory`: what GPU memory holds, where a builtin with a layout there is no value of function bodies alone.
    [[nodiscard]] local_only_found local_only_path(type_id type, bool in_memory = false, bool in_link = false) const;
    /// A value of a hit object that no target can keep: where it stands, and what it is.
    struct hit_object_misuse
    {
        origin where;
        cc::string detail;
    };
    /// CHK-516: the first value of `e` holding a hit object other than a trace's own.
    /// `e` has every generic and call resolved, since a copy is reached through them as much as written.
    [[nodiscard]] cc::optional<hit_object_misuse> find_hit_object_misuse(flat_entry_point const& e) const;
    /// CHK-397: refuses a value of function bodies alone a struct crossing a stage edge holds; true where it did.
    /// `is_link`: the edge is a stage link, where a mat2 and a mat3 cross a column per location (CHK-422).
    bool judge_edge_local_only(i32 file, source_span where, type_id type, bool is_link = false);
    /// Marks the first body `require` of each feature of `features` in each of `functions` as used (CHK-265).
    void mark_requires_used(cc::span<symbol_id const> functions, feature_set features);
    /// `unused-require` for every `require` of a body that nothing needed (CHK-265).
    void report_unused_requires();

    // ---- footprint pins (check_footprint.cc) ------------------------------------------------------------------------

    /// Records the `@expect(footprint = "...")` among the attributes of function `id`, and an entry point's expected
    /// diagnostics (CHK-403); any other `@expect` there is reported.
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
    type_id instance_of(type_id generic, cc::span<type_id const> arguments);
    /// `element[n]`, an array whose length is the value parameter `count_parameter` (CHK-339).
    [[nodiscard]] type_id symbolic_array_type(type_id element, type_id count_parameter);
    /// `instance_of` where it exists already, `none` otherwise.
    [[nodiscard]] type_id existing_instance(type_id generic, cc::span<type_id const> arguments) const;
    /// `type` with every parameter `bindings` binds replaced, interning what that makes.
    type_id substitute(type_id type, cc::span<type_id const> bindings);
    /// `substitute` over what exists already, which flattening reads; `none` where an instance was never made.
    [[nodiscard]] type_id substitute_existing(type_id type, cc::span<type_id const> bindings) const;
    /// Whether `actual` is `pattern` with its open parameters bound, extending `bindings` with what that takes (CHK-340).
    [[nodiscard]] bool unify(type_id pattern, type_id actual, cc::vector<type_id>& bindings);
    /// Makes every instance a generic call's inlining will name, before any entry point is flattened.
    /// Starts from the calls from `first_call` and the types from `first_type`, with the open ones of the layers below.
    /// A lower layer's own calls were instantiated when it was checked, and its bindings touch no type of this layer.
    void instantiate_generics(isize first_call, isize first_type);
    /// Whether no type `bindings` binds a parameter to is open.
    [[nodiscard]] bool is_closed(cc::span<type_id const> bindings) const;
    /// The instance of `function` for `bindings`, checked once; -1 where a binding is open or the function recursive.
    /// `file` and `where` are the call that asks, which an error in the body is noted under (CHK-338).
    i32 instantiate(symbol_id function, cc::span<type_id const> bindings, i32 file, source_span where);
    /// The instance of `function` for `bindings` the check made, or -1.
    [[nodiscard]] i32 existing_function_instance(symbol_id function, cc::span<type_id const> bindings) const;
    /// The constant `bits` of `element`, interned, which a value parameter stands for in an instance (CHK-338).
    [[nodiscard]] type_id constant_type(type_id element, u32 bits);
    /// CHK-423: `scalar_of[of]`, the scalar of a vector or a matrix type: open while `of` is, interned; the error type
    /// for a type that has none, which nothing reports here.
    [[nodiscard]] type_id scalar_of_type(type_id of);
    /// `scalar_of_type` of a closed `of`, which interns nothing.
    [[nodiscard]] type_id scalar_of_closed(type_id of) const;
    /// One of a generic function's or struct's parameters: a type parameter, or a value parameter `n: int` (CHK-338).
    [[nodiscard]] type_id declare_type_parameter(i32 file, ast::field const& p, symbol_id owner, bool& is_failed);
    /// A generic argument a call states: a type, or the constant a value expression evaluates to (CHK-340).
    [[nodiscard]] type_id stated_argument_of(function_scope& scope, ast::expr_id expr);
    /// The case of `enumeration` that `expr` names: `.case`, `E.case` or a const holding one; -1 for anything else.
    [[nodiscard]] i32 enum_case_argument(i32 file, ast::expr_id expr, type_id enumeration, function_scope const* scope);
    /// An argument of a generic struct in a type position, of the kind `parameter` takes: a type, a parameter in
    /// scope, or an int constant for a value parameter (CHK-339).
    [[nodiscard]] type_id type_argument_of(i32 file, ast::expr_id expr, function_scope const* scope, type_id parameter);
    /// The warning for each generic function of the program that nothing instantiated (CHK-338).
    void warn_uninstantiated_generics(isize first);
    /// The instance whose body is being checked, whose tables `set_type` and `set_target` write; -1 for none.
    i32 current_instance = -1;
    cc::vector<instantiation> instantiating;
    /// Above 0 while a generic body is checked with its type parameters open, where only its names are judged.
    i32 generic_passes = 0;
    /// Every error reported, and every one an instance repeated and was not reported again.
    isize error_events = 0;
    /// What `report` hands back for a diagnostic it drops.
    located_diagnostic dropped;
    /// The tables of `file` as the body being checked reads them.
    [[nodiscard]] table_view tables_of(i32 file) const;
    /// The entries of the current instance's tables for `expr`, grown to hold it.
    [[nodiscard]] isize own_slot(ast::expr_id expr);
    /// The type parameters in scope, innermost last: a generic function's while its signature and body are checked,
    /// and a generic struct's while its members are.
    cc::vector<cc::pair<cc::string_view, type_id>> type_parameter_names;
    /// True where `name` is a type parameter in scope, which hides every symbol of its name (CHK-338).
    [[nodiscard]] bool is_type_parameter_name(cc::string_view name) const
    {
        for (auto const& n : type_parameter_names)
            if (n.first == name)
                return true;
        return false;
    }
    /// What a call stands where a type is expected, which a generic callee's result is deduced from where its
    /// arguments leave a parameter unbound (CHK-340); `none` elsewhere.
    type_id expected_result = type_id::none;
    /// What `check_expected` expects of the `if` or `case` value it is about to check (CHK-166); `none` elsewhere.
    /// The `check_expr` of that value takes it and clears it, so no expression inside sees it.
    type_id expected_value = type_id::none;

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
    /// The symbols a ray set's name finds: `set` as `file` sees it, or `m.set` of a module `file` uses (CHK-329).
    [[nodiscard]] cc::vector<symbol_id> const* ray_set_candidates(i32 file, cc::string_view text) const;
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
    /// `instance` -1 checks the body as declared, and an instance's index checks it with that instance's bindings.
    void check_body_of(symbol_id id, i32 instance = -1);
    /// The defaults of function `id`'s parameters, each in the function's own scope with the parameters before it
    /// visible, and of its parameter's type (CHK-243).
    /// Checked once, after every signature is known, since a default may call what is declared below it.
    void check_defaults(symbol_id id);

    // ---- tests (check_test.cc) --------------------------------------------------------------------------------------

    /// Records the `test` `decl` stands for, with the names of `enclosing` it can see and must not use.
    void add_test(i32 file, ast::decl_id decl, cc::string scope_path, function_scope const* enclosing = nullptr);
    /// Every `test` of the files from `first_file` that no other path found, checked as one at file scope with no names
    /// around it; the tests from `first_test` are the only ones those files can hold.
    void add_unregistered_tests(i32 first_file, isize first_test);
    /// Every `test` a struct or an enum declares among its members.
    void add_member_tests(i32 file, ast::range_of<ast::decl_id> members, cc::string_view scope_path);
    /// Checks test `index` as a function of no parameter and no binding, and its last-line rule (CHK-226).
    void check_test(i32 index);
    /// The flat tree of test `index`, when it checked soundly and all it reaches inlines whole (flatten.cc).
    void flatten_test(i32 index);
    /// What a computed const's run gave: its scalars, or the diagnostic that says why it has none.
    struct const_evaluation
    {
        cc::vector<scalar> leaves;
        bool is_ok = false;
        diagnostic_kind kind = diagnostic_kind::unsupported_yet;
        cc::string detail;
    };
    /// CHK-219: the value of a const, whose expression `expr` of `file` was checked as type `type` in the scope of
    /// `scope`, run once on the abstract machine; nothing is reported (flatten.cc).
    [[nodiscard]] const_evaluation evaluate_const(symbol_id scope, i32 file, ast::expr_id expr, type_id type);
    /// Every module-level name as `out.symbols` now holds them, the last of `out.module_names`, made anew only when a
    /// symbol joined since `module_names_for`.
    /// The lower layer's set, shared, and this layer's own, each a flat tree's mint takes.
    struct taken_names
    {
        cc::set<cc::string> const* below = nullptr;
        cc::set<cc::string> const* own = nullptr;
    };
    [[nodiscard]] taken_names module_names();
    /// The emitted names of the first `count` symbols.
    [[nodiscard]] cc::shared_ptr<cc::set<cc::string>> names_up_to(isize count) const;
    isize module_names_for = -1;
    cc::set<cc::string> const* own_module_names = nullptr;
    /// The prelude's every module-level name, made once with it and shared by every program's check behind it, and how
    /// many symbols it covers.
    cc::shared_ptr<cc::set<cc::string>> module_names_below;
    isize module_names_below_count = 0;
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
    /// `img[xy]`, a texel of `image`: the `load` it reads as, or the place an assignment stores to (CHK-367); or the
    /// atomic of an `@atomic` image's texel (CHK-373).
    [[nodiscard]] type_id check_texel(function_scope& scope, ast::expr_id id, ast::index const& node, type_id image);
    /// `v[i]`, one lane of the vector `vector`, of type `lane` (CHK-395).
    [[nodiscard]] type_id check_lane(function_scope& scope, ast::index const& node, type_id vector, type_id lane);
    /// The `store` of an assignment to the texel `check_texel` left in `texel_place`, with `value` of `type` stored.
    void check_texel_store(function_scope& scope, ast::expr_id value, type_id type);
    /// `x as T`, which is the operator function of `as` that takes `x` and gives `T`; the error type where none does.
    [[nodiscard]] type_id check_cast(function_scope& scope, ast::expr_id id, ast::cast const& node);
    /// CHK-411: an `as` whose type is an arithmetic run, reported once over the whole run with the fix that converts
    /// each operand; false where the cast is no such run.
    [[nodiscard]] bool report_as_precedence(i32 file, ast::expr_id id);
    /// The `as` overload visible from `file` that converts `from` to `to`, or `none` where there is no such conversion
    /// (CHK-197).
    [[nodiscard]] symbol_id conversion_of(i32 file, source_span where, type_id from, type_id to);
    /// True where the type `type` of a cast is written `texel[F]`, which alone may change a width (CHK-392).
    [[nodiscard]] bool is_texel_type_expr(i32 file, ast::expr_id type) const;
    /// `x as texel[F]` of another type than the texel, `written` being its `texel[F]` (CHK-391, CHK-392).
    [[nodiscard]] type_id check_texel_conversion(i32 file, ast::expr_id id, type_id from, type_id to, ast::expr_id written);

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
    /// What a `for` runs over: a range, then each `.reversed` and `.by(step)` written after it, innermost first (CHK-415).
    struct range_walk
    {
        ast::range const* range = nullptr;
        /// An argument of `.by`, or `none` for a `.reversed`.
        cc::vector<ast::expr_id> steps;
    };
    /// `iterable` as a range and the walk around it; no range where it is none, or a walk names anything else.
    [[nodiscard]] range_walk range_walk_of(i32 file, ast::expr_id iterable) const;
    void check_return(function_scope& scope, source_span where, ast::expr_id value);
    void check_break(function_scope& scope, source_span where, ast::expr_id value);
    void check_condition(function_scope& scope, ast::expr_id condition);
    /// Declares a local, which shadows every earlier local, parameter and module-level symbol of its name (CHK-53, CHK-54).
    void declare_local(function_scope& scope, local_name local);
    /// The symbol of the `const` that `decl` of a function body declares, minted once however often the body is
    /// checked.
    [[nodiscard]] symbol_id local_constant_of(function_scope const& scope, ast::decl_id decl);
    /// A numeric scalar or a plain vector of `float`, `half`, `int` or `uint`, which a color target holds (CHK-418).
    [[nodiscard]] bool is_color_value(type_id type) const;
    /// CHK-424: a scalar, a plain vector of one, or an enum, which a message interpolates.
    [[nodiscard]] bool is_printable(type_id type) const;
    /// CHK-424: the message of a `print` or an `assert`, a quoted literal whose interpolations each check printable.
    /// A `print`, which `takes_value`, may print one value of any type instead.
    void check_message(function_scope& scope, ast::expr_id message, bool takes_value);
    /// The `@pixel struct` a pixel entry point returning one `color` stands in for, minted under a name nothing has.
    [[nodiscard]] type_id pixel_targets_of(symbol_id entry, type_id color);
    /// A `loop:`; the result is the type its breaks carry, and `nothing` for one that is a statement.
    /// `has_break` is false for a loop nothing leaves, which never ends.
    [[nodiscard]] type_id check_loop(function_scope& scope,
                                     ast::expr_id id,
                                     ast::loop_expr const& loop,
                                     bool yields_value,
                                     bool& has_break);
    /// The body of a `case` arm or of a branch of an `if` value: what it gives in `arm_type`, or that it leaves.
    /// A value is checked against `expected` where that is valid, and one that does not convert gives the error type.
    void check_arm_body(function_scope& scope,
                        ast::body const& body,
                        bool yields_value,
                        type_id expected,
                        type_id& arm_type,
                        bool& arm_exits,
                        bool& is_failed);
    /// `if c => a else b`, read as a value where `yields_value`, and as an `if` statement otherwise (CHK-375).
    /// `expected` is the type the context expects of the value, `none` where it expects none (CHK-166).
    [[nodiscard]] type_id check_if_value(function_scope& scope,
                                         ast::if_expr const& node,
                                         bool yields_value,
                                         flow* ending = nullptr,
                                         type_id expected = type_id::none);
    /// A `case`; the result is the type of its arms, and `nothing` for one that is a statement.
    /// `ending`, where given, is how the `case` ends as a statement: it exits when it is exhaustive and every arm exits.
    /// `expected` is the type the context expects of the value, `none` where it expects none (CHK-166).
    [[nodiscard]] type_id check_case(function_scope& scope,
                                     ast::expr_id id,
                                     ast::case_expr const& node,
                                     bool yields_value,
                                     flow* ending = nullptr,
                                     type_id expected = type_id::none);
    /// One arm's pattern, against the scrutinee's type; appends the enum cases it names, and says whether all were cases.
    void check_pattern(function_scope& scope,
                       ast::expr_id pattern,
                       type_id scrutinee,
                       cc::vector<i32>& named_cases,
                       bool& is_all_constant);
    void check_yield(function_scope& scope, source_span where, ast::expr_id value);
    /// Reports every loop of calls once, and marks the functions on it.
    /// Walks the calls of the functions from symbol `first`, note `first_note` and call `first_call`.
    /// A lower layer's functions call only their own layer's, and were walked when it was checked.
    void find_recursion(isize first, isize first_note, isize first_call);
    /// True when `function` and every function it calls checked soundly and none is recursive, so all of it inlines.
    [[nodiscard]] bool inlines_whole(symbol_id function);

    [[nodiscard]] type_id check_expr(function_scope& scope, ast::expr_id expr);
    [[nodiscard]] type_id check_literal(function_scope& scope, ast::expr_id id, ast::literal const& literal);
    /// `text` must be a `suffixed` number.
    [[nodiscard]] type_id check_suffixed_literal(i32 file, source_span where, cc::string_view text);
    [[nodiscard]] type_id check_name(function_scope& scope, ast::expr_id id, ast::name const& name);
    /// A module-level name as a value, where `found` is what the name, bare or qualified, resolved to.
    [[nodiscard]] type_id check_symbol_value(function_scope& scope,
                                             ast::expr_id id,
                                             cc::string_view text,
                                             cc::vector<symbol_id> const* found);
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
    /// CHK-421: `arguments` with each scalar of another kind converted to the kind of the plain vector `candidates`
    /// construct, where that makes one of them match; nothing for any other call.
    [[nodiscard]] cc::optional<call_arguments> converted_for_plain_vector(i32 file,
                                                                          source_span where,
                                                                          cc::span<symbol_id const> candidates,
                                                                          call_arguments const& arguments);
    /// The length of the chain that converts written argument `i` to `parameter`; nothing where it does not convert.
    [[nodiscard]] cc::optional<i32> chain_of(i32 file, parameter const& taking, call_arguments const& arguments, isize i);
    /// Whether `expr`, already checked, names a place a body could assign, reporting why not; `what` names the use.
    /// With `quiet_why`, nothing is reported, and why not is written there instead.
    bool judge_place(function_scope& scope, ast::expr_id expr, cc::string_view what, cc::string* quiet_why = nullptr);
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
    /// What a number literal `expr` holds, or an operator over number literals alone folds to (CHK-394); `is_number`
    /// is false for any other expression.
    [[nodiscard]] number_literal number_of(i32 file, ast::expr_id expr) const;
    /// Whether the operator call `expr` resolved to a builtin of the prelude, which alone CHK-394 folds.
    [[nodiscard]] bool calls_prelude_operator(i32 file, ast::expr_id expr) const;
    /// What a comparison of number literals alone folds to (CHK-394); none for any other expression.
    [[nodiscard]] cc::optional<bool> comparison_of(i32 file, ast::expr_id expr) const;
    /// What type parameter `name` stands for, innermost first; `none` where no type parameter has the name.
    [[nodiscard]] type_id type_parameter_stands_for(cc::string_view name) const;
    /// Whether `expr`, as an operand, names a type: a type parameter, a struct or an enum, `scalar_of[T]`.
    /// Reports nothing; a local of the name hides every type of it.
    [[nodiscard]] bool is_type_operand(function_scope const& scope, ast::expr_id expr) const;
    /// CHK-429: `A == B` or `A != B` over two types, a `bool` an instance knows; none where neither operand is a type.
    [[nodiscard]] cc::optional<type_id> check_type_comparison(function_scope& scope,
                                                              ast::expr_id id,
                                                              ast::call const& call);
    /// What checked condition `expr` is whatever the target, the device and the options are (CHK-430).
    /// None where it reads any of them, or anything else this compile alone knows.
    [[nodiscard]] cc::optional<bool> known_condition(i32 file, ast::expr_id expr) const;
    /// A number `known_condition` compares: a literal, a value parameter, an enum's case, a const no option reaches.
    [[nodiscard]] cc::optional<f64> known_number_of(i32 file, ast::expr_id expr) const;
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
    /// What a call that matched none of `candidates` was one step from, as a clause its detail ends in; empty for none.
    /// A candidate that a `mut` mark alone keeps from matching names the mark (CHK-316).
    /// One whose first parameter is a condition, and that the arguments would match with the last one moved to the
    /// front, says that the condition comes first, as WGSL's `select` does not have it (CHK-365).
    /// A missing mark is named only where its argument is a place, and several marks or a dot call show the whole call.
    [[nodiscard]] cc::string miss_hint(function_scope& scope,
                                       ast::expr_id callee,
                                       cc::span<symbol_id const> candidates,
                                       call_arguments const& arguments,
                                       cc::string_view spelling);
    /// Remembers how call `id` fills the parameters of `callee`, which is what the flat tree is written from.
    void record_call(i32 file,
                     ast::expr_id id,
                     symbol_id callee,
                     call_arguments const& arguments,
                     cc::span<i32 const> slots,
                     cc::span<type_id const> bindings = {});
    /// The candidates of `spelling` seen from `file` that take exactly `types`, without a report.
    /// It is what the flat tree is written from.
    [[nodiscard]] symbol_id find_operator(i32 file, cc::string_view spelling, cc::span<type_id const> types) const;

    /// PROTOTYPE (autodiff): a value of `type` has a derivative: a float, a half, or what holds one.
    [[nodiscard]] bool is_differentiable_type(type_id type) const;
    /// Parameter `parameter` of `function` takes a tangent: it is of a differentiable type, no `mut`, and no `@no_diff`.
    [[nodiscard]] bool takes_tangent(symbol_id function, isize parameter) const;
    /// The type of a change of a value of `type`: a position's is its direction, and any other type is its own.
    [[nodiscard]] type_id tangent_type(type_id type) const;
    /// For a call of the prelude's `fwd_diff` or `bwd_diff`: one overload per function the first argument names, each
    /// taking that function's parameters and their tangents; empty where the first argument names no function.
    [[nodiscard]] cc::vector<symbol_id> derivative_candidates(cc::span<symbol_id const> generic,
                                                              call_arguments const& arguments,
                                                              i32 file,
                                                              source_span where);
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
    /// A call of the module-level name `text`, bare or qualified, where `found` is what it resolved to (CHK-247).
    /// A qualified call's functions are `found` alone, where a bare one's are all its file sees.
    [[nodiscard]] type_id check_named_call(function_scope& scope,
                                           ast::expr_id id,
                                           ast::call const& call,
                                           cc::string_view text,
                                           cc::vector<symbol_id> const* found,
                                           bool is_qualified,
                                           cc::span<stated_argument const> stated = {});
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
    /// What a metal traversal function fuses into one intersection entry point: the any hit a record runs after it,
    /// and the payload that any hit writes (CHK-345).
    struct traversal_request
    {
        cc::string name;
        symbol_id any_hit = symbol_id::none;
        type_id payload = type_id::none;
    };
    void flatten_entry_point(symbol_id id, traversal_request const* traversal = nullptr);
    /// CHK-345: every procedural hit group's traversal function per ray type, and the empty closest hit, which metal's
    /// tables hold where DXR's run an intersection and an any hit apart, or nothing.
    void flatten_metal_traversals();
};
} // namespace sgl::check::impl
