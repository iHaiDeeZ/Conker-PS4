# Credits

Conker for PS4 is a port. Nearly everything that makes the game run is other people's work.

## The game's recompilation

- **[CBFD-Recompiled](https://github.com/sciaschi/CBFD-Recompiled)** by **Sean Ciaschi** (MIT): the
  recompilation of Conker's Bad Fur Day this port builds, starting from the
  [Conker decompilation](https://github.com/mkst/conker). `patches/cbfd-ps4.patch` applies to the
  commit in `patches/BASE.txt`.
- **[N64Recomp](https://github.com/N64Recomp/N64Recomp)** and **[N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime)**
  by **Wiseguy** and contributors (MIT and GPL-3.0): the static recompiler and the runtime (librecomp,
  ultramodern; `patches/n64modernruntime-ps4.patch` is under the runtime's GPL-3.0),
  with [sljit](https://github.com/zherczeg/sljit) (BSD-2-Clause) for mods.
- **[RT64](https://github.com/rt64/rt64)** (MIT) and its **plume** graphics layer: the renderer.
- **[RecompFrontend](https://github.com/N64Recomp/RecompFrontend)**: the launcher, settings, controls and mods menus.

## The PS4 side

- **[Mesa](https://mesa3d.org)** (MIT): the RADV Vulkan driver, ported to the PS4's GPU in
  [Crema](https://github.com/iHaiDeeZ/Crema). `port/mesa/ac_ps4_drm.c` is that port's winsys.
- **[SDL2](https://libsdl.org)** (zlib) and **[FreeType](https://freetype.org)** (FTL): built into the
  sysroot by `scripts/build-deps.sh`; `ps4/sdl2/SDL_ps4audio.c` is the port's audio driver.
- **[dlmalloc](https://gee.cs.oswego.edu/dl/html/malloc.html)** by Doug Lea (public domain): the heap.
- **[OpenOrbis](https://github.com/OpenOrbis)** (GPL-3.0 toolchain; its libraries under their own terms):
  the PS4 toolchain.
- **[HENkaku-PS4](https://github.com/iHaiDeeZ/HENkaku-PS4)** and **[G-Diffuser-PS4](https://github.com/iHaiDeeZ/G-Diffuser-PS4)**
  by ShiroKlein: the PS4 runtime in `port/`, the toolchain, the VideoOut WSI (`ps4/ps4_wsi.cpp`) and
  the pipeline cache idea. The quick menu follows [Crema](https://github.com/iHaiDeeZ/Crema)'s in-game menu.

## The game

Conker's Bad Fur Day is Rare's. The icon and background are made by `scripts/make-art.py` from the
game's box art and a screenshot (`assets/`). This project is not affiliated with Rare, Nintendo or
Microsoft, and contains none of the game's code or data.
