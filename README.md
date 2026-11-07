# Scalar

**A stripped-down [GLIESE 710](https://github.com/deastrobooking/GLIESE_710) voice as a Eurorack module, running on ESP32-S3.**


| Engine | Source |
|---|---|
| **VA** | PolyBLEP virtual analog with unison, ring mod, wavefold |
| **WT** | Wavetable with FFT mipmaps, warp modes, phase distortion |
| **FM** | 10-operator FM, 40 algorithms, algorithm morph |
| **3D** | GLIESE's signature 3D function oscillator (18 fields, 5 paths) |
| **TERRAIN** | Wave-terrain oscillator with procedural or raw heightmaps |
| **GRAN** | Granular engine with modal body resonator (needs PSRAM) |

Signal flow (full voice): `engine → dual filter (31 modes, filter ADSR) → amp ADSR/VCA → stereo out`.
The current panel firmware selects LP24/AOnly; other filter modes are library APIs.

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
│   └── scalar/             # voice, MIDI/CV router, bounded control transport
├── tests/                  # design regressions and concurrent handoff tests
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
ctest --test-dir build --output-on-failure # smoke, design regressions, PCM boundary
```

## Firmware (ESP32-S3)

Target: **ESP32-S3 with octal PSRAM** (e.g. ESP32-S3-WROOM-1-N8R8).
All six engines are prepared before playback. Their wavetable, terrain, and
granular assets require PSRAM even when another engine is selected. The current
board profile explicitly requires ESP32-S3; other ESP32 variants need their own
validated pin, memory, scheduling, and MIDI profiles.

```bash
cd firmware
python -m pip install -r requirements-build.txt # inside the exported ESP-IDF environment
idf.py set-target esp32s3
idf.py build flash monitor
```

Cross-built with **ESP-IDF 5.2.3**, using the 5.2.x SDK profile and pinned
`esp_tinyusb` 1.7.6~2. Component versions are recorded in
[`dependencies.lock`](firmware/dependencies.lock) and fetched on first build.
CI builds the native tests and ESP32-S3 firmware. Hardware timing and electrical
validation remain on the roadmap.

### Hardware

| Function | Connection |
|---|---|
| Audio out | PCM5102A I2S DAC (BCK=GPIO15, LRCK=GPIO16, DIN=GPIO17) → Eurorack output stage |
| Gate in | GPIO18 (condition Eurorack gate to 3.3 V logic) |
| V/Oct CV | ADC1_CH0 (±5 V scaled to 0–3.0 V, with calibrated offset/gain) |
| Pots | cutoff / resonance / timbre / morph on ADC1_CH3–CH6 |
| Engine select | debounced button on GPIO0 (devkit boot button; holding it at reset enters download mode) |
| MIDI | native USB (class-compliant device, MIDI channel 1, last-note priority) |

The two *timbre*/*morph* macro pots are remapped per engine (VA: shape
morph/wavefold; WT: full-bank scan/phase bend; FM: algorithm/morph; 3D: field frequency/field morph;
terrain: scan rate/terrain morph; granular: position/grain size — see
[`ControlUtils.h`](dsp/scalar/ControlUtils.h)). WT scan and warp amounts ramp over 5 ms.

> **V/Oct accuracy:** the internal ESP32 ADC is fine for pots and mod CVs but
> limited for precise pitch tracking. Internal pitch uses the ESP-IDF factory
> ADC calibration plus front-end offset/gain in `board.h`. Calibrate these
> against measured 0 V and +1 V input readings. Existing 0–3.3 V front ends need
> their own constants and do not have a fully characterized top-end range.
> An external ADC driver can call `cv::setPitchVolts()` in `control_task.h`;
> `cv::useInternalPitch()` restores the internal source. Select and validate the
> external converter against the intended pitch accuracy and latency.

The control task owns MIDI/CV arbitration. MIDI notes take priority while held;
release, all-notes-off (CC120/123), suspend, and disconnect restore CV at full
velocity. Every note-on retriggers from the current envelope level. Gate edges
are captured by interrupt and queued with timestamps, so short pulses retain
their duration after control pickup. Queue overflow clears held MIDI state,
mutes if necessary, and resumes CV. Engine changes preserve the amp/filter
envelopes and ramp from the previous output over 128 samples.

Terrain imports are limited to **128×128** by the embedded memory budget;
oversized imports return false before touching the existing asset. Asset loading
APIs belong to initialization or the audio owner; runtime SD/flash loading still
needs an explicit asset handoff. DSP safety checks use strict floating-point
semantics. Invalid PCM samples become silence, and I2S underruns clear buffers
instead of replaying old audio. `audio_task_stats()` exposes render maxima,
missed deadlines, write failures, and DMA queue overflows; control logs them at
debug level once per second.

## Status / roadmap

- [x] Six GLIESE engines ported and verified on the native build
- [x] Full-voice wiring: engine → dual filter + filter ADSR → amp ADSR
- [x] ESP-IDF firmware skeleton: I2S audio task, control task, USB MIDI
- [x] Design audit software fixes and native regression suite ([details](docs/AUDIT_FIXES.md))
- [x] ESP32-S3 cross-build with pinned components and CI build checks
- [ ] Build + profile on real ESP32-S3 hardware (per-engine CPU headroom)
- [ ] External ADC hardware integration and measured V/Oct accuracy
- [ ] Granular sample / heightmap loading from SD or flash
- [ ] Panel/HP design, CV attenuverters for timbre/morph

## License

The DSP engines derive from GLIESE_710. The port uses a local compatibility shim
instead of the JUCE runtime. Upstream revision and license notices still need to
be recorded; the runtime dependency change does not settle source provenance.
