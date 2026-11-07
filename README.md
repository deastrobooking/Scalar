# Scalar

**A stripped-down [GLIESE 710](https://github.com/deastrobooking/GLIESE_710) voice as a Eurorack module, running on ESP32-S3.**

Scalar ports the GLIESE 710 synthesis engines out of the JUCE plugin and onto
embedded hardware. Like Mutable Instruments Plaits, it is a *macro-voice*: six
engines are compiled in, one is active at a time, selected from the panel.

| Engine | Source |
|---|---|
| **VA** | PolyBLEP virtual analog with unison, ring mod, wavefold |
| **WT** | Wavetable with FFT mipmaps, warp modes, phase distortion |
| **FM** | 10-operator FM, 40 algorithms, algorithm morph |
| **3D** | GLIESE's signature 3D function oscillator (18 fields, 5 paths) |
| **TERRAIN** | Wave-terrain oscillator with procedural or raw heightmaps |
| **GRAN** | Granular engine with modal body resonator (needs PSRAM) |

Signal flow (full voice): `engine → dual filter (31 modes, filter ADSR) → amp ADSR/VCA → stereo out`.

---

## Repository layout

```text
Scalar/
├── CMakeLists.txt          # native host build (development + tests)
├── dsp/                    # platform-agnostic DSP core
│   ├── shim/JuceShim.h     # minimal juce:: compatibility layer
│   ├── gliese/             # engines ported near-verbatim from GLIESE_710
│   │   ├── Oscillators/    # Osc3D, Terrain, Wavetable, FM, VA, Granular
│   │   ├── Envelopes/      # ADSR
│   │   └── Filter/         # DualFilter + all 31 filter models
│   └── scalar/             # ScalarVoice: engine select + full voice wiring
├── tools/render/           # native WAV renderer / smoke test
└── firmware/               # ESP-IDF project (ESP32-S3)
    ├── components/scalar_dsp/  # wraps dsp/ as an IDF component
    └── main/
        ├── audio_task.cpp  # core 1: render loop → I2S → PCM5102A
        ├── control_task.cpp# core 0: CV/gate, pots, engine select (~1 kHz)
        ├── usb_midi.cpp    # TinyUSB class-compliant USB MIDI
        ├── param_bridge.h  # lock-free control→audio snapshots
        └── board.h         # pin map + audio format
```

### Porting approach

The engine sources in `dsp/gliese/` stay as close to upstream GLIESE_710 as
possible so fixes can be diffed across. Instead of rewriting, a ~400 line
[`JuceShim.h`](dsp/shim/JuceShim.h) supplies the small `juce::` surface the DSP
code touches (`jlimit`, `MathConstants`, `SmoothedValue`, a TPT SVF, a biquad,
and an init-time FFT). Desktop-only paths were removed:

- Terrain image import → replaced by `setHeightmap(const float*, w, h)` (raw grids from flash/SD)
- Granular sample serialization (JUCE `MemoryBlock`) → removed; `createSampleData()` from raw floats remains

## Native development build

No hardware needed. Requires CMake ≥ 3.22 and a C++20 compiler:

```bash
cmake -B build -S .
cmake --build build --parallel
./build/scalar_render out_dir     # renders one WAV per engine
ctest --test-dir build            # smoke test: finite, non-silent output
```

## Firmware (ESP32-S3)

Target: **ESP32-S3 with octal PSRAM** (e.g. ESP32-S3-WROOM-1-N8R8).
PSRAM is required by the granular engine's sample buffers (~1 MB).

```bash
cd firmware
idf.py set-target esp32s3
idf.py build flash monitor
```

Requires ESP-IDF ≥ 5.2. The `esp_tinyusb` dependency is fetched by the
component manager on first build.

### Hardware

| Function | Connection |
|---|---|
| Audio out | PCM5102A I2S DAC (BCK=GPIO15, LRCK=GPIO16, DIN=GPIO17) → Eurorack output stage |
| Gate in | GPIO18 (condition Eurorack gate to 3.3 V logic) |
| V/Oct CV | ADC1_CH0 (±5 V scaled to 0–3.3 V) |
| Pots | cutoff / resonance / timbre / morph on ADC1_CH3–CH6 |
| Engine select | button on GPIO0 (devkit boot button) |
| MIDI | native USB (class-compliant device) |

The two *timbre*/*morph* macro pots are remapped per engine (VA: shape
morph/wavefold; FM: algorithm/morph; 3D: field frequency/field morph;
terrain: scan rate/terrain morph; granular: position/grain size — see
[`control_task.cpp`](firmware/main/control_task.cpp)).

> **V/Oct accuracy:** the internal ESP32 ADC is fine for pots and mod CVs but
> marginal for precise pitch tracking. The production design should route
> V/Oct through an external ADC (ADS1115/MCP3202); the control task already
> isolates pitch sourcing so this is a drop-in change.

## Status / roadmap

- [x] Six GLIESE engines ported and verified on the native build
- [x] Full-voice wiring: engine → dual filter + filter ADSR → amp ADSR
- [x] ESP-IDF firmware skeleton: I2S audio task, control task, USB MIDI
- [ ] Build + profile on real ESP32-S3 hardware (per-engine CPU headroom)
- [ ] External ADC for V/Oct + calibration routine
- [ ] Wavetable scan / granular sample loading from SD or flash
- [ ] Panel/HP design, CV attenuverters for timbre/morph

## License

The DSP engines derive from GLIESE_710. JUCE itself is **not** used by this
project (see `dsp/shim/`), so no JUCE license applies here.
