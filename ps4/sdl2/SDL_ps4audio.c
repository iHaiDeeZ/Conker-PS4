/*
  PS4 audio driver for SDL2 (Conker's Bad Fur Day: Recompiled for PS4).
  Plays through sceAudioOut's main port at 48 kHz, 16-bit stereo: SDL converts the game's
  rate and format to that. sceAudioOutOutput blocks until the port takes the buffer, which
  paces SDL's audio thread, as the Vita driver this follows does.
*/
#include "../../SDL_internal.h"

#ifdef SDL_AUDIO_DRIVER_PS4

#include <stdint.h>

#include "SDL_audio.h"
#include "../SDL_audio_c.h"
#include "../SDL_sysaudio.h"

int32_t sceAudioOutInit(void);
int32_t sceAudioOutOpen(int32_t user_id, int32_t type, int32_t index, uint32_t length, uint32_t sample_rate, uint32_t param);
int32_t sceAudioOutClose(int32_t handle);
int32_t sceAudioOutOutput(int32_t handle, const void *buffer);

#define _THIS SDL_AudioDevice *this
#define PS4AUD_BUFFERS 2
#define PS4AUD_USER_SYSTEM 0xFF
#define PS4AUD_PORT_MAIN 0
#define PS4AUD_S16_STEREO 1
/* the port's granularity: a multiple of 256 frames */
#define PS4AUD_FRAMES 256

struct SDL_PrivateAudioData
{
    int32_t port;
    Uint8 *rawbuf;
    Uint8 *mixbufs[PS4AUD_BUFFERS];
    int next_buffer;
};

static int PS4AUD_OpenDevice(_THIS, const char *devname)
{
    int i;
    this->hidden = (struct SDL_PrivateAudioData *)SDL_calloc(1, sizeof(*this->hidden));
    if (!this->hidden) {
        return SDL_OutOfMemory();
    }
    this->hidden->port = -1;
    if (this->iscapture) {
        return SDL_SetError("No audio capture on the PS4");
    }
    this->spec.format = AUDIO_S16LSB;
    this->spec.channels = 2;
    this->spec.freq = 48000;
    this->spec.samples = PS4AUD_FRAMES;
    SDL_CalculateAudioSpec(&this->spec);

    this->hidden->rawbuf = (Uint8 *)SDL_malloc(this->spec.size * PS4AUD_BUFFERS);
    if (!this->hidden->rawbuf) {
        return SDL_OutOfMemory();
    }
    SDL_memset(this->hidden->rawbuf, 0, this->spec.size * PS4AUD_BUFFERS);
    for (i = 0; i < PS4AUD_BUFFERS; i++) {
        this->hidden->mixbufs[i] = &this->hidden->rawbuf[i * this->spec.size];
    }

    this->hidden->port = sceAudioOutOpen(PS4AUD_USER_SYSTEM, PS4AUD_PORT_MAIN, 0, PS4AUD_FRAMES, 48000, PS4AUD_S16_STEREO);
    if (this->hidden->port < 0) {
        return SDL_SetError("sceAudioOutOpen failed: %08x", (unsigned)this->hidden->port);
    }
    this->hidden->next_buffer = 0;
    return 0;
}

static void PS4AUD_PlayDevice(_THIS)
{
    sceAudioOutOutput(this->hidden->port, this->hidden->mixbufs[this->hidden->next_buffer]);
    this->hidden->next_buffer = (this->hidden->next_buffer + 1) % PS4AUD_BUFFERS;
}

static void PS4AUD_WaitDevice(_THIS)
{
    /* sceAudioOutOutput blocks: nothing to wait for here */
}

static Uint8 *PS4AUD_GetDeviceBuf(_THIS)
{
    return this->hidden->mixbufs[this->hidden->next_buffer];
}

static void PS4AUD_CloseDevice(_THIS)
{
    if (this->hidden->port >= 0) {
        /* lets the last buffer play out before the port goes */
        sceAudioOutOutput(this->hidden->port, NULL);
        sceAudioOutClose(this->hidden->port);
        this->hidden->port = -1;
    }
    SDL_free(this->hidden->rawbuf);
    SDL_free(this->hidden);
}

static SDL_bool PS4AUD_Init(SDL_AudioDriverImpl *impl)
{
    /* already initialised is fine (0x8026000E) */
    sceAudioOutInit();
    impl->OpenDevice = PS4AUD_OpenDevice;
    impl->PlayDevice = PS4AUD_PlayDevice;
    impl->WaitDevice = PS4AUD_WaitDevice;
    impl->GetDeviceBuf = PS4AUD_GetDeviceBuf;
    impl->CloseDevice = PS4AUD_CloseDevice;
    impl->OnlyHasDefaultOutputDevice = SDL_TRUE;
    return SDL_TRUE;
}

AudioBootStrap PS4AUD_bootstrap = {
    "ps4", "PS4 sceAudioOut", PS4AUD_Init, SDL_FALSE
};

#endif /* SDL_AUDIO_DRIVER_PS4 */
