/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* I2S output (ALNK0 -> external codec) and the
 * audio ISR: per half buffer, blocks of CTL samples: mix_block (fx.c: events ->
 * each synth part -> dist -> level / pan -> sends -> buses -> master) -> USB audio (uac_tap) -> the metronome's
 * click (click.c, the DAC only) -> 24-bit stereo. */
/* registers: hal/fm1_audio.h */
#define HALF_WORDS (HALF_FRAMES * 2u)
#define DAC_TICKS 544u            /* TIMER4 ticks per I2S frame (24 MHz / 44,117.6 Hz) */
#define CPU_AVG (4096u / HALF_FRAMES)   /* halves the load meter averages: ~93 ms whatever the half */
#define OUT_SHIFT 7               /* Q15 -> 24-bit, -6 dBFS ceiling (M0f ran clean at -18 dBFS) */

static int32_t abuf[2u * HALF_WORDS] __attribute__((aligned(4)));

/* diagnostics, kept across resets and UBOOT entry: read with `fm1t memr` */
#define DBG_MAGIC 0x44424731u                       /* "DBG1" */
struct felucca_dbg {
    uint32_t magic, halves, max_us, nested, in_audio, late, timer_irqs, ui_frames;
    uint32_t last_us, cpu_q8, boots;
    uint32_t stage, page, home;           /* where the main loop is (breadcrumbs) */
    uint32_t prev_stage, prev_page, prev_home, prev_rst, prev_frames;   /* as found at boot */
} felucca_dbg __attribute__((section(".noinit")));
static volatile uint32_t audio_halves, audio_max_us;
static volatile uint32_t t5_nested_ticks;              /* TIMER4 ticks TIMER5 spent nested in this ISR (main.c) */
static uint32_t audio_cpu_rem;                         /* keep the fractional IIR step: no low-load bias */

static void audio_block(int32_t *out, uint32_t n)       /* mix (fx.c), then Q15 -> 24 bit */
{
    uint32_t i;
    mix_block(out, n);
#if FELUCCA_UAC
    uac_tap(out, n);                                    /* the USB audio input: the same master output */
#endif
    if (fx_usb_fixed)                                   /* USB LEVEL FIXED: USB took the full level, the DAC */
        usb_fixed_dac(out, n);                          /* (speaker, headphones) gets MASTER's (fx.c) */
    if (!scope_mix)                                     /* the HOME scope (SCOPE OUT): the music, before the click */
        for (i = 1; i < n; i += 2u)                     /* (MIX: fx.c mix_block took it) */
            scope_buf[scope_w++ & (SCOPE_N - 1u)] = (int16_t)out[2u * i];
    click_render(out, n);                               /* the metronome (click.c): the DAC only, not USB */
    for (i = 0; i < n; i++) {
        out[2u * i] *= 1 << OUT_SHIFT;
        out[2u * i + 1u] *= 1 << OUT_SHIFT;
    }
}

/* (Ozy's FMini-1 Modded) The CPU governor: no half may run past its time, or the audio tears (the DMA plays a half
 * not written yet). Each half measures its render (less TIMER5 nested in it: the keys and USB, not the voices) and the
 * voices that sounded. A voice's share (the render less the idle part, over the voices) is followed a quarter of the
 * way up and slowly down, measured with GOV_NMIN voices or more; the idle part with none. The voice cap (voice.c
 * vcap) is the voices that fit in GOV_TARGET of a half at that share. Only new notes see it: one over the cap takes
 * over the part's oldest released voice in place, as at a full budget (no fade, no click); the voices already
 * sounding are never cut for it. Only a half over GOV_HOT (the deadline near: everything nested counts) fades a
 * voice (voice.c voice_kill: ~7 ms), the quietest released one first, never a part's bass or MONO lead, and takes
 * its share at once. "Warning: CPU High" (fmini_app.c) shows only for a half that ran late: a real tear. */
static volatile uint8_t shed_req;
volatile uint32_t cpu_crit;                            /* halves that ran late: the FMINI pop-up "Warning: CPU High" (fmini_app.c) */
static uint32_t shed_count;
#define GOV_HALF ((HALF_FRAMES * 1000000u / FS) * FM1_TICKS_PER_US)   /* a half, TIMER4 ticks */
#define GOV_TARGET (GOV_HALF * 82u / 100u)
#define GOV_HOT (GOV_HALF * 95u / 100u)
#define GOV_NMIN 4u
static uint32_t gov_base = GOV_HALF / 8u;              /* ticks: a half's render with no voice (the buses, the master) */
static uint32_t gov_voice = GOV_HALF / 24u;            /* ticks: a voice's share of a half (12 fit, until measured) */
static uint32_t gov_hot;                               /* halves over GOV_HOT (console) */

static uint32_t gov_voices(void)                        /* voices that render (the fading ones too) */
{
    uint32_t p, i, n = 0;
    for (p = 0; p < NPART; p++)
        for (i = 0; i < NVOICE; i++)
            n += trk[p].v[i].active;
    return n;
}

static void gov_half(uint32_t all)
{
    uint32_t nv = gov_voices(), own = all - (t5_nested_ticks < all ? t5_nested_ticks : all), cap;
    if (!nv) {
        if (own > gov_base)
            gov_base += (own - gov_base + 7u) / 8u;
        else
            gov_base -= (gov_base - own) / 8u;
    } else if (nv >= GOV_NMIN) {
        uint32_t per = (own > gov_base ? own - gov_base : 0u) / nv;
        if (all > GOV_HOT && per > gov_voice)
            gov_voice = per;                           /* too close: what this half showed, at once */
        else if (per > gov_voice)
            gov_voice += (per - gov_voice + 3u) / 4u;  /* up: a quarter of the way a half */
        else
            gov_voice -= (gov_voice - per) / 32u;      /* down: slower (~0.1 s) */
    }
    cap = gov_voice && GOV_TARGET > gov_base ? (GOV_TARGET - gov_base) / gov_voice : 0u;
    vcap = (uint8_t)(cap < VCAP_MIN ? VCAP_MIN : cap > NVOICE ? NVOICE : cap);
    if (all > GOV_HALF)
        cpu_crit++;                                    /* past its time: a tear ("Warning: CPU High") */
    if (all > GOV_HOT) {
        gov_hot++;
        shed_req = 1;                                  /* the deadline near: a voice fades (~7 ms) */
    }
}

/* end of a half: all = its ticks, TIMER5 nested in it included -> the render alone in us (out of line: keeps the
 * ISR's render loops as they were) */
static __attribute__((noinline)) uint32_t shed_check(uint32_t all)
{
    gov_half(all);
    return (all - t5_nested_ticks) / FM1_TICKS_PER_US;
}

static void shed_voice(void)
{
    uint32_t p, i;
    voice_t *best = 0;
    for (p = 0; p < NPART; p++)
        for (i = 0; i < NVOICE; i++) {
            voice_t *v = &trk[p].v[i];
            if (v->active && !v->gate && v->stage != 4u && (!best || v->env < best->env))
                best = v;
        }
    if (best) {
        voice_kill(best);
        shed_count++;
        return;
    }
    for (p = 0; p < NPART; p++) {
        const track_t *t = &trk[p];
        uint32_t mode = trk_vmode(t), low = mode == V_POLY ? lowest_held(t) : 0u;
        for (i = 0; i < NVOICE; i++) {
            voice_t *v = &trk[p].v[i];
            if (v->active && v->gate && v->stage != 4u &&
                ((mode == V_POLY && i != low) || (mode != V_POLY && i > 0u))) {
                if (!best || v->age < best->age)
                    best = v;
            }
        }
    }
    if (best) {
        voice_kill(best);
        shed_count++;
    }
}

void fm1_alnk0_irq(void)                       /* via isr_alnk0 (hal/fm1_isr.S) */
{
    uint8_t p;
    uint32_t t0;
    felucca_dbg.in_audio = 1;                   /* first: TIMER5 nests from here on (main.c) */
    p = fm1_audio_pending();
    t0 = fm1_ticks();
    t5_nested_ticks = 0;
    fm1_audio_ack_aux(p);
    if (p & FM1_AUDIO_HALF) {
        uint32_t half = fm1_audio_free_half(), b, us;
        int32_t *o = &abuf[half * HALF_WORDS];
        if (shed_req) {
            shed_req = 0;
            shed_voice();
        }
#if FELUCCA_UAC
        uac_render_start();
#endif
        for (b = 0; b < HALF_FRAMES; b += CTL) {
#ifdef FM1_INPUT_LAT
            kb_out_tick = t0 + (HALF_FRAMES + b) * DAC_TICKS;   /* when this block plays (seq.c kb_lat) */
#endif
            audio_block(o + 2u * b, CTL);
        }
        fm1_audio_ack_half();
        audio_halves++;
        us = shed_check(fm1_ticks() - t0);
        if (us > audio_max_us)
            audio_max_us = us;
        {
            uint32_t load = song.cpu_q8 * (CPU_AVG - 1u) + (us * 256u) / (HALF_FRAMES * 1000000u / FS) + audio_cpu_rem;
            song.cpu_q8 = load / CPU_AVG;
            audio_cpu_rem = load % CPU_AVG;
        }
        if (fm1_audio_free_half() != half) {
            felucca_dbg.late++;                         /* the DMA moved on while we rendered */
            cpu_crit++;                                 /* (a tear: "Warning: CPU High") */
        }
        felucca_dbg.halves++;
        felucca_dbg.last_us = us;
        if (us > felucca_dbg.max_us)
            felucca_dbg.max_us = us;
        felucca_dbg.cpu_q8 = song.cpu_q8;
    }
    felucca_dbg.in_audio = 0;
}
extern void isr_alnk0(void);

static void audio_init(void)                   /* codec and ALNK0 bring-up (fm1_audio.h) */
{
    uint32_t i;
    for (i = 0; i < 2u * HALF_WORDS; i++)
        abuf[i] = 0;
    fm1_audio_init(abuf, HALF_WORDS, isr_alnk0, 3);
}
