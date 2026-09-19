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
* any run of [0-9a-Z] + "_" + "-" + "@" + "\" + ' + unicode ranges for identifiers is all a "symbol" token.
  there are no keyword tokens and no number vs identifier vs attribute splits at this point.
  so "10a7" is a single symbol token
  "a-b" is a single symbol token, not "a" "-" "b". subtraction always needs spaces.
  we currently reserve \ here as well because we might want to support math expressions
* "." is DOT
* ";" is SEMICOLON
* ":" is COLON (used for "denotes", aka type annotations and blocks)
* "::" is DOUBLE_COLON
* "," is COMMA
* "->" is SINGLE_ARROW (used for return types)
* "=>" is DOUBLE_ARROW (used for "computes as" in case expressions and properties and lambdas)
* "//" starts a COMMENT for the rest of the block
* " is DOUBLE_QUOTED_OPEN/CLOSE (and then usual \ escape rules in between)
* ' is SINGLE_QUOTED_OPEN/CLOSE (mostly reserved right now)
* ` is BACK_QUOTED_OPEN/CLOSE (mostly reserved right now)
* [!+-*/%=<>?&^/]+ is an "operator" token (we max-munch them with special precedence rules later similar to scala. work with whitespace to disambiguate)
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
```

This also means that the tokenizer has a special "expect end of string delimiter, then code" mode in addition to code/comment/string.
So tokenizing a block always returns the mode that subblocks start in and the mode the next (sibling) block starts in.
Notably, this enforces our invariant: blocks can influence their children and the follow up siblings but never their parents and beyond.
In the expect end of string mode, we simply tokenize with the code rules, except that the first " is not string start but string end.
If the result does not have string end as the first token, we emit a normal (non-fatal) error.

Important invariant: tokenized block trees must round-trip. aka we can write a printer that byte-equal prints back the original source from tokenized block trees.
This also applies 

## Syntax Trees

Block trees are now parsed into syntax trees.
A major invariant is that whole block-subtrees can only be parsed into whole syntax trees.
This is slightly relaxed because runs of sibling block trees can also be together (for multi-line expressions) but that's it.
It matches how we visually recognize and parse source.
The syntax rules here are very loose and uniform on purpose.
We impose more rigid syntactic structure later based on context.
The invariant also means that syntax can be parsed in parallel if desired.

TODO: is the name "syntax tree" ok here or should we change it?



## 



## Comments

TODO: we're not sure if we want to use '#' or '//' for comments yet. - for now we assume '//' so we could treat embedded markdown easier

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
