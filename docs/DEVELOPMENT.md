# Developer notes

How Conker for PS4 is built, what the port changes in CBFD-Recompiled and its tools, and the
diagnostics it has for tracking down slowdowns and freezes. For playing it, see the [README](../README.md).

## Diagnostics

All of them have a switch in the quick menu (L3 + R3) and write to `/data/Conker/`.

**Performance Log** (quick menu, on by default while the port is being tuned): every 5 s a `perf:`
line in `conker.log` with the game's frame rate, per-frame time of RT64's workload and present
threads, audio tasks, the UI, the VI tick (60 a second when the clock keeps up), shader compiles (and
how many wait), and CPU use of the 7 cores. With it an `rt64:` line splits RT64's frame (per frame
drawn, interpolated ones included): draws and framebuffer pairs, then wall and CPU time of the RSP
setup, the framebuffer setup (targets, texture cache), recording the command list, submitting and
waiting for the GPU, and the GPU's own time from RT64's timestamps.

**Freeze watchdog** (with the Performance Log): when the VI tick or the game's frames stop for 10 s,
every thread's place and the eboot addresses on its stack go to `freeze.log`, twice, 3 s apart (the
previous run's is `freeze.prev.log`). It allocates and logs nothing, so a stuck heap or log can't stop it.

**Profiler** (quick menu, off by default): `profile.log`, every 10 s, for each thread that ran: how
often it was running and its busiest code. Running threads are looked at 50 times a second, waiting
ones once a second, through the hang dump's signal. A thread waiting in a system call that the
signal restarts shows as "running" at the call (libkernel 0x800003710 in v00.09's profile).
Symbolize `elf 0x...` lines with `llvm-symbolizer --obj=~/cbfd-build/oelf/vXX.YY.elf`.

## How it's built

You need the PS4 toolchain and sysroot of [HENkaku-PS4](https://github.com/iHaiDeeZ/HENkaku-PS4)
(PacBrew's OpenOrbis, a clang 21 + libc++ 21 sysroot and its RADV Mesa build) in a build root,
`~/cbfd-build` by default (`PS4_BUILD_ROOT`), and this repository at a path without spaces
(`~/conker`).

1. Clone [CBFD-Recompiled](https://github.com/sciaschi/CBFD-Recompiled) with its submodules into
   `~/cbfd-build/src/CBFD-Recompiled`, at the revisions in `patches/BASE.txt`, and apply its own tool
   patches.
2. Put your US ROM at `conker/baserom.us.z64` in that tree and run `python3 recomp/recompile.py` (see
   CBFD-Recompiled's README): this makes the recompiled game code.
3. Apply `patches/*-ps4.patch` to each checkout (`cbfd` to the main tree, the others to the tools of
   the same name, `sdl2` to SDL 2.32.10 cloned into `~/cbfd-build/src/SDL2`; FreeType 2.13.3 goes in
   `~/cbfd-build/src/freetype`).
4. Build and package:

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

## What the port changes

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

## Fixed along the way

- Freezes after 85-340 s (v00.11): the heap's lock was dlmalloc's spin lock, which spins and yields,
  and the PS4 only yields to threads of the same or higher priority. When a low-priority thread held
  it (RT64's shader compilers run at 767 on cores 4-6), the threads waiting for it kept it from ever
  running again. The lock is a mutex now, and a freeze watchdog (below) dumps every thread if it
  happens again.
- 0.4 fps (v00.09): the save states' wait signature (a native backtrace at every game thread wait)
  stepped past the thread's first frame into system code, and libunwind then searched all 1.9 MB of
  `.eh_frame` each time. Backtraces now stop at the thread entry function (`ps4_thread_entry`, never
  tail-called).

## Not yet

- Mods (`.nrm`): the live recompiler's executable memory and patching the eboot's code are untested.
