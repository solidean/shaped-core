# project-template — a new project on shaped-core

The deterministic half of [starting-a-project.md](../../docs/guides/starting-a-project.md): the files every project consuming shaped-core starts with.

- [new-project.py](new-project.py) writes [template/](template/) into a project's root, `__project__`, `__lib__` and `__ns__` substituted, and copies the house-style files from shaped-core's root.
  It writes nothing when any file it would write exists already.
- [template-self-test.py](template-self-test.py) lays a project out in a throwaway directory, this checkout linked in as its `extern/shaped-core`, and runs `dev.py check --all` there.
  It is the `project-template-selftest` gate, and a cold build of clean-core and nexus makes it about a minute long.

**The template's links resolve in the project, not here**, so shaped-core's crossrefs gate skips `template/` (`Layout.crossref_exclude`) and the self-test checks them where they point.
What needs judgement, the CLAUDE.md and the skills above all, stays in the guide rather than in a template file.
