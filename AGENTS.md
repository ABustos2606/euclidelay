# Euclidelay

Euclidelay is an LV2 delay whose echoes fall on the hits of Euclidean rhythms. Four voices, each switched on or off on its own, set a pattern (steps, hits, shift); each voice repeats the input on its hits, loops the pattern with feedback, and sits at its own place in the stereo field. It is a plugin written for **Peepo**, and Peepo is the host that matters. Read `docs/design.md` before changing behaviour or ports: it is the spec. `docs/implementation-plan.md` says how to build it, in milestones.

## Peepo

Peepo is a guitar pedal built on a Raspberry Pi 5 (2 GB, Debian 12 arm64) running the **Ingen** modular host on JACK, with an LVGL touch UI. Users patch LV2 modules together with audio, control (CV) and MIDI cables. It has two encoders and three momentary stomps. It was called "Pedal PI" before; old names (`pedalpi`, `urn:pedalpi:ns#`) still turn up.

- Repository: `C:\Pedal PI` (https://github.com/ABustos2606/Peepo). It is readable from here (see `.claude/settings.json`).
- Docs and code worth reading there:
    - `src/core/euclidean.hpp`: how Peepo draws the Euclidean module's patterns, matching Poly's `poly_euclidean.c`. Euclidelay's patterns must be the same for the same numbers. That file is GPL-3.0: never copy its code here; write ours and test it against its output (see the plan);
    - `docs/29-parameters-files-tempo.md`: the patch tempo, which sets a module's BPM control named in the overlay;
    - `docs/22-plugin-catalogue.md` and `config/catalogue/overlay.json`: how plugins are named, grouped and described in the UI, and `"bypass": "port"`;
    - `docs/03-interface-and-controls.md` and `docs/40-module-rail-and-aura.md`: module pages, and the custom Euclidean page;
    - `reference/upstream/ingen/`: Ingen's source.
- `C:\LV2-own-plugins\sway`, `C:\LV2-own-plugins\discipline` and `C:\LV2-own-plugins\loo-pi` are sister projects: same host limits, same kind of build and tests. Copy their patterns (LV2 glue, `ttl_check`, the sliced plugin test, the timing test, Discipline's `lv2:enabled` bypass) rather than reinventing them.
- Peepo builds every third-party plugin from its own repository at a pinned commit (`tools/ingen-spike/14-more-plugins.sh`). Keep the build plain (`make`, no network access, works natively on arm64).

## Host limits that shape the code

- **Ingen runs a block in slices** when a control changes during a period, moving the audio pointers to each slice. Everything is per sample and carries on across slices. Never assume one `run()` per period.
- **Engine bypass does not run the plugin.** It copies input to output and the echoes stop dead. Peepo bypasses Euclidelay through its `lv2:enabled` port instead, so echoes ring out.
- **xruns are the limit.** In `run()`: no allocation, no locks, no file or console I/O. The delay buffers are allocated once, in `instantiate`.
- **Memory is finite.** Four voices of up to 32 s each are about 25 MB at 48 kHz. Don't raise the limits without measuring on the Pi.
- **Blocks run on several threads.** Instances share nothing.
- **No LV2 state.** Everything is in control ports, so patches save it.

## Rules

- Plugin URI: `https://github.com/ABustos2606/euclidelay#euclidelay`. Never use Peepo's namespace. Don't change a URI, port index or port symbol once a patch has been saved with it; new ports go at the end.
- Port names, value names and descriptions appear on Peepo's small screen: short, plain ASCII, at most 16 characters for port and value names, and they say what the control does. Never name or describe a pedal it resembles.
- License: GPL-2.0-or-later, `Copyright (c) 2026 Alejandro Bustos`; the text is in `LICENSE`. Every source file, header, Makefile and `.ttl` file starts with `SPDX-License-Identifier: GPL-2.0-or-later`; the plugin declares `doap:license <https://spdx.org/licenses/GPL-2.0-or-later>`. Euclidelay is our own code: nothing in it is vendored.
- Line endings are LF. Files go to Linux and the Pi.
- Use Peepo's name, never "Pedal PI", in new text.
- Never commit or push; the user does.

## Testing

- `make test` (Linux; under WSL on the PC): `ttl_check`, the patterns, one voice, the whole plugin sliced as Ingen runs it, and the timing test.
- Desktop: `lv2lint`, `jalv`, Carla.
- Peepo's development engine runs under WSL (Debian) with Ingen in `/opt/peepo`; Peepo's `tools/ingen-spike/env.sh` starts it.
- On the Pi: build in the arm64 build root, play it in a typical patch, and measure xruns and memory before calling a milestone done.
