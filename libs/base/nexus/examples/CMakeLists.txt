# nexus examples: one binary per file, each demonstrating one part of nx::args in practice.
# Run one with `uv run dev.py example nexus/args-locals`; see docs/guides/examples.md.
#
# Each declares its own argument line with nx::config::args, so it demonstrates something with nothing
# typed — and so it is never handed the harness's own --examples flag to parse.
# Pass a different one with `--test-args "..."`.
sc_add_single_file_examples(nexus
    FILES
    args-locals.cc
    args-struct.cc
    args-commands.cc
    args-validation.cc
    LINK nexus
    PCH CC_STD NEXUS
)
