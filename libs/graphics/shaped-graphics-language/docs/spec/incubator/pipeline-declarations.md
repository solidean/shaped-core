# Pipeline Declarations

*Incubator: not normative.*

## The idea

A GPU pipeline combines shader stages with fixed-function state, and today the host builds each one in C++.
**A `pipeline` declaration states the statically known combinations in the shader file**, beside the stages they combine.

```sgl sketch
pipeline cube:
    vertex = main_vs
    pixel = main_ps
    topology = .triangle_list
    cull = .back
    depth = (test = .less, write = true)

pipeline cube_quick = (main_vs, main_ps)
```

* **Most fields come from the entry points.**
  The vertex input is the vertex stage's `@vertex struct`, the color targets are the pixel stage's `@pixel struct`, and the layout is the entry points' binding lists.
  So the short form is enough for most pipelines, and the block carries fixed-function state.
* **Field names and values mirror sg's `raster_pipeline_description`** rather than inventing SGL words.
  The prelude defines a struct or an enum for every pipeline field, so a preset is a named constant, or a compile-time function.
* **Entry points and edge structs may carry defaults as attributes**, such as `@depth(...)` on a pixel shader.
  Configuration then lives beside the code it belongs to, and a declaration overrides it.
* **The binding lists of the stages must be compatible**, and a pair that is not is an error at the declaration rather than at the bind.
* **It generates one symbol per pipeline** next to the entry points, which acquires the pipeline with its layout built from the generated group types.

## What it touches

* The grammar: `pipeline` as a declaration, in a long and a short form.
* The prelude: types for every field of a raster pipeline description.
* [Stage interfaces](stage-interfaces.md): attributes on entry points and edge structs that serve as defaults.
* [Generated host code](host-code-generation.md): one more generated symbol, and the layout from the groups it already generates.

## Already fixed by the syntax

* A `.name` literal resolves against the type it is used as, so `.triangle_list` needs no qualification.
* Attributes are an open, additive set, so a default such as `@depth(...)` needs no grammar of its own.

## Open

* Formats a shader leaves open: stated in the block, or parameters of the generated symbol at acquire.
* What "compatible" binding lists means exactly, beyond agreeing position by position.
* Ray tracing: a fixed set of shaders fits, and a material system that assembles hit groups at runtime keeps the C++ API.
