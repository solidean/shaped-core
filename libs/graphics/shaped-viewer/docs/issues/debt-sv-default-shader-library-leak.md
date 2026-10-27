---
created: 2026-10-17
priority: someday
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/shader_library.cc
  - symbol: sv::impl::acquire_default_shader_library
---

# The default shader library is leaked on purpose, and the shape that forces it is not settled

`impl::acquire_default_shader_library` allocates an `slib::shader_library` it never frees.
The generated package symbols an asset is reached through are process-wide globals, so destroying the library at exit would leave them naming freed assets.
So the lifetime is dictated by slib's package globals rather than chosen here, and sv is where the consequence lands.

The construction is wrapped in a `cc::leak_scope` so LeakSanitizer reads the decision instead of reporting it.
That is an annotation and not a fix — it also hides any real leak in that window.
What would settle it is slib owning the library's lifetime itself, with the package globals resolving through it rather than caching raw pointers into it.
Not urgent: one library per process is slib's rule either way, so the leak is bounded and constant.
