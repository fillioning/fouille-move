# Fouille

Network microsound excavator for [Ableton Move](https://www.ableton.com/move/),
built for the [Schwung](https://github.com/charlesvestal/schwung) framework.

> *"The archive becomes a living acoustic organism."* — Emiliano Pennisi

## What is Fouille?

Fouille (French: *to dig, excavate*) treats the internet as an inexhaustible sound quarry. It uses Move's wifi to fetch short audio fragments from [Freesound.org](https://freesound.org) and the [Internet Archive](https://archive.org), analyzes them on arrival, and sculpts them through a vactrol-modeled envelope engine rooted in musique concrète and lowercase practice.

Every performance is unique. The sound palette is alive — new material trickles in continuously, old material erodes away. Pressing a pad both plays *and* can summon new sounds from the archive.

## Features

- **Dual Seeder** — fetches from Freesound (tagged, descriptor-filtered) and Internet Archive (radio-aporee field recordings)
- **Corpus Navigator** — 4×8 pad grid maps to a 2D descriptor space (brightness × loudness), always musically organized
- **Vactrol LPG** — per-voice coupled amplitude + filter inspired by Buchla Low Pass Gates
- **Ternary Envelope** — Rise/Hold/Fall with three sustain modes: Freeze, Drift, Granulate
- **Echo Pool** — Phase Garden-style feedback re-injection
- **Spectral Lock** — resonant bandpass tracker that extracts tone from noise
- **Erosion Filter** — progressive degradation giving sounds a natural lifecycle
- **Organic Drift** — Brownian LFO per voice for non-periodic slow modulation
- **Lowercase headroom** — designed for very low levels, rewards amplification

## Controls

| Page | Knobs 1–8 |
|------|-----------|
| **Excavation** | Terrain · Depth · Rise · Hold · Fall · Strike · Lock · Erode |
| **Currents** | Flow · Scatter · Stretch · Grain · Drift · Pool · Spread · Volume |

## Setup

### Freesound API Key (optional)

Fouille can work with Internet Archive alone (no key needed). For Freesound access:

1. Register at [freesound.org/apiv2/apply](https://freesound.org/apiv2/apply)
2. Create a file on Move: `/data/UserData/schwung/modules/sound_generators/fouille/freesound.key`
3. Paste your API token into the file

### Building

```
./scripts/build.sh
```

Requires Docker or an `aarch64-linux-gnu-gcc` cross-compiler.

### Installation

```
./scripts/install.sh
```

Or install via the Module Store in Schwung.

## Artistic Lineage

Fouille is directly inspired by the work of **Emiliano Pennisi** (Avenir) and his three-tool ecosystem for deep sonic exploration:

- [**Envion**](https://github.com/aveniridm/envion) (MIT, Pure Data / Max) — Found Net Sound, Dynatext ternary envelopes, de-authoring principle
- [**Endogen**](https://www.peamarte.it/endogen/main.html) (Max/MSP + SuperCollider) — vactrol LPG dynamics, lowercase synthesis, corpus navigation, Phase Garden feedback
- [**Interfera**](https://www.peamarte.it/interfera/interfera_landing.html) (Max/MSP) — geosonic field recording engine, Internet Archive seeder, Spectral Lock, Erosion Recorder

No code is ported from these projects. The conceptual framework — de-authoring, envelope-first articulation, network-as-organism, vactrol dynamics, erosion lifecycle — is a direct lineage.

## License

MIT — see [LICENSE](LICENSE)

Sound content fetched from Freesound and Internet Archive may be under various Creative Commons licenses. Users are responsible for respecting the licenses of fetched content in their published works.
