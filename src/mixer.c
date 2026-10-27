// SDL_mixer 1.2 shims: sound chunks (WAV through SDL_RWops) mixed in an SDL2 audio callback, music (MikMod
// .uni modules, loaded from a file path) rendered by libmikmod's software mixer into the same callback.
#include "host.h"
#include "netplay.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#define MIKMOD_STATIC 1
#include "mikmod.h"

#define CRET(v) do { R.eax = (uint32_t)(v); R.esp += 4; return; } while (0)
#define CARG(n) R32(R.esp + 4 + 4 * (n))

FILE *sdl12_rw_host_file(uint32_t ctx);
void sdl12_rw_free(uint32_t ctx);

static SDL_AudioDeviceID dev;
static int out_rate = 22050;
typedef struct { int16_t *pcm; int frames; int vol; int used; } Chunk;     // stereo frames at out_rate
#define MAXCHUNK 512
static Chunk chunks[MAXCHUNK];
typedef struct { int chunk; int pos; int loops; int active; int vol; } Chan;
static Chan chans[64];
static int nchans = 8, reserved;
static int music_on, music_vol = 128, mikmod_ok;
static MODULE *music;
static uint32_t music_handle;

static void audio_cb(void *ud, Uint8 *stream, int len) {
    (void)ud;
    int16_t *out = (int16_t *)stream;
    int frames = len / 4;
    if (music && music_on && Player_Active()) VC_WriteBytes((SBYTE *)stream, len);
    else memset(stream, 0, len);
    for (int c = 0; c < nchans; c++) {
        Chan *ch = &chans[c];
        if (!ch->active) continue;
        Chunk *k = &chunks[ch->chunk];
        if (!k->used || !k->pcm) { ch->active = 0; continue; }
        int vol = k->vol * ch->vol / 128;
        for (int i = 0; i < frames; i++) {
            if (ch->pos >= k->frames) {
                if (ch->loops == 0) { ch->active = 0; break; }
                if (ch->loops > 0) ch->loops--;
                ch->pos = 0;
            }
            int l = out[i * 2] + k->pcm[ch->pos * 2] * vol / 128;
            int r = out[i * 2 + 1] + k->pcm[ch->pos * 2 + 1] * vol / 128;
            out[i * 2] = (int16_t)SDL_clamp(l, -32768, 32767);
            out[i * 2 + 1] = (int16_t)SDL_clamp(r, -32768, 32767);
            ch->pos++;
        }
    }
}
static void lock(void) { if (dev) SDL_LockAudioDevice(dev); }
static void unlock(void) { if (dev) SDL_UnlockAudioDevice(dev); }

static void m_OpenAudio(void) {
    int freq = (int)CARG(0), chans_out = (int)CARG(2);
    host_log("Mix_OpenAudio(%d, %x, %d, %d)\n", freq, CARG(1), chans_out, CARG(3));
    if (getenv("PORT_NOSOUND")) CRET(0);
    SDL_InitSubSystem(SDL_INIT_AUDIO);
    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof want);
    want.freq = freq; want.format = AUDIO_S16LSB; want.channels = 2;
#ifdef __EMSCRIPTEN__
    want.samples = 4096;
#else
    want.samples = 1024;
#endif
    want.callback = audio_cb;
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!dev) { host_log("audio: %s\n", SDL_GetError()); CRET(0); }
    out_rate = have.freq;
    MikMod_RegisterDriver(&drv_nos);
    MikMod_RegisterLoader(&load_uni);
    md_mixfreq = (UWORD)out_rate;
    md_mode = DMODE_16BITS | DMODE_STEREO | DMODE_SOFT_MUSIC | DMODE_INTERP;
    md_musicvolume = 128; md_sndfxvolume = 128;
    if (MikMod_Init("")) host_log("MikMod_Init: %s\n", MikMod_strerror(MikMod_errno));
    else mikmod_ok = 1;
    SDL_PauseAudioDevice(dev, 0);
    CRET(0);
}
static void m_CloseAudio(void) { CRET(0); }
static void m_QuerySpec(void) {
    if (CARG(0)) W32(CARG(0), out_rate);
    if (CARG(1)) W16(CARG(1), 0x8010);
    if (CARG(2)) W32(CARG(2), 2);
    CRET(dev ? 1 : 0);
}
static uint32_t linked_ver;
static void m_Linked_Version(void) {
    if (!linked_ver) { linked_ver = guest_static_alloc(4); W8(linked_ver, 1); W8(linked_ver + 1, 2); W8(linked_ver + 2, 7); }
    CRET(linked_ver);
}
static void m_AllocateChannels(void) { int n = (int)CARG(0); if (n >= 0) { lock(); nchans = n > 64 ? 64 : n; unlock(); } CRET(nchans); }
static void m_ReserveChannels(void) { reserved = (int)CARG(0); CRET(reserved); }

// WAV from RWops: decode with SDL2 and convert to S16 stereo at the device rate
static void m_LoadWAV_RW(void) {
    uint32_t rw = CARG(0), freesrc = CARG(1);
    FILE *f = sdl12_rw_host_file(rw);
    uint32_t handle = 0;
    if (f) {
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t *buf = malloc(n);
        if (fread(buf, 1, n, f) == (size_t)n) {
            SDL_AudioSpec spec; Uint8 *data; Uint32 dlen;
            if (SDL_LoadWAV_RW(SDL_RWFromConstMem(buf, (int)n), 1, &spec, &data, &dlen)) {
                SDL_AudioCVT cvt;
                SDL_BuildAudioCVT(&cvt, spec.format, spec.channels, spec.freq, AUDIO_S16LSB, 2, out_rate);
                cvt.len = (int)dlen;
                cvt.buf = malloc((size_t)dlen * (cvt.len_mult > 0 ? cvt.len_mult : 1) + 16);
                memcpy(cvt.buf, data, dlen);
                if (cvt.needed) SDL_ConvertAudio(&cvt); else cvt.len_cvt = (int)dlen;
                SDL_FreeWAV(data);
                for (int i = 1; i < MAXCHUNK; i++) if (!chunks[i].used) {
                    lock();
                    chunks[i].used = 1; chunks[i].pcm = (int16_t *)cvt.buf; chunks[i].frames = cvt.len_cvt / 4; chunks[i].vol = 128;
                    unlock();
                    handle = heap_alloc(16);
                    W32(handle, 1); W32(handle + 4, 0); W32(handle + 8, cvt.len_cvt); W8(handle + 12, 128);
                    W32(handle + 8, i | 0x4D490000u);     // alen is not read by the game; it carries our chunk slot
                    break;
                }
                if (!handle) free(cvt.buf);
            } else host_log("Mix_LoadWAV_RW: %s\n", SDL_GetError());
        }
        free(buf);
    }
    if (freesrc) sdl12_rw_free(rw);
    CRET(handle);
}
static Chunk *chunk_of(uint32_t h, int *slot) {
    if (!h) return NULL;
    uint32_t v = R32(h + 8);
    if ((v >> 16) != 0x4D49) return NULL;
    int i = v & 0xffff;
    if (i <= 0 || i >= MAXCHUNK || !chunks[i].used) return NULL;
    if (slot) *slot = i;
    return &chunks[i];
}
static void m_VolumeChunk(void) {
    Chunk *k = chunk_of(CARG(0), NULL);
    int v = (int32_t)CARG(1), old = k ? k->vol : 0;
    if (k && v >= 0) k->vol = v > 128 ? 128 : v;
    CRET(old);
}
static void m_PlayChannelTimed(void) {
    int c = (int32_t)CARG(0), slot = 0, loops = (int32_t)CARG(2);
    Chunk *k = chunk_of(CARG(1), &slot);
    if (!k || !dev || net_skip_render) CRET(0xffffffffu);
    lock();
    if (c < 0) {
        for (c = reserved; c < nchans; c++) if (!chans[c].active) break;
        if (c >= nchans) { unlock(); CRET(0xffffffffu); }
    }
    if (c >= nchans) { unlock(); CRET(0xffffffffu); }
    chans[c].chunk = slot; chans[c].pos = 0; chans[c].loops = loops; chans[c].active = 1; chans[c].vol = 128;
    unlock();
    CRET(c);
}
static void m_HaltChannel(void) {
    int c = (int32_t)CARG(0);
    lock();
    for (int i = 0; i < 64; i++) if (c < 0 || i == c) chans[i].active = 0;
    unlock();
    CRET(0);
}

// music: libmikmod reads the file itself
static void m_LoadMUS(void) {
    const char *name = GSTR(CARG(0));
    char path[1024];
    host_log("Mix_LoadMUS(%s)\n", name);
    if (!mikmod_ok) CRET(0);
    if (!fs_resolve(name, path, sizeof path, 0)) snprintf(path, sizeof path, "%s", name);
    FILE *f = fopen(path, "rb");
    if (!f) { host_log("Mix_LoadMUS: cannot open %s\n", path); CRET(0); }
    lock();
    MODULE *m = Player_LoadFP(f, 64, 0);
    unlock();
    fclose(f);
    if (!m) { host_log("Mix_LoadMUS: %s\n", MikMod_strerror(MikMod_errno)); CRET(0); }
    lock();
    if (music) { Player_Stop(); Player_Free(music); }
    music = m;
    unlock();
    if (!music_handle) music_handle = guest_static_alloc(16);
    CRET(music_handle);
}
static void m_PlayMusic(void) {
    if (!music || CARG(0) != music_handle) CRET(0xffffffffu);
    lock();
    music->wrap = (int32_t)CARG(1) < 0 || CARG(1) > 1;
    music->loop = 1;
    Player_Start(music);
    Player_SetVolume((SWORD)music_vol);
    music_on = 1;
    unlock();
    CRET(0);
}
static void m_VolumeMusic(void) {
    int v = (int32_t)CARG(0), old = music_vol;
    if (v >= 0) { music_vol = v > 128 ? 128 : v; lock(); if (music) Player_SetVolume((SWORD)music_vol); unlock(); }
    CRET(old);
}
static void m_HaltMusic(void) { lock(); if (music) Player_Stop(); music_on = 0; unlock(); CRET(0); }
static void m_FreeMusic(void) {
    lock();
    if (music) { Player_Stop(); Player_Free(music); music = NULL; }
    music_on = 0;
    unlock();
    CRET(0);
}
static void m_PlayingMusic(void) {
    lock();
    int on = music && music_on && Player_Active();
    unlock();
    CRET(on);
}

void audio_shutdown(void) { if (dev) { SDL_CloseAudioDevice(dev); dev = 0; } }

const ShimDef shims_mixer[] = {
    {"SDL_mixer", "Mix_OpenAudio", m_OpenAudio}, {"SDL_mixer", "Mix_CloseAudio", m_CloseAudio},
    {"SDL_mixer", "Mix_QuerySpec", m_QuerySpec}, {"SDL_mixer", "Mix_Linked_Version", m_Linked_Version},
    {"SDL_mixer", "Mix_AllocateChannels", m_AllocateChannels}, {"SDL_mixer", "Mix_ReserveChannels", m_ReserveChannels},
    {"SDL_mixer", "Mix_LoadWAV_RW", m_LoadWAV_RW}, {"SDL_mixer", "Mix_VolumeChunk", m_VolumeChunk},
    {"SDL_mixer", "Mix_PlayChannelTimed", m_PlayChannelTimed}, {"SDL_mixer", "Mix_HaltChannel", m_HaltChannel},
    {"SDL_mixer", "Mix_LoadMUS", m_LoadMUS}, {"SDL_mixer", "Mix_PlayMusic", m_PlayMusic},
    {"SDL_mixer", "Mix_VolumeMusic", m_VolumeMusic}, {"SDL_mixer", "Mix_HaltMusic", m_HaltMusic},
    {"SDL_mixer", "Mix_FreeMusic", m_FreeMusic}, {"SDL_mixer", "Mix_PlayingMusic", m_PlayingMusic},
    {0, 0, 0}
};
