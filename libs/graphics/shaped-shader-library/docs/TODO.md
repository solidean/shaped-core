# shaped-shader-library TODO

Running list of known follow-ups — what is **open**.
What is already implemented is [structure.md](structure.md)'s tagged tree, and the design behind each area is its concept doc.

- **A package has one `SOURCE_DIR`, so a shader cannot include a header from outside it.**
  `sc_add_shader_package` takes a single `SOURCE_DIR`, and the generator's `include_closure` resolves and embeds every `#include` under that one directory.
  A shipped binary reads its shaders from the embedded copy and `ssc::dxc` has no filesystem fallback.
  So an include reaching outside that directory has nothing to embed, and fails at run time rather than at build time.

  A vendored shader header is the case that wants one.
  shaped-rendering's NRD member must call NRD's own `NRD.hlsli` for the packing formulas — reimplementing them drifts silently into a worse image — and NRD ships it inside `extern/nrd/.install`.
  [extern/nrd/CMakeLists.txt](../../../../extern/nrd/CMakeLists.txt) works around it by copying the header into `libs/graphics/shaped-rendering/shaders/nrd/` at configure time.
  That is the only place in this repository where configure writes into the source tree.

  Closing it is an `INCLUDE_DIRS` list beside `SOURCE_DIR`: threaded into the manifest, read by `include_closure` when it resolves an include, and embedded like the package's own files.
  The part that decides the size of the change is what `slib::shader_library` does at run time with an embedded path that is no longer relative to `SOURCE_DIR`.
  A file outside the source directory needs a path the run-time lookup agrees on.

  Until then the copy stands, and two costs come with it.
  Two presets configured from one checkout write the same files, so the source tree carries whichever version configured last.
  And a read-only source tree cannot configure.
