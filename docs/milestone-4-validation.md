# Milestone 4: Ingen and the Pi

**Checked:** 23 and 24 October 2026, on the working tree after `12d2bd0` (milestone 3). The plugin's code is unchanged since milestone 3, apart from the `REFUSE_ALLOC=1` switch, which is off in every normal build: the `euclidelay.so` measured on the Pi is the milestone 3 code built for arm64.

## Ingen under WSL (`make test-hosts`)

Peepo's Ingen (`/opt/peepo`, sourced from `tools/ingen-spike/env.sh`) on a private dummy JACK server at 48 kHz, with 1 and 4 threads:

- loads, runs, Dry on both outputs; every one of the 34 controls saved, the block deleted, the patch loaded again and every control back; no LV2 state saved;
- **the echoes follow BPM:** voice 1 as a plain delay of one 1/16 step puts its echo 6000 frames after the dry impulse at 120 BPM and 7423 at 97 BPM, as the spec gives;
- jalv: loads and runs.

**Patch tempo** is applied by Peepo's client, which sets the control the overlay names (`"tempo": "bpm"`). That overlay entry is milestone 5, in Peepo's repository; until then, the plugin's half is what is checked here: the `bpm` control, set through Ingen, moves the echoes, and `test_changes` shows a BPM change crossfades every voice without a click. Playing a recorded guitar file and listening is still to do.

## When `instantiate` fails

`make REFUSE_ALLOC=1` builds a plugin (in `build/refuse/`) whose fourth buffer is refused, as when memory runs out. `make test-refuse` checks that `instantiate` then returns null at 44.1, 48 and 96 kHz and, with `SAN=1`, that the three buffers it took are freed. `make test-hosts` loads it in hosts:

| Host | What happens |
| --- | --- |
| jalv | exits with an error |
| Ingen, adding the block | refused with status 9 (creation failed); Ingen logs `Failed to instantiate <...euclidelay>`, carries on and describes its graph |
| Peepo, adding the module | Peepo maps status 9 to "Engine could not create the object" and shows **"Could not add Euclidelay: Engine could not create the object"** (`src/ui/application.cpp`, `PendingKind::Add`); read from Peepo's source, not seen on the screen |
| Ingen, loading a saved patch with the block | **Ingen crashes.** The load itself answers status 0 without the block; the next `Get` of the graph, which Peepo sends after every load, segfaults in `ClientUpdate::put_port` (called from `put_graph`) |

The crash is Ingen's, not Euclidelay's: Ingen never gets a handle from a plugin whose `instantiate` fails, so any plugin that fails at load does the same. Reproduced with Peepo's Ingen under WSL (backtrace with gdb), loading into `/main` as Peepo's `LoadPatch` does and into a subgraph; loading the same patch with the working build and describing it is fine. On the Pi, the engine service would restart with an empty graph. `make test-hosts` reports it as `KNOWN Ingen defect` rather than failing, so it passes on its own once Ingen is fixed. It belongs in Peepo's Ingen patches; for Euclidelay it means a patch should not be loaded when the Pi is short of the memory its instances need (about 25 MB each).

## The arm64 build

In Peepo's arm64 build root (`/opt/peepo-arm64`, Debian 12, under qemu): `make OPT="-O2 -mcpu=cortex-a76"`, `make test` (all pass; the timing under emulation means nothing), `make test-refuse`, then `make LV2DIR=/opt/peepo/lib/lv2 install` inside the root, so the next Peepo package built from that root carries the bundle (`check-prefix.py` will also require it once milestone 5 pins Euclidelay in `14-more-plugins.sh`). The same bundle was copied to the Pi's `/opt/peepo/lib/lv2/euclidelay.lv2/`:

```
2d9f935588dfb1c1a36c5e3172ed346cf9a2d88f41da2930f4202dde1306f250  euclidelay.so
a5973469a90fe9445fe754d76fb4b733d52c287c6071e9df378ebd748088a974  euclidelay.ttl
6bee835a28f654148d59e2338c06142d2fdfc224789791f97239cd45368c8eea  manifest.ttl
```

Peepo's catalogue does not offer it until milestone 5 (inventory and overlay).

**A compiler bug found on the way.** Debian 12's GCC 12.2 on arm64 stops with an internal compiler error (`vect_transform_reduction`, at any `-O2`) on two test loops that took a `double` maximum with `std::fmax` over `float` samples, in `test_engine` and `test_changes`. So `make test` did not build on the Pi before this milestone. Those two loops now use `std::max`. The plugin itself was never affected.

## On the Pi

Pi 5, 2 GB, Debian 12, Peepo running (JACK on `hw:CODEC` at 48 kHz, 128 frames x 3 periods), the user's patch left playing in Peepo's engine.

**`make test`, native** (`OPT="-O2 -mcpu=cortex-a76"`): all pass. `test_timing` (10 simulated minutes, all four voices, random control changes, a normal-priority thread beside the running engine): median **16.4 us** a 128-frame period, p99.9 **110.5 us**, worst **269 us**, against a budget of 667 us (25% of the 2667 us period).

**A typical patch, 55 minutes** (`tools/pi_measure.py`): a private Ingen, one more client of Peepo's JACK server, holding IN -> GxAmplifier (`gx_amp`) -> Euclidelay -> Dragonfly Room -> OUT. The script plays plucked notes into it and reads its output, never the speakers, and changes a random Euclidelay control every 2 to 5 s (BPM, Step, Steps with Hits from half to all of them, Shift, Feedback, Level, Pan, Tone, Echoes). Every phase starts with each voice at 16 hits in 16 steps. JACK's xruns and DSP load are the whole machine's, Peepo's engine included; the baseline phase, before any Euclidelay, is the comparison.

Run 24 October 2026, 00:10 to 01:06; the raw results are in [pi-measure-20261024.json](pi-measure-20261024.json).

| Phase | Minutes | Euclidelays | Voices on | Control changes | xruns | DSP load mean / p99 / max | Private engine resident |
| --- | ---: | ---: | ---: | ---: | ---: | --- | ---: |
| baseline (amp, reverb) | 5 | 0 | - | 0 | 29 | 12.5 / 16.0 / 18.1 % | 117.5 MiB |
| one | 10 | 1 | 1 | 183 | 34 | 13.4 / 17.1 / 22.2 % | 142.0 MiB |
| four | 30 | 1 | 4 | 542 | 59 | 13.8 / 18.2 / 36.5 % | 143.5 MiB |
| two | 10 | 2 | 4 each | 176 | 60 | 15.7 / 20.5 / 23.0 % | 170.0 MiB |

- **Memory:** each Euclidelay adds **25.0 and 24.5 MiB** resident (the spec's 24.6 MB): the buffers are touched in `activate`, so all of it is resident at once. Two instances leave 1.5 GiB available on the 2 GB Pi. No change to the longest loop is needed.
- **CPU:** one instance with all four voices at work adds about 1.3 points of mean DSP load over the baseline, two add about 3.2. The single 36.5% reading in the four-voice phase was one second in 1800.
- **Output:** no non-finite block in 55 minutes.
- **xruns:** about 6 a minute with no Euclidelay, 3.4 with one voice, 2.0 with four, 6.0 with two instances. They do not grow with Euclidelay and are there without it. The Pi's power supply is the documented cause: `vcgencmd get_throttled` read `0x50005` (under-voltage and throttling now) at the start and during short tries, and `0x50000` (both have happened) otherwise. The user is replacing the supply; **the 30-minute xrun check must be run again after that** (`tools/pi_measure.py`, same phases) before the milestone is called done.

## Still to do

- Listening: a guitar through Euclidelay in Ingen (WSL or the Pi), and the milestone 1 to 3 listening checks.
- Peepo integration (milestone 5): pin the commit, inventory, overlay with `"tempo": "bpm"` and `"bypass": "port"`; then patch tempo following and the module page on the Pi.
- In Peepo: the Ingen crash on loading a patch with a block whose `instantiate` fails.
