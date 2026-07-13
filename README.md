# StreetsOfRageRecompilation

Native C++ recompilation of Streets of Rage.

This repository contains the game-specific code and analysis data:

- `generated/Sor.hpp` and `generated/Sor.cpp`
- `code-analysis/`
- `rom/`
- build and discovery scripts

It expects sibling checkouts of:

- `../MegaDriveEnvironment`
- `../RageDecompiler`

Build:

```bash
./build.sh
```

Regenerate the recompiled C++ from the ROM, then build:

```bash
./build.sh --full
```

Run disassembly workflows:

```bash
./disassemble.sh
./disassemble_nolabels.sh
./discover_aux_fast.sh
```

Runtime cheats (hold Option/Alt while pressing the key):

- `P`: toggle 12x attack damage for player 1 (capped at the game's native maximum)
- `L`: add one life to player 1
- `S`: add one special attack to player 1
- `1`-`8`: load the corresponding level
