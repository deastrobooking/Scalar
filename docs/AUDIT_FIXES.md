# Design audit implementation follow-up

Implemented 2026-11-07 after the [baseline design audit](DESIGN_AUDIT.md). The
software findings A01–A14 have implementation changes and regression/build
coverage. Hardware validation and the broader product-design questions listed
below remain open.

| Finding | Change | Evidence |
|---|---|---|
| A01 USB descriptors | Explicit MIDI configuration/strings, per-device serial, pinned components, optional USB startup failure | ESP32-S3 cross-build with ESP-IDF 5.2.3 |
| A02 parameter races | Three slots with atomic ownership exchange and a separate producer staging value | Two-publication regression; 100,000 concurrent snapshot publications |
| A03 MIDI/CV ownership | One control-task-owned router, channel 1, last-note priority, coherent event payloads, explicit CV velocity restoration | MIDI priority, held-note fallback, bend, channel filtering, all-notes-off and disconnect tests |
| A04 lost note events | Interrupt-captured gate edges, USB receive callbacks, input/event queues, timestamps scheduled on the audio sample clock | Rapid on/off/on ordering, concurrent FIFO wrap, 1 ms captured pulse lasting 48 frames, overflow recovery tests |
| A05 pitch quantization | Exact-frequency note-on overload; independent note identity and pitch; common voice pitch range starts at 1 Hz | 432 Hz stays 432 Hz; low CV remains below 20 Hz |
| A06 held-note engine changes | Reset/start only incoming engine, preserve amp/filter envelope state, 128-sample output transition from the previous sample | Held notes stay active/audible through all engine selections; bounded initial output step |
| A07 FM double advance | Voice no longer calls a second envelope advance; skip-only APIs documented | FM envelope matches a standalone envelope at the same sample count |
| A08 ADSR timing/curve | Stage-specific exponential targets, exact stage lengths, implemented linear curve, unchanged params do not restart stages | Attack/decay/release at 44.1/48/96 kHz, both curves, zero-time stages |
| A09 terrain memory | Reject imports above 128×128 before mutation; remove duplicate level-zero planes; preflight build budget | Oversized import leaves existing terrain intact; maximum import renders; native allocation probe |
| A10 calibrated pitch | Factory ADC calibration, front-end offset/gain, retain valid pitch on read failure, external calibrated-volts queue API | Offset/gain and invalid-input tests; ADC driver compiles against actual SDK |
| A11 WT controls | Full-bank timbre scan and enabled phase-bend morph; 5 ms ramps | Actual panel mapping changes rendered output for both knobs |
| A12 button bounce | Stable-state 20 ms debounce; periodic control scheduling | Bounce, held button, and second press tests |
| A13 finite recovery | Strict DSP floating-point flags, bit-level PCM guard, reset poisoned engine/filter state | NaN/Inf silence checks, separate fast-math boundary translation unit, UBSan checks |
| A14 audio recovery | Prepare DSP before playback, preload silence, automatic DMA clear, checked task creation/writes, transport retry and telemetry | ESP32-S3 cross-build; runtime error injection still needs hardware |

The target compiler also caught an existing potentially uninitialized filter
routing output. Unknown routing now falls back to initialized bypass output,
covered by a regression test. Granular body coefficients now invalidate on pitch
changes and relevant body parameters, with a rendered comparison test. Wavetable
mips include a one-harmonic top band and account for detune plus several phase
warp bandwidth multipliers. Nonlinear spectral quality still requires measurement.

## Control and failure contracts

- Only the control task publishes parameter snapshots or writes musical events.
  USB and gate callbacks post to the FreeRTOS input queue. Only audio consumes
  snapshots/events. These are single-producer/single-consumer ownership rules.
- A new MIDI note retriggers from the current envelope level. Releasing the
  newest held note returns to the previous held MIDI note; releasing the last
  resumes the tracked CV state at velocity 1. CC120/123, USB suspend, and unplug
  clear held MIDI notes. The current firmware listens on MIDI channel 1.
- Timestamps preserve relative note/gate durations when edges arrive in one
  control pickup. Delivery is quantized to audio samples. This does not establish
  measured absolute gate-to-DAC latency, interrupt latency, or jitter bounds.
- Audio processes at most one queued event per sample. Event overflow suspends
  the producer, asks audio to mute and flush, then acknowledges the control
  owner's reset before resuming CV. Input queue overflow also clears MIDI state
  and reconciles the physical gate. Sustained overload therefore has an explicit
  silence/recovery behavior rather than silently dropping a release event.
- Engine changes preserve the amp/filter envelope and existing filter state.
  The output transitions from its last value over 128 samples; two full engines
  are not rendered simultaneously. This needs listening/transient testing on the
  actual output stage.
- I2S uses four 64-frame stereo buffers at 48 kHz. Write errors/short writes cause
  a silence-prefilled transport restart while retaining musical state. Maximum
  render duration, deadline misses, write failures, and DMA queue overflows are
  exposed through `audio_task_stats()` and debug logging on the control task.
- Heavy factory wavetable/source generation invokes an optional cooperative
  startup hook, supplied by firmware as a one-tick delay, before DMA starts.
  This lets idle tasks service the watchdog during preparation. The hook is
  never called from rendering; that separation has a regression test.

## Pitch and board setup

The default front end now specifies ±5 V at the jack mapped to **0–3.0 V** at
ADC1_CH0. ESP-IDF factory calibration converts raw readings to millivolts. The
second calibration converts those readings to jack volts:

```text
inputVolts = (calibratedAdcMv - zeroMv) / millivoltsPerInputVolt
frequencyHz = 130.81278 * 2^inputVolts
```

Measure the calibrated reading at 0 V input and at +1 V input. Set `kPitchZeroMv`
to the first and `kPitchMvPerVolt` to their difference in `board.h`, or call
`cv::setPitchCalibration()` at runtime. These values are not persisted across
reboots by that API. Internal pitch smoothing reaches 90% in roughly four
control updates; pots update at 100 Hz with separate smoothing.

An external ADC driver can supply measured jack volts through
`cv::setPitchVolts()`. The control task then stops using internal pitch until
`cv::useInternalPitch()` is requested. Invalid readings are rejected; unavailable
factory calibration holds the last pitch instead of estimating from uncalibrated
raw codes. Internal ADC resolution/nonlinearity remain a physical limit. Existing
0–3.3 V front ends require measured calibration constants and range assessment;
software cannot characterize their upper range without hardware.

GPIO0 remains the devkit boot button, with software debounce. A production panel
still needs a suitable non-strapping input and schematic review. Other ESP32
variants explicitly require new board profiles; this change validates the
existing S3 profile and does not claim compatibility for untested chips.

## Memory and validation

The [post-fix probe](audit/probes-after.cpp) and [captured output](audit/results-after.txt)
record the following native ARM64 requested-allocation measurements:

| Asset | Baseline retained bytes | Repaired retained bytes |
|---|---:|---:|
| Wavetable factory bank | 1,311,104 | 1,352,040 |
| Default terrain and mipmaps | 918,888 | 525,672 |
| Shared granular source | 524,456 | 524,456 |
| Combined assets | 2,754,448 | 2,402,168 |

The wavetable bank adds its missing one-harmonic mip level; reserving its factory
capacity reduces its initialization allocation peak from 1,974,848 to 1,401,192
bytes. Terrain initialization peaks at 657,056 bytes, and 512×512 imports are
rejected without allocation. Native C++ metadata differs from ESP32; these are
not runtime heap measurements on a board.

Validation performed locally:

- Native Release CTest: smoke rendering, design regressions, fast-math PCM boundary.
- Debug CTest with undefined-behavior and float-cast-overflow sanitizers.
- Per-engine post-initialization render probe: zero C++ allocations in measured loops.
- 837-case filter sweep: no nonfinite output.
- ESP32-S3 firmware build and partition-size check with ESP-IDF 5.2.3, Xtensa
  GCC 13.2.0, esp_tinyusb 1.7.6~2, and TinyUSB 0.21.0~4. The dependency lock is
  checked in. SDK Python package versions used here are in
  `firmware/requirements-build.txt`.

GitHub Actions configuration adds native Release/sanitizer checks and the S3
cross-build/size check. The remote workflow has not been run by this session.
Local AddressSanitizer and ThreadSanitizer were unavailable during the baseline
audit due to runtime initialization/crash problems; native concurrency stress and
UBSan do not replace a successful race-detector run.

Run the current tests with:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

To rerun the allocation/filter diagnostic, link `docs/audit/probes-after.cpp`
against the built `scalar_dsp` library using the include paths from the baseline
audit. Its standalone WarpMode::Off comparison intentionally remains unchanged;
`tests/regression.cpp` tests the repaired panel mapping with an enabled warp.

## Remaining release evidence

No connected hardware was available. USB enumeration/disconnect behavior, injected
I2S failures, measured worst-case CPU/deadline headroom, runtime heap/stack margins,
pitch accuracy/range, gate-to-audio latency, aliasing, and analog transients still
need bench validation. Schematics/BOM, production input/output circuitry, USB
self-powered/VBUS behavior, runtime SD/flash asset ownership, and upstream source
revision/license notices remain product/release work. Their absence is not
resolved by a successful software build.
