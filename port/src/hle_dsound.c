/**
 * DirectSound (XDK 5659) on a host software mixer, in place of the emulated
 * MCPX APU.
 *
 * The recompiled XDK DirectSound drives the APU's registers and waits on its
 * interrupts for stream packets to complete. The emulated APU does not keep
 * real time, so XMV playback starved its audio stream and the intro stalled.
 * Here the public DirectSound entry points are implemented on the host:
 * buffers and stream packets are read straight from guest memory, mixed on a
 * mixer thread paced by XAudio2, and stream packets complete as they play.
 *
 * Logic ported from SCDA-XBERecomp's src/xdk/dsound.cpp (MIT, Copyright (c)
 * 2026 TGP482, Joshhhuaaa), which targets XDK 5849, by way of the HP: Chamber of
 * Secrets port (XDK 5028). The 5659 entry points and their argument counts were
 * checked against this XBE: addresses from
 * XbSymbolDatabase, argument counts from each function's `ret n`.
 *
 * Every entry point keeps the generated XDK body as sub_X_gen, so
 * RECOMP_HLE_DSOUND=0 runs the original DirectSound on the emulated APU.
 * RECOMP_DSOUND_TRACE=1 logs the calls.
 *
 * Built into the recompile with --exclude-manual pointing at this file.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define COBJMACROS
#include <windows.h>
#include <xaudio2.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the register names (eax, esp, ...) that RECOMP_ICALL expands to */
#define RECOMP_GENERATED_CODE
#include "recomp/gen/recomp_types.h"

#ifdef _MSC_VER
#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")
#endif

uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);
void xbox_HeapFree(uint32_t xbox_va);

/* ── Guest addresses (XDK 5659 DSOUND in this XBE) ───────────────────────── */

#define STREAM_VTABLE   0x0020F098u  /* CDirectSoundStream, XMediaObject part (AddRef 0x1A6696 first) */
#define STREAM_VTABLE2  0x0020F08Cu  /* its second base, at object + 4 */

#define S_OK_           0x00000000u
#define E_FAIL_         0x80004005u
#define DSERR_INVALID   0x88780032u
#define XMEDIAPACKET_STATUS_PENDING  0x8000000Au
#define XMEDIAPACKET_STATUS_FLUSHED  0x80004004u

/* ── Arguments and returns, __stdcall on the guest stack ─────────────────── */

#define ARG(i)  MEM32(g_esp + 4 + 4 * (i))

static void ret_stdcall(uint32_t result, int nargs)
{
    g_eax = result;
    g_esp += 4 + 4 * nargs;
}

static int g_hle = -1, g_trace;

static int hle_on(void)
{
    if (g_hle < 0) {
        const char *e = getenv("RECOMP_HLE_DSOUND");
        const char *t = getenv("RECOMP_DSOUND_TRACE");
        g_hle = !(e && *e == '0');
        g_trace = t && *t && *t != '0';
        fprintf(stderr, "[DSOUND] %s\n", g_hle ? "host mixer (HLE)" : "XDK DirectSound on the emulated APU");
    }
    return g_hle;
}

#define TRACE(...) do { if (g_trace) { fprintf(stderr, "[DSOUND] " __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* ── Voices ─────────────────────────────────────────────────────────────── */

#define MIX_RATE        48000
/* Mixed per pass, ~5.3 ms. The music streams through a ~50 ms looping buffer that the title refills by polling
 * GetCurrentPosition; the console reads it sample by sample. 1024 frames (~21 ms) a pass moved the cursor and read
 * ahead in jumps that ate half of the title's refill margin, and a slow game frame then let the mixer lap the
 * writer: stale audio, heard as clicks. */
#define MIX_FRAMES      256
#define MAX_VOICES      256
#define MAX_PACKETS     64
#define MAX_DONE        1024
#define ADPCM_FRAMES    64   /* Xbox ADPCM plays 64 frames per 36-byte block, not IMA's 65 */

typedef struct {
    uint32_t data, size, completed_ptr, status_ptr, context;
} Packet;

typedef struct {
    uint32_t obj;
    int stream, refs;
    /* format */
    uint16_t tag, channels, bits, align;
    uint32_t rate, freq;
    /* buffer */
    uint32_t data, bytes, loop_start, loop_len, owned;
    int playing, looping, paused, active, ending;
    double pos;  /* frames */
    /* stream */
    uint32_t max_packets, callback, context;
    Packet packets[MAX_PACKETS];
    int npackets;
    /* levels */
    float volume, headroom;
    uint8_t nbins, bin[8];
    int32_t bin_mb[32];
    float gain[2][2];
    float gain6[6][6];  /* 5.1 output (render6): source channel -> speaker */
    int has_gain;
    int fading_out;  /* Stop: one more pass ramping to silence, then stopped */
    /* 3D (DSBCAPS_CTRL3D): position relative to the listener, which this title never moves */
    int is3d;
    float pos3d[3], min_dist, max_dist;
    /* ADPCM block cache */
    int64_t block;
    int16_t decoded[65 * 8];
} Voice;

typedef struct {
    uint32_t voice, callback, context, packet_context, status;
} Done;

static CRITICAL_SECTION g_lock;
static Voice *g_voices[MAX_VOICES];
static Done g_done[MAX_DONE];
static int g_ndone;
static int g_started;
static uint32_t g_ds;

static float db(int32_t hundredths)
{
    return hundredths <= -10000 ? 0.0f : powf(10.0f, hundredths / 2000.0f);
}

static uint32_t frames_of(const Voice *v, uint32_t bytes)
{
    if (v->tag == 0x69) return bytes / (36u * v->channels) * ADPCM_FRAMES;
    return bytes / (v->align ? v->align : 1);
}

static uint32_t bytes_of(const Voice *v, uint32_t frames)
{
    if (v->tag == 0x69) return frames / ADPCM_FRAMES * 36 * v->channels;
    return frames * v->align;
}

static const int k_index[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
static const int k_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
    5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767};

static int clampi(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }

/* One 36-byte-per-channel IMA block: 65 frames decoded, ADPCM_FRAMES played. */
static void decode_block(const uint8_t *b, int ch, int16_t *out)
{
    for (int c = 0; c < ch; c++) {
        int pred = (int16_t)(b[c * 4] | b[c * 4 + 1] << 8), idx = b[c * 4 + 2] > 88 ? 88 : b[c * 4 + 2];
        const uint8_t *d = b + ch * 4;
        out[c] = (int16_t)pred;
        for (int k = 0; k < 8; k++)
            for (int j = 0; j < 8; j++) {
                int n = (d[(k * ch + c) * 4 + j / 2] >> ((j & 1) * 4)) & 15;
                int step = k_step[idx], diff = step >> 3;
                if (n & 1) diff += step >> 2;
                if (n & 2) diff += step >> 1;
                if (n & 4) diff += step;
                pred = clampi(n & 8 ? pred - diff : pred + diff, -32768, 32767);
                idx = clampi(idx + k_index[n], 0, 88);
                out[(1 + k * 8 + j) * ch + c] = (int16_t)pred;
            }
    }
}

/* Sample of channel c at frame f of the memory at guest address data. */
static float sample(Voice *v, uint32_t data, uint32_t f, int c)
{
    if (c >= v->channels) c = v->channels - 1;
    if (v->tag == 0x69) {
        int64_t at = (int64_t)data + (int64_t)(f / ADPCM_FRAMES) * 36 * v->channels;
        if (at != v->block) {
            decode_block((const uint8_t *)XBOX_PTR((uint32_t)at), v->channels > 8 ? 8 : v->channels, v->decoded);
            v->block = at;
        }
        return v->decoded[(f % ADPCM_FRAMES) * v->channels + c] / 32768.0f;
    }
    if (v->bits == 8) return ((int)MEM8(data + f * v->align + c) - 128) / 128.0f;
    return (int16_t)MEM16(data + f * v->align + c * 2) / 32768.0f;
}

/* Packet done: the XDK writes its sizes and status, then queues the stream's
 * callback for DirectSoundDoWork to run on the title's thread. */
static void complete(Voice *v, const Packet *pk, uint32_t status, uint32_t used)
{
    if (pk->completed_ptr) MEM32(pk->completed_ptr) = status == 0 ? pk->size : used;
    if (pk->status_ptr) MEM32(pk->status_ptr) = status;
    if (v->callback) {
        if (g_ndone < MAX_DONE) {
            Done d = {v->obj, v->callback, v->context, pk->context, status};
            g_done[g_ndone++] = d;
        } else {
            fprintf(stderr, "[DSOUND] completion queue full, packet callback dropped\n");
        }
    } else if (pk->context) {
        static int warned;
        if (!warned++) fprintf(stderr, "[DSOUND] packet with a completion event: not signalled (unimplemented)\n");
    }
}

static void pop_packet(Voice *v)
{
    memmove(&v->packets[0], &v->packets[1], (size_t)(v->npackets - 1) * sizeof(Packet));
    v->npackets--;
}

/* listener: distance and rolloff factors (IDirectSound_SetDistanceFactor / SetRolloffFactor) */
static float g_distance_factor = 1.0f, g_rolloff_factor = 1.0f;

/* A 3D voice's level on speaker bin b: inverse distance past the minimum distance (DS3D's model, held at the
 * maximum), panned on the x axis. The console's HRTF and doppler are not reproduced. */
static float atten3d(const Voice *v, uint32_t b)
{
    float d, g, pan, side;
    if (!v->is3d) return 1.0f;
    d = sqrtf(v->pos3d[0] * v->pos3d[0] + v->pos3d[1] * v->pos3d[1] + v->pos3d[2] * v->pos3d[2]) * g_distance_factor;
    if (d > v->max_dist) d = v->max_dist;
    g = d <= v->min_dist || v->min_dist <= 0 ? 1.0f : v->min_dist / (v->min_dist + g_rolloff_factor * (d - v->min_dist));
    pan = d > 0.001f ? v->pos3d[0] / (d / g_distance_factor) : 0.0f;  /* -1 left .. 1 right */
    if (b == 2 || b == 3) return g;
    side = b & 1 ? 1.0f + pan : 1.0f - pan;
    return g * (side > 1.0f ? 1.0f : side);
}

/* Gain of voice channel c on host left and right, from its mix bins. */
static void levels(Voice *v, float g[2][2])
{
    static const uint8_t front[2] = {0, 1};
    const uint8_t *bin = v->nbins ? v->bin : front;
    uint32_t n = v->nbins ? v->nbins : 2, voices = (v->channels - 1) / 2 + 1;
    uint32_t per = n / voices < 2 ? 2 : n / voices, step = v->channels == 1 ? 1 : 2;
    int speakers = 0;
    memset(g, 0, 4 * sizeof(float));
    /* muted speaker bins do not count: such a voice is heard through its effect send */
    for (uint32_t s = 0; s < n; s++) speakers |= bin[s] < 10 && bin[s] != 3 && v->bin_mb[bin[s]] > -10000;
    for (uint32_t c = 0; c < 2; c++)
        for (uint32_t s = c; c < v->channels && s < per && s < n; s += step) {
            uint32_t b = bin[s];
            float x;
            if (speakers && (b == 3 || b > 9)) continue;  /* sub and effect sends */
            x = db(v->bin_mb[b]) * v->volume * v->headroom * (b == 2 ? 0.707f : 1.0f) * atten3d(v, b);
            /* even bins are left, odd right, 2 is the centre */
            if (!speakers || b == 2 || !(b & 1)) g[c][0] = g[c][0] > x ? g[c][0] : x;
            if (!speakers || b == 2 || (b & 1)) g[c][1] = g[c][1] > x ? g[c][1] : x;
        }
}

/* xboxrecomp: 5.1 output (F10 Speakers): the bins are the console's speakers in the order a 5.1 channel mask has them
 * (front left, front right, centre, LFE, back left, back right), so each goes to its own speaker instead of the
 * stereo fold-down above; a buffer of more than two channels (a title's own 5.1 mix) goes channel by channel */
static int g_oc = 2;  /* output channels, set when the mixer starts */
int app_Speakers(void);  /* app.cpp: 2 or 6 */

static void levels6(Voice *v, float g[6][6])
{
    static const uint8_t front[2] = {0, 1};
    const uint8_t *bin = v->nbins ? v->bin : front;
    uint32_t n = v->nbins ? v->nbins : 2;
    int speakers = 0;
    memset(g, 0, 36 * sizeof(float));
    /* bins 6..9 are the crosstalk speakers (front L R, back L R): those positions */
    static const uint8_t spk[10] = {0, 1, 2, 3, 4, 5, 0, 1, 4, 5};
    for (uint32_t s = 0; s < n; s++) speakers |= bin[s] < 10 && v->bin_mb[bin[s]] > -10000;
    if (v->channels > 2) {
        for (uint32_t c = 0; c < v->channels && c < 6; c++) {
            uint32_t b = v->nbins > c ? bin[c] : c;
            if (b < 10) g[c][spk[b]] = db(v->bin_mb[b]) * v->volume * v->headroom * atten3d(v, (uint32_t)b);
        }
        return;
    }
    {
        uint32_t voices = (v->channels - 1) / 2 + 1, per = n / voices < 2 ? 2 : n / voices, step = v->channels == 1 ? 1 : 2;
        for (uint32_t c = 0; c < 2; c++)
            for (uint32_t s = c; c < v->channels && s < per && s < n; s += step) {
                uint32_t b = bin[s];
                float x = db(v->bin_mb[b]) * v->volume * v->headroom * atten3d(v, b);
                if (!speakers) {
                    /* heard only through its effect sends: on the front pair, as the stereo mix has it */
                    g[c][0] = g[c][0] > x ? g[c][0] : x;
                    g[c][1] = g[c][1] > x ? g[c][1] : x;
                } else if (b < 10) {
                    g[c][spk[b]] = g[c][spk[b]] > x ? g[c][spk[b]] : x;
                }
            }
    }
}

static void deactivate(Voice *v)
{
    if (v->active) v->active = v->paused = v->ending = 0;
}

/* xboxrecomp: 5.1 output (F10 Speakers = 5.1): render's mix with six outputs, apart from it so the stereo mix stays
 * the code it was (2026-10-24: folded into one, stereo crackled though no sum in it had changed) */
static void render6(float *out, uint32_t frames)
{
    memset(out, 0, frames * 6 * sizeof(float));
    EnterCriticalSection(&g_lock);
    for (int vi = 0; vi < MAX_VOICES; vi++) {
        Voice *v = g_voices[vi];
        float g[6][6], dg[6][6];
        float e0[6] = {0}, e1[6] = {0};  /* 5.1: a multichannel buffer's channels past the first two */
        int ne = 0;
        double step;
        if (!v || v->paused || (!v->stream && !v->playing)) continue;
        memset(g, 0, sizeof(g));
        levels6(v, g);
        if (v->fading_out) memset(g, 0, sizeof(g));
        /* xboxrecomp: a voice that starts ramps up over this pass (~5 ms) instead of starting at full level: the
         * console's voice processor smooths level steps, and a sound starting or stopping mid waveform at once
         * was heard as a click every time */
        if (!v->has_gain) memset(v->gain6, 0, sizeof(v->gain6));
        v->has_gain = 1;
        for (int c = 0; c < 6; c++)
            for (int side = 0; side < 6; side++) dg[c][side] = (g[c][side] - v->gain6[c][side]) / (float)frames;
        step = (double)(v->freq ? v->freq : v->rate) / MIX_RATE;
        for (uint32_t i = 0; i < frames; i++) {
            uint32_t f = (uint32_t)v->pos;
            float a, b, a1, b1, t, x0, x1;
            if (v->stream) {
                while (v->npackets && f >= frames_of(v, v->packets[0].size)) {
                    Packet pk = v->packets[0];
                    pop_packet(v);
                    complete(v, &pk, 0, pk.size);
                    v->pos -= frames_of(v, pk.size);
                    f = (uint32_t)v->pos;
                }
                if (!v->npackets) {
                    if (v->active && !v->ending && g_trace) {
                        static int underruns;
                        if (underruns++ < 20) fprintf(stderr, "[DSOUND] stream %08X ran dry\n", v->obj);
                    }
                    v->pos = 0;
                    if (v->ending) deactivate(v);
                    break;
                }
                a = sample(v, v->packets[0].data, f, 0);
                b = sample(v, v->packets[0].data, f, 1);
                if (f + 1 < frames_of(v, v->packets[0].size)) {
                    a1 = sample(v, v->packets[0].data, f + 1, 0);
                    b1 = sample(v, v->packets[0].data, f + 1, 1);
                } else if (v->npackets > 1) {
                    a1 = sample(v, v->packets[1].data, 0, 0);
                    b1 = sample(v, v->packets[1].data, 0, 1);
                } else {
                    a1 = a;
                    b1 = b;
                }
            } else {
                uint32_t end = v->looping && v->loop_len ? frames_of(v, v->loop_start + v->loop_len) : frames_of(v, v->bytes);
                uint32_t nx;
                if (f >= end) {
                    uint32_t start;
                    if (!v->looping || !end) {
                        v->playing = 0;
                        v->pos = 0;
                        break;
                    }
                    start = v->loop_len ? frames_of(v, v->loop_start) : 0;
                    v->pos -= end - start;
                    f = (uint32_t)v->pos;
                }
                if (!v->data) break;
                nx = f + 1 < end ? f + 1 : v->looping && end ? (v->loop_len ? frames_of(v, v->loop_start) : 0) : f;
                if (v->channels > 2) {
                    /* 5.1: each channel to its own speaker */
                    ne = v->channels < 6 ? v->channels : 6;
                    a = sample(v, v->data, f, 0);
                    b = sample(v, v->data, f, 1);
                    a1 = sample(v, v->data, nx, 0);
                    b1 = sample(v, v->data, nx, 1);
                    for (int c = 2; c < ne; c++) e0[c] = sample(v, v->data, f, c), e1[c] = sample(v, v->data, nx, c);
                } else {
                    a = sample(v, v->data, f, 0);
                    b = sample(v, v->data, f, 1);
                    a1 = sample(v, v->data, nx, 0);
                    b1 = sample(v, v->data, nx, 1);
                }
            }
            t = (float)(v->pos - f);
            x0 = a + (a1 - a) * t;
            x1 = b + (b1 - b) * t;
            {
                float *o = out + i * 6, xe[6] = {x0, x1, 0, 0, 0, 0};
                for (int c = 2; c < ne; c++) xe[c] = e0[c] + (e1[c] - e0[c]) * t;
                for (int c = 0; c < 6; c++)
                    for (int side = 0; side < 6; side++) v->gain6[c][side] += dg[c][side];
                for (int k = 0; k < 6; k++) {
                    float acc = 0;
                    for (int c = 0; c < (ne > 2 ? ne : 2); c++) acc += xe[c] * v->gain6[c][k];
                    o[k] += acc;
                }
            }
            v->pos += step;
        }
        memcpy(v->gain6, g, sizeof(g));
        if (v->fading_out) {
            v->playing = 0;
            v->fading_out = 0;
            v->has_gain = 0;
        }
    }
    LeaveCriticalSection(&g_lock);
}

/* xboxrecomp: 5.1 output upmixes the front pair, Pro Logic style. On the console this title routes every voice to the
 * front left / right bins only (read from the console's APU voice table, 2026-11-02: no 3D voices, no centre, back
 * or LFE sends), so the 5.1 heard from a console is the receiver's Pro Logic decoding of that stereo; a PC's analog
 * 5.1 has no receiver to do it. Here: the centre steered by how alike left and right are (a sound in both equally,
 * as each of this title's mono effects is, goes to the centre; wide or uncorrelated stereo stays on the sides), the
 * difference L - R to the back pair (10 ms later, under 7 kHz, opposite signs: the passive surround decode), the
 * sum under 120 Hz to the LFE. What a title put on the other speakers itself is kept. RECOMP_UPMIX=0: off */
static void upmix(float *out, uint32_t frames)
{
    enum { kDelay = MIX_RATE / 100 };
    static float dl[kDelay], env_sum, env_dif, lp_s, lfe1, lfe2;
    static uint32_t dpos;
    static int on = -1;
    const float a_env = 0.0007f;   /* ~30 ms */
    const float a_s = 0.6f;        /* 7 kHz */
    const float a_lfe = 0.0156f;   /* 120 Hz */
    if (on < 0) {
        const char *e = getenv("RECOMP_UPMIX");
        on = !(e && *e == '0');
        fprintf(stderr, "[DSOUND] 5.1 upmix of the front pair %s\n", on ? "on" : "off (RECOMP_UPMIX=0)");
    }
    if (!on) return;
    for (uint32_t i = 0; i < frames; i++) {
        float *o = out + i * 6, l = o[0], r = o[1];
        float sum = 0.5f * (l + r), dif = 0.5f * (l - r), cg, s;
        env_sum += a_env * (sum * sum - env_sum);
        env_dif += a_env * (dif * dif - env_dif);
        cg = 2.0f * env_sum / (env_sum + env_dif + 1e-12f) - 1.0f;  /* 0 uncorrelated or wide .. 1 the same in both */
        cg = cg < 0.0f ? 0.0f : cg > 1.0f ? 1.0f : cg;
        s = dl[dpos];
        dl[dpos] = dif;
        dpos = (dpos + 1) % kDelay;
        lp_s += a_s * (s - lp_s);
        lfe1 += a_lfe * (sum - lfe1);
        lfe2 += a_lfe * (lfe1 - lfe2);
        o[0] = l - cg * sum;  /* what went to the centre leaves the sides: the level stays the same */
        o[1] = r - cg * sum;
        o[2] += 1.4142f * cg * sum;
        o[3] += 0.7f * lfe2;
        o[4] += 0.7071f * lp_s;
        o[5] -= 0.7071f * lp_s;
    }
}

static void render(float *out, uint32_t frames)
{
    if (g_oc == 6) {
        render6(out, frames);
        upmix(out, frames);
        return;
    }
    memset(out, 0, frames * 2 * sizeof(float));
    EnterCriticalSection(&g_lock);
    for (int vi = 0; vi < MAX_VOICES; vi++) {
        Voice *v = g_voices[vi];
        float g[2][2], dg[2][2];
        double step;
        if (!v || v->paused || (!v->stream && !v->playing)) continue;
        levels(v, g);
        if (v->fading_out) memset(g, 0, sizeof(g));
        /* xboxrecomp: a voice that starts ramps up over this pass (~5 ms) instead of starting at full level: the
         * console's voice processor smooths level steps, and a sound starting or stopping mid waveform at once
         * was heard as a click every time */
        if (!v->has_gain) memset(v->gain, 0, sizeof(v->gain));
        v->has_gain = 1;
        for (int c = 0; c < 2; c++)
            for (int side = 0; side < 2; side++) dg[c][side] = (g[c][side] - v->gain[c][side]) / (float)frames;
        step = (double)(v->freq ? v->freq : v->rate) / MIX_RATE;
        for (uint32_t i = 0; i < frames; i++) {
            uint32_t f = (uint32_t)v->pos;
            float a, b, a1, b1, t, x0, x1;
            if (v->stream) {
                while (v->npackets && f >= frames_of(v, v->packets[0].size)) {
                    Packet pk = v->packets[0];
                    pop_packet(v);
                    complete(v, &pk, 0, pk.size);
                    v->pos -= frames_of(v, pk.size);
                    f = (uint32_t)v->pos;
                }
                if (!v->npackets) {
                    if (v->active && !v->ending && g_trace) {
                        static int underruns;
                        if (underruns++ < 20) fprintf(stderr, "[DSOUND] stream %08X ran dry\n", v->obj);
                    }
                    v->pos = 0;
                    if (v->ending) deactivate(v);
                    break;
                }
                a = sample(v, v->packets[0].data, f, 0);
                b = sample(v, v->packets[0].data, f, 1);
                if (f + 1 < frames_of(v, v->packets[0].size)) {
                    a1 = sample(v, v->packets[0].data, f + 1, 0);
                    b1 = sample(v, v->packets[0].data, f + 1, 1);
                } else if (v->npackets > 1) {
                    a1 = sample(v, v->packets[1].data, 0, 0);
                    b1 = sample(v, v->packets[1].data, 0, 1);
                } else {
                    a1 = a;
                    b1 = b;
                }
            } else {
                uint32_t end = v->looping && v->loop_len ? frames_of(v, v->loop_start + v->loop_len) : frames_of(v, v->bytes);
                uint32_t nx;
                if (f >= end) {
                    uint32_t start;
                    if (!v->looping || !end) {
                        v->playing = 0;
                        v->pos = 0;
                        break;
                    }
                    start = v->loop_len ? frames_of(v, v->loop_start) : 0;
                    v->pos -= end - start;
                    f = (uint32_t)v->pos;
                }
                if (!v->data) break;
                a = sample(v, v->data, f, 0);
                b = sample(v, v->data, f, 1);
                nx = f + 1 < end ? f + 1 : v->looping && end ? (v->loop_len ? frames_of(v, v->loop_start) : 0) : f;
                a1 = sample(v, v->data, nx, 0);
                b1 = sample(v, v->data, nx, 1);
            }
            t = (float)(v->pos - f);
            x0 = a + (a1 - a) * t;
            x1 = b + (b1 - b) * t;
            for (int c = 0; c < 2; c++)
                for (int side = 0; side < 2; side++) v->gain[c][side] += dg[c][side];
            out[i * 2] += x0 * v->gain[0][0] + x1 * v->gain[1][0];
            out[i * 2 + 1] += x0 * v->gain[0][1] + x1 * v->gain[1][1];
            v->pos += step;
        }
        memcpy(v->gain, g, sizeof(g));
        if (v->fading_out) {
            v->playing = 0;
            v->fading_out = 0;
            v->has_gain = 0;
        }
    }
    LeaveCriticalSection(&g_lock);
}

/* ── Output: XAudio2, or a wall clock when there is no audio device ──────── */

/* Queued output: up to OUT_BUFS - 1 blocks of 256 frames in flight. ~80 ms (the Chamber of Secrets setting) put sound
 * noticeably behind the picture; RECOMP_AUDIO_LATENCY_MS sets it, default 32 ms (6 blocks). */
#define OUT_BUFS 16
static int g_out_bufs = OUT_BUFS;

static void init_latency(void)
{
    const char *e = getenv("RECOMP_AUDIO_LATENCY_MS");
    int ms = e && *e ? atoi(e) : 32;
    int n = ms * MIX_RATE / 1000 / MIX_FRAMES + 1;  /* blocks queued, plus the one being filled */
    g_out_bufs = n < 3 ? 3 : n > OUT_BUFS ? OUT_BUFS : n;
    fprintf(stderr, "[DSOUND] output queue %d x %d frames (~%d ms)\n", g_out_bufs - 1, MIX_FRAMES,
            (g_out_bufs - 1) * MIX_FRAMES * 1000 / MIX_RATE);
}

/* RECOMP_DSOUND_WAV=<file>: the mixed output as it goes to XAudio2, 16-bit stereo 48 kHz, for the first 3 minutes */
static FILE *g_wav;
static uint32_t g_wav_frames;

static void wav_write(const int16_t *pcm, uint32_t frames)
{
    static int opened;
    if (!opened) {
        const char *e = getenv("RECOMP_DSOUND_WAV");
        opened = 1;
        if (e && *e && (g_wav = fopen(e, "wb")) != NULL) {
            static const uint8_t hdr[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0,
                                            0, 1, 0, 2, 0, 0x80, 0xBB, 0, 0, 0, 0xEE, 2, 0, 4, 0, 16, 0, 'd', 'a', 't',
                                            'a', 0, 0, 0, 0};
            fwrite(hdr, 1, sizeof hdr, g_wav);
            fprintf(stderr, "[DSOUND] recording the mix to %s\n", e);
        }
    }
    if (!g_wav) return;
    fwrite(pcm, 4, frames, g_wav);
    g_wav_frames += frames;
    if (g_wav_frames % (MIX_RATE * 5) < frames || g_wav_frames >= MIX_RATE * 180) {  /* sizes kept current */
        uint32_t data = g_wav_frames * 4, riff = data + 36;
        long at = ftell(g_wav);
        fseek(g_wav, 4, SEEK_SET);
        fwrite(&riff, 4, 1, g_wav);
        fseek(g_wav, 40, SEEK_SET);
        fwrite(&data, 4, 1, g_wav);
        fseek(g_wav, at, SEEK_SET);
        fflush(g_wav);
        if (g_wav_frames >= MIX_RATE * 180) {
            fclose(g_wav);
            g_wav = NULL;
            fprintf(stderr, "[DSOUND] mix recording finished\n");
        }
    }
}

/* output health, reported every 10 s when anything went wrong */
static unsigned g_dry, g_clipped, g_mixed;
static DWORD g_report_ms;

static DWORD WINAPI mixer_thread(LPVOID unused)
{
    static float mix[MIX_FRAMES * 6];
    static int16_t pcm[OUT_BUFS][MIX_FRAMES * 6];
    IXAudio2 *xa2 = NULL;
    IXAudio2MasteringVoice *master = NULL;
    IXAudio2SourceVoice *src = NULL;
    WAVEFORMATEX wfx = {0}, *wfx_out = &wfx;
    LARGE_INTEGER freq, now, next;
    int nb = 0;
    (void)unused;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    init_latency();
    /* late wake ups of this thread are what empties the device queue; the CPU itself is mostly idle */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    g_report_ms = GetTickCount();
    g_oc = app_Speakers() == 6 ? 6 : 2;
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = (WORD)g_oc;
    wfx.nSamplesPerSec = MIX_RATE;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = (WORD)(2 * g_oc);
    wfx.nAvgBytesPerSec = MIX_RATE * 2 * (DWORD)g_oc;
    {
        /* 5.1: the speaker positions the bins stand for (front L R, centre, LFE, back L R) */
        static WAVEFORMATEXTENSIBLE wfe;
        static const GUID pcm_guid = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
        wfe.Format = wfx;
        wfe.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wfe.Format.cbSize = sizeof(wfe) - sizeof(WAVEFORMATEX);
        wfe.Samples.wValidBitsPerSample = 16;
        wfe.dwChannelMask = 0x3F;
        wfe.SubFormat = pcm_guid;
        if (g_oc == 6) wfx_out = &wfe.Format;
    }
    if (FAILED(XAudio2Create(&xa2, 0, XAUDIO2_DEFAULT_PROCESSOR)) ||
        FAILED(IXAudio2_CreateMasteringVoice(xa2, &master, (UINT32)g_oc, MIX_RATE, 0, NULL, NULL, 0)) ||
        FAILED(IXAudio2_CreateSourceVoice(xa2, &src, wfx_out, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL)) ||
        FAILED(IXAudio2SourceVoice_Start(src, 0, XAUDIO2_COMMIT_NOW))) {
        fprintf(stderr, "[DSOUND] XAudio2 unavailable: mixing on a wall clock, silently\n");
        src = NULL;
    } else {
        fprintf(stderr, "[DSOUND] XAudio2 output, %d Hz %s\n", MIX_RATE, g_oc == 6 ? "5.1" : "stereo");
    }

    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&next);
    for (;;) {
        render(mix, MIX_FRAMES);
        for (int i = 0; i < MIX_FRAMES * g_oc; i++) {
            float s = mix[i] < -1.0f ? -1.0f : mix[i] > 1.0f ? 1.0f : mix[i];
            if (s != mix[i]) g_clipped++;
            pcm[nb][i] = (int16_t)(s * 32767.0f);
        }
        if (g_oc == 2) wav_write(pcm[nb], MIX_FRAMES);  /* RECOMP_DSOUND_WAV: stereo only */
        if (src) {
            XAUDIO2_BUFFER xb = {0};
            XAUDIO2_VOICE_STATE st;
            for (;;) {
                IXAudio2SourceVoice_GetState(src, &st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
                if (st.BuffersQueued < (UINT32)(g_out_bufs - 1)) break;
                Sleep(1);
            }
            if (st.BuffersQueued == 0 && g_mixed > (unsigned)g_out_bufs) g_dry++;  /* the device played everything: a gap */
            g_mixed++;
            {
                DWORD now_ms = GetTickCount();
                if (now_ms - g_report_ms >= 10000) {
                    if (g_dry || g_clipped)
                        fprintf(stderr, "[DSOUND] last %lus: %u output underruns, %u clipped samples\n",
                                (unsigned long)((now_ms - g_report_ms) / 1000), g_dry, g_clipped);
                    g_dry = g_clipped = 0;
                    g_report_ms = now_ms;
                }
            }
            xb.AudioBytes = MIX_FRAMES * 2 * (UINT32)g_oc;
            xb.pAudioData = (const BYTE *)pcm[nb];
            IXAudio2SourceVoice_SubmitSourceBuffer(src, &xb, NULL);
            nb = (nb + 1) % OUT_BUFS;
        } else {
            next.QuadPart += freq.QuadPart * MIX_FRAMES / MIX_RATE;
            for (;;) {
                QueryPerformanceCounter(&now);
                if (now.QuadPart >= next.QuadPart) break;
                Sleep(1);
            }
        }
    }
    return 0;
}

static void start(void)
{
    if (g_started) return;
    g_started = 1;
    CreateThread(NULL, 0, mixer_thread, NULL, 0, NULL);
}

/* ── Shared helpers ─────────────────────────────────────────────────────── */

static void read_format(Voice *v, uint32_t wfx)
{
    if (!wfx) return;
    v->tag = MEM16(wfx);
    v->channels = MEM16(wfx + 2) ? MEM16(wfx + 2) : 1;
    v->rate = MEM32(wfx + 4);
    v->align = MEM16(wfx + 12) ? MEM16(wfx + 12) : 1;
    v->bits = MEM16(wfx + 14);
    if (v->tag != 1 && v->tag != 0x69) fprintf(stderr, "[DSOUND] format tag 0x%X not supported\n", v->tag);
}

/* DSMIXBINS { count, pairs[] of { bin, volume } }: the voice's bins and volumes. */
static void set_bins(Voice *v, uint32_t mixbins)
{
    uint32_t n, pairs;
    v->nbins = 0;  /* none listed: front left and right */
    if (!mixbins) return;
    n = MEM32(mixbins);
    if (n > 8) n = 8;
    pairs = MEM32(mixbins + 4);
    for (uint32_t i = 0; i < n; i++) {
        v->bin[i] = MEM32(pairs + i * 8) & 31;
        v->bin_mb[v->bin[i]] = (int32_t)MEM32(pairs + i * 8 + 4);
    }
    v->nbins = (uint8_t)n;
}

/* 600 mB unless 3D, or a mix-in or effect-in buffer */
static float default_headroom(uint32_t flags) { return flags & 0x382010 ? 1.0f : db(-600); }

static Voice *find(uint32_t obj)
{
    for (int i = 0; i < MAX_VOICES; i++)
        if (g_voices[i] && g_voices[i]->obj == obj) return g_voices[i];
    return NULL;
}

static Voice *new_voice(uint32_t obj_size)
{
    for (int i = 0; i < MAX_VOICES; i++)
        if (!g_voices[i]) {
            Voice *v = calloc(1, sizeof(Voice));
            v->obj = xbox_HeapAlloc(obj_size, 16);
            memset((void *)XBOX_PTR(v->obj), 0, obj_size);
            v->refs = 1;
            v->tag = 1;
            v->channels = 1;
            v->bits = 16;
            v->align = 2;
            v->rate = MIX_RATE;
            v->max_packets = 1;
            v->volume = v->headroom = 1;
            v->block = -1;
            g_voices[i] = v;
            return v;
        }
    fprintf(stderr, "[DSOUND] out of voices (%d)\n", MAX_VOICES);
    return NULL;
}

static void free_voice(Voice *v)
{
    for (int i = 0; i < MAX_VOICES; i++)
        if (g_voices[i] == v) g_voices[i] = NULL;
    xbox_HeapFree(v->obj);
    free(v);
}

static void init_lock(void)
{
    static LONG once;
    if (!InterlockedExchange(&once, 1)) InitializeCriticalSection(&g_lock);
}

/* Completed packets' callbacks run as the XDK runs them: on the title's own
 * thread, from DirectSoundDoWork or the stream call that flushed them. The
 * callback is __stdcall (stream context, packet context, status). */
static void run_callbacks(uint32_t only_voice, int all)
{
    Done run[MAX_DONE];
    int n = 0, keep = 0;
    uint32_t saved_eax = g_eax;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_ndone; i++) {
        if (all || g_done[i].voice == only_voice) run[n++] = g_done[i];
        else g_done[keep++] = g_done[i];
    }
    g_ndone = keep;
    LeaveCriticalSection(&g_lock);
    if (n) TRACE("  %d packet callback(s)%s", n, all ? " from DoWork" : " from flush");
    for (int i = 0; i < n; i++) {
        PUSH32(g_esp, run[i].status);
        PUSH32(g_esp, run[i].packet_context);
        PUSH32(g_esp, run[i].context);
        PUSH32(g_esp, 0);  /* return address */
        RECOMP_ICALL(run[i].callback);
    }
    g_eax = saved_eax;
}

#define FALLBACK(addr) do { if (!hle_on()) { sub_##addr##_gen(); return; } init_lock(); } while (0)

/* ── Device ─────────────────────────────────────────────────────────────── */

/* DirectSoundCreate(pguid, ppDirectSound, pUnkOuter) */
extern void sub_001A89A2_gen(void);
void sub_001A89A2(void)
{
    FALLBACK(001A89A2);
    EnterCriticalSection(&g_lock);
    start();
    if (!g_ds) {
        g_ds = xbox_HeapAlloc(0x40, 16);
        memset((void *)XBOX_PTR(g_ds), 0, 0x40);
    }
    MEM32(ARG(1)) = g_ds;
    LeaveCriticalSection(&g_lock);
    TRACE("DirectSoundCreate -> %08X", g_ds);
    ret_stdcall(S_OK_, 3);
}

/* IDirectSound_Release(this) */
extern void sub_001A5FAA_gen(void);
void sub_001A5FAA(void) { FALLBACK(001A5FAA); ret_stdcall(0, 1); }

/* DirectSoundUseFullHRTF(), DirectSoundOverrideSpeakerConfig(dwSpeakerConfig): HRTF is not reproduced */
extern void sub_001A5FD6_gen(void);
void sub_001A5FD6(void) { FALLBACK(001A5FD6); ret_stdcall(0, 0); }
extern void sub_001A5FF5_gen(void);
void sub_001A5FF5(void) { FALLBACK(001A5FF5); ret_stdcall(0, 1); }

/* IDirectSound_CommitDeferredSettings(this): every setting applies at once here */
extern void sub_001A844A_gen(void);
void sub_001A844A(void) { FALLBACK(001A844A); ret_stdcall(S_OK_, 1); }

/* IDirectSound_SetDistanceFactor / SetRolloffFactor(this, fl, dwApply) */
extern void sub_001A8794_gen(void);
void sub_001A8794(void)
{
    uint32_t u;
    FALLBACK(001A8794);
    u = ARG(1);
    memcpy(&g_distance_factor, &u, 4);
    if (!(g_distance_factor > 0)) g_distance_factor = 1.0f;
    ret_stdcall(S_OK_, 3);
}

extern void sub_001A87B8_gen(void);
void sub_001A87B8(void)
{
    uint32_t u;
    FALLBACK(001A87B8);
    u = ARG(1);
    memcpy(&g_rolloff_factor, &u, 4);
    if (!(g_rolloff_factor >= 0)) g_rolloff_factor = 1.0f;
    ret_stdcall(S_OK_, 3);
}

/* IDirectSound_SynchPlayback(this) */
extern void sub_001A6DF2_gen(void);
void sub_001A6DF2(void) { FALLBACK(001A6DF2); ret_stdcall(S_OK_, 1); }

/* IDirectSound_SetI3DL2Listener(this, pds3dl, dwApply): no room reverb here */
extern void sub_001A87DC_gen(void);
void sub_001A87DC(void) { FALLBACK(001A87DC); ret_stdcall(S_OK_, 3); }

/* IDirectSound_DownloadEffectsImage(this, pvImageBuffer, dwImageSize,
 * pImageLoc, ppImageDesc). Effects run on the console's DSP and are not
 * reproduced; the title gets a zeroed descriptor with no effects. */
extern void sub_001A6DCB_gen(void);
void sub_001A6DCB(void)
{
    FALLBACK(001A6DCB);
    if (ARG(4)) {
        uint32_t desc = xbox_HeapAlloc(0x400, 16);
        memset((void *)XBOX_PTR(desc), 0, 0x400);
        MEM32(ARG(4)) = desc;
    }
    TRACE("DownloadEffectsImage(%u bytes): not reproduced", ARG(2));
    ret_stdcall(S_OK_, 5);
}

/* DirectSoundDoWork() */
extern void sub_001A6EE1_gen(void);
void sub_001A6EE1(void)
{
    FALLBACK(001A6EE1);
    run_callbacks(0, 1);
    g_esp += 4;
    g_eax = 0;
}

/* ── Buffers ────────────────────────────────────────────────────────────── */

/* DSBUFFERDESC { dwSize, dwFlags, dwBufferBytes, lpwfxFormat, lpMixBins, dwInputMixBin } */
static uint32_t new_buffer(uint32_t desc, uint32_t out)
{
    Voice *v;
    uint32_t flags = MEM32(desc + 4), bytes = MEM32(desc + 8);
    EnterCriticalSection(&g_lock);
    start();
    v = new_voice(0x40);
    if (!v) {
        LeaveCriticalSection(&g_lock);
        return E_FAIL_;
    }
    read_format(v, MEM32(desc + 12));
    set_bins(v, MEM32(desc + 16));
    v->headroom = default_headroom(flags);
    v->is3d = (flags & 0x10) != 0;  /* DSBCAPS_CTRL3D */
    v->min_dist = 1.0f;
    v->max_dist = 1e9f;
    if (bytes) {
        v->data = v->owned = xbox_HeapAlloc(bytes, 16);
        v->bytes = v->loop_len = bytes;
    }
    MEM32(out) = v->obj;
    LeaveCriticalSection(&g_lock);
    TRACE("CreateSoundBuffer flags=%X bytes=%u tag=%X ch=%u rate=%u -> %08X",
          flags, bytes, v->tag, v->channels, v->rate, v->obj);
    return S_OK_;
}

/* IDirectSound_CreateSoundBuffer(this, pdsbd, ppBuffer, pUnkOuter) */
extern void sub_001A8770_gen(void);
void sub_001A8770(void) { FALLBACK(001A8770); ret_stdcall(new_buffer(ARG(1), ARG(2)), 4); }

/* One buffer method: look the voice up under the lock, run the body. */
typedef uint32_t (*buf_fn)(Voice *v);

static void buffer_call(void (*gen)(void), buf_fn fn, const char *name, int nargs)
{
    Voice *v;
    uint32_t r;
    if (!hle_on()) { gen(); return; }
    init_lock();
    EnterCriticalSection(&g_lock);
    v = find(ARG(0));
    r = v ? fn(v) : E_FAIL_;
    LeaveCriticalSection(&g_lock);
    TRACE("Buffer_%s(%08X, %08X, %08X) = %08X", name, ARG(0), nargs > 1 ? ARG(1) : 0, nargs > 2 ? ARG(2) : 0, r);
    ret_stdcall(r, nargs);
}

#define BUFFER(name) static uint32_t buf_##name(Voice *v)

BUFFER(Release)
{
    if (--v->refs > 0) return (uint32_t)v->refs;
    if (v->owned) xbox_HeapFree(v->owned);
    free_voice(v);
    return 0;
}

BUFFER(SetBufferData)
{
    if (ARG(1) == v->data && ARG(2) == v->bytes) return S_OK_;
    if (v->fading_out) v->playing = v->fading_out = 0;  /* new data: the old sound's fade ends here */
    if (v->owned && v->owned != ARG(1)) {
        xbox_HeapFree(v->owned);
        v->owned = 0;
    }
    v->data = ARG(1);
    if (!ARG(1) && ARG(2)) v->data = v->owned = xbox_HeapAlloc(ARG(2), 16);  /* a size with no data */
    v->bytes = v->loop_len = ARG(2);
    v->loop_start = 0;
    v->pos = 0;
    v->block = -1;
    return S_OK_;
}

/* Play(this, dwReserved1, dwReserved2, dwFlags): DSBPLAY_LOOPING = 1, DSBPLAY_FROMSTART = 2 */
BUFFER(Play)
{
    if (!v->playing || v->fading_out) v->has_gain = 0;  /* a start ramps up from silence */
    v->fading_out = 0;
    v->playing = 1;
    v->looping = ARG(3) & 1;
    if (ARG(3) & 2) v->pos = 0;
    return S_OK_;
}

BUFFER(Stop)
{
    /* the console's voice processor ramps a voice down; cutting it mid waveform clicked */
    if (v->playing) v->fading_out = 1;
    return S_OK_;
}

BUFFER(SetLoopRegion)
{
    if (ARG(1) > v->bytes || ARG(2) > v->bytes - ARG(1)) return DSERR_INVALID;
    v->loop_start = ARG(1);
    v->loop_len = ARG(2) ? ARG(2) : v->bytes - ARG(1);
    return S_OK_;
}

/* DSBSTATUS_PLAYING = 1, DSBSTATUS_LOOPING = 4 */
BUFFER(GetStatus)
{
    int on = v->playing && !v->fading_out;
    MEM32(ARG(1)) = (on ? 1u : 0u) | (on && v->looping ? 4u : 0u);
    return S_OK_;
}

BUFFER(SetCurrentPosition)
{
    v->pos = frames_of(v, ARG(1));
    return S_OK_;
}

BUFFER(SetVolume)
{
    v->volume = db((int32_t)ARG(1));
    return S_OK_;
}

BUFFER(SetFrequency)
{
    v->freq = ARG(1);
    return S_OK_;
}

BUFFER(SetMixBins)
{
    set_bins(v, ARG(1));
    return S_OK_;
}

static float argf(int i)
{
    uint32_t u = ARG(i);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

/* SetMaxDistance / SetMinDistance(this, flDistance, dwApply) */
BUFFER(SetMaxDistance)
{
    v->max_dist = argf(1);
    return S_OK_;
}

BUFFER(SetMinDistance)
{
    v->min_dist = argf(1);
    return S_OK_;
}

/* RECOMP_DS3D_TRACE=1 logs [DS3D] for each 3D buffer at most twice a second as the title places it: the caller, the
 * position given, its distance, the min / max distance, the level atten3d makes of it, and Harry's world position
 * (his object's matrix, [0x3565B0]+0x28): whether the positions are the listener's (Harry's or the camera's) frame or
 * the world's */
static void ds3d_trace(const Voice *v)
{
    static int on = -1, lines;
    enum { kSeen = 256 };
    static uint32_t seen[kSeen][2];
    if (on < 0) {
        const char *t = getenv("RECOMP_DS3D_TRACE");
        on = t && *t && *t != '0';
    }
    if (!on || lines > 30000) return;
    uint32_t now = GetTickCount(), buf = ARG(0), i;
    for (i = 0; i < kSeen && seen[i][0] && seen[i][0] != buf; i++) {}
    if (i == kSeen) i = buf % kSeen;
    if (seen[i][0] == buf && now - seen[i][1] < 500) return;
    seen[i][0] = buf, seen[i][1] = now;
    lines++;
    float d = sqrtf(v->pos3d[0] * v->pos3d[0] + v->pos3d[1] * v->pos3d[1] + v->pos3d[2] * v->pos3d[2]);
    float hx = 0, hy = 0, hz = 0;
    uint32_t harry = MEM32(0x3565B0), obj = harry >= 0x80000000u && harry < 0x84000000u ? MEM32(harry + 0x28) : 0;
    if (obj >= 0x80000000u && obj < 0x84000000u) {
        uint32_t m = obj + 0x40 * (MEM32(obj + 0x2C) + 1);
        hx = MEMF(m + 0x30), hy = MEMF(m + 0x34), hz = MEMF(m + 0x38);
    }
    fprintf(stderr, "[DS3D] %8.2f s buf %08X from %08X pos %.0f %.0f %.0f dist %.0f min %.0f max %.0f level %.3f "
            "playing %d | harry %.0f %.0f %.0f\n", now / 1000.0, buf, MEM32(g_esp), v->pos3d[0], v->pos3d[1],
            v->pos3d[2], d, v->min_dist, v->max_dist, atten3d(v, 2), v->playing, hx, hy, hz);
}

/* SetPosition(this, x, y, z, dwApply) */
BUFFER(SetPosition)
{
    v->pos3d[0] = argf(1);
    v->pos3d[1] = argf(2);
    v->pos3d[2] = argf(3);
    ds3d_trace(v);
    return S_OK_;
}

/* SetVelocity, SetDistanceFactor, SetDopplerFactor, SetRolloffCurve: doppler and per-voice curves not reproduced */
BUFFER(Ignore3D) { (void)v; return S_OK_; }

/* SetPlayRegion(this, dwPlayStart, dwPlayLength): a non-looping play covers this region */
BUFFER(SetPlayRegion)
{
    if (ARG(1) > v->bytes) return DSERR_INVALID;
    v->loop_start = ARG(1);
    v->loop_len = ARG(2) && ARG(2) <= v->bytes - ARG(1) ? ARG(2) : v->bytes - ARG(1);
    return S_OK_;
}

/* SetFormat(this, pwfxFormat) */
BUFFER(SetFormat)
{
    read_format(v, ARG(1));
    v->block = -1;
    return S_OK_;
}

/* ── Streams: the object carries the XDK's own vtables ──────────────────── */

/* DirectSoundCreateStream(pdssd, ppStream)
 * DSSTREAMDESC { dwFlags, dwMaxAttachedPackets, lpwfxFormat, lpfnCallback,
 *                lpvContext, lpMixBins } */
extern void sub_001A89E9_gen(void);
void sub_001A89E9(void)
{
    uint32_t desc, flags;
    Voice *v;
    FALLBACK(001A89E9);
    desc = ARG(0);
    flags = MEM32(desc);
    EnterCriticalSection(&g_lock);
    start();
    v = new_voice(0x80);
    if (!v) {
        LeaveCriticalSection(&g_lock);
        ret_stdcall(E_FAIL_, 2);
        return;
    }
    MEM32(v->obj) = STREAM_VTABLE;
    MEM32(v->obj + 4) = STREAM_VTABLE2;
    v->stream = 1;
    v->max_packets = MEM32(desc + 4) ? MEM32(desc + 4) : 1;
    if (v->max_packets > MAX_PACKETS) v->max_packets = MAX_PACKETS;
    read_format(v, MEM32(desc + 8));
    v->callback = MEM32(desc + 12);
    v->context = MEM32(desc + 16);
    set_bins(v, MEM32(desc + 20));
    v->headroom = default_headroom(flags);
    MEM32(ARG(1)) = v->obj;
    LeaveCriticalSection(&g_lock);
    TRACE("CreateStream flags=%X packets=%u tag=%X ch=%u rate=%u cb=%08X -> %08X",
          flags, v->max_packets, v->tag, v->channels, v->rate, v->callback, v->obj);
    ret_stdcall(S_OK_, 2);
}

/* Flush: every queued packet completes as flushed, and the stream's pending
 * callbacks go to the calling thread before the call returns. */
static void flush_stream(Voice *v)
{
    for (int i = 0; i < v->npackets; i++) complete(v, &v->packets[i], XMEDIAPACKET_STATUS_FLUSHED, 0);
    v->npackets = 0;
    v->pos = 0;
    deactivate(v);
}

typedef uint32_t (*str_fn)(Voice *v, int *flushed);

static void stream_call(void (*gen)(void), str_fn fn, const char *name, int nargs)
{
    Voice *v;
    uint32_t r, obj = ARG(0);
    int flushed = 0;
    if (!hle_on()) { gen(); return; }
    init_lock();
    EnterCriticalSection(&g_lock);
    v = find(obj);
    r = v ? fn(v, &flushed) : E_FAIL_;
    LeaveCriticalSection(&g_lock);
    TRACE("Stream_%s(%08X, %08X) = %08X", name, obj, nargs > 1 ? ARG(1) : 0, r);
    ret_stdcall(r, nargs);
    if (flushed) run_callbacks(obj, 0);
}

#define STREAM(name) static uint32_t str_##name(Voice *v, int *flushed)

STREAM(AddRef)
{
    (void)flushed;
    return (uint32_t)++v->refs;
}

STREAM(Release)
{
    if (--v->refs > 0) return (uint32_t)v->refs;
    flush_stream(v);
    *flushed = 1;
    /* the voice goes, its callbacks still run: they only carry the guest
     * object's address, which stays allocated until they have */
    for (int i = 0; i < MAX_VOICES; i++)
        if (g_voices[i] == v) g_voices[i] = NULL;
    free(v);
    return 0;
}

/* XMEDIAINFO { dwFlags, dwInputSize, dwOutputSize, dwMaxLookahead } */
STREAM(GetInfo)
{
    uint32_t info = ARG(1);
    (void)flushed;
    MEM32(info) = 0x3;  /* fixed sample size, fixed packet alignment */
    MEM32(info + 4) = v->tag == 0x69 ? 36u * v->channels : v->align;
    MEM32(info + 8) = 0;
    MEM32(info + 12) = 0;
    return S_OK_;
}

/* XMO_STATUSF_ACCEPT_INPUT = 1 */
STREAM(GetStatus)
{
    (void)flushed;
    MEM32(ARG(1)) = (uint32_t)v->npackets < v->max_packets ? 1u : 0u;
    return S_OK_;
}

/* Process(this, pInputPacket, pOutputPacket)
 * XMEDIAPACKET { pvBuffer, dwMaxSize, pdwCompletedSize, pdwStatus, pContext, prtTimestamp } */
STREAM(Process)
{
    uint32_t in = ARG(1);
    Packet pk;
    (void)flushed;
    if (!in) return S_OK_;
    if (v->npackets >= MAX_PACKETS) return E_FAIL_;
    pk.data = MEM32(in);
    pk.size = MEM32(in + 4);
    pk.completed_ptr = MEM32(in + 8);
    pk.status_ptr = MEM32(in + 12);
    pk.context = MEM32(in + 16);
    if (pk.completed_ptr) MEM32(pk.completed_ptr) = 0;
    if (pk.status_ptr) MEM32(pk.status_ptr) = XMEDIAPACKET_STATUS_PENDING;
    v->packets[v->npackets++] = pk;
    TRACE("  packet %08X size=%u queued=%d", pk.data, pk.size, v->npackets);
    v->active = 1;
    v->ending = 0;
    return S_OK_;
}

/* SetVolume(this, lVolume), SetHeadroom(this, dwHeadroom), SetMixBins(this, pMixBins) */
STREAM(SetVolume)
{
    (void)flushed;
    v->volume = db((int32_t)ARG(1));
    return S_OK_;
}

STREAM(SetHeadroom)
{
    (void)flushed;
    v->headroom = db(-(int32_t)ARG(1));
    return S_OK_;
}

STREAM(SetMixBins)
{
    (void)flushed;
    set_bins(v, ARG(1));
    return S_OK_;
}

STREAM(Discontinuity)
{
    if (v->npackets) {
        v->ending = 1;
    } else if (v->active) {
        flush_stream(v);
        *flushed = 1;
    }
    return S_OK_;
}

STREAM(Flush)
{
    flush_stream(v);
    *flushed = 1;
    return S_OK_;
}

/* FlushEx(this, rtTimeStamp (64-bit), dwFlags): DSSTREAMFLUSHEX_ASYNC = 1
 * flushes only an active stream */
STREAM(FlushEx)
{
    if ((ARG(3) & 1) && !v->active) return S_OK_;
    flush_stream(v);
    *flushed = 1;
    return S_OK_;
}

/* Pause(this, dwPause): DSSTREAMPAUSE_PAUSE = 1, DSSTREAMPAUSE_RESUME = 0 */
STREAM(Pause)
{
    (void)flushed;
    v->paused = ARG(1) == 1;
    return S_OK_;
}

/* ── Entry points: literal definitions, so --exclude-manual finds them ──── */

/* IDirectSoundBuffer_* */
extern void sub_001A5FC0_gen(void);
void sub_001A5FC0(void) { buffer_call(sub_001A5FC0_gen, buf_Release, "Release", 1); }
extern void sub_001A7F53_gen(void);
void sub_001A7F53(void) { buffer_call(sub_001A7F53_gen, buf_SetBufferData, "SetBufferData", 3); }
extern void sub_001A6E26_gen(void);
void sub_001A6E26(void) { buffer_call(sub_001A6E26_gen, buf_Play, "Play", 4); }
extern void sub_001A6E4A_gen(void);
void sub_001A6E4A(void) { buffer_call(sub_001A6E4A_gen, buf_Stop, "Stop", 1); }
extern void sub_001A6E62_gen(void);
void sub_001A6E62(void) { buffer_call(sub_001A6E62_gen, buf_SetLoopRegion, "SetLoopRegion", 3); }
extern void sub_001A6E82_gen(void);
void sub_001A6E82(void) { buffer_call(sub_001A6E82_gen, buf_GetStatus, "GetStatus", 2); }
extern void sub_001A6E9E_gen(void);
void sub_001A6E9E(void) { buffer_call(sub_001A6E9E_gen, buf_SetCurrentPosition, "SetCurrentPosition", 2); }
extern void sub_001A6E0A_gen(void);
void sub_001A6E0A(void) { buffer_call(sub_001A6E0A_gen, buf_SetVolume, "SetVolume", 2); }
extern void sub_001A77EF_gen(void);
void sub_001A77EF(void) { buffer_call(sub_001A77EF_gen, buf_SetFrequency, "SetFrequency", 2); }
extern void sub_001A780B_gen(void);
void sub_001A780B(void) { buffer_call(sub_001A780B_gen, buf_SetMixBins, "SetMixBins", 2); }

extern void sub_001A7827_gen(void);
void sub_001A7827(void) { buffer_call(sub_001A7827_gen, buf_SetMaxDistance, "SetMaxDistance", 3); }
extern void sub_001A784B_gen(void);
void sub_001A784B(void) { buffer_call(sub_001A784B_gen, buf_SetMinDistance, "SetMinDistance", 3); }
extern void sub_001A786F_gen(void);
void sub_001A786F(void) { buffer_call(sub_001A786F_gen, buf_SetPosition, "SetPosition", 5); }
extern void sub_001A78A4_gen(void);
void sub_001A78A4(void) { buffer_call(sub_001A78A4_gen, buf_Ignore3D, "SetVelocity", 5); }
extern void sub_001A78D9_gen(void);
void sub_001A78D9(void) { buffer_call(sub_001A78D9_gen, buf_Ignore3D, "SetDistanceFactor", 3); }
extern void sub_001A78FD_gen(void);
void sub_001A78FD(void) { buffer_call(sub_001A78FD_gen, buf_Ignore3D, "SetDopplerFactor", 3); }
extern void sub_001A7921_gen(void);
void sub_001A7921(void) { buffer_call(sub_001A7921_gen, buf_Ignore3D, "SetRolloffCurve", 4); }
extern void sub_001A7945_gen(void);
void sub_001A7945(void) { buffer_call(sub_001A7945_gen, buf_SetPlayRegion, "SetPlayRegion", 3); }
extern void sub_001A7F37_gen(void);
void sub_001A7F37(void) { buffer_call(sub_001A7F37_gen, buf_SetFormat, "SetFormat", 2); }

/* CDirectSoundStream_*, reached through the stream vtable */
extern void sub_001A6696_gen(void);
void sub_001A6696(void) { stream_call(sub_001A6696_gen, str_AddRef, "AddRef", 1); }
extern void sub_001A66DD_gen(void);
void sub_001A66DD(void) { stream_call(sub_001A66DD_gen, str_Release, "Release", 1); }
extern void sub_001A672B_gen(void);
void sub_001A672B(void) { stream_call(sub_001A672B_gen, str_GetInfo, "GetInfo", 2); }
extern void sub_001A682A_gen(void);
void sub_001A682A(void) { stream_call(sub_001A682A_gen, str_GetStatus, "GetStatus", 2); }
extern void sub_001A687B_gen(void);
void sub_001A687B(void) { stream_call(sub_001A687B_gen, str_Process, "Process", 3); }
extern void sub_001A6792_gen(void);
void sub_001A6792(void) { stream_call(sub_001A6792_gen, str_Discontinuity, "Discontinuity", 1); }
extern void sub_001A67DF_gen(void);
void sub_001A67DF(void) { stream_call(sub_001A67DF_gen, str_Flush, "Flush", 1); }
extern void sub_001A6943_gen(void);
void sub_001A6943(void) { stream_call(sub_001A6943_gen, str_FlushEx, "FlushEx", 4); }
extern void sub_001A68F2_gen(void);
void sub_001A68F2(void) { stream_call(sub_001A68F2_gen, str_Pause, "Pause", 2); }

extern void sub_001A6D27_gen(void);
void sub_001A6D27(void) { stream_call(sub_001A6D27_gen, str_SetVolume, "SetVolume", 2); }
extern void sub_001A6D79_gen(void);
void sub_001A6D79(void) { stream_call(sub_001A6D79_gen, str_SetHeadroom, "SetHeadroom", 2); }
extern void sub_001A779D_gen(void);
void sub_001A779D(void) { stream_call(sub_001A779D_gen, str_SetMixBins, "SetMixBins", 2); }

/* IDirectSoundStream_SetVolume / SetHeadroom / SetMixBins: jmps to the CDirectSoundStream methods */
extern void sub_001A6EBA_gen(void);
void sub_001A6EBA(void)
{
    FALLBACK(001A6EBA);
    sub_001A6D27();
}

extern void sub_001A6EBF_gen(void);
void sub_001A6EBF(void)
{
    FALLBACK(001A6EBF);
    sub_001A6D79();
}

extern void sub_001A7965_gen(void);
void sub_001A7965(void)
{
    FALLBACK(001A7965);
    sub_001A779D();
}

/* IDirectSoundStream_Pause: a jmp to CDirectSoundStream_Pause */
extern void sub_001A6EC4_gen(void);
void sub_001A6EC4(void)
{
    FALLBACK(001A6EC4);
    sub_001A68F2();
}

/* IDirectSoundStream_FlushEx: forwards its four arguments */
extern void sub_001A6EC9_gen(void);
void sub_001A6EC9(void)
{
    FALLBACK(001A6EC9);
    sub_001A6943();
}
