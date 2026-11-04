# Ozy's FMini-1 Modded: what changed

A modified version of [FMini-1](https://github.com/ruppert/fmini-1) by Guilherme Ruppert (GPL-3.0-only, section 5a:
this file is the notice of the modifications and their dates). Based on FMini-1 2.2.0 (2026-11-04, commit 54818a1).
The version number stays FMini-1's (`FMINI_VERSION` in `firmware/src/core.h`), so the web editor keeps working with it.

The two-core voices and the first look were installed and working on Ozy's FM-1 (2026-11-04). The effects on the second
core, the balance and the dark look were installed on 2026-11-04 (the first governor tore more: see the later entry). The
later changes have not been tested on an FM-1 yet.

## 2026-11-04 (later): the governor made gentle, the voice render faster

The first governor (below) made the tearing worse with DREAMER (preset 26) on an FM-1. Whenever more voices sounded
than its cap, it cut one every half buffer with a fade of one block (0.7 ms): a click each time, over and over. Every
half over 92 % cut one too. Now:

- **The cap only limits new notes.** A note over the cap takes over the part's oldest released voice in place (no
  fade). Voices already sounding are never cut for the cap.
- **A voice is cut only when a half passes 95 %** (the deadline near), and the cut fades over ~7 ms (-6 dB a block),
  not one block. The same gentler fade applies wherever a voice is given up (`voice.c env_tick` stage 4).
- **The cost estimate leaves out TIMER5** (keys, USB) nested in the half, measures with 4 or more voices, rises a quarter
  of the way a half (not half) and falls over ~0.1 s. The cap targets 82 % of a half (was 80 %).
- **FMINI voices end at -60 dB of their release, not -72.** A long DECAY (the release time on FMINI) frees its voice
  about a sixth sooner, under the reverb and the other voices.
- **The ladder's loop is faster, with the same output bit for bit** (`eng_fmini.c fmini_render`).
  - LP and HP each have their own loop (the HP sum and its three extra live values are gone from LP).
  - The block's amplitude ramp is held in registers (`fmini_amp`), not re-read from memory after every store to `out[]`.
  - The ten tanh table reads a sample come from a RAM copy of `TANH_Q15` (`fmini_th`, filled by `fmini_init`), not from
    the flash through the cache both cores share.

| File | Change |
| --- | --- |
| `firmware/src/audio.c` | `gov_half`: the cap for new notes only, sheds only over 95 %, the estimate without TIMER5 |
| `firmware/src/voice.c` | `env_tick`: the release ends at -60 dB (FMINI), stage 4 fades over ~7 ms |
| `firmware/src/eng_fmini.c` | `fmini_th`, `fmini_tanh`, `fmini_amp`; `fmini_render`'s loop split LP / HP |
## 2026-11-04: no tearing at full polyphony

With the voices on both cores (next section), "Warning: CPU High" still showed at full polyphony and the audio still
tore: the first core also ran the effect buses, the master and the events alone, and nothing stopped a chord bigger
than the CPU from starting. Three changes.

**The CPU governor** (`audio.c gov_half`, `voice.c vcap`). Each half buffer (2.9 ms) measures its render and the voices
that sounded. A voice's cost is followed fast up and slowly down, and the voice cap is the number of voices that fit in
80 % of a half. A new note over the cap takes a voice, as at a full budget: the oldest released one, else the oldest held
one, never the bass. Voices over the cap fade out, one a half. A half over 92 % takes its own cost at once and fades a
voice. The cap is between 4 and 12, and it learns each patch's cost as it plays. "Warning: CPU High" now shows only for a
half that ran past its time (a real tear), and reads "CPU High: 1 CORE" when the second core is not working (MENU > SYSTEM
> 2ND CORE OFF, or it gave up).

**The effect buses on the second core** (`fx.c fx_queue`, `fx_c1`). A block's sends are queued after the parts, and the
next block runs the chorus, delay and reverb on them. The second core runs them first in its job, before its voices,
while the first core renders its own voices. The wet signal comes one block (0.73 ms) later than the dry one, which no bus
can be heard to change. With no job for the second core (one core, or parts handed out whole), the first core runs them
after the parts, as before. The second core reads only the queued copies and the buses' own state. The parts' send
levels it needs are read when the block is queued (`fxq_on`), because the modulation matrix moves them while the first
core works. A reverb model change fades through `mx[1].b`, not the parts' buffer.

**The balance** (`voice.c vsplit_mask`, `vs_bias`). The second core used to take every other voice. Now it takes half
the voices plus a bias. After each block the bias moves one voice towards the core that finished first: it compares the
second core's end time (TIMER4) with the first core's, and differences under 20 us count as even. So the effects'
share and the voices even out between the cores.

| File | Change |
| --- | --- |
| `firmware/src/audio.c` | The governor (`gov_half`, `gov_voices`) in place of the two-halves-over-93 % shed; `cpu_crit` counts late halves only |
| `firmware/src/voice.c` | `vcap` in `voice_room` and `voice_alloc`; `vsplit_mask` with the bias and the buses' job; `c1_voices` runs the buses first and stamps its end; the balance in `track_render` |
| `firmware/src/fx.c` | `fx_send_mask` / `FX_SENDS`, `fx_queue`, `fx_c1`; `mix_block` queues the sends and runs the buses when no core took them; `rev_switch` fades through `mx[1].b` |
| `firmware/src/fmini_app.c` | "CPU High: 1 CORE" |
| `firmware/src/console.c` | `gov_vcap`, `gov_voice_us`, `gov_base_us`, `gov_hot`, `cpu2_vbias` |

The host tests `tests/audio_test.c` (the old shed's two halves) and `tests/dualcore_test.c` (bit-for-bit one core's
output: the wet path is now a block later) describe the old behaviour. The Installer workflow does not run them.

## 2026-11-04: voices on both cores

FMini-1 plays every voice on track 1 (tracks 2-4 are muted parts). The FM-1's second core (MENU > SYSTEM > 2ND CORE ON)
was only ever handed whole parts by `fx.c mix_block`, so it idled while the first core rendered all 12 voices, and at full
polyphony the render ran out of time ("Warning: CPU High", voices cut). Over TRS MIDI too: it was never the MIDI.

| File | Change |
| --- | --- |
| `firmware/src/voice.c` | `track_render`'s voice loop is its own function, `track_voices` (the voices of a mask, with the block's values in a `vblk_t`). For an FMINI part with two or more voices sounding, while the second core has no part of its own that block, `track_render` hands a share of the sounding voices to the second core (`c1_voices`, into `vs_out` / `vs_spread`), renders the rest itself, waits, and adds them in. If the second core stops answering, its voices are rendered on the first (one glitch, then one core, as the part split does) |
| `firmware/src/fx.c` | `mix_block` sets `vs_parts` while the second core has parts, so no voices are handed to it then |
| `firmware/src/console.c` | `cpu2_vblocks`: the blocks the second core took voices of |
| `DEVELOPING.md` | "Two cores for one part's voices" under section 5 |

Why it is safe: each voice's render writes its own `voice_t` and the output buffer it is given; the part's values are
made before the voices and put back after them. The one shared thing written during the voices is the VCO drift's draw
from the part's noise generator (`eng_fmini.c fmini_nst`): two cores drawing at once can repeat a draw, which only makes
two voices' slow drift alike for a moment. The split is for the FMINI engine only.

What it needs: MENU > SYSTEM > 2ND CORE ON (FMini-1's BETA setting, off by default), and an FM-1 whose second core reads
correctly (the firmware turns it off by itself on one that does not: `2ND CORE MISREAD: OFF`).

## 2026-11-04: the dark psychedelic look

The first look (bright poison green, hot pink and acid yellow on violet, earlier the same day) is replaced by a dark one,
after the dark psychedelic art of the psytrance scene.

| File | Change |
| --- | --- |
| `firmware/src/fmini_app.c` | The palette: values, curves and the active tab luminous teal `#00FFC8`, labels ultraviolet `#CFB4FF`, muted dim violet `#6E5E96`, red blood magenta `#FF1E6E`, blue `#2E8CFF`, the panel a near-black gradient (`#140B26` to `#020104`), the dialogs, rows, tabs and boxes in very dark violets, the buttons dark gold `#D8A030`, the knobs cold gunmetal. The wooden side cheeks are dark conduits with a glowing vein (teal, ultraviolet, magenta down the screen) and nodes |
| `firmware/src/fmini_splash.h`, `assets/fmini-splash.png` | The power-on and ABOUT picture: an eight-fold fractal mandala of fine glowing lines around a teal eye, on black, with OZY'S / FMini-1 / MODDED, drawn with its own 256-colour palette (four glow ramps and the inks; the header is written straight from the palette indices, no quantizing). `tools/gen_splash.py` still makes the header from a new PNG |
| `assets/readme/banner.png`, `assets/fmini-installer-hero.jpg` | The README's banner and the site's picture, the same art |
| `web/site_fmini.html` | The published site (the workflow builds it into the installer page): the dark palette and glow, the name, "a fork of FMini-1 by Ruppert", this fork's source link in the credits; the support section names FMini-1's author as the one it supports |
| `web/index_fmini.html` | The stand-alone installer page (not published by the workflow): the same colours, name and credits |

The MENU screens drawn by Felucca's own UI code (and the browser emulator, built from upstream) keep their colours.

## 2026-11-04: the name and the documents

| File | Change |
| --- | --- |
| `README.md` | Rewritten for the fork: what it is, what changed, installing (and turning the second core on), building and publishing, keeping up with FMini-1, credits, licence and trademarks |
| `CHANGES-OZY.md` | This file |
| `CREDITS.md`, `LICENSING.md` | The fork and its author at the top; the modified-version notice |
| `CLAUDE.md` | The fork's identity and owner for AI-assisted sessions |
| `.github/workflows/installer.yml` | The run's summary names the fork; the JieLi SDK download from gitee.com is tried four times (it is sometimes unreachable from GitHub's runners for a few minutes) |

## Tried and dropped: Bluetooth MIDI (2026-11-02 to 2026-11-04)

Bluetooth LE MIDI was ported from fm1-dock (Optimist's open BLE stack) and tested on an FM-1 over six builds. It
connected and played, but stayed too unreliable next to the stock firmware's Bluetooth: about half the radio packets
came in flagged as bad, notes came late, and some were lost and hung. It was taken out, put back once on 2.1.0 with the
two-core voices (still CPU High and tearing), and taken out again. Nothing of it is in this fork.
