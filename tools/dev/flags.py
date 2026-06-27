"""Shape raw CMake File API target JSON into resolved compile/link flags.

The File API target object (see targets.load_target_models) already separates a
target's compile settings into `compileGroups` — one per distinct flag-set — and
its link line into role-tagged fragments. This module turns that into the plain
CompileGroup / TargetFlags data the `info` command prints. No I/O: callers pass
the dict that targets.py loaded.
"""

from __future__ import annotations

from .models import CompileGroup, TargetFlags


def _compile_group(group: dict, sources: list[dict]) -> CompileGroup:
    indexes = group.get("sourceIndexes", [])
    std = (group.get("languageStandard") or {}).get("standard")
    return CompileGroup(
        language=group.get("language"),
        std=std,
        defines=[d["define"] for d in group.get("defines", []) if "define" in d],
        includes=[
            (inc["path"], bool(inc.get("isSystem", False)))
            for inc in group.get("includes", [])
            if "path" in inc
        ],
        flags=[
            frag["fragment"]
            for frag in group.get("compileCommandFragments", [])
            if frag.get("fragment")
        ],
        sources=[sources[i]["path"] for i in indexes if 0 <= i < len(sources)],
    )


def extract_flags(target_json: dict) -> TargetFlags:
    """Resolve a target's compile groups and link line from its File API JSON.

    Targets without a link step (static libraries) yield empty link lists; their
    compile groups are still populated.
    """
    sources = target_json.get("sources", [])
    link = target_json.get("link") or {}
    link_flags: list[str] = []
    link_libraries: list[str] = []
    for frag in link.get("commandFragments", []):
        text = frag.get("fragment")
        if not text:
            continue
        if frag.get("role") == "flags":
            link_flags.append(text)
        elif frag.get("role") == "libraries":
            link_libraries.append(text)
    return TargetFlags(
        name=target_json["name"],
        kind=target_json.get("type", "UNKNOWN"),
        compile_groups=[_compile_group(g, sources) for g in target_json.get("compileGroups", [])],
        link_flags=link_flags,
        link_libraries=link_libraries,
    )
