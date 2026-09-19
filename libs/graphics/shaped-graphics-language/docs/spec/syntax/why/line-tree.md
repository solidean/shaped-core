# Why: line tree

The reasons behind the rules of [line-tree.md](../line-tree.md).
Nothing here is normative.

## LINE-3

All three line ends exist in real files, and a shader is often pasted together from sources of mixed origin.
Accepting all of them costs nothing, because the bytes are kept and the source still round-trips.
A lone `\r` counts so that an old Mac line end cannot glue two lines into one and hide a whole statement.

## LINE-9

A tab has no width of its own, so a file with tabs looks different in every editor, and in SGL the look is the structure.
The language still has to give the tab *some* width to stay total, and the next multiple of 4 is what most editors show.
It is a normal error and not a fatal one because the reading is obvious and the fix is mechanical.
Once per line is enough: forty tabs on one line are one mistake.

## LINE-11

Python builds its blocks with an indentation stack, and a line that returns to a width the stack never held is an error that stops the parse.
SGL asks a simpler question: which line above is less indented?
That question always has an answer, so the line tree is total, and it needs no token to answer it.
This is the root of every locality promise the language makes.
An unclosed parenthesis or an open string cannot swallow the rest of the file, because by the time tokens are read the structure is already fixed.
It also matches how people skim code: by the shape of the left margin, long before they read a character.

## LINE-14

Take a line indented by two that sits below siblings indented by four.
It is still beyond its parent, so it is a child of it.
Refusing it would need a rule with no good recovery: is the line a child, a sibling of the parent, or nothing?
Comments and multi-line strings need the freedom anyway, since their lines are text and align with whatever the text wants.
So alignment is a lint for code, 4 columns per level, and not a rule of the tree.

## LINE-18

A blank line has no indentation worth trusting: editors strip it, or leave stale spaces from the line before.
So it cannot place itself, and it borrows its place from the next line that can.
The consequences are the ones a reader expects.
A blank line between two statements of a body is inside the body.
A blank line after the last statement is outside, so a multi-line string does not end in an accidental empty line.
A blank line at the end of the file belongs to the file.
The alternative, giving it the parent of the line *before*, would make trailing blank lines part of every block and every string.

## LINE-21

Whether a line is code, comment or string content is not something the line decides.
`let x = 1` under a comment line is comment text, and under an open quote it is string content.
Deciding the kind from the tree means a tokenizer never scans backwards and never guesses.
It also makes syntax highlighting exact with a very small grammar: the editor needs the parent, and nothing else.

## LINE-23

This is the principle the rest of the syntax is built to keep: no syntax error escapes its indentation.
In most languages one missing quote or brace turns the rest of the file into noise, and the compiler reports the damage far from the cause.
For a shading language that is edited live, that is the difference between one red line and a black screen.
The reach is exactly as wide as the features need.
Children, because a comment, a string and a block own their lines.
The first token of the next sibling, because a multi-line string and a multi-line parenthesis close there.
Nothing reaches a parent, and nothing reaches past the next sibling.
A pleasant side effect is that sibling subtrees are independent, so they can be parsed in parallel and re-parsed one at a time on an edit.
