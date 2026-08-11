#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <shaped-linter/fwd.hh>
#include <shaped-linter/lex/source_span.hh>

namespace scl
{
/// The node kinds the parser produces.
/// Deliberately tiny: shaped-linter only parses what its rules need, treating everything else as opaque.
enum class node_kind : u8
{
    translation_unit,
    record_definition, // a class/struct/union WITH a body
    variable_declaration,
    namespace_definition, // a namespace WITH a body; an alias (`namespace A = B;`) produces no node
    using_directive,      // `using namespace X::Y;` — a using-declaration or a type alias produces no node
};

enum class record_keyword : u8
{
    class_,
    struct_,
    union_,
};

/// How a declaration's initializer is spelled.
/// `brace` is `name{…}`; `assignment` is `name = …`.
enum class init_form : u8
{
    none,
    assignment,
    brace,
};

/// Where a variable declaration sits.
/// Rules that care about the difference — a data member reads differently from a local — branch on this rather than on the tree shape.
enum class decl_scope : u8
{
    namespace_scope, // file scope or a namespace body
    record_scope,    // a data member of a class/struct/union
    function_scope,  // a local — a function body, a nested block, or a lambda body
};
} // namespace scl

/// One node in the arena tree.
/// Fields are interpreted by `kind`:
///  - translation_unit / record_definition / namespace_definition: `children` are the node ids inside.
///  - record_definition: `rec_keyword` and `name` (the record name span; empty if anonymous).
///    `follows_record` says the previous statement in the same scope was a record definition too, which is what makes a run of them one block to move.
///    `scope` says where it sits, so a type declared inside a function body is not mistaken for one of the namespace's.
///    `has_declarator` marks the `struct S { } s;` form, whose variable would change scope if the definition moved.
///  - namespace_definition: `name` (the name as written — `a::b` for `namespace a::b`, empty when anonymous).
///    Its `body` is the `{…}` including the braces, which is what a rule tests an offset against.
///  - using_directive: `name` (the nominated namespace, `cc::primitive_defines`).
///    Its `effect` is the bytes over which the directive is in force: past its `;` to the end of the enclosing scope.
///  - variable_declaration: `scope`, `form`, and for brace form `init_span` (the `{…}` incl. braces) and `init_inner` (strictly between the braces).
///    Its `name` is the declarator-id span, and `declarator` that plus any array suffix — a rewrite replacing the initializer starts at the declarator's end.
struct scl::node
{
    node_kind kind = node_kind::translation_unit;
    source_span span; // the whole construct

    // record_definition
    record_keyword rec_keyword = record_keyword::struct_;
    bool follows_record = false; // the statement right before this one was a record definition too
    bool has_declarator = false; // `struct S { } s;` — the definition also declares a variable

    // record_definition (record name) OR variable_declaration (declarator-id) OR the name a
    // namespace_definition / using_directive spells
    source_span name;

    // record_definition / translation_unit / namespace_definition
    cc::vector<isize> children; // node ids

    // namespace_definition
    source_span body;

    // using_directive
    source_span effect;

    // record_definition / variable_declaration
    decl_scope scope = decl_scope::namespace_scope;
    init_form form = init_form::none;
    source_span declarator; // brace form: the declarator-id THROUGH any array suffix — `a[N]`, not `a`
    source_span init_span;  // brace form: the `{…}` including the braces
    source_span init_inner; // brace form: the bytes strictly between `{` and `}`
};

/// A soft parse diagnostic.
/// The parser recovers and produces a best-effort tree rather than failing.
struct scl::parse_diagnostic
{
    source_span span;
    cc::string message;
};

/// The arena tree for one translation unit.
/// Nodes are referenced by `isize` id; `root` is the translation_unit node.
struct scl::syntax_tree
{
    cc::vector<node> nodes;
    isize root = -1;
    cc::vector<parse_diagnostic> diagnostics;

    node const& operator[](isize id) const { return nodes[id]; }
    node const& root_node() const { return nodes[root]; }
};
