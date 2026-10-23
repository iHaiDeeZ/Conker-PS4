// Conker's Bad Fur Day: Recompiled for PS4: the console-specific services the host, the runtime
// and RecompFrontend call (ps4_platform.cpp, ps4_pad.cpp).
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// The app's folder: /data/Conker (port/ps4_crashlog.cpp)
const char *ps4_data_dir(void);
// The log, the user service: first thing in main()
void ps4_platform_init(void);

// Game memory. The PS4 can't reserve gigabytes with mmap(PROT_NONE) (anonymous memory is
// flexible memory, a few hundred MiB): the runtime's 4 GiB of guest space is a virtual range
// reservation, and the parts in use are backed by direct memory, zeroed, 2 MiB at a time.
void *ps4_vm_reserve(size_t size);
// Backs [address, address + size) inside the reservation (backed parts keep their data). 0 on success.
int ps4_vm_commit(void *address, size_t size);

// DualShock 4s as SDL game controllers (virtual joysticks fed from ScePad), after SDL_Init.
void ps4_pad_init(void);

#ifdef __cplusplus
}
#endif
