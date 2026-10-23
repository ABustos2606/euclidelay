# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 Alejandro Bustos
"""Optional Linux host check, using a private dummy JACK server.

Loads Euclidelay in jalv and in Ingen (1 and 4 threads), checks the sound
on both outputs, and saves and reloads an Ingen patch: every control must
come back. Needs jackd, jalv, Ingen, python3-jack-client and numpy. For
Peepo's Ingen, source Peepo's tools/ingen-spike/env.sh first. Never stops an
existing engine or server. Logs are kept in build/host-smoke/.
"""
import argparse
import os
from pathlib import Path
import re
import socket
import shutil
import subprocess
import sys
import time

import jack
import numpy as np

URI = "https://github.com/ABustos2606/euclidelay#euclidelay"
PREFIXES = """@prefix patch: <http://lv2plug.in/ns/ext/patch#> .
@prefix ingen: <http://drobilla.net/ns/ingen#> .
@prefix lv2: <http://lv2plug.in/ns/lv2core#> .
@prefix xsd: <http://www.w3.org/2001/XMLSchema#> .
"""
LEVEL = 0.25
# Values other than the defaults, for every control: saved, then reloaded.
# Echoes 0: the probe listens for the dry sound alone, every sample the same.
SAVED = {"enabled": 1, "bpm": 97, "step": 4, "dry": 0.5, "echoes": 0, "tone": 0.2}
for n in range(1, 5):
    SAVED.update({f"voice_{n}": n % 2, f"steps_{n}": 10 + n, f"hits_{n}": n, f"shift_{n}": n + 1,
                  f"feedback_{n}": 0.1 * n, f"level_{n}": 1 - 0.1 * n, f"pan_{n}": 0.3 * n - 0.75})


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def wait_for(check, message):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if check():
            return
        time.sleep(0.05)
    raise RuntimeError(message)


class Protocol:
    """Small request/response client for Ingen's patch protocol (as in Discipline)."""
    def __init__(self, path, transcript):
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.socket.connect(str(path))
        self.buffer = b""
        self.sequence = 0
        self.transcript = transcript.open("w")

    def request(self, verb, fields):
        self.sequence += 1
        message = (PREFIXES + f'[] a patch:{verb} ; patch:sequenceNumber '
                   f'"{self.sequence}"^^xsd:int ; {fields} .\n')
        self.socket.sendall(message.encode())
        self.transcript.write("SEND " + message + "\n")
        deadline = time.monotonic() + 10
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
            self.transcript.write("RECV " + text + "\n")
            for match in re.finditer(r"a patch:Response\s*;(.*?)\.\s*(?:\n|$)", text, re.S):
                sequence = re.search(r'patch:sequenceNumber "(\d+)"', match[1])
                status = re.search(r'patch:body "(-?\d+)"', match[1])
                if sequence and int(sequence[1]) == self.sequence and status:
                    require(int(status[1]) == 0, f"Ingen {verb}: {text}")
                    return text

    def close(self):
        self.socket.close()
        self.transcript.close()

    def put(self, subject, body):
        return self.request("Put", f"patch:subject <{subject}> ; patch:body [ {body} ]")

    def connect(self, tail, head):
        return self.put("ingen:/main/", f"a ingen:Arc ; ingen:tail <{tail}> ; ingen:head <{head}>")

    def set(self, path, value):
        return self.request("Set", f'patch:subject <{path}> ; patch:property ingen:value ; '
                            f'patch:value "{value}"^^xsd:float')

    def value(self, path):
        values = re.findall(r'ingen:value "([0-9.eE+-]+)"', self.request("Get", f"patch:subject <{path}>"))
        require(values, f"no value for {path}")
        return float(values[-1])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path)
    parser.add_argument("--ingen", default="ingen")
    parser.add_argument("--period", type=int, default=1024,
                        help="dummy JACK period (default: 1024, tolerant of WSL scheduling)")
    args = parser.parse_args()
    logdir = Path("build/host-smoke").resolve()
    logdir.mkdir(parents=True, exist_ok=True)
    server_name = f"euclidelay-{os.getpid()}"
    env = dict(os.environ, JACK_DEFAULT_SERVER=server_name, JACK_NO_AUDIO_RESERVATION="1")
    private_lv2 = logdir / f"lv2-{os.getpid()}"
    shutil.copytree(args.bundle.resolve(), private_lv2 / "euclidelay.lv2")
    env["LV2_PATH"] = str(private_lv2) + ":" + env.get("LV2_PATH", "/usr/local/lib/lv2:/usr/lib/lv2")
    processes, logs = [], []
    probe = protocol = None

    def launch(name, command):
        log = (logdir / f"{name}.log").open("w")
        logs.append(log)
        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT, env=env)
        processes.append(process)
        return process

    def stop(process):
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)

    try:
        server = launch("jack", ["jackd", "--no-realtime", "--timeout", "5000", "--name", server_name,
                                 "-d", "dummy", "-r", "48000", "-p", str(args.period)])
        wait_for(lambda: subprocess.run(["jack_lsp"], env=env, stdout=subprocess.DEVNULL,
                                        stderr=subprocess.DEVNULL).returncode == 0, "private JACK server did not start")
        require(server.poll() is None, "JACK server exited")
        probe = jack.Client("euclidelay_probe", servername=server_name, no_start_server=True)
        source = probe.outports.register("src")
        captures = [probe.inports.register(name) for name in ("cap_l", "cap_r")]
        observed = [np.nan, np.nan]

        @probe.set_process_callback
        def process(frames):
            source.get_array()[:] = LEVEL
            for i, port in enumerate(captures):
                data = port.get_array()
                # Every sample the same, or NaN: a block that only matches on average fails.
                observed[i] = float(data[0]) if np.all(data == data[0]) else np.nan

        probe.activate()

        def hears(gain):
            return lambda: observed == [LEVEL * gain, LEVEL * gain]

        name = "euclidelay_jalv"
        host = launch("jalv", ["jalv", "-i", "-p", "-n", name, "-c", "dry=0.5", "-c", "echoes=0", URI])
        wait_for(lambda: bool(probe.get_ports(f"{name}:")), "jalv did not load Euclidelay")
        time.sleep(0.5)  # JACK ports are registered before jalv activates its client.
        require(host.poll() is None, "jalv failed during activation")
        probe.connect(source, f"{name}:in")
        probe.connect(f"{name}:out_l", captures[0])
        probe.connect(f"{name}:out_r", captures[1])
        wait_for(hears(0.5), f"jalv: Dry 0.5 on both outputs, heard {observed}")
        require(host.poll() is None, "jalv exited")
        stop(host)
        print("PASS jalv: loads and runs, Dry on both outputs", flush=True)

        for threads in (1, 4):
            client = f"euclidelay_ingen_{threads}"
            path = Path(f"/tmp/{server_name}-{threads}.sock")
            engine = launch(f"ingen-{threads}", [args.ingen, "-e", "-f", "-s", server_name, "-n", client,
                                                 "-S", str(path), "-E", "0", "-p", str(threads)])
            wait_for(path.exists, "Ingen socket did not appear")
            time.sleep(1)  # The control socket opens before the JACK driver activates.
            require(engine.poll() is None, "Ingen failed during JACK activation")
            protocol = Protocol(path, logdir / f"protocol-{threads}.log")
            protocol.put("ingen:/main/in", "a lv2:InputPort, lv2:AudioPort")
            for side in ("l", "r"):
                protocol.put(f"ingen:/main/out_{side}", "a lv2:OutputPort, lv2:AudioPort")
            protocol.put("ingen:/main/euclidelay", f"a ingen:Block ; lv2:prototype <{URI}>")

            def wire(block):
                protocol.connect("ingen:/main/in", f"ingen:/main/{block}/in")
                for side in ("l", "r"):
                    protocol.connect(f"ingen:/main/{block}/out_{side}", f"ingen:/main/out_{side}")

            wire("euclidelay")
            wait_for(lambda: bool(probe.get_ports(f"{client}:out_l")), "Ingen JACK outputs")
            probe.connect(source, f"{client}:in")
            probe.connect(f"{client}:out_l", captures[0])
            probe.connect(f"{client}:out_r", captures[1])
            protocol.set("ingen:/main/euclidelay/echoes", 0)
            wait_for(hears(1), f"Ingen: default Dry, Echoes 0, passes the input to both outputs, heard {observed}")
            for symbol, value in SAVED.items():
                protocol.set(f"ingen:/main/euclidelay/{symbol}", value)
            wait_for(hears(SAVED["dry"]), f"Ingen: Dry {SAVED['dry']} on both outputs, heard {observed}")
            print(f"PASS Ingen: loads and runs, Dry on both outputs, {threads} threads", flush=True)

            save_path = logdir / f"{server_name}-{threads}.ingen"
            protocol.request("Copy", f"patch:subject <ingen:/main> ; patch:destination <{save_path.as_uri()}>")
            protocol.request("Delete", "patch:subject <ingen:/main/euclidelay>")
            protocol.request("Copy", f"patch:subject <{save_path.as_uri()}> ; patch:destination <ingen:/main/restored>")
            wire("restored")
            for symbol, value in SAVED.items():
                got = protocol.value(f"ingen:/main/restored/euclidelay/{symbol}")
                require(abs(got - value) < 1e-5, f"saved {symbol} {value} came back as {got}")
            require("state:state" not in (save_path / "main.ttl").read_text(), "no LV2 state is saved")
            wait_for(hears(SAVED["dry"]), f"Ingen: the reloaded block plays, heard {observed}")
            require(engine.poll() is None, "Ingen exited")
            protocol.close()
            protocol = None
            stop(engine)
            path.unlink(missing_ok=True)
            print(f"PASS Ingen: all {len(SAVED)} controls saved and restored, {threads} threads", flush=True)
    finally:
        if protocol:
            protocol.close()
        if probe:
            probe.close()
        for process in reversed(processes):
            if process.poll() is None:
                stop(process)
        for log in logs:
            log.close()
        shutil.rmtree(private_lv2, ignore_errors=True)


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        sys.exit(f"FAIL host_smoke: {error} (see build/host-smoke/*.log)")
