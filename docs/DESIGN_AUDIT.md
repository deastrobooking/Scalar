# Scalar design audit — ESP32 family

Reviewed 2026-11-07 against commit `57cb4f3`. Production sources were left unchanged.

**Historical baseline:** the software changes following this audit are tracked in
[AUDIT_FIXES.md](AUDIT_FIXES.md). The findings, measurements, and probes below
describe the original implementation. Use the current CTest regression suite to
verify the repaired behavior.

Scalar has a useful separation between portable DSP and hardware drivers, and all six engines produce finite, audible output in the native harness. The current firmware is an ESP32-S3 prototype. It is not yet ready for a playable hardware release or a claim of ESP32-family compatibility: USB initialization, control ownership, event delivery, pitch handling, and engine switching have blocking defects.

This audit covers the repository's DSP, JUCE shim, voice wiring, firmware tasks, build configuration, and documented hardware interface. There are no schematics, PCB files, or BOM in this checkout. ESP-IDF, an ESP toolchain, and connected hardware were unavailable, so target compilation, electrical behavior, and real-time performance remain unverified.

## Prioritized findings

P1 means fix before a playable firmware release. P2 means correct before the affected feature is shipped or advertised. Findings distinguish native reproductions, source evidence, and hardware-dependent risks.

### A01 — P1: USB MIDI lacks a configuration descriptor

**Location:** `firmware/main/usb_midi.cpp:84`, `firmware/main/main.cpp:14`, `firmware/main/idf_component.yml:3`.

The zero-initialized `tinyusb_config_t` does not expose MIDI automatically. I inspected the official esp_tinyusb 1.4.2 and 1.7.6~2 packages, both permitted by `^1.4`. Their default configuration descriptors omit MIDI. In 1.7.6~2, `descriptors_control.c:180` explicitly returns `ESP_ERR_INVALID_ARG` when MIDI is enabled and the configuration descriptor is null. `ESP_ERROR_CHECK` then aborts startup before `audio_task_start()` executes. This is a source-confirmed startup blocker; it was not tested on a device.

**Fix:** supply static MIDI configuration and string descriptors, select an explicitly tested dependency/SDK combination, and handle optional USB failure without preventing CV-driven audio startup. Espressif's [ESP-IDF 5.2.3 MIDI example](https://github.com/espressif/esp-idf/blob/v5.2.3/examples/peripherals/usb/device/tusb_midi/main/tusb_midi_main.c) supplies these descriptors explicitly. The inspected [1.7.6~2 package](https://components-file.espressif.com/components/espressif/esp_tinyusb/1.7.6~2/espressif__esp_tinyusb-v1.7.6_2.zip) contains the rejection path.

### A02 — P1: Parameter publication does not protect the reader

**Location:** `firmware/main/param_bridge.h:22` and `:30`.

Release/acquire makes the preceding write visible, but it does not reserve a buffer for the reader. After publication, the producer starts editing the other buffer without knowing whether audio is still copying it from a previous publication. Concurrent ordinary reads and writes of `VoiceParams` constitute a C++ data race. The published value is also just a toggling index, so two publications between consumes look like no update.

**Reproduction:** publish level 0.1 and consume it; publish 0.2, then 0.3; the next consume returns false and leaves the output at 0.1. This test needs no threads.

**Fix:** use a bounded handoff with explicit ownership, such as a FreeRTOS queue with a length-one parameter snapshot and nonblocking audio receive, or a correctly implemented triple buffer. An incrementing sequence alone does not fix concurrent payload access. Stress the replacement with producer bursts and reader preemption.

### A03 — P1: CV polling defeats MIDI priority and produces incoherent note state

**Location:** `firmware/main/control_task.cpp:126`, `firmware/main/usb_midi.cpp:24`, `:39`.

The control task overwrites pitch and gate from the panel every tick, even while a MIDI note is active. MIDI writes the same atomics only when packets arrive. With the panel gate low, a MIDI note is canceled on the next control iteration; its pitch is also replaced by CV. MIDI velocity persists when control returns to CV. Independent relaxed atomics additionally allow audio to observe a new gate with pitch/velocity from another update.

**Fix:** let one task own the musical state. Feed MIDI events into it, implement the documented MIDI-over-CV priority, restore CV velocity deliberately, and publish pitch/gate/velocity as one coherent state or ordered event stream.

### A04 — P1: A gate boolean loses triggers and repeated notes

**Location:** `firmware/main/audio_task.cpp:82`, `firmware/main/control_task.cpp:126`, `firmware/main/usb_midi.cpp:46`.

Audio sees only the gate level at each 64-frame boundary. Note-off followed by note-on within 1.333 ms can collapse into a continuously high gate, so the next note never retriggers. An entire short MIDI note can disappear. Polling the physical gate at approximately 1 kHz can also miss short input pulses. A second note-on while high changes pitch but does not deliver its velocity or a note-on to the engines; no explicit legato policy exists.

**Fix:** deliver bounded, ordered note/trigger events independently of continuous parameters. Capture physical gate edges with an appropriate interrupt/capture path. Define repeated-note, legato, note priority, disconnect, all-notes-off, and queue-overflow behavior. Sample timestamps are useful if tighter timing is required.

### A05 — P1: Note-on replaces continuous pitch with a tempered MIDI note

**Location:** `firmware/main/audio_task.cpp:75`, `:87`, `dsp/scalar/ScalarVoice.cpp:93`.

Audio first sets the exact frequency, then derives an integer note and calls `noteOn()`, which recalculates frequency from that integer. `lastPitch` already records the exact input, so an unchanged input does not restore it on the next block. This affects detuned CV and pitch-bent MIDI note starts.

**Reproduction:** use the firmware call order with 432 Hz and a sine voice. Output measures 440 Hz. When noisy ADC readings move again, the error may be temporary, which can conceal the defect during manual testing.

**Fix:** separate note identity/key scaling from oscillator frequency, or explicitly preserve/reapply exact pitch after the note-on operation. Specify common pitch limits: VA/WT/3D currently clamp to 20 Hz, while the documented CV range extends down to approximately 4.09 Hz.

### A06 — P1: Changing engines silences a held gate

**Location:** `dsp/scalar/ScalarVoice.cpp:73`, `firmware/main/audio_task.cpp:83`.

An engine change resets every engine, filter, and the amp envelope. The gate detector retains `lastGate=true`, so it does not retrigger the new engine. The voice remains silent until the gate falls and rises again. Reset also clears four comb buffers, adding unnecessary work to the audio block.

**Reproduction:** hold a VA note, select wavetable, and continue rendering. Peak output drops from approximately 0.638 to exactly zero; `isActive()` becomes false.

**Fix:** define a musical transition that preserves the gate/envelope state or retriggers deliberately. Apply a short fade/crossfade to avoid discontinuities, and reset only the state required by the transition.

### A07 — P2: FM envelopes run at twice the audio rate

**Location:** `dsp/gliese/Oscillators/FMOperator.cpp:152`, `dsp/gliese/Oscillators/FMAlgorithm.cpp:229`, `dsp/scalar/ScalarVoice.cpp:135`.

`FMOperator::processSample()` advances its envelope. The voice then calls `advanceEnvelopes()`, advancing all ten envelopes again. FM timings therefore differ from the amp/filter envelope and from the programmed durations.

**Reproduction:** with a one-second attack, the shared ADSR takes 7,642 samples to finish attack; the FM render path takes 3,821 audio samples.

**Fix:** establish one envelope advance per audio sample, then verify attack, decay, sustain, release, and retrigger behavior independently of oscillator output.

### A08 — P2: ADSR attack duration uses the wrong target ratio

**Location:** `dsp/gliese/Envelopes/ADSREnvelope.cpp:6`, `:15`, `:62`, and `ADSREnvelope.h:16`.

Attack uses an overshoot target of 1.3, but its coefficient is calculated using the decay/release ratio 0.0001. The resulting attack lasts roughly 15.9% of the selected duration. `Params::curve` is also never read, so selecting Linear produces the exponential behavior.

**Reproduction:** a one-second attack at 48 kHz finishes in 159.208 ms. The FM double-advance defect reduces this further to 79.604 ms.

**Fix:** derive each stage's coefficient from its actual target and time definition; implement the curve selection or remove that unsupported setting. Check measured stage durations at multiple sample rates, including zero-time stages.

### A09 — P2: Terrain memory limits exceed the documented board

**Location:** `dsp/gliese/Oscillators/TerrainOsc.h:69`, `TerrainOsc.cpp:170`, `:238`, `:267`.

The accepted 512×512 heightmap allocates the raw terrain and five full-size blurred versions, then duplicates that base data in mip level zero and builds smaller mip levels. Temporary arrays increase the peak further. Native allocation instrumentation measured **14,682,664 bytes retained** and **16,780,832 bytes peak**, excluding the caller's 1 MiB input grid and every other engine. The N8R8 baseline cannot accommodate this. The current firmware does not yet expose heightmap loading, so this is a blocker for the loading feature rather than the stock patch.

**Fix:** choose an explicit memory budget and constrain dimensions accordingly; eliminate duplicate base storage, limit blur variants, and preflight both peak and retained allocation requirements. With exceptions disabled, ordinary `std::vector` allocation failure does not provide a recoverable false return from `setHeightmap()`.

### A10 — P2: V/Oct conversion assumes an uncalibrated 3.3 V full scale

**Location:** `firmware/main/control_task.cpp:33`, `:42`, `:130`, `firmware/main/board.h:18`.

Raw ADC codes are mapped directly to ten input volts without calibration. The intended 0–3.3 V front end also extends beyond the S3's documented approximate 3.1 V measurement range at this attenuation. Even an ideal 12-bit conversion over ten octaves gives approximately **2.93 cents per code**, before noise and nonlinearity. Ignoring ADC read errors turns an invalid conversion into a pitch excursion. The documented `cv::setPitchVolts()` integration point does not exist.

**Fix:** specify the pitch accuracy and latency requirements, design the analog range around the selected ADC, calibrate offset/gain, retain the last valid value on conversion failure, and provide a real calibrated pitch input interface. The smoothing coefficient 0.15 takes roughly 14 control updates to reach 90% of a pitch step; account for that glide explicitly. See Espressif's [S3 ADC range](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32s3/api-reference/peripherals/adc.html) and [oneshot conversion/error behavior](https://docs.espressif.com/projects/esp-idf/en/v5.2/esp32s3/api-reference/peripherals/adc_oneshot.html).

### A11 — P2: Both panel macros are ineffective in the stock wavetable engine

**Location:** `firmware/main/control_task.cpp:88`, `dsp/gliese/Oscillators/WavetableOsc.h:41`, `WavetableOsc.cpp:434`.

Timbre has no implementation, and morph changes only `warpAmount` while `warpMode` stays Off. Neither table scan nor a warp mode is selected elsewhere in firmware. The instrument remains on its first sine table regardless of those two controls.

**Reproduction:** render the default wavetable oscillator after reset at warp amounts zero and one. The maximum sample difference is exactly zero.

**Fix:** map timbre to a defined table-scan range and choose a meaningful morph/warp behavior, with smoothing. Test the actual panel mapping as well as direct engine APIs.

### A12 — P2: Engine selection has no debounce

**Location:** `firmware/main/control_task.cpp:136`, `firmware/main/board.h:28`.

Every falling edge advances the engine. Contact bounce can advance it several times, repeatedly causing A06. GPIO0 is also the devkit boot button; the production panel must account for its boot-strapping role.

**Fix:** debounce against elapsed time or a stable-state interval, and move the final panel button to a suitable board-specific input. Use elapsed-time scheduling for controls if a specified update period matters; `vTaskDelay(1)` adds work time to the loop period.

### A13 — P2: Firmware optimization contradicts finite-value safety guards

**Location:** `firmware/components/scalar_dsp/CMakeLists.txt:23`, `dsp/gliese/Filter/FilterModels.h:48`, `dsp/gliese/Oscillators/GranularOsc.cpp:23`, `firmware/main/audio_task.cpp:48`.

The firmware compiles the DSP with `-ffast-math`, which permits assuming floating-point inputs/results are finite. Its `std::isfinite` recovery branches cannot be relied on under that contract. A separate native translation unit compiled with this flag returned **8** for a NaN through `FilterModelDetail::clampAudio()`, whose source promises zero. This is a compiler-semantics reproduction, not a prediction of the exact Xtensa output. The final float-to-PCM conversion has no explicit nonfinite handling either.

**Fix:** retain finite checks in code compiled with suitable floating-point semantics, or use a verified bit-level boundary check. Define recovery of poisoned filter state and a silence fallback before PCM conversion. Test the actual target compiler flags. GCC documents the [finite-only assumptions enabled by fast math](https://gcc.gnu.org/onlinedocs/gcc/Optimize-Options.html).

### A14 — P2: Audio underruns and initialization failures have no recovery policy

**Location:** `firmware/main/audio_task.cpp:26`, `:56`, `:104`, `:113`, `firmware/main/control_task.cpp:172`, `firmware/main/usb_midi.cpp:86`.

I2S is enabled before expensive terrain, wavetable FFT, and granular initialization. The render loop ignores the write result and byte count; task creation results are also ignored. The IDF 5.2 default leaves TX `auto_clear=false`, so exhausted DMA buffers can replay stale audio when rendering misses deadlines. There is no deadline/underrun counter or controlled silence/restart path. These are source-confirmed omissions; no underrun rate was measured.

**Fix:** prepare DSP before starting playback, preload silence, check resource creation and writes, configure the intended underrun behavior, and collect timing/failure counters outside the audio logging path. See the [IDF 5.2.3 I2S defaults and API](https://github.com/espressif/esp-idf/blob/v5.2.3/components/driver/i2s/include/driver/i2s_common.h).

## ESP32 target assessment

The current pin map, octal PSRAM configuration, USB initialization, and core-1 affinity are S3-specific. Define supported board profiles and compile-time capability checks before advertising broader compatibility.

| Target | Assessment of this implementation |
|---|---|
| ESP32-S3 with 8 MiB octal PSRAM | Closest match to the existing implementation. Fix the findings and prove memory/timing on the exact module. |
| Original ESP32 with PSRAM | Requires a different MIDI transport or external USB controller, pin map, and PSRAM configuration; native USB MIDI is unavailable. |
| ESP32-S2 | USB device capability exists, but core-1 affinity is invalid on its single application CPU; scheduling and DSP capacity need separate validation. |
| ESP32-C3 / C6 | Their USB Serial/JTAG controller cannot be repurposed into USB MIDI. Single application CPU, memory, and DSP constraints require a substantially different profile. |
| ESP32-P4 / S31 | Separate porting candidates with different hardware/SDK assumptions. No inference of compatibility follows from having USB and multiple cores. |

These distinctions follow Espressif's [chip comparison](https://docs.espressif.com/projects/esp-techpedia/en/latest/esp-friends/get-started/board-selection.html) and [USB capability matrix](https://docs.espressif.com/projects/esp-iot-solution/en/latest/usb/usb_overview/usb_overview.html). This is a shortlist, not an exhaustive compatibility matrix.

### Memory and real-time budgets

All six engines are prepared unconditionally (`ScalarVoice.cpp:25`), regardless of the selected engine. PSRAM is therefore required by the current default voice even when granular is never selected.

| Native measurement | Retained allocation / size |
|---|---:|
| Wavetable factory bank, vector capacity included | 1,311,104 bytes |
| Default 128×128 terrain, blur variants and mipmaps | 918,888 bytes |
| Generated granular source, shared by both layers | 524,456 bytes |
| Combined engine heap above | 2,754,448 bytes, about 2.63 MiB |
| `ScalarVoice` object | 78,216 bytes |
| Four comb delay buffers within that object | 65,536 bytes |
| Explicit audio/control/MIDI task stacks | 28,672 bytes, excluding TinyUSB/IDF tasks |

These are native ARM64 C++ allocation/size measurements, not an ESP linker map. Pointer/string metadata sizes differ on ESP32. Granular's default source is retained by a static shared pointer; in the probe, the later whole-voice measurement excludes that already-created 524,456-byte allocation. Large heap allocations can use PSRAM with the supplied malloc settings; the static voice object does not move there merely because malloc can. Keep DMA descriptors, audio scratch, and hot state deliberately placed, and inspect internal heap, largest free block, and stack high-water marks. See [Espressif's memory placement rules](https://docs.espressif.com/projects/esp-idf/en/v5.2/esp32s3/api-guides/external-ram.html).

At 48 kHz, a 64-frame block allows **1.333 ms** average production time. At 240 MHz that is **320,000 cycles per block**, or **5,000 cycles per stereo frame**. Four configured DMA blocks cover about **5.333 ms** of audio; the 1.333 ms block size is not an end-to-end latency claim. Set a measured headroom requirement; an initial engineering target is worst-case render plus parameter/event handling below approximately 0.93 ms, with zero underruns under the intended workload.

Profiles must include knob movement, pitch updates, engine transitions, USB traffic, maximum unison/grain/body-mode settings, and expensive filters. Likely optimization targets are repeated per-sample trig/power calculations, terrain's rotation/mip calculations, wavetable detune/pan calculations, all-engine parameter updates, and granular's duplicate layer reads when both sources are identical. Move invariant work to parameter/block updates and precompute factory data where practical. The S3 accelerates single precision; double arithmetic is software-emulated. Follow [Espressif's measurement guidance](https://docs.espressif.com/projects/esp-idf/en/v5.2/esp32s3/api-guides/performance/speed.html) before choosing optimizations or enabling engine combinations.

### Additional design limits and unverified hardware

- **Spectral quality:** wavetable mip selection uses the base frequency without detune/warp bandwidth, and its last level still retains two harmonics. Above 12 kHz at 48 kHz sampling, even that level is not universally bandlimited. Nonlinear VA/FM/terrain paths need aliasing measurements. Filter oversampling repeats substeps and takes the final result; 3D uses a short box average. Neither establishes adequate stopband rejection by itself. Define aliasing targets and compare spectra before calling these paths alias-free.
- **Granular resonator pitch cache:** `GranularOsc::setFrequency()` does not mark `bodyDirty`, although body coefficients depend on frequency. A later `setParams()` currently refreshes it at panel publication rate; direct continuous-frequency use can leave the resonator on its old pitch. Dirty the cache when relevant inputs change, without rebuilding it for every unrelated parameter snapshot.
- **Future sample loading:** terrain/wavetable/sample setters mutate data used by rendering; direct control-thread calls are not safe handoff mechanisms. Build immutable replacement assets off the audio task, switch ownership at an audio boundary, and reclaim old storage off that task. Account for simultaneous old/new asset memory.
- **Panel feature scope:** firmware fixes the filter to LP24/AOnly and exposes no envelope programming or deep FM controls. The engine library's 31 filters and other modes are not currently selectable instrument features. Make the intended product controls and fallback patches explicit.
- **Electrical design:** the documented GPIO/audio arrangement is a baseline, not a verified Eurorack interface. Input protection, ADC drive/filtering/calibration, gate thresholds/hysteresis, output gain/impedance/protection, DAC mute sequencing, USB self-powered/VBUS behavior, power rails, grounding, and decoupling need schematics and measurements. The PCM5102A supports the chosen 48 kHz/16-bit 3-wire format; correct SCK/FMT/XSMT strapping and analog implementation still matter. See the [TI datasheet](https://www.ti.com/lit/ds/symlink/pcm5102a.pdf).
- **Build and provenance:** pin a tested ESP-IDF/component combination, add target-build and binary-size checks, and record upstream revision/license notices. This checkout has no LICENSE/COPYING/NOTICE file. Removing JUCE runtime dependencies does not establish the provenance of the copied GLIESE sources.

## Verification and reproducibility

The review added only this report and diagnostic evidence under `docs/audit/`; it did not repair the production defects. The probes report current behavior, including known failures, and are not a passing regression suite.

| Check | Result |
|---|---|
| Clean native Release build and existing CTest smoke | Passed, six engines |
| Native build with `-ffast-math` and existing smoke | Passed; this does not validate finite guards |
| Debug build with undefined-behavior and float-cast-overflow sanitizers | Existing smoke and focused probes passed without sanitizer diagnostics |
| Targeted behavioral probes | Reproduced A02, A05, A06, A07, A08, A09, A11 |
| One second rendered per default engine after initialization | Zero C++ `new` calls in the measured render loops |
| Filter sweep: 31 modes × 3 cutoffs × 3 resonances × 3 oversampling settings, 2,048 samples each | 837 cases; no nonfinite output, maximum peak 1.5 |
| AddressSanitizer | Could not run: local runtime aborted during initialization in `sanitizer_malloc_mac.inc` |
| ThreadSanitizer | Local instrumented probe exited with SIGSEGV and no report; no usable race-detector result |
| Firmware compile, USB enumeration, pitch accuracy, audio timing, electrical tests | Not performed; tooling/hardware unavailable |

Native checks used Apple clang 15.0.0 on ARM64. The filter sweep tests numerical finiteness, not transfer functions, aliasing, or perceptual quality. The memory tracker counts C++ allocations and requested bytes, excluding allocator overhead and direct C allocations.

Run the preserved probes from the repository root:

```sh
cmake -S . -B /tmp/scalar-audit-build -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/scalar-audit-build --parallel
ctest --test-dir /tmp/scalar-audit-build --output-on-failure
c++ -std=c++20 -O2 -I dsp/shim -I dsp -I firmware/main \
    docs/audit/probes.cpp /tmp/scalar-audit-build/libscalar_dsp.a \
    -o /tmp/scalar-design-probes
/tmp/scalar-design-probes

c++ -std=c++20 -O2 -ffast-math -I dsp -I dsp/shim \
    -c docs/audit/finite.cpp -o /tmp/scalar-finite.o
c++ -std=c++20 -O2 docs/audit/finite-main.cpp /tmp/scalar-finite.o \
    -o /tmp/scalar-finite-probe
/tmp/scalar-finite-probe
```

The [captured output](audit/results.txt) accompanies the [probe source](audit/probes.cpp). The finite-guard probe printed `guard_nan finite=1 value=8` on this host.

## Recommended implementation order and release evidence

1. Fix USB configuration and establish a reproducible S3 firmware build. Capture linker size, internal RAM/PSRAM placement, and clean startup on the exact module.
2. Replace parameter publication, centralize MIDI/CV ownership, add note-event delivery, preserve continuous pitch, and make engine transitions musical. Add regression tests for these behaviors.
3. Correct envelope timing, panel mappings, debounce, ADC error handling, and finite-value recovery. Calibrate pitch against a defined accuracy/range specification.
4. Enforce asset/engine memory limits, then profile every exposed engine/filter/quality combination on hardware. Restrict settings that cannot meet the measured budget.
5. Validate clock/data framing, gate-to-audio latency and jitter, repeated notes, USB disconnect recovery, pitch tracking, spectra, clipping, and startup/shutdown transients. Run a sustained worst-case workload with zero underruns and documented memory/stack margins.
6. Review the analog schematic/PCB and production controls. Add other ESP32 board profiles only after each passes its own compile, memory, timing, and hardware validation.
