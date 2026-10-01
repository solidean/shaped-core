# shaped-shader-library TODO

Running list of known follow-ups — what is **open**.
What is already implemented is [structure.md](structure.md)'s tagged tree, and the design behind each area is its concept doc.

- **SGL modules are read whole on every compile.** Every SGL compile lists and reads every module directory of the library.
- **A module file that appears is not seen by hot reload.** A shader depends on the module files it reached, or on every one that existed when it failed, so a file created later is nobody's dependency.
  The way out is a dependency per module directory whose revision is its listing.
- **Module names are the library's at run time and the package's at build time.** A build describes a package against its own module directories, while a running library merges every package's.
  So two packages whose directories each declare a module `common` build cleanly and then become one module `common` at run time,
  and a package can `use` a module only another package's directory holds without listing it, which works only where both are added.
  Neither is checked yet; separating the two cleanly needs a design of its own.
  `real_filesystem::list` reads the disk through `<filesystem>`, since clean-core has no directory listing yet.

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
