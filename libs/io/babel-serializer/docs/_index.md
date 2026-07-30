# babel-serializer docs

Documentation hub for babel-serializer — serialization / deserialization of various formats (namespace `babel`).
Start at the [readme](../readme.md) for the overview and the [cheat-sheet](../cheat-sheet.md) for the API at a glance.

## Source organization

```text
src/babel-serializer/
  fwd.hh          forward decls + the cc::primitive_defines vocabulary aliases
  all.hh          umbrella include (base64 + json + markdown + sqlite + obj + gltf + image)
  data/
    base64.hh/.cc base64 codec: decoded_size() / decode() / decode_into() / encode()
    json.hh/.cc   JSON reader: document / node / ref, read()
    markdown.hh/.cc  markdown block reader: document / node / ref, read() (no inline parsing)
    sqlite.hh/.cc live SQLite handle: database / statement / row (fetch-on-demand backend)
  geometry/
    gltf.hh/.cc   glTF 2.0 / GLB reader: data (+ accessor_view), read() over pinned bytes (zero-copy buffers)
    obj.hh/.cc    Wavefront OBJ reader: data / corner / face / group, read()
  image/
    png.hh/.cc    low-level PNG codec: data (+ native metadata), read() / encode() / write()
    jpg.hh/.cc    low-level JPEG codec: data (+ native metadata), read() / encode() / write()
    image.hh/.cc  image aggregator: image, detect_format() / read() / encode() / write()
    impl/         internal stb backend seam (not public)
```

## Topics

- [structure.md](structure.md) — the format roadmap: what is `[done]` vs `[planned]`, and the design that binds them.
- [coding-guidelines.md](coding-guidelines.md) — babel-specific conventions on top of the repo-wide ones: the always-available-API rule for a fetch-on-demand backend (`sqlite`), the writer convention, the pinned-bytes reader shape and the import-issue list (`gltf`).
- [lower-library-gaps.md](lower-library-gaps.md) — capabilities babel wants from clean-core / typed-geometry that do not exist yet, what it hand-rolls instead, and why.

## Conventions

- **Namespace `babel`**, with a sub-namespace per format (`babel::base64`, `babel::json`, `babel::markdown`, `babel::sqlite`, `babel::obj`, `babel::gltf`, `babel::png`, `babel::jpg`) plus the `babel::image` aggregator; internal details in `babel::impl`.
- **Dependencies:** clean-core (streams / containers / `result`) and typed-geometry (`vec` / `pos` / `mat` / `quat`).
  babel-serializer sits above typed-geometry and below the graphics stack — it never depends on `sg`.
- **Reading** normally takes a `cc::read_stream` (plus string_view / span convenience overloads) and parses against the buffered window.
  A format whose result must hand back zero-copy views of its input takes a `cc::pinned_data<byte const>` instead — see the coding guidelines.
- Formatting follows the repo-wide [coding-guidelines](../../../../docs/coding-guidelines.md); `.clang-format` is authoritative.
