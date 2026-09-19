# Beyond Shaders

*Incubator: not normative.*

## The idea

SGL is a shading language first.
It is also, quietly, **a test run for a general-purpose language** built on the same ideas:

* indentation as structure that nothing can break
* a generic form tree under a thin surface language
* errors that cannot escape their block
* whitespace that carries meaning wherever it removes an ambiguity

That second purpose is why some decisions are made more generally than a shading language needs.
It is worth knowing which ones, so nobody "simplifies" them away later for being unused:

* **The form tree is language-independent, and the keyword table is data.**
  A different surface language could be read by the same parser with a different table.
* **Tagged strings and embedded languages** ([string-family.md](string-family.md)) matter little for shaders.
* **User-declared operators** ([user-operators.md](user-operators.md)) are reserved by construction rather than needed.
* **Structural and nominal typing side by side** ([structural-types.md](structural-types.md)) is a type-system experiment as much as a convenience.

**Modules instead of namespaces** is a decision made for the general case too.
A C++-style namespace can be reopened by unrelated code, which invites identifier poisoning and stealing.
A module cannot: unrelated code cannot add to it, and two modules of the same name are still different modules.
So `.` is the only accessor, and `::` does not exist.

**What does not generalize** is equally worth writing down.
The function model ([function-model.md](function-model.md)) — no recursion, no indirect calls, everything inlined, no captures — is a gift of the shader domain.
A general-purpose successor would have to find another answer there, and much of SGL's simplicity rests on it.

## Open

* Which SGL decisions a successor would keep unchanged, and which depend on the function model.
* Whether the form-tree layer should become a library of its own once a second language wants it.
