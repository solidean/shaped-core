# babel-serializer docs

Documentation hub for babel-serializer — serialization / deserialization of various formats (namespace `babel`).
Start at the [readme](../readme.md) for the overview and the [cheat-sheet](../cheat-sheet.md) for the API at a glance.

## Source organization

```text
src/babel-serializer/
  fwd.hh          forward decls + the cc::primitive_defines vocabulary aliases
  all.hh          umbrella include (json + sqlite + obj + image)
  data/
    json.hh/.cc   JSON reader: document / node / ref, read()
    sqlite.hh/.cc live SQLite handle: database / statement / row (fetch-on-demand backend)
  geometry/
    obj.hh/.cc    Wavefront OBJ reader: data / corner / face / group, read()
  image/
    png.hh/.cc    low-level PNG codec: data (+ native metadata), read() / encode() / write()
    jpg.hh/.cc    low-level JPEG codec: data (+ native metadata), read() / encode() / write()
    image.hh/.cc  image aggregator: image, detect_format() / read() / encode() / write()
    impl/         internal stb backend seam (not public)
```

## Topics

- [structure.md](structure.md) — the format roadmap: what is `[done]` vs `[planned]`, and the design that binds them.
- [coding-guidelines.md](coding-guidelines.md) — babel-specific conventions on top of the repo-wide ones; today: the always-available-API rule for a fetch-on-demand backend (`sqlite`).

## Conventions

- **Namespace `babel`**, with a sub-namespace per format (`babel::json`, `babel::sqlite`, `babel::obj`, `babel::png`, `babel::jpg`) plus the `babel::image` aggregator; internal details in `babel::impl`.
- **Dependencies:** clean-core (streams / containers / `result`) and typed-geometry (`vec` / `pos`).
  babel-serializer sits above typed-geometry and below the graphics stack — it never depends on `sg`.
- **Reading** always takes a `cc::read_stream` (plus string_view / span convenience overloads) and parses against the buffered window.
- Formatting follows the repo-wide [coding-guidelines](../../../../docs/coding-guidelines.md); `.clang-format` is authoritative.
