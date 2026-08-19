# Dependencies

Everything shaped-core builds against that we did not write lives under `extern/`, at a pin we chose deliberately.
This guide covers where a pin is declared, how to see whether upstream has moved on, and how to bump one.

`uv run dev.py deps list` answers *what are we on, and is any of it stale*.
`uv run dev.py deps licenses` regenerates [docs/licenses/](../licenses/_index.md), the shipping bundle of every license we redistribute.

We bump dependencies rarely enough that automating it would be a liability, so there is deliberately no `deps update`.
Bumping means re-vetting an upstream and often adjusting a copy map — a human call, taken once and recorded in a commit.

## The manifest

Each dependency directory carries a `dependency.yml` declaring its upstreams.
It is the single source of truth for the pin: the `vendor-*.py` / `fetch-*.py` next to it reads its pin from there, and so does `dev.py deps`.
Nothing else in the tree records a version.

```yaml
# extern/xxhash/dependency.yml
upstreams:
  - name: xxHash
    homepage: https://github.com/Cyan4973/xxHash
    source: git
    repo: https://github.com/Cyan4973/xxHash
    track: tags
    tag: v0.8.3
    digest_algo: git-commit
    pin_hash: e626a72bc2321cd320e953a0ccf1584cad60f363
    license: BSD-2-Clause
    license_files: [LICENSE]
    used_by: clean-core — cc::hash128 (the XXH3 128-bit hash)
```

A directory holds a *list* because several upstreams can share one build target: `extern/imgui/` holds three, `extern/zydis/` two.

### The fields

**`pin_hash` is uniform and `digest_algo` says what it is.**
That pairing is the point of the schema.
`pin_hash` is whatever the fetch must match — a clone's resolved HEAD, or the content of `.install/pin.txt` — and `digest_algo` is `git-commit`, `sha256` or `sha3-256`.
Before the manifests, the same constant name meant a git commit in six scripts and an archive digest in three, with sqlite's being SHA3-256 and nothing recording that.

**`source` and `track` are separate.**
`source` is how we obtain it (`git`, `github-release`, `url`); `track` is how "what is current" is defined (`tags`, `default-branch`, `github-releases`, `sqlite`, `none`).
They diverge more often than you would expect.
Zydis is a `github-release` we *clone* rather than download, so its digest is a git commit.
stb, ImPlot and ImGuizmo cut no usable tags, so they are `default-branch`: "newer" means a head some number of commits ahead, not a higher version.

**`install` names the tier.**
`vendored` is committed in-tree.
`fetched` hydrates a gitignored `.install/` on demand, so it can be absent or stale on any given checkout.
`bundled` arrives inside another upstream in the same directory, as Zycore does inside the Zydis amalgamation.

**`tag_pattern` filters what counts as a version**, for `track: tags` only.
Upstreams tag far more than they release, and GitHub's tags endpoint has no useful order, so tags are filtered by this pattern and then sorted numerically.
The default matches a plain version number.
Dear ImGui needs `^v\d+(\.\d+)*-docking$`, because we track its docking branch rather than mainline.

**`license` is an SPDX identifier or expression**, `license_files` are paths relative to the dependency directory, and `used_by` is the one-line answer to "why do we have this".
An upstream that ships no license file of its own carries the text inline as `license_text` — the SQLite amalgamation is the only one.

**`notes` is free text for what a reader would otherwise have to rediscover**, and it is where a real subtlety belongs rather than in a comment nobody rechecks.
SDL3's note records that SDL vendors a hidapi with a GPL-3 option, and that our `extern/sdl3/CMakeLists.txt` sets `SDL_HIDAPI OFF` so it never compiles.

### What is *not* in the manifest

Copy mechanics — `COPY_MAP`, `WIPE`, `STRIP_PREFIX`, `ARCH_MAP`, post-copy rewrites, Zydis's amalgamate step — stay in the scripts.
A copy plan is executable, not configuration.
DXC's members are architecture-templated and ImGuizmo needs a post-copy include fixup, neither of which YAML expresses without becoming a worse programming language.
The split is that the manifest owns *what we are on*, and the script owns *how it gets here*.

## `deps list`

```bash
uv run dev.py deps list              # resolve upstream (cached for a day)
uv run dev.py deps list --offline    # manifests and installed pins only, no network
uv run dev.py deps list --refresh    # ignore the cache
uv run dev.py deps list --json       # for scripting
```

The network is the default path, because a pin with no notion of what is current answers half the question.
It is about a dozen requests, well inside the unauthenticated GitHub rate limit; `GITHUB_TOKEN` is honored when set, and a rate-limit error says so rather than reading as "no update".
Results cache to `.tmp/deps/updates.json` for 24 hours, keyed by pin — bumping a pin invalidates that entry by itself.

The second block reports the fetched dependencies as **current**, **stale** or **not fetched**.
A `.install/` that does not match its manifest is otherwise invisible, and it is what makes a build mysteriously use the wrong version.

**`deps list` is deliberately not a `check` gate.** Network in the pre-commit gate buys only flakiness.
If nagging is ever wanted, a CI cron job that opens an issue is the right home.

## `deps licenses`

```bash
uv run dev.py deps licenses          # regenerate docs/licenses/
uv run dev.py deps licenses --check  # verify without writing; what `dev.py check` runs
```

It writes one file per license, a generated `_index.md`, and a copy of our own `LICENSE` as `shaped-core.txt`, so the directory is a complete bundle rather than everything-except-ours.
The output is committed, which keeps it complete on a checkout where the fetched dependencies were never fetched.

**It never deletes a committed license for a dependency that is not currently fetched.**
It warns and keeps what is there.
Otherwise regenerating on a Linux box, where DXC does not exist, would silently gut the bundle — and the gutting would look exactly like a legitimate regeneration in the diff.

`--check` needs no network, which is what makes it cheap enough to sit in `dev.py check` as the `deps-licenses` gate.

### The license policy

[tools/deps/license-policy.yml](../../tools/deps/license-policy.yml) is an allowlist of SPDX identifiers, each carrying why we accept it, plus the copyleft ones we refuse and why.
Every `license:` field must appear there or the gate fails.
An expression like `MIT OR Unlicense` passes only when every branch does, since we may end up relying on any of them.

That gate exists for the case where a version bump quietly swaps a license, which is otherwise invisible in a diff full of vendored source.
DXC is the standing exception, and reading its actual text is what found it.
DirectXShaderCompiler's *source* is NCSA, but the prebuilt release we download ships Microsoft Software License Terms.
That is proprietary, and permissive only about redistributing `dxcompiler.dll` and `dxil.dll` with an application.
It is listed by name under `exceptions` with that reasoning, never approximated as an open-source identifier.

## Bumping a dependency

1. `uv run dev.py deps list` to see what upstream offers.
2. **Read the upstream changelog**, and re-read its license if the version jump is large.
   This is the step the tooling cannot do for you, and the reason there is no `deps update`.
3. Edit `extern/<dep>/dependency.yml`: `tag` / `version` / `pin_hash` together, plus `asset` for an archive-fetched one.
4. Run the dependency's own script — `uv run extern/<dep>/vendor-<dep>.py`, or the `fetch-`/`download-` one.
   It verifies the pin before copying anything, so a mistyped hash fails loudly rather than vendoring the wrong tree.
5. `uv run dev.py deps licenses` — a changed version changes the index, and may change the license text.
6. `uv run dev.py check --fix`, then commit the manifest, the payload and the regenerated licenses together.

For a fetched dependency there is nothing to commit but the manifest: the next `dev.py configure` sees the pin no longer matches `.install/pin.txt` and re-fetches.

## Adding a dependency

Add `extern/<dep>/` with a `dependency.yml`, a `CMakeLists.txt`, and a vendor or fetch script modeled on the closest existing one.
`extern/xxhash/` is the model for a vendored git pin, `extern/sdl3/` for an archive fetch.
Register it in [extern/CMakeLists.txt](../../extern/CMakeLists.txt) with an `SC_USE_VENDORED_<LIB>` option; a fetched one also gates on `EXISTS .install/pin.txt`, so a plain checkout still configures.
A fetched one needs an `ensure_*` in [tools/dev/lib/pipeline/prereqs.py](../../tools/dev/lib/pipeline/prereqs.py) too.

If its license is not already on the allowlist, read the terms and add it there with a reason — that is the gate doing its job, not an obstacle to route around.

## Related

- [building-and-testing.md](building-and-testing.md) — the `dev.py` command surface these two sit in.
- [../dev-py-driver.md](../dev-py-driver.md) — why the logic lives in `tools/deps/deps.py` and the subcommand is thin wiring.
- [../licenses/_index.md](../licenses/_index.md) — the generated bundle itself.
