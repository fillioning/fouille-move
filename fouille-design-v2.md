# Fouille — Network Microsound Generator for Ableton Move

> *"The archive becomes a living acoustic organism."*
> — Emiliano Pennisi

> *"Sound is not chosen, but found. Not composed, but excavated."*
> — Interfera manifesto

## Concept

**Fouille** (French: *to dig, excavate*) is a sound generator for Ableton Move that treats the internet as an inexhaustible sound quarry. Inspired by Emiliano Pennisi's complete triad — **Envion** (envelope-first articulation), **Endogen** (lowercase microsound synthesis), and **Interfera** (geosonic field recording excavation) — Fouille fetches micro-sounds from Freesound.org and the Internet Archive via Move's wifi, analyzes them on arrival, and sculpts them through a vactrol-modeled envelope engine rooted in musique concrète and lowercase practice.

### The Pennisi Triad, Compressed into a Groovebox

Pennisi's three tools each address a different relationship to sound:

- **Envion** — procedural envelope-first logic. Sound is articulated through ternary envelopes, sourced from the web, depersonalized.
- **Endogen** — generative internal organism. Vactrol LPGs, modal synthesis, cybernetic feedback, ~150 parameters evolving over minutes.
- **Interfera** — geosonic excavation. Sound found via geographic coordinates and semantic queries, processed through spectral lock, pulse engine, erosion recorder.

Fouille compresses this into Move's 8-knob × 32-pad interface:
- From **Envion**: the Dynatext ternary envelope, Found Net Sound, de-authoring
- From **Endogen**: vactrol LPG dynamics, lowercase headroom, organic drift, corpus-based navigation, feedback re-injection
- From **Interfera**: dual seeder architecture (semantic + geographic*), spectral lock processing, erosion/degradation, tape scrub

*Geographic seeding is aspirational — Move has no GPS. But the semantic keyword seeder from Interfera's Internet Archive mode maps directly.

### Core Philosophy

**De-authoring**: the network provides the material. You provide the gesture. Every performance is unique because the sound palette is alive — new material trickles in continuously, old material erodes away. Pressing a pad both plays *and* summons new sounds from the archive.

---

## Architecture Overview

```
┌──────────────────────────────────────────────────────────────────┐
│                          FOUILLE                                  │
│                                                                   │
│  ┌──────────────────┐   ┌────────────────────────────────────┐   │
│  │  DUAL SEEDER     │   │       SOUND POOL (RAM)              │   │
│  │  (bg thread)     │──▶│  8 slots × ~10s mono 22kHz          │   │
│  │                  │   │  per-slot descriptors:               │   │
│  │  Freesound API   │   │    centroid, loudness, noisiness     │   │
│  │  Internet Archive│   │  double-buffered, atomic swap        │   │
│  │  Poetic keywords │   │  circular rotation + pad-triggered   │   │
│  └──────────────────┘   └──────────┬─────────────────────────┘   │
│                                    │                              │
│                    ┌───────────────▼────────────────────┐        │
│                    │     CORPUS NAVIGATOR (pad grid)     │        │
│                    │  4×8 pads → 2D descriptor space     │        │
│                    │  X: brightness  Y: density           │        │
│                    │  velocity → scatter + LPG strike     │        │
│                    └───────────────┬────────────────────┘        │
│                                    │                              │
│                    ┌───────────────▼────────────────────┐        │
│                    │      VACTROL LPG VOICE ENGINE       │        │
│                    │  8 voices, round-robin              │        │
│                    │                                     │        │
│                    │  Per voice:                         │        │
│                    │  • Ternary envelope (Rise/Hold/Fall)│        │
│                    │  • Vactrol LPG (amp+filter coupled) │        │
│                    │  • Organic drift modulator           │        │
│                    │  • Pitch/pan/position                │        │
│                    └───────────────┬────────────────────┘        │
│                                    │                              │
│        ┌───────────────────────────▼─────────────────────┐       │
│        │              PROCESSING CHAIN                    │       │
│        │                                                  │       │
│        │  Spectral Lock → Pulse Engine → Erosion Filter   │       │
│        │  → Feedback Delay → Echo Pool → Soft Limiter     │       │
│        │                         ↑                        │       │
│        │              ┌──────────┴──────────┐             │       │
│        │              │    ECHO POOL         │             │       │
│        │              │  (Phase Garden-style │             │       │
│        │              │   feedback buffer)   │             │       │
│        │              └─────────────────────┘             │       │
│        └──────────────────────────────────────────────────┘       │
│                                                                   │
└───────────────────────────────────────────────────────────────────┘
```

---

## Dual Seeder (Fetch Engine)

### Inspired by Interfera's Architecture

Interfera uses three seeders — geographic (Aporee), semantic (Internet Archive), and procedural (Envion). Fouille adapts this to two seeders suitable for Move's embedded environment:

**Seeder 1 — Freesound (descriptor-aware)**
- Queries Freesound API v2 with token key
- Supports text search + descriptor filtering (spectral centroid, loudness, noisiness)
- Previews don't require OAuth — direct MP3 download
- Best for: categorized, tagged, individual sound events

**Seeder 2 — Internet Archive (semantic drift)**
- Queries `archive.org/advancedsearch` against the `radio-aporee-maps` collection
- Keyword/tag/place-name search — same API Pennisi uses in Interfera
- Returns direct MP3 URLs at 320kbps
- Best for: field recordings, environmental textures, site-specific ambience
- Enables Interfera's "semantic drift" — type "rain" or "Tokyo" or "bells" and let the archive respond

### Thread Architecture

A background pthread handles all network I/O. The audio thread never touches the network.

**Per-slot state** (lock-free via `stdatomic.h`):
```c
typedef struct {
    atomic_int state;          // EMPTY=0, LOADING=1, READY=2, PLAYING=3
    float *pcm_a, *pcm_b;     // double-buffered PCM
    atomic_int active_buf;     // which buffer is live (0=a, 1=b)
    int length;                // samples in current buffer
    // Descriptors (computed on arrival)
    float centroid;            // spectral brightness 0-1
    float loudness;            // RMS energy 0-1
    float noisiness;           // spectral flatness 0-1
} sound_slot_t;
```

### Descriptor Analysis on Arrival

When a new sound arrives, the fetch thread computes three simple descriptors before making it available:
- **Spectral centroid** — weighted mean frequency (brightness)
- **RMS loudness** — overall energy
- **Spectral flatness** — noise vs tone ratio

These are used by the Corpus Navigator to map sounds to the pad grid.

### Query Word System

Three layered sources, inspired by the Envion + Interfera approach:

**1. Curated poetic word lists** (Pennisi's de-authoring principle):
```
texture, rust, breath, glass, membrane, erosion, vapor, residue,
hum, crackle, whisper, murmur, resonance, fragment, sediment,
pulse, shimmer, static, drift, grain, fossil, echo, patina,
threshold, dissolution, filament, oxidation, tremor, lichen,
rain, market, forest, bells, microsound, signal, surface, dust
```

**2. Category knob** (Knob 1 — "Terrain"):
| Value | Category | Seeder preference |
|-------|----------|-------------------|
| 0-12% | Nature | IA (aporee field recordings) |
| 13-25% | Machines | Freesound (tagged industrial) |
| 25-37% | Voices | Freesound (speech, whisper, choir) |
| 38-50% | Abstract | Freesound (noise, texture, drone) |
| 51-62% | Material | Freesound (metal, wood, glass, paper) |
| 63-75% | Urban | IA (city, market, subway) |
| 76-87% | Organic | Freesound (body, liquid, biological) |
| 88-100% | Cosmic | Freesound (radio, electromagnetic, signal) |

**3. Descriptor filtering** (Knob 2 — "Depth"):
Low values fetch dark/bassy sounds, high values fetch bright/airy sounds via Freesound's `descriptors_filter` parameter.

### Rotation Behavior

- **Continuous trickle:** Every ~20s (configurable via Flow knob), replace the oldest non-playing slot
- **Pad-triggered fetch:** Velocity > 100 queues a new fetch — high velocity = urgency
- **Seeder alternation:** Fetches alternate between Freesound and Internet Archive for maximum variety
- **Offline fallback:** If wifi fails, use existing buffer + generate synthetic micro-textures (filtered noise bursts, sine clusters) as seed material

---

## Corpus Navigator (Pad Grid)

### Inspired by Endogen's FluCoMa 2D Corpus Explorer

Rather than fixed Pad→Slot mapping, the pad grid becomes a **2D descriptor space**:

```
         Bright (high centroid)
            ▲
            │
    Quiet ──┼── Loud
            │
            ▼
         Dark (low centroid)
```

- **X axis (pad columns 1-8):** maps to loudness (quiet → loud)
- **Y axis (pad rows 1-4):** maps to spectral centroid (dark → bright)

When a pad is pressed, the Navigator finds the sound slot whose descriptors are **closest** to that pad's position in the 2D space. Multiple pads can trigger the same slot at different positions within it, or different slots entirely.

This means the pad grid is always musically organized — even as new sounds replace old ones, the mapping stays coherent. Dark/quiet sounds cluster in one corner, bright/loud in the other.

**Velocity** controls two things simultaneously:
1. **Strike intensity** for the vactrol LPG (harder = brighter, louder bloom)
2. **Position scatter** within the selected sound (harder = more random offset)

---

## Vactrol LPG Voice Engine

### Inspired by Endogen's Resonant Objects + Lowercase Dynamics

The biggest departure from the v1 design: replacing the simple SVF + ADSR with a **vactrol-modeled Low Pass Gate** per voice. This is the heart of Endogen's sound — and the reason microsounds feel physical rather than digital.

### What a Vactrol LPG Does

In hardware, a vactrol is an LED + photoresistor in a sealed tube. When voltage hits the LED, light gradually reaches the photoresistor, which opens both an amplifier and a filter simultaneously. The result:

- **Coupled amplitude + filter**: as the sound gets louder, it also gets brighter. As it decays, it gets darker. This is how acoustic instruments work — a struck bell gets duller as it fades.
- **Asymmetric response**: fast-ish attack (LED lights up quickly), slow exponential decay (photoresistor relaxes slowly). The decay time depends on the strike intensity.
- **Non-linear**: gentle strikes produce warm, muffled sounds. Hard strikes produce bright, snappy transients. Same mechanism, totally different character.

### Implementation (per voice)

```c
typedef struct {
    float vactrol_cv;          // "LED brightness" 0-1
    float vactrol_state;       // "photoresistor" state 0-1 (slowly follows cv)
    float lpg_cutoff;          // derived from vactrol_state
    float lpg_amplitude;       // derived from vactrol_state

    // Vactrol model parameters
    float rise_rate;           // LED → LDR coupling speed (attack character)
    float fall_rate;           // LDR relaxation speed (decay character)
    float nonlinearity;        // how much harder strikes = brighter response
} vactrol_lpg_t;
```

**Per sample:**
```
vactrol_cv = envelope_output * velocity^nonlinearity
vactrol_state += (vactrol_cv - vactrol_state) * (cv > state ? rise_rate : fall_rate)
lpg_cutoff = 200 + vactrol_state * 18000  // 200Hz–18kHz
lpg_amplitude = vactrol_state^0.7         // soft power curve
output = onepole_lpf(sample * lpg_amplitude, lpg_cutoff)
```

The ternary envelope (Rise/Hold/Fall) feeds into `vactrol_cv`. The vactrol model then adds its own organic character on top. Gentle pad touches produce Buchla-style "bongo" blooms. Hard strikes produce bright microsound snaps.

### Ternary Envelope (feeds the LPG)

Following Envion's Dynatext paradigm — three segments:

- **Rise (Knob 3):** 0.5ms → 2s. Shape morphs from exponential snap to logarithmic swell.
- **Hold (Knob 4):** While pad is held — three behaviors:
  - **Freeze** (low): hold at current sample position, vactrol sustains
  - **Drift** (mid): slowly scan buffer with organic LFO, vactrol modulates gently
  - **Granulate** (high): micro-loop current region with jitter, vactrol pulses
- **Fall (Knob 5):** 1ms → 5s. The LPG's natural decay extends this — even with a short Fall, the vactrol's slow relaxation creates a warm tail.

---

## Processing Chain

### Spectral Lock (from Interfera)

Interfera's **Spectral Lock** is described as a "lock-in amplifier inspired spectral isolation and tracking for frequency-selective processing." For Fouille, this becomes a **resonant bandpass tracker**:

A narrow bandpass filter that slowly tracks the dominant frequency of the incoming signal. When engaged, it isolates and sustains the most prominent spectral component, creating singing, ringing tones from noisy source material. Like listening through a resonant tube that slowly tunes itself to whatever it hears.

On Move: controlled by Knob 7 "Lock" — at 0%, bypassed. As you increase, the Q narrows and the tracker engages, gradually extracting pure tone from noise.

### Pulse Engine (from Interfera)

Interfera's **Pulse Engine** provides "discrete-time emission and micro-pulsar synthesis for granular articulation." For Fouille, this gates the output through a stochastic pulse train:

Instead of continuous output, the signal is chopped into micro-events — tiny windows of sound separated by silence. Density (pulses per second) and regularity (periodic → chaotic) are controllable. At low density, individual micro-sounds emerge from silence. At high density, it becomes a buzzing texture. This is Curtis Roads' microsound territory.

On Move: integrated into the Hold sustain behavior — the "Granulate" mode uses the Pulse Engine internally.

### Erosion Filter (from Interfera)

Interfera's **Erosion Recorder** captures "unstable states and temporal decay." For Fouille, this becomes a degradation processor:

A filter that progressively removes spectral content over time — like tape wearing out, or metal oxidizing. Each time a sound is played, it can be slightly more eroded than the last. New fetched sounds arrive fresh; as they age in the buffer, they gradually degrade. This creates a natural lifecycle: sounds are born (fetched), live (played), age (erode), and die (replaced).

On Move: Knob 8 "Erode" controls the erosion rate. At 0%, sounds stay pristine. At 100%, aggressive spectral thinning — only the most resonant frequencies survive.

### Echo Pool (from Endogen's Phase Garden)

Fouille's output is re-recorded into a circular **Echo Pool** buffer. This buffer becomes a 9th sound source that voices can read from, creating feedback:

**Gestures become material → material becomes texture → texture feeds back into new synthesis.**

The Echo Pool captures the last ~5 seconds of Fouille's output. When engaged, one of the 8 pad zones can be redirected to read from the Echo Pool instead of from the network-fetched slots. This means previously played sounds — already shaped by your envelopes and processing — become raw material for further excavation.

On Move: controlled via Page 2 "Pool" parameter. At 0%, Echo Pool is silent. As you increase, it blends with fresh network material. At 100%, Fouille is entirely self-feeding.

### Lowercase Headroom + Companding

Following Endogen's philosophy: Fouille defaults to **very low output levels**. Microsound detail is preserved. Multiple voices layer without clipping. A gentle compander (inspired by Endogen's internal companding) keeps dynamics stable while preserving the quietest transients.

The user brings the level up downstream — through Move's track volume, or external amplification. This is intentional: lowercase music rewards amplification, where details invisible at normal levels become physical at high monitoring volume.

### Organic Drift (from Endogen)

Rather than periodic LFOs, a **Brownian motion drift generator** slowly modulates:
- Slice read position (±5% wander)
- LPG cutoff bias (±10% wander)
- Stereo pan (slow spatial drift)

Built in the style of Endogen's organic LFO — closer to irregular drift than classic sine modulation. Computed per-voice, so each voice wanders independently. Makes the sound feel alive even when you're not touching anything.

---

## Parameter Layout

### Page 1: Excavation (root — main performance page)

| Knob | Name | Range | Description |
|------|------|-------|-------------|
| 1 | Terrain | 8 categories | Sound category + seeder routing |
| 2 | Depth | 0–100% | Spectral brightness filter for queries |
| 3 | Rise | 0.5ms–2s | Ternary envelope attack → LPG strike |
| 4 | Hold | freeze/drift/granulate | Sustain behavior mode |
| 5 | Fall | 1ms–5s | Ternary envelope decay + LPG tail |
| 6 | Strike | soft → hard | Vactrol nonlinearity (gentle bloom → bright snap) |
| 7 | Lock | bypass → narrow Q | Spectral Lock resonant tracker |
| 8 | Erode | 0–100% | Erosion degradation rate |

### Page 2: Currents (secondary — network & texture control)

| Knob | Name | Range | Description |
|------|------|-------|-------------|
| 1 | Flow | 5s–120s | Auto-rotation interval |
| 2 | Scatter | 0–100% | Velocity-to-position randomness |
| 3 | Stretch | 0.25x–4x | Playback speed (pitch-linked) |
| 4 | Grain | 1ms–200ms | Pulse Engine micro-event window size |
| 5 | Drift | 0–100% | Organic LFO modulation depth |
| 6 | Pool | 0–100% | Echo Pool feedback blend |
| 7 | Spread | 0–100% | Stereo voice panning width |
| 8 | Volume | 0–100% | Master output (low default!) |

---

## Technical Constraints & Solutions

### Threading on Move (Linux/ARM64)

Move runs Linux. `pthread_create` is available. The fetch thread:
- Runs at lowest priority (`SCHED_IDLE`)
- Sleeps between fetches (`nanosleep`)
- Communicates with audio thread via `stdatomic.h` atomics only
- Computes descriptors (centroid, loudness, flatness) in the fetch thread after decoding
- Never touches `render_block` data directly — only writes to shadow buffers

### HTTP Fetching

**Approach: `popen("curl")`** for v0.1 — curl is available on Move's Linux. The fetch thread:
1. Builds the API URL (Freesound or Internet Archive)
2. `popen("curl -s <url>")` to fetch JSON response
3. Parse JSON (minimal parser, or just sscanf for the preview URL field)
4. `popen("curl -s -o /tmp/fouille_slot_N.mp3 <preview_url>")`
5. Decode with minimp3 → compute descriptors → atomic swap

### MP3 Decoding + Descriptor Computation

**minimp3** (header-only, public domain, ~15KB): Decode → float PCM in a single pass.

**Descriptor computation** (in fetch thread, ~50 lines of C):
```
centroid = Σ(freq[i] * magnitude[i]) / Σ(magnitude[i])  // via simple FFT
loudness = sqrt(Σ(sample^2) / N)                          // RMS
flatness = geometric_mean(magnitude) / arithmetic_mean(magnitude)  // spectral flatness
```

A lightweight 256-point FFT (Radix-2 DIT, ~80 lines of C) is sufficient for descriptor extraction. This runs once per fetched sound, not in the audio thread.

### Vactrol LPG Model (audio thread)

The vactrol model is extremely lightweight — one multiply-accumulate per sample for the state follower, plus a one-pole lowpass (2 multiplies). Total per-voice cost: ~5 operations/sample. With 8 voices × 128 frames: ~5120 operations per render_block. Negligible on ARM64.

### Memory Budget

```
8 slots × 10 seconds × 22050 Hz × sizeof(float) = ~6.7 MB
+ shadow buffers (×2): ~13.4 MB
+ Echo Pool (5s stereo): ~0.9 MB
+ minimp3 decode buffer: ~1 MB
+ FFT scratch: ~4 KB
+ instance struct + misc: ~0.1 MB
Total: ~15.4 MB
```

### API Key Management

- Freesound key: `/data/UserData/schwung/modules/sound_generators/fouille/freesound.key`
- Internet Archive: no key needed (public API)
- First-run: operates in IA-only mode if no Freesound key exists
- Status via `get_param("status")`: "Fetching...", "8/8 slots", "Offline", "No Freesound key — using IA only"

---

## Module Identity

| Field | Value |
|-------|-------|
| **Module ID** | `fouille` |
| **Name** | Fouille |
| **Abbreviation** | `FOUIL` |
| **Author** | Vincent Music (fillioning) |
| **Description** | Network microsound excavator — fetches sounds from the internet, sculpts them with vactrol LPG envelopes |
| **License** | MIT (with Freesound/IA content CC attribution) |
| **Language** | C (with pthreads) |
| **Voice architecture** | 8-voice polyphonic, round-robin stealing |
| **Component type** | sound_generator |

---

## Artistic Credit & Lineage

Fouille is directly inspired by the work of **Emiliano Pennisi** (Avenir) and his three-tool ecosystem:

- **Envion** (MIT, Pure Data / Max) — Found Net Sound, Dynatext ternary envelopes, de-authoring principle, Freesound seeder architecture
- **Endogen** (Max/MSP + SuperCollider) — vactrol LPG dynamics, lowercase headroom philosophy, organic drift modulation, corpus-based 2D navigation (via FluCoMa), Phase Garden feedback re-injection, ~150-parameter slow evolution
- **Interfera** (Max/MSP) — geosonic field recording engine, Internet Archive seeder, Spectral Lock, Pulse Engine, Erosion Recorder, de-location principle, dual-seeder complementarity

Fouille does not port any code from these projects. The conceptual framework — de-authoring, envelope-first articulation, network-as-organism, vactrol dynamics, erosion lifecycle — is a direct lineage that is acknowledged in README and documentation.

Interfera's **miRings** module (clone of Mutable Instruments Rings by Volker Böhm / Émilie Gillet) also suggests a future direction: physical modeling resonator as a processing stage, where fetched sounds excite a resonant body. This could be a v1.0 feature.

---

## Implementation Phases

### v0.1 — Proof of concept
- Scaffold repo with build system
- Implement fetch thread with curl + minimp3
- Freesound seeder only, hardcoded word list
- 8 slots with descriptor computation
- Simple fixed pad→slot mapping (corpus navigator comes in v0.2)
- Basic vactrol LPG (one-pole model)
- Single page (Excavation), 8 knobs

### v0.2 — Corpus Navigator + Dual Seeder
- Internet Archive seeder (radio-aporee-maps collection)
- Pad grid → 2D descriptor space mapping
- Full ternary envelope with Rise/Hold/Fall
- Hold modes: freeze, drift, granulate (pulse engine)
- Category knob with seeder routing

### v0.3 — Processing chain
- Spectral Lock (resonant bandpass tracker)
- Erosion filter (progressive degradation)
- Echo Pool (Phase Garden-style feedback buffer)
- Organic drift modulator (Brownian LFO per voice)
- Page 2 (Currents) parameters
- Lowercase companding

### v0.4 — Polish & release
- Presets (curated word list × envelope × processing combinations)
- Status display in Shadow UI
- Offline synthetic seed material (filtered noise, sine clusters)
- README with API key setup + Pennisi attribution
- miRings-style resonator exploration (stretch goal)
