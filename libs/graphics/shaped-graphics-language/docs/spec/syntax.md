# SGL Syntax

the sgl language is strongly indentation based, even stronger than python is
this is for error tolerance
an important principle is that no syntax error can escape the current indentation and invalidate the file afterwards. no wrong paren nesting, no open string literal, nothing.
this heavily affects valid syntax.

Instead of a conventional unified grammar for everything, sgl has a different style of parsing.
At a high level:

* source is parsed into block trees by an intentionally stupid parser based on lines and indentation
* the block tree is parsed into a uniform syntax tree (where whole sub-block-trees parse into whole sub-syntax-trees)
* syntax trees follow a 


## Nesting and Block Trees and Tokenization

The first step of the parser is naively to split by line and build nested trees of "blocks".

```
A
B
    C
        D
    E
  F
G
```

produces the tree:
```
* A
* B
    * C
        * D
    * E
    * F
* G
```

So even if "F" has less indentation than "E", it is still beyond "B" and therefore a child of it.
Now, the linting will warn for code: code must be 4 spaces indented (no tabs for indentation for now).
But comments and multi line string literals also abide by this parsing, and for them it's legal.

Source decomposes into these blocks semantically but also syntactically.
We already think about source in these hierarchical blocks and skim based on them.
This rigid preprocess makes the distinction reliable as the language cannot disagree with it.

TODO: is the name "block" a good idea here? it conflates with a "block" in the code, e.g. the whole body of an "if:"

### Tokenization

This process also tokenizes each block (i think it's fine to do it here immediately).
The block parser has a starting mode, which is basically code/comment/string.
For comment and string, it is almost trivial: the whole block is emitted as comment/string token and subblocks start in the same mode.

Strings get support for interpolation later, which means their tokenizer _can_ change modes in between.
Comments might get support for embedded languages later, which could also change tokenizer states.

For code, we make a few simple max-munch rules:
* any run of [0-9a-Z] + "_" + "-" + "@" + "#" + "\" + ' (not at the start) + unicode ranges for identifiers is all a "symbol" token.
  there are no keyword tokens and no number vs identifier vs attribute splits at this point.
  so "10a7" is a single symbol token
  "a-b" is a single symbol token, not "a" "-" "b". subtraction always needs spaces.
  we currently reserve \ here as well because we might want to support math expressions
  we reserve # for things like #rgb etc.
* "." is DOT
* ";" is SEMICOLON
* ":" is COLON (used for "denotes", aka type annotations and blocks)
* "::" is DOUBLE_COLON
* "," is COMMA
* "_" is WILDCARD (can be folded into symbol recognition but is a special token)
* "->" is SINGLE_ARROW (used for return types)
* "=>" is DOUBLE_ARROW (used for "computes as" in case expressions and properties and lambdas)
* "//" starts a COMMENT for the rest of the block
* " is DOUBLE_QUOTED_OPEN/CLOSE (and then usual \ escape rules in between)
* ' is SINGLE_QUOTED_OPEN/CLOSE (mostly reserved right now)
* ` is BACK_QUOTED_OPEN/CLOSE (mostly reserved right now)
* [!+-*/%=<>?&^|~]+ is an "operator" token (we max-munch them with special precedence rules later similar to scala. work with whitespace to disambiguate)
* ([{}]) individually are ROUND/SQUARE/CURLY_OPEN/CLOSE tokens
* there is a special "FUSED" token emitted before DOT, COLON, and the OPEN parens whenever there is no whitespace between the previous token
  the parse uses this to disambiguate "a.b" from "a .b" and from "a . b", which are different. and "a(b)" from "a (b)", which also are
  TODO: is the synthetic fused token the best approach? we could also emit different tokens but then paren balancing becomes less elegant
* an ERROR token for anything we do not recognize

The rules here are inclusive on purpose

Strings are split into the string start/end delimiter and the string body token.
If a block ends with a string start token (aka no body, no end delimiter), then all sub blocks start in the string mode.
If it ends with a string body token, this does not happen (it will emit a "undelimited string" error).
This allows for multi-line strings without special syntax:

```
// normal string literal
print "hello world"

// undelimited string literal (an error)
print "hello

// multi-line strings
print "
    hello world
"
// this is the only valid form
   the print line must not have any string content, not even whitespace
   the end delimiter must be placed on the next sibling block and it must be the first thing
   we always remove a fixed 4 space indentation (same as code)
```

This also means that the tokenizer has a special "expect end of string delimiter, then code" mode in addition to code/comment/string.
So tokenizing a block always returns the mode that subblocks start in and the mode the next (sibling) block starts in.
Notably, this enforces our invariant: blocks can influence their children and the follow up siblings but never their parents and beyond.
In the expect end of string mode, we simply tokenize with the code rules, except that the first " is not string start but string end.
If the result does not have string end as the first token, we emit a normal (non-fatal) error.

Important invariant: tokenized block trees must round-trip. aka we can write a printer that byte-equal prints back the original source from tokenized block trees.


## Syntax Trees

Block trees are now parsed into syntax trees.
A major invariant is that whole block-subtrees can only be parsed into whole syntax trees.
This is slightly relaxed because runs of sibling block trees can also be together (for multi-line expressions) but that's it.
It matches how we visually recognize and parse source.
The syntax rules here are very loose and uniform on purpose.
We impose more rigid syntactic structure later based on context.
The invariant also means that syntax can be parsed in parallel if desired.

TODO: is the name "syntax tree" ok here or should we change it?

So this is going to feel weird the first time you read it, but basically we try to find a common expression-oriented way to build these syntax trees.
And it is almost independent from the whole later sgl surface language.
It is like a common but generic way to read the tokens into a syntax tree.
It not only increases the syntactic uniformity but also provides a generaic way to limit how much errors can affect.

A syntax node can be:
* a literal
    * number literal 
        * a num-only symbol 
        * potentially with 0x or 0b prefix
        * potentially followed by DOT
        * potentially followed by a num-only symbol
        * potentially with "e" or "p" for exponents
        * potentially with a size suffix a la i32, f32 etc.
        * ' is allowed as separator
        * this is all parsed as max-munch runs of tokens of the proper type
    * a paren literal (non-fused round/square/curly open .. close)
        * these literals are lists of sub nodes, delimited by COMMA
        * COMMA is optional at the end of a block
    * a quote literal (single, double, back)
        * TODO: string interpolation
    * a hash literal
        * a symbol that starts with #, like #ff00bb
* an attribute
    * any symbol that starts with "@", like "@builtin"
    * attributes can have arguments started by a fused "(", like "@range(1, 2)" or "@color(#f00)" or "@description("this is a slider")"
* a keyword
    * a symbol from the keyword list
    * keywords have kinds as well (in/as have slightly different syntax rules than the others)
* an identifier
    * a symbol that is not a literal and not an attribute
    * we might want to error identifiers that dont start with "_a-Z"+unicode identifier chars for now (but a normal error, not a fatal one)
* an application (in the lambda calculus sense. not called "call" because it can also become a decl later)
    * an optional list of keywords (not in/as)
    * a list of fused paren lists 
        * within same rules as for paren literals regarding commas etc.
        * any number and amount is allowed here, concrete disallowed stuff rejected later
          (aka "foo(1, 2)(10)[7]" is allowed syntax here but will probably be refused later)
    * a list of inline args "<node>"
        * this is the fallback parse after no ascription arg (next segment) is found anymore
        * the order is always paren args / inline args / trailing args, never mixed
        * the first inline arg is often a plain identifier ("fun foo()" is kw, inline arg ("foo", id), fused round parens without children)
        * in "foo a as baz b" the "b" belongs as inline arg to "baz", not to "foo"
    * a list of trailing args
        * arrow arg "-> <node>" (e.g. for trailing return types)
        * colon arg ": <node>" (e.g. for type ascriptions)
        * in/as arg "in <node>" and "as <node>" (e.g. for loops and casts and renames)
    * a trailing block ":<indent><node>" (only if that is the last : on the line, it starts the parser in block mode for the indented line)
* an operator expression
    * variadic "<op>? <node> <op> <node> <op> ..." (+ a bit of special logic for connectives)
    * already split by precedence levels
        * exp-like (any op starting with "^") - TODO: do we really want this? or "**" special?
        * mul-like (any op starting with "*" or "/" or "%")
        * add-like (any op starting with "+" or "-")
        * bit-like (any op starting with >, <, &, |, ~ that is not a comparison) TODO: xor..
        * comparisons (< <= == != >= >)
        * logical connectives ("and", "or", "not" -> here any <node> can have "not?" and we treat is tightly bound semantically. BUT any non-last not is a normal error due to potential to misread)
        * assignment "=" and any op ending in "=" (except comparisons) - so this includes compound assignments
    * this means non-parenthised expressions of the same precedence level stay together here
      this is needed for comparison chains and other nice features
* a composite block
    * simply a list of nodes

Each node has an optional list of attributes.
They always attach to the next syntax node or if we're inside one to the current one.
If they are last on a line, they apply to the whole line.
If they are the whole line, they apply to the next sibling line that is not just attributes.
If that doesnt exist, it is a normal error (unattached attribute).

IDEA: we could do a simple pre-process before we parse:
* any comment, whitespace, attribute is removed from the token stream and attached to tokens themselves
  aka each token can have leading/trailing comment, whitespace, attributes
  (the trailing versions only exist so we dont loose information)
  and attributes can also be attached to blocks as a whole (for some rules)
  the attribute rules as above should be decidable without parsing
  we can keep the attribute arguments themselves unparsed in the prepass
* the prepass can also already 
* we need to preserve enough information that a formatter can preserve placement that we approve
  aka whole-line, prefix, trailing attributes must be preserved
  but it would be fine if intermediate attributes dont roundtrip (e.g. "fun @vertex foo" is allowed to roundtrip to "@vertex fun foo")
* this pre-pass can also already enforce our paren rules:
  parens you open in a block must be either closed in the same block (NOT in a child one) or you need to close at least the innermost paren at the start of the next line.
  So:

    ```
    // valid
    foo(
        1
        2
    )
    
    // invalid
    foo(
        1
        2
        )
    
    // valid
    [foo(
        1
        2
    ) + bar(
        3
    )]
    ```
* child blocks are either treated as "blocks" or "expressions"
    ```
    // children are expressions and are folded into the parent token stream
    print 10
        + 20
        + 30
    
    // children are blocks (because the parent ended with a COLON) and are treated according to block fuse rules
    if true:
        print 10

    // this is a normal error: indentation is only allowed to vary if it's another parenthesis inside
    // we still parse this as if the tokens just continue but it can be misleading to read
    print 10
        * 20
            + 10
        * 30

    // the first token of a new line is treated as-if fused to the last token on the prev line
    // needed to be able to break up larger chains
    print foo
        .bar(7)
    ```
So basically, we can probably simplify the parser extremely if we pre-pass the tokens into "macro tokens".
Comments, whitespace, attributes become macro token annotations.
The rules for parentheses and which sibling blocks fuse is done in this pass.
So the actual parser doesn't see whitespace/comments/attributes anymore.
We only need to ensure that each consumed token is inspected for carrying attributes, which then have to be parsed as well and are added to the syntax node.
It does not matter in which order or on which tokens they are.
(Attributes that are not on the first token, or whole-line, are a normal error. semantically fine but the formatter will shuffle them accordingly)
The macro tokens also dont have paren open/close anymore but only paren with list of children.
So paren parsing at this stage becomes simple child recursion.
The parser here only sees a straight line of usually a low number of macro tokens.
This makes each parse invocation also a lot cheaper with a lot less potential backtracking.
It is a form of phased & hierarchical parsing.

### Examples

```
// number literals
1
1.
1.0
1e6
1p8
0x7
1'000'000
1p8f32
100u32
18u8

// string literals (double quote literal in this stage)
"hello world"
"
    hello
    second row
"

// paren literals
(1, 2, 3) // called a "tuple literal" in expressions
[1, 2, 3] // called an "array literal" in expressions
{1, 2, 3} // called an "object literal" in expressions
(1, 2,) // trailing comma allowed
(
    1,
    2,
) // multi line with commas
(
    1
    2
) // multi line without commas (mixed also allowed)
foo(1, 2) // this is NOT a parent literal! it is a call expr because it's a fused ()

// attributes
@builtin enum bool
@range(0, 1) // affects the next line
const bias = 0.5
const bias = 0.5 @range(0,1) // trailing attribute affects the whole line node

// applications
let x // yes this is an app here (keyword + id)
foo a : float // identifier + inline arg + trailing colon arg
use materials as mat // kw, id, trailing as arg
if x > 0: // kw, inline arg (the "x > 0" expr)
```


## 



## Comments

TODO: we're not sure if we want to use '#' or '//' for comments yet. - for now we assume '//' so we could treat embedded markdown easier
* // also means we free up #rgb(a) and #rrggbb(aa) syntax

```
// single line comment

let x = 0 // also valid after something

// anything indented after a comment attaches to it
   so nested comment lines are no problem
      even if the nesting
    doesnt
   align with normal nesting rules

```



## Notes

* our parsers should strive to preserve source spans (for diagnostics and various features such as debug info)
* our parsers should not throw away information, even if it is whitespace-like like comments (we want to provide a code formatter as well, so this is important)
* we split errors into fatal errors and normal errors and strive to keep the number of fatal errors small
    * a fatal error cannot be safely recovered from
    * a normal error has recovery logic and in dev/debug mode we can still continue. it is a bit like a warning you have on Werror for production but not for dev. 
        * for example, undelimited string literals are just auto-delimited and we can move on
        * so normal errors are for things the language forbids but where we can make a reasonable interpretation to ease the development process
