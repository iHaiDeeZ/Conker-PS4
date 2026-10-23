# Conker's Bad Fur Day: Recompiled for PS4

[CBFD-Recompiled](https://github.com/sciaschi/CBFD-Recompiled) (Conker's Bad Fur Day, statically
recompiled with N64Recomp, running on N64ModernRuntime and drawn by RT64) as an app for jailbroken
PS4 consoles. The game's code runs natively on the PS4's CPU and RT64 renders with **Vulkan** on the
PS4's GPU (Mesa's RADV on GNM, the same driver as HENkaku and G-Diffuser), presenting through VideoOut.

Status: **v00.09 on the PS4 at 192.168.0.90, not yet tested there**. v00.07's profile found the
slowness: the save states' wait signature (a native backtrace at every game thread wait) stepped past
the thread's first frame into system code, and libunwind then searched all 1.9 MB of `.eh_frame` each
time. v00.08's cut-off compared stack addresses and never matched; v00.09 stops at the thread entry
function itself (`ps4_thread_entry`, never tail-called) and logs a `backtrace check` at start.

## Using it

1. Install `out/Conker-vXX.YY.pkg` (title ID `CBFD00064`) and start it.
2. Pick **Start Game**. The launcher keeps RecompFrontend's **Controls**, **Settings** and **Mods**.

The package contains the game code recompiled from your ROM and the ROM itself (`/app0/game`, the game
reads its data from it; stored in `/data/Conker` on first start): it's for your own console only.

Everything the app writes is in `/data/Conker/`: saves, settings, `conker.log` (this run, with
stdout/stderr of the runtime, RT64 and the host), `conker.prev.log`, `boot-trace.log`, `crash.log`,
`pipelines.bin` (compiled GPU pipelines, kept between runs) and `ps4.ini` (the port's own switches).

Controls: the DualShock 4 is an SDL game controller (Cross = A, Circle = B, Square = X, Triangle = Y,
OPTIONS = Start); RecompFrontend's control remapping works as on PC.

**Quick menu** (Crema's in-game menu): click the touch pad (or press L3 + R3) in game. A panel slides
in from the left: Resume, Save State / Load State (Left/Right pick slot 1-9; a card shows whether the
slot is saved and when), Frame Rate (30 original / 60 interpolated), Anti-Aliasing, Resolution,
Volume, FPS Counter, Performance Log, All Settings (RecompFrontend's settings window) and Quit Game.
Up/Down pick, Left/Right change (applied and saved at once), Cross does it, Circle or the touch pad
closes it. The game keeps running behind it (the runtime has no pause) but gets no input.

On first start the PS4 defaults are applied once: anti-aliasing off and the game's own 30 fps (RT64's
interpolation to 60 draws every frame twice). Both can be changed back in the quick menu.

**Performance Log** (quick menu, on by default while the port is being tuned): every 5 s a `perf:`
line in `conker.log` with the game's frame rate, per-frame time of RT64's workload and present
threads, audio tasks, the UI, the VI tick (60 a second when the clock keeps up), shader compiles (and
how many wait), and CPU use of the 7 cores.

**Profiler** (quick menu, on by default for now): `profile.log`, every 10 s, for each thread that
ran: how often it was running and its busiest code. Running threads are looked at 50 times a second,
waiting ones once a second (through the hang dump's signal, never the 1 kHz sampler that froze SMW).
Symbolize `elf 0x...` lines with `llvm-symbolizer --obj=~/cbfd-build/oelf/vXX.YY.elf`.

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
- Threads (`ps4_thread_role`): all 7 cores; the game's threads, the timer and VI threads and RT64's
  workload and present threads above normal priority; RT64's shader compiles at the lowest priority on
  cores 4-6 only, so the ubershader carries frames while they compile (v00.05's 0.5 fps). RT64's idle
  workaround thread (a GPU job waited for every millisecond, for desktop power states) is off.
- plume keeps a `VkPipelineCache` in `/data/Conker/pipelines.bin` (G-Diffuser's idea): loaded at
  start, written every 20 s while new pipelines appear.
- `port/` and `toolchain/`: HENkaku's PS4 runtime (heap, crash log, file shims) and toolchain.

### Not yet

- Mods (`.nrm`): the live recompiler's executable memory and patching the eboot's code are untested.
