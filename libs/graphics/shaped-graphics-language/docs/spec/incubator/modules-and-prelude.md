# Modules, Visibility and the Prelude

*Incubator: not normative.*

## The idea

**A `module` line declares the file as belonging to a module, and it is optional.**

```sgl sketch
module example
```

A shader can be written without one.
Reusable code needs one.

* **Without a module, everything is private in a sense.**
  Nothing in the file can be named from outside, because there is no module to name it through.
* **With a module, everything is exported by default.**
  A `private` keyword or something similar can come later.

The guess behind that default is that the majority of symbols are meant to be public in a shader setting.

Another file reaches a module through `use`, at file level or inside a function body ([scopes.md](scopes.md)):

```sgl sketch
use brdf_library as brdf

let color = brdf.lighting_diffuse(base_color = albedo, normal = n, light_dir = l)
```

Modules rather than namespaces is a decision of its own: unrelated code cannot add to a module.
[beyond-shaders.md](beyond-shaders.md) has the reasoning.

**A standard prelude is added automatically.**
It predefines lots of useful types: the component-wise vectors, the `tg` mirrors, the format types ([vector-and-format-types.md](vector-and-format-types.md)).
Opting out is a matter of shader config, not of source.

**The prelude is two files, split by who writes them.**

* **`builtins.sgl` is generated.**
  Every declaration in it is `@builtin`, and each stands for one record of the compiler's builtin registry, which is C++.
  A record holds what SGL cannot say about a builtin: how it evaluates, how each target spells it, what it measures in a constant block.
  C++ is the source of truth and the file is written from it, so nothing has to agree with anything.
  It is committed anyway: the prelude is the documentation of the builtins, and a diff of it is how a change to them is reviewed.
* **`core.sgl` is written by hand.**
  It is ordinary SGL with bodies, checked and inlined like a program's own functions, so an editor can jump into it.
  Whatever can be said in terms of `builtins.sgl` belongs here and not in C++.

Until SGL has generics, an overload family cannot be written once, so the registry's C++ loops write the families out: `dot` for every vector that has one.
That is the reason `core.sgl` is nearly empty today, and it is what generics are expected to reverse.
The normative half of this is [CHK-138](../semantics/checking.md#the-two-files-of-the-prelude), and [adding-a-builtin.md](../../adding-a-builtin.md) is the walk-through.

## What it touches

* Name resolution: the module table, what `use` brings into scope, and the prelude as an implicit first `use`.
* Visibility: a later `private` must be additive, which the export-by-default rule allows.
* The shader config: the prelude opt-out lives there.
* The toolchain: finding the files of a module, and compiling a file that has none.
* Notation: a notation is importable from a module like any other name ([notation.md](../notation.md)).

## Already fixed by the syntax

* `module` and `use` are keywords, and `as` names an import.
* `module`, when present, comes before every other declaration of the file.
* `.` is the only accessor, and `::` does not exist.
* Notation never applies to the names of modules.

## Open

* Whether several files may declare the same module, and how that squares with "unrelated code cannot add to it".
* Whether a file without a module may be the target of a `use` at all, for example by path.
* How `private` is spelled: a keyword, an attribute, or a naming convention.
* Whether `use` without `as` brings the names in unqualified, or only the module name.
* Whether modules nest, and what a dotted module name would mean.
* What exactly the prelude holds, and how much of `builtins.sgl` moves into `core.sgl` once generics can write a family once.
* Whether the prelude is itself an ordinary module, which `use` could then name.
* Whether a prelude name may be shadowed by a declaration of the file.
