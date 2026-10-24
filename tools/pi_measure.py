# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 Alejandro Bustos
"""Euclidelay on the Pi, in a typical patch, with controls being moved.

Starts a private Ingen as one more client of Peepo's running JACK server and
builds IN -> amp -> Euclidelay -> reverb -> OUT in it. Peepo's own engine,
its patch and the audio interface are left alone: the test signal (plucked
notes) comes from this script and the output comes back to it, never to the
speakers. Each phase runs for its minutes while controls change every few
seconds, and records JACK's xruns and DSP load and the private engine's
resident memory before and after adding Euclidelay. Run as the user that
owns the JACK server, on the Pi:

    sudo -u peepo python3 pi_measure.py --phases baseline:5,one:10,four:30,two:10 --out result.json

Phases: baseline (amp and reverb only), one (one Euclidelay, voice 1 on),
four (one Euclidelay, all four voices on), two (two Euclidelays in series,
all voices on). Needs python3-jack-client and numpy.
"""
import argparse
import json
import os
from pathlib import Path
import random
import re
import socket
import subprocess
import sys
import time

import jack
import numpy as np

URI = "https://github.com/ABustos2606/euclidelay#euclidelay"
AMP = "http://guitarix.sourceforge.net/plugins/gx_amp#GUITARIX"
REVERB = "urn:dragonfly:room"
PREFIX = "/opt/peepo"
PREFIXES = """@prefix patch: <http://lv2plug.in/ns/ext/patch#> .
@prefix ingen: <http://drobilla.net/ns/ingen#> .
@prefix lv2: <http://lv2plug.in/ns/lv2core#> .
@prefix xsd: <http://www.w3.org/2001/XMLSchema#> .
"""


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


class Protocol:
    """Request/response client for Ingen's patch protocol (as in tests/host_smoke.py)."""
    def __init__(self, path):
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.socket.connect(str(path))
        self.buffer = b""
        self.sequence = 0

    def request(self, verb, fields):
        self.sequence += 1
        message = (PREFIXES + f'[] a patch:{verb} ; patch:sequenceNumber '
                   f'"{self.sequence}"^^xsd:int ; {fields} .\n')
        self.socket.sendall(message.encode())
        deadline = time.monotonic() + 20
        text_all = ""
        while True:
            while b"\0" not in self.buffer:
                remaining = deadline - time.monotonic()
                require(remaining > 0, f"Ingen {verb} response timed out")
                self.socket.settimeout(remaining)
                data = self.socket.recv(65536)
                require(data, "Ingen closed its socket")
                self.buffer += data
            data, self.buffer = self.buffer.split(b"\0", 1)
            text = data.decode()
            text_all += text
            for match in re.finditer(r"a patch:Response\s*;(.*?)\.\s*(?:\n|$)", text, re.S):
                sequence = re.search(r'patch:sequenceNumber "(\d+)"', match[1])
                status = re.search(r'patch:body "(-?\d+)"', match[1])
                if sequence and int(sequence[1]) == self.sequence and status:
                    require(int(status[1]) == 0, f"Ingen {verb}: status {status[1]}")
                    return text_all

    def put(self, subject, body):
        return self.request("Put", f"patch:subject <{subject}> ; patch:body [ {body} ]")

    def connect(self, tail, head):
        return self.put("ingen:/main/", f"a ingen:Arc ; ingen:tail <{tail}> ; ingen:head <{head}>")

    def set(self, path, value):
        return self.request("Set", f'patch:subject <{path}> ; patch:property ingen:value ; '
                            f'patch:value "{value}"^^xsd:float')

    def audio_ports(self, block):
        """The block's audio inputs and outputs, in index order, from what Ingen says about it."""
        # Ingen answers a Get before it sends the description: a second request's
        # answer comes after all of it.
        text = self.request("Get", f"patch:subject <ingen:/main/{block}>")
        text += self.request("Get", "patch:subject <ingen:/main/in>")
        self.last_get = text
        ports = {"in": [], "out": []}
        for chunk in re.split(r"\n\s*\n", text):
            subject = re.search(rf"patch:subject <(?:ingen:)?/main/{block}/([A-Za-z0-9_]+)>", chunk)
            index = re.search(r'lv2:index "(\d+)"', chunk)
            if not subject or not index or "lv2:AudioPort" not in chunk:
                continue
            kind = "in" if "lv2:InputPort" in chunk else "out" if "lv2:OutputPort" in chunk else None
            if kind and subject[1] not in [p for _, p in ports[kind]]:
                ports[kind].append((int(index[1]), subject[1]))
        return {k: [p for _, p in sorted(v)] for k, v in ports.items()}


def plucks(rate, seconds, seed=7):
    """A loop of plucked notes: decaying harmonics, a new note every 0.2 to 1.5 s."""
    rng = np.random.default_rng(seed)
    out = np.zeros(int(rate * seconds), np.float32)
    t = 0.0
    while t < seconds - 2:
        start, length = int(t * rate), int(2 * rate)
        f = rng.choice([82.4, 110, 146.8, 196, 246.9, 329.6]) * rng.choice([1, 1, 2])
        n = np.arange(length) / rate
        note = sum(np.sin(2 * np.pi * f * h * n) * np.exp(-n * (2 + 1.5 * h)) / h for h in range(1, 7))
        out[start:start + length] += (0.25 * rng.uniform(0.4, 1) * note).astype(np.float32)
        t += rng.uniform(0.2, 1.5)
    return out


def rss_kib(pid):
    for line in Path(f"/proc/{pid}/status").read_text().splitlines():
        if line.startswith("VmRSS:"):
            return int(line.split()[1])
    return 0


def mem_available_kib():
    for line in Path("/proc/meminfo").read_text().splitlines():
        if line.startswith("MemAvailable:"):
            return int(line.split()[1])
    return 0


def throttled():
    try:
        return subprocess.run(["vcgencmd", "get_throttled"], capture_output=True, text=True, timeout=5).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return "unknown"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--phases", default="baseline:5,one:10,four:30,two:10",
                        help="name:minutes,... (baseline, one, four, two)")
    parser.add_argument("--out", type=Path, default=Path("euclidelay-pi-measure.json"))
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--lv2-path", default="", help="directories searched before Peepo's (a bundle not installed yet)")
    args = parser.parse_args()
    phases = [(name, float(minutes)) for name, minutes in (p.split(":") for p in args.phases.split(","))]
    require(all(name in ("baseline", "one", "four", "two") for name, _ in phases), "unknown phase")
    rng = random.Random(args.seed)

    run = Path(f"/tmp/euclidelay-measure-{os.getpid()}")
    run.mkdir(parents=True, exist_ok=True)
    sock = run / "ingen.sock"
    env = dict(os.environ)
    # As Peepo's peepo-engine sets them.
    lib = Path(PREFIX, "lib")
    env["LD_LIBRARY_PATH"] = ":".join(sorted({str(p.parent) for p in [*lib.glob("libingen*"), *lib.glob("*/libingen*")]}))
    env["LV2_PATH"] = ":".join([*filter(None, args.lv2_path.split(":")),
                                *(str(p) for p in sorted([*lib.glob("lv2"), *lib.glob("*/lv2")]) if p.is_dir()), "/usr/lib/lv2"])
    client_name = "euclidelay_measure"
    log = (run / "ingen.log").open("w")
    engine = subprocess.Popen([f"{PREFIX}/bin/ingen", "-e", "-f", "-E", "0", "-n", client_name, "-S", str(sock)],
                              stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, env=env)
    probe = protocol = None
    report = {"started": time.strftime("%Y-%m-%d %H:%M:%S"), "throttled_before": throttled(), "phases": []}
    try:
        deadline = time.monotonic() + 60
        while not sock.exists():
            require(engine.poll() is None, "private Ingen exited; see " + str(run / "ingen.log"))
            require(time.monotonic() < deadline, "private Ingen did not open its socket")
            time.sleep(0.2)
        time.sleep(1.5)
        protocol = Protocol(sock)

        probe = jack.Client("euclidelay_measure_probe", no_start_server=True)
        rate, period = probe.samplerate, probe.blocksize
        report.update({"sample_rate": rate, "period_frames": period})
        signal = plucks(rate, 16)
        source = probe.outports.register("src")
        back = [probe.inports.register(n) for n in ("ret_l", "ret_r")]
        stats = {"pos": 0, "xruns": 0, "nonfinite": 0, "peak": 0.0}

        def process(frames):
            start = stats["pos"]
            idx = (start + np.arange(frames)) % len(signal)
            source.get_array()[:] = signal[idx]
            stats["pos"] = (start + frames) % len(signal)
            for port in back:
                data = port.get_array()
                if not np.all(np.isfinite(data)):
                    stats["nonfinite"] += 1
                else:
                    stats["peak"] = max(stats["peak"], float(np.max(np.abs(data))))

        def xrun(_delay):
            stats["xruns"] += 1

        probe.set_process_callback(process)
        probe.set_xrun_callback(xrun)
        probe.activate()

        protocol.put("ingen:/main/in", "a lv2:InputPort, lv2:AudioPort")
        for side in ("l", "r"):
            protocol.put(f"ingen:/main/out_{side}", "a lv2:OutputPort, lv2:AudioPort")
        time.sleep(1)
        report["engine_rss_empty_kib"] = rss_kib(engine.pid)
        protocol.put("ingen:/main/amp", f"a ingen:Block ; lv2:prototype <{AMP}>")
        protocol.put("ingen:/main/reverb", f"a ingen:Block ; lv2:prototype <{REVERB}>")
        amp, reverb = protocol.audio_ports("amp"), protocol.audio_ports("reverb")
        if not (amp["in"] and amp["out"] and len(reverb["in"]) >= 2 and len(reverb["out"]) >= 2):
            (run / "get.txt").write_text(protocol.last_get)
            raise RuntimeError(f"unexpected ports: amp {amp}, reverb {reverb}; Ingen's answer is in {run / 'get.txt'}")
        time.sleep(1)
        report["engine_rss_amp_reverb_kib"] = rss_kib(engine.pid)
        probe.connect(source, f"{client_name}:in")
        probe.connect(f"{client_name}:out_l", back[0])
        probe.connect(f"{client_name}:out_r", back[1])
        protocol.connect("ingen:/main/in", f"ingen:/main/amp/{amp['in'][0]}")
        for side, port in zip(("l", "r"), reverb["out"]):
            protocol.connect(f"ingen:/main/reverb/{port}", f"ingen:/main/out_{side}")

        chain = []  # Euclidelay blocks in series between amp and reverb
        arcs = []

        def connect(tail, head):
            protocol.connect(tail, head)
            arcs.append((tail, head))

        def rewire(count):
            """amp -> count Euclidelays (in series, the left output into the next) -> reverb."""
            for tail, head in arcs:
                protocol.request("Delete", f"patch:subject <ingen:/main/> ; patch:body [ a ingen:Arc ; "
                                 f"ingen:tail <{tail}> ; ingen:head <{head}> ]")
            arcs.clear()
            while len(chain) > count:
                name = chain.pop()
                protocol.request("Delete", f"patch:subject <ingen:/main/{name}>")
            while len(chain) < count:
                name = f"euclidelay_{len(chain) + 1}"
                before = rss_kib(engine.pid)
                protocol.put(f"ingen:/main/{name}", f"a ingen:Block ; lv2:prototype <{URI}>")
                time.sleep(1)
                report.setdefault("engine_rss_added_kib", []).append(rss_kib(engine.pid) - before)
                chain.append(name)
            tail = [f"ingen:/main/amp/{amp['out'][0]}"]
            for name in chain:
                for t in tail[:1]:
                    connect(t, f"ingen:/main/{name}/in")
                tail = [f"ingen:/main/{name}/out_l", f"ingen:/main/{name}/out_r"]
            for i, port in enumerate(reverb["in"][:2]):
                connect(tail[min(i, len(tail) - 1)], f"ingen:/main/reverb/{port}")

        def move(name, voices_on):
            """One random control change on one Euclidelay."""
            v = rng.randrange(1, 5)
            choice = rng.randrange(10)
            path = f"ingen:/main/{name}"
            if choice == 0:
                protocol.set(f"{path}/bpm", rng.randrange(30, 301))
            elif choice == 1:
                protocol.set(f"{path}/step", rng.randrange(0, 7))
            elif choice in (2, 3):
                steps = rng.randrange(1, 17)
                protocol.set(f"{path}/steps_{v}", steps)
                protocol.set(f"{path}/hits_{v}", rng.randrange(steps // 2, steps + 1))
            elif choice == 4:
                protocol.set(f"{path}/shift_{v}", rng.randrange(0, 16))
            elif choice == 5:
                protocol.set(f"{path}/feedback_{v}", round(rng.uniform(0, 0.95), 3))
            elif choice == 6:
                protocol.set(f"{path}/level_{v}", round(rng.uniform(0.3, 1), 3))
            elif choice == 7:
                protocol.set(f"{path}/pan_{v}", round(rng.uniform(-1, 1), 3))
            elif choice == 8:
                protocol.set(f"{path}/tone", round(rng.uniform(0, 1), 3))
            else:
                protocol.set(f"{path}/echoes", round(rng.uniform(0.4, 1), 3))
            return 2 if choice in (2, 3) else 1

        for phase, minutes in phases:
            count = {"baseline": 0, "one": 1, "four": 1, "two": 2}[phase]
            rewire(count)
            for name in chain:
                for n in range(1, 5):
                    protocol.set(f"ingen:/main/{name}/voice_{n}", 1 if phase != "one" or n == 1 else 0)
                    # The heaviest pattern to start with: 16 hits in 16 steps.
                    protocol.set(f"ingen:/main/{name}/steps_{n}", 16)
                    protocol.set(f"ingen:/main/{name}/hits_{n}", 16)
            time.sleep(2)
            x0, nf0 = stats["xruns"], stats["nonfinite"]
            stats["peak"] = 0.0
            loads, changes, minute_xruns = [], 0, []
            start = time.monotonic()
            next_change = start + rng.uniform(2, 5)
            next_minute, minute_x = start + 60, stats["xruns"]
            while time.monotonic() - start < minutes * 60:
                time.sleep(1)
                require(engine.poll() is None, "private Ingen exited")
                loads.append(float(probe.cpu_load()))
                now = time.monotonic()
                if chain and now >= next_change:
                    changes += move(rng.choice(chain), 1 if phase == "one" else 4)
                    next_change = now + rng.uniform(2, 5)
                if now >= next_minute:
                    minute_xruns.append(stats["xruns"] - minute_x)
                    minute_x, next_minute = stats["xruns"], next_minute + 60
                    print(f"{phase}: {len(minute_xruns)} min, xruns {stats['xruns'] - x0}, "
                          f"DSP load mean {np.mean(loads):.1f}% max {np.max(loads):.1f}%", file=sys.stderr, flush=True)
            result = {
                "phase": phase, "minutes": minutes, "euclidelays": count,
                "voices_on": 0 if phase == "baseline" else (1 if phase == "one" else 4),
                "xruns": stats["xruns"] - x0, "xruns_per_minute": minute_xruns,
                "dsp_load_percent": {"mean": round(float(np.mean(loads)), 2),
                                     "p99": round(float(np.percentile(loads, 99)), 2),
                                     "max": round(float(np.max(loads)), 2)},
                "control_changes": changes, "nonfinite_blocks": stats["nonfinite"] - nf0,
                "output_peak": round(stats["peak"], 4),
                "engine_rss_kib": rss_kib(engine.pid), "mem_available_kib": mem_available_kib(),
                "throttled": throttled(),
            }
            report["phases"].append(result)
            print(json.dumps(result), file=sys.stderr, flush=True)
            args.out.write_text(json.dumps(report, indent=1) + "\n")
    finally:
        report["throttled_after"] = throttled()
        report["finished"] = time.strftime("%Y-%m-%d %H:%M:%S")
        args.out.write_text(json.dumps(report, indent=1) + "\n")
        if probe:
            probe.deactivate()
            probe.close()
        if protocol:
            protocol.socket.close()
        if engine.poll() is None:
            engine.terminate()
            try:
                engine.wait(timeout=10)
            except subprocess.TimeoutExpired:
                engine.kill()
        log.close()
    print(json.dumps(report, indent=1))


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        sys.exit(f"FAIL pi_measure: {error}")
