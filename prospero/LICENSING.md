# Licences of the PS5 port's files

Each source file names its licence in its header.

| Path | Licence | From |
| --- | --- | --- |
| `sdl/`, `patches/sdl.patch` | Zlib, as SDL | griches' PS5_DevilutionX (`4dc8074`) |
| `src/platform.c`, `src/platform.h` | MIT | Mihawk's PS5 Vulkan Template, by way of PS5_DevilutionX |
| `src/shims.c` | MIT | griches' PS5_Ghostship |
| `stubs/`, `tools/cc-init-array.sh`, `tools/link-title.sh`, `tools/setup-sdl.sh`, `cmake/`, `src/startup.c` | MIT | adapted from PS5_DevilutionX |
| The rest | MIT | this project |

A built title links GPL-3.0 parts of the PS5 stack (`PS5_Vulkan`'s platform layer), so griches reads a built title as GPL-3.0. That is their reading, not a legal review.
