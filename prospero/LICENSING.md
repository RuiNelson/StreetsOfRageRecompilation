# Licences of the PS5 port's files

Each source file names its licence in its header.

| Path | Licence | From |
| --- | --- | --- |
| `sdl/`, `patches/sdl.patch` | Zlib, as SDL | griches' PS5_DevilutionX (`4dc8074`) |
| `src/platform.c`, `src/platform.h` | MIT | Mihawk's PS5 Vulkan Template, by way of PS5_DevilutionX |
| `src/shims.c` | MIT | griches' PS5_Ghostship |
| `stubs/`, `tools/cc-init-array.sh`, `tools/link-title.sh`, `tools/setup-sdl.sh`, `cmake/`, `src/startup.c` | MIT | adapted from PS5_DevilutionX |
| `prospero_icon.png`, `sce_sys/icon0.png` | the title's icon, supplied by the project's author | this project |
| The rest | MIT | this project |

## The built title is GPL-3.0

A PS5 title built from this tree, and its ZIPs, are distributed under the **GNU GPL, version 3** (user decision). The full text is [`COPYING-GPL-3.0`](COPYING-GPL-3.0). The source files above keep their own licences (MIT, Zlib); the combined binary is GPL-3.0.

Why, by component of the PS5 stack (licences checked on GitHub and in the source headers):

| Component | Licence | Effect |
| --- | --- | --- |
| `mihawk-99/PS5_Vulkan` (platform layer, CRT, fake SELF), linked into the title | GPL-3.0 | requires the combined binary to be GPL-3.0 |
| `mihawk-99/PS5_PayloadSDK` (the SDK the title is built with) | GPL-3.0 | same |
| RADV, from `mihawk-99/PS5_Mesa` | MIT (Mesa) | none; RADV is **not** GPL |
| SDL, with griches' console drivers | Zlib | none |

The desktop builds are unaffected and stay MIT, as the root `LICENSE`.

Anyone given a built title is owed its source: this repository, `MegaDriveEnvironment` and the pinned revisions in `docker/Dockerfile` are that source. This is a reading of the licences, not a legal review.
