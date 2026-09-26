# SGL Terminology

Kind of like a glossary.
We strive to be consistent with `sg`, not with any existing shader language.
The specification uses each term in one meaning.
Back to the [specification](_index.md).

## Graphics terms

* "inline constants". called push constants or root constants or SetBytes* in other apis
* "buffer"
* "texture" - a sampled texture, read through the texture unit; also sg's word for the resource itself
* "image" - a storage texture, addressed per texel in a named format, read, written or both
* "binding" - we have binding groups and these hold descriptors
* shader types:
    * raster
        * "vertex"
        * "pixel" - not fragment
        * "geometry"
        * "tessellation eval/control" - TODO i feel like we want "tessellation" in the name but not sure about the other
    * mesh shading??
        * "mesh" - is this right? what about amplification? this is basically TODO
    * compute
        * "compute"
    * ray tracing - TODO: do we want a shared "ray" prefix or not?
        * "raygen"
        * "miss"
        * "callable"
        * "closest_hit"
        * "any_hit"
        * "intersection"

## Syntax terms

Each term links to the rule that defines it.

| term | meaning |
|---|---|
| **line tree** | the tree whose nodes are lines, built from indentation alone ([line-tree.md](syntax/line-tree.md)) |
| **form tree** | the tree whose nodes are forms, generic and nearly independent of the language ([forms.md](syntax/forms.md)) |
| **AST** | the interpretation of the form tree as declarations, statements and expressions ([ast.md](syntax/ast.md)) |
| **line** | a node of the line tree: the bytes up to and with one line end |
| **blank line** | a line that is empty or holds only spaces and tabs (LINE-6) |
| **indentation** | the leading run of spaces and tabs of a line (LINE-7) |
| **indentation width** | the indentation counted in columns: a space is 1, a tab advances to the next multiple of 4 (LINE-8) |
| **parent**, **child** | the parent of a line is the nearest line above it with a smaller indentation width (LINE-11) |
| **sibling** | a line with the same parent; the next sibling is the next one in source order |
| **top-level line** | a line without a parent (LINE-12) |
| **line kind** | blank, code, comment or string content (LINE-20) |
| **mode** | how a line is tokenized: code, comment or string content (TOK-1) |
| **token** | a span of one line with a kind; whitespace is the gap between tokens and is no token |
| **symbol** | the one token kind for names, keywords, number parts, attributes and hash literals (TOK-12) |
| **wildcard** | the symbol that is exactly `_` (TOK-17) |
| **operator** | a token that is a run of operator characters, or that starts with two dots (TOK-22) |
| **word operator** | `and`, `or`, `not`, `as`, `in`: a symbol the keyword table marks as an operator (OP-13) |
| **fused** | a token that touches the token before it, on one line and with no whitespace between (TOK-31) |
| **comment line** | a code line whose only token is a comment; it owns every deeper line below it (CMT-3) |
| **documentation comment** | a comment that starts with `///` (CMT-2) |
| **quoted literal** | a quote open, a body and a quote close, with any of the three quote characters (STR-1) |
| **one-line string** | a quoted literal that opens and closes on one line (STR-2) |
| **multi-line string** | a quoted literal whose opening quote ends its line and whose content is the children (STR-7) |
| **interpolation** | `$name`, `$name.member` or `$(expr)` inside a double-quoted literal (STR-23) |
| **tagged opener** | a quote followed by exactly one symbol and the end of the line; reserved (STR-31) |
| **group token** | a token, or a paren group with its elements as children; the output of the grouping phase (GRP-1) |
| **paren group** | an opener, its closer and the elements between; round, square or curly (GRP-1) |
| **opener**, **closer** | `(` `[` `{` and `)` `]` `}`; the closing quote is the closer of an open string |
| **run** | the flat sequence of group tokens that the form parser reads into one form |
| **block colon** | a `:` that is the last token of its line; it makes the children a block (GRP-9) |
| **block** | the children of a line that ends in the block colon, each a statement of its own |
| **element line** | a child line of an open paren group: a run of elements, or the continuation of the element above (GRP-14, GRP-40) |
| **continuation line** | a child line whose group tokens are appended to the run of its parent (GRP-24) |
| **attribute** | a symbol that starts with `@`, with optional arguments in a fused round group, read like any paren list (GRP-27, FORM-42) |
| **form** | a node of the form tree |
| **keyword** | a symbol that the keyword table lists (FORM-5) |
| **identifier** | a symbol that is no keyword, number, hash literal, attribute or wildcard (FORM-8) |
| **hash literal** | a symbol that starts with `#`, such as `#ff00bb` (FORM-7) |
| **applied** | a paren group that is fused to a symbol, a quoted literal or a paren group before it (FORM-10) |
| **paren literal** | a paren group that is not applied (FORM-11) |
| **named argument** | `name = value` directly inside a paren group (FORM-15) |
| **call** | a form and a paren group applied to it (FORM-18) |
| **member access** | a form, a fused DOT and a name (FORM-20) |
| **leading-dot form** | a DOT that is not fused with a name fused to it, such as `.point` (FORM-23) |
| **application** | a head and inline arguments by juxtaposition: `f a b` (FORM-26) |
| **keyword form** | leading keywords, comma-separated arguments and an optional block (FORM-30) |
| **composite** | the forms of a block, in order (FORM-36) |
| **missing form** | the form that stands where an operand was expected and none stood (FORM-40) |
| **ascription** | every `:` that is not the block colon: `x : int` |
| **computes-as** | the operator `=>` |
| **number literal** | a number assembled from fused tokens by the form phase (NUM-2) |
| **name-free** | the AST phase never looks a name up, and keeps a neutral node where only lookup can decide (AST-2) |
| **type position** | the right side of `:`, of `->`, of `as` or of a `type` alias; the expression there is ordinary, and the AST records the position (AST-10) |
| **reserved name** | an identifier whose meaning is fixed, such as `self`; it is no keyword ([keywords.md](keywords.md#reserved-names)) |
| **call spelling** | how a `call` node is written: paren, juxtaposition, infix or prefix (AST-15) |
| **argument** | an element of a paren group in the AST: an optional name, a value, and whether it is a splat (AST-23) |
| **object shorthand** | a bare name as an element of a curly paren literal, short for `a = a`; the other elements are named arguments and splats (AST-27, AST-97) |
| **splat** | the prefix operator `..` on a whole element of a paren group: `(..normal, 0)` (AST-28) |
| **arrow lambda** | the lambda `parameters => body`; it takes no type parameters and no bindings, and no `return` leaves it (AST-34) |
| **anonymous function** | the lambda that is a `fun` without a name, in expression position: `fun (x) => x + 1` (AST-101) |
| **value block** | a block after `=>:` that is the body of a `case` arm, an arrow lambda or a property; it has a value only through `yield` (AST-107) |
| **yield** | the jump `yield expression`, which gives the nearest enclosing value block its value (AST-108) |
| **jump** | `return`, `break`, `continue` or `yield`; each is an expression (AST-40) |
| **arm** | one `pattern => result` statement of a `case` block (AST-36) |
| **body** | a block, or the right side of an `=>` (AST-42) |
| **pattern** | what a `let` declares: a name, `_`, or a round list of patterns (AST-44) |
| **signature** | what follows `fun`: a name, which an anonymous function leaves out, then `[type parameters]`, `(parameters)` and `{bindings}` in this order (AST-66) |
| **parameter** | a field in the parameters of a signature (AST-68) |
| **binding entry** | an element of the bindings of a signature (AST-69) |
| **signature-only** | a function without a body (AST-72) |
| **composition short form** | `binding name = other` or `binding name = (a, b)` (AST-74) |
| **setting** | one `name = value` line of a `sampler` block (AST-76), or one `path = value` line of a `pipeline` block (AST-131) |
| **pipeline** | shader stages and the configuration compiled into them ([pipelines.md](pipelines.md)) |
| **frozen part** | what a host's own code is built against: a pipeline's binding layout, vertex input, target set, formats and sample count; no hot reload changes it |
| **open part** | a format or a sample count a pipeline leaves to the host with `.host` (CHK-180) |
| **member** | a statement of a `struct`, `enum` or `binding` block, or an element of a `struct_type` (AST-77) |
| **field** | the member `name: type`, with an optional default; also what a parameter is (AST-79) |
| **property** | the member `name => expression`, or `name =>:` and a value block: read-only, no parameter list, no keyword (AST-81) |
| **method** | the member `fun` and a signature; an instance method when it has a receiver, and static otherwise (AST-82) |
| **receiver** | the first parameter of a method when it is `self` or `mut self` (AST-83) |
| **extension** | a `fun` whose name is a type's name, a DOT and a name; it declares a function of that type's scope from outside it (AST-142, CHK-237) |
| **extended type** | the type an extension names before its DOT (AST-142) |
| **named-only** | a parameter or a field written with a leading dot, `.level: float`, which a call fills by name alone (AST-144, CHK-244) |
| **type scope** | the properties and functions of a struct or an enum, its extensions included (CHK-233) |
| **static** | a function of a type scope without `self` (CHK-235) |
| **synthesized constructor** | the function of a struct's name whose parameters are its fields (CHK-239) |
| **candidate** | a function a call collects by its name before any argument is checked against it (CHK-247) |
| **conversion chain** | how an argument becomes its parameter's type; its length ranks candidates (CHK-70, CHK-254) |
| **default type** | the type a number literal has where no other is asked of it: `int` or `float` (CHK-60, CHK-61) |
| **case** | a bare name, or `name = value`, as a member of an `enum`; not the keyword `case` (AST-78, AST-115) |
| **owner** | the declaration or `struct_type` a member stands in; it decides which members are allowed (AST-85) |
| **diagnostic** | a kind, a byte span and a message (DIAG-1) |
| **normal error** | a forbidden construct with one reasonable reading, which the tree carries (DIAG-4) |
| **fatal error** | a construct with no reasonable reading (DIAG-6) |
| **notation** | a replacement inside symbols, applied during name lookup ([notation.md](notation.md)) |
