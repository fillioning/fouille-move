/**
 * Fouille — Network Microsound Excavator
 * Schwung sound generator for Ableton Move
 *
 * Author: Vincent Music (fillioning)
 * License: MIT
 * Architecture: 8-voice polyphonic, round-robin stealing
 *
 * Inspired by Emiliano Pennisi's triad:
 *   Envion  — Found Net Sound, Dynatext ternary envelopes, de-authoring
 *   Endogen — vactrol LPG, lowercase headroom, organic drift, corpus nav
 *   Interfera — dual seeder, spectral lock, erosion, pulse engine
 *
 * API: plugin_api_v2_t
 * Audio: 44100Hz, 128 frames/block, stereo interleaved int16 output
 *
 * Parameter pages:
 *   Excavation: [Terrain, Depth, Rise, Hold, Fall, Strike, Lock, Erode]
 *   Currents:   [Flow, Scatter, Stretch, Grain, Drift, Pool, Spread, Volume]
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>

#define SAMPLE_RATE     44100.0f
#define BLOCK_SIZE      128
#define N_VOICES        8
#define N_SLOTS         8
#define MAX_SLOT_SAMPLES (22050 * 10)  /* 10 seconds at 22050Hz mono */
#define ECHO_POOL_SIZE  (44100 * 5)    /* 5 seconds stereo */

#define SLOT_EMPTY   0
#define SLOT_LOADING 1
#define SLOT_READY   2
#define SLOT_PLAYING 3

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ── Utility ────────────────────────────────────────────────────────────── */

static inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

static inline float lerpf(float a, float b, float t) {
    return a + (b - a) * t;
}

/* Simple xorshift32 PRNG — no malloc, no state beyond a uint32 */
static inline uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static inline float randf(uint32_t *state) {
    return (float)(xorshift32(state) & 0x7FFFFF) / (float)0x7FFFFF;
}

/* ── Sound Slot ─────────────────────────────────────────────────────────── */

typedef struct {
    atomic_int state;
    float *pcm_a;               /* buffer A (double-buffered) */
    float *pcm_b;               /* buffer B */
    atomic_int active_buf;      /* 0 = A is live, 1 = B is live */
    int length;                 /* samples in current buffer */
    /* Descriptors (computed on arrival by fetch thread) */
    float centroid;             /* spectral brightness 0-1 */
    float loudness;             /* RMS energy 0-1 */
    float noisiness;            /* spectral flatness 0-1 */
    uint32_t fetch_time;        /* tick when fetched (for erosion aging) */
} sound_slot_t;

/* ── Vactrol LPG ────────────────────────────────────────────────────────── */

typedef struct {
    float vactrol_cv;           /* LED brightness (envelope output) */
    float vactrol_state;        /* photoresistor state (slowly follows cv) */
    float lp_z1;                /* one-pole LPF state for LPG */
} vactrol_lpg_t;

static inline void vactrol_reset(vactrol_lpg_t *v) {
    v->vactrol_cv = 0.0f;
    v->vactrol_state = 0.0f;
    v->lp_z1 = 0.0f;
}

static inline float vactrol_process(vactrol_lpg_t *v, float input,
                                     float rise_rate, float fall_rate,
                                     float nonlinearity) {
    /* Vactrol state follows CV with asymmetric rates */
    float target = v->vactrol_cv;
    float rate = (target > v->vactrol_state) ? rise_rate : fall_rate;
    v->vactrol_state += (target - v->vactrol_state) * rate;
    if (fabsf(v->vactrol_state) < 1e-15f) v->vactrol_state = 0.0f;

    /* Derive amplitude and cutoff from vactrol state */
    float amp = powf(v->vactrol_state, 0.7f + nonlinearity * 0.3f);
    float cutoff_hz = 200.0f + v->vactrol_state * 17800.0f;

    /* One-pole lowpass: y[n] = y[n-1] + alpha * (x[n] - y[n-1]) */
    float omega = 2.0f * (float)M_PI * cutoff_hz / SAMPLE_RATE;
    float alpha = omega / (omega + 1.0f);
    v->lp_z1 += alpha * (input * amp - v->lp_z1);
    if (fabsf(v->lp_z1) < 1e-15f) v->lp_z1 = 0.0f;

    return v->lp_z1;
}

/* ── Organic Drift (Brownian LFO) ───────────────────────────────────────── */

typedef struct {
    float value;                /* current drift value -1..1 */
    float velocity;             /* rate of change */
    uint32_t rng;               /* per-voice PRNG state */
} organic_drift_t;

static inline float drift_process(organic_drift_t *d, float depth) {
    /* Brownian walk: random acceleration, friction-damped velocity */
    float accel = (randf(&d->rng) - 0.5f) * 0.0002f;
    d->velocity += accel;
    d->velocity *= 0.9995f;     /* friction */
    d->value += d->velocity;
    d->value = clampf(d->value, -1.0f, 1.0f);
    return d->value * depth;
}

/* ── Voice ──────────────────────────────────────────────────────────────── */

typedef enum {
    ENV_OFF = 0,
    ENV_RISE,       /* attack */
    ENV_HOLD,       /* sustain (freeze / drift / granulate) */
    ENV_FALL        /* decay/release */
} env_stage_t;

typedef enum {
    HOLD_FREEZE = 0,
    HOLD_DRIFT,
    HOLD_GRANULATE
} hold_mode_t;

typedef struct {
    int active;
    int slot_idx;               /* which sound slot this voice reads from */
    int note;
    float velocity;

    /* Playback */
    float read_pos;             /* fractional sample position into slot buffer */
    float play_speed;           /* playback rate (1.0 = normal) */

    /* Ternary envelope → feeds vactrol LPG */
    env_stage_t env_stage;
    float env_level;            /* 0-1 envelope output */
    float env_rise_rate;        /* per-sample rise increment */
    float env_fall_rate;        /* per-sample fall decrement */

    /* Vactrol LPG */
    vactrol_lpg_t lpg;

    /* Organic drift */
    organic_drift_t drift_pos;   /* position drift */
    organic_drift_t drift_pan;   /* pan drift */

    /* Stereo */
    float pan;                  /* -1..1 */

    /* Grain state (for HOLD_GRANULATE) */
    float grain_origin;         /* center of grain loop */
    float grain_size_samples;   /* size of micro-loop */
    int grain_counter;          /* samples until next grain jitter */
} voice_t;

/* ── Fetch Thread Context ───────────────────────────────────────────────── */

typedef struct {
    pthread_t thread;
    atomic_int running;
    atomic_int fetch_requested;  /* set by audio thread on hard velocity */
    int next_slot;               /* round-robin slot target */
    char module_dir[512];        /* path to module directory (for key file) */

    /* Shared with instance — pointers to sound slots */
    sound_slot_t *slots;

    /* Parameters (read by fetch thread, written by set_param) */
    atomic_int terrain;          /* 0-7 category index */
    atomic_int depth_pct;        /* 0-100 brightness filter */
    atomic_int flow_seconds;     /* seconds between auto-fetches */
    atomic_int mode;             /* 0=Fouille (internet), 1=Montreal */
    atomic_int neighbourhood;    /* 0-15 Montreal neighbourhood index */

    uint32_t rng;                /* PRNG for word selection */
    uint32_t tick;               /* monotonic tick counter */
} fetch_context_t;

/* ── Instance ───────────────────────────────────────────────────────────── */

typedef struct {
    sound_slot_t slots[N_SLOTS];
    voice_t voices[N_VOICES];
    int voice_cursor;            /* round-robin for note stealing */

    fetch_context_t fetch;

    /* Echo Pool (Phase Garden feedback) */
    float *echo_pool;            /* circular buffer, mono */
    int echo_pool_write;         /* write position */

    /* Parameters — Page 1: Excavation */
    float param_terrain;         /* 0-1 → 8 categories */
    float param_depth;           /* 0-1 spectral brightness filter */
    float param_rise;            /* 0-1 → 0.5ms to 2s */
    float param_hold;            /* 0-1 → freeze/drift/granulate */
    float param_fall;            /* 0-1 → 1ms to 5s */
    float param_strike;          /* 0-1 → vactrol nonlinearity */
    float param_lock;            /* 0-1 → spectral lock Q (0=bypass) */
    float param_erode;           /* 0-1 → erosion rate */

    /* Parameters — Page 3: Location */
    float param_mode;            /* 0-1 → 0=Fouille, 1=Cities */
    float param_city;            /* 0-1 → city index (only Montreal for now) */
    float param_hood;            /* 0-1 → neighbourhood index */

    /* Parameters — Page 2: Currents */
    float param_flow;            /* 0-1 → 5s to 120s */
    float param_scatter;         /* 0-1 → velocity randomness */
    float param_stretch;         /* 0-1 → 0.25x to 4x playback speed */
    float param_grain;           /* 0-1 → 1ms to 200ms grain size */
    float param_drift;           /* 0-1 → organic drift depth */
    float param_pool;            /* 0-1 → echo pool blend */
    float param_spread;          /* 0-1 → stereo width */
    float param_volume;          /* 0-1 → master volume */

    /* Spectral Lock state (global, mono) */
    float lock_freq;             /* tracked frequency */
    float lock_bp_z1, lock_bp_z2; /* biquad state */

    /* Erosion state */
    float erosion_lp_z1;         /* progressive LPF */

    /* Compander state */
    float comp_env;              /* RMS follower for companding */

    uint32_t rng;                /* master PRNG */
    uint32_t tick;               /* monotonic sample counter */
    int current_page;            /* 0 = Excavation, 1 = Currents, 2 = Location */
} fouille_instance_t;

/* ── Poetic Word Lists ──────────────────────────────────────────────────── */

static const char *POETIC_WORDS[] = {
    "texture", "rust", "breath", "glass", "membrane", "erosion",
    "vapor", "residue", "hum", "crackle", "whisper", "murmur",
    "resonance", "fragment", "sediment", "pulse", "shimmer", "static",
    "drift", "grain", "fossil", "echo", "patina", "threshold",
    "dissolution", "filament", "oxidation", "tremor", "lichen",
    "rain", "market", "forest", "bells", "microsound", "signal",
    "surface", "dust", "friction", "contact", "drip", "hiss",
    "rumble", "chirp", "knock", "scrape", "rattle", "drone"
};
#define N_POETIC_WORDS (sizeof(POETIC_WORDS) / sizeof(POETIC_WORDS[0]))

static const char *CATEGORY_TERMS[][3] = {
    /* Nature   */ {"field-recording", "wind", "water"},
    /* Machines */ {"motor", "industrial", "mechanism"},
    /* Voices   */ {"speech", "whisper", "choir"},
    /* Abstract */ {"noise", "texture", "drone"},
    /* Material */ {"metal", "wood", "glass"},
    /* Urban    */ {"city", "traffic", "construction"},
    /* Organic  */ {"body", "liquid", "biological"},
    /* Cosmic   */ {"radio", "electromagnetic", "signal"}
};

/* ── Montreal Neighbourhoods (Geosonic Seeder) ─────────────────────────── */

typedef struct {
    const char *name;           /* display name */
    const char *search_terms;   /* URL-encoded search keywords for IA */
} neighbourhood_t;

/* Neighbourhoods are searched via radio-aporee-maps + location keywords.
 * The IA title/description fields contain street names and landmarks. */
static const neighbourhood_t MONTREAL_HOODS[] = {
    {"Plateau",       "plateau+montreal"},
    {"Mile End",      "mile+end+montreal"},
    {"St-Henri",      "saint-henri+montreal"},
    {"Hochelaga",     "hochelaga+montreal"},
    {"Vieux-Mtl",     "vieux+montreal+old"},
    {"Centre-Ville",  "centre+ville+downtown+montreal"},
    {"Verdun",        "verdun+montreal"},
    {"Rosemont",      "rosemont+montreal"},
    {"NDG",           "notre+dame+grace+montreal"},
    {"Villeray",      "villeray+parc+extension+montreal"},
    {"Griffintown",   "griffintown+pointe+charles+montreal"},
    {"Outremont",     "outremont+mile+ex+montreal"},
    {"Parc Jarry",    "jarry+villeray+montreal"},
    {"Mont-Royal",    "mont+royal+mountain+montreal"},
    {"Lachine",       "lachine+canal+montreal"},
    {"Ahuntsic",      "ahuntsic+cartierville+montreal"},
};
#define N_MONTREAL_HOODS (sizeof(MONTREAL_HOODS) / sizeof(MONTREAL_HOODS[0]))

/* ── Descriptor Computation (simple, runs in fetch thread) ──────────────── */

static void compute_descriptors(float *pcm, int len, float *centroid, float *loudness, float *noisiness) {
    /* RMS loudness */
    float sum_sq = 0.0f;
    for (int i = 0; i < len; i++) {
        sum_sq += pcm[i] * pcm[i];
    }
    *loudness = sqrtf(sum_sq / (float)(len > 0 ? len : 1));

    /* Spectral centroid approximation:
     * Use zero-crossing rate as a proxy for brightness (cheap, no FFT needed for v0.1) */
    int crossings = 0;
    for (int i = 1; i < len; i++) {
        if ((pcm[i] >= 0.0f) != (pcm[i-1] >= 0.0f)) crossings++;
    }
    float zcr = (float)crossings / (float)(len > 0 ? len : 1);
    *centroid = clampf(zcr * 10.0f, 0.0f, 1.0f);  /* normalize to 0-1 range */

    /* Spectral flatness approximation:
     * Ratio of high-frequency energy to total energy */
    float hi_energy = 0.0f;
    float lo_energy = 0.0f;
    float hp_z1 = 0.0f;
    for (int i = 0; i < len; i++) {
        /* Simple one-pole HP at ~2kHz to split hi/lo */
        float hp = pcm[i] - hp_z1;
        hp_z1 += 0.3f * hp;
        hi_energy += hp * hp;
        lo_energy += pcm[i] * pcm[i];
    }
    *noisiness = (lo_energy > 0.0001f) ? clampf(hi_energy / lo_energy, 0.0f, 1.0f) : 0.5f;
}

/* ── minimp3 decoder ────────────────────────────────────────────────────── */

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_FLOAT_OUTPUT
#include "minimp3.h"

/* ── Fetch Thread ───────────────────────────────────────────────────────── */

/*
 * v0.2: Real HTTP fetching from Freesound + Internet Archive.
 *
 * Dual Seeder architecture (Interfera-inspired):
 *   1. Freesound — tagged, descriptor-filtered short sounds (needs API key)
 *   2. Internet Archive — field recordings, ambient textures (no key needed)
 *
 * Flow:
 *   popen("curl") → search API → parse preview URL → download MP3
 *   → minimp3 decode to float PCM → compute descriptors → atomic slot swap
 *
 * Falls back to synthetic textures if network is unavailable.
 */

/* Path to curl on Move */
#define CURL_PATH "/data/UserData/schwung/bin/curl"

/* Max size for API response JSON buffer */
#define JSON_BUF_SIZE  (64 * 1024)
/* Max size for downloaded MP3 file */
#define MP3_BUF_SIZE   (512 * 1024)

static void generate_synthetic_texture(float *buf, int len, uint32_t *rng) {
    /* Generate a filtered noise burst with random character */
    float freq = 200.0f + randf(rng) * 4000.0f;
    float decay = 0.9990f + randf(rng) * 0.0009f;
    float lp_z1 = 0.0f;
    float amp = 0.3f + randf(rng) * 0.4f;
    float omega = 2.0f * (float)M_PI * freq / 22050.0f;
    float alpha = omega / (omega + 1.0f);
    float env = amp;

    for (int i = 0; i < len; i++) {
        float noise = (randf(rng) - 0.5f) * 2.0f;
        lp_z1 += alpha * (noise - lp_z1);
        buf[i] = lp_z1 * env;
        env *= decay;
        if ((xorshift32(rng) & 0xFFF) < 2) {
            env = amp * (0.5f + randf(rng) * 0.5f);
        }
    }
}

/* ── Simple JSON helpers (no library needed) ───────────────────────────── */

/* Find a JSON string value by key — returns pointer into json, writes length.
 * Only handles flat "key":"value" patterns (good enough for Freesound response). */
static const char *json_find_string(const char *json, const char *key, int *out_len) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = strstr(json, pattern);
    if (!p) return NULL;
    p += strlen(pattern);
    /* Skip whitespace */
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return NULL;
    p++;  /* skip opening quote */
    const char *end = p;
    while (*end && *end != '"') {
        if (*end == '\\') end++;  /* skip escaped chars */
        end++;
    }
    *out_len = (int)(end - p);
    return p;
}

/* ── Read Freesound API key from file ──────────────────────────────────── */

static int read_api_key(const char *module_dir, char *key_buf, int key_buf_len) {
    char path[600];
    snprintf(path, sizeof(path), "%s/freesound.key", module_dir);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(key_buf, key_buf_len, f)) { fclose(f); return 0; }
    fclose(f);
    /* Strip trailing whitespace/newline */
    int len = (int)strlen(key_buf);
    while (len > 0 && (key_buf[len-1] == '\n' || key_buf[len-1] == '\r' || key_buf[len-1] == ' '))
        key_buf[--len] = '\0';
    return len > 0;
}

/* ── Run a command and capture stdout into a buffer ────────────────────── */

static int run_cmd_capture(const char *cmd, uint8_t *buf, int buf_size) {
    FILE *pipe = popen(cmd, "r");
    if (!pipe) return 0;
    int total = 0;
    while (total < buf_size - 1) {
        size_t n = fread(buf + total, 1, (size_t)(buf_size - 1 - total), pipe);
        if (n == 0) break;
        total += (int)n;
    }
    pclose(pipe);
    buf[total] = '\0';
    return total;
}

/* ── Decode MP3 buffer to mono float PCM ───────────────────────────────── */

static int decode_mp3_to_mono_float(const uint8_t *mp3_data, int mp3_size,
                                     float *out, int out_capacity) {
    mp3dec_t dec;
    mp3dec_init(&dec);
    mp3dec_frame_info_t info;
    int total = 0;
    int offset = 0;
    float pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];

    while (offset < mp3_size && total < out_capacity) {
        int samples = mp3dec_decode_frame(&dec,
                          mp3_data + offset,
                          mp3_size - offset,
                          pcm, &info);
        if (samples == 0 && info.frame_bytes == 0) break;
        if (samples > 0) {
            for (int i = 0; i < samples && total < out_capacity; i++) {
                if (info.channels == 2) {
                    out[total++] = (pcm[i * 2] + pcm[i * 2 + 1]) * 0.5f;
                } else {
                    out[total++] = pcm[i];
                }
            }
        }
        offset += info.frame_bytes;
    }
    return total;
}

/* ── Freesound seeder ──────────────────────────────────────────────────── */

/* Try to fetch a sound from Freesound API.
 * Returns number of PCM samples written to `out`, or 0 on failure. */
static int fetch_freesound(fetch_context_t *ctx, float *out, int out_capacity) {
    char api_key[128];
    if (!read_api_key(ctx->module_dir, api_key, sizeof(api_key)))
        return 0;  /* no API key */

    /* Build search query from terrain category + poetic word */
    int terrain = atomic_load(&ctx->terrain);
    if (terrain < 0) terrain = 0;
    if (terrain > 7) terrain = 7;
    int word_idx = xorshift32(&ctx->rng) % 3;
    const char *term = CATEGORY_TERMS[terrain][word_idx];

    /* Also mix in a poetic word sometimes */
    char query[256];
    if (xorshift32(&ctx->rng) % 3 == 0) {
        int pw = xorshift32(&ctx->rng) % N_POETIC_WORDS;
        snprintf(query, sizeof(query), "%s+%s", term, POETIC_WORDS[pw]);
    } else {
        snprintf(query, sizeof(query), "%s", term);
    }

    /* Random page offset to get variety */
    int page = 1 + (xorshift32(&ctx->rng) % 5);

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        CURL_PATH " -s --connect-timeout 5 --max-time 10 "
        "\"https://freesound.org/apiv2/search/text/"
        "?query=%s&filter=duration:%%5B0.5+TO+15%%5D"
        "&fields=id,name,previews&page_size=15&page=%d"
        "&token=%s\"",
        query, page, api_key);

    /* Allocate JSON buffer on heap (too large for stack) */
    char *json = (char *)malloc(JSON_BUF_SIZE);
    if (!json) return 0;

    int json_len = run_cmd_capture(cmd, (uint8_t *)json, JSON_BUF_SIZE);
    if (json_len < 50) { free(json); return 0; }  /* too short = error */

    /* Pick a random result from the response */
    /* Find all "preview-hq-mp3" URLs */
    char preview_urls[15][512];
    int n_urls = 0;
    const char *search_pos = json;
    while (n_urls < 15) {
        int url_len = 0;
        const char *url = json_find_string(search_pos, "preview-hq-mp3", &url_len);
        if (!url || url_len <= 0 || url_len >= 511) break;
        memcpy(preview_urls[n_urls], url, url_len);
        preview_urls[n_urls][url_len] = '\0';
        n_urls++;
        search_pos = url + url_len;
    }
    free(json);

    if (n_urls == 0) return 0;

    /* Pick a random preview */
    int pick = xorshift32(&ctx->rng) % n_urls;

    /* Download the MP3 preview */
    uint8_t *mp3_buf = (uint8_t *)malloc(MP3_BUF_SIZE);
    if (!mp3_buf) return 0;

    snprintf(cmd, sizeof(cmd),
        CURL_PATH " -sL --connect-timeout 5 --max-time 15 \"%s\"",
        preview_urls[pick]);

    int mp3_size = run_cmd_capture(cmd, mp3_buf, MP3_BUF_SIZE);
    if (mp3_size < 1000) { free(mp3_buf); return 0; }  /* too small = error */

    /* Decode MP3 to mono float */
    int samples = decode_mp3_to_mono_float(mp3_buf, mp3_size, out, out_capacity);
    free(mp3_buf);

    return samples;
}

/* ── Internet Archive seeder ───────────────────────────────────────────── */

/* Try to fetch a sound from Internet Archive (no API key needed).
 * Uses the radio-aporee-maps collection for field recordings,
 * or general audio search with poetic terms.
 * Returns number of PCM samples written to `out`, or 0 on failure. */
static int fetch_internet_archive(fetch_context_t *ctx, float *out, int out_capacity) {
    /* Build search query */
    int terrain = atomic_load(&ctx->terrain);
    if (terrain < 0) terrain = 0;
    if (terrain > 7) terrain = 7;

    /* Alternate between aporee field recordings and general audio search */
    char query[256];
    int use_aporee = (xorshift32(&ctx->rng) % 3 == 0);

    if (use_aporee) {
        /* Radio Aporee: field recordings from around the world */
        int pw = xorshift32(&ctx->rng) % N_POETIC_WORDS;
        snprintf(query, sizeof(query),
            "collection:radio-aporee-maps+AND+%s", POETIC_WORDS[pw]);
    } else {
        int word_idx = xorshift32(&ctx->rng) % 3;
        const char *term = CATEGORY_TERMS[terrain][word_idx];
        int pw = xorshift32(&ctx->rng) % N_POETIC_WORDS;
        snprintf(query, sizeof(query),
            "%s+%s+AND+mediatype:audio", term, POETIC_WORDS[pw]);
    }

    int page = 1 + (xorshift32(&ctx->rng) % 3);

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        CURL_PATH " -s --connect-timeout 5 --max-time 10 "
        "\"https://archive.org/advancedsearch.php"
        "?q=%s&fl%%5B%%5D=identifier&fl%%5B%%5D=title"
        "&rows=10&page=%d&output=json\"",
        query, page);

    char *json = (char *)malloc(JSON_BUF_SIZE);
    if (!json) return 0;

    int json_len = run_cmd_capture(cmd, (uint8_t *)json, JSON_BUF_SIZE);
    if (json_len < 50) { free(json); return 0; }

    /* Extract identifiers from response */
    char identifiers[10][256];
    int n_ids = 0;
    const char *search_pos = json;
    while (n_ids < 10) {
        int id_len = 0;
        const char *id = json_find_string(search_pos, "identifier", &id_len);
        if (!id || id_len <= 0 || id_len >= 255) break;
        memcpy(identifiers[n_ids], id, id_len);
        identifiers[n_ids][id_len] = '\0';
        n_ids++;
        search_pos = id + id_len;
    }
    free(json);

    if (n_ids == 0) return 0;

    /* Pick a random item */
    int pick = xorshift32(&ctx->rng) % n_ids;

    /* Get file list for this item — look for an MP3 file */
    snprintf(cmd, sizeof(cmd),
        CURL_PATH " -s --connect-timeout 5 --max-time 10 "
        "\"https://archive.org/metadata/%s/files\"",
        identifiers[pick]);

    json = (char *)malloc(JSON_BUF_SIZE);
    if (!json) return 0;

    json_len = run_cmd_capture(cmd, (uint8_t *)json, JSON_BUF_SIZE);
    if (json_len < 20) { free(json); return 0; }

    /* Find first .mp3 filename in the file list */
    char mp3_filename[256] = {0};
    const char *p = json;
    while ((p = strstr(p, ".mp3")) != NULL) {
        /* Walk back to find the opening quote of this filename */
        const char *end = p + 4;  /* past ".mp3" */
        /* Check this is inside a "name":"..." field */
        const char *name_check = p;
        while (name_check > json && *name_check != '"') name_check--;
        if (name_check > json) {
            const char *start = name_check + 1;
            int fname_len = (int)(end - start);
            if (fname_len > 0 && fname_len < 255) {
                memcpy(mp3_filename, start, fname_len);
                mp3_filename[fname_len] = '\0';
                break;
            }
        }
        p = end;
    }
    free(json);

    if (mp3_filename[0] == '\0') return 0;

    /* URL-encode spaces in filename */
    char encoded_name[512];
    int ei = 0;
    for (int i = 0; mp3_filename[i] && ei < 500; i++) {
        if (mp3_filename[i] == ' ') {
            encoded_name[ei++] = '%';
            encoded_name[ei++] = '2';
            encoded_name[ei++] = '0';
        } else {
            encoded_name[ei++] = mp3_filename[i];
        }
    }
    encoded_name[ei] = '\0';

    /* Download the MP3 file (only first 512KB to limit large files) */
    uint8_t *mp3_buf = (uint8_t *)malloc(MP3_BUF_SIZE);
    if (!mp3_buf) return 0;

    snprintf(cmd, sizeof(cmd),
        CURL_PATH " -sL --connect-timeout 5 --max-time 20 "
        "-r 0-%d "
        "\"https://archive.org/download/%s/%s\"",
        MP3_BUF_SIZE - 1, identifiers[pick], encoded_name);

    int mp3_size = run_cmd_capture(cmd, mp3_buf, MP3_BUF_SIZE);
    if (mp3_size < 1000) { free(mp3_buf); return 0; }

    /* Decode MP3 to mono float */
    int samples = decode_mp3_to_mono_float(mp3_buf, mp3_size, out, out_capacity);
    free(mp3_buf);

    return samples;
}

/* ── Geosonic seeder (Montreal neighbourhoods via radio-aporee-maps) ──── */

/* Searches the radio-aporee-maps collection on Internet Archive for
 * recordings from a specific Montreal neighbourhood.
 * Uses the same MP3 download + decode pipeline as the IA seeder. */
static int fetch_geosonic(fetch_context_t *ctx, float *out, int out_capacity) {
    FILE *dbg = NULL;
    int hood_idx = atomic_load(&ctx->neighbourhood);
    if (hood_idx < 0) hood_idx = 0;
    if (hood_idx >= (int)N_MONTREAL_HOODS) hood_idx = 0;

    const neighbourhood_t *hood = &MONTREAL_HOODS[hood_idx];
    int page = 1 + (xorshift32(&ctx->rng) % 3);

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        CURL_PATH " -s --connect-timeout 5 --max-time 10 "
        "\"https://archive.org/advancedsearch.php"
        "?q=collection:radio-aporee-maps+AND+(%s)"
        "&fl%%5B%%5D=identifier&fl%%5B%%5D=title"
        "&rows=15&page=%d&output=json\"",
        hood->search_terms, page);

    dbg = fopen("/tmp/fouille_fetch.log", "a");
    if (dbg) { fprintf(dbg, "geosonic cmd: %s\n", cmd); fclose(dbg); }

    char *json = (char *)malloc(JSON_BUF_SIZE);
    if (!json) return 0;

    int json_len = run_cmd_capture(cmd, (uint8_t *)json, JSON_BUF_SIZE);
    dbg = fopen("/tmp/fouille_fetch.log", "a");
    if (dbg) { fprintf(dbg, "geosonic search json_len=%d\n", json_len); fclose(dbg); }
    if (json_len < 50) { free(json); return 0; }

    /* Extract identifiers */
    char identifiers[15][256];
    int n_ids = 0;
    const char *search_pos = json;
    while (n_ids < 15) {
        int id_len = 0;
        const char *id = json_find_string(search_pos, "identifier", &id_len);
        if (!id || id_len <= 0 || id_len >= 255) break;
        memcpy(identifiers[n_ids], id, id_len);
        identifiers[n_ids][id_len] = '\0';
        n_ids++;
        search_pos = id + id_len;
    }
    free(json);

    /* If neighbourhood-specific search found nothing, try general Montreal */
    if (n_ids == 0) {
        snprintf(cmd, sizeof(cmd),
            CURL_PATH " -s --connect-timeout 5 --max-time 10 "
            "\"https://archive.org/advancedsearch.php"
            "?q=collection:radio-aporee-maps+AND+montreal"
            "&fl%%5B%%5D=identifier&fl%%5B%%5D=title"
            "&rows=15&page=%d&output=json\"",
            page);

        json = (char *)malloc(JSON_BUF_SIZE);
        if (!json) return 0;

        json_len = run_cmd_capture(cmd, (uint8_t *)json, JSON_BUF_SIZE);
        if (json_len < 50) { free(json); return 0; }

        search_pos = json;
        while (n_ids < 15) {
            int id_len = 0;
            const char *id = json_find_string(search_pos, "identifier", &id_len);
            if (!id || id_len <= 0 || id_len >= 255) break;
            memcpy(identifiers[n_ids], id, id_len);
            identifiers[n_ids][id_len] = '\0';
            n_ids++;
            search_pos = id + id_len;
        }
        free(json);
    }

    dbg = fopen("/tmp/fouille_fetch.log", "a");
    if (dbg) { fprintf(dbg, "geosonic n_ids=%d\n", n_ids); fclose(dbg); }
    if (n_ids == 0) return 0;

    /* Pick a random item */
    int pick = xorshift32(&ctx->rng) % n_ids;
    dbg = fopen("/tmp/fouille_fetch.log", "a");
    if (dbg) { fprintf(dbg, "geosonic pick=%d id=%s\n", pick, identifiers[pick]); fclose(dbg); }

    /* Get file list — look for an MP3 */
    snprintf(cmd, sizeof(cmd),
        CURL_PATH " -s --connect-timeout 5 --max-time 10 "
        "\"https://archive.org/metadata/%s/files\"",
        identifiers[pick]);

    json = (char *)malloc(JSON_BUF_SIZE);
    if (!json) return 0;

    json_len = run_cmd_capture(cmd, (uint8_t *)json, JSON_BUF_SIZE);
    if (json_len < 20) { free(json); return 0; }

    /* Find first .mp3 filename */
    char mp3_filename[256] = {0};
    const char *p = json;
    while ((p = strstr(p, ".mp3")) != NULL) {
        const char *end = p + 4;
        const char *name_check = p;
        while (name_check > json && *name_check != '"') name_check--;
        if (name_check > json) {
            const char *start = name_check + 1;
            int fname_len = (int)(end - start);
            if (fname_len > 0 && fname_len < 255) {
                memcpy(mp3_filename, start, fname_len);
                mp3_filename[fname_len] = '\0';
                break;
            }
        }
        p = end;
    }
    free(json);

    dbg = fopen("/tmp/fouille_fetch.log", "a");
    if (dbg) { fprintf(dbg, "geosonic mp3=%s\n", mp3_filename[0] ? mp3_filename : "(none)"); fclose(dbg); }
    if (mp3_filename[0] == '\0') return 0;

    /* URL-encode spaces */
    char encoded_name[512];
    int ei = 0;
    for (int i = 0; mp3_filename[i] && ei < 500; i++) {
        if (mp3_filename[i] == ' ') {
            encoded_name[ei++] = '%'; encoded_name[ei++] = '2'; encoded_name[ei++] = '0';
        } else {
            encoded_name[ei++] = mp3_filename[i];
        }
    }
    encoded_name[ei] = '\0';

    /* Download MP3 */
    uint8_t *mp3_buf = (uint8_t *)malloc(MP3_BUF_SIZE);
    if (!mp3_buf) return 0;

    snprintf(cmd, sizeof(cmd),
        CURL_PATH " -sL --connect-timeout 5 --max-time 20 "
        "-r 0-%d "
        "\"https://archive.org/download/%s/%s\"",
        MP3_BUF_SIZE - 1, identifiers[pick], encoded_name);

    int mp3_size = run_cmd_capture(cmd, mp3_buf, MP3_BUF_SIZE);
    dbg = fopen("/tmp/fouille_fetch.log", "a");
    if (dbg) { fprintf(dbg, "geosonic mp3_size=%d\n", mp3_size); fclose(dbg); }
    if (mp3_size < 1000) { free(mp3_buf); return 0; }

    int samples = decode_mp3_to_mono_float(mp3_buf, mp3_size, out, out_capacity);
    dbg = fopen("/tmp/fouille_fetch.log", "a");
    if (dbg) { fprintf(dbg, "geosonic decoded=%d samples\n", samples); fclose(dbg); }
    free(mp3_buf);

    return samples;
}

/* ── Main fetch thread ─────────────────────────────────────────────────── */

static void *fetch_thread_func(void *arg) {
    fetch_context_t *ctx = (fetch_context_t *)arg;
    int use_ia_next = 0;  /* alternate between Freesound and IA */

    while (atomic_load(&ctx->running)) {
        int flow_sec = atomic_load(&ctx->flow_seconds);
        if (flow_sec < 5) flow_sec = 5;

        /* Sleep in 1-second intervals, checking for shutdown and urgent requests */
        for (int s = 0; s < flow_sec && atomic_load(&ctx->running); s++) {
            sleep(1);
            if (atomic_load(&ctx->fetch_requested)) {
                atomic_store(&ctx->fetch_requested, 0);
                break;
            }
        }

        if (!atomic_load(&ctx->running)) break;

        /* Find the target slot (round-robin, skip if currently playing) */
        int target = -1;
        for (int attempt = 0; attempt < N_SLOTS; attempt++) {
            int idx = (ctx->next_slot + attempt) % N_SLOTS;
            int st = atomic_load(&ctx->slots[idx].state);
            if (st != SLOT_PLAYING && st != SLOT_LOADING) {
                target = idx;
                break;
            }
        }
        if (target < 0) continue;

        atomic_store(&ctx->slots[target].state, SLOT_LOADING);

        /* Determine which buffer to write to (shadow buffer) */
        int live = atomic_load(&ctx->slots[target].active_buf);
        float *shadow = (live == 0) ? ctx->slots[target].pcm_b : ctx->slots[target].pcm_a;

        /* Try to fetch from the internet */
        int len = 0;
        int current_mode = atomic_load(&ctx->mode);

        /* Debug log */
        {
            FILE *dbg = fopen("/tmp/fouille_fetch.log", "a");
            if (dbg) {
                fprintf(dbg, "fetch: mode=%d hood=%d target=%d\n",
                        current_mode, atomic_load(&ctx->neighbourhood), target);
                fclose(dbg);
            }
        }

        if (current_mode == 1) {
            /* Cities mode — geosonic seeder */
            len = fetch_geosonic(ctx, shadow, MAX_SLOT_SAMPLES);
            {
                FILE *dbg = fopen("/tmp/fouille_fetch.log", "a");
                if (dbg) { fprintf(dbg, "geosonic returned %d samples\n", len); fclose(dbg); }
            }
        } else {
            /* Fouille mode — alternate Freesound and IA */
            if (use_ia_next) {
                len = fetch_internet_archive(ctx, shadow, MAX_SLOT_SAMPLES);
                if (len < 100)
                    len = fetch_freesound(ctx, shadow, MAX_SLOT_SAMPLES);
            } else {
                len = fetch_freesound(ctx, shadow, MAX_SLOT_SAMPLES);
                if (len < 100)
                    len = fetch_internet_archive(ctx, shadow, MAX_SLOT_SAMPLES);
            }
            use_ia_next = !use_ia_next;
        }

        /* If both seeders failed, generate synthetic texture as fallback */
        if (len < 100) {
            len = 22050 * (2 + (xorshift32(&ctx->rng) % 8));
            if (len > MAX_SLOT_SAMPLES) len = MAX_SLOT_SAMPLES;
            generate_synthetic_texture(shadow, len, &ctx->rng);
        }

        /* Compute descriptors */
        float centroid, loudness, noisiness;
        compute_descriptors(shadow, len, &centroid, &loudness, &noisiness);

        /* Commit: update metadata and swap buffer */
        ctx->slots[target].length = len;
        ctx->slots[target].centroid = centroid;
        ctx->slots[target].loudness = loudness;
        ctx->slots[target].noisiness = noisiness;
        ctx->slots[target].fetch_time = ctx->tick++;
        atomic_store_explicit(&ctx->slots[target].active_buf,
                              live == 0 ? 1 : 0, memory_order_release);
        atomic_store_explicit(&ctx->slots[target].state,
                              SLOT_READY, memory_order_release);

        ctx->next_slot = (target + 1) % N_SLOTS;
    }

    return NULL;
}

/* ── Corpus Navigator ───────────────────────────────────────────────────── */

/*
 * Map a MIDI note to a descriptor coordinate and find the closest sound slot.
 *
 * The Move sends notes based on the user's scale/root settings, NOT fixed pad
 * positions. So we use the note pitch itself to navigate the descriptor space:
 *   - Note pitch (0-127) → target centroid (dark → bright)
 *   - Velocity → target loudness (quiet → loud)
 *
 * Lower notes seek darker sounds, higher notes seek brighter sounds.
 * Soft hits seek quiet sounds, hard hits seek loud sounds.
 */

static int corpus_find_nearest_slot(fouille_instance_t *inst, float target_centroid, float target_loudness) {

    int best = -1;
    float best_dist = 1e9f;

    for (int i = 0; i < N_SLOTS; i++) {
        int st = atomic_load(&inst->slots[i].state);
        if (st != SLOT_READY && st != SLOT_PLAYING) continue;

        float dl = inst->slots[i].loudness - target_loudness;
        float dc = inst->slots[i].centroid - target_centroid;
        float dist = dl * dl + dc * dc;
        if (dist < best_dist) {
            best_dist = dist;
            best = i;
        }
    }

    /* Fallback: -1 if no material available */
    return best;
}

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

static void *create_instance(const char *module_dir, const char *json_defaults) {
    fouille_instance_t *inst = calloc(1, sizeof(fouille_instance_t));
    if (!inst) return NULL;

    /* Allocate sound slot buffers */
    for (int i = 0; i < N_SLOTS; i++) {
        inst->slots[i].pcm_a = calloc(MAX_SLOT_SAMPLES, sizeof(float));
        inst->slots[i].pcm_b = calloc(MAX_SLOT_SAMPLES, sizeof(float));
        if (!inst->slots[i].pcm_a || !inst->slots[i].pcm_b) {
            /* Allocation failed — clean up and bail */
            for (int j = 0; j <= i; j++) {
                free(inst->slots[j].pcm_a);
                free(inst->slots[j].pcm_b);
            }
            free(inst);
            return NULL;
        }
        atomic_store(&inst->slots[i].state, SLOT_EMPTY);
        atomic_store(&inst->slots[i].active_buf, 0);
    }

    /* Allocate Echo Pool */
    inst->echo_pool = calloc(ECHO_POOL_SIZE, sizeof(float));

    /* Initialize voices */
    for (int i = 0; i < N_VOICES; i++) {
        inst->voices[i].drift_pos.rng = 0xDEAD0000 + i * 7919;
        inst->voices[i].drift_pan.rng = 0xBEEF0000 + i * 6271;
    }

    /* Default parameters — lowercase headroom: volume starts low */
    inst->param_terrain  = 0.0f;
    inst->param_depth    = 0.5f;
    inst->param_rise     = 0.1f;    /* ~200ms attack */
    inst->param_hold     = 0.5f;    /* drift mode */
    inst->param_fall     = 0.3f;    /* ~1.5s decay */
    inst->param_strike   = 0.3f;    /* moderate nonlinearity */
    inst->param_lock     = 0.0f;    /* spectral lock bypassed */
    inst->param_erode    = 0.0f;    /* no erosion */

    inst->param_flow     = 0.15f;   /* ~22s rotation */
    inst->param_scatter  = 0.5f;
    inst->param_stretch  = 0.5f;    /* 1x playback */
    inst->param_grain    = 0.2f;    /* ~40ms grains */
    inst->param_drift    = 0.3f;    /* moderate drift */
    inst->param_pool     = 0.0f;    /* echo pool off */
    inst->param_spread   = 0.7f;    /* wide stereo */
    inst->param_volume   = 0.6f;    /* moderate default — audible but not hot */

    inst->rng = 0x12345678;

    /* Pre-fill all slots with synthetic textures so sound is immediate */
    {
        uint32_t init_rng = 0xCAFE0000;
        for (int i = 0; i < N_SLOTS; i++) {
            int len = 22050 * (2 + (xorshift32(&init_rng) % 8));
            if (len > MAX_SLOT_SAMPLES) len = MAX_SLOT_SAMPLES;
            generate_synthetic_texture(inst->slots[i].pcm_a, len, &init_rng);
            inst->slots[i].length = len;
            float centroid, loudness, noisiness;
            compute_descriptors(inst->slots[i].pcm_a, len, &centroid, &loudness, &noisiness);
            inst->slots[i].centroid = centroid;
            inst->slots[i].loudness = loudness;
            inst->slots[i].noisiness = noisiness;
            atomic_store(&inst->slots[i].active_buf, 0);
            atomic_store(&inst->slots[i].state, SLOT_READY);
        }
    }

    /* Start fetch thread (will rotate in new textures over time) */
    inst->fetch.slots = inst->slots;
    inst->fetch.rng = 0xFE7C0000;
    inst->fetch.next_slot = 0;
    atomic_store(&inst->fetch.running, 1);
    atomic_store(&inst->fetch.fetch_requested, 0);
    atomic_store(&inst->fetch.terrain, 0);
    atomic_store(&inst->fetch.depth_pct, 50);
    atomic_store(&inst->fetch.flow_seconds, 20);
    if (module_dir) {
        strncpy(inst->fetch.module_dir, module_dir, sizeof(inst->fetch.module_dir) - 1);
    }
    pthread_create(&inst->fetch.thread, NULL, fetch_thread_func, &inst->fetch);

    return inst;
}

static void destroy_instance(void *instance) {
    fouille_instance_t *inst = (fouille_instance_t *)instance;
    if (!inst) return;

    /* Stop fetch thread */
    atomic_store(&inst->fetch.running, 0);
    pthread_join(inst->fetch.thread, NULL);

    /* Free buffers */
    for (int i = 0; i < N_SLOTS; i++) {
        free(inst->slots[i].pcm_a);
        free(inst->slots[i].pcm_b);
    }
    free(inst->echo_pool);
    free(inst);
}

/* ── MIDI ───────────────────────────────────────────────────────────────── */

static void on_midi(void *instance, const uint8_t *msg, int len, int source) {
    fouille_instance_t *inst = (fouille_instance_t *)instance;
    if (len < 3) return;

    uint8_t status = msg[0] & 0xF0;
    uint8_t note   = msg[1];
    uint8_t vel    = msg[2];

    if (status == 0x90 && vel > 0) {
        /* Note On — find a voice (round-robin steal) */
        int vi = inst->voice_cursor;
        inst->voice_cursor = (vi + 1) % N_VOICES;
        voice_t *v = &inst->voices[vi];

        /* Corpus Navigator: note pitch → centroid, velocity → loudness */
        float target_centroid = clampf((float)(note - 36) / 48.0f, 0.0f, 1.0f);
        float target_loudness = vel / 127.0f;
        int slot = corpus_find_nearest_slot(inst, target_centroid, target_loudness);
        if (slot < 0) return;  /* no material available yet */

        v->active = 1;
        v->note = note;
        v->velocity = vel / 127.0f;
        v->slot_idx = slot;
        atomic_store(&inst->slots[slot].state, SLOT_PLAYING);

        /* Compute playback speed from Stretch param */
        /* 0.0→0.25x, 0.5→1x, 1.0→4x (exponential) */
        float stretch_exp = powf(2.0f, (inst->param_stretch - 0.5f) * 4.0f);
        v->play_speed = stretch_exp;

        /* Slice position: note pitch maps into the sound + velocity scatter */
        int slot_len = inst->slots[slot].length;
        if (slot_len < 1) slot_len = 1;
        float base_pos = target_centroid;  /* pitch maps to position in the sound */
        float scatter = inst->param_scatter * v->velocity * (randf(&inst->rng) - 0.5f) * 2.0f;
        float start_frac = clampf(base_pos + scatter * 0.3f, 0.0f, 0.95f);
        v->read_pos = start_frac * (float)slot_len;

        /* Ternary envelope: compute rates from params */
        float rise_time = 0.0005f + inst->param_rise * 1.9995f;   /* 0.5ms → 2s */
        float fall_time = 0.001f + inst->param_fall * 4.999f;     /* 1ms → 5s */
        v->env_rise_rate = 1.0f / (rise_time * SAMPLE_RATE);
        v->env_fall_rate = 1.0f / (fall_time * SAMPLE_RATE);
        v->env_stage = ENV_RISE;
        v->env_level = 0.0f;

        /* Reset LPG */
        vactrol_reset(&v->lpg);

        /* Pan: note pitch maps to stereo field + spread */
        float base_pan = target_centroid * 2.0f - 1.0f;
        v->pan = base_pan * inst->param_spread;

        /* Grain state for HOLD_GRANULATE */
        v->grain_origin = v->read_pos;
        float grain_ms = 1.0f + inst->param_grain * 199.0f;
        v->grain_size_samples = grain_ms * 0.001f * SAMPLE_RATE;
        v->grain_counter = (int)v->grain_size_samples;

        /* Trigger urgent fetch on hard velocity */
        if (vel > 100) {
            atomic_store(&inst->fetch.fetch_requested, 1);
        }

    } else if (status == 0x80 || (status == 0x90 && vel == 0)) {
        /* Note Off — release matching voices */
        for (int i = 0; i < N_VOICES; i++) {
            if (inst->voices[i].active && inst->voices[i].note == note) {
                inst->voices[i].env_stage = ENV_FALL;
            }
        }
    }
}

/* ── Parameters ─────────────────────────────────────────────────────────── */

/* Page definitions for knob overlay */
typedef struct { const char *key; const char *label; float min, max, step; } knob_def_t;

static const knob_def_t PAGE_EXCAVATION[8] = {
    {"terrain", "Terrain",  0, 1, 0.01f},
    {"depth",   "Depth",    0, 1, 0.01f},
    {"rise",    "Rise",     0, 1, 0.01f},
    {"hold",    "Hold",     0, 1, 0.01f},
    {"fall",    "Fall",     0, 1, 0.01f},
    {"strike",  "Strike",   0, 1, 0.01f},
    {"lock",    "Lock",     0, 1, 0.01f},
    {"erode",   "Erode",    0, 1, 0.01f},
};

static const knob_def_t PAGE_CURRENTS[8] = {
    {"flow",    "Flow",     0, 1, 0.01f},
    {"scatter", "Scatter",  0, 1, 0.01f},
    {"stretch", "Stretch",  0, 1, 0.01f},
    {"grain",   "Grain",    0, 1, 0.01f},
    {"drift",   "Drift",    0, 1, 0.01f},
    {"pool",    "Pool",     0, 1, 0.01f},
    {"spread",  "Spread",   0, 1, 0.01f},
    {"volume",  "Volume",   0, 1, 0.01f},
};

static const knob_def_t PAGE_LOCATION[8] = {
    {"mode",    "Mode",     0, 1, 0.01f},
    {"city",    "City",     0, 1, 0.01f},
    {"hood",    "Hood",     0, 1, 0.01f},
    {"rise",    "Rise",     0, 1, 0.01f},
    {"hold",    "Hold",     0, 1, 0.01f},
    {"fall",    "Fall",     0, 1, 0.01f},
    {"flow",    "Flow",     0, 1, 0.01f},
    {"volume",  "Volume",   0, 1, 0.01f},
};

static float *get_param_ptr(fouille_instance_t *inst, const char *key) {
    if (strcmp(key, "terrain")  == 0) return &inst->param_terrain;
    if (strcmp(key, "depth")    == 0) return &inst->param_depth;
    if (strcmp(key, "rise")     == 0) return &inst->param_rise;
    if (strcmp(key, "hold")     == 0) return &inst->param_hold;
    if (strcmp(key, "fall")     == 0) return &inst->param_fall;
    if (strcmp(key, "strike")   == 0) return &inst->param_strike;
    if (strcmp(key, "lock")     == 0) return &inst->param_lock;
    if (strcmp(key, "erode")    == 0) return &inst->param_erode;
    if (strcmp(key, "flow")     == 0) return &inst->param_flow;
    if (strcmp(key, "scatter")  == 0) return &inst->param_scatter;
    if (strcmp(key, "stretch")  == 0) return &inst->param_stretch;
    if (strcmp(key, "grain")    == 0) return &inst->param_grain;
    if (strcmp(key, "drift")    == 0) return &inst->param_drift;
    if (strcmp(key, "pool")     == 0) return &inst->param_pool;
    if (strcmp(key, "spread")   == 0) return &inst->param_spread;
    if (strcmp(key, "volume")   == 0) return &inst->param_volume;
    /* mode and hood are enums — handled separately in get_param/set_param */
    return NULL;
}

static const knob_def_t *get_active_page(fouille_instance_t *inst, int *page_out) {
    if (page_out) *page_out = inst->current_page;
    if (inst->current_page == 0) return PAGE_EXCAVATION;
    if (inst->current_page == 2) return PAGE_LOCATION;
    return PAGE_CURRENTS;
}

static void set_param(void *instance, const char *key, const char *val) {
    fouille_instance_t *inst = (fouille_instance_t *)instance;
    if (!inst || !key || !val) return;

    /* Knob adjust (from Shadow UI knob turns) */
    if (strncmp(key, "knob_", 5) == 0 && strstr(key, "_adjust")) {
        int idx = atoi(key + 5) - 1;
        if (idx >= 0 && idx < 8) {
            const knob_def_t *page = get_active_page(inst, NULL);
            float *p = get_param_ptr(inst, page[idx].key);
            if (p) {
                int delta = atoi(val);
                *p = clampf(*p + delta * page[idx].step, page[idx].min, page[idx].max);
            }
        }
        return;
    }

    /* Page navigation */
    if (strcmp(key, "page") == 0) {
        int pg = atoi(val);
        inst->current_page = (pg >= 0 && pg <= 2) ? pg : 0;
        return;
    }

    /* State deserialization (must come before fetch sync) */
    if (strcmp(key, "state") == 0) {
        sscanf(val, "%f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f",
               &inst->param_terrain, &inst->param_depth,
               &inst->param_rise, &inst->param_hold,
               &inst->param_fall, &inst->param_strike,
               &inst->param_lock, &inst->param_erode,
               &inst->param_flow, &inst->param_scatter,
               &inst->param_stretch, &inst->param_grain,
               &inst->param_drift, &inst->param_pool,
               &inst->param_spread, &inst->param_volume,
               &inst->param_mode, &inst->param_hood);
    } else if (strcmp(key, "mode") == 0) {
        /* Enum: "Fouille" → 0, "Cities" → 1 */
        if (strcmp(val, "Cities") == 0) inst->param_mode = 1.0f;
        else inst->param_mode = 0.0f;
    } else if (strcmp(key, "city") == 0) {
        /* Enum: only "Montreal" for now → 0 */
        inst->param_city = 0.0f;
    } else if (strcmp(key, "hood") == 0) {
        /* Enum: match neighbourhood name to index */
        for (int i = 0; i < (int)N_MONTREAL_HOODS; i++) {
            if (strcmp(val, MONTREAL_HOODS[i].name) == 0) {
                inst->param_hood = (float)i / (float)(N_MONTREAL_HOODS - 1);
                break;
            }
        }
    } else {
        /* Direct param set */
        float *p = get_param_ptr(inst, key);
        if (p) {
            *p = clampf(atof(val), 0.0f, 1.0f);
        }
    }

    /* Sync fetch thread params */
    atomic_store(&inst->fetch.terrain, (int)(inst->param_terrain * 7.99f));
    atomic_store(&inst->fetch.depth_pct, (int)(inst->param_depth * 100.0f));
    atomic_store(&inst->fetch.flow_seconds, 5 + (int)(inst->param_flow * 115.0f));
    {
        int new_mode = (inst->param_mode >= 0.5f) ? 1 : 0;
        int old_mode = atomic_load(&inst->fetch.mode);
        atomic_store(&inst->fetch.mode, new_mode);
        int new_hood = (int)(inst->param_hood * (float)(N_MONTREAL_HOODS - 1) + 0.5f);
        int old_hood = atomic_load(&inst->fetch.neighbourhood);
        atomic_store(&inst->fetch.neighbourhood, new_hood);
        /* Force immediate fetch when mode or neighbourhood changes */
        if (new_mode != old_mode || new_hood != old_hood) {
            atomic_store(&inst->fetch.fetch_requested, 1);
        }
    }
}

static int get_param(void *instance, const char *key, char *buf, int buf_len) {
    fouille_instance_t *inst = (fouille_instance_t *)instance;

    /* chain_params — memcpy pattern to avoid truncation */
    if (strcmp(key, "chain_params") == 0) {
        static const char *cp =
            "["
            "{\"key\":\"terrain\",\"name\":\"Terrain\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"depth\",\"name\":\"Depth\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"rise\",\"name\":\"Rise\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"hold\",\"name\":\"Hold\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"fall\",\"name\":\"Fall\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"strike\",\"name\":\"Strike\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"lock\",\"name\":\"Lock\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"erode\",\"name\":\"Erode\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"flow\",\"name\":\"Flow\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"scatter\",\"name\":\"Scatter\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"stretch\",\"name\":\"Stretch\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"grain\",\"name\":\"Grain\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"drift\",\"name\":\"Drift\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"pool\",\"name\":\"Pool\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"spread\",\"name\":\"Spread\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"volume\",\"name\":\"Volume\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"mode\",\"name\":\"Mode\",\"type\":\"enum\",\"options\":[\"Fouille\",\"Cities\"]},"
            "{\"key\":\"city\",\"name\":\"City\",\"type\":\"enum\",\"options\":[\"Montreal\"]},"
            "{\"key\":\"hood\",\"name\":\"Hood\",\"type\":\"enum\",\"options\":[\"Plateau\",\"Mile End\",\"St-Henri\",\"Hochelaga\",\"Vieux-Mtl\",\"Centre-Ville\",\"Verdun\",\"Rosemont\",\"NDG\",\"Villeray\",\"Griffintown\",\"Outremont\",\"Parc Jarry\",\"Mont-Royal\",\"Lachine\",\"Ahuntsic\"]}"
            "]";
        int len = (int)strlen(cp);
        if (len >= buf_len) return -1;
        memcpy(buf, cp, len + 1);
        return len;
    }

    /* ui_hierarchy — MUST be in get_param for synths (module.json alone not enough) */
    if (strcmp(key, "ui_hierarchy") == 0) {
        static const char *hier =
            "{\"levels\":{"
            "\"root\":{\"name\":\"Fouille\","
            "\"knobs\":[\"terrain\",\"depth\",\"rise\",\"hold\",\"fall\",\"strike\",\"lock\",\"erode\"],"
            "\"params\":[{\"level\":\"Excavation\",\"label\":\"Excavation\"},{\"level\":\"Currents\",\"label\":\"Currents\"},{\"level\":\"Location\",\"label\":\"Location\"}]},"
            "\"Excavation\":{\"label\":\"Excavation\","
            "\"knobs\":[\"terrain\",\"depth\",\"rise\",\"hold\",\"fall\",\"strike\",\"lock\",\"erode\"],"
            "\"params\":[\"terrain\",\"depth\",\"rise\",\"hold\",\"fall\",\"strike\",\"lock\",\"erode\"]},"
            "\"Currents\":{\"label\":\"Currents\","
            "\"knobs\":[\"flow\",\"scatter\",\"stretch\",\"grain\",\"drift\",\"pool\",\"spread\",\"volume\"],"
            "\"params\":[\"flow\",\"scatter\",\"stretch\",\"grain\",\"drift\",\"pool\",\"spread\",\"volume\"]},"
            "\"Location\":{\"label\":\"Location\","
            "\"knobs\":[\"mode\",\"city\",\"hood\",\"rise\",\"hold\",\"fall\",\"flow\",\"volume\"],"
            "\"params\":[\"mode\",\"city\",\"hood\",\"rise\",\"hold\",\"fall\",\"flow\",\"volume\"]}"
            "}}";
        int len = (int)strlen(hier);
        if (len >= buf_len) return -1;
        memcpy(buf, hier, len + 1);
        return len;
    }

    /* Knob names (Shadow UI popup) */
    if (strncmp(key, "knob_", 5) == 0 && strstr(key, "_name")) {
        int idx = atoi(key + 5) - 1;
        if (idx >= 0 && idx < 8) {
            const knob_def_t *page = get_active_page(inst, NULL);
            return snprintf(buf, buf_len, "%s", page[idx].label);
        }
        return -1;
    }

    /* Knob values (Shadow UI popup) */
    if (strncmp(key, "knob_", 5) == 0 && strstr(key, "_value")) {
        int idx = atoi(key + 5) - 1;
        if (idx >= 0 && idx < 8) {
            const knob_def_t *page = get_active_page(inst, NULL);
            float *p = get_param_ptr(inst, page[idx].key);
            if (p) {
                /* Special display for Terrain (category name) */
                if (strcmp(page[idx].key, "terrain") == 0) {
                    const char *names[] = {"Nature","Machine","Voice","Abstract","Material","Urban","Organic","Cosmic"};
                    int cat = (int)(*p * 7.99f);
                    return snprintf(buf, buf_len, "%s", names[cat]);
                }
                /* Special display for Hold (mode name) */
                if (strcmp(page[idx].key, "hold") == 0) {
                    if (*p < 0.33f) return snprintf(buf, buf_len, "Freeze");
                    if (*p < 0.66f) return snprintf(buf, buf_len, "Drift");
                    return snprintf(buf, buf_len, "Grain");
                }
                /* Special display for Mode */
                if (strcmp(page[idx].key, "mode") == 0) {
                    return snprintf(buf, buf_len, "%s", (*p < 0.5f) ? "Fouille" : "Cities");
                }
                /* Special display for City */
                if (strcmp(page[idx].key, "city") == 0) {
                    return snprintf(buf, buf_len, "Montreal");
                }
                /* Special display for Neighbourhood */
                if (strcmp(page[idx].key, "hood") == 0) {
                    int hi = (int)(*p * (float)(N_MONTREAL_HOODS - 1) + 0.5f);
                    if (hi < 0) hi = 0;
                    if (hi >= (int)N_MONTREAL_HOODS) hi = (int)N_MONTREAL_HOODS - 1;
                    return snprintf(buf, buf_len, "%s", MONTREAL_HOODS[hi].name);
                }
                return snprintf(buf, buf_len, "%d%%", (int)(*p * 100));
            }
        }
        return -1;
    }

    /* Module name */
    if (strcmp(key, "name") == 0) return snprintf(buf, buf_len, "Fouille");

    /* Status (for Shadow UI display) */
    if (strcmp(key, "status") == 0) {
        int ready = 0;
        for (int i = 0; i < N_SLOTS; i++) {
            int st = atomic_load(&inst->slots[i].state);
            if (st == SLOT_READY || st == SLOT_PLAYING) ready++;
        }
        return snprintf(buf, buf_len, "%d/%d slots", ready, N_SLOTS);
    }

    /* Enum param: mode */
    if (strcmp(key, "mode") == 0) {
        return snprintf(buf, buf_len, "%s", (inst->param_mode >= 0.5f) ? "Cities" : "Fouille");
    }

    /* Enum param: city */
    if (strcmp(key, "city") == 0) {
        return snprintf(buf, buf_len, "Montreal");
    }

    /* Enum param: hood */
    if (strcmp(key, "hood") == 0) {
        int hi = (int)(inst->param_hood * (float)(N_MONTREAL_HOODS - 1) + 0.5f);
        if (hi < 0) hi = 0;
        if (hi >= (int)N_MONTREAL_HOODS) hi = (int)N_MONTREAL_HOODS - 1;
        return snprintf(buf, buf_len, "%s", MONTREAL_HOODS[hi].name);
    }

    /* Individual param values */
    float *p = get_param_ptr(inst, key);
    if (p) return snprintf(buf, buf_len, "%.4f", *p);

    /* State serialization */
    if (strcmp(key, "state") == 0) {
        return snprintf(buf, buf_len,
            "%.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g",
            inst->param_terrain, inst->param_depth,
            inst->param_rise, inst->param_hold,
            inst->param_fall, inst->param_strike,
            inst->param_lock, inst->param_erode,
            inst->param_flow, inst->param_scatter,
            inst->param_stretch, inst->param_grain,
            inst->param_drift, inst->param_pool,
            inst->param_spread, inst->param_volume,
            inst->param_mode, inst->param_hood);
    }

    return -1;  /* unknown key */
}

/* ── Audio Processing ───────────────────────────────────────────────────── */

/* Fast tanh approximation: x / (1 + |x|) — no libm call */
static inline float fast_tanh(float x) {
    return x / (1.0f + fabsf(x));
}

static void render_block(void *instance, int16_t *out_lr, int frames) {
    fouille_instance_t *inst = (fouille_instance_t *)instance;
    if (frames > BLOCK_SIZE) frames = BLOCK_SIZE;

    /* Vactrol model parameters derived from knobs */
    float rise_rate = 0.01f + (1.0f - inst->param_rise) * 0.09f;   /* faster rise → higher rate */
    float fall_rate = 0.0001f + (1.0f - inst->param_fall) * 0.005f; /* slower fall → lower rate */
    float nonlinearity = inst->param_strike;
    float drift_depth = inst->param_drift;

    /* Hold mode */
    hold_mode_t hold_mode = HOLD_FREEZE;
    if (inst->param_hold >= 0.33f && inst->param_hold < 0.66f) hold_mode = HOLD_DRIFT;
    else if (inst->param_hold >= 0.66f) hold_mode = HOLD_GRANULATE;

    float mix_buf_l[BLOCK_SIZE];
    float mix_buf_r[BLOCK_SIZE];
    memset(mix_buf_l, 0, sizeof(mix_buf_l));
    memset(mix_buf_r, 0, sizeof(mix_buf_r));

    /* Process each voice */
    for (int vi = 0; vi < N_VOICES; vi++) {
        voice_t *v = &inst->voices[vi];
        if (!v->active) continue;

        sound_slot_t *slot = &inst->slots[v->slot_idx];
        int live_buf = atomic_load(&slot->active_buf);
        float *pcm = (live_buf == 0) ? slot->pcm_a : slot->pcm_b;
        int slot_len = slot->length;
        if (slot_len < 2) { v->active = 0; continue; }

        /* Pre-compute per-voice constants (hoisted from per-sample loop) */
        float vel_shaped = powf(v->velocity, 1.0f + nonlinearity);
        float pan = clampf(v->pan + drift_process(&v->drift_pan, drift_depth * 0.3f), -1.0f, 1.0f);
        float pan_angle = (pan + 1.0f) * 0.25f * (float)M_PI;
        float pan_gain_l = cosf(pan_angle);
        float pan_gain_r = sinf(pan_angle);

        for (int i = 0; i < frames; i++) {
            /* ── Ternary Envelope ── */
            switch (v->env_stage) {
                case ENV_RISE:
                    v->env_level += v->env_rise_rate;
                    if (v->env_level >= 1.0f) {
                        v->env_level = 1.0f;
                        v->env_stage = ENV_HOLD;
                    }
                    break;
                case ENV_HOLD:
                    /* Sustain: behavior depends on hold_mode */
                    break;
                case ENV_FALL:
                    v->env_level -= v->env_fall_rate;
                    if (v->env_level <= 0.0f) {
                        v->env_level = 0.0f;
                        v->env_stage = ENV_OFF;
                        v->active = 0;
                        /* Mark slot as ready again (not playing) */
                        atomic_store(&slot->state, SLOT_READY);
                    }
                    break;
                default:
                    break;
            }

            if (!v->active) break;

            /* ── Read sample from slot buffer ── */
            int pos_int = (int)v->read_pos;
            float pos_frac = v->read_pos - (float)pos_int;
            if (pos_int < 0) pos_int = 0;
            if (pos_int >= slot_len - 1) pos_int = slot_len - 2;

            /* Linear interpolation */
            float sample = pcm[pos_int] * (1.0f - pos_frac) + pcm[pos_int + 1] * pos_frac;

            /* ── Advance read position (depends on hold mode during HOLD) ── */
            if (v->env_stage == ENV_HOLD) {
                switch (hold_mode) {
                    case HOLD_FREEZE:
                        /* Very slow crawl — not dead stop, avoids DC */
                        v->read_pos += v->play_speed * 0.02f;
                        break;
                    case HOLD_DRIFT:
                        /* Slow scan with organic drift */
                        v->read_pos += v->play_speed * 0.1f;
                        v->read_pos += drift_process(&v->drift_pos, drift_depth) * 10.0f;
                        break;
                    case HOLD_GRANULATE:
                        /* Micro-loop around grain_origin */
                        v->read_pos += v->play_speed;
                        if (v->read_pos > v->grain_origin + v->grain_size_samples) {
                            v->read_pos = v->grain_origin;
                            /* Jitter the grain origin slightly */
                            v->grain_origin += (randf(&inst->rng) - 0.5f) * v->grain_size_samples * 0.3f;
                            v->grain_origin = clampf(v->grain_origin, 0, (float)(slot_len - 1));
                        }
                        break;
                }
            } else {
                /* During RISE and FALL: normal playback + drift */
                v->read_pos += v->play_speed;
                v->read_pos += drift_process(&v->drift_pos, drift_depth * 0.5f) * 5.0f;
            }

            /* Wrap read position */
            if (v->read_pos >= (float)slot_len) v->read_pos = 0.0f;
            if (v->read_pos < 0.0f) v->read_pos = (float)(slot_len - 1);

            /* ── Vactrol LPG ── */
            v->lpg.vactrol_cv = v->env_level * vel_shaped;
            float processed = vactrol_process(&v->lpg, sample, rise_rate, fall_rate, nonlinearity);

            /* ── Pan + mix (using pre-computed gains, update slowly via drift) ── */
            mix_buf_l[i] += processed * pan_gain_l;
            mix_buf_r[i] += processed * pan_gain_r;
        }
    }

    /* ── Master processing ── */
    float volume = inst->param_volume;

    /* Echo Pool write (Phase Garden feedback) */
    float pool_blend = inst->param_pool;

    for (int i = 0; i < frames; i++) {
        float l = mix_buf_l[i];
        float r = mix_buf_r[i];

        /* Write to echo pool */
        if (inst->echo_pool) {
            inst->echo_pool[inst->echo_pool_write] = (l + r) * 0.5f;
            inst->echo_pool_write = (inst->echo_pool_write + 1) % ECHO_POOL_SIZE;
        }

        /* Blend echo pool into output */
        if (pool_blend > 0.001f && inst->echo_pool) {
            int read_pos = (inst->echo_pool_write - (int)(SAMPLE_RATE * 0.5f) + ECHO_POOL_SIZE) % ECHO_POOL_SIZE;
            float pool_sample = inst->echo_pool[read_pos] * pool_blend * 0.5f;
            l += pool_sample;
            r += pool_sample;
        }

        /* Erosion: progressive LPF that gets more aggressive over time */
        if (inst->param_erode > 0.001f) {
            float erode_alpha = 0.05f + (1.0f - inst->param_erode) * 0.95f;
            inst->erosion_lp_z1 += erode_alpha * ((l + r) * 0.5f - inst->erosion_lp_z1);
            if (fabsf(inst->erosion_lp_z1) < 1e-15f) inst->erosion_lp_z1 = 0.0f;
            float eroded = inst->erosion_lp_z1;
            l = lerpf(l, eroded, inst->param_erode);
            r = lerpf(r, eroded, inst->param_erode);
        }

        /* Soft limiter — lowercase-safe companding */
        l = fast_tanh(l * 2.0f) * volume;
        r = fast_tanh(r * 2.0f) * volume;

        /* Output */
        int16_t sl = (int16_t)(clampf(l, -1.0f, 1.0f) * 32767.0f);
        int16_t sr = (int16_t)(clampf(r, -1.0f, 1.0f) * 32767.0f);
        out_lr[i * 2]     = sl;
        out_lr[i * 2 + 1] = sr;
    }

    inst->tick += frames;
}

/* ── API v2 export ──────────────────────────────────────────────────────── */

typedef struct {
    uint32_t api_version;
    void* (*create_instance)(const char *, const char *);
    void  (*destroy_instance)(void *);
    void  (*on_midi)(void *, const uint8_t *, int, int);
    void  (*set_param)(void *, const char *, const char *);
    int   (*get_param)(void *, const char *, char *, int);
    int   (*get_error)(void *, char *, int);
    void  (*render_block)(void *, int16_t *, int);
} plugin_api_v2_t;

__attribute__((visibility("default")))
plugin_api_v2_t* move_plugin_init_v2(const void *host) {
    (void)host;
    static plugin_api_v2_t api = {
        .api_version      = 2,
        .create_instance  = create_instance,
        .destroy_instance = destroy_instance,
        .on_midi          = on_midi,
        .set_param        = set_param,
        .get_param        = get_param,
        .get_error        = NULL,
        .render_block     = render_block,
    };
    return &api;
}
