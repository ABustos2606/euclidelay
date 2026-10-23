# Euclidelay implementation plan

**Written:** 23 October 2026. **Follows:** [design.md](design.md), the spec. Where this plan and the spec disagree, the spec wins and this plan is fixed. **Checked against:** Peepo's `src/core/euclidean.hpp` (the patterns), `docs/29-parameters-files-tempo.md` (patch tempo), `config/catalogue/overlay.json` (`"bypass": "port"`, `"tempo"`); Sway's plan and Discipline's implementation plan, whose Ingen findings apply unchanged.

This plan says how to build Euclidelay: code layout, the engine, real-time rules, tests, and the milestones in order. Tick a step only when it has been checked the way it says. Commits and tags are the user's; "tag" below means "ready for the user to tag".

## 1. What Ingen's source changes

From Discipline's plan (section 1), unchanged:

1. **Engine bypass does not run the plugin.** It copies audio input *n* to output *n*: with one input and two outputs, a block-bypassed Euclidelay would play the dry sound on the left only and cut every echo. Peepo must use the `enabled` port (`"bypass": "port"` in the overlay).
2. **`run()` is called in slices.** Ingen splits a period at every control change. Nothing assumes one `run()` per period or a fixed `n_samples`.
3. **Blocks run on several threads.** Instances share nothing.
4. **Block length.** Not needed: Euclidelay has no latency and no scratch buffer sized by the period.

## 2. Technology and build

- **Plain LV2 C API in C++17**, no DPF. One shared object, `euclidelay.so`, in `euclidelay.lv2/` beside `manifest.ttl` and `euclidelay.ttl`.
- **License:** GPL-2.0-or-later. **Dependencies:** `lv2-dev` headers only; the tests need `python3-rdflib`.
- **Makefile**, no configure step, no network, as in Sway and Discipline: `make`, `make install` (`DESTDIR`, `PREFIX`, `LV2DIR`), `make test`, `make lint` (lv2lint if on the path), `OPT` (default `-O2`; the Pi build passes `OPT="-O2 -mcpu=cortex-a76"`) and `SAN=1` (address and undefined-behaviour sanitizers for the tests).
- **Flags:** `-std=c++17 -fPIC -fvisibility=hidden -Wall -Wextra -fno-math-errno -fno-trapping-math`, then `-fno-fast-math` after `OPT`: the non-finite guard needs IEEE arithmetic. Only `lv2_descriptor` is exported (`src/exports.map`).

### Repository layout

```
Makefile
LICENSE                      GPL-2.0
README.md
euclidelay.lv2/
    manifest.ttl
    euclidelay.ttl
src/
    exports.map
    ports.hpp                port indices, ranges and defaults; the one source for the .ttl check
    pattern.hpp              Euclidean pattern -> list of hit steps (ours, tested against Peepo's)
    voice.hpp/.cpp           one voice: loop buffer, taps, feedback, tone, crossfade
    engine.hpp/.cpp          four voices, step time, smoothing, pan, mix, clip, on/off
    lv2_entry.cpp            descriptor, instantiate, connect, activate, run, cleanup
tools/
    gen_patterns.sh          writes tests/patterns_peepo.inc from Peepo's euclidean.hpp (run by hand)
tests/
    ttl_check.py
    patterns_peepo.inc       every (steps, hits, shift) up to 16, as Peepo draws them (generated data)
    test_pattern.cpp
    test_voice.cpp
    test_engine.cpp
    test_changes.cpp
    test_voices.cpp          switching voices on and off
    test_lv2.cpp             dlopen the .so, run it sliced as Ingen does
    test_timing.cpp          10 simulated minutes of random controls
docs/
    design.md, implementation-plan.md
```

**Why a generated table.** Peepo's `euclidean.hpp` is GPL-3.0, so its code cannot be copied into a GPL-2.0-or-later plugin. `pattern.hpp` is written from the spec's three rules. `tools/gen_patterns.sh` compiles a small program against Peepo's header (read from `C:\Pedal PI`, never copied) under WSL, prints all 16 x 17 x 16 patterns, and writes `tests/patterns_peepo.inc`. The table is data and is committed; the program is not. `test_pattern` then checks every entry.

## 3. The engine

### 3.1 Data

```
struct Taps {                // one voice's settings, in samples
    int   loop;              // L = round(N * S)
    int   count;             // number of hits, 0..16
    int   delay[16];         // D_k = round(k * S), ascending
    bool  zero;              // step 0 is a hit: subtract the input
};

class Voice {
    float* buf; int size; int write;      // loop buffer y, allocated once
    Taps  cur, next, pending;             // fade from cur to next; pending waits
    int   fade_left;                      // samples left in the fade, 0 = none
    bool  has_pending;
    float tone_z;                         // one-pole state in the feedback path
    Smoother fb, level, gl, gr;           // per sample, about 20 ms
    enum { Off, Starting, On, Stopping } state;
    float gate;                           // on/off fade, 0..1 over 20 ms
    int64_t fresh;                        // samples written since it came on
};
```

**Switching a voice.** `Off` skips the voice entirely: no reads, no writes. Going on: `fresh = 0`, `tone_z = 0`, the write position carries on, `cur = next = ` the current settings with no fade. While `fresh` is below a read's distance, that read returns 0. This stands in for clearing the buffer, which in `run()` would be a 6 MB `memset`. Once `fresh` reaches the buffer size it stops counting and every read is valid. Coming on, the voice's *input* fades in over 20 ms: its output has nothing to fade (the first echo is at least a step away), and a note already sounding must not enter the loop with a step, which every tap would repeat as a click. Going off, the voice's *output* fades out over 20 ms, so the fade is immediate in sound, then `Off`. A switch back on during that fade restarts as above, empty, so a quick off-on never brings back old echoes; but the old echoes are not cut. Everything older than the restart is the old life: taps reading further back than `fresh` (and within the old life's own `fresh`) carry on at the old fade's gain until it reaches 0, and the old life's step 0 keeps its own loop read and tone state for that time. A third switch during that fade keeps only the newest two lives. As implemented in milestone 2 (`src/voice.hpp`), the gains are integer ramps, so a fade lasts exactly its length in samples.

`size` = `ceil(32.03 * rate) + 1`. No power-of-two rounding: that would make 8.4 MB a voice instead of 6.2 MB. Wrapping is one compare and subtract.

### 3.2 Per sample, per voice

```
a      = fade_left ? 1 - fade_left / F : 1          // F = 30 ms in samples; linear, tune to equal power
back   = mix(y[w - cur.loop], y[w - next.loop], a)  // with no fade, just y[w - cur.loop]
y[w]   = x_in + fb * tone(back)                     // x_in: input times the on/off fade
v      = mix(sum_k y[w - cur.delay[k]], sum_k y[w - next.delay[k]], a)
         - (zero ? x_in : 0)                        // the zero term fades the same way
```

When the fade ends: `cur = next`; if a pending setting differs, `next = pending` and a new fade starts.

### 3.3 Order of work in a slice

1. Read the controls once, at the start of the slice. Turn Step and BPM into `S`, and each voice's Steps, Hits and Shift into a `Taps` through `pattern.hpp`. Compare with the voice's newest setting (`pending`, else `next`, else `cur`); if it changed, set a fade going or update `pending`.
2. Process the slice voice by voice, skipping voices that are `Off`. A voice's loop is at least 1200 samples (1/32 at 300 BPM, 48 kHz), longer than any Ingen period, so a whole slice of `y` can be written before the taps are read. Each tap then reads a contiguous run of the buffer. That keeps the memory reads in order (6 MB buffers don't fit in cache) and lets the compiler vectorise the tap sums. If a slice is ever longer than the shortest loop, split it at that length. Per-sample smoothing and fades still run in the per-sample loop that writes `y`, so the result does not depend on where slices start.
3. Pan, level, sum, clip, dry, write both outputs.

Worst case per sample, all four voices on: 4 x (2 loop reads + 32 taps in a fade) = 136 reads and one write per voice. At 48 kHz that is about 6.5 M reads a second, little for a Pi 5 core, but the cache behaviour has to be measured, not assumed (milestone 4). Cost scales with the voices that are on: one voice is a quarter of it.

### 3.4 Parameter rules

- Voice n is on at 0.5 or above.
- Hits clamped to Steps, Shift taken modulo Steps, Steps clamped to 1..16, Step to 0..6, BPM to 30..300, every continuous control to its range, before use. A non-finite control counts as its default.
- `S` is a double; `L` and `D_k` are rounded from `k * S` in double precision, so long loops at odd tempos land on the right sample.

## 4. Real-time rules, checked

- `instantiate` allocates the four buffers (`calloc`; null on failure, after freeing what was allocated). `activate` zeroes them and resets every smoother to its control's value. `run()` allocates nothing, locks nothing, prints nothing.
- Outputs are always finite: the non-finite guard of the spec, checked in `test_engine`.
- Denormals: the loops decay towards zero forever. Set flush-to-zero in `run()` (`FPCR.FZ` on arm64, MXCSR on x86, restored on exit), and also stop each voice's tone state from sinking into denormals.

## 5. Tests

### 5.1 Offline (`make test`)

| Test | Checks |
| --- | --- |
| `ttl_check` | every port in the `.ttl` matches `ports.hpp`: index, symbol, kind, range, default, enumeration values; names ASCII, at most 16 characters |
| `test_pattern` | every entry of `patterns_peepo.inc`; the spec's examples; Hits > Steps and Shift >= Steps |
| `test_voice` | an impulse into one voice gives impulses exactly at `D_k` (pass 0, step 0 absent) and `m * L + D_k` (pass `m`) at `fb^m`, within `m / 2 + 1` samples, for 7/4, 4/3, 1/1 and 16/16; Feedback 0 plays one pass; Tone 1 leaves every impulse intact, Tone 0 lowers the high band each pass; Hits 0 is silent |
| `test_engine` | pan law at -1, 0, 1; Dry and Echoes; four voices sum; Enabled off lets the loops ring out with the dry sound at unity, and back on; a held full-scale sine with all voices at 16 hits stays under 0 dBFS through the clipper; a NaN input clears the voice and the output stays finite; every one of the 16 on/off combinations of the four voices gives the sum of exactly the voices that are on; all off gives the dry sound only |
| `test_voices` | a voice switched off fades out within 20 ms and then reads and writes nothing (its buffer is untouched afterwards); switched on again after echoes were in its loop, it plays none of them, only what came in after; off and on again within the fade does the same; switching causes no jump above the `test_changes` bound |
| `test_changes` | each crossfaded control changed during a held sine: no sample-to-sample jump above a bound (**tune**, about -40 dB of the signal) at the moment of change; a knob dragged through 16 values costs at most two fades; a tempo change fades all four voices |
| `test_lv2` | dlopen the built `.so`; one descriptor with the URI; the same input run sliced at random points (pointers moved per slice, as Ingen does) and unsliced, with fixed controls, matches sample for sample |
| `test_timing` | 10 simulated minutes with random control changes every few blocks at 48 kHz and a 128-frame period: no non-finite output, and the worst block's time printed, against a budget of 25% of the period on the build machine |

### 5.2 Desktop hosts

`lv2lint` (clean, or only the warnings Sway has), `jalv`, Carla.

### 5.3 Ingen under WSL

Peepo's `tools/ingen-spike/env.sh`: load, play a recorded guitar file through it, change every control, save the patch, reload (every control comes back), change the patch tempo and hear it follow.

### 5.4 On the Pi

A typical patch (amp, Euclidelay, reverb) for 30 minutes with controls being moved: xruns, DSP load and the engine's resident memory before and after adding it, into Peepo's `docs/evidence/`.

## 6. Milestones

### Milestone 0: scaffolding (tag `v0.0.1`)

The repository builds a plugin that loads everywhere and passes the dry sound.

- [x] GitHub repository `https://github.com/ABustos2606/euclidelay` and its first commit (the user's).
- [x] `Makefile` with every target of section 2, `src/exports.map`.
- [x] `src/ports.hpp` and `euclidelay.lv2/euclidelay.ttl` with all 37 ports of the spec, their names, ranges, defaults and the Step enumeration; `manifest.ttl`; `doap:license`, `doap:maintainer`, `lv2:DelayPlugin`, `lv2:enabled` on port 3.
- [x] `lv2_entry.cpp`: descriptor, `instantiate` (allocates the four buffers), `activate`, `run` writing `Dry * in` to both outputs, `cleanup`.
- [x] `tests/ttl_check.py`; `test_lv2` (descriptor, URI, slicing with the dry-only plugin). `make test-hosts` (`tests/host_smoke.py`, as Discipline's): jalv, and Ingen with 1 and 4 threads on a private dummy JACK server, Dry on both outputs, every control saved and reloaded.
- [x] **Done when** `make`, `make test`, `make install DESTDIR=...` and `lv2lint` are clean under WSL, and Euclidelay loads in `jalv` and in WSL's Ingen. Checked 23 October 2026: lv2lint gives only the author e-mail warning Sway has.

### Milestone 1: patterns and one voice (tag `v0.0.2`)

One voice, fixed settings, exactly right.

- [x] `tools/gen_patterns.sh` and the generated `tests/patterns_peepo.inc` (all 4352 patterns, generated under WSL from Peepo's header).
- [x] `src/pattern.hpp` written from the spec, and `test_pattern` passing on every entry.
- [x] Step time from Step and BPM; `L` and `D_k` from section 3.4.
- [x] `Voice`: loop buffer, taps, step-0 subtraction, feedback, Tone in the feedback path. No fades or smoothing yet: settings are read at the start of each slice and take effect at once (a change can click until milestone 3).
- [x] Voice 1 wired into `run()`, panned with the spec's law and scaled by its Level and Echoes (no clipper yet); Voice 1 off silences it at once; voices 2 to 4 silent.
- [x] `test_voice` passing; `test_lv2` slicing still passing, now with echoes, plus an impulse through voice 1 checking its times, levels and pan. All also with `SAN=1`; `make test-hosts` passes (it listens with Echoes 0); lv2lint gives only the author e-mail warning.
- [ ] **Done when** an impulse in jalv gives the 7/4 pattern by ear and in a recording (Audacity or a test WAV), at 120 BPM, 1/16. An offline render (7/4, 120 BPM, 1/16) puts the echoes at steps 1, 3, 5, then 7, 8, 10, 12 at half level, as the spec says; listening in jalv is still to do.

### Milestone 2: four voices and the mix (tag `v0.0.3`)

The whole sound, with settings fixed while playing.

- [x] Four voices, each with its own controls, and the Voice n switches with their fades and the `fresh` count (section 3.1). `src/engine.hpp/.cpp` holds the four voices and the mix; `lv2_entry.cpp` only reads the ports into `Settings`.
- [x] `test_voices` passing: the worst second difference while switching every voice in every way on a held sine is 8.3e-5 against a bound of 5e-4 (-40 dB of the input), 2.2e-5 with no switching.
- [x] Level, Pan, Dry, Echoes; constant-power pan; the echo clipper (transparent to -6 dBFS, ceiling -0.2 dBFS, tanh between). Applied at the start of each slice, no smoothing yet (milestone 3).
- [x] Enabled: input fade into the loops (10 ms), trails, dry at unity while off (the same fade crossfades Dry and unity).
- [x] The non-finite guard: a non-finite input sample counts as silence and empties every voice; a voice whose loop or output overflows empties itself. Flush-to-zero (`FPCR.FZ` on arm64, MXCSR FTZ and DAZ on x86) for the length of `run()`, and the tone state flushed below 1e-20.
- [x] `test_engine` passing. All tests also with `SAN=1`; `test_lv2` now slices all four voices with every global control moved; `make test-hosts` passes; lv2lint gives only the author e-mail warning.
- [ ] **Done when** the default patch played with a guitar file in jalv or Carla sounds like the spec: two voices left and right, voices 3 and 4 come in when switched on, any combination of voices can be chosen while playing without clicks, and bypass by the Enabled port leaves trails.

### Milestone 3: changes while playing (tag `v0.0.4`)

Every control can move during a note without a click.

- [ ] Per-sample smoothers for Level, Pan (on the two gains), Feedback, Tone, Dry and Echoes.
- [ ] The voice crossfade of section 3.2, with `pending`; Step and BPM fade every voice that is on.
- [ ] Block-wise processing of section 3.3, split at the shortest loop if needed; the output must not change (compare with the per-sample version kept in the test).
- [ ] `test_changes` and `test_timing` passing, also with `SAN=1`.
- [ ] Settle by listening: fade length and shape, smoothing time, Tone range, Feedback maximum, the clipper's knee, the pan law. Update the spec's **tune** values.
- [ ] **Done when** dragging every control in Carla while playing makes no clicks, and a tempo change sounds like a speed change, not a glitch.

### Milestone 4: Ingen and the Pi (tag `v0.1.0`)

- [ ] WSL Ingen as in section 5.3, including save and reload, and patch tempo following once the overlay names `bpm` (milestone 5 can be done locally first).
- [ ] What Peepo shows when `instantiate` fails (simulate it with a build that refuses to allocate).
- [ ] Pi: built in Peepo's arm64 build root and installed from there, so the next package does not wipe it. Section 5.4 measured: xruns, DSP load, memory, with one voice on and with all four. Two instances in one patch as well (about 50 MB).
- [ ] If memory or CPU is too high: lower the longest loop (fewer steps, or Step up to 1/8) in the spec first, before any patch is saved.
- [ ] README status updated.
- [ ] **Done when** 30 minutes on the Pi give no xruns traceable to Euclidelay and the numbers are recorded.

### Milestone 5: Peepo integration (in Peepo's repository)

Not in this repository; listed so nothing is forgotten.

- [ ] Pin the commit of `v0.1.0` in `tools/ingen-spike/14-more-plugins.sh` (`make` with `OPT` and `LV2DIR=/opt/peepo/lib/lv2 install`).
- [ ] URI in `config/catalogue/inventory.txt`.
- [ ] Overlay entry: name Euclidelay, category Delay, a description that says what it does (for example "Echoes on four Euclidean rhythms. Each voice loops its pattern with feedback, with its own level and pan."), `"bypass": "port"`, `"tempo": "bpm"`, groups: Main (Step, Echoes, Dry, Tone) and one per voice (Voice n, Steps, Hits, Shift, Feedback, Level, Pan). If the module page caps a group's size, split each voice into Pattern n and Sound n.
- [ ] Regenerate `docs/33-module-list.md` with `tools/document-catalogue.py`, and grep the catalogue export for the URI before saying it is offered.
- [ ] A module page check on Windows (`peepo_ui_check`) with 37 controls on one page.

### Later

- A custom Peepo page: four rings like the Euclidean module's, drawn from the controls (Peepo's `pedal_layout` and `euclidean_view` in `src/ui/application.cpp`), with the compact bars two to a row below. Peepo work only; Euclidelay needs no new port for it.
- The items under "Not in the first version" in [design.md](design.md), once the first version has been played.

## 7. Risks

| Item | Why it matters | What is done about it |
| --- | --- | --- |
| Memory | 24.6 MB an instance at 48 kHz on a 2 GB Pi | Measured in milestone 4; the longest loop can still shrink before patches exist |
| Cache misses | Taps read 6 MB buffers at scattered places | Block-wise tap reads (section 3.3); measured on the Pi |
| Loud stacking | Up to 64 taps on a held chord | Echoes level, soft clipper, tested |
| Clicks on change | Pattern and tempo changes move every read | Crossfades with one pending setting; `test_changes` |
| Patterns differ from the Euclidean module | Users will set both side by side | Tested against Peepo's own function, every combination |
| 37 controls | A long module page | Groups in the overlay; a custom page later |
| Engine bypass | Cuts the echoes and leaves the right side silent | `enabled` port and `"bypass": "port"` |
