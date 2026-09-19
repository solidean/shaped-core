# Why: strings and comments

The reasons behind the rules of [strings-and-comments.md](../strings-and-comments.md).
Nothing here is normative.

## CMT-1

The choice was between `#` and `//`.
`//` is what HLSL, GLSL, WGSL, MSL and C++ use, so nobody has to relearn it.
It also frees `#` for hash literals such as `#rgb` and `#rrggbbaa`, which a graphics language wants far more than a second comment marker.
There is no block comment, because a block comment is a construct that opens on one line and closes on another, and that is exactly what escapes indentation.

## CMT-2

`///` plus a space is four characters wide.
So the continuation lines of a documentation comment, indented by the usual 4 columns, line up exactly with the text of the first line.
That is a small thing, and it makes long documentation comments pleasant to write without a marker on every line.

## CMT-4

One marker comments a paragraph.
Prose in a comment wants to wrap and to be indented for lists and examples, and a marker on every line is noise that every editor needs a command for.
The stronger reason is commenting code out.
Putting `//` before an `if` header comments out its whole body, because the body is its children, and removing the marker brings it back.
No block comment syntax is needed, and the rule cannot fail: there is no closing marker to forget.
The lines are comment text "whatever they contain", so a stray quote inside commented-out code is harmless.

## STR-6

An undelimited string is the classic file-destroying typo.
Here the string simply closes at the end of its line, the tree carries that reading, and the next line is code again.
A development build even runs.
This is the model case of a normal error: the language forbids it, and one reasonable reading exists.

## STR-7

Multi-line strings need no special syntax because the line tree already provides the block.
A quote at the end of a line has nothing to delimit on that line, so its children are the content, exactly as a comment line owns its children.
The closing quote sits on the next sibling, because a child line can never affect its parent, and the first token of the next sibling is as far as a line may reach ([LINE-23](line-tree.md#line-23)).
After the closing quote the line is ordinary code, so `" + "` and `")` work without any further rule.
There is only this one form.
Content on the opener line would raise the question of what its indentation is, and every language that allows it has a footnote about that.
The fixed 4 columns that are taken off are the same 4 columns code uses, so a string body looks like any other body.
No trailing newline is added, because adding one is trivial and removing one is not.

## STR-21

In a multi-line string nothing can end the string early: the end is decided by indentation, not by a character.
So nothing needs escaping, and a string without escapes can hold a regular expression, a Windows path or a snippet of another language as is.
A content line that holds one `"` is simply the string `"`.
One-line strings keep the usual backslash escapes, because there the quote *does* close the string.
A newline in a multi-line string is a line break in the source, and a tab is a tab.

## STR-23

SGL has no first-class strings: a GPU has no use for them.
Strings exist for `print` and `assert`, which the compiler supports directly and compiles out of production code.
Their one real job is formatting values into messages, so interpolation is the feature, not an extra.
`$name` is the shortest spelling for the common case, and `$(expr)` is the general one.
`$` was free: it is not an operator character and not a symbol character.
A literal dollar is `$$` and not `\$` so that one rule covers one-line and multi-line strings, since the latter have no backslash escapes.
Only double quotes interpolate, which leaves single quotes and backquotes free for a later meaning.
The tokens are produced now, so that tools and the highlighter agree on them, and the AST rejects them until the semantics exist.
The reserved `"""` opener is meant for the raw variant, a multi-line string without interpolation.
