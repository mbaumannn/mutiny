# Reference engine

A copy of `src/engine/engine.c` and `engine.h` from before the scale-table change.
`tests/test_engine_equivalence.py` renders the same corpus with this copy and with the current
engine, in two separate executables, and compares every field of all 64 steps.

The only expected difference is the note field for the twelve scale rows the OS 1.44 table leaves
empty, which the current engine plays chromatically. Do not edit these files; `manifest.json` holds
their hashes.
