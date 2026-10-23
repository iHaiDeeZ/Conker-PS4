# Conker's Bad Fur Day: Recompiled for PS4

[CBFD-Recompiled](https://github.com/sciaschi/CBFD-Recompiled) (Conker's Bad Fur Day, statically
recompiled with N64Recomp, running on N64ModernRuntime and drawn by RT64) as an app for jailbroken
PS4 consoles. The game's code runs natively on the PS4's CPU and RT64 renders with **Vulkan** on the
PS4's GPU (Mesa's RADV on GNM, the same driver as HENkaku and G-Diffuser), presenting through VideoOut.

Status: **v00.05 on the PS4 at 192.168.0.90, not yet tested there** (v00.03: launcher and pad work;
Load ROM showed instead of Start Game, fixed in v00.05).

## Using it

1. Install `out/Conker-vXX.YY.pkg` (title ID `CBFD00064`) and start it.
2. Pick **Start Game**. The launcher keeps RecompFrontend's **Controls**, **Settings** and **Mods**.

The package contains the game code recompiled from your ROM and the ROM itself (`/app0/game`, the game
reads its data from it; stored in `/data/Conker` on first start): it's for your own console only.

Everything the app writes is in `/data/Conker/`: saves, settings, `conker.log` (this run, with
stdout/stderr of the runtime, RT64 and the host), `conker.prev.log`, `boot-trace.log` and `crash.log`.

Controls: the DualShock 4 is an SDL game controller (Cross = A, Circle = B, Square = X, Triangle = Y,
OPTIONS = Start); RecompFrontend's control remapping works as on PC.

**Quick menu**: click the touch pad (or press L3 + R3) in game: Resume, Save State / Load State (slot
1-9), Next Slot, Settings (RecompFrontend's settings window) and Quit Game. Circle or the touch pad
closes it. The game keeps running behind it (the runtime has no pause) but gets no input.

## How it's built

WSL tree `~/cbfd-build` (seeded from HENkaku's `~/vita3k-build`: clang 21 + libc++ 21 sysroot, RADV
Mesa build), this folder linked as `~/conker`.

```
scripts/build-deps.sh                 # SDL2 2.32 (+ PS4 audio driver) and FreeType into the sysroot
scripts/configure-conker.sh           # host/ of CBFD-Recompiled with toolchain/ps4-conker.cmake
scripts/build-conker.sh               # ConkerRecomp + eboot.bin
CONKER_VERSION=00.01 scripts/package-conker.sh
scripts/upload-to-console.sh out/Conker-v00.01.pkg   # from Windows (Git Bash)
scripts/save-patch.sh                 # refresh patches/ from the build tree
```

The source tree is `~/cbfd-build/src/CBFD-Recompiled` with `python3 recomp/recompile.py` run on your
ROM and CBFD's own tool patches committed as each checkout's baseline; `patches/*-ps4.patch` are the
PS4 changes on top (revisions in `patches/BASE.txt`).

### What the port changes

- `ps4/ps4_wsi.cpp` (from HENkaku): `VK_EXT_headless_surface` + swapchain on VideoOut, linear images
  flipped GPU-side. plume creates a headless surface on PS4, loads RADV through
  `ps4_vk_get_instance_proc_addr`, and puts every queue type on the one graphics queue (the WSI
  drops semaphores, which lose the device on the PS4).
- SDL2 with the dummy video driver (1920x1080 display), a `ps4` sceAudioOut audio driver
  (`ps4/sdl2/SDL_ps4audio.c`, 48 kHz) and DualShock 4s as virtual game controllers fed from ScePad
  (`ps4/ps4_pad.cpp`, rumble and light bar too).
- Game memory: the runtime's 4 GiB space is a virtual range reservation, RDRAM and the TLB-mapped
  code segments backed by direct memory (`ps4/ps4_platform.cpp`).
- `ps4/ps4_nfd.cpp`: the file dialogs pick files from `/data/Conker` and USB drives.
- Paths: program files in `/app0`, app folder `/data/Conker` (recompui, RT64).
- Save states: backtraces through the unwinder, the eboot's `.text` as the image range.
- `port/` and `toolchain/`: HENkaku's PS4 runtime (heap, crash log, file shims) and toolchain.

### Not yet

- Mods (`.nrm`): the live recompiler's executable memory and patching the eboot's code are untested.
- The Vulkan pipeline cache isn't kept between runs (G-Diffuser does: first starts compile shaders).
