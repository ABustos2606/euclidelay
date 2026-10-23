# Euclidelay

A delay whose echoes fall on Euclidean rhythms, written for Peepo and other LV2 hosts.
GPL-2.0-or-later, Copyright (c) 2026 Alejandro Bustos; see [LICENSE](LICENSE).

**Status, 23 October 2026: milestone 3, changes while playing.** Every
control can move while playing: Level, Pan, Feedback, Tone, Dry and Echoes
glide over 20 ms, and Steps, Hits, Shift, Step and BPM crossfade each voice
to its new pattern over 30 ms. The offline tests find no clicks; the fade
and glide times are still to be settled by listening.

Euclidelay has four voices, each switched on or off on its own, so one, two,
three or four can play in any combination. Each voice spreads a number of
**hits** as evenly as it can over a number of **steps**, the way Peepo's Euclidean module does,
and the same numbers give the same pattern. The moment you play is step 0.
After that, the voice repeats what you played on each hit of its pattern.
When the pattern ends it starts again, quieter by the voice's **Feedback**,
until it dies away. Each voice has its own level and its own place between
left and right. With voice 1 at 7 steps, 4 hits and voice 2 at 4 steps,
3 hits, one note becomes two interlocking echo rhythms that drift in and out
of phase as they loop.

One **Step** (a note value, 1/32 to 1/4) sets how far apart steps are. Its
**BPM** follows Peepo's patch tempo. Steps 1, Hits 1 is a plain delay of one
step.

| Plugin | URI |
| --- | --- |
| Euclidelay | `https://github.com/ABustos2606/euclidelay#euclidelay` |

Build on Linux with a C++17 compiler, GNU Make, pkg-config and LV2 headers
(`build-essential pkg-config lv2-dev` on Debian; the tests need
`python3-rdflib`). The build needs no network:

```sh
make                                       # build/euclidelay.lv2
make test                                  # ttl_check, patterns, one voice, voices, engine, changes, the sliced plugin test, timing
make test-hosts                            # jalv and Ingen (source Peepo's env.sh for its Ingen)
make install PREFIX=/usr/local
make install LV2DIR=/opt/peepo/lib/lv2     # where Peepo finds plugins
make OPT="-O2 -mcpu=cortex-a76"            # native Pi build
```

The behaviour specification is [docs/design.md](docs/design.md); the build
and milestones are in
[docs/implementation-plan.md](docs/implementation-plan.md). Do not rename
plugin URIs or port symbols after patches use them. Peepo's catalogue overlay,
pinned build integration and hardware evidence belong in Peepo's repository.
