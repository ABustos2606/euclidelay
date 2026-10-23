# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 Alejandro Bustos
"""Check the parsed Turtle against src/ports.hpp. Test dependency: rdflib."""
import pathlib
import re
import sys

try:
    import rdflib
    from rdflib import Namespace, RDF, RDFS, URIRef
except ImportError:
    sys.exit("FAIL ttl_check: install python3-rdflib (or pip install rdflib)")

ROOT = pathlib.Path(__file__).resolve().parents[1]
LV2 = Namespace("http://lv2plug.in/ns/lv2core#")
DOAP = Namespace("http://usefulinc.com/ns/doap#")
UNITS = Namespace("http://lv2plug.in/ns/extensions/units#")
PLUGIN = URIRef("https://github.com/ABustos2606/euclidelay#euclidelay")
KINDS = {"audio_in": (LV2.AudioPort, LV2.InputPort), "audio_out": (LV2.AudioPort, LV2.OutputPort),
         "control_in": (LV2.ControlPort, LV2.InputPort)}
INTEGER = re.compile(r"(voice|steps|hits|shift)_[1-4]")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def number(text):
    return float(text.rstrip("f"))


def from_header():
    header = (ROOT / "src/ports.hpp").read_text(encoding="utf-8")
    require(f'kURI[] = "{PLUGIN}"' in header, "kURI")
    table = re.search(r"kPorts\[\] = \{(.*?)\n\};", header, re.S)
    require(table is not None, "missing kPorts")
    rows = re.findall(r'\{"(\w+)", Kind::(\w+), ([-\d.f]+), ([-\d.f]+), ([-\d.f]+)\}', table[1])
    require(len(rows) == len(table[1].strip().splitlines()), "every kPorts line parsed")
    count = int(re.search(r"port_count = (\d+)", header)[1])
    require(len(rows) == count == 37, f"37 ports, got {len(rows)}")
    steps = re.search(r"kStepNames\[\] = \{(.*?)\};", header)
    return [(s, k, number(lo), number(d), number(hi)) for s, k, lo, d, hi in rows], re.findall(r'"([^"]*)"', steps[1])


def check(bundle):
    ports, step_names = from_header()
    manifest = rdflib.Graph().parse(bundle / "manifest.ttl", format="turtle")
    require(set(manifest.subjects(RDF.type, LV2.Plugin)) == {PLUGIN}, "manifest plugins")
    require(manifest.value(PLUGIN, LV2.binary) == URIRef((bundle / "euclidelay.so").as_uri()), "binary")
    require(manifest.value(PLUGIN, RDFS.seeAlso) == URIRef((bundle / "euclidelay.ttl").as_uri()), "seeAlso")
    graph = rdflib.Graph().parse(bundle / "euclidelay.ttl", format="turtle")
    require((PLUGIN, RDF.type, LV2.Plugin) in graph and (PLUGIN, RDF.type, LV2.DelayPlugin) in graph, "class")
    require(graph.value(PLUGIN, DOAP.license) == URIRef("https://spdx.org/licenses/GPL-2.0-or-later"), "license")
    require(graph.value(PLUGIN, DOAP.maintainer) is not None, "maintainer")
    require(not set(graph.objects(PLUGIN, LV2.requiredFeature)), "no required features")
    require(LV2.hardRTCapable in set(graph.objects(PLUGIN, LV2.optionalFeature)), "hardRTCapable")
    require(str(graph.value(PLUGIN, DOAP.name)) == "Euclidelay", "name")
    require(str(graph.value(PLUGIN, RDFS.comment)).isascii(), "description must be ASCII")
    seen = {}
    for port in graph.objects(PLUGIN, LV2.port):
        index, symbol, name = int(graph.value(port, LV2["index"])), str(graph.value(port, LV2.symbol)), str(graph.value(port, LV2.name))
        require(index not in seen and 0 <= index < len(ports), f"index {index}")
        seen[index] = symbol
        want, kind, lo, default, hi = ports[index]
        require(symbol == want, f"port {index} is {want}, not {symbol}")
        require(name.isascii() and 0 < len(name) <= 16, f"{symbol}: name must be ASCII, at most 16 characters")
        types = set(graph.objects(port, RDF.type))
        require(types == set(KINDS[kind]), f"{symbol}: {kind}")
        props = set(graph.objects(port, LV2.portProperty))
        if kind != "control_in":
            continue
        got = tuple(float(graph.value(port, p)) for p in (LV2.minimum, LV2.default, LV2.maximum))
        require(got == (lo, default, hi), f"{symbol}: range and default {got}, ports.hpp says {(lo, default, hi)}")
        require(not INTEGER.fullmatch(symbol) or LV2.integer in props, f"{symbol}: integer")
        if symbol == "enabled":
            require(index == 3 and graph.value(port, LV2.designation) == LV2.enabled, "enabled designation on port 3")
        if symbol == "enabled" or symbol.startswith("voice_"):
            require(LV2.toggled in props, f"{symbol}: toggle")
        if symbol == "bpm":
            require(graph.value(port, UNITS.unit) == UNITS.bpm, "BPM unit")
        points = {int(graph.value(p, RDF.value)): str(graph.value(p, RDFS.label)) for p in graph.objects(port, LV2.scalePoint)}
        for label in points.values():
            require(label.isascii() and 0 < len(label) <= 16, f"{symbol}: value names must be ASCII, at most 16 characters")
        if symbol == "step":
            require({LV2.integer, LV2.enumeration} <= props, "Step is an enumeration")
            require(points == dict(enumerate(step_names)) and hi == len(step_names) - 1, f"Step values {points}")
        else:
            require(LV2.enumeration not in props and not points, f"{symbol}: not an enumeration")
    require(len(seen) == len(ports), f"{len(ports) - len(seen)} ports missing from the Turtle")
    print(f"PASS ttl_check: {len(ports)} ports match ports.hpp: symbols, kinds, ranges, defaults, Step values, names")


if __name__ == "__main__":
    try:
        check(pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / "euclidelay.lv2").resolve())
    except Exception as error:
        sys.exit(f"FAIL ttl_check: {error}")
