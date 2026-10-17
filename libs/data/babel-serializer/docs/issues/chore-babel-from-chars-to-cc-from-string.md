---
created: 2026-10-17
refs:
  - file: libs/data/babel-data/src/babel-data/data/json.cc
  - file: libs/data/babel-serializer/src/babel-serializer/geometry/obj.cc
  - file: libs/data/babel-serializer/src/babel-serializer/geometry/stl.cc
  - file: libs/data/babel-data/.shaped-lint.yml
  - file: libs/data/babel-serializer/.shaped-lint.yml
  - symbol: cc::from_string
---

# babel's text readers still parse numbers with `std::from_chars`

The JSON, OBJ and STL readers parse each number token with `std::from_chars`, under a `<charconv>` lint exemption whose reason was "migrate once a cc:: from-string lands".
`cc::from_string` has landed, and it goes through `std::from_chars` itself without allocating, so speed is no longer the obstacle.

What to check before switching: `cc::from_string` accepts exactly what `cc::to_string` writes, so it rejects a leading `+` among other spellings.
JSON's number grammar forbids `+` anyway; OBJ and ASCII STL files in the wild may not, and a reader must not start refusing them.
Every call site already requires the whole token to be consumed, which matches `cc::from_string`.

Once switched, drop both `<charconv>` exemptions.
