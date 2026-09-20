#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/ast/file_ast.hh>
#include <shaped-graphics-language/syntax/parsed_file.hh>

namespace sgl::ast::impl
{
/// What the expression builder does with attributes written on the form it is handed.
enum class attribute_mode : u8
{
    /// An arbitrary sub-expression: kept on the node and reported.
    reject,
    /// A type position: kept on the node.
    keep,
    /// The statement, declaration or list element around it has taken them.
    taken,
};

/// Whose lines are being read, which decides what a line may be.
enum class scope_kind : u8
{
    file,
    function,
    struct_body,
    enum_body,
    binding_body,
};

/// Which of the word-less operator levels an `op` leaf belongs to.
enum class operator_level : u8
{
    assignment,
    computes_as,
    connective,
    comparison,
    ascription,
    arrow,
    range,
    arithmetic,
};

/// What a `yield` or a `return` found around it, innermost last.
enum class body_owner : u8
{
    /// A `fun`, named or anonymous: `return` leaves it, and `yield` has nothing to hand a value to.
    function,
    /// `x => …`: `yield` hands its value on, and `return` has no `fun` to leave.
    arrow_lambda,
    /// A `case` arm or a property: `yield` hands its value on, and `return` looks through it.
    value_block,
};

/// What stands between `fun` and the body.
struct fun_signature
{
    /// The identifier, `none` when nothing or a list stands where the name belongs.
    form_id name_form = form_id::none;
    /// Where a missing name is reported: the first list, or `none` for a `fun` that has no signature at all.
    form_id first_list = form_id::none;
    range_of<field> type_parameters;
    range_of<field> parameters;
    range_of<argument> bindings;
    expr_id return_type = expr_id::none;
    bool has_parameter_list = false;
};

/// A keyword form taken apart.
struct keyword_parts
{
    cc::vector<form_id> keywords;
    cc::vector<form_id> arguments;
    form_id block = form_id::none;
};

/// An operator run taken apart; `operators` is one shorter than `operands`.
struct run_parts
{
    cc::vector<form_id> operands;
    cc::vector<form_id> operators;
};

/// A statement form seen from its keyword form.
/// `=>` and assignment are looser than a keyword form, so `if done => total = 0` reaches the builder as
/// `((if done) => total) = 0` and is put back together here.
struct statement_head
{
    form_id whole = form_id::none;
    /// `none` when no keyword leads the statement.
    form_id keyword_form = form_id::none;
    form_id arrow_operator = form_id::none;
    form_id arrow = form_id::none;
    form_id assign_operator = form_id::none;
    form_id assign_value = form_id::none;
};

/// The one pass from forms to AST; every member function only appends to `ast`.
/// A child list is collected locally and appended whole, which is what keeps it contiguous under recursion.
struct builder
{
    parsed_file const& file;
    file_ast ast;
    /// The bodies being read, which is all a jump needs to know where it goes.
    cc::vector<body_owner> owners;

    // ---- forms (build.cc) ------------------------------------------------------------------------------------

    [[nodiscard]] form const& at(form_id id) const { return file.at(id); }
    [[nodiscard]] cc::string_view text_of(form_id id) const { return file.text_of(file.at(id).where); }
    /// The spelling of a leaf that names one token: a keyword, an operator, an identifier.
    [[nodiscard]] cc::string_view token_text_of(form_id id) const;
    [[nodiscard]] bool is_kind(form_id id, form_kind kind) const { return is_valid(id) && at(id).kind == kind; }
    [[nodiscard]] keyword_parts keyword_parts_of(form_id keyword_form) const;
    [[nodiscard]] run_parts run_parts_of(form_id run) const;
    [[nodiscard]] operator_level level_of(form_id op) const;
    /// True for a keyword form that has at least one keyword; one without is an expression that owns a block.
    [[nodiscard]] bool is_keyword_led(form_id id) const;
    [[nodiscard]] bool is_keyword_led(form_id id, cc::string_view keyword) const;
    /// True for a two-operand run whose operator is spelled exactly `spelling`.
    [[nodiscard]] bool is_binary_run(form_id id, cc::string_view spelling) const;
    [[nodiscard]] statement_head head_of(form_id statement) const;
    /// The statement forms of a block, with every `a; b` sequence spread out.
    [[nodiscard]] cc::vector<form_id> lines_of(form_id block);

    void report(diagnostic_kind kind, source_span where);
    /// A statement that a keyword heads is reported at that keyword, since its own span runs to the end of its block.
    void report(diagnostic_kind kind, form_id where);

    // ---- parts (build.cc) ------------------------------------------------------------------------------------

    template <class T>
    [[nodiscard]] static range_of<T> append(cc::vector<T>& all, cc::span<T const> items)
    {
        auto const result = range_of<T>{.first = u32(all.size()), .count = u32(items.size())};
        all.push_back_range(items);
        return result;
    }

    template <class T>
    [[nodiscard]] static range_of<T> append_one(cc::vector<T>& all, T const& item)
    {
        all.push_back(item);
        return {.first = u32(all.size() - 1), .count = 1};
    }

    [[nodiscard]] range_of<attribute> attributes_of(form_id id);
    void reject_attributes(form_id id);

    [[nodiscard]] argument list_element(form_id element, bool is_object, bool allows_attributes);
    [[nodiscard]] range_of<argument> list_elements(form_id list, bool is_object = false, bool allows_attributes = false);

    /// True for `name`, `_`, `mut name`, and each of them followed by `: type` and then `= default`.
    [[nodiscard]] bool is_field_like(form_id element, bool needs_type) const;
    /// Reports `on_failure` and yields a nameless field around an `invalid` type when `element` is not field-like.
    [[nodiscard]] field make_field(form_id element, diagnostic_kind on_failure);
    [[nodiscard]] range_of<field> fields_of(form_id list, diagnostic_kind on_failure);

    // ---- expressions (build_expr.cc) -------------------------------------------------------------------------

    template <class Node>
    expr_id make_expr(form_id form, Node node)
    {
        ast.exprs.push_back({.form = form, .attributes = {}, .node = cc::move(node)});
        return expr_id(i32(ast.exprs.size() - 1));
    }
    expr_id invalid_expression(form_id form) { return make_expr(form, invalid_expr{}); }
    expr_id invalid_expression(form_id form, diagnostic_kind kind)
    {
        report(kind, form);
        return invalid_expression(form);
    }

    expr_id expression(form_id form, attribute_mode mode = attribute_mode::reject);
    expr_id type_expression(form_id form) { return expression(form, attribute_mode::keep); }

    expr_id expression_node(form_id form);
    expr_id round_list_expression(form_id form);
    expr_id curly_list_expression(form_id form);
    expr_id call_expression(form_id form);
    expr_id application_expression(form_id form);
    expr_id prefix_expression(form_id form);
    expr_id run_expression(form_id form);
    expr_id keyword_expression(form_id form);
    /// Reads the keywords from `first_keyword` on, which is how the value of `return case x:` is reached.
    expr_id keyword_expression_from(form_id form, keyword_parts const& parts, isize first_keyword);
    expr_id lambda_expression(form_id form, run_parts const& parts);
    expr_id arrow_lambda_expression(form_id form, form_id left, form_id right);
    /// `fun` without a name; `right_of_arrow` is `none` when the body is the keyword form's block.
    expr_id fun_lambda_expression(form_id form, form_id keyword_form, keyword_parts const& parts, form_id right_of_arrow);
    expr_id case_expression(form_id form, keyword_parts const& parts);
    expr_id jump_expression(form_id form, keyword_parts const& parts, cc::string_view keyword);
    /// The node of `return`, `break` or `yield`, after saying whether the jump has somewhere to go.
    expr_id make_jump(form_id form, cc::string_view keyword, expr_id value);
    [[nodiscard]] static bool is_value_jump(cc::string_view keyword)
    {
        return keyword == "return" || keyword == "break" || keyword == "yield";
    }

    expr_id infix_call(form_id run, expr_id left, form_id op, expr_id right, bool is_short_circuit);
    /// `left -> right`, wherever it is not the return arrow of a signature.
    expr_id function_type_expression(form_id run, form_id left, form_id right);
    /// Reads `operands` joined by `: as in` left to right.
    /// `first_mode` is how the first operand is read: `keep` when the whole run stands in a type position.
    expr_id ascription_fold(form_id run,
                            cc::span<form_id const> operands,
                            cc::span<form_id const> operators,
                            attribute_mode first_mode);
    /// The type written after the first operator of an ascription run.
    expr_id type_after_first_operator(form_id run, run_parts const& parts);
    [[nodiscard]] bool has_comma_after(form_id list, form_id element) const;

    // ---- statements (build_stmt.cc) --------------------------------------------------------------------------

    template <class Node>
    stmt_id make_stmt(form_id form, range_of<attribute> attributes, Node node)
    {
        ast.stmts.push_back({.form = form, .attributes = attributes, .node = cc::move(node)});
        return stmt_id(i32(ast.stmts.size() - 1));
    }

    [[nodiscard]] range_of<stmt_id> statements(form_id block);
    stmt_id statement(statement_head const& head);
    /// `takes_attributes` is false for the assignment right of `if … =>`, whose form is the whole `if`.
    stmt_id assignment(form_id whole, form_id target, form_id op, form_id value, bool takes_attributes);
    stmt_id let_statement(statement_head const& head, keyword_parts const& parts);
    stmt_id for_statement(statement_head const& head, keyword_parts const& parts);
    stmt_id while_statement(statement_head const& head, keyword_parts const& parts);
    stmt_id assert_statement(statement_head const& head, keyword_parts const& parts);
    stmt_id print_statement(statement_head const& head, keyword_parts const& parts);
    stmt_id expression_statement(form_id form);
    /// `chain` is an `if` or a stray `else`, then the `else` forms that follow it directly.
    stmt_id if_chain(cc::span<form_id const> chain);

    [[nodiscard]] body block_body(form_id block);
    /// The body of something that yields a value: a block, or the expression right of `=>`.
    /// `owner` is what the jumps inside it find around them.
    [[nodiscard]] body value_body(form_id right_of_arrow, body_owner owner);
    /// The body of a control statement: its block, or the one statement right of `=>`.
    [[nodiscard]] body statement_body(statement_head const& head, keyword_parts const& parts);

    // ---- declarations (build_decl.cc) ------------------------------------------------------------------------

    template <class Node>
    decl_id make_decl(form_id form, range_of<attribute> attributes, Node node)
    {
        ast.decls.push_back({.form = form, .attributes = attributes, .node = cc::move(node)});
        return decl_id(i32(ast.decls.size() - 1));
    }
    decl_id invalid_declaration(form_id form, diagnostic_kind kind)
    {
        report(kind, form);
        return make_decl(form, attributes_of(form), invalid_decl{});
    }

    /// True for the keywords that head a declaration, `let` excluded: that one is a statement.
    [[nodiscard]] static bool is_declaration_keyword(cc::string_view keyword);

    [[nodiscard]] range_of<decl_id> declarations(form_id block, scope_kind scope);
    decl_id declaration(statement_head const& head, scope_kind scope, bool is_first_in_file);
    decl_id member_declaration(form_id line, scope_kind owner);

    decl_id module_declaration(statement_head const& head, keyword_parts const& parts);
    decl_id use_declaration(statement_head const& head, keyword_parts const& parts);
    decl_id fun_declaration(statement_head const& head, keyword_parts const& parts);
    /// True when no identifier stands where the name of a `fun` belongs, which in expression position is a lambda.
    [[nodiscard]] bool is_anonymous_fun(keyword_parts const& parts) const;
    /// Reads the lists and the return type, and leaves a missing name or parameter list to the caller to report.
    [[nodiscard]] fun_signature signature_of(keyword_parts const& parts);
    decl_id type_body_declaration(statement_head const& head, keyword_parts const& parts, scope_kind body);
    decl_id type_declaration(statement_head const& head, keyword_parts const& parts);
    decl_id const_declaration(statement_head const& head, keyword_parts const& parts);
    decl_id sampler_declaration(statement_head const& head, keyword_parts const& parts);
    decl_id notation_declaration(statement_head const& head, keyword_parts const& parts);

    /// The single identifier a declaration is named by; reports and yields an empty span for anything else.
    [[nodiscard]] source_span declared_name(form_id keyword_form, keyword_parts const& parts);
    /// Reports whatever of `=> …` and `= …` the head carries, for a declaration that takes neither.
    void reject_arrow(statement_head const& head);
    void reject_assignment(statement_head const& head);
};
} // namespace sgl::ast::impl
