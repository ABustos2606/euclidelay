// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
// The patterns: every (Steps, Hits, Shift) against Peepo's Euclidean module
// (tests/patterns_peepo.inc, written by tools/gen_patterns.sh), the spec's
// examples, out-of-range values, and the taps a pattern gives.
#include "../src/pattern.hpp"
#include <cstdio>
#include <stdexcept>
#include <string>

using namespace euclidelay;
namespace {
struct Entry { int steps, hits; unsigned mask[16]; };
const Entry kPeepo[] = {
#include "patterns_peepo.inc"
};

void require(bool ok, const std::string& what) { if (!ok) throw std::runtime_error(what); }

// "xx.x.x." for a mask of `steps` steps.
std::string draw(uint32_t mask, int steps) {
    std::string s;
    for (int j = 0; j < steps; ++j) s += mask & (1u << j) ? 'x' : '.';
    return s;
}

void example(int steps, int hits, int shift, const char* expected) {
    const std::string got = draw(pattern(steps, hits, shift), steps);
    require(got == expected, std::to_string(steps) + ", " + std::to_string(hits) + ", " + std::to_string(shift) +
                                 ": expected " + expected + ", got " + got);
}
}

int main() {
    try {
        int checked = 0;
        require(sizeof kPeepo / sizeof kPeepo[0] == 16 * 17, "the table has every Steps and Hits");
        for (const Entry& e : kPeepo)
            for (int shift = 0; shift < 16; ++shift, ++checked) {
                const uint32_t got = pattern(e.steps, e.hits, shift);
                require(got == e.mask[shift], "Steps " + std::to_string(e.steps) + ", Hits " + std::to_string(e.hits) +
                                                  ", Shift " + std::to_string(shift) + ": Peepo draws " +
                                                  draw(e.mask[shift], e.steps) + ", Euclidelay " + draw(got, e.steps));
            }

        example(7, 4, 0, "xx.x.x.");
        example(4, 3, 0, "xxx.");
        example(8, 3, 0, "x.x..x..");
        example(5, 2, 0, "x.x..");
        example(5, 2, 1, ".x.x.");
        example(16, 5, 0, "x..x..x..x..x...");
        example(1, 1, 0, "x");
        for (int steps = 1; steps <= 16; ++steps) {
            require(pattern(steps, 0, 0) == 0, "Hits 0 is silent");
            for (int hits = 1; hits <= steps; ++hits)
                require(pattern(steps, hits, 0) & 1u, "Shift 0 with a hit has step 0");
            require(pattern(steps, steps + 3, 2) == pattern(steps, steps, 2), "Hits above Steps count as Steps");
            for (int hits = 0; hits <= steps; ++hits)
                require(pattern(steps, hits, 15 + steps) == pattern(steps, hits, (15 + steps) % steps),
                        "Shift is taken modulo Steps");
        }
        require(pattern(0, 1, 0) == pattern(1, 1, 0) && pattern(40, 3, 0) == pattern(16, 3, 0), "Steps clamped to 1..16");

        // Taps: 7, 4 at 1000.4 samples a step. Each time is rounded on its own.
        const Taps t = make_taps(pattern(7, 4, 0), 7, 1000.4);
        require(t.zero && t.count == 3 && t.loop == 7003, "7, 4: step 0 and three more hits, a loop of 7003");
        require(t.delay[0] == 1000 && t.delay[1] == 3001 && t.delay[2] == 5002, "7, 4: hits at 1000, 3001, 5002");
        const Taps shifted = make_taps(pattern(5, 2, 1), 5, 100);
        require(!shifted.zero && shifted.count == 2 && shifted.delay[0] == 100 && shifted.delay[1] == 300,
                "5, 2, 1: no step 0, hits at 1 and 3 steps");
        const Taps none = make_taps(pattern(4, 0, 0), 4, 100);
        require(!none.zero && none.count == 0 && none.loop == 400, "Hits 0: no taps");
        require(step_samples(0.25, 120, 48000) == 6000, "1/16 at 120 BPM is 6000 samples at 48 kHz");
        require(step_samples(1, 30, 48000) * 16 == 32 * 48000.0, "the longest loop is 32 s");

        std::printf("PASS test_pattern: %d patterns match Peepo's Euclidean module; the spec's examples; taps\n", checked);
    } catch (const std::exception& e) { std::printf("FAIL test_pattern: %s\n", e.what()); return 1; }
}
