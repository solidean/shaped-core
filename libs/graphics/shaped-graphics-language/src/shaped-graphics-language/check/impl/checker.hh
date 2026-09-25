#pragma once

#include <clean-core/container/fixed_array.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/check/check.hh>

namespace sgl::check::impl
{
/// How a number literal reads before any literal types exist.
enum class number_class : u8
{
    /// A decimal literal with a DOT or an exponent and no suffix.
    plain_float,
    /// Decimal digits and nothing else.
    plain_integer,
    /// A prefix, a suffix or a `p` exponent: spellings whose meaning needs literal types.
    other,
};

[[nodiscard]] number_class classify_number(cc::string_view text);

/// `text` must be a `plain_float`; nullopt when the value does not fit an f64.
[[nodiscard]] cc::optional<f64> parse_plain_float(cc::string_view text);

/// `text` must be a `plain_integer`; nullopt when the value does not fit an i32.
[[nodiscard]] cc::optional<i32> parse_plain_integer(cc::string_view text);

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

    /// The newest visible local or parameter of that name, which hides every module-level symbol of it.
    [[nodiscard]] local_name const* find_local(cc::string_view name) const
    {
        for (auto i = locals.size() - 1; i >= 0; --i)
            if (locals[i].name == name)
                return &locals[i];
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
    /// Stands on a loop of calls, which was reported.
    bool is_recursive = false;
    /// 0 before anybody asked, 1 for a function that inlines whole, 2 for one that does not.
    /// It inlines whole when its body is sound, it is not recursive, and the same holds for every function it calls.
    u8 inlines_whole = 0;
};

/// The argument types of one call after its splats were spread.
struct call_arguments
{
    cc::vector<type_id> types;
    /// An argument had the error type or was reported, so the call reports nothing about its arguments.
    bool is_poisoned = false;
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
    /// The symbols in compilation, outermost first, which is the loop a dependency cycle names.
    cc::vector<symbol_id> compiling;
    cc::vector<function_notes> notes;
    /// Every call of a function that is no `@builtin`, in the order the bodies were checked.
    cc::vector<call_edge> calls;
    /// The object `check_index` is checking right now: the one place a buffer may stand as an expression.
    ast::expr_id subscripted = ast::expr_id::none;
    /// The arguments of the call being checked, which a texture, an image or a sampler may stand as (CHK-206).
    cc::vector<ast::expr_id> handed;

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

    void report(diagnostic_kind kind, i32 file, source_span where, cc::string detail);
    void unsupported(i32 file, source_span where, cc::string_view construct);
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

    /// Compiles the symbol when nobody has, and reports a cycle when somebody is.
    /// The state it returns is `checked` or `failed`, or `in_compilation` for a cycle, which was reported at `where`.
    symbol_state demand(symbol_id id, i32 file, source_span where);
    void compile(symbol_id id);
    void compile_struct(symbol_id id);
    void compile_enum(symbol_id id);
    /// A `const`: its value is a number literal, an enum case or another `const`, and anything else is `unsupported-yet`.
    void compile_const(symbol_id id);
    /// False where `attributes` hold `@shadowable(false)`; a malformed one is reported when its declaration is compiled.
    [[nodiscard]] bool is_shadowable_by(i32 file, ast::range_of<ast::attribute> attributes) const;
    /// Reports `shadows-unshadowable` where a local or a parameter named `name` would hide a `@shadowable(false)` symbol.
    void judge_shadowing(i32 file, cc::string_view name, source_span where);
    void compile_binding(symbol_id id);
    void compile_function(symbol_id id);
    void judge_entry_point(symbol_id id);

    /// True for the prelude's `int3`, the type a dispatch reports a thread's id as.
    [[nodiscard]] bool is_int3(type_id type) const;
    /// The stages a `@stages` attribute names, as `function_info::stages`; every stage without one or after a bad one.
    [[nodiscard]] u8 stages_of(i32 file, ast::attribute const* a);
    /// The grid of a `@compute` attribute; `{1, 1, 1}` without one, and after a bad argument it reports.
    [[nodiscard]] cc::fixed_array<i32, 3> workgroup_of(i32 file, ast::attribute const* a);
    /// The name of a `@stream(name)`; empty without one, and after a bad argument it reports.
    [[nodiscard]] cc::string stream_of(i32 file, ast::attribute const* a);
    /// The members of a struct or a binding, collected locally and appended whole so the range stays contiguous.
    /// A `@pixel struct`'s members are targets, so their attributes may be a target's settings.
    [[nodiscard]] ast::range_of<member_info> compile_members(i32 file,
                                                             ast::range_of<ast::decl_id> members,
                                                             bool is_struct,
                                                             bool is_target_struct = false);
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
    /// A texture, image or sampler type, interned like `buffer_type`; `info` needs no `spelled`.
    [[nodiscard]] type_id resource_type(type_info info);
    /// The resource type a bare name in a type position names — a depth texture or a sampler — and `none` otherwise.
    [[nodiscard]] type_id resolve_resource_name(i32 file, ast::expr_id expr, cc::string_view text);
    /// `texture2d[float4]` or `image2d[.rgba8_unorm]`; `none` where `node` heads with no texture or image name.
    [[nodiscard]] type_id resolve_resource_applied(i32 file, ast::expr_id expr, ast::index const& node);
    /// `mut` or `out` in front of `inner`, which only a buffer and an image take.
    [[nodiscard]] type_id qualify_resource(i32 file, ast::expr_id expr, type_id inner, ast::type_access access);
    /// The settings of a `sampler name:` block; a setting that is wrong is reported and left at its default.
    [[nodiscard]] sampler_state compile_sampler(i32 file, ast::sampler_decl const& s);
    /// Reports a call that hands over an `@unfilterable` texture member together with a sampler member that filters.
    void judge_filtering(i32 file, source_span call, ast::range_of<ast::argument> arguments);
    /// A builtin's parameter type, where an image names the texel it reads or writes: `out image2d[float4]`.
    [[nodiscard]] type_id resolve_pattern_type(i32 file, ast::expr_id expr);
    /// True where an argument of type `argument` may stand for a parameter of type `parameter` (CHK-70, CHK-207).
    [[nodiscard]] bool takes(type_id parameter, type_id argument) const;
    /// Reports `form` as `needs-feature`, naming the feature that would grant it.
    void judge_feature(i32 file, source_span where, cc::string_view form, cc::string_view feature);
    /// True where `expr` is the bare name `name`, which is how a resource type is recognized before lookup.
    [[nodiscard]] bool is_named(i32 file, ast::expr_id expr, cc::string_view name) const;

    // ---- pipelines (check_pipeline.cc) ------------------------------------------------------------------------------

    void compile_pipeline(symbol_id id);
    /// The prelude's `raster_pipeline_description`, compiled on first use; the error type where the prelude has none.
    [[nodiscard]] type_id pipeline_description_type();
    /// True when `name` alone is one field of the description, or of one target's part when `is_on_target`.
    /// A stage name never is: it marks an entry point.
    [[nodiscard]] bool is_setting_attribute(cc::string_view name, bool is_on_target);
    /// Every field an attribute of that name could set, as full paths; more than one means it is ambiguous.
    [[nodiscard]] cc::vector<cc::string> setting_attribute_paths(cc::string_view name, bool is_on_target);

    // ---- bodies and expressions (check_expr.cc) ---------------------------------------------------------------------

    void check_body(symbol_id id);
    void convert_object(function_scope& scope, ast::expr_id object, type_id to);
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
                                             cc::span<type_id const> types);
    /// True for a function whose inferred result is being compiled right now and whose parameters do not take `types`.
    /// Its parameters are known by then, so a call that could never choose it does not need its result.
    /// That keeps an overload set usable from inside one of its own inferred members, where demanding it would be a cycle.
    [[nodiscard]] bool is_out_of_the_running(symbol_id candidate, cc::span<type_id const> types) const;
    /// The candidates of `spelling` that take exactly `types`, without a report; what the flat tree is written from.
    [[nodiscard]] symbol_id find_operator(cc::string_view spelling, cc::span<type_id const> types) const;
    [[nodiscard]] call_arguments check_arguments(function_scope& scope,
                                                 ast::range_of<ast::argument> range,
                                                 bool is_constructor);
    [[nodiscard]] type_id construct(function_scope& scope,
                                    ast::expr_id id,
                                    ast::expr_id callee,
                                    symbol_id structure,
                                    call_arguments const& arguments);
    [[nodiscard]] type_id resolve_overload(function_scope& scope,
                                           ast::expr_id id,
                                           ast::expr_id callee,
                                           cc::span<symbol_id const> candidates,
                                           call_arguments const& arguments,
                                           cc::string_view spelling);
    [[nodiscard]] cc::string signature_text(cc::string_view spelling, cc::span<type_id const> types) const;
    /// Records a call of `callee`, a function of the program, as an edge of the call graph, and reports each binding it
    /// reads that the caller does not list.
    void note_program_call(function_scope const& scope, symbol_id callee, source_span where);

    // ---- the flat tree (flatten.cc) ---------------------------------------------------------------------------------

    /// True when every member of the type, at any depth, has a type.
    [[nodiscard]] bool is_sound(type_id type) const;
    void flatten_entry_point(symbol_id id);
};
} // namespace sgl::check::impl
