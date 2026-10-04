/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2022 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "../../SDL_internal.h"

/* Output audio to nowhere... */

#include "SDL_timer.h"
#include "SDL_audio.h"
#include "../SDL_audio_c.h"
#include "SDL_dummyaudio.h"

#if defined(_WIN32)
/* timeBeginPeriod: raise the OS timer resolution to 1 ms while the dummy device
 * is open. WaitDevice sleeps with SDL_Delay, which on Windows is an OS timer
 * wait whose granularity is otherwise ~15.6 ms — coarser than one 21.33 ms
 * quantum — so each sleep overshot by a whole tick, the deadline was missed,
 * and the next callback fired with no wait at all: the mixer produced its
 * quanta in bursts of two, which is what made the captured audio jitter and
 * RetroArch's ring oscillate empty<->full. At 1 ms resolution SDL_Delay is
 * accurate and the pace holds.
 *
 * Declared by hand rather than via <mmsystem.h>: that header pulls in mciapi.h,
 * whose macros clash with the SDL internal headers included above. On x64
 * Windows there is a single calling convention, so no WINAPI specifier is
 * needed and the name links straight to winmm. */
unsigned int timeBeginPeriod(unsigned int);
unsigned int timeEndPeriod(unsigned int);
#define DUMMYAUDIO_LOCK_TIMER_RESOLUTION 1
#endif

/* LIBRETRO LOCAL CHANGE ------------------------------------------------------
 *
 * Upstream's dummy *output* has no device functions at all: SDL therefore
 * takes its work-buffer branch and paces the callback with a bare
 * SDL_Delay(samples/freq). For a normal app that is harmless (the sound is
 * discarded), but this engine runs the mixer on the dummy device on purpose
 * and captures its output into the libretro frontend's ring — the callback is
 * the thing that *produces* the audio. A bare delay quantizes to the platform
 * timer tick (Windows ~15.6 ms), yielding ~33000 frames/sec where 48000 is
 * expected: the frontend then plays stretched, underrun audio.
 *
 * So the dummy output is made to behave like a real device — GetDeviceBuf
 * routes SDL into the GetDeviceBuf -> callback -> PlayDevice -> WaitDevice
 * path, and WaitDevice paces against a monotonic deadline — while still
 * discarding the samples. CaptureFromDevice is upstream's.
 * -------------------------------------------------------------------------- */

static int
DUMMYAUDIO_OpenDevice(_THIS, const char *devname)
{
    /* SDL leaves this->hidden NULL; each driver allocates its own
     * SDL_PrivateAudioData (the struct this header declares). */
    _this->hidden = (struct SDL_PrivateAudioData *)
        SDL_malloc(sizeof(struct SDL_PrivateAudioData));
    if (_this->hidden == NULL) {
        return SDL_OutOfMemory();
    }
    SDL_zerop(_this->hidden);

    _this->hidden->mixbuf = (Uint8 *) SDL_malloc(_this->spec.size);
    if (_this->hidden->mixbuf == NULL) {
        return SDL_OutOfMemory();
    }
    _this->hidden->mixlen = _this->spec.size;
    _this->hidden->write_delay = (Uint32) (((Sint64) _this->spec.samples * 1000) / _this->spec.freq);
    _this->hidden->initial_calls = 0;   /* unused, kept for the header's shape */
    _this->hidden->next_deadline = 0;   /* set on the first WaitDevice */
    SDL_memset(_this->hidden->mixbuf, _this->spec.silence, _this->spec.size);
#if defined(DUMMYAUDIO_LOCK_TIMER_RESOLUTION)
    timeBeginPeriod(1);   /* accurate SDL_Delay while this device is open */
#endif
    return 0;
}

static void
DUMMYAUDIO_CloseDevice(_THIS)
{
#if defined(DUMMYAUDIO_LOCK_TIMER_RESOLUTION)
    timeEndPeriod(1);
#endif
    if (_this->hidden) {
        SDL_free(_this->hidden->mixbuf);
        _this->hidden->mixbuf = NULL;
        SDL_free(_this->hidden);
        _this->hidden = NULL;
    }
}

/* A real buffer is what routes SDL's audio thread into the branch that calls
 * our WaitDevice (GetDeviceBuf -> callback -> PlayDevice -> WaitDevice). */
static Uint8 *
DUMMYAUDIO_GetDeviceBuf(_THIS)
{
    return _this->hidden->mixbuf;
}

/* Pace the callback at real time like a real device: sleep until the next
 * buffer boundary using SDL's high-resolution performance counter.
 *
 * SDL_Delay alone cannot do this on Windows: it resolves to the OS timer,
 * whose granularity ~15.6 ms is coarser than one quantum (21.33 ms at 1024
 * frames/48 kHz), so each sleep overshot to a whole tick and production fell
 * far short of real time (measured ~24000 frames/sec where 48000 is expected).
 * Driving the wait from SDL_GetPerformanceCounter and spinning out the last
 * millisecond holds the pace regardless of the OS tick. */
static void
DUMMYAUDIO_WaitDevice(_THIS)
{
    struct SDL_PrivateAudioData *dev = _this->hidden;
    const Uint64 freq = SDL_GetPerformanceFrequency();
    const Uint64 step = (Uint64) (((double) _this->spec.samples / _this->spec.freq) * freq);
    Uint64 now;

    if (dev == NULL || step == 0) {
        return;
    }

    now = SDL_GetPerformanceCounter();
    if (dev->next_deadline == 0) {
        dev->next_deadline = now;
    }
    dev->next_deadline += step;

    if (now < dev->next_deadline) {
        const Uint64 remain = dev->next_deadline - now;
        const Uint64 spin = freq / 1000;   /* last 1 ms: spin, do not sleep */
        if (remain > spin) {
            SDL_Delay((Uint32) (((remain - spin) * 1000) / freq));
        }
        while (SDL_GetPerformanceCounter() < dev->next_deadline) {
            SDL_Delay(0);   /* yield; last <=1 ms only */
        }
    } else if (now - dev->next_deadline > freq / 4) {
        /* More than 250 ms behind (debugger, scheduling stall): do not try to
         * catch up by freewheeling; restart the schedule from now. */
        dev->next_deadline = now;
    }
}

static void
DUMMYAUDIO_PlayDevice(_THIS)
{
    /* Output to nowhere: the buffer just handed out by GetDeviceBuf is
     * discarded. */
}

static int
DUMMYAUDIO_CaptureFromDevice(_THIS, void *buffer, int buflen)
{
    /* Delay to make this sort of simulate real audio input. */
    SDL_Delay((_this->spec.samples * 1000) / _this->spec.freq);

    /* always return a full buffer of silence. */
    SDL_memset(buffer, _this->spec.silence, buflen);
    return buflen;
}

static SDL_bool
DUMMYAUDIO_Init(SDL_AudioDriverImpl * impl)
{
    /* Set the function pointers */
    impl->OpenDevice = DUMMYAUDIO_OpenDevice;
    impl->CloseDevice = DUMMYAUDIO_CloseDevice;
    impl->GetDeviceBuf = DUMMYAUDIO_GetDeviceBuf;
    impl->PlayDevice = DUMMYAUDIO_PlayDevice;
    impl->WaitDevice = DUMMYAUDIO_WaitDevice;
    impl->CaptureFromDevice = DUMMYAUDIO_CaptureFromDevice;

    impl->OnlyHasDefaultOutputDevice = SDL_TRUE;
    impl->OnlyHasDefaultCaptureDevice = SDL_TRUE;
    impl->HasCaptureSupport = SDL_TRUE;

    return SDL_TRUE;   /* this audio target is available. */
}

AudioBootStrap DUMMYAUDIO_bootstrap = {
    "dummy", "SDL dummy audio driver", DUMMYAUDIO_Init, SDL_TRUE
};

/* vi: set ts=4 sw=4 expandtab: */
