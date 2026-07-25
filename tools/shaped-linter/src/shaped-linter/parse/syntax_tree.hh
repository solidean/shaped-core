#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <shaped-linter/fwd.hh>
#include <shaped-linter/lex/source_span.hh>

namespace scl
{
/// The node kinds the parser produces. Deliberately tiny: shaped-linter only parses what its rules
/// need (records and variable declarations), treating everything else as opaque.
enum class node_kind : u8
{
    translation_unit,
    record_definition, // a class/struct/union WITH a body
    variable_declaration,
};

enum class record_keyword : u8
{
    class_,
    struct_,
    union_,
};

/// How a declaration's initializer is spelled. `brace` is `name{…}`; `assignment` is `name = …`.
enum class init_form : u8
{
    none,
    assignment,
    brace,
};

/// Where a variable declaration sits. Rules that care about the difference (a data member reads
/// differently from a local) branch on this rather than on the tree shape.
enum class decl_scope : u8
{
    namespace_scope, // file scope or a namespace body
    record_scope,    // a data member of a class/struct/union
    function_scope,  // a local — a function body, a nested block, or a lambda body
};

/// One node in the arena tree. Fields are interpreted by `kind`:
///  - translation_unit / record_definition: `children` are node ids of the records/variables inside.
///  - record_definition: `rec_keyword` and `name` (the record name span; empty if anonymous).
///  - variable_declaration: `scope`, `form`, and for brace form `init_span` (the `{…}` incl. braces),
///    `init_inner` (strictly between the braces), `name` (the declarator-id span), and `declarator`
///    (the declarator-id plus any array suffix — a rewrite replacing the initializer starts at its end).
struct node
{
    node_kind kind = node_kind::translation_unit;
    source_span span; // the whole construct

    // record_definition
    record_keyword rec_keyword = record_keyword::struct_;

    // record_definition (record name) OR variable_declaration (declarator-id)
    source_span name;

    // record_definition / translation_unit
    cc::vector<isize> children; // node ids

    // variable_declaration
    decl_scope scope = decl_scope::namespace_scope;
    init_form form = init_form::none;
    source_span declarator; // brace form: the declarator-id THROUGH any array suffix — `a[N]`, not `a`
    source_span init_span;  // brace form: the `{…}` including the braces
    source_span init_inner; // brace form: the bytes strictly between `{` and `}`
};

/// A soft parse diagnostic. The parser recovers and produces a best-effort tree rather than failing.
struct parse_diagnostic
{
    source_span span;
    cc::string message;
};

/// The arena tree for one translation unit. Nodes are referenced by `isize` id; `root` is the
/// translation_unit node.
struct syntax_tree
{
    cc::vector<node> nodes;
    isize root = -1;
    cc::vector<parse_diagnostic> diagnostics;

    node const& operator[](isize id) const { return nodes[id]; }
    node const& root_node() const { return nodes[root]; }
};
} // namespace scl
