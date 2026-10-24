# Euclidelay: design

A delay whose echoes fall on the hits of Euclidean rhythms. Four voices each spread their hits over their steps, repeat the input on each hit, loop the pattern with feedback, and sit at their own place between left and right. It is the patch "Euclidean into four gated, panned delays" done properly: one module instead of about eighteen, with clean echoes, no clock or triggers, and overlapping notes that just work, as in any delay. Written for Peepo (Ingen on JACK, Raspberry Pi 5). This document is the spec: ports and behaviour change here first.

Plugin: **Euclidelay** (`https://github.com/ABustos2606/euclidelay#euclidelay`), an `lv2:DelayPlugin`. Peepo lists it under Delay.

**Written:** 23 October 2026. Values marked **tune** are starting points that listening settles.

## The idea in one paragraph

A Euclidean pattern is a list of times. Voice 1 at 7 steps, 4 hits is `xx.x.x.`: hits on steps 0, 1, 3 and 5. The moment you play is step 0. Euclidelay plays what you played again at steps 1, 3 and 5. At step 7 the pattern starts again (that is step 0 of the second pass), and the whole pass repeats at the voice's Feedback level, then again at Feedback squared, and so on. Up to four voices do this at once from the same input, each switched on or off and with its own pattern, level and pan. Because every voice is a linear delay, two notes played close together each get their own echoes, summed, as in any delay.

## Patterns

A voice's pattern comes from **Steps** (`N`, 1 to 16), **Hits** (`K`, 0 to 16) and **Shift** (`R`, 0 to 15). They give the same pattern as Peepo's Euclidean module for the same numbers (Poly's `poly_euclidean.c`, drawn by Peepo's `src/core/euclidean.hpp`):

1. Hits above Steps count as Steps; Shift is taken modulo Steps.
2. Walk `i` from 0 to `N - 1` with a bucket starting at 0. Add `K` to the bucket at each `i`. When the bucket reaches `N` or more, take `N` away and mark step `N - 1 - i` as a hit.
3. Shift moves every hit `R` steps later, wrapping around: a hit on step `j` lands on `(j + R) mod N`.

Examples (`x` hit, `.` rest, step 0 first):

| Steps, Hits, Shift | Pattern |
| --- | --- |
| 7, 4, 0 | `xx.x.x.` |
| 4, 3, 0 | `xxx.` |
| 8, 3, 0 | `x.x..x..` |
| 5, 2, 0 | `x.x..` |
| 5, 2, 1 | `.x.x.` |
| 16, 5, 0 | `x..x..x..x..x...` |
| 1, 1, 0 | `x` (a plain delay of one step) |

With Shift 0 and at least one hit, step 0 is always a hit. Hits 0 silences the voice.

**What Shift does to the echoes.** Shift rotates the pattern against the moment you play. With 5, 2, 0 (`x.x..`) the note itself is the first hit and the first echo comes 2 steps later. With 5, 2, 1 (`.x.x.`) the note falls on a rest: the echoes come 1 and 3 steps later, and on every later pass step 0 stays silent. Voices with the same Steps and Hits but different Shifts play the same rhythm offset from each other, for call-and-response between the left and right sides. A Shift change while playing crossfades like a Steps or Hits change (see "Changes while playing").

## Time

All voices share one step length:

    step (s) = Step in beats * 60 / BPM

**Step** is a note value: 1/32, 1/16 triplet, 1/16, 1/8 triplet, 1/8, dotted 1/8 or 1/4, which are 0.125, 1/6, 0.25, 1/3, 0.5, 0.75 and 1 beat. **BPM** runs from 30 to 300 (Peepo's tempo range) and Peepo sets it from the patch tempo. A voice's loop is `N` steps long, so the longest loop is 16 steps of 1/4 at 30 BPM: **32 s**. That bounds the memory (section "Memory").

In samples, with `S = step * rate`, the voice's loop length is `L = round(N * S)` and hit `k` sits at `D_k = round(k * S)`. Rounding each value on its own means pass `m` can be up to `m / 2` samples off the ideal time: 0.5 ms after 50 passes at 48 kHz, which nothing can hear. Delays are whole samples, never interpolated, so loops don't get duller with every pass.

## One voice

Each voice has its own loop buffer `y`, written every sample:

    y[t] = x[t] + fb * tone(y[t - L])

`x` is the input (muted while the plugin is off; see "On and off"), `fb` is the voice's Feedback and `tone` is the shared Tone filter. The voice's output reads `y` at each hit:

    v[t] = sum over hits k of y[t - D_k]   minus   x[t] if step 0 is a hit

The subtraction removes the first pass's step 0, which is the note itself and already in the dry sound. Later passes keep their step 0: `y[t - 0]` still holds `fb * y[t - L]`. So for one short note at time 0 the voice plays:

- pass 0: the hits other than step 0, at full level;
- pass `m` (from 1): every hit, at `fb^m`, filtered `m` times by Tone.

**Feedback** runs from 0 to 0.95 (**tune**); 0 plays the pattern once. Feedback is never 1: a loop always dies away.

**Tone** is a one-pole low-pass in every voice's feedback path, so each pass is darker than the last. 1 leaves the loops untouched; lower values darken them (0 is about 1.5 kHz, **tune**). Pass 0 is never filtered.

## Mix and stereo

Euclidelay takes one input and gives two outputs.

    left  = Dry * x  +  Echoes * clip( sum over voices of Level_v * gL(Pan_v) * v_v )
    right = Dry * x  +  Echoes * clip( sum over voices of Level_v * gR(Pan_v) * v_v )

- **Pan** runs from -1 (left) to 1 (right). Constant power: `gL = cos((Pan + 1) * pi / 4)`, `gR = sin((Pan + 1) * pi / 4)`, so a centred voice is -3 dB on each side (**tune**: a law with the centre at 0 dB may sit better beside the dry sound).
- **Dry** (0 to 1, default 1) is the input on both outputs. **Echoes** (0 to 1, default 0.8, **tune**) is the level of all the voices that are on.
- `clip` is a soft clipper on each echo side, transparent below about -6 dBFS and never above -0.2 dBFS (**tune**). Four voices of up to 16 hits can stack on a held chord. The clipper stops that from reaching full scale hard; it isn't a limiter, and Echoes is still the way to set the level.

## On and off

**Enabled** is the plugin's `lv2:enabled` port; Peepo's bypass uses it (`"bypass": "port"`), never the engine's block bypass, which would cut the echoes dead.

- Off: the input stops entering the loops (faded over 10 ms) and the dry sound passes at unity, whatever Dry is set to. Echoes already in the loops ring out with their patterns and feedback: trails.
- On: the input enters the loops again (faded in over 10 ms) and Dry applies again.

## Choosing voices

Each voice has its own **Voice n** switch, so any one, two, three or four voices can play, in any combination (voices 1 and 3, say), not only the first few.

- **Off:** the voice fades out over 20 ms (**tune**) and then stops: its echoes stop with it, and it costs no CPU. Its settings stay as they are, ready for when it comes back. Trails across a whole bypass are the job of Enabled, not of these switches.
- **On:** the voice starts empty and takes the input from that moment, faded in over 20 ms (**tune**) so a note already sounding enters without a click; it never plays what was in its loop before it was switched off. Switched on again while still fading out, its old echoes finish their fade while the new input comes in. Clearing a 6 MB buffer in `run()` would cost too much at once, so the voice instead counts the samples written since it came on and treats anything older as silence.
- All four off: Euclidelay passes the dry sound only.
- A voice that is on with Hits 0 or Level 0 is silent but still runs; Off is the way to save its CPU.

## Changes while playing

- **Level, Pan, Feedback, Tone, Dry and Echoes** glide to a new value over about 20 ms (**tune**), sample by sample, so knob moves and stomps never click.
- **Steps, Hits, Shift, Step and BPM** change where a voice reads and how long its loop is. The voice crossfades between its old and new settings over 30 ms (**tune**): for the length of the fade it runs both its old and new readings (taps and loop read) and fades from one to the other. A voice has at most one fade at a time. A change during a fade waits until the fade ends and then fades to the newest settings, so dragging a knob through many values costs at most two fades in a row. Step and BPM change every voice, so a tempo change starts a fade on every voice that is on.
- After a change of loop length, echoes already in the loop were written at the old spacing; they play out at the new one for one pass, slightly out of rhythm. That is what a tape echo does when its speed changes, and it is accepted.

## Memory

Each voice's buffer holds the longest loop plus the 30 ms fade: 32.03 s. At 48 kHz that is 1.54 M floats, 6.2 MB per voice and **24.6 MB** per instance. At 96 kHz it doubles. All of it is allocated in `instantiate` and zeroed in `activate`, never in `run()`. If an instance can't get its memory, `instantiate` returns null; it never runs with less. Peepo then refuses to add the module ("Could not add Euclidelay: Engine could not create the object"); loading a saved patch with it currently crashes Ingen, an Ingen bug for Peepo (see [milestone 4](milestone-4-validation.md)). Measured on the Pi: 25.0 MiB resident an instance.

## Ports

Indices and symbols are permanent once a patch is saved with them; new ports go at the end.

| Index | Symbol | Name | Kind | Range, default |
| --- | --- | --- | --- | --- |
| 0 | `in` | In | audio in | |
| 1 | `out_l` | Out L | audio out | |
| 2 | `out_r` | Out R | audio out | |
| 3 | `enabled` | Enabled | control, `lv2:enabled` toggle | 1 |
| 4 | `bpm` | BPM | control | 30 to 300; 120 |
| 5 | `step` | Step | enumeration | 1/32, 1/16T, 1/16, 1/8T, 1/8, 1/8 dot, 1/4; 1/16 |
| 6 | `dry` | Dry | control | 0 to 1; 1 |
| 7 | `echoes` | Echoes | control | 0 to 1; 0.8 |
| 8 | `tone` | Tone | control | 0 to 1; 0.7 |
| 9 + 7(n-1) | `voice_n` | Voice n | toggle | Off, On |
| 10 + 7(n-1) | `steps_n` | Steps n | integer | 1 to 16 |
| 11 + 7(n-1) | `hits_n` | Hits n | integer | 0 to 16 |
| 12 + 7(n-1) | `shift_n` | Shift n | integer | 0 to 15; 0 |
| 13 + 7(n-1) | `feedback_n` | Feedback n | control | 0 to 0.95; 0.5 |
| 14 + 7(n-1) | `level_n` | Level n | control | 0 to 1; 1 |
| 15 + 7(n-1) | `pan_n` | Pan n | control | -1 to 1 |

`n` is 1 to 4, so the voices take indices 9-15, 16-22, 23-29 and 30-36; 37 ports in all. Per-voice defaults (**tune**):

| Voice | On | Steps | Hits | Pan | Pattern |
| --- | --- | --- | --- | --- | --- |
| 1 | On | 8 | 3 | -0.6 | `x.x..x..` |
| 2 | On | 5 | 2 | 0.6 | `x.x..` |
| 3 | Off | 7 | 4 | -0.2 | `xx.x.x.` |
| 4 | Off | 4 | 3 | 0.2 | `xxx.` |

Voices 3 and 4 start off with their patterns ready: switching them on brings them in.

## Host limits that shape the code

- **Ingen runs a block in slices** when a control changes during a period, moving the audio pointers to each slice. Everything is per sample and carries on across slices; control values are read at the start of each slice. The plugin test runs it sliced at random points and unsliced and needs the same output, sample for sample, when controls don't change.
- **Engine bypass does not run the plugin** (Discipline's plan, section 1). Hence the `enabled` port.
- **No allocation, locks or I/O in `run()`.**
- **Blocks may run on several threads;** instances share nothing.
- **No LV2 state.** Everything is in control ports, so patches save them. The echoes in the buffers are not saved.
- **Non-finite values.** A non-finite input sample counts as silence and empties every voice; a voice whose loop or output would become non-finite empties itself at once. An emptied voice treats everything in its loop as silence (as when switched on) and is silent until new input arrives. The outputs are always finite. This should never happen; it guards against a bad input sample.

## Not in the first version

- Stereo input. The first version has one input, as most guitar chains are mono before a delay.
- A Step per voice (voice 2 in triplets against voice 1 in sixteenths).
- Tone, pitch shift or reverse per voice.
- A clock input, so steps follow an external clock rather than the BPM control.
- Freeze (feedback 1, input off) and Clear, as controls a stomp can play.
- Swing.
- CV inputs for Feedback, Level or Pan.
- A custom Peepo page with four rings, like the Euclidean module's. The pattern can be drawn from the controls alone, so no output port is needed for it.
