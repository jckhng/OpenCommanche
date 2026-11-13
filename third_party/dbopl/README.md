# DBOPL

Portable synthesis core from the project's pinned DOSBox-X backend.
Copyright (C) 2002–2021 The DOSBox Team. GPL-2.0-or-later; see COPYING.

The emulator Handler, mixer callbacks and save-state serialization are removed.
Backend includes are replaced with standard integer types and a branch hint.
Synthesis and table generation are unchanged. Initialization is serialized by
the caller. This code has no DOSBox-X process, SDL or operating-system dependency.

Source SHA-256:

- `src/hardware/dbopl.h`: `92aa49340928a672f2911430aab6becb2141eb22e7d3debc16a785eb67048bcd`
- `src/hardware/dbopl.cpp`: `c388c5dfd81da3e9f22ed3f40dce23e00b171cfa9ed43e2e92e78bd47fe050ae`
- `COPYING`: `8674f5aabf4dd033831e7148a39184d8579e50c385da61a7d8bac6a5dd11c6da`
