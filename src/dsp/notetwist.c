/*
 * NoteTwist — MIDI FX Module
 *
 * Play Effects, MIDI Delay, and octave transposer inspired by the
 * Yamaha RS7000.
 *
 * Processing stages (in order):
 *   1. Octave Transpose
 *   2. Harmonize (unison, octaver, harmonize_1, harmonize_2)
 *   3. Note Page (note_offset, gate_time, velocity_offset)
 *   4. Clock Shift
 *   5. MIDI Delay with feedback
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include "host/midi_fx_api_v1.h"
#include "host/plugin_api_v1.h"

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

#define MAX_EVENTS          2048
#define MAX_GEN_NOTES       6       /* original + octaver + harm1 + harm2 + headroom */
#define MAX_REPEATS         64
#define CLOCK_WINDOW        24      /* rolling window for BPM measurement */
#define UNISON_STAGGER      220     /* ~5 ms at 44100 Hz */
#define DEFAULT_SR          44100
#define MAX_DELAY_SAMPLES   (30ULL * 44100)  /* cap cumulative delay at 30 s */

/* Clock resolution: 480 PPQN */
static const int CLOCK_VALUES[11] = {
    0, 30, 60, 80, 120, 160, 240, 320, 480, 960, 1920
};
static const char *CLOCK_LABELS[11] = {
    "0","1/64","1/32","1/16T","1/16","1/8T","1/8","1/4T","1/4","1/2","1/1"
};
#define NUM_CLOCK_VALUES 11

static const char *UNISON_LABELS[3] = { "OFF", "x2", "x3" };

/* ------------------------------------------------------------------ */
/*  Minimal JSON helpers (same pattern as arp.c)                       */
/* ------------------------------------------------------------------ */

static int json_get_string(const char *json, const char *key,
                           char *out, int out_len) {
    if (!json || !key || !out || out_len < 1) return 0;
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *pos = strstr(json, search);
    if (!pos) return 0;
    const char *colon = strchr(pos + strlen(search), ':');
    if (!colon) return 0;
    while (*colon && (*colon == ':' || *colon == ' ' || *colon == '\t')) colon++;
    if (*colon != '"') return 0;
    colon++;
    const char *end = strchr(colon, '"');
    if (!end) return 0;
    int len = (int)(end - colon);
    if (len >= out_len) len = out_len - 1;
    memcpy(out, colon, len);
    out[len] = '\0';
    return len;
}

static int json_get_int(const char *json, const char *key, int *out) {
    if (!json || !key || !out) return 0;
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *pos = strstr(json, search);
    if (!pos) return 0;
    const char *colon = strchr(pos + strlen(search), ':');
    if (!colon) return 0;
    colon++;
    while (*colon && (*colon == ' ' || *colon == '\t')) colon++;
    *out = atoi(colon);
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Structs                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t fire_at;
    uint8_t  msg[3];
    uint8_t  len;
} sched_event_t;

typedef struct {
    uint8_t  active;
    uint8_t  channel;        /* MIDI channel of the original note */
    uint64_t on_time;        /* sample_counter when note-on arrived */
    uint8_t  orig_velocity;  /* velocity after velocity_offset */
    uint8_t  gen_notes[MAX_GEN_NOTES];
    int      gen_count;
    uint64_t clock_shift_samples;
    int      stored_unison;  /* unison value at note-on time */

    /* Delay repeat snapshot */
    double   spc;            /* samples_per_clock at note-on time */
    int      stored_repeat_count;
    struct {
        uint64_t cumul_delay;   /* cumulative delay from note-on base */
        int8_t   pitch_offset;  /* cumulative pitch shift */
        uint8_t  velocity;
        double   gate_factor;   /* cumulative gate multiplier */
    } reps[MAX_REPEATS];
} active_note_t;

typedef struct {
    /* --- Parameters --- */
    int octave_shift;       /* -4 .. +4 */
    int unison;             /* 0=OFF, 1=x2, 2=x3 */
    int octaver;            /* -4 .. +4, 0 = off */
    int harmonize_1;        /* -24 .. +24, 0 = off */
    int harmonize_2;        /* -24 .. +24, 0 = off */
    int note_offset;        /* -24 .. +24 */
    int gate_time;          /* 0 .. 200 percent */
    int velocity_offset;    /* -127 .. +127 */
    int clock_shift_idx;    /* index into CLOCK_VALUES */
    int delay_time_idx;     /* index into CLOCK_VALUES */
    int delay_level;        /* 0 .. 127 */
    int repeat_times;       /* 0 .. 64 */
    int fb_velocity;        /* -127 .. +127 */
    int fb_note;            /* -24 .. +24 */
    int fb_note_random;     /* 0 or 1 */
    int fb_gate_time;       /* -100 .. +100 */
    int fb_clock;           /* -100 .. +100 */
    int bpm;                /* fallback BPM 20 .. 300 */

    /* --- Tempo tracking --- */
    uint64_t clock_ts[CLOCK_WINDOW];
    int      clock_ts_idx;
    int      clock_ts_cnt;
    uint64_t last_clock_time;
    double   live_bpm;

    /* --- Runtime --- */
    uint64_t       sample_counter;
    int            sample_rate;
    sched_event_t  events[MAX_EVENTS];
    int            event_count;
    active_note_t  active_notes[128];
    uint32_t       rng;
} notetwist_t;

static const host_api_v1_t *g_host = NULL;

/* ------------------------------------------------------------------ */
/*  Utility                                                            */
/* ------------------------------------------------------------------ */

static int clamp_i(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static int rs_rand(notetwist_t *inst, int lo, int hi) {
    uint32_t x = inst->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    inst->rng = x;
    return lo + (int)(x % (uint32_t)(hi - lo + 1));
}

static double get_bpm(notetwist_t *inst) {
    /* 1. Live clock */
    if (inst->live_bpm > 0 && inst->last_clock_time > 0 &&
        (inst->sample_counter - inst->last_clock_time) <
         (uint64_t)(inst->sample_rate * 2))
        return inst->live_bpm;
    /* 2. Host BPM */
    if (g_host && g_host->get_bpm) {
        float h = g_host->get_bpm();
        if (h > 10.0f) return (double)h;
    }
    /* 3. Fallback parameter */
    return (double)inst->bpm;
}

/* samples_per_clock at 480 PPQN */
static double spc(notetwist_t *inst) {
    double b = get_bpm(inst);
    return ((double)inst->sample_rate * 60.0) / (b * 480.0);
}

static uint64_t clk2smp(int clocks, double samples_per_clk) {
    return (uint64_t)(clocks * samples_per_clk + 0.5);
}

/* ------------------------------------------------------------------ */
/*  Event queue (sorted by fire_at, ascending)                         */
/* ------------------------------------------------------------------ */

static void q_insert(notetwist_t *inst, uint64_t fire_at,
                     uint8_t s, uint8_t d1, uint8_t d2) {
    if (inst->event_count >= MAX_EVENTS) return;

    int lo = 0, hi = inst->event_count;
    while (lo < hi) {
        int mid = (lo + hi) >> 1;
        if (inst->events[mid].fire_at <= fire_at) lo = mid + 1;
        else hi = mid;
    }
    if (lo < inst->event_count)
        memmove(&inst->events[lo + 1], &inst->events[lo],
                (size_t)(inst->event_count - lo) * sizeof(sched_event_t));

    inst->events[lo] = (sched_event_t){ fire_at, {s, d1, d2}, 3 };
    inst->event_count++;
}

static int q_fire(notetwist_t *inst, uint64_t now,
                  uint8_t out[][3], int lens[], int max) {
    int n = 0, f = 0;
    while (f < inst->event_count &&
           inst->events[f].fire_at <= now && n < max) {
        out[n][0] = inst->events[f].msg[0];
        out[n][1] = inst->events[f].msg[1];
        out[n][2] = inst->events[f].msg[2];
        lens[n]   = inst->events[f].len;
        n++; f++;
    }
    if (f > 0) {
        inst->event_count -= f;
        if (inst->event_count > 0)
            memmove(&inst->events[0], &inst->events[f],
                    (size_t)inst->event_count * sizeof(sched_event_t));
    }
    return n;
}

/* ------------------------------------------------------------------ */
/*  Build generated-note list                                          */
/* ------------------------------------------------------------------ */

static int build_gen_notes(notetwist_t *inst, int orig_note, uint8_t *out) {
    int cnt = 0;
    int n = orig_note + inst->octave_shift * 12 + inst->note_offset;
    n = clamp_i(n, 0, 127);
    out[cnt++] = (uint8_t)n;

    if (inst->octaver != 0) {
        int o = n + inst->octaver * 12;
        if (o >= 0 && o <= 127 && cnt < MAX_GEN_NOTES) out[cnt++] = (uint8_t)o;
    }
    if (inst->harmonize_1 != 0) {
        int h = n + inst->harmonize_1;
        if (h >= 0 && h <= 127 && cnt < MAX_GEN_NOTES) out[cnt++] = (uint8_t)h;
    }
    if (inst->harmonize_2 != 0) {
        int h = n + inst->harmonize_2;
        if (h >= 0 && h <= 127 && cnt < MAX_GEN_NOTES) out[cnt++] = (uint8_t)h;
    }
    return cnt;
}

/* ------------------------------------------------------------------ */
/*  Schedule delay repeats (note-ons only; note-offs scheduled later)  */
/* ------------------------------------------------------------------ */

static void sched_delay_ons(notetwist_t *inst, active_note_t *an,
                            uint64_t base_time, double sp) {
    if (inst->repeat_times == 0 || inst->delay_level == 0) return;
    int dclk = CLOCK_VALUES[inst->delay_time_idx];
    if (dclk == 0) return;

    an->spc = sp;
    int reps = clamp_i(inst->repeat_times, 0, MAX_REPEATS);
    an->stored_repeat_count = reps;

    double cumul = 0.0;
    double cur_delay = (double)dclk * sp;
    int    cumul_pitch = 0;
    int    rep_vel = (int)an->orig_velocity * inst->delay_level / 127;

    for (int i = 0; i < reps; i++) {
        cumul += cur_delay;
        if ((uint64_t)(cumul + 0.5) > MAX_DELAY_SAMPLES) {
            an->stored_repeat_count = i;
            break;
        }

        /* Pitch feedback */
        if (inst->fb_note_random)
            cumul_pitch += rs_rand(inst, -12, 12);
        else
            cumul_pitch += inst->fb_note;
        an->reps[i].pitch_offset = (int8_t)clamp_i(cumul_pitch, -127, 127);

        /* Velocity feedback */
        if (i > 0) rep_vel += inst->fb_velocity;
        rep_vel = clamp_i(rep_vel, 1, 127);
        an->reps[i].velocity = (uint8_t)rep_vel;

        /* Gate feedback (cumulative multiplier) */
        double gf = 1.0;
        for (int k = 0; k <= i; k++)
            gf *= (1.0 + inst->fb_gate_time / 100.0);
        an->reps[i].gate_factor = gf;

        an->reps[i].cumul_delay = (uint64_t)(cumul + 0.5);

        /* Schedule repeat note-ons for each generated note */
        uint64_t ft = base_time + an->reps[i].cumul_delay;
        uint8_t ch = an->channel;
        for (int j = 0; j < an->gen_count; j++) {
            int note = (int)an->gen_notes[j] + an->reps[i].pitch_offset;
            note = clamp_i(note, 0, 127);
            q_insert(inst, ft, (uint8_t)(0x90 | ch),
                     (uint8_t)note, an->reps[i].velocity);
        }

        /* Delay time feedback */
        cur_delay *= (1.0 + inst->fb_clock / 100.0);
        if (cur_delay < 1.0) cur_delay = 1.0;
    }
}

/* Schedule delay repeat note-offs (called when original note-off arrives) */
static void sched_delay_offs(notetwist_t *inst, active_note_t *an,
                             uint64_t base_time, uint64_t gate_smp) {
    uint8_t ch = an->channel;
    for (int i = 0; i < an->stored_repeat_count; i++) {
        double rg = (double)gate_smp * an->reps[i].gate_factor;
        if (rg < 1.0) rg = 1.0;
        uint64_t off = base_time + an->reps[i].cumul_delay + (uint64_t)(rg + 0.5);

        for (int j = 0; j < an->gen_count; j++) {
            int note = (int)an->gen_notes[j] + an->reps[i].pitch_offset;
            note = clamp_i(note, 0, 127);
            q_insert(inst, off, (uint8_t)(0x80 | ch), (uint8_t)note, 0);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                          */
/* ------------------------------------------------------------------ */

static void *rs_create(const char *module_dir, const char *cfg) {
    (void)module_dir; (void)cfg;
    notetwist_t *inst = calloc(1, sizeof(notetwist_t));
    if (!inst) return NULL;

    inst->gate_time      = 100;
    inst->delay_time_idx = 8;   /* 1/4 */
    inst->delay_level    = 80;
    inst->repeat_times   = 3;
    inst->fb_velocity    = -10;
    inst->bpm            = 120;
    inst->sample_rate    = DEFAULT_SR;
    inst->rng            = 12345;
    return inst;
}

static void rs_destroy(void *instance) { free(instance); }

/* ------------------------------------------------------------------ */
/*  process_midi                                                       */
/* ------------------------------------------------------------------ */

static int rs_process(void *instance,
                      const uint8_t *in, int in_len,
                      uint8_t out[][3], int olens[], int max_out) {
    notetwist_t *inst = (notetwist_t *)instance;
    if (!inst || in_len < 1 || max_out < 1) return 0;

    uint8_t status = in[0];

    /* --- MIDI clock (0xF8) — measure BPM, consume silently --- */
    if (status == 0xF8) {
        uint64_t now = inst->sample_counter;
        inst->clock_ts[inst->clock_ts_idx] = now;
        inst->clock_ts_idx = (inst->clock_ts_idx + 1) % CLOCK_WINDOW;
        if (inst->clock_ts_cnt < CLOCK_WINDOW) inst->clock_ts_cnt++;
        inst->last_clock_time = now;

        if (inst->clock_ts_cnt >= 2) {
            int oldest = (inst->clock_ts_idx - inst->clock_ts_cnt
                          + CLOCK_WINDOW) % CLOCK_WINDOW;
            int newest = (inst->clock_ts_idx - 1 + CLOCK_WINDOW) % CLOCK_WINDOW;
            uint64_t span = inst->clock_ts[newest] - inst->clock_ts[oldest];
            if (span > 0) {
                double avg = (double)span / (inst->clock_ts_cnt - 1);
                inst->live_bpm = (60.0 * inst->sample_rate) / (avg * 24.0);
            }
        }
        return 0;
    }

    /* Consume start / continue / stop */
    if (status == 0xFA || status == 0xFB || status == 0xFC) return 0;

    uint8_t stype = status & 0xF0;
    uint8_t ch    = status & 0x0F;

    /* ---- NOTE ON ------------------------------------------------- */
    if (stype == 0x90 && in_len >= 3 && in[2] > 0) {
        uint8_t orig_note = in[1];
        int     vel       = clamp_i((int)in[2] + inst->velocity_offset, 1, 127);
        int     count     = 0;
        uint64_t now      = inst->sample_counter;
        active_note_t *an = &inst->active_notes[orig_note];

        /* Retrigger: send note-offs for currently sounding notes */
        if (an->active) {
            uint8_t off_status = (uint8_t)(0x80 | an->channel);
            for (int i = 0; i < an->gen_count && count < max_out; i++) {
                out[count][0] = off_status;
                out[count][1] = an->gen_notes[i];
                out[count][2] = 0;
                olens[count]  = 3;
                count++;
            }
        }

        /* Build generated notes */
        uint8_t gen[MAX_GEN_NOTES];
        int gc = build_gen_notes(inst, (int)orig_note, gen);

        /* Clock shift */
        double sp_clk     = spc(inst);
        int    cs_clocks  = CLOCK_VALUES[inst->clock_shift_idx];
        uint64_t cs_smp   = clk2smp(cs_clocks, sp_clk);
        uint64_t base     = now + cs_smp;

        /* Store active-note info */
        memset(an, 0, sizeof(active_note_t));
        an->active              = 1;
        an->channel             = ch;
        an->on_time             = now;
        an->orig_velocity       = (uint8_t)vel;
        an->gen_count           = gc;
        memcpy(an->gen_notes, gen, gc);
        an->clock_shift_samples = cs_smp;
        an->stored_unison       = inst->unison;

        /* Output / schedule note-ons */
        uint8_t on_status = (uint8_t)(0x90 | ch);
        for (int i = 0; i < gc; i++) {
            if (cs_smp == 0) {
                if (count < max_out) {
                    out[count][0] = on_status;
                    out[count][1] = gen[i];
                    out[count][2] = (uint8_t)vel;
                    olens[count]  = 3;
                    count++;
                }
            } else {
                q_insert(inst, base, on_status, gen[i], (uint8_t)vel);
            }
        }

        /* Unison staggered copies */
        for (int c = 0; c < inst->unison; c++) {
            uint64_t stagger = base + (uint64_t)(UNISON_STAGGER * (c + 1));
            for (int i = 0; i < gc; i++)
                q_insert(inst, stagger, on_status, gen[i], (uint8_t)vel);
        }

        /* Delay repeats */
        sched_delay_ons(inst, an, base, sp_clk);

        return count;
    }

    /* ---- NOTE OFF ------------------------------------------------ */
    if ((stype == 0x80 || (stype == 0x90 && in[2] == 0)) && in_len >= 3) {
        uint8_t orig_note = in[1];
        active_note_t *an = &inst->active_notes[orig_note];

        if (!an->active) {
            /* Unknown note — pass through */
            out[0][0] = in[0]; out[0][1] = in[1]; out[0][2] = in[2];
            olens[0] = 3;
            return 1;
        }

        uint64_t now  = inst->sample_counter;
        uint64_t dur  = (now > an->on_time) ? (now - an->on_time) : 0;
        uint64_t gate = (uint64_t)((double)dur * inst->gate_time / 100.0 + 0.5);
        if (gate < 1 && inst->gate_time > 0) gate = 1;

        uint64_t off_time = an->on_time + an->clock_shift_samples + gate;
        /* Account for unison stagger: extend note-off to cover last copy */
        uint64_t uni_ext  = (uint64_t)(UNISON_STAGGER * an->stored_unison);
        off_time += uni_ext;

        int count = 0;
        uint8_t off_status = (uint8_t)(0x80 | an->channel);

        for (int i = 0; i < an->gen_count; i++) {
            if (off_time <= now) {
                if (count < max_out) {
                    out[count][0] = off_status;
                    out[count][1] = an->gen_notes[i];
                    out[count][2] = 0;
                    olens[count]  = 3;
                    count++;
                }
            } else {
                q_insert(inst, off_time, off_status, an->gen_notes[i], 0);
            }
        }

        /* Delay repeat note-offs */
        sched_delay_offs(inst, an, an->on_time + an->clock_shift_samples + uni_ext, gate);

        an->active = 0;
        return count;
    }

    /* ---- Pass through everything else ---- */
    out[0][0] = in[0];
    out[0][1] = in_len > 1 ? in[1] : 0;
    out[0][2] = in_len > 2 ? in[2] : 0;
    olens[0]  = in_len;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  tick                                                               */
/* ------------------------------------------------------------------ */

static int rs_tick(void *instance, int frames, int sr,
                   uint8_t out[][3], int olens[], int max_out) {
    notetwist_t *inst = (notetwist_t *)instance;
    if (!inst) return 0;
    inst->sample_rate    = sr;
    inst->sample_counter += (uint64_t)frames;
    return q_fire(inst, inst->sample_counter, out, olens, max_out);
}

/* ------------------------------------------------------------------ */
/*  set_param                                                          */
/* ------------------------------------------------------------------ */

static int find_clock_idx(const char *val) {
    for (int i = 0; i < NUM_CLOCK_VALUES; i++)
        if (strcmp(val, CLOCK_LABELS[i]) == 0) return i;
    /* Try as integer index */
    int v = atoi(val);
    if (v >= 0 && v < NUM_CLOCK_VALUES) return v;
    return -1;
}

static void rs_set(void *instance, const char *key, const char *val) {
    notetwist_t *inst = (notetwist_t *)instance;
    if (!inst || !key || !val) return;

    if      (!strcmp(key, "octave_shift"))    inst->octave_shift    = clamp_i(atoi(val), -4, 4);
    else if (!strcmp(key, "unison")) {
        if      (!strcmp(val, "OFF")) inst->unison = 0;
        else if (!strcmp(val, "x2"))  inst->unison = 1;
        else if (!strcmp(val, "x3"))  inst->unison = 2;
        else { int v = atoi(val); inst->unison = clamp_i(v, 0, 2); }
    }
    else if (!strcmp(key, "octaver"))         inst->octaver         = clamp_i(atoi(val), -4, 4);
    else if (!strcmp(key, "harmonize_1"))     inst->harmonize_1     = clamp_i(atoi(val), -24, 24);
    else if (!strcmp(key, "harmonize_2"))     inst->harmonize_2     = clamp_i(atoi(val), -24, 24);
    else if (!strcmp(key, "note_offset"))     inst->note_offset     = clamp_i(atoi(val), -24, 24);
    else if (!strcmp(key, "gate_time"))       inst->gate_time       = clamp_i(atoi(val), 0, 200);
    else if (!strcmp(key, "velocity_offset")) inst->velocity_offset = clamp_i(atoi(val), -127, 127);
    else if (!strcmp(key, "clock_shift")) {
        int idx = find_clock_idx(val);
        if (idx >= 0) inst->clock_shift_idx = idx;
    }
    else if (!strcmp(key, "delay_time")) {
        int idx = find_clock_idx(val);
        if (idx >= 0) inst->delay_time_idx = idx;
    }
    else if (!strcmp(key, "delay_level"))     inst->delay_level     = clamp_i(atoi(val), 0, 127);
    else if (!strcmp(key, "repeat_times"))    inst->repeat_times    = clamp_i(atoi(val), 0, 64);
    else if (!strcmp(key, "fb_velocity"))     inst->fb_velocity     = clamp_i(atoi(val), -127, 127);
    else if (!strcmp(key, "fb_note"))         inst->fb_note         = clamp_i(atoi(val), -24, 24);
    else if (!strcmp(key, "fb_note_random")) {
        if (!strcmp(val, "on"))  inst->fb_note_random = 1;
        else if (!strcmp(val, "off")) inst->fb_note_random = 0;
        else inst->fb_note_random = clamp_i(atoi(val), 0, 1);
    }
    else if (!strcmp(key, "fb_gate_time"))    inst->fb_gate_time    = clamp_i(atoi(val), -100, 100);
    else if (!strcmp(key, "fb_clock"))        inst->fb_clock        = clamp_i(atoi(val), -100, 100);
    else if (!strcmp(key, "bpm"))             inst->bpm             = clamp_i(atoi(val), 20, 300);
    else if (!strcmp(key, "state")) {
        /* Restore all params from JSON blob */
        int v;
        char s[16];
        if (json_get_int(val, "octave_shift",    &v)) inst->octave_shift    = clamp_i(v, -4, 4);
        if (json_get_int(val, "octaver",         &v)) inst->octaver         = clamp_i(v, -4, 4);
        if (json_get_int(val, "harmonize_1",     &v)) inst->harmonize_1     = clamp_i(v, -24, 24);
        if (json_get_int(val, "harmonize_2",     &v)) inst->harmonize_2     = clamp_i(v, -24, 24);
        if (json_get_int(val, "note_offset",     &v)) inst->note_offset     = clamp_i(v, -24, 24);
        if (json_get_int(val, "gate_time",       &v)) inst->gate_time       = clamp_i(v, 0, 200);
        if (json_get_int(val, "velocity_offset", &v)) inst->velocity_offset = clamp_i(v, -127, 127);
        if (json_get_int(val, "delay_level",     &v)) inst->delay_level     = clamp_i(v, 0, 127);
        if (json_get_int(val, "repeat_times",    &v)) inst->repeat_times    = clamp_i(v, 0, 64);
        if (json_get_int(val, "fb_velocity",     &v)) inst->fb_velocity     = clamp_i(v, -127, 127);
        if (json_get_int(val, "fb_note",         &v)) inst->fb_note         = clamp_i(v, -24, 24);
        if (json_get_int(val, "fb_note_random",  &v)) inst->fb_note_random  = clamp_i(v, 0, 1);
        if (json_get_int(val, "fb_gate_time",    &v)) inst->fb_gate_time    = clamp_i(v, -100, 100);
        if (json_get_int(val, "fb_clock",        &v)) inst->fb_clock        = clamp_i(v, -100, 100);
        if (json_get_int(val, "bpm",             &v)) inst->bpm             = clamp_i(v, 20, 300);
        if (json_get_string(val, "unison", s, sizeof(s))) {
            if      (!strcmp(s, "OFF")) inst->unison = 0;
            else if (!strcmp(s, "x2"))  inst->unison = 1;
            else if (!strcmp(s, "x3"))  inst->unison = 2;
        }
        if (json_get_string(val, "clock_shift", s, sizeof(s))) {
            int idx = find_clock_idx(s);
            if (idx >= 0) inst->clock_shift_idx = idx;
        }
        if (json_get_string(val, "delay_time", s, sizeof(s))) {
            int idx = find_clock_idx(s);
            if (idx >= 0) inst->delay_time_idx = idx;
        }
        if (json_get_string(val, "fb_note_random", s, sizeof(s))) {
            inst->fb_note_random = (!strcmp(s, "on")) ? 1 : 0;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  get_param                                                          */
/* ------------------------------------------------------------------ */

static int rs_get(void *instance, const char *key, char *buf, int buf_len) {
    notetwist_t *inst = (notetwist_t *)instance;
    if (!inst || !key || !buf || buf_len < 1) return -1;

    if      (!strcmp(key, "octave_shift"))    return snprintf(buf, buf_len, "%d", inst->octave_shift);
    else if (!strcmp(key, "unison"))          return snprintf(buf, buf_len, "%s", UNISON_LABELS[inst->unison]);
    else if (!strcmp(key, "octaver"))         return snprintf(buf, buf_len, "%d", inst->octaver);
    else if (!strcmp(key, "harmonize_1"))     return snprintf(buf, buf_len, "%d", inst->harmonize_1);
    else if (!strcmp(key, "harmonize_2"))     return snprintf(buf, buf_len, "%d", inst->harmonize_2);
    else if (!strcmp(key, "note_offset"))     return snprintf(buf, buf_len, "%d", inst->note_offset);
    else if (!strcmp(key, "gate_time"))       return snprintf(buf, buf_len, "%d", inst->gate_time);
    else if (!strcmp(key, "velocity_offset")) return snprintf(buf, buf_len, "%d", inst->velocity_offset);
    else if (!strcmp(key, "clock_shift"))     return snprintf(buf, buf_len, "%s", CLOCK_LABELS[inst->clock_shift_idx]);
    else if (!strcmp(key, "delay_time"))      return snprintf(buf, buf_len, "%s", CLOCK_LABELS[inst->delay_time_idx]);
    else if (!strcmp(key, "delay_level"))     return snprintf(buf, buf_len, "%d", inst->delay_level);
    else if (!strcmp(key, "repeat_times"))    return snprintf(buf, buf_len, "%d", inst->repeat_times);
    else if (!strcmp(key, "fb_velocity"))     return snprintf(buf, buf_len, "%d", inst->fb_velocity);
    else if (!strcmp(key, "fb_note"))         return snprintf(buf, buf_len, "%d", inst->fb_note);
    else if (!strcmp(key, "fb_note_random"))  return snprintf(buf, buf_len, "%s", inst->fb_note_random ? "on" : "off");
    else if (!strcmp(key, "fb_gate_time"))    return snprintf(buf, buf_len, "%d", inst->fb_gate_time);
    else if (!strcmp(key, "fb_clock"))        return snprintf(buf, buf_len, "%d", inst->fb_clock);
    else if (!strcmp(key, "bpm"))             return snprintf(buf, buf_len, "%d", inst->bpm);

    else if (!strcmp(key, "bpm_display")) {
        double b = get_bpm(inst);
        return snprintf(buf, buf_len, "%.1f", b);
    }

    else if (!strcmp(key, "state")) {
        return snprintf(buf, buf_len,
            "{\"octave_shift\":%d,\"unison\":\"%s\","
            "\"octaver\":%d,\"harmonize_1\":%d,\"harmonize_2\":%d,"
            "\"note_offset\":%d,\"gate_time\":%d,\"velocity_offset\":%d,"
            "\"clock_shift\":\"%s\",\"delay_time\":\"%s\","
            "\"delay_level\":%d,\"repeat_times\":%d,"
            "\"fb_velocity\":%d,\"fb_note\":%d,\"fb_note_random\":\"%s\","
            "\"fb_gate_time\":%d,\"fb_clock\":%d,\"bpm\":%d}",
            inst->octave_shift, UNISON_LABELS[inst->unison],
            inst->octaver, inst->harmonize_1, inst->harmonize_2,
            inst->note_offset, inst->gate_time, inst->velocity_offset,
            CLOCK_LABELS[inst->clock_shift_idx],
            CLOCK_LABELS[inst->delay_time_idx],
            inst->delay_level, inst->repeat_times,
            inst->fb_velocity, inst->fb_note,
            inst->fb_note_random ? "on" : "off",
            inst->fb_gate_time, inst->fb_clock, inst->bpm);
    }

    else if (!strcmp(key, "chain_params")) {
        const char *p =
            "["
            "{\"key\":\"octave_shift\",\"name\":\"Octave\",\"type\":\"int\",\"min\":-4,\"max\":4,\"step\":1},"
            "{\"key\":\"unison\",\"name\":\"Unison\",\"type\":\"enum\",\"options\":[\"OFF\",\"x2\",\"x3\"]},"
            "{\"key\":\"octaver\",\"name\":\"Octaver\",\"type\":\"int\",\"min\":-4,\"max\":4,\"step\":1},"
            "{\"key\":\"harmonize_1\",\"name\":\"Harmony 1\",\"type\":\"int\",\"min\":-24,\"max\":24,\"step\":1},"
            "{\"key\":\"harmonize_2\",\"name\":\"Harmony 2\",\"type\":\"int\",\"min\":-24,\"max\":24,\"step\":1},"
            "{\"key\":\"note_offset\",\"name\":\"Note Ofs\",\"type\":\"int\",\"min\":-24,\"max\":24,\"step\":1},"
            "{\"key\":\"gate_time\",\"name\":\"Gate %\",\"type\":\"int\",\"min\":0,\"max\":200,\"step\":1},"
            "{\"key\":\"velocity_offset\",\"name\":\"Vel Ofs\",\"type\":\"int\",\"min\":-127,\"max\":127,\"step\":1},"
            "{\"key\":\"clock_shift\",\"name\":\"Clk Shift\",\"type\":\"enum\","
              "\"options\":[\"0\",\"1/64\",\"1/32\",\"1/16T\",\"1/16\",\"1/8T\",\"1/8\",\"1/4T\",\"1/4\",\"1/2\",\"1/1\"]},"
            "{\"key\":\"delay_time\",\"name\":\"Dly Time\",\"type\":\"enum\","
              "\"options\":[\"0\",\"1/64\",\"1/32\",\"1/16T\",\"1/16\",\"1/8T\",\"1/8\",\"1/4T\",\"1/4\",\"1/2\",\"1/1\"]},"
            "{\"key\":\"delay_level\",\"name\":\"Dly Level\",\"type\":\"int\",\"min\":0,\"max\":127,\"step\":1},"
            "{\"key\":\"repeat_times\",\"name\":\"Repeats\",\"type\":\"int\",\"min\":0,\"max\":64,\"step\":1},"
            "{\"key\":\"fb_velocity\",\"name\":\"FB Vel\",\"type\":\"int\",\"min\":-127,\"max\":127,\"step\":1},"
            "{\"key\":\"fb_note\",\"name\":\"FB Note\",\"type\":\"int\",\"min\":-24,\"max\":24,\"step\":1},"
            "{\"key\":\"fb_note_random\",\"name\":\"FB Rand\",\"type\":\"enum\",\"options\":[\"off\",\"on\"]},"
            "{\"key\":\"fb_gate_time\",\"name\":\"FB Gate\",\"type\":\"int\",\"min\":-100,\"max\":100,\"step\":1},"
            "{\"key\":\"fb_clock\",\"name\":\"FB Clock\",\"type\":\"int\",\"min\":-100,\"max\":100,\"step\":1},"
            "{\"key\":\"bpm\",\"name\":\"BPM\",\"type\":\"int\",\"min\":20,\"max\":300,\"step\":1}"
            "]";
        return snprintf(buf, buf_len, "%s", p);
    }

    return -1;
}

/* ------------------------------------------------------------------ */
/*  API table & init                                                   */
/* ------------------------------------------------------------------ */

static midi_fx_api_v1_t g_api = {
    .api_version     = MIDI_FX_API_VERSION,
    .create_instance = rs_create,
    .destroy_instance = rs_destroy,
    .process_midi    = rs_process,
    .tick            = rs_tick,
    .set_param       = rs_set,
    .get_param       = rs_get
};

midi_fx_api_v1_t *move_midi_fx_init(const host_api_v1_t *host) {
    g_host = host;
    return &g_api;
}
