# blob-cache examples. Run one with `uv run dev.py example blob-cache/persistent-cache`.
sc_add_single_file_examples(blob-cache
    FILES
    persistent-cache.cc
    LINK blob-cache
)
