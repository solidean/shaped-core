---
created: 2026-10-29
refs:
  - file: tools/dev/lib/pipeline/eligibility.py
  - file: tools/dev/lib/core/process.py
---

# dev.py test crashes on a test name outside the console's code page: the listing is decoded in it, not in UTF-8

`dev.py test "<name>"` crashes when the name holds a character outside the console's code page, `³` in "fuzz - unit cubes on a 4³ grid combine as their bit masks do" for one:

```text
File "tools/dev/lib/pipeline/eligibility.py", line 92, in query_listing
    data = json.loads(proc.stdout)
TypeError: the JSON object must be str, bytes or bytearray, not NoneType
```

Seen on Windows, where the locale's code page is cp1252.
`query_listing` runs the binary with `subprocess.run(..., text=True)`, which decodes its output in that code page, while nexus writes the JSON listing in UTF-8.
The cause, read from the code and not yet confirmed by a fix: a `³` in the listing fails to decode in the reader thread, which leaves `proc.stdout` as `None`.
The `except (json.JSONDecodeError, ValueError)` around `json.loads` does not catch the `TypeError` that `None` raises, so the command dies instead of reporting "could not determine".

What it needs:
- `encoding="utf-8"` on the listing's `subprocess.run`, the encoding nexus writes;
- `None` output treated as any other unreadable listing;
- the other `text=True` runs in `tools/dev/lib` checked for the same: `core/process.py` decodes captured output the same way.
