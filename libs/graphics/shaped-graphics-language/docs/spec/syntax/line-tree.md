# Line tree

The line tree is the first phase: bytes in, a tree of lines out.
It looks at line ends and leading whitespace, and at nothing else.
Back to the [phases](_index.md); the reasons are in [why/line-tree.md](why/line-tree.md).

## Lines

* **LINE-1** A source file is a sequence of bytes, read as UTF-8.
* **LINE-2** A UTF-8 byte order mark at the start of the file is skipped and kept.
* **LINE-3** A line ends at `\n`, at `\r\n`, or at a `\r` that no `\n` follows ([why](why/line-tree.md#line-3)).
* **LINE-4** The bytes of a line end belong to the line they end, and are kept.
* **LINE-5** The last line of a file ends at the end of the file when no line end is there.
* **LINE-6** A **blank line** is a line that is empty or holds only spaces and tabs.

## Indentation

* **LINE-7** The **indentation** of a line is its leading run of spaces and tabs.
* **LINE-8** The **indentation width** is counted in columns: a space is one column, and a tab advances to the next multiple of 4.
* **LINE-9** A tab in the indentation is the normal error `tab-in-indentation`, reported once per line ([why](why/line-tree.md#line-9)).
* **LINE-10** A blank line has no indentation, and LINE-9 does not apply to it.

The second line below is indented with one tab, which reads as 4 columns and reports `tab-in-indentation`.

```sgl error
if ready:
	let a = 1
```

## Parents

* **LINE-11** The **parent** of a line is the nearest line above it that is not blank and has a smaller indentation width ([why](why/line-tree.md#line-11)).
* **LINE-12** A line without such a line above it is a **top-level line**.
* **LINE-13** The children of a line keep their source order.
* **LINE-14** A child does not have to align with its siblings ([why](why/line-tree.md#line-14)).
* **LINE-15** The first line of a file is a top-level line, whatever its indentation.

A comment may indent its lines freely, so it shows LINE-11 and LINE-14 without a diagnostic.

```sgl
// the comment starts here
        this line is its child
    and so is this one
  and this one, which aligns with nothing
let x = 1
```

| line | parent |
|---|---|
| `// the comment starts here` | top level |
| `this line is its child` | `// the comment starts here` |
| `and so is this one` | `// the comment starts here` |
| `and this one, which aligns with nothing` | `// the comment starts here` |
| `let x = 1` | top level |

Code lines are written with 4 columns per level.
A code line that aligns with nothing is still a child by LINE-11, and a lint reports it.

```sgl sketch
if ready:
    let a = 1
  let b = 2
let c = 3
```

Here `let b = 2` is a child of `if ready:`.

## Blank lines

* **LINE-16** A blank line opens nothing and closes nothing.
* **LINE-17** The spaces and tabs on a blank line are never content.
* **LINE-18** A blank line takes the parent of the next line that is not blank ([why](why/line-tree.md#line-18)).
* **LINE-19** A blank line with no such line below it is a top-level line.

```sgl
if ready:
    let a = 1

    let b = 2

let c = 3
```

| line | parent |
|---|---|
| `let a = 1` | `if ready:` |
| first blank line | `if ready:` |
| `let b = 2` | `if ready:` |
| second blank line | top level |
| `let c = 3` | top level |

## Line kinds

* **LINE-20** Each line has one kind: blank, code, comment, or string content.
* **LINE-21** A line that is not blank is tokenized in the mode its parent hands down, and that mode decides its kind ([why](why/line-tree.md#line-21)).
* **LINE-22** A top-level line is tokenized in code mode.

The modes are defined in [tokens.md](tokens.md).
The line tree is built before any line is tokenized, so no token can change it.

## Locality

* **LINE-23** A line affects only itself, its children, and the first token of its next sibling ([why](why/line-tree.md#line-23)).
* **LINE-24** A line never affects its parent, a sibling above it, or any line after its next sibling.

The first line below is an undelimited string, and the error ends with that line.

```sgl error
print "hello
let x = 1
```

## Open

* Whether a form feed or another control character in the indentation is part of it or starts the content.
* The name and severity of the lint that reports a misaligned code line.
